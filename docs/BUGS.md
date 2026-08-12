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

<!-- Add entries as you go. Suggested candidates from the plan:
     - the first tombstone resurrection you hit once SSTables land (day 2-4)
     - whatever the lineage round-trip test catches on day 11 (it will catch
       something - usually a transform that is not actually pure)
     - the transitive-chaining over-merge in entity resolution (day 10)
-->
