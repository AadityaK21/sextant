// Ingestion: rows in, RAW and SRCREC out.
//
// WHY BATCHES EXIST AT ALL
//
// RAW is append-only, which is what makes lineage permanent - a provenance
// record written today still resolves to real bytes in a year, because nothing
// ever overwrites them. The cost of that guarantee is that the archive has no
// opinion about duplication: run the same ingest twice and you get two copies
// of every row, with no way to tell afterwards which was which.
//
// A batch is the unit that fixes this. Every ingest gets a monotonically
// increasing BatchId, every RAW key carries it, and a manifest in the INGEST
// keyspace records what the batch was - including a fingerprint of the input
// bytes. That gives three properties worth having:
//
//   IDEMPOTENT      re-ingesting an unchanged file is detected and skipped
//   NON-DESTRUCTIVE a changed file gets a new batch and the old one survives
//   AUDITABLE       `sextant stats` can say where every record came from
//
// SRCREC BEHAVES THE OPPOSITE WAY, DELIBERATELY. Its key is the natural key
// hash with no batch in it, so a new batch replaces the normalised view of each
// row. RAW answers "what did the source say on the 3rd of April"; SRCREC
// answers "what does the source say now". Entity resolution wants the second,
// lineage wants the first, and conflating them would cost one of the two.
//
// FAILURE LEAVES NO MANIFEST. The manifest is written last. If the process dies
// mid-ingest, the RAW rows that made it are still there and still readable, but
// no manifest claims the batch completed - so the next run does not treat it as
// already loaded, and the retry gets a fresh batch id rather than interleaving
// with the corpse of the previous one.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mapping.h"
#include "record.h"
#include "schema.h"
#include "sextant/lsm/status.h"
#include "source.h"
#include "store.h"
#include "transform.h"

namespace sextant::connectors {

using ontology::BatchManifest;
using ontology::Mapper;
using ontology::Ontology;
using ontology::SourceSpec;
using ontology::TransformRegistry;

class Ingestor {
 public:
  struct Options {
    // Ingest even when a batch with the same content fingerprint exists.
    bool force = false;
    // Stop after this many rows. Zero means no limit. Used by the demo and by
    // anyone who wants to see the shape of a source without loading all of it.
    uint64_t limit = 0;
    // What to record as the batch's origin. Defaults to the source's uri; an
    // HTTP source overrides it per endpoint, because one source with five
    // endpoints produces five batches and "which file was this" needs to
    // distinguish them.
    std::string uri_label;
  };

  struct Result {
    codec::BatchId batch = 0;
    // True when an identical batch was already present and nothing was written.
    bool skipped = false;
    codec::BatchId existing_batch = 0;
    BatchManifest manifest;
  };

  Ingestor(codec::Store* store, const Ontology* ontology,
           const TransformRegistry* transforms);

  // `content_fingerprint` is a hash of the input bytes, or zero when the input
  // cannot be hashed before reading it - a Postgres query, for instance. Zero
  // disables the idempotency check for that run, which is honest: without a
  // fingerprint there is nothing to compare.
  Status Run(const SourceSpec& spec, RowSource* rows,
             uint64_t content_fingerprint, const Options& options,
             Result* result);

  // Highest batch id recorded for a source, or zero if it has never been
  // ingested.
  Status LatestBatch(codec::SourceId source, codec::BatchId* out);
  Status ListBatches(codec::SourceId source, std::vector<BatchManifest>* out);

  // Hash a file's bytes, for the idempotency check.
  static Status FingerprintFile(const std::string& path, uint64_t* out);
  static uint64_t FingerprintBytes(const std::string& bytes);

 private:
  Status FindBatchWithFingerprint(codec::SourceId source, uint64_t fingerprint,
                                  codec::BatchId* out);

  codec::Store* store_;
  const Ontology* ontology_;
  const TransformRegistry* transforms_;
};

// --- counting, for `sextant stats` -----------------------------------------

struct SourceStats {
  codec::SourceId source_id = 0;
  std::string key;
  uint64_t batches = 0;
  uint64_t raw_records = 0;
  uint64_t source_records = 0;
  codec::BatchId latest_batch = 0;
};

Status CollectSourceStats(codec::Store* store, const SourceSpec& spec,
                          SourceStats* out);

}  // namespace sextant::connectors
