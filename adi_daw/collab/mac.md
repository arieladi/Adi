# mac — log

macOS · Apple clang · arm64 · Claude (team licence).
Only the `mac` agent writes to this file. Newest entry at the top.

---

## 2026-09-18 — SPEC 6.3 holds on clang/arm64, and on two more ABIs besides

Branch `mac/portability-ci` → open.

**Headline: nothing in `blob.hpp` is wrong on this platform.** No `static_assert`
fired, no struct needed changing, and no spec claim turned out to be false. The
four record layouts are now proved on **three compilers and two architectures**
rather than one of each:

| compiler | arch | stdlib | StreamHeader | NoteRecord | AutomationPoint | ExpressionPoint |
|---|---|---|---|---|---|---|
| MSVC 19.44 (yours) | x86_64 | MS STL | 16 | 40 | 32 | 24 |
| Apple clang 17.0.0 | arm64 | libc++ | 16 | 40 | 32 | 24 |
| Apple clang 17.0.0 | x86_64 | libc++ | 16 | 40 | 32 | 24 |
| GNU 16.2.0 | arm64 | libstdc++ | 16 | 40 | 32 | 24 |

The x86_64 row is not a second machine: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`
compiles both slices in one build, so the `static_assert`s are evaluated twice
against two different ABIs, and Rosetta then runs the x86_64 slice for real.

**Results.** All of this is verbatim.

```
$ cmake -S adi_daw -B adi_daw/build -DCMAKE_BUILD_TYPE=Debug && cmake --build adi_daw/build
$ ./adi_daw/build/adi_tests
adi_tests -- SPEC 6.3 binary layouts

[layout]
[round trip]
[ADR-0008 striding]
[malformed input]
[automation + expression]

PASS -- 31 checks, 0 failure(s)

$ ./adi_daw/build/adi_tool versions
sqlite3   3.53.4
SQLiteCpp 3.3.3
C++       202002
```

x86_64 slice, under Rosetta, same binary:

```
$ arch -x86_64 /tmp/adi-univ/adi_tests
PASS -- 31 checks, 0 failure(s)
```

GNU 16.2.0, whole tree through CMake:

```
$ CC=gcc-16 CXX=g++-16 cmake -S adi_daw -B /tmp/adi-gcc -DCMAKE_BUILD_TYPE=Debug
-- The CXX compiler identification is GNU 16.2.0
$ cmake --build /tmp/adi-gcc && /tmp/adi-gcc/adi_tests
PASS -- 31 checks, 0 failure(s)
  StreamHeader        16 bytes
  NoteRecord          40 bytes  (ANOT)
  AutomationPoint     32 bytes  (AAUT)
  ExpressionPoint     24 bytes  (AEXP)
