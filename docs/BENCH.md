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

## Milestone summary

| Milestone | fillseq | readrandom | p99 read | recovery | notes |
|---|---|---|---|---|---|
| Day 1 - memtable + WAL | 673k/s | 935k/s | 2.37 µs | 0.233 s | all in memory |
| Day 2 - L0 SSTables | 599k/s | 10.9k/s | 177 µs | 0.008 s | 13.3 tables probed/read |
| Day 3 - bloom + block cache | | | | | target: `readmissing` recovers |
| Day 4 - leveled compaction | | | | | target: ≤1 file per level |
