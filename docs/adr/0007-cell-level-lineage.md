# ADR 0007 - Cell-level lineage, verified by replay

**Status:** accepted · **Date:** 2026-08-16

## Context

Every data platform claims lineage. Almost all of them mean **dataset-level**
lineage: this table was built from those tables, by that job, at that time. It
is drawn as a DAG of boxes, it is genuinely useful for impact analysis, and it
cannot answer the question anyone actually asks when they distrust a number.

That question is: *why does this field say Rotterdam?*

Dataset lineage answers "it came from the ports pipeline". The follow-up - which
of the four sources won, what the other three said, and what transformed the
original bytes into this - is not representable in it.

## Decision

Lineage is recorded **per property of per entity**, and it is **verified by
replay rather than asserted**.

Each fused value carries a `Provenance` record naming:

- the exact source, batch and row it came from, and the column within that row
- the transform chain, by pinned numeric id, plus a fingerprint of the chain
- the fusion rule that chose it, and its confidence
- every rejected alternative, with the reason it lost
- the cluster members and the merge evidence, with per-feature contributions

And the round-trip property test, over every property of every entity: read the
provenance, fetch the raw row it names, extract the cell, re-apply the chain by
id, and assert the result equals the stored value.

**4,201 of 4,201 properties pass. 100%.**

## Rationale

**Lineage that is not verified is a comment.** It is written at the same moment
as the value, by the same code, from the same variables. If that code has a bug
- an off-by-one column index, a chain recorded before a step was appended - the
provenance is wrong in exactly the way that makes it useless, and nothing
notices, because the only thing that would notice is a check nobody wrote.

Replay converts the claim into an invariant. The test does not trust the
provenance record; it uses it as instructions and checks the result. A wrong
column produces a mismatch. A stale chain produces a mismatch. A transform that
is not actually pure produces a mismatch.

This is only possible because of two decisions made earlier and specifically to
enable it:

1. **Transforms are pure functions with pinned numeric ids.** `TransformFn` is a
   plain function pointer, so a transform cannot capture state, and the id is
   written down by hand and never reused. A chain recorded today replays
   identically years later. Had transforms been closures or lambdas over
   configuration, replay would be impossible and this ADR would not exist.
2. **`RAW` is append-only and never overwritten.** Provenance points at bytes.
   Provenance pointing at bytes that were later overwritten is not provenance.

## The cost, stated plainly

One `PROV` record per property, not per row or per table. For this corpus that
is 4,201 provenance records against 1,187 entities, and each one carries the
rejected alternatives too - so the lineage is comfortably larger than the data
it describes.

That is the trade, and it is worth it here because the whole point of the
project is the question dataset lineage cannot answer. It would not be worth it
for a system whose values are copied rather than fused: if there is only ever
one source for a field, "which source won and why" has no content.

## Alternatives considered

**Dataset-level lineage.** Cheap, standard, and answers a different question.
Would have been dishonest to build and then describe as lineage in a README, and
an interviewer who has used both would spot the gap in one follow-up.

**Recording provenance and trusting it.** This is what most implementations do,
and it is what this one did until the round-trip test was written. Writing the
test found real bugs, which is the argument: see `docs/BUGS.md` for the
clustering that dropped every record blocking never paired, and the port calls
referencing vessels that no longer existed.

**Storing the replayed value instead of replaying.** Would make the check a
comparison of two stored strings, both written by the same code path at the same
moment, which proves nothing. The check has value precisely because it
recomputes from the raw bytes through the declared chain.

**Lineage as a separate store or service.** A second system to keep in sync with
the first, and a second place for the answer to be stale. Provenance lives in
the same keyspace as the data, written in the same atomic batch, so it cannot
drift from what it describes.

## Consequences

- `sextant explain` is a check, not a report. It can fail, and a negative
  control in `test_lineage.cpp` corrupts a provenance record to prove it does.
- The lineage panel in the UI recomputes the verdict on every open rather than
  displaying a stored flag.
- Union-fused properties are checked by containment rather than equality, since
  no single raw cell can reproduce a merged list. That rule lives in
  `LineageReader::Explain()` - having it in two places once caused the API and
  the round-trip test to disagree, which is written up in `docs/BUGS.md`.
- Provenance storage exceeds the size of the resolved data. Acceptable here;
  would need revisiting at a scale where the lineage dominates the disk budget,
  most likely by keeping full provenance for a retention window and a reduced
  form beyond it.
- A transform's id can never be reused. That is a permanent constraint on the
  schema, stated in `schema/ontology.yaml`, and the loader rejects duplicates so
  the failure is a startup error rather than corrupt lineage.
