// Day-1 DB: memtable + WAL + recovery on open.
//
// No SSTables yet, so the whole dataset must fit in memory — that is the
// day-2 milestone. Everything here is written so that adding the flush path
// is additive rather than a rewrite: the write path already assigns sequence
// numbers, frames batches, and checks the memtable size against
// write_buffer_size.
//
// RECOVERY, which is the part worth understanding:
//   Open() replays every record in the WAL back into a fresh memtable, in
//   order. Because each WAL record is a serialised WriteBatch carrying its own
//   starting sequence number, replay reconstructs the exact sequence ordering
//   of the original run. A torn final record fails its CRC, the reader stops
//   there, and every acknowledged write is present. That is the invariant the
//   crash test asserts.

#include <atomic>
#include <mutex>

#include "env.h"
#include "internal_key.h"
#include "memtable.h"
#include "sextant/lsm/db.h"
#include "wal.h"

namespace sextant::lsm {
namespace {

std::string WALFileName(const std::string& dbname) { return dbname + "/000001.log"; }

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

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname)
      : options_(options),
        dbname_(std::move(dbname)),
        internal_comparator_{},
        mem_(new MemTable(internal_comparator_)) {}

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

    // Reserve a contiguous sequence range for the batch. One sequence per
    // record, so two writes to the same key inside one batch stay ordered.
    const SequenceNumber first = last_sequence_ + 1;
    updates->SetSequence(first);
    last_sequence_ += static_cast<SequenceNumber>(updates->Count());

    // WAL BEFORE MEMTABLE. This ordering is the entire durability guarantee:
    // a record in the memtable but not the log would vanish on crash, and the
    // caller would already have been told the write succeeded.
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

    Status s;
    if (mem_->Get(lkey, value, &s)) {
      // True even for a tombstone — see MemTable::Get. Day 2 adds "else fall
      // through to L0, then L1..Ln"; the tombstone short-circuit here is what
      // will stop a deleted key resurrecting from a lower level.
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.memtable_hits;
      return s;
    }
    return Status::NotFound(Slice());
  }

  const Snapshot* GetSnapshot() override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* snap = new SnapshotImpl(last_sequence_);
    return snap;
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
    return s;
  }

 private:
  const Options options_;
  const std::string dbname_;
  const InternalKeyComparator internal_comparator_;

  mutable std::mutex mutex_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<wal::Writer> log_writer_;
  SequenceNumber last_sequence_ = 0;
  Stats stats_;
};

Status DBImpl::Recover() {
  Status s = CreateDir(dbname_);
  if (!s.ok()) return s;

  const std::string wal_path = WALFileName(dbname_);

  if (FileExists(wal_path)) {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(dbname_, "database already exists");
    }

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

    // A dropped tail is expected after a crash and is not an error. Only
    // mid-file corruption is reported when paranoid_checks is on.
    if (options_.paranoid_checks && !replay_status.ok()) return replay_status;
  } else if (!options_.create_if_missing) {
    return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
  }

  // Reopen the log for append. Day 4 replaces this with a fresh numbered log
  // plus a MANIFEST edit.
  s = WritableFile::Open(wal_path, /*append=*/true, &logfile_);
  if (!s.ok()) return s;

  uint64_t existing = 0;
  GetFileSize(wal_path, &existing);
  log_writer_ = std::make_unique<wal::Writer>(logfile_.get(), existing);
  return Status::OK();
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
