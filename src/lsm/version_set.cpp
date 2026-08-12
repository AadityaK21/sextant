#include "version_set.h"

#include <algorithm>
#include <cstdio>

#include "coding.h"
#include "filename.h"
#include "merger.h"
#include "table_builder.h"

namespace sextant::lsm {

double MaxBytesForLevel(int level) {
  // L0 is governed by file COUNT, not bytes, because its files overlap and the
  // cost of reading it scales with how many there are.
  double result = 10.0 * 1048576.0;  // L1 = 10 MB
  while (level > 1) {
    result *= 10;
    --level;
  }
  return result;
}

namespace {

const InternalKeyComparator kIcmp{};

bool AfterFile(const FileMetaData* f, const Slice* user_key) {
  return user_key != nullptr &&
         user_key->compare(ExtractUserKey(Slice(f->largest))) > 0;
}

bool BeforeFile(const FileMetaData* f, const Slice* user_key) {
  return user_key != nullptr &&
         user_key->compare(ExtractUserKey(Slice(f->smallest))) < 0;
}

// Binary search a sorted, non-overlapping level for the first file whose
// largest key is >= key. Only valid for L1+, where files do not overlap.
int FindFile(const std::vector<FileMetaData*>& files, const Slice& key) {
  uint32_t left = 0;
  uint32_t right = static_cast<uint32_t>(files.size());
  while (left < right) {
    const uint32_t mid = (left + right) / 2;
    if (kIcmp.Compare(Slice(files[mid]->largest), key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return static_cast<int>(right);
}

bool SomeFileOverlapsRange(bool disjoint_sorted_files,
                           const std::vector<FileMetaData*>& files,
                           const Slice* smallest_user_key,
                           const Slice* largest_user_key) {
  if (!disjoint_sorted_files) {
    // L0: files overlap, so every one has to be checked.
    for (const auto* f : files) {
      if (AfterFile(f, smallest_user_key) || BeforeFile(f, largest_user_key)) {
        continue;
      }
      return true;
    }
    return false;
  }

  uint32_t index = 0;
  if (smallest_user_key != nullptr) {
    std::string small;
    AppendInternalKey(&small, ParsedInternalKey(*smallest_user_key,
                                                kMaxSequenceNumber,
                                                kValueTypeForSeek));
    index = static_cast<uint32_t>(FindFile(files, Slice(small)));
  }
  if (index >= files.size()) return false;
  return !BeforeFile(files[index], largest_user_key);
}

// Iterates a level's file list, yielding (largest key -> encoded handle). The
// two-level iterator turns each handle into an iterator over that file, which
// is how an entire sorted level reads as one continuous stream.
class LevelFileNumIterator final : public Iterator {
 public:
  explicit LevelFileNumIterator(const std::vector<FileMetaData*>* flist)
      : flist_(flist), index_(static_cast<uint32_t>(flist->size())) {}

  bool Valid() const override { return index_ < flist_->size(); }

  void Seek(const Slice& target) override {
    index_ = static_cast<uint32_t>(FindFile(*flist_, target));
  }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override {
    index_ = flist_->empty() ? 0 : static_cast<uint32_t>(flist_->size() - 1);
  }
  void Next() override {
    assert(Valid());
    ++index_;
  }
  void Prev() override {
    assert(Valid());
    if (index_ == 0) {
      index_ = static_cast<uint32_t>(flist_->size());  // invalid
    } else {
      --index_;
    }
  }

  Slice key() const override {
    assert(Valid());
    return Slice((*flist_)[index_]->largest);
  }

  Slice value() const override {
    assert(Valid());
    EncodeFixed64BE(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64BE(value_buf_ + 8, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }

  Status status() const override { return Status::OK(); }

 private:
  const std::vector<FileMetaData*>* const flist_;
  uint32_t index_;
  mutable char value_buf_[16];
};

Iterator* GetFileIterator(void* arg, const ReadOptions& options,
                          const Slice& file_value) {
  auto* cache = reinterpret_cast<TableCache*>(arg);
  if (file_value.size() != 16) {
    return NewErrorIterator(Status::Corruption("bad file handle in level index"));
  }
  return cache->NewIterator(options, DecodeFixed64BE(file_value.data()),
                            DecodeFixed64BE(file_value.data() + 8));
}

// --- point lookup plumbing -------------------------------------------------

enum class SaverState { kNotFound, kFound, kDeleted, kCorrupt };

struct Saver {
  SaverState state = SaverState::kNotFound;
  const Slice* user_key = nullptr;
  std::string* value = nullptr;
};

void SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  auto* saver = reinterpret_cast<Saver*>(arg);
  ParsedInternalKey parsed;
  if (!ParseInternalKey(ikey, &parsed)) {
    saver->state = SaverState::kCorrupt;
    return;
  }
  if (parsed.user_key.compare(*saver->user_key) != 0) return;
  if (parsed.type == kTypeValue) {
    saver->state = SaverState::kFound;
    saver->value->assign(v.data(), v.size());
  } else {
    saver->state = SaverState::kDeleted;
  }
}

bool NewestFirst(FileMetaData* a, FileMetaData* b) { return a->number > b->number; }

}  // namespace

// --- Version ---------------------------------------------------------------

Version::~Version() {
  assert(refs_ == 0);
  prev_->next_ = next_;
  next_->prev_ = prev_;
  for (int level = 0; level < kNumLevels; ++level) {
    for (auto* f : files_[level]) {
      assert(f->refs > 0);
      if (--f->refs == 0) delete f;
    }
  }
}

void Version::Ref() { ++refs_; }

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) delete this;
}

int64_t Version::NumBytes(int level) const {
  int64_t sum = 0;
  for (const auto* f : files_[level]) sum += static_cast<int64_t>(f->file_size);
  return sum;
}

Status Version::Get(const ReadOptions& options, const LookupKey& k,
                    std::string* value, GetStats* stats) {
  const Slice user_key = k.user_key();
  const Slice internal_key = k.internal_key();

  std::vector<FileMetaData*> tmp;
  for (int level = 0; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;

    FileMetaData* const* candidates;
    size_t num_candidates;
    FileMetaData* tmp2 = nullptr;

    if (level == 0) {
      // L0 files overlap, so collect every one whose range contains the key and
      // sort them NEWEST FIRST. Getting this order wrong resurrects deleted
      // values.
      tmp.clear();
      for (auto* f : files_[0]) {
        if (user_key.compare(ExtractUserKey(Slice(f->smallest))) >= 0 &&
            user_key.compare(ExtractUserKey(Slice(f->largest))) <= 0) {
          tmp.push_back(f);
        } else if (stats != nullptr) {
          ++stats->range_rejections;
        }
      }
      if (tmp.empty()) continue;
      std::sort(tmp.begin(), tmp.end(), NewestFirst);
      candidates = tmp.data();
      num_candidates = tmp.size();
    } else {
      // L1+ is sorted and non-overlapping, so AT MOST ONE file can hold the
      // key and a binary search finds it. This is the payoff for compaction.
      const int index = FindFile(files_[level], internal_key);
      if (index >= static_cast<int>(files_[level].size())) {
        num_candidates = 0;
        candidates = nullptr;
      } else {
        tmp2 = files_[level][static_cast<size_t>(index)];
        if (user_key.compare(ExtractUserKey(Slice(tmp2->smallest))) < 0) {
          num_candidates = 0;  // before this file's range, so not in this level
          candidates = nullptr;
          if (stats != nullptr) ++stats->range_rejections;
        } else {
          candidates = &tmp2;
          num_candidates = 1;
        }
      }
    }

    for (size_t i = 0; i < num_candidates; ++i) {
      FileMetaData* f = candidates[i];
      if (stats != nullptr) ++stats->files_probed;

      Saver saver;
      saver.state = SaverState::kNotFound;
      saver.user_key = &user_key;
      saver.value = value;

      Status s = vset_->table_cache_->Get(options, f->number, f->file_size,
                                          internal_key, &saver, &SaveValue);
      if (!s.ok()) return s;

      switch (saver.state) {
        case SaverState::kNotFound:
          break;  // keep looking, in this level or the next
        case SaverState::kFound:
          return Status::OK();
        case SaverState::kDeleted:
          return Status::NotFound(Slice());
        case SaverState::kCorrupt:
          return Status::Corruption("corrupt entry in sstable");
      }
    }
  }

  return Status::NotFound(Slice());
}

void Version::AddIterators(const ReadOptions& options, std::vector<Iterator*>* iters) {
  // L0 files overlap, so each needs its own iterator in the merge.
  for (auto* f : files_[0]) {
    iters->push_back(vset_->table_cache_->NewIterator(options, f->number,
                                                      f->file_size));
  }

  // L1+ is sorted and disjoint, so a whole level reads as ONE concatenating
  // iterator rather than one per file. That is what keeps a merge over a large
  // database down to (L0 files + number of levels) children instead of
  // thousands.
  for (int level = 1; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;
    iters->push_back(NewTwoLevelIterator(new LevelFileNumIterator(&files_[level]),
                                         &GetFileIterator, vset_->table_cache_,
                                         options, kIcmp));
  }
}

void Version::GetOverlappingInputs(int level, const Slice* begin, const Slice* end,
                                   std::vector<FileMetaData*>* inputs) {
  inputs->clear();
  Slice user_begin, user_end;
  if (begin != nullptr) user_begin = *begin;
  if (end != nullptr) user_end = *end;

  for (size_t i = 0; i < files_[level].size();) {
    FileMetaData* f = files_[level][i++];
    const Slice file_start = ExtractUserKey(Slice(f->smallest));
    const Slice file_limit = ExtractUserKey(Slice(f->largest));

    if (begin != nullptr && file_limit.compare(user_begin) < 0) continue;
    if (end != nullptr && file_start.compare(user_end) > 0) continue;

    inputs->push_back(f);

    // L0 files overlap, so pulling one in can widen the range, which may pull
    // in others. Restart the scan with the wider range until it stabilises.
    if (level == 0) {
      if (begin != nullptr && file_start.compare(user_begin) < 0) {
        user_begin = file_start;
        inputs->clear();
        i = 0;
      } else if (end != nullptr && file_limit.compare(user_end) > 0) {
        user_end = file_limit;
        inputs->clear();
        i = 0;
      }
    }
  }
}

std::string Version::DebugString() const {
  std::string r;
  char buf[128];
  for (int level = 0; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;
    std::snprintf(buf, sizeof(buf), "L%d: %zu files, %.2f MB\n", level,
                  files_[level].size(),
                  static_cast<double>(NumBytes(level)) / 1048576.0);
    r += buf;
  }
  return r;
}

// --- VersionSet::Builder ---------------------------------------------------
//
// Applies a sequence of edits to a base Version and produces the file list for
// a new one. Deletions are collected before additions so that a file which is
// both removed and re-added within one batch ends up present.
class VersionSet::Builder {
 public:
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    base_->Ref();
  }

  ~Builder() {
    for (auto& level : levels_) {
      for (auto* f : level.added_files) {
        if (--f->refs == 0) delete f;
      }
    }
    base_->Unref();
  }

  void Apply(const VersionEdit* edit) {
    for (const auto& [level, key] : edit->compact_pointers()) {
      vset_->compact_pointer_[level] = key;
    }
    for (const auto& [level, number] : edit->deleted_files()) {
      levels_[level].deleted_files.insert(number);
    }
    for (const auto& [level, meta] : edit->new_files()) {
      auto* f = new FileMetaData(meta);
      f->refs = 1;
      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files.push_back(f);
    }
  }

  void SaveTo(Version* v) {
    for (int level = 0; level < kNumLevels; ++level) {
      auto added = levels_[level].added_files;
      std::sort(added.begin(), added.end(), BySmallestKey);

      const auto& base_files = base_->files_[level];
      v->files_[level].reserve(base_files.size() + added.size());

      // Merge the surviving base files with the newly added ones, keeping the
      // level sorted by smallest key.
      size_t bi = 0;
      for (auto* added_file : added) {
        while (bi < base_files.size() &&
               BySmallestKey(base_files[bi], added_file)) {
          MaybeAddFile(v, level, base_files[bi]);
          ++bi;
        }
        MaybeAddFile(v, level, added_file);
      }
      for (; bi < base_files.size(); ++bi) {
        MaybeAddFile(v, level, base_files[bi]);
      }

#ifndef NDEBUG
      // INVARIANT: above L0, files must not overlap. If this ever fires, a
      // compaction produced an inconsistent level and every subsequent binary
      // search would be silently wrong.
      if (level > 0) {
        for (size_t i = 1; i < v->files_[level].size(); ++i) {
          const Slice prev_end(v->files_[level][i - 1]->largest);
          const Slice this_begin(v->files_[level][i]->smallest);
          assert(kIcmp.Compare(prev_end, this_begin) < 0);
        }
      }
#endif
    }
  }

 private:
  static bool BySmallestKey(FileMetaData* a, FileMetaData* b) {
    const int r = kIcmp.Compare(Slice(a->smallest), Slice(b->smallest));
    if (r != 0) return r < 0;
    return a->number < b->number;  // stable tie-break
  }

  void MaybeAddFile(Version* v, int level, FileMetaData* f) {
    if (levels_[level].deleted_files.count(f->number) > 0) return;
    ++f->refs;
    v->files_[level].push_back(f);
  }

  struct LevelState {
    std::set<uint64_t> deleted_files;
    std::vector<FileMetaData*> added_files;
  };

  VersionSet* vset_;
  Version* base_;
  LevelState levels_[kNumLevels];
};

// --- VersionSet ------------------------------------------------------------

VersionSet::VersionSet(std::string dbname, const Options& options,
                       TableCache* table_cache)
    : dbname_(std::move(dbname)), options_(options), table_cache_(table_cache) {
  dummy_versions_.next_ = &dummy_versions_;
  dummy_versions_.prev_ = &dummy_versions_;
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);
}

void VersionSet::AppendVersion(Version* v) {
  assert(v->refs_ == 0);
  assert(v != current_);
  if (current_ != nullptr) current_->Unref();
  current_ = v;
  v->Ref();

  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

int VersionSet::NumLevelFiles(int level) const {
  return static_cast<int>(current_->files_[level].size());
}

int64_t VersionSet::NumLevelBytes(int level) const {
  return current_->NumBytes(level);
}

// Decide which level is most in need of compaction, and by how much.
void VersionSet::Finalize(Version* v) {
  int best_level = -1;
  double best_score = -1;

  for (int level = 0; level < kNumLevels - 1; ++level) {
    double score;
    if (level == 0) {
      // L0 is scored by FILE COUNT, not bytes. Its files overlap, so read cost
      // scales with how many there are, and small files from frequent flushes
      // would otherwise never trigger a compaction despite hurting reads.
      score = static_cast<double>(v->files_[0].size()) /
              static_cast<double>(kL0CompactionTrigger);
    } else {
      const double byte_score =
          static_cast<double>(v->NumBytes(level)) / MaxBytesForLevel(level);

      // ALSO score by file count, which LevelDB does not do.
      //
      // Found by a test: with a small write buffer, every flush produces a tiny
      // file, and PickLevelForMemTableOutput pushes non-overlapping ones
      // straight down to L2. 212 files of 16 KB accumulated there while the
      // byte score sat at 0.018, so compaction never ran. Reads were still
      // correct - the files are disjoint and sorted - but the file handle count
      // and per-file overhead grew without bound.
      //
      // LevelDB avoids this by accident rather than design: its default 4 MB
      // buffer makes files large enough that the byte target is reached first.
      // Scoring both makes the behaviour independent of the buffer size.
      const double expected_files = MaxBytesForLevel(level) / kMaxFileSize;
      const double file_score =
          static_cast<double>(v->files_[level].size()) / expected_files;

      score = std::max(byte_score, file_score);
    }

    if (score > best_score) {
      best_score = score;
      best_level = level;
    }
  }

  v->vset_->compaction_level_ = best_level;
  v->vset_->compaction_score_ = best_score;
}

bool VersionSet::NeedsCompaction() const { return compaction_score_ >= 1.0; }

Status VersionSet::LogAndApply(VersionEdit* edit, std::mutex* mu) {
  if (edit->has_log_number()) {
    assert(edit->log_number() >= log_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }
  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  auto* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }
  Finalize(v);

  // Start a MANIFEST on first use.
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == nullptr) {
    new_manifest_file = ManifestFileName(dbname_, manifest_file_number_);
    s = WritableFile::Open(new_manifest_file, /*append=*/false, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = std::make_unique<wal::Writer>(descriptor_file_.get());
      s = WriteSnapshot(descriptor_log_.get());
    }
  }

  // Release the lock during I/O. Only one thread reaches here at a time
  // because callers hold the DB mutex, so the MANIFEST cannot interleave.
  mu->unlock();
  if (s.ok()) {
    std::string record;
    edit->EncodeTo(&record);
    s = descriptor_log_->AddRecord(Slice(record));
    if (s.ok()) s = descriptor_file_->Sync();
  }
  // CURRENT is written last and by atomic rename, so it never names a MANIFEST
  // that is not already durable.
  if (s.ok() && !new_manifest_file.empty()) {
    const std::string tmp = TempFileName(dbname_, manifest_file_number_);
    std::unique_ptr<WritableFile> file;
    Status ts = WritableFile::Open(tmp, /*append=*/false, &file);
    if (ts.ok()) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "MANIFEST-%06llu\n",
                    static_cast<unsigned long long>(manifest_file_number_));
      ts = file->Append(Slice(buf));
      if (ts.ok()) ts = file->Sync();
      Status cs = file->Close();
      if (ts.ok()) ts = cs;
      if (ts.ok()) ts = RenameFile(tmp, CurrentFileName(dbname_));
      if (!ts.ok()) RemoveFile(tmp);
    }
    s = ts;
  }
  mu->lock();

  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number();
  } else {
    delete v;
    if (!new_manifest_file.empty()) {
      descriptor_log_.reset();
      descriptor_file_.reset();
      RemoveFile(new_manifest_file);
    }
  }
  return s;
}

