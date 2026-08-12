# ADR 0003: The HTTP connector reads from disk by default

**Status:** accepted, 2026-08-13
**Context:** days 6-7, the three connectors

## Context

One of the three sources is a REST API. The obvious shape is a connector that
takes a URL, performs the request, parses the response and emits records. Every
tutorial connector looks like that.

It does not work here, for three separate reasons that all point the same way.

**Lineage.** A provenance record names a source row: `(source, batch, row)`.
The whole claim of this project is that you can follow that reference back to
the bytes the source produced. If those bytes only ever existed in an HTTP
response that nobody kept, the reference is a claim rather than a pointer. The
API returns *current* port calls; ask it the same question tomorrow and the
answer is different. A lineage record pointing into that is not lineage.

**Testing.** A connector that calls the network cannot be tested without the
network. CI would depend on Fintraffic being up, on the response shape not
having drifted, and on the rate limiter being in a good mood. The tests that
matter here - does a nested path resolve, does an invalid IMO get rejected with
a reason, does a re-ingest skip - have nothing to do with HTTP.

**Courtesy.** Digitraffic is free and keyless and asks users to identify
themselves and not hammer it. A build that hits it on every `ctest` run is
rude, and the rudeness scales with how well the project goes.

## Decision

Fetching and parsing are separate operations behind a `Fetcher` interface.

```
Fetcher          returns the raw response body for an endpoint
  SnapshotFetcher  reads data/snapshots/<source>/<endpoint>.json
  HttpFetcher      performs the request AND records it as a snapshot

JsonRowSource    parses a body into rows; knows nothing about where it came from
```

`sextant ingest` uses `SnapshotFetcher`. Recording a new snapshot is a separate,
explicit step - `data/fetch.sh`, and later `sextant fetch`. Small excerpts are
committed so a clean clone runs with no network at all.

## Consequences

**Good.**

- Lineage points at bytes that still exist. `sextant lineage --source
  digitraffic --batch 2 --row 7` prints the record, a year later, unchanged.
- The whole test suite runs offline. So does the demo.
- Ingest is deterministic, which makes the idempotency check meaningful: the
  same file produces the same fingerprint and the second run does nothing.
- The API is hit once per fetch rather than once per run.

**Bad, and accepted.**

- The data is only as fresh as the last fetch. For a project whose subject is
  provenance rather than real-time traffic, that is the right trade.
- There is a step between "clone" and "current data" that a user has to know
  about. Mitigated by the error message: a missing snapshot names the command
  that would create it, rather than just failing.
- Pagination and rate-limit handling live in the fetcher, not the connector, so
  they are exercised less often than the parsing path. This is a real gap and
  it is worth saying so.

## Alternatives considered

**Fetch live, cache in the store.** Write the response to RAW on first use and
read from there afterwards. This preserves lineage, and it was tempting. It was
rejected because it makes the first run of a fresh database require the
network, which is exactly the run a new contributor makes - and because a cache
that is also the archive has no way to be repopulated if the store is deleted.

**Fetch live, no caching, accept the lineage gap.** Rejected outright. The
lineage round-trip test on day 11 is the headline result of the project; a
source it cannot cover would have to be excluded from it, and a property test
with an exception is not a property test.

**Record a full HTTP transcript (VCR-style).** More faithful - headers, status
codes, timing - and more machinery than a JSON file per endpoint deserves. If
the fetcher grows retry and pagination logic worth testing, this becomes worth
revisiting.
