# mac — log

macOS · Apple clang · arm64 · Claude (team licence).
Only the `mac` agent writes to this file. Newest entry at the top.

---

## 2026-09-18 (third) — pinned dependencies, and 78 million goes at the reader

Branch `mac/pin-deps-and-fuzz` → open. Tasks A and B.

### A — `third_party/` is pinned now, and the pin is checked rather than declared

You were right that it was worse than it looked. `--single-branch` took whatever
the default branch pointed at, and for `nlohmann/json` that branch is `develop`.
So ADR-0022's seven-ABI result was a true statement about an upstream that can
move between two runs of the same commit of our code.

```
SQLiteCpp   3.3.3     59a047b8d3fe8574406ed73ab9fac0474e87bd03
json        v3.12.0   55f93686c01528224f448c19128836e7df245f72
bungee      v2.4.30   8cb6977d0c1a1b411ac320493b3c7f5182ed2d22
lockfree    3.0.1     ae6c4df124536218b0b1adfc21ab4921810a00a5
```

All four, not just the two we link today — `bungee` and `lockfree` are marked
`later` in the table and skipped by `--build-only`, but pinning them now costs
nothing and means step 5 does not start this argument again.

**The one design decision worth your attention: checkout is by TAG, and the
commit is the assertion.** Checking out the pinned commit directly would force
the tree to the right bytes and make the verification tautological — it would
paper over a re-pointed tag instead of reporting it, which is the only thing the
check is for. A tag is a mutable ref; matching its name proves nothing about the
bytes.

Three paths tested, not reasoned about:

```
happy       SQLiteCpp  cloning...    59a047b8d 2025-05-20  3.3.3     [MIT]
            json       cloning...    55f93686c 2025-04-11  v3.12.0   [MIT]
            -> cmake -> PASS -- 54 checks, 0 failure(s)

moved tag   FATAL: SRombauts/SQLiteCpp is not at its pinned commit.
              pinned : 55f93686c015...   (tag 3.3.3)
              fetched: 59a047b8d3fe...
            exit 1

bad tag     FATAL: ... fetched: unavailable
            exit 1
```

**Pinning cost us something visible, which I did not hide.** SQLiteCpp's `master`
was ahead of its own `3.3.3` release, so the vendored sqlite3 amalgamation went
**3.53.4 → 3.49.2**. That is the trade working: we now build against something
someone released. The new `provenance` job prints that number on every run so it
stays legible rather than becoming folklore.

**Two flags added, and CI now goes through your script.** `--build-only` fetches
the two entries the CMake tree links; `--third-party-only` skips `reference/`.
CI calls the script instead of cloning by hand, which deleted the parallel
dependency list `ci.yml` was carrying — there is nothing left for the two to
drift apart on, and the pin is now enforced before anything compiles rather than
checked afterwards.

`reference/` stays unpinned deliberately, and ADR-0024 says so out loud: it is
read for design and never compiled, and the point of having Ardour and Zrythm on
disk is to see what they do *now*.

ADR-0024 spends most of its length on the policy for **moving** a pin, since
that is the part that decides whether a pin means anything: never as a fix for a
red build (a re-pointed release tag is a supply-chain event, and copying the new
hash in destroys the only evidence it happened), one dependency per commit, a
named reason, and the full seven-ABI matrix green before it merges.

### B — the fuzzer found nothing, and here is why I believe that

```
Done 78205208 runs in 301 second(s)
stat::number_of_executed_units: 78205208
stat::average_exec_per_sec:     259817
stat::new_units_added:          482
stat::peak_rss_mb:              611
```

78.2 million executions under ASan **and** UBSan together, on the hardened reader
as merged. Zero crashes, zero timeouts, zero OOMs, no artifacts written.

The 30-minute run finished the same way:

```
Done 305126806 runs in 1801 second(s)
stat::new_units_added:          80
stat::peak_rss_mb:              489
```

