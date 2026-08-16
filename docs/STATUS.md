# Status

What is built, what each piece produced, and where the implementation departed
from the design in [`ARCHITECTURE.md`](ARCHITECTURE.md).

The design document describes the system as intended. This one records what
actually happened, including the places the two differ - those are usually the
interesting part.

---

## Definition of done

| | |
|---|---|
| ✅ | **`make demo` works from a clean checkout.** Build, ingest, resolve, verify, build the UI, serve. |
| ⚠️ | **`docker compose up`** is written and **not verified**. See the note below. |
| ✅ | **Three connector types ingest real data.** CSV (World Port Index, UN/LOCODE), REST/JSON (Digitraffic), Postgres (optional, compiled only when libpq is present). |
| ✅ | **The ontology is declarative.** Strip the comments from `web/src` and no entity type, property or link name appears anywhere in the frontend. |
| ✅ | **ER F1 0.9912** on a held-out split the weights were never fitted to (266 port pairs); 1.0000 on vessels. |
| ✅ | **Blocking RR 0.999 / PC 0.997**, with per-key attribution - including the key measured at zero unique recall. |
| ✅ | **Lineage round trip passes for 100% of 4,201 properties**, with a negative control that corrupts a record and asserts the check goes red. |
| ✅ | **Crash recovery 50/50.** The WAL is truncated at 50 different offsets; survivors form a prefix of the write order at every one. |
| ✅ | **Differential test at 10⁶ operations** against `std::map`, spanning hundreds of flushes and repeated compactions. |
| ✅ | **Benchmarks reproducible by command** - `./build/bench/lsm_bench 200000 100`, with hardware and configuration stated next to every number. |
| ✅ | **Frontend** navigates links, opens lineage, shows rejected alternatives, and renders the query plan and its cost. |
| ⚠️ | **Demo** is a generated terminal transcript, not a UI recording. See the note below. |
| ✅ | **Seven ADRs**, covering the decisions worth arguing about. |
| ✅ | **CI green** - 415 tests on Linux, Windows and macOS, plus a clean run under ASan and UBSan. |

### The two marked ⚠️

**`docker compose up` is unverified.** The `Dockerfile` and the compose service
exist, and the steps inside them are the same ones `make demo` runs on every
push. But no Docker daemon was available where this was built, so nobody has
watched it come up. The risk is in the packaging rather than in the steps.

**The demo shows the CLI, not the UI.** `docs/demo.svg` is generated from real
command output by `scripts/record_demo.py`, which makes it better than a
screen-recorded GIF in the way that matters - it is regenerated rather than
remembered, so it cannot drift from the code. But the lineage drawer, the link
graph and the review queue are not in it, and reconstructing a browser session
frame by frame would be a drawing of the frontend rather than a recording of one.

---

## What each milestone produced

### The storage engine

| | |
|---|---|
| Memtable, WAL, recovery, snapshots | 67 tests |
| SSTable build and read, flush to L0 | 98 tests |
| Bloom filters, block cache, merging iterator | 132 tests |
| VersionSet, MANIFEST, leveled compaction | 145 tests |

Written from scratch: no LevelDB, no RocksDB, no embedded database of any kind.

The read path is the part worth measuring. Adding SSTables made reads slower;
bloom filters and per-file key ranges made them fast again. Both directions are
in [`BENCH.md`](BENCH.md), because a benchmark with no regression in it is a
benchmark that was run once.

**Result:** 1.53M batched writes/sec, 494k random writes/sec, 3.7M misses/sec,
write amplification 1.20x. Also 655 writes/sec with `sync=true` - the fsync
floor, quoted so the batched number is not mistaken for a durability figure.

### The keyspace codec

Twelve keyspaces, ULIDs, order-preserving encodings. This is where the engine
stops being a key-value store and becomes an ontology engine: entities, links,
provenance, indexes and raw records are all bytes in one ordered keyspace, laid
out so that the access pattern each one needs is a sequential range scan.

Byte order is proven equal to logical order for strings, signed integers and
doubles. The quarter query is verified to touch exactly the matching keys.

### The ontology and the connectors

21 transforms, each asserted pure - same chain, same input, same answer, every
time - with numeric ids pinned by test, because those ids are written into
provenance records permanently.

The schema loader rejects 14 classes of mistake at startup. Adding an entity
type is a YAML edit.

Three connectors behind one `RowSource` interface, all streaming. Re-ingesting
an unchanged input is a verified no-op; a changed one never destroys the old
batch.

### Entity resolution

Blocking reduces 1.05M possible pairs to a few hundred candidates at RR 0.999,
losing one true pair in a thousand. Per-key attribution found one of the five
keys contributing zero unique recall, which is written up rather than quietly
removed.

