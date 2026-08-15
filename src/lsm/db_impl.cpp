// Day-4 DB: versioned file sets, MANIFEST, leveled compaction, background work.
//
// WHAT CHANGED FROM DAY 3, and why each thing matters.
//
// 1. VERSIONS. The set of live files is now an immutable, refcounted Version.
//    An iterator pins one for its lifetime, so compaction can delete files
//    concurrently without pulling the ground out from under an open scan. This
//    is the fix for the limitation documented on day 3.
//
// 2. LEVELS. L0 still holds overlapping flushed memtables, but compaction now
//    merges them down into L1..L6 where files are non-overlapping and sorted.
//    A lookup checks every candidate L0 file, then AT MOST ONE file per level
//    below. Read amplification stops growing with the number of flushes.
//
// 3. BACKGROUND WORK. Flush and compaction run on their own thread. Writers no
//    longer stop dead for the duration of a flush.
//
// 4. WRITE STALLS, which are the honest consequence of (3). If ingest outruns
//    compaction, L0 grows without bound and reads degrade toward day-2
//    behaviour. So the engine deliberately slows writers at 8 L0 files and
//    stops them at 12. Backpressure is a feature: the alternative is unbounded
//    read amplification and eventually running out of disk.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bloom.h"
#include "cache.h"
#include "db_iter.h"
#include "env.h"
#include "filename.h"
#include "internal_key.h"
#include "memtable.h"
#include "merger.h"
#include "sextant/lsm/db.h"
#include "table_builder.h"
#include "table_cache.h"
#include "version_set.h"
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

  void Corruption(size_t, const Status& s) override {
    if (paranoid && status != nullptr && status->ok()) *status = s;
  }
};

// Work in progress for one compaction. Outputs are accumulated here and only
// become visible when the whole compaction commits.
struct CompactionState {
  struct Output {
    uint64_t number = 0;
    uint64_t file_size = 0;
    std::string smallest;
    std::string largest;
  };

  explicit CompactionState(Compaction* c) : compaction(c) {}

  Output* current_output() { return &outputs.back(); }

