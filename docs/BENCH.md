# Benchmarks

Reproduce with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/bench/lsm_bench 200000 100      # 200k entries, 100-byte values
```

Re-run after every milestone and keep the history. One number with no context
says nothing; a series says what each piece of the engine cost. The read
regression when SSTables landed, and its recovery when bloom filters did, are
only visible because both were measured.

---

## Day 1 - memtable + WAL, no SSTables

200,000 entries · 100-byte values

| Benchmark | Windows · MSVC · NVMe | Linux container · GCC 11.4 |
|---|---|---|
| `fillseq` (sync=false) | 181,708 ops/sec | **805,633 ops/sec** |
| `fillbatch` (1000/batch) | **1,108,510 ops/sec** | **1,537,945 ops/sec** |
| `fillsync` (sync=true) | **3,003 ops/sec** | 655 ops/sec |
| `readrandom` | 879,015 ops/sec · p99 **2.10 µs** | 1,051,688 ops/sec · p99 2.18 µs |
| `readmissing` | 4,113,517 ops/sec · p99 0.30 µs | 6,718,144 ops/sec · p99 0.14 µs |
| recovery (WAL replay) | 0.281 s / 202,200 records | 0.211 s / 202,200 records |

Two platforms is not padding. The *differences* between them are the most
informative thing on this page.

## What these numbers actually tell you

**Batching is 6.1× on Windows but only 1.9× on Linux.** Same source, same
benchmark. The cause is `Writer::EmitPhysicalRecord`, which calls `Flush()`
after every physical record - pushing the CRT's userspace buffer into the OS.
That call is markedly more expensive in the MSVC runtime than in glibc, so
Windows pays a much higher per-record toll, and batching (one WAL record for
1,000 keys instead of 1,000 records) removes proportionally more of it.

This is the single best observation in the day-1 numbers, because it is not
about the algorithm at all - it is about a library call the algorithm makes.
The general lesson is that *per-operation overhead is a platform property*, and
you cannot reason about it from the source alone. You have to measure on the
machine you care about.

**`fillsync` is 60× slower than `fillseq` on Windows, and 1,230× slower on the
Linux container.** Both ratios are correct and neither is the "real" one. The
Windows figure is a genuine NVMe device round-trip. The container figure is a
round-trip *through an overlay filesystem*, which is far slower - an artefact
of where it ran, not of the engine. Knowing which of your numbers describe your
code and which describe your environment is most of what reading a benchmark
honestly consists of.

Either way, the conclusion for the engine is identical: an `fsync` is a device
round-trip, no amount of CPU optimisation touches it, and that is the entire
argument for group commit. It is also why bulk ingest runs `sync=false` plus one
explicit sync per batch - source data is replayable, so ingest needs a weaker
durability guarantee than an online write does.

**`readmissing` is 4-6× faster than `readrandom`.** Today that is only "the
skiplist search terminates early and no value gets copied". After day 3 this
becomes the *bloom filter* line, and the interesting quantity will be how many
disk reads it avoids. Recorded now so the comparison exists later.

**p99 read is ~2.1 µs against a mean of ~1 µs on both platforms.** That tail is
skiplist level traversal plus cache misses, and its consistency across two very
different machines suggests it is a property of the data structure rather than
the hardware. The `max` values (331 µs and 224 µs) are scheduler preemption,
not the engine.

**Recovery replays 202,200 records in 0.21-0.28 s** - roughly 750k-960k
records/sec, close to the write path's own throughput, which is what you would
expect: replay is the same memtable insert without the log append. This number
will get *better* once SSTables exist, because the WAL will only hold writes
since the last flush. Replay time becomes bounded by `write_buffer_size`
instead of by total data volume.

## Known inefficiency, deliberately not yet fixed

`EmitPhysicalRecord` flushes once per *physical* record. For a record split
across N blocks that is N flushes where one would do. Fixing it means moving the
flush up into `AddRecord`, which is a genuine improvement for large values.

It is left in place for now because the flush is load-bearing for the stated
durability contract - `sync=false` promises "survives a process crash", which
requires the bytes to have reached the OS page cache before `Put` returns.
Removing it entirely would quietly weaken that promise. This is a real trade
rather than an oversight, which is why it is written down rather than silently
patched.

---

## Day 2 - L0 SSTables

200,000 entries · 100-byte values · **4 MB write buffer** (small on purpose, so
the run actually flushes) · Linux container · GCC 11.4

| Benchmark | Day 1 (memory only) | Day 2 (15 L0 tables) | Change |
|---|---|---|---|
| `fillseq` | 673,254 ops/sec | 599,416 ops/sec | −11% |
| `fillbatch` | 1,634,229 ops/sec | 854,953 ops/sec | −48% |
| `fillsync` | 773 ops/sec | 874 ops/sec | ≈ |
| `readrandom` | 935,003 ops/sec | **10,861 ops/sec** | **−86×** |
| `readrandom` p99 | 2.37 µs | **176.99 µs** | **75× worse** |
| `readmissing` | 6,610,034 ops/sec | **8,478 ops/sec** | **−780×** |
| recovery | 0.233 s / 202,200 records | **0.008 s / 2,013 records** | **29× better** |

```
flushes         : 15          bytes flushed : 42.34 MB
live sstables   : 15          sstables probed per read : 13.29
```

### Reads got 86× slower. That is the correct outcome.

Data now lives in fifteen L0 files whose key ranges overlap, so a lookup has to
consult them **newest-first** until it finds the key - and nothing yet prunes
that search. At 13.29 tables probed per read, with each probe costing an index
binary search plus a 4 KB block read, ~90 µs is roughly what the arithmetic
predicts.

`readmissing` is worse still, and for a sharper reason: **a miss cannot
short-circuit.** Proving a key is absent means probing *every single table*.
That is why it fell 780× while hits fell 86×.

This is the textbook L0 read-amplification problem, and it is precisely what the
next two milestones attack:

- **Day 3, bloom filters** - 10 bits per key answers "definitely not here"
  without touching the disk, at a ~0.8% false-positive rate. Most of those 13.29
  probes become a bitmap check. Expect `readmissing` to recover almost entirely.
- **Day 4, leveled compaction** - merges L0 into non-overlapping levels, so at
  most one file per level can contain a given key.

The benchmark is also close to L0 worst case by construction: keys are written
in ascending order, so each table holds a contiguous range, and `readrandom`
probes the *oldest* tables - meaning it walks nearly the whole list. Worth
stating rather than quietly enjoying a flattering average.

### Writes got slower, and batched writes got much slower

`fillseq` lost 11% and `fillbatch` lost 48%. Flushing is synchronous and holds
the write mutex, so every ~4 MB a writer stalls while a 3 MB SSTable is built
and fsynced. Batched writes suffer more because they reach the flush threshold
sooner in wall-clock terms.

Day 4 moves the flush to a background thread. The interesting part is that this
does not make the work disappear - it relocates it, and introduces the write
stall that every LSM eventually has to reason about: if ingest outruns
compaction, L0 grows without bound and the engine must eventually throttle
writers on purpose.

### Recovery got 29× faster, exactly as predicted

Day 1's notes said replay time would improve once SSTables existed, because the
WAL would only hold writes since the last flush. It replays **2,013 records
instead of 202,200** - recovery is now bounded by `write_buffer_size` rather
than by total data volume, which is the property that makes an LSM restartable
at any size.

---

## Day 3 - bloom filters, block cache, key-range pruning

Same 200,000 entries, 100-byte values, 4 MB write buffer, 15 L0 tables.
10 bits/key bloom, 8 MB block cache.

| Benchmark | Day 2 | Day 3 | Change |
|---|---|---|---|
| `readrandom` | 10,861 ops/sec | **146,253 ops/sec** | **13.5x faster** |
| `readrandom` p99 | 176.99 µs | **17.08 µs** | **10x better** |
| `readmissing` | 8,478 ops/sec | **4,604,622 ops/sec** | **543x faster** |
| sstables probed per read | 13.29 | **0.50** | |
| `fillseq` | 599,416 ops/sec | 568,780 ops/sec | -5% |
| `fillbatch` | 854,953 ops/sec | 811,813 ops/sec | -5% |

```
range rejects  : 5,115,884        bloom rejects : 0
cache hit rate : 36.3%            cache bytes   : 7.95 MB
```

### The most interesting number here is `bloom rejects: 0`

The bloom filters did nothing in this benchmark, and understanding why is worth
more than the speedup itself.

Three filters run in order, cheapest first:

1. **key range** - is the key inside this file's `[smallest, largest]`? Two
   comparisons against memory. No I/O.
2. **bloom filter** - does this block definitely not contain the key? Seven bit
   tests. No I/O.
3. **block cache** - is the decoded block already in memory? A hash lookup.

The benchmark writes keys in ascending order, so **each L0 file covers a
disjoint key range**. The very first check therefore eliminates every
non-candidate file, and the bloom filter is never consulted. `readmissing`
probes keys that sort before every file, so it rejects all 15 files without a
single byte of I/O, which is why it is back to within 30% of the day-1
memory-only figure.

That is a real result, not a defect - the cheapest filter was sufficient for
this access pattern. But it means **the benchmark is not evidence the bloom
filter works**, so two tests cover the case it does not:

- `FlushTest.SequentialWritesArePrunedByKeyRange` - asserts under 3 files
  probed per read when ranges are disjoint
- `FlushTest.OverlappingRangesArePrunedByBloomFilter` - writes keys in random
  order so every file spans the whole key space, making range pruning useless,
  and asserts `filter_rejections > 0`

The second is the realistic case for this project. Sextant's keys are entity,
link and provenance records interleaved by ULID across eleven keyspaces - not
written in sorted order. In production the bloom filter will be doing the work
that key ranges do here.

### The cache hit rate is 36%, and that is roughly the ceiling

`readrandom` touches 200,000 keys uniformly across 42 MB of tables with an 8 MB
cache. With a uniform random access pattern and a cache holding under a fifth of
the data, ~20-35% is about what LRU can achieve; there is no locality to exploit.
125,461 evictions against 200,000 reads says the cache is thrashing, which is the
honest reading. A real workload with skew would do far better, and the fix for a
uniform one is a bigger cache, not a smarter policy.

### Writes lost another 5%

Building a bloom filter costs one hash and seven bit-sets per key at flush time.
5% of write throughput for a 13.5x read improvement is not a close call.

---

---

## Day 4 - leveled compaction and background work

Same 200,000 entries, 100-byte values, 4 MB write buffer. A **random-write
phase was added** because sequential writes produce disjoint files that need no
merging at all, which makes them a flattering and misleading benchmark for a
compaction milestone.

| Benchmark | Day 3 | Day 4 | Change |
|---|---|---|---|
| `fillseq` | 568,780 ops/sec | **793,902 ops/sec** | **+40%** |
| `fillbatch` | 811,813 ops/sec | **1,527,101 ops/sec** | **+88%** |
| `fillrandom` | - | 492,386 ops/sec | new |
| `readrandom` | 146,253 ops/sec | 98,910 ops/sec | see below |
| `readmissing` | 4,604,622 ops/sec | 3,723,260 ops/sec | ≈ |

```
compactions   : 3          bytes rewritten : 24.04 MB
keys dropped  : 64,216     files deleted   : 35
write stalls  : 4          write amplification : 1.20x

