#include "table.h"

#include <cassert>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "cache.h"
#include "coding.h"
#include "filter_block.h"
#include "table_builder.h"

namespace sextant::lsm {

struct Table::Rep {
  Options options;
  Status status;
  std::unique_ptr<RandomAccessFile> file;
  uint64_t cache_id = 0;
  BlockHandle metaindex_handle;
  std::unique_ptr<Block> index_block;
  InternalKeyComparator comparator{};

  // Filter block plus the buffer it points into. Both null when filters are
  // disabled or the table predates them.
  std::unique_ptr<FilterBlockReader> filter;
  std::unique_ptr<const char[]> filter_data;

  // Atomic for the same reason TableCache::filter_rejections_ is: one Table is
  // shared by every reader thread, and InternalGet runs without the DB mutex.
  // See the note in table_cache.h.
  std::atomic<uint64_t> filter_rejections{0};
};

Table::~Table() { delete rep_; }

uint64_t Table::FilterRejections() const {
  return rep_->filter_rejections.load(std::memory_order_relaxed);
}

Status Table::Open(const Options& options, std::unique_ptr<RandomAccessFile> file,
                   uint64_t size, std::unique_ptr<Table>* table) {
  table->reset();

  if (size < static_cast<uint64_t>(Footer::kEncodedLength)) {
    return Status::Corruption("file is too short to be an sstable");
  }

  // Step 1: the footer lives at a known distance from the end of the file.
  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // Step 2: read the index block once and keep it. It is roughly one entry per
  // data block, so it is small enough to stay resident.
  BlockContents index_block_contents;
  ReadOptions opt;
  opt.verify_checksums = options.paranoid_checks;
  s = ReadBlock(file.get(), opt, footer.index_handle(), &index_block_contents);
  if (!s.ok()) return s;

  auto* rep = new Table::Rep;
  rep->options = options;
  rep->file = std::move(file);
  rep->metaindex_handle = footer.metaindex_handle();
  rep->index_block = std::make_unique<Block>(index_block_contents);
  rep->cache_id = (options.block_cache != nullptr) ? options.block_cache->NewId() : 0;

  auto owned = std::unique_ptr<Table>(new Table(rep));
  owned->ReadFilter(opt);
  *table = std::move(owned);
  return Status::OK();
}

// The metaindex names the filter block. A table written without filters simply
// has an empty metaindex, so this is a no-op and the read path degrades to
// "check every block" - which is exactly the day-2 behaviour.
void Table::ReadFilter(const ReadOptions& opt) {
  if (rep_->options.filter_policy == nullptr) return;

  BlockContents meta_contents;
  if (!ReadBlock(rep_->file.get(), opt, rep_->metaindex_handle, &meta_contents).ok()) {
    return;  // filters are an optimisation; failing to load one is not fatal
  }

  Block meta(meta_contents);
  std::unique_ptr<Iterator> iter(meta.NewIterator(rep_->comparator));

  std::string key;
  AppendInternalKey(&key, ParsedInternalKey(Slice(TableBuilder::kFilterBlockKey),
                                            kMaxSequenceNumber, kTypeValue));
  iter->Seek(Slice(key));
  if (!iter->Valid()) return;
  if (ExtractUserKey(iter->key()).compare(Slice(TableBuilder::kFilterBlockKey)) != 0) {
    return;
  }

  BlockHandle filter_handle;
  Slice v = iter->value();
  if (!filter_handle.DecodeFrom(&v).ok()) return;

  BlockContents filter_contents;
  if (!ReadBlock(rep_->file.get(), opt, filter_handle, &filter_contents).ok()) return;

  if (filter_contents.heap_allocated) {
    rep_->filter_data.reset(filter_contents.data.data());
  }
  rep_->filter =
      std::make_unique<FilterBlockReader>(rep_->options.filter_policy,
                                          filter_contents.data);
}

namespace {

void DeleteCachedBlock(const Slice& /*key*/, void* value) {
  delete reinterpret_cast<Block*>(value);
}

void ReleaseCachedBlock(void* arg, void* handle) {
  auto* cache = reinterpret_cast<Cache*>(arg);
  cache->Release(reinterpret_cast<Cache::Handle*>(handle));
}

}  // namespace

Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  auto* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  if (!s.ok()) return NewErrorIterator(s);

  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  if (block_cache != nullptr) {
    // Cache key is (this table's id, block offset). The table id is what keeps
    // two different files' block 0 from colliding in a shared cache.
    char cache_key_buffer[16];
    EncodeFixed64BE(cache_key_buffer, table->rep_->cache_id);
    EncodeFixed64BE(cache_key_buffer + 8, handle.offset());
    const Slice cache_key(cache_key_buffer, sizeof(cache_key_buffer));

    cache_handle = block_cache->Lookup(cache_key);
    if (cache_handle != nullptr) {
      block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      // Counted separately from blocks_read on purpose. A query that reports
      // 40 blocks_read on a cold cache and 0 on a warm one is telling the
      // truth twice; collapsing both into one number would hide which run was
      // measured and make the figure impossible to compare.
      if (options.stats != nullptr) ++options.stats->block_cache_hits;
    } else {
      BlockContents contents;
      s = ReadBlock(table->rep_->file.get(), options, handle, &contents);
      if (!s.ok()) return NewErrorIterator(s);
      if (options.stats != nullptr) ++options.stats->blocks_read;
      block = new Block(contents);
      if (contents.cachable && options.fill_cache) {
        cache_handle = block_cache->Insert(cache_key, block, block->size(),
                                           &DeleteCachedBlock);
      }
    }
  } else {
    BlockContents contents;
    s = ReadBlock(table->rep_->file.get(), options, handle, &contents);
    if (!s.ok()) return NewErrorIterator(s);
    if (options.stats != nullptr) ++options.stats->blocks_read;
    block = new Block(contents);
  }