  Compaction* const compaction;
  SequenceNumber smallest_snapshot = 0;
  std::vector<Output> outputs;
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t total_bytes = 0;
};

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status Recover();
  void StartBackgroundThread();

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

  Status Write(const WriteOptions& opts, WriteBatch* updates) override;
  Status Get(const ReadOptions& opts, const Slice& key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& opts) override;

  const Snapshot* GetSnapshot() override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* s = new SnapshotImpl(versions_->LastSequence());
    snapshots_.insert(s->sequence());
    return s;
  }

  void ReleaseSnapshot(const Snapshot* snapshot) override {
    const auto* s = static_cast<const SnapshotImpl*>(snapshot);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = snapshots_.find(s->sequence());
      if (it != snapshots_.end()) snapshots_.erase(it);
    }
    delete s;
  }

  Status SyncWAL() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_writer_->Sync();
  }

  Stats GetStats() const override;

  void WaitForBackgroundWork() override;

 private:
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
  Status NewLogFileLocked(uint64_t number);

  void MaybeScheduleCompaction();
  void BackgroundLoop();
  void BackgroundCompaction(std::unique_lock<std::mutex>& lock);
  Status CompactMemTable(std::unique_lock<std::mutex>& lock);
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base,
                          std::unique_lock<std::mutex>& lock);
  Status DoCompactionWork(CompactionState* compact, std::unique_lock<std::mutex>& lock);
  Status OpenCompactionOutputFile(CompactionState* compact);
  Status FinishCompactionOutputFile(CompactionState* compact, Iterator* input);
  Status InstallCompactionResults(CompactionState* compact);
  void DeleteObsoleteFiles();

  bool HasBackgroundWork() const {
    return imm_ != nullptr || versions_->NeedsCompaction();
  }

  Options options_;
  const std::string dbname_;
  const InternalKeyComparator internal_comparator_;

  std::unique_ptr<BloomFilterPolicy> filter_policy_;
  std::unique_ptr<Cache> block_cache_;
  std::unique_ptr<TableCache> table_cache_;

  mutable std::mutex mutex_;
  std::condition_variable bg_work_cv_;   // signals the worker there is work
  std::condition_variable bg_done_cv_;   // signals writers that work finished

  std::thread bg_thread_;
  bool bg_running_ = false;
  bool bg_busy_ = false;
  // Atomic because the compaction loop polls it with the mutex released.
  std::atomic<bool> shutting_down_{false};
  Status bg_error_;

  // Refcounted, not unique_ptr: an open iterator may outlive the DB's own
  // reference to a memtable. See MemTable::Ref.
  MemTable* mem_ = nullptr;
  MemTable* imm_ = nullptr;

  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<wal::Writer> log_writer_;
  uint64_t logfile_number_ = 0;

  std::unique_ptr<VersionSet> versions_;
  std::multiset<SequenceNumber> snapshots_;

  // Files being written by an in-flight compaction. They are not in any
  // Version yet, so DeleteObsoleteFiles would otherwise consider them garbage
  // and unlink them mid-write.
  std::set<uint64_t> pending_outputs_;

  Stats stats_;
};

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options), dbname_(std::move(dbname)), internal_comparator_{} {
  if (options_.bloom_bits_per_key > 0) {
    filter_policy_ = std::make_unique<BloomFilterPolicy>(options_.bloom_bits_per_key);
    options_.filter_policy = filter_policy_.get();
  }
  if (options_.block_cache_size > 0) {
    block_cache_ = Cache::NewLRUCache(options_.block_cache_size);
    options_.block_cache = block_cache_.get();
  }
  table_cache_ = std::make_unique<TableCache>(dbname_, options_, 500);
  versions_ = std::make_unique<VersionSet>(dbname_, options_, table_cache_.get());
  mem_ = new MemTable(internal_comparator_);
  mem_->Ref();
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_.store(true, std::memory_order_relaxed);
  }
  bg_work_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();

  if (log_writer_) log_writer_->Sync();
  if (logfile_) logfile_->Close();

  // Order matters on teardown: versions reference table-cache entries, which
  // reference block-cache entries.
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();

  versions_.reset();
  table_cache_.reset();
}

void DBImpl::StartBackgroundThread() {
  std::lock_guard<std::mutex> lock(mutex_);
  bg_running_ = true;
  bg_thread_ = std::thread([this] { BackgroundLoop(); });
}

void DBImpl::WaitForBackgroundWork() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (bg_busy_ || HasBackgroundWork()) {
    bg_work_cv_.notify_all();
    bg_done_cv_.wait(lock);
  }
}

// --- write path ------------------------------------------------------------

Status DBImpl::Write(const WriteOptions& opts, WriteBatch* updates) {
  if (updates == nullptr || updates->Count() == 0) return Status::OK();

  std::unique_lock<std::mutex> lock(mutex_);

  Status s = MakeRoomForWrite(lock, /*force=*/false);
  if (!s.ok()) return s;

  const SequenceNumber first = versions_->LastSequence() + 1;
  updates->SetSequence(first);
  versions_->SetLastSequence(first + static_cast<SequenceNumber>(updates->Count()) - 1);

  // WAL BEFORE MEMTABLE - a record in memory but not in the log would vanish
  // on a crash after the caller was told the write succeeded.
  s = log_writer_->AddRecord(Slice(updates->Contents()));
  if (!s.ok()) return s;

  if (opts.sync) {
    s = log_writer_->Sync();
    if (!s.ok()) return s;
  }

  s = updates->InsertInto(mem_);
  if (!s.ok()) return s;

  stats_.writes += static_cast<uint64_t>(updates->Count());
  stats_.bytes_written += updates->ApproximateSize();
  return Status::OK();
}