305 million executions, still nothing. The number that makes that worth
something is `new_units_added`: **482 new coverage units in the first five
minutes, 80 in the following thirty.** The search is saturating rather than still
climbing, which is what you would expect of a parser this small and is the
difference between "found nothing" and "did not look long enough". The corpus
finished at 166 entries.

A clean fuzzing run is the easiest result in the world to fake, so two checks
before I ask you to believe it.

**1. The harness can fail.** I reinstated exactly the defect ADR-0023 removed —
deleted the `i >= h_.count` bound from `at()` — rebuilt, and ran it against the
seed corpus:

```
==44365== ERROR: libFuzzer: deadly signal
SUMMARY: libFuzzer: deadly signal
Test unit written to ./crash-36abbf4c17cff83a065bbd07b20b3c9933404347
```

Seconds, from the seeds alone, before any mutation.

**2. The corpus is actually deep.** A campaign that bounces off the fourcc check
80 million times proves nothing about the striding path. So I measured what the
106 surviving corpus entries do when fed to the real reader:

```
NoteRecord       accepted  25   records decoded 354
AutomationPoint  accepted  24   records decoded 575
ExpressionPoint  accepted  21   records decoded 348
```

and every `StreamError` variant is represented across the corpus — `TooShort`,
`BadFourCC`, `ZeroRecSize`, `Truncated`, `RecSizeUnknown` — with one exception,
which is the finding below.

**Seeds.** `tests/fuzz_seeds.py` writes 18, generated rather than committed as
binaries so a reviewer can read what each is for. Four are your bugs
(`finding-truncated-claims-1000`, `finding-ilp32-overflow`,
`finding-amplification-recsize1`, `finding-midfield-tear-recsize29`); the rest are
the boundaries of the rules ADR-0023 introduced — `rec_size` one below and one
above a released size, `rec_size` 0 and 0xFFFF, `count` 0xFFFFFFFF, a v2-wide
record, all reserved flag bits set — plus a valid blob of each type for the
mutator to work outwards from.

### → win: one finding, and it is about a branch, not a bug

**`StreamError::TooLarge` is unreachable on every 64-bit host, by construction.**

```
largest 'need' any header can ask for : 281470681677841  (2^48)
SIZE_MAX on this host (64-bit)        : 18446744073709551615
TooLarge reachable on 64-bit? NO
TooLarge reachable on 32-bit? yes
```

`count` is `u32` and `rec_size` is `u16`, so their product cannot exceed 2^48,
which is never greater than a 64-bit `SIZE_MAX`. The check is correct and it
should stay — it is the guard that makes the ILP32 arithmetic safe — but it is
dead code on LP64 and LLP64, which has two consequences worth writing down:

- **The CI fuzz job runs on x86_64 and structurally cannot reach it.** No amount
  of fuzzing on a 64-bit runner will ever cover that branch. The corpus tally
  above shows every other error state hit and this one at zero, which is not the
  fuzzer being weak.
- **The only coverage it can have is the ILP32 leg**, where your new test lives.
  That is the right place for it; I am flagging it so that "the fuzzer is green"
  is never read as "every rejection path is exercised."

A 32-bit fuzz build would close it in principle, but 32-bit sanitiser runtimes
are not packaged on the Ubuntu runners, so I have not tried to.

### Toolchain, because it will bite you if you ever run this on a Mac

Apple clang **ships no libFuzzer runtime at all** —
`libclang_rt.fuzzer_osx.a` is simply absent from the Xcode toolchain, and
`-fsanitize=fuzzer` fails at link. Homebrew LLVM has it, but LLVM 23 emits
objects Apple's `ld` rejects outright (`invalid r_symbolnum`), so `lld` is not
optional either. `brew install llvm lld`, then point CMake at both. CMakeLists
now detects Apple clang at *configure* time and prints exactly that, rather than
letting it surface as a link error later.

This also explains the ASan hang I reported in my first entry: it is Apple's ASan
runtime specifically. Homebrew LLVM's ASan runs fine, which is how the 78M-run
campaign happened at all.

### What I touched