```

Both validators, on Python 3.9.6:

```
$ python3 adi_daw/tools/validate_schema.py   ->  PASS -- 0 problem(s)
$ python3 adi_daw/tools/validate_ops.py      ->  PASS -- 0 problem(s)
```

Release build: clean, 31/31. UBSan (`-fsanitize=undefined
-fno-sanitize-recover=all`): clean, 31/31 — so nothing in the packed-struct
handling is actually misaligned at runtime on arm64, it really is memcpy all the
way down. ASan could not be used: it hangs before `main` on this host even for
`int main(){return 0;}`, which is an Xcode 17 / macOS 26.6 problem, not ours. CI
runs ASan-capable runners, so that gap closes there, not here.

**Found, and fixed, in `CMakeLists.txt`.** Both are portability defects rather
than preferences, and both are in your lane — pull before you continue.

1. **`cmake_minimum_required(VERSION 3.21)` was wrong.** The `SYSTEM` argument to
   `add_subdirectory` was added in **CMake 3.25** (`cmake --help-command
   add_subdirectory` → `.. versionadded:: 3.25`). Anyone on 3.21–3.24 gets a hard
   configure failure on line 59, not a warning. Declared minimum is now 3.25.

2. **The non-MSVC warning set was landing on `sqlite3.c`.** `add_compile_options`
   is directory-scoped and ran *before* `add_subdirectory`, so `-Wconversion
   -Wsign-conversion` applied to the dependencies too:

   ```
   before:  1306 warnings   (1305 from third_party/SQLiteCpp/sqlite3/sqlite3.c,
                             1 from SQLiteCpp/src/Column.cpp, 0 from ours)
   after:      0 warnings
   ```

   `SYSTEM` on `add_subdirectory` does not and cannot help — it marks the
   dependency's *include directories* as system, which silences diagnostics from
   its headers when we include them, not diagnostics raised while compiling its
   own `.c` files. The flags now live on an `adi_warnings` INTERFACE target that
   only our three targets link. Verified both directions from
   `compile_commands.json`: our three TUs still get the full set, all 8
   third_party TUs get none, and a deliberately-narrowing line added to
   `blob.cpp` still produced `-Wshorten-64-to-32` (then reverted).

   I did **not** add `-Werror`, because that is a policy change rather than a
   portability fix and it is your file. For the record it would pass today: the
   full set plus `-Werror` is silent on all three of our TUs under both Apple
   clang 17 and GNU 16.2. I had predicted gcc would flag the integer promotions
   at `blob.hpp:227` and `:231`; it does not.

**Added, in my lane.**

- **`.github/workflows/ci.yml`** — `macos-latest`, `windows-latest`, plus four
  more legs, because two compilers is still not a portability claim. Seven ABIs:
  clang/arm64, clang/x86_64, clang/x86_64+libstdc++, gcc/x86_64, gcc/arm64,
  gcc/i386 (ILP32), MSVC/x86_64 (LLP64). Both Python validators run on 3.9, 3.11
  and 3.13, and on Windows — the documents they parse are full of en-dashes, and
  a bare `open()` there would decode them as cp1252. They pass `encoding="utf-8"`
  explicitly; the Windows leg is there to keep it that way.

  CI does **not** run `fetch_external.sh`. It clones the two dependencies the
  build actually needs. Cloning ardour and zrythm on every push would make an
  unrelated upstream outage look like our failure. A `deps-match-fetch-script`
  step fails the build if CI's two repos stop matching the `THIRD_PARTY` table in
  your script, so the two lists cannot drift silently.

- **A leg that is supposed to fail.** `abi-big-endian` cross-compiles `blob.cpp`
  for s390x and requires the compile to fail *with the words "byte-swapping
  accessors" in the diagnostic*. `blob.hpp:34`'s endianness `static_assert` has
  never been watched to fire; until it is, it is a comment rather than a
  guarantee. If someone later deletes it, that job goes red instead of us
  shipping a reader that mis-decodes every field on a big-endian host.

- **`.github/scripts/check_spec_layout.py`** — this one matters more than the
  rest. `adi_tests` proves the layouts are self-consistent; the `static_assert`s
  prove they equal four numbers written in a header. **Nothing proved those
  numbers were the ones `SPEC.md` publishes** — and SPEC.md is what a third-party
  implementer reads. This parses the sizes out of the spec's own prose and
  headings and diffs them against `adi_tool layout`, per ABI. It is the standing
  rule applied to the four numbers that did not have a validator yet. I tested it
  against four ways of being wrong — spec and build disagreeing, the spec wording
  drifting out from under the regex, `adi_tool`'s output format changing, and the
  binary missing — and it exits non-zero on all four.

**Verified rather than assumed, while I was in there.** Every count README.md
asserts is correct: 38 tables, 27 explicit indexes, 174 ops, nine repos in
`fetch_external.sh`, 22 ADR entries. The validators are CWD-independent
(`__file__`-relative, so `python adi_daw/tools/...` works from anywhere) and both
exit 1 on real failure — I proved that by corrupting copies in `/tmp`, not by
reading the code.

**→ win:** three things for you, and one process note.

1. **`DECISIONS.md` has two `ADR-0018` entries** — the original `OPEN` one and
   `ADR-0018 (revised)`. Appending the revision was right; reusing the number was
   not, since `collab/README.md` asks for "a new numbered ADR". "ADR-0018" is now
   ambiguous to cite, and the highest number in the file is 0021, so the next
   free number is 0022. I have not touched it — superseding by editing is exactly
   what the rule forbids, and the fix is another append, which is yours to write.

2. **A detailed findings report is not in this entry yet.** I ran a structured
   audit across six dimensions with adversarial verification of each finding;
   three of the six verification passes were cut short by a credit limit, and I
   am re-running them rather than reporting findings that have not survived a
   refutation pass. One that has already been verified and independently
   reproduced, because it is in a validator and you will want it early:
   **both blob fixtures in `validate_schema.py` are malformed.**

   ```
   event_streams   X'414E4F540100280001000000010000'  -> 15 bytes
   note_expression X'4145585001001800000000000000'    -> 14 bytes
   ```

   SPEC 6.3 says the header is 16. The first is one byte short and declares
   `count=1, rec_size=40` with an empty body; the second is two bytes short.
   `StreamReader` returns `TooShort` for both. The schema validator is currently
   inserting blobs the reference reader rejects.

3. **I touched `CMakeLists.txt`**, which your claims row covers. Two changes
   only, both above, both verified by a clean rebuild. Pull before you continue
   in that file.

**Process note.** I own `.github/**` and have added a claims row for it. The
monorepo's existing workflows are named per project (`console-plugin.yml`,
`midi-helper.yml`); `ci.yml` is the name the mission specified, but if a third
C++ project ever lands here, `adi-daw.yml` is the convention it should have.

---

---

## (no entries yet)

First mission is described in `collab/README.md` and in the handoff at the
bottom of `collab/win.md`. Append your first entry here when you have something
to report — including a negative result. "Built clean on clang/arm64, no
portability issues found" is a valuable entry, not an empty one.

Suggested shape for an entry:

```markdown
## YYYY-MM-DD — short title

Branch `mac/...` → merged / open.

**Did.** What changed and why.
**Found.** Anything surprising, especially anything that contradicts a doc.
**Results.** Actual command output, not a summary of it.
**→ win:** anything the other agent needs to know or act on.
```
