# ADR 0006 - Store every link twice

**Status:** accepted · **Date:** 2026-08-16

## Context

The engine underneath is an ordered key-value store. A graph edge has to be
expressed as a key, and a key has exactly one sort order, so the layout chosen
decides which traversal direction is a range scan and which is a table scan.

The two questions the domain actually asks are:

- *forward*: which port did this voyage arrive at?
- *reverse*: which voyages arrived at this port?

The second is the common one. Nobody opens a maritime dataset to ask about one
voyage; they ask what came through Rotterdam.

## Decision

Write each edge twice, under two keyspaces, in the same atomic batch:

```
0x04  LINKOUT   src_eid(16) link_type(2) dst_eid(16)   edge payload
0x05  LINKIN    dst_eid(16) link_type(2) src_eid(16)   edge payload
```

`EntityWriter::AddLink` writes both. It is not possible to write one from
outside `src/codec/`, because the API does not expose a way to.

## Rationale

**This is denormalisation, and denormalisation is the native idiom of an ordered
key-value store.** In a relational database you would index the foreign key and
let the planner choose; here, "add an index" and "write the fact under a second
ordering" are the same operation. Recognising that is most of the design.

Without `LINKIN`, "which voyages arrived at this port" has no better answer than
scanning every voyage and testing its `arrives_at`. On this corpus that is 1,000
voyages to find 92 arrivals; at a realistic scale it is the difference between a
range scan and a full table scan, and it is the single most common query in the
domain.

With it, the same question is a prefix scan over a contiguous byte range. The
measured result is in the README: **24 arrivals returned, 24 keys scanned**.

The cost is stated plainly: **2x link storage** and one more write per edge.
Links are 16+2+16 bytes of key and a small payload, and there are 3,000 of them
against 4,201 property values with full provenance. Link storage is not close to
being the dominant term.

## Why both writes must be in one batch

A half-written edge is worse than no edge. If `LINKOUT` lands and `LINKIN` does
not, the graph traverses correctly forwards and silently loses edges backwards -
and *silently* is the operative word, because every individual query still
returns a plausible answer. Nothing errors, no count looks obviously wrong, and
the discrepancy only appears if someone compares the two directions.

So `AddLink` puts both into the same `WriteBatch`, which the engine commits
atomically. The same argument covers `AddTimedLink`, which additionally writes a
`TIDX` entry: three keys, one batch.

This is also why the API shape matters more than the storage decision. A
`Store::PutLinkOut` that a caller could use on its own would make the invariant
a convention, and conventions are the things that hold until the day someone is
in a hurry.

## Alternatives considered

**Forward only, scan for reverse.** Half the link storage, and the most common
query in the domain becomes a full scan. Rejected on the numbers.

**Forward only, plus a secondary index on the target.** This is the same 2x
storage in a different wrapper, with an extra indirection at read time: the
index yields an id, which then needs a second lookup to get the edge payload.
Storing the edge itself under both orderings skips that hop.

**Adjacency lists: one key per entity holding all its neighbours.** Fewer, larger
values, and reads of one entity's whole neighbourhood become a single lookup.
Rejected for two reasons. Adding one edge means a read-modify-write of the whole
list, which is a lost update waiting to happen under concurrency. And a hub port
with thousands of arrivals produces one enormous value that has to be decoded in
full to read any part of it, which destroys the bounded-cost property that makes
the time-range scan worth having.

**A separate graph database.** Would answer traversal well and would make the
project two systems with a synchronisation problem between them, plus a second
place for lineage to be lost. The whole thesis of this project is that one
ordered keyspace serves all of it.

## Consequences

- 2x storage for links, and two writes per edge instead of one.
- Both directions are prefix scans. Neither is privileged.
- The graph can only become inconsistent if the engine's atomic write is broken,
  which is covered by `test_store.cpp` and the differential test.
- `TIDX` is a third copy of the same fact for timed links, anchored on the target
  and ordered by timestamp. Same reasoning, one step further: see
  [ADR 0002](0002-big-endian-key-encoding.md) for why the byte order of the
  timestamp is what makes it work.
- Deleting an edge would require deleting both keys. Nothing in the pipeline
  deletes individual edges today - `sextant resolve` clears the derived
  keyspaces wholesale and rebuilds - so this is a latent requirement rather than
  a live one, and it is written down here so it is not a surprise later.
