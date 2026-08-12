#include "ingest.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "env.h"
#include "hash.h"
#include "keyspace.h"

namespace sextant::connectors {
namespace {

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Ingestor::Ingestor(codec::Store* store, const Ontology* ontology,
                   const TransformRegistry* transforms)
    : store_(store), ontology_(ontology), transforms_(transforms) {}

uint64_t Ingestor::FingerprintBytes(const std::string& bytes) {
  return codec::Hash64(lsm::Slice(bytes));
}

Status Ingestor::FingerprintFile(const std::string& path, uint64_t* out) {
  std::unique_ptr<lsm::SequentialFile> file;
  Status s = lsm::SequentialFile::Open(path, &file);
  if (!s.ok()) return s;

  // Streamed rather than slurped: the whole point of hashing the input is to
  // avoid loading it, and an AIS extract is larger than memory.
  std::string chunk;
  chunk.resize(256 * 1024);
  std::string all;
  uint64_t total = 0;
  // Hash64 has no streaming form, so the file is hashed in one pass over an
  // accumulated buffer only when it is small; larger inputs are folded chunk by
  // chunk into a running value. The fold is order-dependent, which is what
  // matters - it must give the same answer for the same bytes.
  uint64_t running = 0;
  while (true) {
    lsm::Slice piece;
    s = file->Read(chunk.size(), &piece, chunk.data());
    if (!s.ok()) return s;
    if (piece.empty()) break;
    total += piece.size();
    const uint64_t part = codec::Hash64(piece);
    running = running * 0x100000001b3ULL ^ part;
  }
  // Mix the length in so that a truncated file cannot collide with the whole
  // one by accident.
  *out = running ^ (total * 0x9E3779B97F4A7C15ULL);
  return Status::OK();
}

Status Ingestor::FindBatchWithFingerprint(codec::SourceId source,
                                          uint64_t fingerprint,
                                          codec::BatchId* out) {
  *out = 0;
  if (fingerprint == 0) return Status::OK();
  auto it = store_->ScanIngest(source);
  for (; it->Valid(); it->Next()) {
    BatchManifest m;
    lsm::Slice value = it->value();
    if (!BatchManifest::DecodeFrom(&value, &m)) continue;
    if (m.content_fingerprint == fingerprint) {
      *out = m.batch_id;
      return Status::OK();
    }
  }
  return it->status();
}

Status Ingestor::LatestBatch(codec::SourceId source, codec::BatchId* out) {
  *out = 0;
  auto it = store_->ScanIngest(source);
  for (; it->Valid(); it->Next()) {
    codec::SourceId s;
    codec::BatchId b;
    if (codec::DecodeIngestKey(it->key(), &s, &b) && b > *out) *out = b;
  }
  return it->status();
}

Status Ingestor::ListBatches(codec::SourceId source,
                             std::vector<BatchManifest>* out) {
  out->clear();
  auto it = store_->ScanIngest(source);
  for (; it->Valid(); it->Next()) {
    BatchManifest m;
    lsm::Slice value = it->value();
    if (BatchManifest::DecodeFrom(&value, &m)) out->push_back(std::move(m));
  }
  return it->status();
}

Status Ingestor::Run(const SourceSpec& spec, RowSource* rows,
                     uint64_t content_fingerprint, const Options& options,
                     Result* result) {
  *result = Result{};

  if (!options.force) {
    codec::BatchId existing = 0;
    Status s = FindBatchWithFingerprint(spec.id, content_fingerprint, &existing);
    if (!s.ok()) return s;
    if (existing != 0) {
      result->skipped = true;
      result->existing_batch = existing;
      return Status::OK();
    }
  }

  codec::BatchId latest = 0;
  Status s = LatestBatch(spec.id, &latest);
  if (!s.ok()) return s;
  const codec::BatchId batch = latest + 1;

  BatchManifest manifest;
  manifest.source_id = spec.id;
  manifest.batch_id = batch;
  manifest.source_key = spec.key;
  manifest.uri = options.uri_label;
  if (manifest.uri.empty()) {
    manifest.uri = spec.uri.empty()
                       ? (spec.base_url.empty() ? spec.dsn : spec.base_url)
                       : spec.uri;
  }
  manifest.content_fingerprint = content_fingerprint;
  manifest.started_ms = NowMs();

  Mapper mapper(ontology_, transforms_, &spec);
  const std::string endpoint = rows->endpoint();

  std::vector<ontology::SourceRecord> records;
  std::string encoded;

  while (rows->Next()) {
    if (options.limit != 0 && manifest.rows_read >= options.limit) break;
    ++manifest.rows_read;

    const Row& row = rows->current();
    if (!mapper.Accepts(row)) {
      ++manifest.rows_filtered;
      continue;
    }

    records.clear();
    s = mapper.MapRow(row, batch, rows->row_seq(), endpoint, &records);
    if (!s.ok()) return s;
    if (records.empty()) {
      // The row survived the filter but produced no identity for any mapping.
      // Counted as filtered rather than ignored, so the numbers add up.
      ++manifest.rows_filtered;
      continue;
    }

    // RAW and every SRCREC this row produced go in as one atomic unit. A crash
    // between them would leave provenance pointing at bytes that are not there.
    codec::RowWriter writer =
        store_->NewRow(spec.id, batch, rows->row_seq());
    writer.SetRaw(lsm::Slice(row.Raw()));
    for (const auto& rec : records) {
      encoded.clear();
      rec.EncodeTo(&encoded);
      writer.AddSourceRecord(rec.natural_key_hash, lsm::Slice(encoded));
      ++manifest.records_written;
      for (const auto& p : rec.properties) {
        if (p.rejected()) {
          ++manifest.properties_rejected;
        } else if (!p.value.IsNull()) {
          ++manifest.properties_written;
        }
      }
      // AddSourceRecord copies into the batch, so `encoded` may be reused.
    }
    s = writer.Commit();
    if (!s.ok()) return s;
  }

  // A source that stopped early because of an I/O error must not be recorded as
  // a completed batch, or the next run will skip it as already loaded.
  s = rows->status();
  if (!s.ok()) return s;

  manifest.finished_ms = NowMs();
  encoded.clear();
  manifest.EncodeTo(&encoded);
  s = store_->PutIngestManifest(spec.id, batch, lsm::Slice(encoded));
  if (!s.ok()) return s;

  result->batch = batch;
  result->manifest = std::move(manifest);
  return Status::OK();
}

// --- stats ------------------------------------------------------------------

Status CollectSourceStats(codec::Store* store, const SourceSpec& spec,
                          SourceStats* out) {
  *out = SourceStats{};
  out->source_id = spec.id;
  out->key = spec.key;

  auto ingest = store->ScanIngest(spec.id);
  for (; ingest->Valid(); ingest->Next()) {
    ++out->batches;
    codec::SourceId s;
    codec::BatchId b;
    if (codec::DecodeIngestKey(ingest->key(), &s, &b) && b > out->latest_batch) {
      out->latest_batch = b;
    }
  }
  Status s = ingest->status();
  if (!s.ok()) return s;

  auto srcrec = store->ScanSourceRecords(spec.id);
  for (; srcrec->Valid(); srcrec->Next()) ++out->source_records;
  s = srcrec->status();
  if (!s.ok()) return s;

  // RAW is scanned per batch rather than per source, because the RAW prefix for
  // a source spans every batch it ever had and the per-batch number is the one
  // that tells you whether the last run did anything.
  for (codec::BatchId b = 1; b <= out->latest_batch; ++b) {
    auto raw = store->ScanRawBatch(spec.id, b);
    for (; raw->Valid(); raw->Next()) ++out->raw_records;
    s = raw->status();
    if (!s.ok()) return s;
  }
  return Status::OK();
}

}  // namespace sextant::connectors