L0:  3 files   8.51 MB
L1:  4 files   7.05 MB
L2: 17 files  42.90 MB
```

### Writes got 40-88% faster, which is the real headline

Day 3 flushed synchronously while holding the write mutex, so every 4 MB a
writer stopped dead for the duration of building and fsyncing a 3 MB SSTable.
Day 4 moves that to a background thread and writers keep going.

That is not free work disappearing - it is work relocating. The honest
accounting is the **4 write stalls**: when ingest outruns compaction, the
engine deliberately throttles writers (1 ms delay at 8 L0 files, hard block at
12). Backpressure is a feature. Without it, L0 grows without bound, read
amplification climbs back toward day-2 numbers, and eventually the disk fills.

### The readrandom comparison is not apples to apples

Day 3's 146k was measured over a database built by sequential writes only.
Day 4's 98.9k is measured after sequential *and* random writes, which leaves
overlapping L0 files and a genuinely harder lookup. Reporting it as a 32%
regression would be dishonest; the workload changed. What can be said is that
`readmissing` held at 3.7M/sec, so the filters are still doing their job.

### Write amplification is 1.20x, and that number is the point

Every byte the caller wrote resulted in 1.20 bytes reaching disk: 65.6 MB
flushed plus 24.0 MB rewritten by compaction, against 74.6 MB of logical
writes. That is the price of keeping levels sorted, and it is the number to
have ready when someone asks what leveled compaction costs.

For comparison, pure sequential writes give **0.87x** - below 1.0, because
compaction never has to run and obsolete versions are dropped at flush time.

### 64,216 keys dropped

Obsolete versions and tombstones reclaimed during compaction. Without this the
database would grow monotonically with every overwrite, regardless of how much
data is actually live.

---

## Milestone summary

| Milestone | fillseq | readrandom | p99 read | readmissing | recovery |
|---|---|---|---|---|---|
| Day 1 - memtable + WAL | 673k/s | 935k/s | 2.37 µs | 6.6M/s | 0.233 s |
| Day 2 - L0 SSTables | 599k/s | 10.9k/s | 177 µs | 8.5k/s | 0.008 s |
| Day 3 - filters + cache | 569k/s | 146k/s | 17.1 µs | 4.6M/s | 0.008 s |
| Day 4 - leveled compaction | **794k/s** | 99k/s* | 25.6 µs* | 3.7M/s | 0.018 s |

\* measured on a harder workload than day 3 - see above.

The shape of the four days is worth reading as a whole. Day 2 traded an 86x
read regression for durability. Day 3 bought most of it back with filters that
cost 5% of write throughput. Day 4 bounded read amplification structurally and
gave back the write throughput day 2 took, at the cost of 1.20x write
amplification and the need for backpressure. Every step was a trade, and none
of them was free.