// Ensure there is room in the memtable, applying backpressure if compaction is
// falling behind. This is where an LSM admits that ingest cannot outrun
// compaction forever.
Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  bool allow_delay = !force;

  while (true) {
    if (!bg_error_.ok()) {
      return bg_error_;
    }

    if (allow_delay && versions_->NumLevelFiles(0) >= kL0SlowdownWritesTrigger) {
      // SOFT STALL. One millisecond per write, applied before the situation
      // becomes critical. Spreading the pain out like this is far better than
      // letting writes run at full speed and then blocking for seconds when L0
      // hits the hard limit.
      ++stats_.write_stalls;
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lock.lock();
      allow_delay = false;  // only delay once per write
      continue;
    }

    if (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
      return Status::OK();  // room available
    }

    if (imm_ != nullptr) {
      // A flush is already in flight; wait for it rather than queueing another.
      ++stats_.write_stalls;
      bg_work_cv_.notify_all();
      bg_done_cv_.wait(lock);
      continue;
    }

    if (versions_->NumLevelFiles(0) >= kL0StopWritesTrigger) {
      // HARD STOP. Compaction is losing. Blocking here is the only thing that
      // prevents read amplification growing without bound.
      ++stats_.write_stalls;
      bg_work_cv_.notify_all();
      bg_done_cv_.wait(lock);
      continue;
    }

    // Freeze the memtable and start a fresh log.
    const uint64_t new_log_number = versions_->NewFileNumber();
    Status s = NewLogFileLocked(new_log_number);
    if (!s.ok()) return s;

    imm_ = mem_;
    mem_ = new MemTable(internal_comparator_);
    mem_->Ref();
    force = false;

    MaybeScheduleCompaction();
  }
}

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
  logfile_number_ = number;
  return Status::OK();
}

// --- read path -------------------------------------------------------------

Status DBImpl::Get(const ReadOptions& opts, const Slice& key, std::string* value) {
  std::unique_lock<std::mutex> lock(mutex_);

  const SequenceNumber snapshot =
      (opts.snapshot != nullptr)
          ? static_cast<const SnapshotImpl*>(opts.snapshot)->sequence()
          : versions_->LastSequence();
  ++stats_.reads;

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  Version* current = versions_->current();
  current->Ref();  // pin the file set for the duration of this lookup

  const LookupKey lkey(key, snapshot);

  Status s;
  bool found = false;

  if (mem->Get(lkey, value, &s)) {
    ++stats_.memtable_hits;
    if (opts.stats != nullptr) ++opts.stats->memtable_hits;
    found = true;
  } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
    ++stats_.memtable_hits;
    if (opts.stats != nullptr) ++opts.stats->memtable_hits;
    found = true;
  }

  if (!found) {
    ReadOptions table_opts;
    table_opts.verify_checksums = options_.paranoid_checks;
    table_opts.fill_cache = opts.fill_cache;
    // Carry the caller's sink down into the table layer, which is where the
    // block reads and filter rejections that make up most of a read's cost
    // actually happen.
    table_opts.stats = opts.stats;

    // Release the lock for the disk read. The Version is pinned, so compaction
    // may run concurrently without invalidating anything we are reading.
    Version::GetStats get_stats;
    lock.unlock();
    s = current->Get(table_opts, lkey, value, &get_stats);
    lock.lock();

    stats_.range_rejections += get_stats.range_rejections;
    stats_.sstables_probed += get_stats.files_probed;
    if (opts.stats != nullptr) {
      opts.stats->range_rejections += get_stats.range_rejections;
      opts.stats->sstables_probed += get_stats.files_probed;
    }
    if (s.ok()) ++stats_.sstable_hits;
  }

  current->Unref();
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  return s;
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& opts) {
  std::lock_guard<std::mutex> lock(mutex_);

  const SequenceNumber snapshot =
      (opts.snapshot != nullptr)
          ? static_cast<const SnapshotImpl*>(opts.snapshot)->sequence()
          : versions_->LastSequence();

  ReadOptions table_opts;
  table_opts.verify_checksums = options_.paranoid_checks;
  table_opts.fill_cache = opts.fill_cache;
  // The sink outlives the iterator by contract: it belongs to the request, and
  // the iterator is created and destroyed inside one. Every data block this
  // scan pulls off disk lands here.
  table_opts.stats = opts.stats;

  std::vector<Iterator*> children;
  children.push_back(mem_->NewIterator());
  if (imm_ != nullptr) children.push_back(imm_->NewIterator());

  Version* current = versions_->current();
  current->AddIterators(table_opts, &children);

  Iterator* merged = NewMergingIterator(internal_comparator_, std::move(children));

  // PIN THE VERSION for the life of the iterator. This is what makes a scan
  // safe against concurrent compaction: the files it reads cannot be unlinked
  // until this reference is dropped.
  current->Ref();
  merged->RegisterCleanup(
      [](void* v, void*) { reinterpret_cast<Version*>(v)->Unref(); }, current,
      nullptr);

  // Same argument for the memtables: the iterator reads directly out of their
  // arenas, so a concurrent flush must not be able to free them.
  mem_->Ref();
  merged->RegisterCleanup(
      [](void* m, void*) { reinterpret_cast<MemTable*>(m)->Unref(); }, mem_,
      nullptr);
  if (imm_ != nullptr) {
    imm_->Ref();
    merged->RegisterCleanup(
        [](void* m, void*) { reinterpret_cast<MemTable*>(m)->Unref(); }, imm_,
        nullptr);
  }

  return std::unique_ptr<Iterator>(
      NewDBIterator(internal_comparator_, merged, snapshot));
}

