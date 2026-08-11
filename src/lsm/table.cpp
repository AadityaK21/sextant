#include "table.h"

#include <cassert>

#include "coding.h"

namespace sextant::lsm {

struct Table::Rep {
  Options options;
  Status status;
  std::unique_ptr<RandomAccessFile> file;
  BlockHandle metaindex_handle;
  std::unique_ptr<Block> index_block;
  InternalKeyComparator comparator{};
};

Table::~Table() { delete rep_; }

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
  // 4 KB data block, so it is small enough to stay resident.
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
  table->reset(new Table(rep));
  return Status::OK();
}

Iterator* Table::BlockReader(void* arg, const ReadOptions& options,
                             const Slice& index_value) {
  auto* table = reinterpret_cast<Table*>(arg);

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  if (!s.ok()) return NewErrorIterator(s);

  BlockContents contents;
  s = ReadBlock(table->rep_->file.get(), options, handle, &contents);
  if (!s.ok()) return NewErrorIterator(s);

  auto* block = new Block(contents);
  Iterator* iter = block->NewIterator(table->rep_->comparator);
  // The iterator points into the block, so the block must outlive it. This is
  // exactly what Iterator::RegisterCleanup exists for.
  iter->RegisterCleanup([](void* b, void*) { delete reinterpret_cast<Block*>(b); },
                        block, nullptr);
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

  std::unique_ptr<Iterator> block_iter(
      BlockReader(this, options, index_iter->value()));
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