  Iterator* iter = block->NewIterator(table->rep_->comparator);
  if (cache_handle != nullptr) {
    // The block is owned by the cache; releasing the handle is what allows it
    // to be evicted later.
    iter->RegisterCleanup(&ReleaseCachedBlock, block_cache, cache_handle);
  } else {
    iter->RegisterCleanup([](void* b, void*) { delete reinterpret_cast<Block*>(b); },
                          block, nullptr);
  }
  return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(rep_->index_block->NewIterator(rep_->comparator),
                             &Table::BlockReader, const_cast<Table*>(this), options,
                             rep_->comparator);
}

Status Table::InternalGet(const ReadOptions& options, const Slice& k, void* arg,
                          void (*handle_result)(void*, const Slice&, const Slice&)) {
  std::unique_ptr<Iterator> index_iter(rep_->index_block->NewIterator(rep_->comparator));
  index_iter->Seek(k);

  if (!index_iter->Valid()) {
    // Past the last index entry: the key is greater than everything in this
    // table. No data block needs to be read at all.
    return index_iter->status();
  }

  // THE BLOOM FILTER CHECK. At this point we know which data block would hold
  // the key if it existed. Asking the filter costs a handful of bit tests
  // against memory; reading the block costs a disk seek. Skipping ~99% of the
  // reads that would have found nothing is the entire point of day 3.
  Slice handle_value = index_iter->value();
  BlockHandle handle;
  if (rep_->filter != nullptr) {
    Slice v = handle_value;
    if (handle.DecodeFrom(&v).ok() &&
        !rep_->filter->KeyMayMatch(handle.offset(), ExtractUserKey(k))) {
      rep_->filter_rejections.fetch_add(1, std::memory_order_relaxed);
      // `options.stats` is NOT atomic and does not need to be: a ReadStats
      // belongs to one request, which is served by one thread. That contract is
      // stated in options.h, and it is what keeps per-query accounting free.
      if (options.stats != nullptr) ++options.stats->bloom_rejections;
      return Status::OK();  // definitely not present; handle_result never runs
    }
  }

  std::unique_ptr<Iterator> block_iter(BlockReader(this, options, handle_value));
  block_iter->Seek(k);
  if (block_iter->Valid()) {
    handle_result(arg, block_iter->key(), block_iter->value());
  }

  Status s = block_iter->status();
  if (s.ok()) s = index_iter->status();
  return s;
}