// --- background work -------------------------------------------------------

void DBImpl::MaybeScheduleCompaction() { bg_work_cv_.notify_all(); }

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    while (!shutting_down_.load(std::memory_order_relaxed) &&
           !HasBackgroundWork()) {
      bg_done_cv_.notify_all();
      bg_work_cv_.wait(lock);
    }
    if (shutting_down_.load(std::memory_order_relaxed)) break;

    bg_busy_ = true;
    BackgroundCompaction(lock);
    bg_busy_ = false;

    bg_done_cv_.notify_all();
  }
  bg_busy_ = false;
  bg_done_cv_.notify_all();
}

void DBImpl::BackgroundCompaction(std::unique_lock<std::mutex>& lock) {
  // Flushing the immutable memtable takes priority: it is what frees writers
  // that are waiting for memtable space.
  if (imm_ != nullptr) {
    Status s = CompactMemTable(lock);
    if (!s.ok()) bg_error_ = s;
    return;
  }

  std::unique_ptr<Compaction> c(versions_->PickCompaction());
  if (c == nullptr) return;

  Status status;
  if (c->IsTrivialMove()) {
    // One file, nothing below it overlaps. Move it down by editing metadata;
    // no bytes are read or written.
    FileMetaData* f = c->input(0, 0);
    c->edit()->RemoveFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size, Slice(f->smallest),
                       Slice(f->largest));
    status = versions_->LogAndApply(c->edit(), &mutex_);
    ++stats_.trivial_moves;
  } else {
    CompactionState compact(c.get());
    status = DoCompactionWork(&compact, lock);
    c->ReleaseInputs();
    DeleteObsoleteFiles();
  }

  if (!status.ok() && !shutting_down_.load(std::memory_order_relaxed)) {
    bg_error_ = status;
  }
}

