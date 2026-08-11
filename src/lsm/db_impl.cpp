// Day-2 DB: memtable + WAL + L0 SSTables.
//
// THE READ PATH now has to consult several places, in a strict order:
//
//   memtable ->  immutable memtable ->  L0 tables, NEWEST FIRST
//
// Order is not an optimisation, it is correctness. The same user key may exist
// in all of them at different sequence numbers, and the first hit wins. Search
// an older table first and a stale value shadows a fresh one - or worse, a
// deleted key comes back to life because you found its old value before its
// tombstone.
//
// This is also why MemTable::Get returns true for a tombstone. "Found a
// deletion" must stop the search; treating it as "not here, keep looking" would
// resurrect the value from the level below.
//
// CRASH SAFETY OF A FLUSH. The steps are ordered so that a crash at any point
// leaves a consistent database:
//
//   1. freeze the memtable, open a NEW log            (old log still on disk)
//   2. write the SSTable and fsync it                 (not yet referenced)
//   3. write DESCRIPTOR atomically  <-- COMMIT POINT
//   4. delete the old log
//
//   crash between 1 and 3: the sstable is an orphan, the data is still in the
//     old log, and recovery replays it. Correct.
//   crash between 3 and 4: the descriptor already names the new log, so the
//     old one is ignored. Correct.
//
// Everything here is synchronous, including the flush, which blocks the writer.
// Day 4 moves it to a background thread; the ordering above does not change.

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "crc32c.h"
#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "memtable.h"
#include "sextant/lsm/db.h"
#include "table.h"
#include "table_builder.h"
#include "wal.h"

namespace sextant::lsm {
namespace {

class SnapshotImpl : public Snapshot {
 public:
  explicit SnapshotImpl(SequenceNumber seq) : sequence_(seq) {}
  SequenceNumber sequence() const { return sequence_; }

 private:
  SequenceNumber sequence_;
};

class LogReporter : public wal::Reader::Reporter {
 public:
  Status* status = nullptr;
  bool paranoid = false;
  size_t dropped = 0;

  void Corruption(size_t bytes, const Status& s) override {
    dropped += bytes;
    if (paranoid && status != nullptr && status->ok()) *status = s;
  }
};

// One L0 table: its file number and the open reader.
struct FileEntry {
  uint64_t number = 0;
  std::unique_ptr<Table> table;
};

// --- point-lookup callback -------------------------------------------------

enum class SaverState { kNotFound, kFound, kDeleted, kCorrupt };

struct Saver {
  SaverState state = SaverState::kNotFound;
  const Slice* user_key = nullptr;
  std::string* value = nullptr;
};

// Table::InternalGet seeks to the first entry >= our lookup key and hands it
// here. Two things still have to be checked: that the USER key actually
// matches (the seek may have landed on the next key entirely), and whether what
// we found is a value or a tombstone.
void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  auto* saver = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed;
  if (!ParseInternalKey(ikey, &parsed)) {
    saver->state = SaverState::kCorrupt;
    return;
  }
  if (parsed.user_key.compare(*saver->user_key) != 0) {
    return;  // different key; this table does not have ours
  }
  if (parsed.type == kTypeValue) {
    saver->state = SaverState::kFound;
    saver->value->assign(v.data(), v.size());
  } else {
    saver->state = SaverState::kDeleted;
  }
}

// --- flush -----------------------------------------------------------------

// Write a memtable out as a sorted table. The memtable iterator is already in
// internal-key order, so this is a single forward pass with no sorting and no
// seeking.
Status BuildTable(const std::string& dbname, const Options& options, MemTable* mem,
                  uint64_t number, uint64_t* file_size, uint64_t* num_entries) {
  const std::string fname = TableFileName(dbname, number);

  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(fname, /*append=*/false, &file);
  if (!s.ok()) return s;

  TableBuilder builder(options, file.get());
  uint64_t count = 0;

  MemTable::Iterator it(mem);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    builder.Add(it.key(), it.value());
    ++count;
  }

  if (count == 0) {
    builder.Abandon();
    file->Close();
    RemoveFile(fname);
    *file_size = 0;
    *num_entries = 0;
    return Status::OK();
  }

  s = builder.Finish();
  if (s.ok()) *file_size = builder.FileSize();
  *num_entries = count;

  // fsync before the descriptor references this file. Without it, a power cut
  // could leave DESCRIPTOR pointing at a table that is not fully on disk.
  if (s.ok()) s = file->Sync();
  Status close_status = file->Close();
  if (s.ok()) s = close_status;

