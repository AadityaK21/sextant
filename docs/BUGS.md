# Bug log

Keep this. Three sentences per entry, written the day you fix it.

By day 15 you will have a dozen entries, and **"tell me about a hard bug you
debugged"** turns from a question you dread into one you want. Nobody can fake
this file retroactively - the specificity is the signal.

Format:

```
## <short title>          <date>

**Symptom.**    What you actually observed. Not the cause - the observation.
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
`unsigned long` on LP64 Linux. Same width, *different types* - so there is no
single `T` for the initializer_list. Would have compiled fine on Windows, where
`uint64_t` is `unsigned long long`.

**Fix.** Declare an explicit `std::vector<uint64_t>` instead of relying on
deduction.

**Lesson.** `uint64_t` is a typedef, not a type. Braced lists deduce on the
*declared* type, and the answer differs between LP64 and LLP64. Worth knowing
before the Windows CI job finds it for you.

---

## Off by one bit, out by one byte          2026-08-12

**Symptom.** Four ULID tests failed together, and the failures made no sense as
a group: `StringRoundTrip`, `CrockfordDecodingIsForgiving`,
`GeneratedIdsAreUnique` and `GenerationIsThreadSafe`. Uniqueness and thread
safety have nothing to do with text formatting.

**Root cause.** 128 bits does not divide evenly into 5-bit base32 symbols. 26
symbols hold 130 bits, so the first symbol carries only **3** real bits and the
remaining 25 carry 125. I wrote 2, leaving 126 bits spread across 25 symbols -
which needs a 27th symbol to flush the remainder:

```cpp
std::string out(kTextSize, '0');            // 26 bytes
out[pos++] = kEncoding[(bytes_[0] >> 6) & 0x03];   // 2 bits, should be 3
...
if (bits > 0) out[pos++] = ...;             // writes out[26]
```

`out[26]` is one past the end. The uniqueness and thread-safety failures were
downstream: the overflow corrupted adjacent memory, so ids that were in fact
distinct compared equal.

**Fix.** Three bits in the first symbol, and an assertion that the encoder
consumes exactly 128 bits and emits exactly 26 characters.

**Lesson.** Two things. First, when several unrelated tests fail at once,
suspect memory corruption rather than looking for a shared logical cause - the
symptoms will not point at the bug. Second, bit-packing arithmetic deserves an
assertion rather than trust: `assert(pos == kTextSize && bits == 0)` would have
caught this the first time the encoder ran, instead of three tests later.

Worth noting that a debug build catches this instantly and the release build
happily wrote past the buffer, which is the same argument as the sanitiser job
in CI.

---

## 111 missing includes, invisible until CI ran          2026-08-09

**Symptom.** Four of five CI jobs failed. macOS passed. ubuntu-latest, ASan and
TSan all died in 19-26 seconds - too fast to be tests, so a build failure.
Locally: 145/145 green.

**Root cause.** libstdc++ used to pull in a great many headers transitively.
**GCC 13 removed most of that.** My sandbox runs GCC 11.4 (Ubuntu 22.04) and
CI's ubuntu-latest is 24.04 with GCC 13+, so code like

```cpp
#include <vector>
// ...
std::sort(logs.begin(), logs.end());   // <algorithm> never included
std::set<uint64_t> live;               // <set> never included
```

compiled fine for me and for Clang on macOS, and failed everywhere else. An
audit found **111** such omissions across the tree.

**Fix.** Added every missing include, and wrote `scripts/check_includes.py` -
a heuristic that maps well-known `std::` symbols to their canonical headers and
fails if one is used without being included. It runs as the first CI job because
it needs no compiler and finishes in seconds.

**Second bug, self-inflicted.** The script that inserted the includes sorted
each file's include block alphabetically. `env.cpp` has a `#if defined(_WIN32)
/ #else / #endif` block of platform headers, and sorting reordered the
directives themselves, producing `#else without #if`. The fixer now refuses to
touch that file and says so.