Status DBImpl::CompactMemTable(std::unique_lock<std::mutex>& lock) {
  assert(imm_ != nullptr);

  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status s = WriteLevel0Table(imm_, &edit, base, lock);
  base->Unref();

  if (s.ok()) {
    edit.SetLogNumber(logfile_number_);  // earlier logs are now redundant
    s = versions_->LogAndApply(&edit, &mutex_);
  }

  if (s.ok()) {
    imm_->Unref();
    imm_ = nullptr;
    ++stats_.flushes;
    DeleteObsoleteFiles();
  }
  return s;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base,
                                std::unique_lock<std::mutex>& lock) {
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);

  const std::string fname = TableFileName(dbname_, meta.number);
  Status s;
  uint64_t count = 0;

  // Build the table with the lock released - this is disk I/O and there is no
  // reason for writers to be blocked during it.
  lock.unlock();
  {
    std::unique_ptr<WritableFile> file;
    s = WritableFile::Open(fname, /*append=*/false, &file);
    if (s.ok()) {
      TableBuilder builder(options_, file.get());
      MemTable::Iterator it(mem);
      for (it.SeekToFirst(); it.Valid(); it.Next()) {
        if (count == 0) meta.smallest.assign(it.key().data(), it.key().size());
        meta.largest.assign(it.key().data(), it.key().size());
        builder.Add(it.key(), it.value());
        ++count;
      }
      if (count == 0) {
        builder.Abandon();
        file->Close();
        RemoveFile(fname);
      } else {
        s = builder.Finish();
        if (s.ok()) meta.file_size = builder.FileSize();
        if (s.ok()) s = file->Sync();
        Status cs = file->Close();
        if (s.ok()) s = cs;
        if (!s.ok()) RemoveFile(fname);
      }
    }
  }
  lock.lock();

  pending_outputs_.erase(meta.number);

  if (s.ok() && count > 0) {
    // Push the new table past L0 when it overlaps nothing, saving a compaction.
    const Slice min_user_key = ExtractUserKey(Slice(meta.smallest));
    const Slice max_user_key = ExtractUserKey(Slice(meta.largest));
    const int level = versions_->PickLevelForMemTableOutput(min_user_key, max_user_key);

    edit->AddFile(level, meta.number, meta.file_size, Slice(meta.smallest),
                  Slice(meta.largest));
    stats_.bytes_flushed += meta.file_size;
  }
  return s;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  const uint64_t file_number = versions_->NewFileNumber();
  pending_outputs_.insert(file_number);

  CompactionState::Output out;
  out.number = file_number;
  compact->outputs.push_back(out);

  const std::string fname = TableFileName(dbname_, file_number);
  Status s = WritableFile::Open(fname, /*append=*/false, &compact->outfile);
  if (s.ok()) {
    compact->builder = std::make_unique<TableBuilder>(options_, compact->outfile.get());
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact, Iterator* input) {
  assert(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
  Status s = input->status();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  compact->builder.reset();

  if (s.ok()) s = compact->outfile->Sync();
  Status cs = compact->outfile->Close();
  if (s.ok()) s = cs;
  compact->outfile.reset();

  if (!s.ok()) RemoveFile(TableFileName(dbname_, output_number));
  return s;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (const auto& out : compact->outputs) {
    compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size,
                                         Slice(out.smallest), Slice(out.largest));
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact,
                                std::unique_lock<std::mutex>& lock) {
  // Anything at or below the oldest live snapshot can have its shadowed
  // versions dropped, because no reader can ever ask for them again.
  compact->smallest_snapshot = snapshots_.empty() ? versions_->LastSequence()
                                                  : *snapshots_.begin();

  std::unique_ptr<Iterator> input(versions_->MakeInputIterator(compact->compaction));

  // The merge itself is pure I/O and CPU; hold no lock for it.
  lock.unlock();

  input->SeekToFirst();
  Status status;

  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  uint64_t dropped = 0;

  for (; input->Valid() && !shutting_down_.load(std::memory_order_relaxed);) {
    const Slice key = input->key();

    bool drop = false;
    ParsedInternalKey ikey;
    if (!ParseInternalKey(key, &ikey)) {
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          ikey.user_key.compare(Slice(current_user_key)) != 0) {
        current_user_key = ikey.user_key.ToString();
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // A newer version of this key is already visible to every snapshot, so
        // this one can never be read again.
        drop = true;
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // THE TOMBSTONE RULE, and the reason it is not simply "drop deletions".
        //
        // A tombstone may only be discarded when no OLDER version of the key
        // can exist in any level below this compaction's output. Drop it too
        // early and the value it was hiding becomes visible again - deleted
        // data comes back. IsBaseLevelForKey is what proves nothing older
        // exists beneath.
        drop = true;
      }
      last_sequence_for_key = ikey.sequence;
    }

    if (!drop) {
      if (compact->builder == nullptr) {
        lock.lock();
        status = OpenCompactionOutputFile(compact);
        lock.unlock();
        if (!status.ok()) break;
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.assign(key.data(), key.size());
      }
      compact->current_output()->largest.assign(key.data(), key.size());
      compact->builder->Add(key, input->value());

      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input.get());
        if (!status.ok()) break;
      }
    } else {
      ++dropped;
    }

    input->Next();
  }

  // A compaction interrupted by shutdown MUST NOT be installed.
  //
  // Installing it would delete the input files while only part of their
  // contents had been written to the output - and if one of the keys not yet
  // written was a tombstone, the value it was shadowing in a lower level
  // becomes visible again. That is a silent resurrection of deleted data, and
  // it is exactly what the differential test caught.
  if (shutting_down_.load(std::memory_order_relaxed)) {
    status = Status::IOError("compaction abandoned: database is shutting down");
  }

  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input.get());
  } else if (compact->builder != nullptr) {
    compact->builder->Abandon();
    compact->builder.reset();
    compact->outfile.reset();
  }
  if (status.ok()) status = input->status();
  input.reset();

  lock.lock();

  for (const auto& out : compact->outputs) pending_outputs_.erase(out.number);

  if (status.ok()) {
    status = InstallCompactionResults(compact);
    ++stats_.compactions;
    stats_.compaction_bytes_written += compact->total_bytes;
    stats_.keys_dropped += dropped;
  } else {
    for (const auto& out : compact->outputs) {
      RemoveFile(TableFileName(dbname_, out.number));
    }
  }
  return status;
}