  if (!s.ok()) RemoveFile(fname);
  return s;
}

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname)
      : options_(options), dbname_(std::move(dbname)), internal_comparator_{} {
    mem_ = std::make_unique<MemTable>(internal_comparator_);
  }

  ~DBImpl() override {
    if (log_writer_) log_writer_->Sync();
    if (logfile_) logfile_->Close();
  }

  Status Recover();

  Status Put(const WriteOptions& opts, const Slice& key, const Slice& value) override {
    WriteBatch batch;
    batch.Put(key, value);
    return Write(opts, &batch);
  }

  Status Delete(const WriteOptions& opts, const Slice& key) override {
    WriteBatch batch;
    batch.Delete(key);
    return Write(opts, &batch);
  }

  Status Write(const WriteOptions& opts, WriteBatch* updates) override {
    if (updates == nullptr || updates->Count() == 0) return Status::OK();

    std::lock_guard<std::mutex> lock(mutex_);

    // Make room before inserting, so the memtable never overshoots the budget
    // by more than one batch.
    if (mem_->ApproximateMemoryUsage() >= options_.write_buffer_size) {
      Status s = FlushMemTableLocked();
      if (!s.ok()) return s;
    }

    const SequenceNumber first = last_sequence_ + 1;
    updates->SetSequence(first);
    last_sequence_ += static_cast<SequenceNumber>(updates->Count());

    // WAL BEFORE MEMTABLE - a record in memory but not in the log would vanish
    // on a crash after the caller was told the write succeeded.
    Status s = log_writer_->AddRecord(Slice(updates->Contents()));
    if (!s.ok()) return s;

    if (opts.sync) {
      s = log_writer_->Sync();
      if (!s.ok()) return s;
    }

    s = updates->InsertInto(mem_.get());
    if (!s.ok()) return s;

    stats_.writes += static_cast<uint64_t>(updates->Count());
    stats_.bytes_written += updates->ApproximateSize();
    return Status::OK();
  }

  Status Get(const ReadOptions& opts, const Slice& key, std::string* value) override {
    SequenceNumber snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot = (opts.snapshot != nullptr)
                     ? static_cast<const SnapshotImpl*>(opts.snapshot)->sequence()
                     : last_sequence_;
      ++stats_.reads;
    }

    const LookupKey lkey(key, snapshot);

    std::lock_guard<std::mutex> lock(mutex_);

    // 1. active memtable
    Status s;
    if (mem_->Get(lkey, value, &s)) {
      ++stats_.memtable_hits;
      return s;
    }

    // 2. memtable currently being flushed
    if (imm_ && imm_->Get(lkey, value, &s)) {
      ++stats_.memtable_hits;
      return s;
    }

    // 3. L0 tables, newest first. They overlap in key range, so order matters.
    //
    // Every table is probed today. Day 3 adds bloom filters and per-file key
    // ranges, which is what turns this from O(files) into ~O(1) disk reads.
    ReadOptions table_opts;
    table_opts.verify_checksums = options_.paranoid_checks;

    // LookupKey::user_key() returns by value, so it must be bound to a named
    // local before its address is taken - otherwise Saver would hold a pointer
    // to a destroyed temporary.
    const Slice user_key = lkey.user_key();

    for (const auto& f : files_) {
      Saver saver;
      saver.state = SaverState::kNotFound;
      saver.user_key = &user_key;
      saver.value = value;

      ++stats_.sstables_probed;
      Status ts = f.table->InternalGet(table_opts, lkey.internal_key(), &saver,
                                       &SaveValue);
      if (!ts.ok()) return ts;

      switch (saver.state) {
        case SaverState::kFound:
          ++stats_.sstable_hits;
          return Status::OK();
        case SaverState::kDeleted:
          // A tombstone. Stop here: searching older tables would resurrect the
          // value this delete was meant to remove.
          return Status::NotFound(Slice());
        case SaverState::kCorrupt:
          return Status::Corruption("corrupt entry in sstable");
        case SaverState::kNotFound:
          break;  // try the next-oldest table
      }
    }

    return Status::NotFound(Slice());
  }

  const Snapshot* GetSnapshot() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return new SnapshotImpl(last_sequence_);
  }

  void ReleaseSnapshot(const Snapshot* snapshot) override {
    delete static_cast<const SnapshotImpl*>(snapshot);
  }

  Status SyncWAL() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_writer_->Sync();
  }

  Stats GetStats() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.memtable_bytes = mem_->ApproximateMemoryUsage();
    s.sequence = last_sequence_;
    s.num_sstables = files_.size();
    return s;
  }

 private:
  Status FlushMemTableLocked();
  Status WriteDescriptorLocked();
  Status ReadDescriptor(bool* found);
  Status OpenTable(uint64_t number, std::unique_ptr<Table>* table);
  Status NewLogFileLocked(uint64_t number);

  const Options options_;
  const std::string dbname_;
  const InternalKeyComparator internal_comparator_;

  mutable std::mutex mutex_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<MemTable> imm_;

  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<wal::Writer> log_writer_;

  // Newest first. This ordering IS the correctness of the L0 read path.
  std::vector<FileEntry> files_;

  SequenceNumber last_sequence_ = 0;
  uint64_t next_file_number_ = 1;
  uint64_t log_number_ = 0;
  Stats stats_;
};