**Lesson.** The compiler you develop on is not the compiler your CI runs, and
"it builds" is a statement about one toolchain, not about the code. This is the
strongest possible argument for the three-OS matrix - and for putting the
cheapest, fastest check first, so a two-second script catches what would
otherwise be a two-minute build failure on four machines.

---

## Shutdown mid-compaction resurrected deleted data          2026-08-09

**Symptom.** `DifferentialTest.StateSurvivesRepeatedReopen` failed under ASan
only, and only sometimes: `key 'k32' should be absent but Get returned
'pbaluxevgjcbtaaowyiudjcpxamibrtfoqumrzze'`. A key that had been deleted came
back after a reopen, with a value from thousands of operations earlier.

**Root cause.** The compaction loop polls `shutting_down_` so it can bail out
when the DB is being destroyed:

```cpp
for (; input->Valid() && !shutting_down_;) { ... }
```

but nothing checked that flag *after* the loop. So on shutdown the compaction
exited early with only part of its input written, and then went straight on to
`FinishCompactionOutputFile` and `InstallCompactionResults` - committing a
MANIFEST edit that **deleted all the input files** while the output contained
only the keys processed before the interrupt.

If one of the keys not yet written was a tombstone, the value it had been
shadowing in a lower level became visible again. Deleted data, silently
restored, with no error anywhere.

Only ASan reproduced it because the sanitiser slows compaction enough that
`db_.reset()` reliably lands in the middle of one. In the release build the
compaction usually finished first.

**Fix.** Treat shutdown as a failure of the compaction:

```cpp
if (shutting_down_.load(std::memory_order_relaxed)) {
  status = Status::IOError("compaction abandoned: database is shutting down");
}
```

so the results are discarded and the input files stay live. Also made
`shutting_down_` atomic - the compaction loop reads it with the mutex released,
so the plain `bool` was a data race as well.

**Lesson.** A partial compaction is worse than no compaction. Anything that can
interrupt a multi-step commit needs a check at the COMMIT point, not only at
the loop condition - bailing out of the work is meaningless if you then publish
the half-finished result anyway. This is also the strongest argument for the
differential test existing: no unit test would have thought to kill a database
during a compaction and then check whether a specific deleted key came back.

---

## Iterators pinned the file set but not the memtable          2026-08-09

**Symptom.** `CompactionTest.IteratorSurvivesConcurrentCompaction` segfaulted.
The test opens an iterator, writes 20,000 more keys to force flushes and
compactions, then walks the iterator.

**Root cause.** Day 4 introduced refcounted `Version` objects precisely so an
open iterator could pin the SSTables it was reading. That worked. But the same
iterator also holds a `MemTable::Iterator` pointing directly into the
memtable's arena, and `mem_` / `imm_` were plain `unique_ptr`. A flush moved
the memtable to `imm_`, wrote it out, and destroyed it - freeing the arena the
open iterator was still walking.

**Fix.** Made `MemTable` refcounted with a private destructor, the same pattern
as `Version`, and had `NewIterator` `Ref()` both memtables and register an
`Unref` cleanup.

**Lesson.** Solving a lifetime problem for one resource does not solve it for
the others in the same object. The fix on day 4 was framed as "pin the file
set", and that framing hid the fact that an iterator reads from *two* kinds of
storage. Worth asking, whenever adding a refcount: what else does this thing
point at?

---

## One field, two representations, size_t underflow          2026-08-09

**Symptom.** Seven of the new iteration tests died with
`C++ exception with description "basic_string::_M_create"`, and several were
killed outright by the harness after allocating wildly. Even
`IterationTest.MemtableOnly` - three keys, no sstables - failed.

**Root cause.** `DBIter::saved_key_` held a **user** key everywhere except in
`Seek`, which overwrote it with an **internal** key (user key plus an 8-byte
trailer). `Next` then called `ExtractUserKey(saved_key_)` to recover the user
key, which does `Slice(data, size - 8)`. On a 1-byte key like `"a"` that is
`1 - 8` in `size_t` arithmetic: not -7, but 18446744073709551609. The Slice
happily described a 16-exabyte string, and `ToString()` threw.