void DBImpl::DeleteObsoleteFiles() {
  if (!bg_error_.ok()) return;  // do not delete anything if state is uncertain

  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  if (!GetChildren(dbname_, &filenames).ok()) return;

  for (const auto& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (!ParseFileName(filename, &number, &type)) continue;

    bool keep = true;
    switch (type) {
      case FileType::kLog:
        // The current log, plus any log still needed by an unflushed memtable.
        keep = (number >= versions_->LogNumber()) || (number == logfile_number_);
        break;
      case FileType::kManifest:
        keep = (number >= versions_->ManifestFileNumber());
        break;
      case FileType::kTable:
        keep = (live.count(number) > 0);
        break;
      case FileType::kTemp:
        keep = (live.count(number) > 0);
        break;
      case FileType::kCurrent:
      case FileType::kDescriptor:
      case FileType::kUnknown:
        keep = true;
        break;
    }

    if (!keep) {
      if (type == FileType::kTable) table_cache_->Evict(number);
      RemoveFile(dbname_ + "/" + filename);
      ++stats_.files_deleted;
    }
  }
}

// --- recovery --------------------------------------------------------------

Status DBImpl::Recover() {
  Status s = CreateDir(dbname_);
  if (!s.ok()) return s;

  bool save_manifest = false;
  s = versions_->Recover(&save_manifest);

  if (s.IsNotFound()) {
    // Brand new database. Write an initial MANIFEST so CURRENT exists.
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_, "does not exist");
    }
    VersionEdit new_db;
    new_db.SetLogNumber(0);
    new_db.SetNextFile(2);
    new_db.SetLastSequence(0);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      s = versions_->LogAndApply(&new_db, &mutex_);
    }
    if (!s.ok()) return s;
  } else if (!s.ok()) {
    return s;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "database already exists");
  }

  // Replay every log at or after the one the MANIFEST says is current. There
  // can be more than one if a crash landed between a flush and its commit.
  std::vector<std::string> filenames;
  s = GetChildren(dbname_, &filenames);
  if (!s.ok()) return s;

  std::vector<uint64_t> logs;
  for (const auto& filename : filenames) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(filename, &number, &type) && type == FileType::kLog &&
        number >= versions_->LogNumber()) {
      logs.push_back(number);
    }
  }
  std::sort(logs.begin(), logs.end());

  SequenceNumber max_sequence = versions_->LastSequence();
  for (uint64_t number : logs) {
    std::unique_ptr<SequentialFile> file;
    s = SequentialFile::Open(LogFileName(dbname_, number), &file);
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
      if (record.size() < WriteBatch::kHeaderSize) continue;
      batch.SetContents(record);
      s = batch.InsertInto(mem_);
      if (!s.ok()) return s;

      const SequenceNumber last_in_batch =
          batch.Sequence() + static_cast<SequenceNumber>(batch.Count()) - 1;
      if (last_in_batch > max_sequence) max_sequence = last_in_batch;
      ++stats_.wal_records_replayed;
    }

    // A dropped tail is what a crash looks like, not an error.
    if (options_.paranoid_checks && !replay_status.ok()) return replay_status;
    versions_->MarkFileNumberUsed(number);
  }

  if (max_sequence > versions_->LastSequence()) {
    versions_->SetLastSequence(max_sequence);
  }

  // Flush anything recovered from the logs so recovery is idempotent and the
  // old logs can be reclaimed.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t new_log_number = versions_->NewFileNumber();
    s = NewLogFileLocked(new_log_number);
    if (!s.ok()) return s;

    if (mem_->NumEntries() > 0) {
      imm_ = mem_;
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      s = CompactMemTable(lock);
      if (!s.ok()) return s;
    } else {
      VersionEdit edit;
      edit.SetLogNumber(new_log_number);
      s = versions_->LogAndApply(&edit, &mutex_);
      if (!s.ok()) return s;
    }
    DeleteObsoleteFiles();
  }

  return Status::OK();
}