Status VersionSet::WriteSnapshot(wal::Writer* log) {
  // The first record of a fresh MANIFEST states the entire current state, so
  // recovery does not need any earlier MANIFEST.
  VersionEdit edit;
  for (int level = 0; level < kNumLevels; ++level) {
    if (!compact_pointer_[level].empty()) {
      edit.SetCompactPointer(level, Slice(compact_pointer_[level]));
    }
    for (const auto* f : current_->files_[level]) {
      edit.AddFile(level, f->number, f->file_size, Slice(f->smallest),
                   Slice(f->largest));
    }
  }
  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(Slice(record));
}

namespace {

class ManifestReporter : public wal::Reader::Reporter {
 public:
  Status* status = nullptr;
  void Corruption(size_t, const Status& s) override {
    if (status != nullptr && status->ok()) *status = s;
  }
};

}  // namespace

Status VersionSet::Recover(bool* save_manifest) {
  *save_manifest = false;

  const std::string current_path = CurrentFileName(dbname_);
  if (!FileExists(current_path)) {
    return Status::NotFound(dbname_, "no CURRENT file");
  }

  uint64_t current_size = 0;
  Status s = GetFileSize(current_path, &current_size);
  if (!s.ok()) return s;
  if (current_size == 0) return Status::Corruption("CURRENT is empty");

  std::string current;
  {
    std::unique_ptr<SequentialFile> file;
    s = SequentialFile::Open(current_path, &file);
    if (!s.ok()) return s;
    std::string buf(static_cast<size_t>(current_size), '\0');
    Slice contents;
    s = file->Read(static_cast<size_t>(current_size), &contents, buf.data());
    if (!s.ok()) return s;
    current = contents.ToString();
  }
  while (!current.empty() && (current.back() == '\n' || current.back() == '\r')) {
    current.pop_back();
  }

  const std::string manifest_path = dbname_ + "/" + current;
  std::unique_ptr<SequentialFile> file;
  s = SequentialFile::Open(manifest_path, &file);
  if (!s.ok()) return s;

  bool have_log_number = false;
  bool have_next_file = false;
  bool have_last_sequence = false;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;

  Builder builder(this, current_);
  {
    ManifestReporter reporter;
    Status log_status;
    reporter.status = &log_status;

    wal::Reader reader(file.get(), &reporter, /*checksum=*/true);
    Slice record;
    std::string scratch;

    while (reader.ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (!s.ok()) break;

      builder.Apply(&edit);

      if (edit.has_log_number()) {
        log_number = edit.log_number();
        have_log_number = true;
      }
      if (edit.has_next_file_number()) {
        next_file = edit.next_file_number();
        have_next_file = true;
      }
      if (edit.has_last_sequence()) {
        last_sequence = edit.last_sequence();
        have_last_sequence = true;
      }
    }
    if (s.ok() && !log_status.ok()) s = log_status;
  }

  if (s.ok()) {
    if (!have_next_file) {
      s = Status::Corruption("no next-file-number entry in MANIFEST");
    } else if (!have_log_number) {
      s = Status::Corruption("no log-number entry in MANIFEST");
    } else if (!have_last_sequence) {
      s = Status::Corruption("no last-sequence entry in MANIFEST");
    }
  }
  if (!s.ok()) return s;

  auto* v = new Version(this);
  builder.SaveTo(v);
  Finalize(v);
  AppendVersion(v);

  manifest_file_number_ = next_file;
  next_file_number_ = next_file + 1;
  last_sequence_ = last_sequence;
  log_number_ = log_number;

  // Always start a fresh MANIFEST on open. Simpler than deciding whether the
  // old one can be appended to, at the cost of one extra small file per open.
  *save_manifest = true;
  return Status::OK();
}

