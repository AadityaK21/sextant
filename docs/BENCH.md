# Benchmarks

Reproduce with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/bench/lsm_bench 200000 100      # 200k entries, 100-byte values
```

Re-run after every milestone and keep the history. *"Writes got 3× slower when
I added leveled compaction, and here is why"* is a far better interview answer
than one number with no context.

---

## Day 1 — memtable + WAL, no SSTables

200,000 entries · 100-byte values · GCC 11.4 · `-O2` · Linux container

| Benchmark | Throughput | Notes |
|---|---|---|
| `fillseq` (sync=false) | **805,633 ops/sec** · 89 MB/s | one `Put` per call |
| `fillbatch` (1000/batch) | **1,537,945 ops/sec** · 170 MB/s | ~1.9× faster than unbatched |
| `fillsync` (sync=true) | **655 ops/sec** · 0.07 MB/s | one `fsync` per write |
| `readrandom` | **1,051,688 ops/sec** | mean 0.85 µs · p50 0.75 · p99 **2.18 µs** |
| `readmissing` | **6,718,144 ops/sec** | mean 0.13 µs — no key comparison on miss |
| recovery (WAL replay) | **0.211 s** | 202,200 records, 402,000 sequence numbers |

## What these numbers actually tell you

**`fillsync` is 1,230× slower than `fillseq`.** That single ratio is the whole
argument for group commit, and it is worth being able to state precisely: an
`fsync` is a device round-trip, and no amount of CPU optimisation touches it.
It is also why bulk ingest runs with `sync=false` plus one explicit sync per
batch — the source data is replayable, so the durability guarantee you need
during ingest is weaker than the one you need for an online write.

**Batching is only ~1.9× faster than unbatched at `sync=false`.** Modest,
because with no `fsync` in the loop the cost is dominated by memtable insertion
rather than log framing. The gap widens enormously with `sync=true`, where one
batch costs one fsync instead of N.

**`readmissing` is 6× faster than `readrandom`.** Today that is just "the
skiplist search terminates early and no value is copied". After day 3 this line
becomes the *bloom filter* line, and the interesting number will be how many
disk reads it avoids. Record it now so the comparison exists.

**p99 read is 2.18 µs against a mean of 0.85 µs.** That tail is skiplist level
traversal plus cache misses. The `max` of 224 µs is a scheduler artefact, not
the engine — worth saying, because knowing which outliers are yours and which
are the OS is part of reading a benchmark honestly.

**Recovery replays 202,200 records in 0.21 s** — about 960k records/sec, close
to the write path's own throughput, which is what you would expect since replay
is the same memtable insert without the log append. This number will get *worse*
in a good way once SSTables exist: the WAL only needs to hold writes since the
last flush, so replay time becomes bounded by `write_buffer_size` rather than by
total data volume.

---

## Later milestones

Fill these in as they land. The comparison is the point.

| Milestone | fillseq | readrandom p99 | space amp | notes |
|---|---|---|---|---|
| Day 1 — memtable + WAL | 805k/s | 2.18 µs | — | all in memory |
| Day 2 — SSTable flush | | | | |
| Day 3 — bloom + block cache | | | | expect `readmissing` to matter |
| Day 4 — leveled compaction | | | | expect fillseq to drop; explain why |
