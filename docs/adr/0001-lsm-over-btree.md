# ADR 0001 - LSM tree rather than a B-tree

**Status:** accepted · **Date:** 2026-08-09

## Context

The engine has to absorb bulk ingest - millions of AIS broadcast rows, port
records and port calls - and then serve point lookups and, crucially, ordered
range scans for graph traversal.

## Decision

A log-structured merge tree.

## Rationale

The trade is best framed as the **RUM conjecture**: optimise for **R**ead
amplification, **U**pdate amplification or **M**emory/space amplification -
pick two, the third suffers.

|  | B-tree | LSM tree |
|---|---|---|
| Writes | In-place update → random I/O; a 100-byte row costs a 4-16 KB page write | Sequential appends, batched |
| Reads | One tree descent | Check memtable + every level; mitigated by bloom filters |
| Space | Fragmentation, ~50-70% typical fill factor | Obsolete versions linger until compaction |
| Write amp | Low per operation, but random | High - data is rewritten at each level |

Our workload is write-heavy at ingest and scan-heavy at query. That is the LSM
sweet spot. A B-tree would win for a read-mostly workload with in-place updates.

Two secondary reasons that matter specifically here:

1. **Immutable files suit lineage.** Provenance that points at bytes which were
   later overwritten is not provenance. An append-only design makes the
   immutability guarantee structural rather than a convention.
2. **Sequence numbers give time travel for free.** A snapshot is an integer, so
   "what did this entity look like before Tuesday's ingest?" falls out of the
   storage design instead of being built on top of it.

## Consequences

- Write amplification is real: leveled compaction rewrites data roughly
  `10 × num_levels` times in the worst case. Acceptable at our volume; at
  petabyte scale you would tune the level ratio or move to tiered.
- Reads must consult several levels. Bloom filters (day 3) reduce this to
  approximately one disk read per lookup.
- Compaction is a background cost that has to be scheduled and rate-limited, or
  L0 accumulates and the engine stalls writes.

## Alternatives rejected

- **B-tree / LMDB-style.** Better point reads, worse ingest, and in-place
  mutation is at odds with the lineage guarantee.
- **Embed RocksDB.** Would be the correct engineering choice for a product and
  the wrong one for this project - the storage engine is a deliverable, not a
  dependency.