Status DBImpl::NewLogFileLocked(uint64_t number) {
  if (log_writer_) log_writer_->Flush();
  if (logfile_) logfile_->Close();

  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(LogFileName(dbname_, number), /*append=*/true, &file);
  if (!s.ok()) return s;

  uint64_t existing = 0;
  GetFileSize(LogFileName(dbname_, number), &existing);

  logfile_ = std::move(file);
  log_writer_ = std::make_unique<wal::Writer>(logfile_.get(), existing);
  log_number_ = number;
  return Status::OK();
}

Status DBImpl::FlushMemTableLocked() {
  if (mem_->NumEntries() == 0) return Status::OK();

  // 1. Freeze the memtable and start a fresh log. Writes that arrive after
  //    this point go to the new memtable and the new log.
  imm_ = std::move(mem_);
  mem_ = std::make_unique<MemTable>(internal_comparator_);

  const uint64_t old_log_number = log_number_;
  const uint64_t new_log_number = next_file_number_++;
  Status s = NewLogFileLocked(new_log_number);
  if (!s.ok()) return s;

  // 2. Write the table and fsync it. Nothing references it yet.
  const uint64_t table_number = next_file_number_++;
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  s = BuildTable(dbname_, options_, imm_.get(), table_number, &file_size, &num_entries);
  if (!s.ok()) return s;

  if (num_entries > 0) {
    std::unique_ptr<Table> table;
    s = OpenTable(table_number, &table);
    if (!s.ok()) return s;

    FileEntry entry;
    entry.number = table_number;
    entry.table = std::move(table);
    files_.insert(files_.begin(), std::move(entry));  // newest first
  }

  // 3. COMMIT POINT. Until the descriptor lands, the old log is still the
  //    source of truth.
  s = WriteDescriptorLocked();
  if (!s.ok()) return s;

  // 4. Now, and only now, the old log is redundant.
  if (old_log_number != 0) {
    RemoveFile(LogFileName(dbname_, old_log_number));
  }

  imm_.reset();
  ++stats_.flushes;
  stats_.bytes_flushed += file_size;
  return Status::OK();
}

// A minimal stand-in for a real MANIFEST. Day 4 replaces it with an append-only
// edit log (VersionEdit) so that adding one file does not rewrite the whole
// state, and so that concurrent readers can pin a version.
Status DBImpl::WriteDescriptorLocked() {
  std::string body;
  PutFixed64BE(&body, last_sequence_);
  PutFixed64BE(&body, log_number_);
  PutFixed64BE(&body, next_file_number_);
  PutFixed32BE(&body, static_cast<uint32_t>(files_.size()));
  for (const auto& f : files_) PutFixed64BE(&body, f.number);
  PutFixed32BE(&body, crc32c::Mask(crc32c::Value(body.data(), body.size())));

  // Write to a temp file, fsync, then rename. Rename is atomic on both POSIX
  // and Win32, so a reader sees either the whole old descriptor or the whole
  // new one - never a half-written mixture.
  const std::string tmp = TempDescriptorFileName(dbname_);
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(tmp, /*append=*/false, &file);
  if (!s.ok()) return s;

  s = file->Append(Slice(body));
  if (s.ok()) s = file->Sync();
  Status close_status = file->Close();
  if (s.ok()) s = close_status;
  if (!s.ok()) {
    RemoveFile(tmp);
    return s;
  }

  return RenameFile(tmp, DescriptorFileName(dbname_));
}