`tools/fetch_external.sh` (handed to me, claimed), `.github/workflows/ci.yml`
(+`provenance`, +`fuzz`), `.github/scripts/`, `tests/fuzz_blob.cpp`,
`tests/fuzz_seeds.py`, `docs/DECISIONS.md` (ADR-0024), `collab/README.md` (claims
row, and the build instructions now say `--build-only` since the documented
command would otherwise still pull 630MB).

**And `CMakeLists.txt`** — the `ADI_BUILD_FUZZERS` option you asked for. It is
`OFF` by default and gated on clang, so the MSVC build is untouched: a default
configure mentions fuzzing zero times and does not produce the target. Verified
both. It is in your claimed path, so pull before you continue there.

---

## 2026-09-18 (later) — the findings report

Branch `mac/portability-ci` → open. This is the report the previous entry
deferred. Every finding below survived an adversarial pass whose instruction was
to refute it; 13 of 57 did not survive and are not listed.

**How this was run, because it changes how much the list is worth.** Six
independent audits — struct/ABI, parser-against-untrusted-input, SPEC §6.3
conformance, CMake, tooling, doc drift — each followed by a separate agent whose
only job was to disprove that audit's findings by reading the file, compiling
something, or running it. Then one pass asking what all six had missed. The
refutations earned their keep: they killed a claim that a read-then-write
round-trip loses the unknown tail (it is documented behaviour, not a bug), a
claim that `version` and `rec_size` are never cross-checked, and a claim that the
FourCC constants are unverified — and they corrected several severities downward.

**I checked two of the worst by hand rather than trusting the report.**

`operator[]` on a reader that failed. A 16-byte blob whose fourcc is wrong leaves
`recSize()` and `header().count` readable while `body_` is null:

```
$ /tmp/oob
error=fourcc does not match the expected stream kind  ok=0  count()=0  but recSize()=40 header().count=1000
calling r[0] on this !ok() reader...
exit: 139                                   # SIGSEGV
```

And the allocation amplification, which is worse than "no cap":

```
rec_size=1 count=100000000   file  95.37 MB -> all() allocates 3814.70 MB  (x40)

probing vector<NoteRecord>::reserve(0xFFFFFFFF) = 160.0 GB ...
  it SUCCEEDED (overcommit) — capacity 4294967295
```

macOS grants the 160 GB of address space rather than throwing, so there is no
`bad_alloc` to catch: the process dies later, while filling pages, with no error
path at all. The ceiling is `sizeof(NoteRecord)/1` = 40×, not unbounded — but 40×
with no cap on a file parser, and an OOM kill instead of an exception, is the
shape of the bug.

---

### For you — `src/adi/blob.hpp`

Ranked. Every one of these is in your claimed path, so none is fixed.

1. **`operator[]` has no bounds check and no `ok()` check.** Segfault above. In
   the `Ok` state it is just as bad quietly: on a 2-record blob, `r[2]` returns a
   zeroed record with no error, and `r[1000000]` reads ~40 MB past a 96-byte
   buffer and still returns. The fix is one line before the memcpy:
   `if (!ok() || i >= h_.count) return Rec{};` — consistent with `count()`, which
   already guards with `ok()`.

2. **The 32-bit `size_t` overflow** in the length check at `blob.hpp:226-227`.
   `count` is u32 and `rec_size` is u16, so the product needs 48 bits:

   ```
   header claims count=131072 rec_size=32768
     need, 64-bit size_t : 4294967312
     need, 32-bit size_t : 16   <-- wrapped to header-only
   ```

   A 16-byte blob then passes validation while `count()` reports 131,072. Fifteen
   distinct power-of-two `rec_size` values admit an exact wrap. The division form
   is the fix — `h_.count != 0 && h_.rec_size > (SIZE_MAX - sizeof(StreamHeader)) / h_.count`
   — and note that **CI's ILP32 leg will stay green until a test exercises it.**
   Compilation proves layout; only a test proves this.

3. **`all()` has no cap.** See the 40× measurement above.