**Fix.** `saved_key_` now holds a user key at all times; `Seek` builds its
internal-key target in a separate `seek_scratch_` buffer.

**Lesson.** The bug was not the arithmetic, it was letting one variable mean two
different things depending on which method last touched it. Unsigned
underflow just made the consequence loud instead of subtle - if `size_t` had
been signed, this would have been a quiet out-of-bounds read that passed the
tests. Naming the field `saved_user_key_` would have made the mistake visible at
the call site.

---

## An assertion encoded the wrong ordering, and only Debug knew          2026-08-09

**Symptom.** The whole day-2 suite passed in `RelWithDebInfo` - 98/98. The same
build with `-DSEXTANT_ASAN=ON` aborted:

```
Assertion `buffer_.empty() || key.compare(last_key_piece) > 0' failed.
```

**Root cause.** `BlockBuilder::Add` asserted that keys arrive in increasing
order, and expressed "increasing" as `Slice::compare` - a **bytewise**
comparison. But the keys are *internal* keys, ordered by
`InternalKeyComparator`: user key ascending, then trailer **descending** so the
newest version sorts first. Two versions of one user key are therefore correctly
ordered while being bytewise *decreasing*, and the assertion fired the moment a
flush put both into one block.

The code was right. The *assertion about* the code was wrong.

**Fix.** Gave `BlockBuilder` an `InternalKeyComparator` and asserted against
that instead.

**Lesson.** Two things worth keeping. First, `assert` compiles out under
`NDEBUG`, so a release-only test run silently skips every invariant you wrote -
which is the entire argument for the sanitiser job in CI running a Debug build.
Second, this engine has *two* orderings in play (bytewise for user keys,
InternalKeyComparator for internal keys) and code that confuses them will look
correct until versions collide. Worth being suspicious of any bare
`.compare()` on something that might be an internal key.

---

## windows.h macro silently renamed my function          2026-08-09

**Symptom.** Linux and GCC: clean build, 67 tests green. First MSVC build:
`LNK2019: unresolved external symbol "class Status __cdecl
sextant::lsm::DeleteFile(...)"` from three separate translation units. The
function was plainly there, in a file that plainly compiled.

**Root cause.** `<windows.h>` defines a large family of Win32 names as
*preprocessor macros* that expand to an `-A`/`-W` suffixed variant:

```c
#define DeleteFile      DeleteFileW
#define CreateDirectory CreateDirectoryW
#define GetMessage      GetMessageW
```

`env.cpp` includes `<windows.h>`, so my **definition** was preprocessed into
`sextant::lsm::DeleteFileW`. Every caller includes only `env.h`, never
`<windows.h>`, so their **references** stayed `sextant::lsm::DeleteFile`. Two
different symbols, one of which nothing defines. A macro does not respect
namespaces - that is the whole lesson.

**Fix.** Renamed to `RemoveFile`, and documented the hazard at the declaration
site so the next function added to `env.h` gets checked against the macro list.
Also set `NOMINMAX` and `WIN32_LEAN_AND_MEAN` globally for MSVC in
`CMakeLists.txt`, since `min`/`max` are the same class of landmine.

**Lesson.** Namespaces are a *language* feature; macros run before the language
exists. Any identifier that collides with a Win32 API name is a link-time trap
that a Linux-only CI will never catch. This is precisely the argument for the
three-OS build matrix in `.github/workflows/ci.yml` - it found this within
minutes of the first Windows compile.

---

## A raw string literal ended in the middle of a schema          2026-08-13

**Symptom.** Twenty compile errors in `test_schema.cpp`, starting with
`missing terminating " character` and then complaining that `Voyage`,
`arrived_at` and `link_types` were not types. None of those identifiers exist in
the file - they are YAML.

**Root cause.** The test schema was written as `R"( ... )"`, and it contained
the line

```yaml
display: "{name} ({locode})"
```

A raw string ends at the first `)` followed by `"`. The literal terminated
inside the YAML, and everything after it was compiled as C++.

**Fix.** A delimiter the content cannot contain: `R"YAML( ... )YAML"`.

