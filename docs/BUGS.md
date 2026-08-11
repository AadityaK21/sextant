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