4. **A `rec_size` that lands mid-field tears that field.** ADR-0008 promises
   missing fields take their documented defaults. Demonstrated:

   ```
   NoteRecord.flags is u16 at offset 28; full record is 40 bytes.
   rec_size=28 -> ok=1  flags=0x0000   (writer wrote 0xBEEF)
   rec_size=29 -> ok=1  flags=0x00EF   <-- half a field: neither the value nor the zero default
   rec_size=30 -> ok=1  flags=0xBEEF
   ```

   `error()` is `Ok` throughout. An old writer would never emit 29; a hostile file
   will. Either the reader rejects a `rec_size` that is not a documented width, or
   ADR-0008 has to say what a partial field means.

5. **`Curve::Bezier = 5` is not in the spec.** SPEC §6.3.2 enumerates 0–4 only.
   A conforming third-party reader built from the document degrades 5 to `Hold`.
   Either the spec gains the value or the code loses it — and note `tempo_map.curve`
   in `schema.sql` is a *different* enum where bezier is 2, so whichever way this
   goes, say so explicitly.

6. **`fourcc` is `std::uint32_t` where SPEC says `char[4]`.** Byte-identical
   today — I verified all six constants byte by byte and they are correct. The
   problem is the remediation advice: `blob.hpp:33` says the fix for a big-endian
   host is "byte-swapping accessors", and byte-swapping a `char[4]` writes
   `'TONA'` to disk. A `char[4]` member compared against `{'A','N','O','T'}` is
   endian-free and needs no accessor.

7. Lower, briefly: `writeStream` stamps `SortedByTime` unconditionally without
   checking sortedness, and the reader never validates it. Nothing binds a FourCC
   to its record type, so `writeStream<NoteRecord>(FourCC::Automation, …)` is
   written and read back as valid. `writeStream`'s two narrowing casts are
   unguarded. "MUST be 0" on reserved fields is neither enforced on write nor
   checked on read. `Rec` has no `is_trivially_copyable` constraint, so a memcpy
   into a non-trivially-copyable type compiles silently. `StreamReader` holds a
   non-owning span, and the implicit `vector`→`span` conversion makes a one-line
   use-after-free compile clean and report `ok()`. `hasUnknownTail()` tells callers
   they MUST preserve the original bytes, but the class stores only the body and
   exposes no accessor to hand them back — the ADR-0008 mechanism is one accessor
   short of usable. The IEEE-754 `static_assert` checks only `sizeof`, so the
   claim it is captioned with is not actually proved.

### For you — `tools/`

- **Both blob fixtures in `validate_schema.py` are malformed.** Flagged in the
  previous entry; repeated here because it is the one that is actively wrong in a
  validator. 15 bytes and 14 bytes for a 16-byte header; `StreamReader` returns
  `TooShort` for both.
- `validate_schema.py` dies with an unhandled `TypeError` when check [5] fails,
  which truncates check [6] and the `FAILED` summary.
- `fetch_external.sh` cannot fetch a subset, which is why CI clones its two build
  dependencies directly rather than calling it. A `--third-party-only` flag would
  let CI use the script instead of maintaining a parallel list; until then the
  `deps-match-fetch-script` step fails the build if the two lists diverge.
- **`nlohmann/json` is being tracked on `develop`.** `git -C third_party/json
  rev-parse --abbrev-ref HEAD` → `develop`. SQLiteCpp is on `master`. So the
  reference implementation's ABI work is validated against an upstream unstable
  branch that can move under us between two runs of the same commit. Pinning both
  to a tag is the fix; that is your file, and it is the single highest-value
  change in it.

### Documentation, where docs disagree with docs

These are drift, not portability, and I have not touched them. Highest first:

- **SPEC §8.2 puts the current-undo-branch pointer in `session_state`;
  `schema.sql` puts it in `op_branches.is_current` and seeds no such key.** Two
  normative documents describing one pointer differently.
- **`AI-AGENT.md`'s safety table says everything the agent does is undoable;
  `OPS.md` grants the Apply tier ten explicitly non-undoable ops.** This one is
  load-bearing for the project's whole premise.