**Lesson.** `R"(` is only safe for content you have read for `)"`. The failing
line number pointed at the end of the schema and the error named identifiers
from the data, which is the tell: when a compiler reports errors about words
that only appear inside a string, the string is not a string any more.

---

## The IMO check digit does not catch every typo          2026-08-13

**Symptom.** A test asserting that every single-digit corruption of a valid IMO
number fails validation. It failed, on `9574729`, `9014729`, `9034729` and
seven others. The checksum implementation matched the standard.

**Root cause.** Not a bug in the code - a wrong belief, caught by a test that
overclaimed. The IMO check digit weights the first six digits by 7, 6, 5, 4, 3,
2 and compares the last digit of the sum. Changing a digit by `d` at weight `w`
shifts the sum by `w*d`, which is invisible whenever `w*d` is a multiple of 10.
Weights 7 and 3 are coprime to 10 and catch everything. Weight 6 misses a shift
of five, weight 4 misses a shift of five, weight 2 misses a shift of five, and
weight 5 misses **every even shift**. For `9074729`, 7 of the 63 possible
single-digit typos validate as though nothing happened.

**Fix.** The test now derives the expected answer from the weight
(`(w * delta) % 10 != 0`) and asserts the implementation matches exactly,
including the misses.

**Lesson.** Worth more than the arithmetic: the resolver was going to treat an
IMO match as near-proof of identity, and this is the difference between "the
check digit rules out transcription errors" and "the check digit rules out most
transcription errors". A test that asserts a property you have not actually
proved is a good way to find out you were wrong about the domain, not just
about the code.

---

## An ignore rule quietly excluded the data the tests needed          2026-08-13

**Symptom.** Everything passed locally. The committed sample data was in the
working tree, the tests read it, `sextant ingest` loaded it. But `git status`
showed nothing to commit for `data/snapshots/`.

**Root cause.** `.gitignore` had `data/snapshots/*` with a single exception for
`data/snapshots/sample/`, a directory that never got created. The samples were
written to `data/snapshots/wpi/` and friends, so every one of them was ignored.
On any machine that cloned the repo, seven tests would have failed on missing
files - and they would have failed in CI rather than here.

**Fix.** Explicit un-ignore entries per committed directory, and a comment
saying why they are listed one by one rather than by wildcard.

**Lesson.** A negative pattern in `.gitignore` is a claim about a path that
exists. When it stops being true, nothing tells you - the file simply is not
there any more. Test fixtures that live outside the source tree are worth an
explicit rule and a sentence explaining it.

---

## Ten hulls with one IMO, and a golden set that believed it          2026-08-13

**Symptom.** The generated vessel corpus produced 995 labeled pairs of which 760
were matches. A three-to-one ratio of matches to non-matches is not what a
record-linkage set looks like, and it was the ratio - not any test - that gave
it away.

**Root cause.** The IMO was built as `f"9{index + 100:06d}"[:6]`, which for
index 0 formats "9000100" and then truncates to "900010". Indices 0 through 9
all truncate to the same six digits, so every group of ten vessels shared an IMO
number. Since the IMO is the truth id, the golden set concluded that ten
distinct hulls were one entity and emitted 45 "matches" per group.

**Fix.** `str(900000 + index)` with an `assert len(six) == 6` next to it.

**Lesson.** The bug was in the thing that decides what is true, so nothing
downstream could have caught it - the resolver would have been measured against
a corrupted answer key and any number it produced would have been meaningless.
Ground truth needs sanity checks of its own, and the cheapest one is a
distribution: match-to-non-match ratio, cluster sizes, identifier uniqueness.
Look at the shape of an answer key before trusting a score computed against it.

---

## The blocking key I argued for, measured at zero          2026-08-13

**Symptom.** Not a failure - a measurement. The per-key attribution table showed
`name_soundex` producing 358 candidate pairs, the second-largest contribution of
any key, and catching **zero** true pairs that another key had not already
caught.

