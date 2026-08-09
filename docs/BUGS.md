# Bug log

Keep this. Three sentences per entry, written the day you fix it.

By day 15 you will have a dozen entries, and **"tell me about a hard bug you
debugged"** turns from a question you dread into one you want. Nobody can fake
this file retroactively — the specificity is the signal.

Format:

```
## <short title>          <date>

**Symptom.**    What you actually observed. Not the cause — the observation.
**Root cause.** What was really wrong.
**Fix.**        What you changed.
**Lesson.**     What you would do differently, or what invariant now protects you.
```

---

## Uninitialised const member in DBImpl          2026-08-09

**Symptom.** `error: uninitialized const member in 'const class InternalKeyComparator'`
when compiling `db_impl.cpp`, pointing at a class with no data members at all.

**Root cause.** `InternalKeyComparator` is stateless, but declaring the member
`const` makes it a const object of class type with no *user-provided* default
constructor. `= default` on the first declaration does not count as
user-provided, so the compiler refuses to default-initialise it.

**Fix.** Value-initialise explicitly in the constructor's member-initialiser
list: `internal_comparator_{}`.

**Lesson.** "Empty class" and "trivially constructible in every context" are
not the same thing. Value-initialisation with `{}` is the general escape hatch.

---

## initializer_list deduction failure across integer widths          2026-08-09

**Symptom.** `unable to deduce 'std::initializer_list<auto>&&' from
{0, 127, 128, 16383, 16384, (1 << 35), (~0)}` in a range-for over a braced list
of what looked like uniformly 64-bit constants.

**Root cause.** `0ull` is `unsigned long long`; `~static_cast<uint64_t>(0)` is
`unsigned long` on LP64 Linux. Same width, *different types* — so there is no
single `T` for the initializer_list. Would have compiled fine on Windows, where
`uint64_t` is `unsigned long long`.

**Fix.** Declare an explicit `std::vector<uint64_t>` instead of relying on
deduction.

**Lesson.** `uint64_t` is a typedef, not a type. Braced lists deduce on the
*declared* type, and the answer differs between LP64 and LLP64. Worth knowing
before the Windows CI job finds it for you.

---

<!-- Add entries as you go. Suggested candidates from the plan:
     - the first tombstone resurrection you hit once SSTables land (day 2-4)
     - whatever the lineage round-trip test catches on day 11 (it will catch
       something — usually a transform that is not actually pure)
     - the transitive-chaining over-merge in entity resolution (day 10)
-->