int VersionSet::PickLevelForMemTableOutput(const Slice& smallest_user_key,
                                           const Slice& largest_user_key) {
  int level = 0;
  // If the new table overlaps nothing in L0, it can be pushed further down for
  // free, saving a compaction later. Stop before it would overlap too much of
  // the level below.
  if (!SomeFileOverlapsRange(false, current_->files_[0], &smallest_user_key,
                             &largest_user_key)) {
    static constexpr int kMaxMemCompactLevel = 2;
    while (level < kMaxMemCompactLevel) {
      if (SomeFileOverlapsRange(true, current_->files_[level + 1],
                                &smallest_user_key, &largest_user_key)) {
        break;
      }
      ++level;
    }
  }
  return level;
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  // Walk every LIVE version, not just the current one. A version pinned by an
  // open iterator still needs its files on disk.
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
    for (int level = 0; level < kNumLevels; ++level) {
      for (const auto* f : v->files_[level]) live->insert(f->number);
    }
  }
}

Compaction* VersionSet::PickCompaction() {
  if (!NeedsCompaction()) return nullptr;

  const int level = compaction_level_;
  assert(level >= 0 && level + 1 < kNumLevels);

  auto* c = new Compaction(options_, level);

  // Resume from where the last compaction of this level stopped, so the level
  // is swept evenly instead of the same key range being rewritten forever.
  for (auto* f : current_->files_[level]) {
    if (compact_pointer_[level].empty() ||
        kIcmp.Compare(Slice(f->largest), Slice(compact_pointer_[level])) > 0) {
      c->inputs_[0].push_back(f);
      break;
    }
  }
  if (c->inputs_[0].empty()) {
    c->inputs_[0].push_back(current_->files_[level][0]);  // wrap around
  }

  c->input_version_ = current_;
  c->input_version_->Ref();

  if (level == 0) {
    // L0 files overlap, so compacting one means compacting every file that
    // shares any part of its range - otherwise the output would still overlap
    // what remains in L0 and the invariant would break.
    const Slice smallest = ExtractUserKey(Slice(c->inputs_[0][0]->smallest));
    const Slice largest = ExtractUserKey(Slice(c->inputs_[0][0]->largest));
    current_->GetOverlappingInputs(0, &smallest, &largest, &c->inputs_[0]);
    assert(!c->inputs_[0].empty());
  } else {
    // MERGE SMALL ADJACENT FILES rather than relabelling them one at a time.
    //
    // Without this, a level full of tiny files compacts by "trivial move":
    // each file is relabelled into the next level, one per compaction, and the
    // small files simply cascade downward forever. The file count is never
    // reduced, it just relocates.
    //
    // Files in L1+ are disjoint and sorted, so consecutive ones can be merged
    // safely. Pulling in neighbours until the combined size approaches one
    // output file turns N tiny files into one properly-sized file.
    const auto& files = current_->files_[level];
    size_t start = 0;
    while (start < files.size() && files[start] != c->inputs_[0][0]) ++start;

    uint64_t total = c->inputs_[0][0]->file_size;
    static constexpr size_t kMaxMergeInputs = 24;
    for (size_t i = start + 1;
         i < files.size() && c->inputs_[0].size() < kMaxMergeInputs; ++i) {
      if (total + files[i]->file_size > kMaxFileSize) break;
      total += files[i]->file_size;
      c->inputs_[0].push_back(files[i]);
    }
  }

  SetupOtherInputs(c);
  return c;
}