**Root cause.** Nothing is broken. The cross-source name variation in this
corpus is casing, diacritics and truncation, and the normalizer already folds
all three before the phonetic key runs. Soundex earns its keep when two sources
*spell* a name differently, and the sources modelled here both derive from the
same romanisation, so that case does not occur.

**Fix.** None yet, deliberately. Cutting the key because a 451-record sample did
not need it would be over-fitting to the sample - the full UN/LOCODE download
contains exactly the transliteration variance this corpus lacks. It stays in,
with the table in `docs/ER.md` as the standing reason to revisit it after
`data/fetch.sh`.

**Lesson.** The value was in building the attribution column at all. "Which key
caught this pair, and was it the only one" costs a `std::vector<std::string>`
per candidate and turns an argument about blocking design into a table. The
counts were what changed my mind; the header comment in `phonetic.h` had
predicted this outcome and I had still assumed the key was pulling its weight.

---

## Parallel tests, one database directory, eight segfaults          2026-08-13

**Symptom.** Eight of the eleven blocking tests crashed with SIGSEGV under
`ctest -j4`. Run individually, every one passed. Run with `-j1`, all passed.

**Root cause.** `gtest_discover_tests` registers each `TEST_F` as its own ctest
entry, so ctest runs them as separate processes - each executing the whole
fixture, including `SetUpTestSuite`. Every process opened the same directory
name. The first took the LOCK, the rest got an error from `Store::Open`, the
`ASSERT_TRUE` in `SetUpTestSuite` aborted setup but did not stop the test bodies
from running, and they dereferenced a null `store_`.

**Fix.** A directory name including the process id, plus reporting the actual
`Status` rather than only asserting on it.

**Lesson.** A crash that only appears under parallelism is usually not a memory
bug, however much a segfault suggests one - it is a shared resource with a fixed
name. And an `ASSERT_` in `SetUpTestSuite` does not prevent the tests from
running, which is worth knowing before spending an hour reading a blocker for
the pointer error that was never there.

---

## A veto rule that never runs          2026-08-15

**Symptom.** A test asserting the port distance veto fires at least once over
the corpus. It fired zero times.

**Root cause.** Not a bug - an ordering consequence I had not thought through.
The distance check is third, after the locode and country vetoes, and blocking
only proposes pairs that already share a code, a name or a geographic cell. By
the time a pair reaches the distance check, everything it would have caught has
been rejected for a more specific reason. The rule is redundant given today's
blocking scheme.

**Fix.** The test now asserts the count is **zero** and says why, so the day the
blocking scheme changes and the veto starts firing, the test fails and the
comment gets revisited. A second test exercises the rule directly on a synthetic
pair - Portsmouth, New Hampshire against Portsmouth, Virginia, same name, same
country, 837 km apart.

**Lesson.** A rule nobody runs is a rule nobody is testing, and deleting it is
not obviously right either, because it is only redundant against a configuration
that can change. Asserting the redundancy is the honest middle: the guard stays,
its uselessness is documented, and the documentation is executable.

---

## Two Portsmouths, and a test fixture built on a bad guess          2026-08-15

**Symptom.** The replacement test above failed immediately. It scored Portsmouth
New Hampshire against Portsmouth Virginia expecting *no* veto - I had picked
them as a "close enough to be ambiguous" pair - and got `837 km apart, beyond
the 150 km limit`.

**Root cause.** My mental estimate of the distance was wrong by a factor of
five. The code was right; the fixture encoded a guess about geography that I had
not checked.

**Fix.** A synthetic pair 94 km apart for the under-the-limit case, and the two
real Portsmouths kept as the over-the-limit one - where they are a genuinely
good example, since name similarity alone would merge them happily.

**Lesson.** This is the second time in two days a test failed because the
*expectation* was wrong rather than the code, after the IMO check digit. Both
times the failure taught me something about the domain. A test that encodes a
belief you have not verified is still worth writing - it just needs to be read
as a question rather than an assertion when it goes red.

---

## A negative control that did not fail, and was right not to          2026-08-15

