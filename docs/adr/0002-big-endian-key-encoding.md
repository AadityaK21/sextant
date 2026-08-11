# ADR 0002 - Big-endian fixed-width integers in all keys

**Status:** accepted · **Date:** 2026-08-09

## Context

Every higher layer of the system - entities, links, provenance, indexes - is a
key encoding over one ordered key-value store. Those keys are composites:
`link_type ‖ port_id ‖ timestamp ‖ voyage_id`. The store orders keys with
`memcmp`.

## Decision

All fixed-width integers embedded in keys are encoded **big-endian**.
Varints are used only for *lengths inside records*, never for key fields.

## Rationale

Big-endian means **byte order equals numeric order under memcmp**.

That single property is what turns

> "all voyages into Rotterdam between 1 April and 1 July"

into a seek plus one contiguous sequential scan over

```
TIDX ‖ arrives_at ‖ rotterdam_eid ‖ timestamp_BE ‖ voyage_eid
```

with **no candidate examined and rejected**. Little-endian would make byte order
meaningless, and the same query would degrade to a full scan of all voyages with
a filter - `O(all voyages)` instead of `O(matching voyages)`.

Varints are excluded from key fields for the same reason: a varint's byte
ordering does not match its numeric ordering, so a varint in a key silently
breaks range scans.

This is enforced by a test rather than a comment -
`Coding.Fixed64BEPreservesNumericOrderUnderMemcmp` asserts the property over
20,000 random pairs.

## Consequences

- Encoding and decoding cost a byte-shuffle on little-endian hardware, i.e.
  effectively nothing next to a memory access.
- Composite keys share long prefixes by construction, which makes SSTable
  prefix compression (day 2) unusually effective on this workload.
- Anyone adding a key field must remember the rule. Hence this ADR and the
  banner comment at the top of `src/lsm/coding.h`.

## Note on the internal key trailer

The 8-byte trailer `(sequence << 8) | value_type` is also big-endian, but it is
*not* compared with memcmp - the comparator decodes it and sorts it
**descending**, so the newest version of a user key sorts first. Big-endian is
used there for consistency rather than necessity.