void VersionSet::SetupOtherInputs(Compaction* c) {
  const int level = c->level();

  Slice smallest = ExtractUserKey(Slice(c->inputs_[0][0]->smallest));
  Slice largest = ExtractUserKey(Slice(c->inputs_[0][0]->largest));
  for (auto* f : c->inputs_[0]) {
    if (ExtractUserKey(Slice(f->smallest)).compare(smallest) < 0) {
      smallest = ExtractUserKey(Slice(f->smallest));
    }
    if (ExtractUserKey(Slice(f->largest)).compare(largest) > 0) {
      largest = ExtractUserKey(Slice(f->largest));
    }
  }

  current_->GetOverlappingInputs(level + 1, &smallest, &largest, &c->inputs_[1]);

  // Advance the cursor so the next compaction of this level starts after the
  // range we are about to rewrite.
  compact_pointer_[level] = c->inputs_[0].back()->largest;
  c->edit_.SetCompactPointer(level, Slice(compact_pointer_[level]));
}

Iterator* VersionSet::MakeInputIterator(Compaction* c) {
  ReadOptions options;
  options.verify_checksums = options_.paranoid_checks;
  // Compaction streams through every block exactly once. Caching them would
  // evict the working set of live reads for no benefit.
  options.fill_cache = false;

  std::vector<Iterator*> list;
  for (int which = 0; which < 2; ++which) {
    if (c->inputs_[which].empty()) continue;

    if (c->level() + which == 0) {
      for (auto* f : c->inputs_[which]) {
        list.push_back(table_cache_->NewIterator(options, f->number, f->file_size));
      }
    } else {
      list.push_back(NewTwoLevelIterator(new LevelFileNumIterator(&c->inputs_[which]),
                                         &GetFileIterator, table_cache_, options,
                                         kIcmp));
    }
  }
  return NewMergingIterator(icmp_, std::move(list));
}