**Symptom.** The lineage round trip reported 100% on its first run. The plan
had said in bold that it *will* find something, so I did not believe it and
wrote a negative control: break `title_case`, replay a database written before
the break, expect failures.

It still reported 100%. That looked like the checker was doing nothing at all.

**Root cause.** The checker was fine. Fusion picks the winning value by source
trust, and for `name` that is always UN/LOCODE - whose `NameWoDiacritics`
column is already in title case. Running `title_case` over "Rotterdam" produces
"Rotterdam" whether the function works or not, so breaking it genuinely changed
nothing about any stored value.

**Fix.** Break `first_char` instead, which turns the World Port Index's "Large"
into "L" and is fed by no other source. 144 failures, each naming the entity,
the raw cell, the replayed value and the stored one.

**Lesson.** A negative control that does not fail is a statement about the
control, not necessarily about the thing under test - but you cannot tell which
without a second one. The test file now carries this story above the round trip,
because "100%" is the most dangerous number in the project and the next person
to read it deserves to know it was checked.

---

## Records that were never compared were never created          2026-08-15

**Symptom.** `sextant resolve` reported `0 link references -> 0 edges`. The
graph was empty, so the headline time-range query had nothing to scan.

**Root cause.** Clustering seeded its union-find from the endpoints of the
candidate PAIRS. A record that blocking never proposed a pair for therefore
never entered any cluster, never became an entity, and disappeared. Voyages hit
this every time - they produce no blocking keys at all, because they are
resolved through their links rather than by comparing attributes.

It was also silently losing any Port or Vessel that happened to be blocked with
nothing: 459 records produced 187 entities, and eight of the missing ones were
every Voyage in the corpus.

**Fix.** Both clustering functions take the full record list and seed from it,
so anything with no edges becomes a singleton cluster rather than nothing at all.

**Lesson.** The bug hid because the counts looked plausible - a dedup ratio of
0.59 is exactly what heavy merging looks like. It only surfaced downstream, when
a completely different subsystem reported zero. Worth asking of any pipeline
stage: what happens to an input this stage has no opinion about?

---

## The port calls referenced vessels that no longer existed          2026-08-15

**Symptom.** With the graph populated, `arrives_at` and `departs_from` resolved
240 edges each and `operated_by` resolved none. All 8 references were reported
unresolved.

**Root cause.** `eval/make_corpus.py` regenerates the vessel feeds with fresh
MMSIs, and `port_calls.json` was the one file it did not regenerate - it was
still the hand-written original from day 7, naming MMSIs like 230123456 that
had not existed since the corpus grew. The links pointed at nothing, and
pointing at nothing is exactly what "unresolved" means, so the pipeline was
reporting the truth.

**Fix.** The generator builds port calls too, drawing both endpoints from the
locodes and MMSIs it just emitted, so a reference cannot name something absent.

**Lesson.** A generator that produces four of five related files leaves the
fifth as a landmine, and it will not go off until something downstream tries to
join across them. If data is generated, generate all of it.

---

## The time index was chosen in a direction it cannot serve       2026-08-16

**Symptom.** None, at first. The planner chose `TIDX` whenever a hop had a time
predicate and the link declared a `time_index`, and the quarter query worked.

**Root cause.** TIDX is anchored on the **target** of the timed link:

```
TIDX | link_type(2) | anchor_eid(16) | ts_be(8) | entity_id(16)
```

The anchor is the port, because the question the index exists for is "what
arrived *here* in this window". That makes it useful walking *backwards* along
the link, from the port to the voyages. Walking forwards, from a voyage to its
port, there is no anchor to seek to - the executor would have seeked to a key
prefixed by the voyage's id, found nothing, and returned an empty result.

It never fired because the natural way to write the query is the direction that
works. A demo would have hit it the first time someone asked "which port did
this voyage arrive at, in April".

**Fix.** The planner tests direction as well as declaration, and when the index
cannot serve the hop it says so in the reason rather than choosing it anyway:

> `'arrives_at' has a time_index, but TIDX is anchored on Port and this hop
> walks away from the anchor, so the window is applied as a filter instead`

