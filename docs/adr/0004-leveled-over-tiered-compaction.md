# ADR 0004 - Leveled compaction rather than size-tiered

**Status:** accepted · **Date:** 2026-08-16

## Context

Having chosen an LSM tree ([ADR 0001](0001-lsm-over-btree.md)), the next
decision is how compaction organises files on disk. The two mainstream answers
are size-tiered (Cassandra's default, and RocksDB's universal compaction) and
leveled (LevelDB, RocksDB's default).

The workload this engine serves is stated in
[ARCHITECTURE.md](../ARCHITECTURE.md): a bulk ingest phase, then a resolve
phase, then a long read phase serving graph traversals and range scans. Reads
dominate wall-clock time in the demo and in any real use; writes arrive in
bursts and are allowed to be slower.

## Decision

Leveled compaction. Level 0 holds overlapping files straight from the memtable;
every level below it holds files with disjoint key ranges, each level roughly
ten times the size of the one above.

## Rationale

The two schemes trade the same three quantities in opposite directions.

|  | Size-tiered | Leveled |
|---|---|---|
| Read amplification | Up to one file per tier, **all of which may contain the key** | At most one file per level below L0 |
| Write amplification | Low: each byte is rewritten once per tier | High: each byte is rewritten roughly once per level, ~10x fan-out |
| Space amplification | Poor: up to 2x during a major compaction, because the largest run is duplicated | Good: obsolete versions are reclaimed at each level |

**Read amplification is the deciding factor here**, for a reason specific to
this project rather than a general preference.

A graph traversal is not one lookup. The quarter query resolves a start set,
then range-scans `TIDX`, then materialises every entity it found - and each of
those materialisations is a point lookup. Twenty-four arrivals means twenty-four
`ENTITY` reads on top of the index scan. Read amplification therefore multiplies
against the fan-out of the traversal, not against a single request.

Under size-tiering, files within a tier have overlapping key ranges, so a point
lookup may have to consult every file in every tier. Bloom filters make each of
those cheap, but "cheap" times "several files per tier" times "twenty-four
lookups" is a real number. Under leveling, each level below L0 is a sorted run
with disjoint ranges, so a lookup consults **at most one file per level** and
the per-file key range check skips most of those without any I/O at all.

The cost is write amplification, measured at **1.20x** on the ingest benchmark
(`docs/BENCH.md`). That number is low for leveled compaction because the dataset
does not yet reach L2 in earnest; at a much larger scale it would climb toward
the ~10x that the fan-out implies. That is an honest limitation of the
measurement, not evidence that leveling is free.

## Alternatives considered

**Size-tiered.** Would make ingest faster and is materially simpler to
implement: pick N files of similar size, merge them, done. It was on the cut
list in `docs/EXECUTION_PLAN.md` as item 3, to be dropped to if the schedule
slipped. It did not slip. Had it been taken, the honest framing would have been
"faster ingest, slower and more variable point lookups, and up to 2x transient
disk use during a major compaction".

**Hybrid: tiered at L0, leveled below.** This is in fact what is implemented,
and it is not really an alternative so much as the standard shape. L0 must
tolerate overlapping files because they arrive directly from memtable flushes
with no opportunity to partition them; everything below is leveled. The
consequence is that L0 is the one place where a lookup may touch several files,
which is exactly why the L0 file count triggers compaction and why exceeding it
stalls writers.

**Leaving compaction out entirely.** Viable for a demo and dishonest for a
storage engine. Without compaction, deleted keys are never reclaimed, tombstones
accumulate forever, and read amplification grows without bound - and the
tombstone-dropping logic is one of the genuinely subtle parts of an LSM, so
skipping it would skip the interesting problem.

## Consequences

- A point lookup consults at most one file per level below L0, which is what
  makes traversal fan-out affordable.
- Writes are amplified. Measured 1.20x at the current scale; expect it to grow
  with data volume.
- Compaction runs on a background thread, so a write burst can outrun it. That
  is what the write-stall mechanism exists for, and `write_stalls` is reported
  in `/api/stats` rather than hidden.
- The tombstone-dropping rule - a deletion may only be discarded once no older
  level can still hold the key - is a real invariant with a real test
  (`test_compaction.cpp`). Getting it wrong resurrects deleted data, which is
  the worst class of storage bug because nothing errors.

## Revisit if

Ingest throughput becomes the binding constraint rather than query latency, or
the working set grows enough that write amplification dominates the disk budget.
At that point the interesting middle ground is RocksDB-style universal
compaction with a bounded space-amplification target, which sits between the two.
