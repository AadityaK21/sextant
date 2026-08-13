# Sextant

**A mini-Foundry: ontology, entity resolution and cell-level lineage over a
from-scratch LSM storage engine.**

> Sextant ingests maritime data from three heterogeneous sources, maps it onto a
> declarative ontology, resolves duplicate real-world entities across sources,
> and records - for every single property value - the exact source row and
> transform chain that produced it.

*A sextant fixes your position by combining several independent observations.
That is literally entity resolution.*

C++20 · React · no storage dependencies

---

## Status

| Milestone | | |
|---|---|---|
| **Day 1** - memtable, WAL, recovery, snapshots | ✅ | 67 tests green |
| **Day 2** - SSTable build/read, flush to L0 | ✅ | 98 tests green |
| **Day 3** - bloom filters, block cache, merging iterator | ✅ | 132 tests green |
| **Day 4** - VersionSet/MANIFEST, leveled compaction | ✅ | 145 tests green |
| **Day 5** - keyspace codec, ULID, ordered encodings | ✅ | 209 tests green |
| **Days 6-7** - ontology, transforms, three connectors | ✅ | 314 tests green |
| Days 8-10 - entity resolution | ⬜ | |
| Days 11-12 - lineage, query engine, HTTP API | ⬜ | |
| Days 13-14 - React frontend | ⬜ | |

Plan: [`docs/EXECUTION_PLAN.md`](docs/EXECUTION_PLAN.md).

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The same three lines work in PowerShell. Do not join them with `&&` there -
Windows PowerShell 5 does not have it.

Dependencies are fetched and version-pinned at configure time - nothing to
install. Requires CMake ≥ 3.20 and a C++20 compiler. Postgres is optional: if
`libpq` is absent the SQL connector is stubbed and everything else still builds.

```bash
./build/bench/lsm_bench 200000 100      # benchmarks
```

## Run it

Small excerpts of every source are committed, so this works on a fresh clone
with no network and no downloads.

```bash
./build/src/cli/sextant schema                      # the ontology, validated
./build/src/cli/sextant ingest --source wpi         # NGA World Port Index (CSV)
./build/src/cli/sextant ingest --source unlocode    # UN/LOCODE (CSV)
./build/src/cli/sextant ingest --source digitraffic # Digitraffic (REST/JSON)
./build/src/cli/sextant stats
```

On Windows, MSVC is a multi-config generator, so the binary lands one directory
deeper. Run these from the repository root - the schema and data paths are
relative to it:

```powershell
$sx = (Get-ChildItem -Recurse build -Filter sextant.exe)[0].FullName
& $sx schema
& $sx ingest --source wpi
& $sx stats
```

```
ingest unlocode (csv)
  data/snapshots/unlocode/code-list.csv batch 1  17 rows -> 15 records,
  90 values (0 rejected, 2 filtered) in 0 ms

source            batches        raw    records   latest
digitraffic             3         27         27        3
unlocode                1         15         15        1
wpi                     1         12         12        1
```

Run the same ingest twice and the second one does nothing - the input
fingerprint has not changed. Then follow a value back to the bytes it came from:

```bash
./build/src/cli/sextant lineage --source unlocode --batch 1 --row 9
# RAW unlocode batch 1 row 9
# ,SE,GOT,Göteborg,Goteborg,14,AI,1234----,0401,GOT,5742N 01156E,
```

`./data/fetch.sh all` replaces the excerpts with the full datasets.

---

## The idea that holds it together

Most projects in this shape become seven disconnected demos. Sextant is one
system because of a single decision:

> **The LSM engine provides an ordered key-value store with prefix scan.
> Every layer above it is a key encoding.**

Entities, links, provenance, secondary indexes, blocking indexes and raw records
are all bytes in one keyspace, laid out so the access pattern each one needs is
a *sequential range scan*.

Graph traversal is not a subsystem - it is a prefix scan over
`LINKOUT ‖ entity ‖ link_type`. "All voyages through this port last quarter" is
not a filter - it is a range scan over a big-endian timestamp suffix
(see [ADR 0002](docs/adr/0002-big-endian-key-encoding.md)).

```
   web/            React - browse entities, links, lineage      NOT BUILT (days 13-14)
   src/api/        HTTP + JSON                                  NOT BUILT (day 12)
   src/query/      planner · index selection · traversal        NOT BUILT (day 12)
   src/lineage/    provenance at every fusion decision          NOT BUILT (day 11)
   src/resolve/    normalize → block → score → cluster → fuse   NOT BUILT (days 8-10)
  ─────────────────────────────────────────────────────────────────────────────────
   src/cli/        ingest · stats · lineage · schema            built
   src/connectors/ CSV · REST/JSON · Postgres                   built
   src/ontology/   declarative types, links and transforms      built
   src/codec/      key encoding - the glue                      built
   src/lsm/        WAL · MemTable · SSTable · Compaction        built
```

**The directories above the line do not exist yet.** They are the plan, not the
code, and this diagram says so rather than letting the architecture imply a
completeness the repository does not have. `git log` and the status table are
the honest account of what is here; everything else is a design document.

Full design: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Data sources

All free, all open, all genuinely messy - the duplication is real, not seeded.