**Lesson.** An index is not "available for a link". It is available for a
*direction* of a link, and a planner that only checks the schema flag will
happily pick one that cannot answer the question. The keyspace layout said this
plainly and I read past it.

---

## Declining to use an index quietly dropped the predicate         2026-08-16

**Symptom.** Caught while writing the fallback above, before it could ship.

**Root cause.** The time window lived only in the TIDX branch. When the planner
chose `LINKOUT` instead, nothing applied the window at all - the hop returned
every neighbour, ignoring the dates entirely. Choosing a slower access path had
silently become choosing a *different question*.

**Fix.** The executor applies the window as a filter on the materialised entity
whenever the plan did not push it into a key range, and there is a test that
runs the same window both ways and asserts the two return the same voyages:

```
index 24, scan-and-filter 24, all arrivals 92 (24 keys vs 92)
```

**Lesson.** The line between "how fast" and "what answer" is the one line in a
query engine that must never move. Every access-path decision needs a partner
test proving the answer did not change, because the failure mode - fewer rows,
returned quickly - looks like success from every angle except correctness.

---

## The architecture document named a link the schema did not have  2026-08-16

**Symptom.** Six tests failed with `unknown link type: arrivals`.

**Root cause.** `ARCHITECTURE.md` §10 uses `"link": "arrivals"` in its example
traverse request. The ontology declares the link as `arrives_at` with
`inverse: arrivals`. Only the forward name resolved, so the documented request
could not run.

Neither was wrong on its own. `arrivals` is the right name for the question a
port asks, and it is what a UI would put on a button. The gap was that nothing
in the code knew the inverse name existed.

**Fix.** `Ontology::LinkOrInverse` resolves either name and reports which end
was named, and naming the inverse *is* the direction - so a client asking a Port
for its `arrivals` never has to supply a `reverse` flag it would have to
re-derive the schema to get right.

**Lesson.** A design document is only executable if something executes it. This
one had been read many times and the mismatch survived every reading, because
prose does not fail.

---

## The search box had no index behind it                           2026-08-16

**Symptom.** Not a failure. The planner said so, out loud, on its first run:

```
full scan of Port: no usable index for name starts_with Rott
```

**Root cause.** `Port.name` was declared `title: true` but not `indexed: true`.
The `/api/entities?q=` route searches the title property, so the search box -
the single most-used thing in the eventual UI - was going to be a full scan of
every port, every keystroke.

**Fix.** `indexed: true` on the title property of Port and Vessel. The prefix
search is now a genuine range scan, which needed one more piece:
`EncodeOrderedStringPrefix`, the escaping *without* the terminator. The
terminated form is an exact-match bound - `"ROTT"` plus its terminator sorts
after every key for `"ROTTERDAM"`, so seeking to it finds nothing.

**Lesson.** This is the entire argument for putting the plan on the response.
The query would have worked. It would have been correct, and slow, and nothing
would have said why - and by the time anyone profiled it, the missing index
would look like a database problem rather than a one-line schema fix.

---

## The CLI and the API disagreed about what "dedup ratio" meant    2026-08-16

**Symptom.** `sextant resolve` printed `dedup ratio 0.1819`. `/api/stats`
returned `"dedup_ratio": 0.8181`. Same database, same moment.

**Root cause.** Two defensible definitions of one name. The CLI reports the
fraction of records *removed* by merging; the API had been written to report
entities per source record. They are complements, so both looked plausible in
isolation and neither test noticed.

**Fix.** The API matches the CLI, and the test now derives the expected value
from the two counts in the same response rather than asserting a loose range.

**Lesson.** A metric with two definitions is worse than no metric. This one
would have surfaced live, in the worst possible way: a number on screen that
does not match the number in the README.

---

<!-- Add entries as you go. Suggested candidates from the plan:
     - the first tombstone resurrection you hit once SSTables land (day 2-4)
     - whatever the lineage round-trip test catches on day 11 (it will catch
       something - usually a transform that is not actually pure)
     - the transitive-chaining over-merge in entity resolution (day 10)
-->
