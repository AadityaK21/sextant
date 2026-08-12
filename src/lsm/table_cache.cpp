#include "table_cache.h"

#include "coding.h"
#include "env.h"
#include "filename.h"

namespace sextant::lsm {
namespace {

// One cached entry: the open file and the parsed table that reads from it.
struct TableAndFile {
  std::unique_ptr<RandomAccessFile> file;
  std::unique_ptr<Table> table;
};

void DeleteEntry(const Slice& /*key*/, void* value) {
  delete reinterpret_cast<TableAndFile*>(value);
}

void UnrefEntry(void* arg1, void* arg2) {
  auto* cache = reinterpret_cast<Cache*>(arg1);
  auto* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

}  // namespace

TableCache::TableCache(std::string dbname, const Options& options, int max_open_files)
    : dbname_(std::move(dbname)),
      options_(options),
      // Charge 1 per entry, so capacity is a file count rather than a byte
      // budget. An open table's memory is dominated by its index and filter,
      // which are roughly proportional to file size, so a count is the honest
      // unit here.
      cache_(Cache::NewLRUCache(static_cast<size_t>(max_open_files))) {}

TableCache::~TableCache() = default;

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  char buf[8];
  EncodeFixed64BE(buf, file_number);
  const Slice key(buf, sizeof(buf));

  *handle = cache_->Lookup(key);
  if (*handle != nullptr) return Status::OK();

  const std::string fname = TableFileName(dbname_, file_number);

  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(fname, &file);
  if (!s.ok()) return s;

  std::unique_ptr<Table> table;
  s = Table::Open(options_, std::move(file), file_size, &table);
  if (!s.ok()) return s;

  auto* tf = new TableAndFile;
  tf->table = std::move(table);
  *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
  return Status::OK();
}

Iterator* TableCache::NewIterator(const ReadOptions& options, uint64_t file_number,
                                  uint64_t file_size, Table** tableptr) {
  if (tableptr != nullptr) *tableptr = nullptr;

  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) return NewErrorIterator(s);

  auto* tf = reinterpret_cast<TableAndFile*>(cache_->Value(handle));
  Iterator* result = tf->table->NewIterator(options);
  // The iterator reads through the table, so the cache entry must stay pinned
  // for as long as the iterator lives.
  result->RegisterCleanup(&UnrefEntry, cache_.get(), handle);
  if (tableptr != nullptr) *tableptr = tf->table.get();
  return result;
}

Status TableCache::Get(const ReadOptions& options, uint64_t file_number,
                       uint64_t file_size, const Slice& k, void* arg,
                       void (*handle_result)(void*, const Slice&, const Slice&)) {
  Cache::Handle* handle = nullptr;
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) return s;

  auto* tf = reinterpret_cast<TableAndFile*>(cache_->Value(handle));
  const uint64_t before = tf->table->FilterRejections();
  s = tf->table->InternalGet(options, k, arg, handle_result);
  filter_rejections_ += tf->table->FilterRejections() - before;

  cache_->Release(handle);
  return s;
}

void TableCache::Evict(uint64_t file_number) {
  char buf[8];
  EncodeFixed64BE(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace sextant::lsm
