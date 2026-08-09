# Sextant

**A mini-Foundry: ontology, entity resolution and cell-level lineage over a
from-scratch LSM storage engine.**

> Sextant ingests maritime data from three heterogeneous sources, maps it onto a
> declarative ontology, resolves duplicate real-world entities across sources,
> and records — for every single property value — the exact source row and
> transform chain that produced it.

*A sextant fixes your position by combining several independent observations.
That is literally entity resolution.*

C++20 · React · no storage dependencies

---

## Status

| Milestone | | |
|---|---|---|
| **Day 1** — memtable, WAL, recovery, snapshots | ✅ | 67 tests green |
| Day 2 — SSTable build/read, flush to L0 | ⬜ | |
| Day 3 — bloom filters, block cache, merging iterator | ⬜ | |
| Day 4 — VersionSet/MANIFEST, leveled compaction | ⬜ | |
| Day 5 — keyspace codec | ⬜ | |
| Days 6–7 — ontology, transforms, three connectors | ⬜ | |
| Days 8–10 — entity resolution | ⬜ | |
| Days 11–12 — lineage, query engine, HTTP API | ⬜ | |
| Days 13–14 — React frontend | ⬜ | |

Plan: [`docs/EXECUTION_PLAN.md`](docs/EXECUTION_PLAN.md).

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
cd build && ctest --output-on-failure
```

Dependencies are fetched and version-pinned at configure time — nothing to
install. Requires CMake ≥ 3.20 and a C++20 compiler.

```bash
./build/bench/lsm_bench 200000 100      # benchmarks
```

---

## The idea that holds it together

Most projects in this shape become seven disconnected demos. Sextant is one
system because of a single decision:

> **The LSM engine provides an ordered key-value store with prefix scan.
> Every layer above it is a key encoding.**

Entities, links, provenance, secondary indexes, blocking indexes and raw records
are all bytes in one keyspace, laid out so the access pattern each one needs is
a *sequential range scan*.

Graph traversal is not a subsystem — it is a prefix scan over
`LINKOUT ‖ entity ‖ link_type`. "All voyages through this port last quarter" is
not a filter — it is a range scan over a big-endian timestamp suffix
(see [ADR 0002](docs/adr/0002-big-endian-key-encoding.md)).

```
┌──────────────────────────────────────────────────────────┐
│ web/       React — browse entities, links, lineage        │
├──────────────────────────────────────────────────────────┤
│ src/api/   HTTP + JSON                                    │
│ src/query/ planner · index selection · traversal          │
│ src/ontology/  declarative types and links                │
│ src/resolve/   normalize → block → score → cluster → fuse │
│ src/lineage/   provenance emitted at every fusion decision │
│ src/connectors/  CSV · REST · Postgres                    │
│ src/codec/     key encoding — the glue                    │
├──────────────────────────────────────────────────────────┤
│ src/lsm/   WAL · MemTable · SSTable · Compaction          │
└──────────────────────────────────────────────────────────┘
```

Full design: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## Data sources

All free, all open, all genuinely messy — the duplication is real, not seeded.

| Connector | Source | What it contributes |
|---|---|---|
| **CSV** | [UN/LOCODE](https://unece.org/trade/uncefact/unlocode) (UNECE), [NGA World Port Index](https://msi.nga.mil/Publications/WPI) | The code authority vs. the geographic authority. Diacritics, degree-minute coordinates, `ALL CAPS` names, patchy code coverage. |
| **REST/JSON** | [Fintraffic Digitraffic](https://www.digitraffic.fi/en/marine-traffic/) | Port calls — which *are* the `Voyage` entity, with arrival/departure links already implied. Plus AIS positions and a vessel registry. |
| **Postgres** | [MarineCadastre.gov AIS](https://hub.marinecadastre.gov/datasets/vessel-traffic-ais-1) (NOAA + BOEM + USCG) | MMSI-keyed rows with mostly no IMO, a different vessel-type taxonomy, US naming conventions. |

```bash
./data/fetch.sh all
```

---

## Results

Filled in as milestones land. Every number here is reproducible with a command.

| | |
|---|---|
| Storage engine | 805k writes/sec · p99 read **2.18 µs** · [full benchmarks](docs/BENCH.md) |
| Correctness | 67 tests green · clean under ASan + UBSan · lock-free skiplist clean under ThreadSanitizer |
| | Differential test: 60k random ops vs `std::map`, including across recovery boundaries |
| | Torn-WAL recovery: every acknowledged write survives a truncated log tail |
| Entity resolution | *day 10* — F1 on a held-out labeled set |
| Blocking | *day 8* — reduction ratio, pair completeness |
| Lineage | *day 11* — round-trip verified for 100% of resolved properties |

### The result that matters most

> **Lineage round-trip property test.** For every property of every entity: read
> the provenance, fetch the raw source row it names, re-apply the transform
> chain by ID, and assert the result equals the stored value.

Lineage that isn't verified is a comment. This makes it an invariant.

---

## What this deliberately does not do

Stated up front, because knowing what you didn't build is part of the design.

- **No distribution.** Single node.
- **No ML-based resolution.** A hand-tuned Fellegi–Sunter-style scorer is
  interpretable and tunable on 500 labels; a learned model needs 10⁵ and gives
  worse lineage.
- **No cross-entity transactions.** `WriteBatch` gives per-merge atomicity.
- **No incremental re-resolution.** Re-ingest triggers a full re-resolve.
  Incremental ER — especially cluster *splits* — is genuinely a research
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