Status DBImpl::ReadDescriptor(bool* found) {
  *found = false;
  const std::string path = DescriptorFileName(dbname_);
  if (!FileExists(path)) return Status::OK();

  uint64_t size = 0;
  Status s = GetFileSize(path, &size);
  if (!s.ok()) return s;
  if (size < 28) return Status::Corruption("descriptor too short");

  std::unique_ptr<SequentialFile> file;
  s = SequentialFile::Open(path, &file);
  if (!s.ok()) return s;

  std::string buf(static_cast<size_t>(size), '\0');
  Slice contents;
  s = file->Read(static_cast<size_t>(size), &contents, buf.data());
  if (!s.ok()) return s;
  if (contents.size() != size) return Status::Corruption("short descriptor read");

  const size_t body_len = contents.size() - 4;
  const uint32_t expected = crc32c::Unmask(DecodeFixed32BE(contents.data() + body_len));
  if (crc32c::Value(contents.data(), body_len) != expected) {
    return Status::Corruption("descriptor checksum mismatch");
  }

  const char* p = contents.data();
  last_sequence_ = DecodeFixed64BE(p);
  log_number_ = DecodeFixed64BE(p + 8);
  next_file_number_ = DecodeFixed64BE(p + 16);
  const uint32_t num_files = DecodeFixed32BE(p + 24);

  if (28 + static_cast<size_t>(num_files) * 8 != body_len) {
    return Status::Corruption("descriptor file count mismatch");
  }

  for (uint32_t i = 0; i < num_files; ++i) {
    const uint64_t number = DecodeFixed64BE(p + 28 + i * 8);
    std::unique_ptr<Table> table;
    s = OpenTable(number, &table);
    if (!s.ok()) return s;
    FileEntry entry;
    entry.number = number;
    entry.table = std::move(table);
    files_.push_back(std::move(entry));  // already stored newest first
  }

  *found = true;
  return Status::OK();
}

Status DBImpl::OpenTable(uint64_t number, std::unique_ptr<Table>* table) {
  const std::string fname = TableFileName(dbname_, number);

  uint64_t size = 0;
  Status s = GetFileSize(fname, &size);
  if (!s.ok()) return s;

  std::unique_ptr<RandomAccessFile> file;
  s = RandomAccessFile::Open(fname, &file);
  if (!s.ok()) return s;

  return Table::Open(options_, std::move(file), size, table);
}

Status DBImpl::Recover() {
  Status s = CreateDir(dbname_);
  if (!s.ok()) return s;

  bool have_descriptor = false;
  s = ReadDescriptor(&have_descriptor);
  if (!s.ok()) return s;

  if (!have_descriptor) {
    // No descriptor. Either a brand-new database, or one created before any
    // flush happened, in which case log 1 holds everything.
    if (options_.error_if_exists && FileExists(LogFileName(dbname_, 1))) {
      return Status::InvalidArgument(dbname_, "database already exists");
    }
    log_number_ = 1;
    next_file_number_ = 2;
  }

  // Replay the current log. Anything older was already folded into an SSTable
  // before the descriptor that named it was committed.
  const std::string wal_path = LogFileName(dbname_, log_number_);
  if (FileExists(wal_path)) {
    std::unique_ptr<SequentialFile> file;
    s = SequentialFile::Open(wal_path, &file);
    if (!s.ok()) return s;

    LogReporter reporter;
    Status replay_status;
    reporter.status = &replay_status;
    reporter.paranoid = options_.paranoid_checks;

    wal::Reader reader(file.get(), &reporter, /*checksum=*/true);
    std::string scratch;
    Slice record;
    WriteBatch batch;

    while (reader.ReadRecord(&record, &scratch)) {
      if (record.size() < WriteBatch::kHeaderSize) {
        reporter.Corruption(record.size(), Status::Corruption("log record too small"));
        continue;
      }
      batch.SetContents(record);
      s = batch.InsertInto(mem_.get());
      if (!s.ok()) return s;

      const SequenceNumber last_in_batch =
          batch.Sequence() + static_cast<SequenceNumber>(batch.Count()) - 1;
      if (last_in_batch > last_sequence_) last_sequence_ = last_in_batch;

      ++stats_.wal_records_replayed;
    }

    // A dropped tail is what a crash looks like and is not an error. Only
    // mid-file corruption is escalated when paranoid_checks is on.
    if (options_.paranoid_checks && !replay_status.ok()) return replay_status;
  } else if (!options_.create_if_missing && !have_descriptor) {
    return Status::InvalidArgument(dbname_,
                                   "does not exist (create_if_missing is false)");
  }

  return NewLogFileLocked(log_number_);
}

}  // namespace

Status DB::Open(const Options& options, const std::string& name,
                std::unique_ptr<DB>* dbptr) {
  dbptr->reset();
  auto impl = std::make_unique<DBImpl>(options, name);
  Status s = impl->Recover();
  if (!s.ok()) return s;
  *dbptr = std::move(impl);
  return Status::OK();
}

}  // namespace sextant::lsm