Stats DBImpl::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats s = stats_;
  s.memtable_bytes = mem_->ApproximateMemoryUsage();
  s.sequence = versions_->LastSequence();

  uint64_t total_files = 0;
  uint64_t total_bytes = 0;
  for (int level = 0; level < kNumLevels; ++level) {
    const int n = versions_->NumLevelFiles(level);
    total_files += static_cast<uint64_t>(n);
    total_bytes += static_cast<uint64_t>(versions_->NumLevelBytes(level));
    s.files_per_level[level] = static_cast<uint64_t>(n);
    s.bytes_per_level[level] = static_cast<uint64_t>(versions_->NumLevelBytes(level));
  }
  s.num_sstables = total_files;
  s.total_bytes_on_disk = total_bytes;

  s.filter_rejections = table_cache_->FilterRejections();

  if (block_cache_) {
    s.cache_hits = block_cache_->Hits();
    s.cache_misses = block_cache_->Misses();
    s.cache_evictions = block_cache_->Evictions();
    s.cache_bytes = block_cache_->TotalCharge();
  }
  return s;
}

}  // namespace

Status DB::Open(const Options& options, const std::string& name,
                std::unique_ptr<DB>* dbptr) {
  dbptr->reset();
  auto impl = std::make_unique<DBImpl>(options, name);
  Status s = impl->Recover();
  if (!s.ok()) return s;
  impl->StartBackgroundThread();
  *dbptr = std::move(impl);
  return Status::OK();
}

}  // namespace sextant::lsm
