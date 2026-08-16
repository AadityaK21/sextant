# ADR 0005 - Veto-constrained union-find rather than plain transitive closure

**Status:** accepted · **Date:** 2026-08-16

## Context

Pair scoring produces a set of edges: "these two source records are the same
thing, with score 8.4". Turning those pairwise judgements into entities means
choosing a clustering.

The obvious answer is transitive closure by union-find: if A matches B and B
matches C, then A, B and C are one entity. It is one pass, near-linear, and
easy to explain.

It is also how entity resolution systems produce their most embarrassing
failures.

## The problem, concretely

Transitivity is an assumption about the data, not a property of it. Similarity
is not transitive: A can be close to B and B close to C while A and C are
plainly different. Union-find has no way to notice, because it never compares A
to C - it only ever merges components.

The failure compounds. Each merge makes the component larger, and a larger
component has more opportunities to attract a further merge. One wrong edge can
chain a hundred distinct entities into a single blob, and the result looks like
excellent deduplication until someone reads it.

This corpus contains the case deliberately. Three vessel pairs share an exact
MMSI and have **different IMO numbers**. An MMSI is tied to a radio licence and
is reassigned when a ship reflags; an IMO number is assigned to the hull for
life. Two hulls that briefly shared an MMSI are not the same vessel, and no
amount of other agreement changes that.

## Decision

Union-find, constrained by veto rules. Before any merge, check whether the two
components contain a pair with a hard-identifier conflict; if they do, refuse
the merge and record the refusal.

A veto is a **rule**, not a large negative weight. That distinction is the whole
decision: a weight can be outvoted by enough other evidence, and the entire
point of an IMO conflict is that it cannot be.

## Rationale

Both clusterings are computed and both are measured, on the same golden set, in
the same run. That is the argument, not the reasoning:

| | Precision | Recall | F1 |
|---|---|---|---|
| Plain transitive | 0.9726 | 1.0000 | 0.9861 |
| Veto-constrained | **1.0000** | 1.0000 | **1.0000** |

**10 merges refused.** Precision without the constraint is 0.973, which sounds
close to perfect and means roughly one in forty merged pairs is wrong - and each
wrong merge is two real-world things conflated into one record that then
propagates into every query and every downstream count.

Recall does not drop. That is worth stating explicitly, because the expected
objection is that a constraint must cost recall: refusing a merge should cost
you the true merges you refused. It does not here, because the veto fires only
on conflicting hard identifiers, and two records with different IMO numbers were
never the same hull. The constraint removes false positives without removing
true ones, which is what makes it a rule rather than a threshold.

## Alternatives considered

**Plain transitive closure.** Simpler, and the numbers above are the argument
against it. Kept in the codebase and measured on every run rather than deleted,
so the comparison stays live and the claim can be re-checked rather than quoted.

**Correlation clustering.** The theoretically right formulation: minimise
disagreement with the pairwise scores over all partitions. It is NP-hard, the
usual approximations need tuning of their own, and it would have consumed days
that the lineage work needed. The gap it would close is small - the constrained
clustering already reaches F1 1.000 on this golden set - so the return would
have been close to zero.

**A large negative weight instead of a rule.** Rejected, and the reason is in
`src/resolve/scorer.h`: a sufficiently strong pile of other evidence overcomes
any finite negative weight. The corpus contains three pairs constructed
specifically so that anything treating the IMO conflict as a weight rather than
a rule fails visibly.

**Post-hoc cluster splitting.** Merge transitively, then look for internally
inconsistent clusters and split them. Requires deciding *where* to split, which
is the hard part of the original problem in a worse position - now with the
evidence already discarded.

## Consequences

- Clustering is no longer a pure union-find. Each merge tests the two components
  for a conflicting pair, which costs more than the near-linear ideal.
- The refusals are recorded rather than silent. "10 merges refused" is a number
  in the resolve output, and each refusal names the conflict.
- A veto rule is only as good as the identifier it trusts. This relies on IMO
  numbers being correct in the source data; a mistyped IMO produces a refused
  merge rather than a wrong one, which is the right direction to fail in.
- Both clusterings run on every `sextant resolve`, so the comparison in the
  README is regenerated rather than remembered.

## Revisit if

The veto rules grow beyond a handful, at which point they start to look like a
constraint system and deserve to be expressed as one rather than as special
cases in the merge check.