| Connector | Source | What it contributes |
|---|---|---|
| **CSV** | [UN/LOCODE](https://unece.org/trade/uncefact/unlocode) (UNECE), [NGA World Port Index](https://msi.nga.mil/Publications/WPI) | The code authority vs. the geographic authority. Diacritics, degree-minute coordinates, `ALL CAPS` names, patchy code coverage. |
| **REST/JSON** | [Fintraffic Digitraffic](https://www.digitraffic.fi/en/marine-traffic/) | Port calls - which *are* the `Voyage` entity, with arrival/departure links already implied. Plus AIS positions and a vessel registry. |
| **Postgres** | [MarineCadastre.gov AIS](https://hub.marinecadastre.gov/datasets/vessel-traffic-ais-1) (NOAA + BOEM + USCG) | MMSI-keyed rows with mostly no IMO, a different vessel-type taxonomy, US naming conventions. |

```bash
./data/fetch.sh all
```

---

## Results

Filled in as milestones land. Every number here is reproducible with a command.

**Read the conditions before the numbers.** All storage figures below come from
one command - `./build/bench/lsm_bench 200000 100` - on **one machine**: a Linux
container, GCC 11.4, 200,000 entries, 100-byte values, 4 MB write buffer,
1,000 keys per batch, `sync=false`. Change any of those and the numbers move,
sometimes by an order of magnitude. [`docs/BENCH.md`](docs/BENCH.md) has the
full table including a second platform, and says which differences are the
engine and which are the environment.

The most important caveat: **`sync=false` is not a durability claim.** With
`fsync` on every write the same engine does 655 ops/sec in that container,
because an fsync is a device round-trip and no amount of CPU work touches it.
That number is in `BENCH.md` too, and it is the one to quote if anyone asks
what the engine does when durability is actually required.

| | |
|---|---|
| Storage engine | 1.53M batched writes/sec · 494k random writes/sec · 3.7M misses/sec · write amplification **1.20x** |
| | 655 writes/sec with `sync=true` - the fsync floor, quoted so the batched number is not read as a durability figure |
| | Recovery bounded by buffer size, not data size: 2,013 records replayed instead of 202,200 |
| Correctness | 314 tests green on Linux, Windows and macOS · clean under ASan + UBSan · lock-free skiplist clean under ThreadSanitizer |
| | Differential test: 60k random ops vs `std::map`, forced across ~40 flushes and many SSTables |
| | Torn-WAL recovery: every acknowledged write survives a truncated log tail |
| | Block and SSTable CRCs reject single-bit corruption |
| | Bloom filter proven to have zero false negatives over 10k keys |
| | Leveled compaction: read amplification stays bounded as data grows 10x |
| | Iterators survive concurrent compaction deleting the files they read |
| Keyspace codec | Byte order proven to equal logical order for strings, signed ints and doubles |
| | The quarter-query verified to touch exactly the matching keys - a range scan, not a filter |
| Ontology | 21 transforms, each asserted pure: same chain, same input, same answer, every time |
| | Transform ids pinned by test, because they are written into provenance records forever |
| | Adding an entity type is a YAML edit - the loader rejects 14 classes of schema mistake at startup |
| Connectors | CSV, REST/JSON and Postgres behind one `RowSource` interface, all three streaming |
| | Re-ingesting an unchanged input is a verified no-op; a changed one never destroys the old batch |
| | Every stored value replays from its recorded transform chain and raw cell - the day 11 test in miniature |
| Entity resolution | *day 10* - F1 on a held-out labeled set |
| Blocking | *day 8* - reduction ratio, pair completeness |
| Lineage | *day 11* - round-trip verified for 100% of resolved properties |

### The result that matters most

> **Lineage round-trip property test.** For every property of every entity: read
> the provenance, fetch the raw source row it names, re-apply the transform
> chain by ID, and assert the result equals the stored value.

Lineage that isn't verified is a comment. This makes it an invariant.

---

## What this deliberately does not do

Stated up front, because knowing what you didn't build is part of the design.

- **No distribution.** Single node.
- **No ML-based resolution.** A hand-tuned Fellegi-Sunter-style scorer is
  interpretable and tunable on 500 labels; a learned model needs 10⁵ and gives
  worse lineage.
- **No cross-entity transactions.** `WriteBatch` gives per-merge atomicity.
- **No incremental re-resolution.** Re-ingest triggers a full re-resolve.
  Incremental ER - especially cluster *splits* - is genuinely a research
  problem.
- **No auth or multi-tenancy.**

---

## Documentation

| | |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Full design: keyspaces, LSM internals, ER algorithm, lineage model, query API |
| [`docs/EXECUTION_PLAN.md`](docs/EXECUTION_PLAN.md) | Day-by-day build order, cut lines, risk register |
| [`docs/INTERVIEW_PREP.md`](docs/INTERVIEW_PREP.md) | Every design decision, the alternative rejected, and the cost |
| [`docs/BENCH.md`](docs/BENCH.md) | Benchmarks and what they mean |
| [`docs/BUGS.md`](docs/BUGS.md) | Bug log |
| [`docs/adr/`](docs/adr/) | Architecture decision records |

## Licence

MIT