std::string VersionSet::LevelSummary() const {
  std::string r;
  char buf[64];
  for (int level = 0; level < kNumLevels; ++level) {
    const int n = NumLevelFiles(level);
    if (n == 0) continue;
    std::snprintf(buf, sizeof(buf), "L%d:%d(%.1fMB) ", level, n,
                  static_cast<double>(NumLevelBytes(level)) / 1048576.0);
    r += buf;
  }
  return r;
}

// --- Compaction ------------------------------------------------------------

Compaction::Compaction(const Options& /*options*/, int level)
    : level_(level), max_output_file_size_(kMaxFileSize) {
  for (int i = 0; i < kNumLevels; ++i) level_ptrs_[i] = 0;
}

Compaction::~Compaction() {
  if (input_version_ != nullptr) input_version_->Unref();
}

bool Compaction::IsTrivialMove() const {
  // One file, nothing below it to merge with: just relabel it as belonging to
  // the next level. No bytes are read or written at all.
  return num_input_files(0) == 1 && num_input_files(1) == 0;
}

void Compaction::AddInputDeletions(VersionEdit* edit) {
  for (int which = 0; which < 2; ++which) {
    for (auto* f : inputs_[which]) {
      edit->RemoveFile(level_ + which, f->number);
    }
  }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  // Called with monotonically increasing keys during a compaction, so each
  // level's cursor resumes rather than rescanning from the start.
  for (int level = level_ + 2; level < kNumLevels; ++level) {
    const auto& files = input_version_->files_[level];
    while (level_ptrs_[level] < files.size()) {
      FileMetaData* f = files[level_ptrs_[level]];
      if (user_key.compare(ExtractUserKey(Slice(f->largest))) <= 0) {
        if (user_key.compare(ExtractUserKey(Slice(f->smallest))) >= 0) {
          return false;  // an older version may live here
        }
        break;
      }
      ++level_ptrs_[level];
    }
  }
  return true;
}

void Compaction::ReleaseInputs() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
    input_version_ = nullptr;
  }
}

}  // namespace sextant::lsm