Scoring produces a feature vector, not a number, because the lineage panel has
to answer *why* two records merged. Vetoes are rules rather than negative
weights: two hulls with different IMO numbers are different hulls, and no amount
of other agreement changes that.

Both clusterings are computed and measured on every run:

| | Precision | Recall | F1 |
|---|---|---|---|
| Plain transitive | 0.9726 | 1.0000 | 0.9861 |
| Veto-constrained | **1.0000** | 1.0000 | **1.0000** |

10 merges refused. See [ADR 0005](adr/0005-veto-constrained-clustering.md).

### Lineage

The headline result:

```
lineage round trip
  1187 entities, 4201 properties
  4201 verified, 0 failed  ->  100.00%
```

**Departures from the design.** Three, all deliberate:

1. Union-fused properties are checked by containment rather than equality, since
   no single raw cell can reproduce a merged list. That rule lives in one place
   after having briefly lived in two - see [`BUGS.md`](BUGS.md).
2. `Explanation` carries `provenance_found` and `raw_row_found` separately, so a
   `NotFound` distinguishes "no provenance was written" from "the provenance
   points at a row that is not there". Those are different bugs.
3. `LineageReader` takes a `data_root`, because resolving a source's relative
   URI against the process working directory silently halved the round-trip rate
   when it was run from anywhere but the repository root.

### The query engine

A planner that names its access path and its reason on every step, an executor
that expands the frontier breadth-first, and cost accounting measured inside the
storage engine rather than estimated above it.

```
"_stats": { "index_used": "TIDX", "keys_scanned": 24,
            "entities_materialised": 24, "elapsed_us": 230 }
```

**Departures from the design.** Four:

1. **Cost is a per-request sink, not a diff of global counters.**
   `lsm::ReadOptions` carries a `ReadStats*` that the engine increments as it
   reads. The first version diffed `DB::GetStats()` before and after, which is
   correct on one thread and wrong the moment a second request or a background
   compaction moves the same counters.
2. **`TIDX` is chosen by direction, not just by declaration.** The index is
   anchored on the target of the timed link, so it only serves the reverse hop.
3. **A link resolves by either of its names** - `arrives_at` or its declared
   inverse `arrivals` - and naming the inverse *is* the direction. The design
   document's own example used the inverse name, which nothing could resolve.
4. **`POST /api/review/{id}` records a decision, it does not apply it.** The
   design said "persists as a decision edge", but a decision is about a pair of
   source records, which are not entities and have no id for an edge to point
   at. It is written onto the candidate record, and the merge happens on the
   next `sextant resolve` - re-clustering inside a request handler would mean
   running a whole-graph operation on one HTTP thread.

### The frontend

Vite, React, TypeScript. No entity type or property name appears in the code:

```bash
for f in $(find web/src -name "*.ts*"); do
  sed 's,//.*,,' "$f" | grep -nE "'(Port|Vessel|Voyage)'|locode|arrives_at"
done   # returns nothing
```

**Departures from the design.** Two:

1. **No `react-force-graph`.** It pulls in three.js and d3-force for one view.
   The link explorer is about 120 lines of velocity-Verlet simulation in plain
   SVG: repulsion, springs, centring, and a decaying alpha with a stopping rule.
2. **`web/scripts/check-api-contract.mjs`**, which the design did not call for.
   `tsc` proves the app uses its own types consistently; it cannot prove those
   types describe what the C++ sends, because the two are joined by an assertion.
   The script walks a live server and checks 162 fields. It found the bug where
   running `sextant resolve` twice doubled the entire graph.

---

## Deliberate non-goals

Each of these is a real capability the system does not have.

- **No distribution.** Single node.
- **No ML-based entity resolution.** A hand-tuned Fellegi-Sunter-style scorer is
  interpretable and tunable on 500 labels; a learned model needs 10⁵ and gives
  worse lineage.
- **No cross-entity transactions.** `WriteBatch` gives per-merge atomicity.
- **No incremental re-resolution.** Re-ingest triggers a full re-resolve, which
  clears the derived keyspaces and rebuilds them.
- **No authentication, rate limiting or TLS** on the API. A real deployment puts
  it behind something that does all three.

---

## Where this would go next

In rough order of value:

1. **Verify the container on a clean machine**, which is the one claim in the
   repository resting on reasoning rather than observation.
2. **Capture the lineage drawer**, the part of the UI the CLI demo cannot show.
3. **Fit the scorer's `m` and `u` from the labeled set with EM**, which would
   make the Fellegi-Sunter correspondence exact rather than approximate.
4. **Incremental re-resolution.** Currently a re-ingest means recomputing every
   entity; only the affected blocks need to change.
5. **Barnes-Hut for the link graph**, if it ever needs to draw thousands of
   nodes rather than dozens.