// --- two-level iterator ----------------------------------------------------

namespace {

class TwoLevelIterator final : public Iterator {
 public:
  using BlockFunction = Iterator* (*)(void*, const ReadOptions&, const Slice&);

  TwoLevelIterator(Iterator* index_iter, BlockFunction block_function, void* arg,
                   const ReadOptions& options, const InternalKeyComparator& cmp)
      : block_function_(block_function),
        arg_(arg),
        options_(options),
        comparator_(cmp),
        index_iter_(index_iter) {}

  ~TwoLevelIterator() override = default;

  void Seek(const Slice& target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_) data_iter_->Seek(target);
    SkipEmptyDataBlocksForward();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_) data_iter_->SeekToFirst();
    SkipEmptyDataBlocksForward();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataBlock();
    if (data_iter_) data_iter_->SeekToLast();
    SkipEmptyDataBlocksBackward();
  }

  void Next() override {
    assert(Valid());
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  void Prev() override {
    assert(Valid());
    data_iter_->Prev();
    SkipEmptyDataBlocksBackward();
  }

  bool Valid() const override { return data_iter_ && data_iter_->Valid(); }

  Slice key() const override {
    assert(Valid());
    return data_iter_->key();
  }
  Slice value() const override {
    assert(Valid());
    return data_iter_->value();
  }

  Status status() const override {
    if (!index_iter_->status().ok()) return index_iter_->status();
    if (data_iter_ && !data_iter_->status().ok()) return data_iter_->status();
    return status_;
  }

 private:
  // Walk forward across block boundaries until a valid entry is found. This is
  // what makes a scan across a hundred blocks look like one flat sequence.
  void SkipEmptyDataBlocksForward() {
    while (!data_iter_ || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_) data_iter_->SeekToFirst();
    }
  }

  void SkipEmptyDataBlocksBackward() {
    while (!data_iter_ || !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        SetDataIterator(nullptr);
        return;
      }
      index_iter_->Prev();
      InitDataBlock();
      if (data_iter_) data_iter_->SeekToLast();
    }
  }

  void SetDataIterator(Iterator* data_iter) {
    if (data_iter_ && !data_iter_->status().ok() && status_.ok()) {
      status_ = data_iter_->status();
    }
    data_iter_.reset(data_iter);
  }

  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      SetDataIterator(nullptr);
      return;
    }
    const Slice handle = index_iter_->value();
    if (data_iter_ && handle.compare(Slice(data_block_handle_)) == 0) {
      return;  // already on this block; do not re-read it
    }
    Iterator* iter = block_function_(arg_, options_, handle);
    data_block_handle_.assign(handle.data(), handle.size());
    SetDataIterator(iter);
  }

  BlockFunction block_function_;
  void* arg_;
  const ReadOptions options_;
  const InternalKeyComparator comparator_;
  Status status_;
  std::unique_ptr<Iterator> index_iter_;
  std::unique_ptr<Iterator> data_iter_;
  std::string data_block_handle_;
};

}  // namespace

Iterator* NewTwoLevelIterator(Iterator* index_iter,
                              Iterator* (*block_function)(void*, const ReadOptions&,
                                                          const Slice&),
                              void* arg, const ReadOptions& options,
                              const InternalKeyComparator& comparator) {
  return new TwoLevelIterator(index_iter, block_function, arg, options, comparator);
}

}  // namespace sextant::lsm