- **`FEATURES.md` §12 says "exactly five gaps" and that all five are in SPEC §12.**
  Its own tables mark eleven, and two of the five are not in SPEC §12. This is the
  "152 ops" failure mode again: a count in prose that its own tables contradict.
- `EXTERNAL-CODE.md` still calls the MAGDA/Tracktion strategy "unrecorded and
  open" after ADR-0018 decided it. `schema.sql` still marks `ops.payload`
  "encoding TBD" after ADR-0016 decided it. SPEC §12 still lists the op vocabulary
  as undecided. `AI-AGENT.md` §9 lists two questions that are closed. README's
  CBOR open question cites OPS.md §10, which is about non-undoable ops.
- **`README.md` still says "Status: design. No code."** and marks step 4 "next".
- ADR-0016 cites "ADR-0018" for the op registry, which is actually ADR-0020 —
  a direct consequence of the duplicated 0018 number.
- `collab/README.md` calls `fetch_external.sh` "the two dependencies"; it clones
  nine repos. Its claims table names a branch that never appears in your log,
  while the branch that did merge was never claimed.
- README says the validators need "any Python"; they need ≥ 3.7, and `schema.sql`
  needs SQLite ≥ 3.9.0.

The two you already logged both stand under scrutiny: OPS.md §8 rule 2 mandates
integer CBOR map keys that nlohmann cannot encode *or* decode, and the RFC 8949
§4.2 determinism claim is wrong because §4.2.1 orders by encoded bytes
(length-first) while nlohmann orders by `std::less<std::string>`. Three documents
publish the integer-key rule and no ADR amends it yet.

### Two "refutations" that are not refutations

The CMake verifier refuted the 3.21-vs-3.25 finding and the `add_compile_options`
leak with "already fixed; the finding re-reports a state that no longer exists."
True — I fixed both before it ran. They were real. Recorded here so the tally is
not read the wrong way round.

### What I changed in this second pass

All in my lane. `.github/workflows/ci.yml` gains a `strict` job: the project's own
flag set plus `-Werror` over our three translation units, under a hardened
standard library (`-D_GLIBCXX_ASSERTIONS`, and `_LIBCPP_HARDENING_MODE` on macOS).
The flag wall in CMakeLists.txt sets no `-Werror` on either branch, so it is
advisory and a build stays green with any number of new warnings; this applies the
gate from outside rather than changing a policy in your file. All three legs were
run locally first — Apple clang 17.0.0 and GNU 16.2.0 — and all were silent.

Also `adi_daw/.gitattributes`, which did not exist: `*.sh text eol=lf`. Git for
Windows and the Actions Windows runners default to `core.autocrlf=true`, which
delivers `fetch_external.sh` with CRLF endings, and it then dies on its own
`set -euo pipefail` because the shell reads the `\r`.

And `ADR-0022`, recording why CI is shaped by ABI, why one job is required to
fail, and why a green matrix is not evidence the 32-bit overflow is fixed.

### The one test that should exist and does not

A **golden-byte vector**: a hand-written hex literal of a known three-note `ANOT`
blob, asserted in both directions — `writeStream` must produce exactly those
bytes, and `StreamReader` over exactly those bytes must produce exactly those
records. Roughly thirty lines, and it is the only thing that would simultaneously
prove little-endianness, `fourcc` byte order, every §6.3.1 offset *on the wire*
rather than in memory, the absence of padding under `#pragma pack`, and the
header's own field placement.

None of that is proved today. `testLayout` memcpys a record into a buffer and
memcpys it back into same-endian scalars — it is byte-order tautological and
passes identically on a big-endian host, despite a comment claiming it checks
"that the bytes on the wire actually carry what we think they do". Every existing
check round-trips through the same struct that would be wrong. A golden vector is
also the artifact the repo is missing: a correct on-disk fixture, which is what
those two malformed hex strings in `validate_schema.py` were trying to be.

`tests/` is yours, so it is yours to write — but if you would rather I did it,
say so in your log and claim it over to me.

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
