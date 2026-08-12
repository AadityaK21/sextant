# Committed sample data

Small excerpts so the test suite, `sextant ingest` and the demo all run on a
clean machine with no network. The full datasets are downloaded by
[`data/fetch.sh`](../fetch.sh) and are not committed - UN/LOCODE alone is
around 110,000 rows.

**These files are excerpts, not captures.** The column names, JSON field names
and value formats match the real sources; the rows are a small hand-picked set
chosen to exercise specific behaviour. Where a row is here to demonstrate a
failure it says so. Run `data/fetch.sh` for the genuine article, and
`sextant ingest` will use whatever is on disk.

| File | Stands in for | What it exercises |
|---|---|---|
| `wpi/UpdatedPub150.csv` | NGA World Port Index (Pub 150) | `ALL CAPS` names, semicolon-separated alternates, missing UN/LOCODEs, quoted fields with embedded commas |
| `unlocode/code-list.csv` | UNECE UN/LOCODE | diacritics, degree-minute coordinates, the Function bitfield filter, split country and location columns |
| `digitraffic/ports.json` | `/api/port-call/v1/ports` | a bare JSON array |
| `digitraffic/vessel_details.json` | `/api/port-call/v1/vessel-details` | numeric IMO fields, one deliberately invalid check digit |
| `digitraffic/port_calls.json` | `/api/port-call/v1/port-calls` | a wrapped array (`records_at: portCalls`) and nested paths like `portAreaDetails[0].ata` |

## A note on the UN/LOCODE header row

The official UNECE distribution ships **without a header row**. `data/fetch.sh`
pulls the `datasets/un-locode` mirror instead, which is pre-parsed and carries
one, and this excerpt matches that shape.

That is not cosmetic. A headerless file forces a mapping to address columns by
position - `from: 5` rather than `from: "NameWoDiacritics"` - and the day
upstream inserts a column, every value silently shifts one place to the left.
Naming the column turns that into a load error instead of a dataset that is
quietly wrong.

## Why snapshots exist at all for the HTTP source

Lineage points at a source row. If that row only ever existed in an HTTP
response nobody kept, the provenance record is a claim rather than a reference.
So every response is recorded to disk on fetch and replayed from disk
afterwards, which also means the build does not depend on a live network and
Digitraffic's rate limits are hit once rather than on every run.
