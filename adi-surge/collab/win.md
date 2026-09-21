# win — log

Windows 11 desktop · MSVC 19.44 (VS 2022 Community) · x64 · Claude Opus 5.
Only the `win` agent writes to this file. Newest entry at the top.

---

## 2026-09-21 — ADR-0010 and the first pack, `wavetables/adi-gen-01/`

Branch `win/adi-surge-wavetables`.

**Adi's call replaces ADR-0009's caution.** Tables generated from a
reference's descriptor are ours if they don't null against it, and the
references are not named again. ADR-0010 records the rule and its two
exemptions: pure sines, and textbook shapes.

**Null test on the 80 tables (best gain, shift and polarity; `null_dB`):**

- **Deepest frame: −30.5 dB.** Identical audio would go to −90 dB and below.
- **146 frame pairs go deeper than −20 dB, and they are all simple shapes.**
  89 of them share 1–4 harmonics, 36 share 5–24. The richest are band-limited
  squares and saws, at −27 to −29 dB.
- **60 of 80 tables** leave more than a quarter of the signal behind, averaged
  over matched frames.

**A mistake of mine the null test exposed.** `compare` printed the correlation
to 3 decimals. So 0.9995 read as 1.000, and five tables showed −120 dB
"perfect nulls" that were really −30 dB. It now prints 9 decimals plus the
null in dB. Separately, a run against our own tables found that two pure sines
null to −55 dB. That is why sines are an explicit exemption, not just "about
−30 dB".

**The pack.**

- 80 files, byte-identical to the verified output, in five categories of our
  own: Tones 14, Core 10, Voices 23, Grit 15, Chimes 18.
- The names come from each table's own descriptor: brightness, then
  character, then movement ("Warm Hollow Rise"). Old names and groupings are
  gone.
- MIT, with a `LICENSE` and a `README` in the folder.
- The references were deleted afterwards, so the files are now the canonical
  artifact.
- `wtgen pack` now reports `deepest_null_dB` per table. I checked that on a
  run using the pack's own `Core/` tables as references, where it caught a
  −55.2 dB sine pair.

**Not done:** factory placement in `surge/resources/data/wavetables/` waits on
ADR-0006. Loading a table in a running Surge is still unverified.

---

## 2026-09-21 — ADR-0009: a wavetable generator goal, and a working prototype

Branch `win/adi-surge-wtgen`. Adi added a goal: an **auto/random wavetable
generator inside the plugin**, drivable by the AI, **fully integrated with
adi_daw**. ADR-0009 records it.

**Upstream already has the substrate, and nobody had written it down.**
`WtGenService` runs a per-oscillator Lua wavetable script on a background
worker and publishes the finished table into the oscillator (§4.6).

**Prototype: `tools/wtgen/`.** It is standalone C++ with no dependencies, MIT,
and 1,247 lines. It has two layers:

- `analyze` reduces a table to 41 numbers.
- `generate` makes a new table from those numbers and a seed, and from nothing
  else.

That boundary is the point. It is what lets "make one like X" work without
copying X. It is also the natural thing for an AI to write: a model can emit
41 named numbers, but not 2048-sample frames.

**`wtgen selftest`: 15/15.**

- The analyzer matches theory exactly: a saw reads −6.0206 dB/oct with odd
  ratio 0.7501; a square has odd ratio 1.0; a sine has centroid 1.0.
- A synthetic table round-trips: brightness within 0.15%, odd ratio exact.
- The copy detector separates cleanly: 0.05 for an independent table against
  1.00 for a copy.
- The same seed gives the same table.

**Adi's test: 80 reference tables in, 80 of our own out.** The references are
a local third-party pack of Serum-format tables (2048 samples a frame, 2–35
frames). It is not ours, it is in no repository, and it was used only as
descriptor sources. Output went to a local folder outside this repo.

- **80/80** written, with identical relative paths and frame counts. Each
  carries the `clm ` and `srge` chunks. It took 67 s.
- **78/80** landed within 15% brightness and 0.1 odd ratio at every keypoint.
  The median brightness error is 0.0%, the 90th percentile 0.7%, and the worst
  odd-ratio error 0.020. The two misses (16.6%, 38.3%) are very sparse, narrow
  tables.
- **0 flagged as possible copies.** The highest ripple correlation with any
  reference is 0.836. That is below the null: the same descriptor, generated
  with another seed, reaches 0.909. The median is 0.20.
- **Watch items.** Three tables sit more than 0.3 above their own null, though
  at only 0.13–0.42 in absolute terms, where a copy gives about 1.0. One of
  them is a 35-frame table, whose maximum is taken over 1,225 frame pairs.
- **Waveform correlation reaches 0.999, and only on textbook shapes** (squares,
  saws, sines). A band-limited square is a square. On frames with real
  content, the median is 0.81.
- **The guarantee is structural, not statistical.** `generate()` receives the
  descriptor and a seed, and cannot see a sample. The metrics are secondary
  evidence.

**Four of my own bugs were caught by that verification** before any of this was
reported:

1. **Gaps counted as ripple.** Missing harmonics were measured as roughness,
   and every generated frame hit the 12 dB cap.
2. **An odd/even imbalance counted as ripple.** It inflated the copy metric to
   0.42 for independent tables.
3. **The copy test had no power on near-sines.** Two sines correlate at 0.999;
   ripple over 8–31 harmonics is noise; and the null used fewer frame pairs
   than the test, which biased the flag.
4. **Chord masks were chosen blind to brightness and parity.** An even-rooted
   chord cannot hold 88% odd power at brightness 9.8, and Chrome-like tables
   came out at 40 instead. The fix is a parity-aware chord choice plus
   best-of-4 generation, judged against the descriptor only.

**Not done.**

- No `wtgen` table has been loaded in a running Surge yet. The format is
  checked only structurally, against `WAVFileSupport.cpp:221-261`.
- Nothing is in the plugin: that needs the fork origin (ADR-0006).
- The adi_daw half is undesigned, and ADR-0009 lists its questions.

**Licence:** `wtgen` is original and copies nothing from Surge, so it is MIT,
the default in `OPEN_SOURCE_POLICY.md` §1.

---

## 2026-09-21 — step 4.4: the CLAP loads headless and plays A4 at 440.08 Hz

Branch `win/adi-surge-clap-smoke`. Adi's call was to load Surge in a headless
CLAP host rather than install a DAW.

**Why the host is new rather than one we already had:**

- adi_daw's CLAP host exists only on the unmerged `agent/win-dev` and
  `agent/mac-dev` branches, and another session was editing it at the time.
  Building on another project's unmerged, moving code would couple adi-surge to
  adi_daw, which ADR-0003 says not to assume.
- No other project's tooling was a fit either.

So the host is ours: `tools/clap_smoke/`, with no JUCE and no DAW. It builds
against Surge's own vendored CLAP headers (1.2.7, MIT), so it adds no
dependency. It compiled clean under `/W4 /WX` the first time.

```
clap_smoke: ...\surge_xt_products\Surge XT.clap
  factory[0]: org.surge-synth-team.surge-xt  "Surge XT" 1.4.0
  in  port 0: "Sidechain", 2 ch
  out port 0: "Output", 2 ch
  out port 1: "Scene A", 2 ch
  out port 2: "Scene B", 2 ch
  ok   main output is stereo                    3 output port(s)
  ok   start_processing() accepted
  ok   process() never returned ERROR           0 error block(s)
  ok   no NaN or Inf in the output              155136 frames
  ok   silent before the note                   peak -999.0 dBFS
  ok   sound while the note is held             RMS L -19.9 dBFS, R -19.9 dBFS
  ok   pitch of key 69 is 440 Hz                measured 440.08 Hz
  ok   silent again after the release           tail -419.7 dBFS vs held -19.9 dBFS
  ok   wrote the WAV                            ...\build-clap-smoke\surge-xt-a4.wav
clap_smoke: 9/9 checks passed
```

The measured values are far from the thresholds, so none of these passes is
marginal:

| Check | Measured | Threshold |
|---|---|---|
| Held note | -19.9 dBFS | -40 dBFS |
| Pitch error | 0.02% | 1% |
| Silence before the note | exactly 0 | below -80 dBFS |

Two things the run established that nobody had measured before:

- **Surge's CLAP has 4 audio ports, not 1.** It has 1 stereo input
  ("Sidechain") and 3 stereo outputs. CLAP requires the buffer counts in
  `clap_process` to equal `audio_ports->count()` (`clap/process.h:47-49`), so
  a host must supply all four. I have not tested what Surge does if a host
  supplies fewer. This is now in ARCHITECTURE.md §2.1.
- **A fresh instance plays straight away.** It needs no editor and no patch
  load before sound comes out. The 250 ms before the note is exactly 0, and
  the first note sounds.

`build_clap_win.bat tests` now runs three stages: the Surge build, ctest
(145/145), then `clap_smoke`. It exits 6 if a smoke check fails. It also takes
`ADI_SURGE_SURGE_DIR`, so it runs from a git worktree without re-pointing the
Surge build cache. That is how I ran it: from the worktree, against the real
clone. Afterwards the cache still names the real path.

**Not done:** macOS. The host has a `dlopen` branch, but it has never been
compiled on macOS. It must be given the binary inside the `.clap` bundle. It
is not in the README's "done" definition; making it part of the gate would be
a decision, so it needs an ADR, not a log line.

---

## 2026-09-21 — ADR-0008, and the docs corrected

Branch `win/adi-surge-adr0008`. mac is away for a week. Adi's call: win
proceeds alone, and win writes the ADR-0007 correction.

**ADR-0008** supersedes ADR-0007's *Context*. Its *Decision* stands: CMake ≥ 3.22,
record the version, assert the `.clap`. Two pieces of evidence were added since
the entry below:

- **JUCE 7 has the same floor.** `surge-7.0.12`, the JUCE that upstream's
  "JUCE 7" CI leg swaps in, also demands 3.22 (line 24). So no JUCE Surge
  builds against reopens the silent path.
- **The ordering argument reproduces.** I built a scratch project with the same
  shape: a 3.15 root, then a subdirectory demanding 99.0 added first, then the
  "Skipping CLAP" warning. On CMake 3.31.6 the configure exits 1 and the
  warning never prints.

**Docs corrected** to match that and the six corrections below:

- `ARCHITECTURE.md`:
  - the status line;
  - the §2.1 warning box, rewritten;
  - §2.2's `protocol.file` reason, now marked unverified;
  - §2.3 now points at `build_clap_win.bat` and says `ctest` needs no `-C`;
  - a new §2.6 row for the configure-time LuaJIT trap;
  - §4.4: the gate is 145, the golden test covers one oscillator, the
    citations are fixed, and upstream's `--rerun-failed` leniency is noted;
  - §7: the Windows items are ticked.
- `collab/WIN-ONBOARDING.md`:
  - the `findstr` drift check;
  - step 4.1's CMake claim;
  - the test count;
  - the `/MT`-and-HTTP bullet, which contradicted the prompt's own later
    paragraph;
  - ADR range 0001..0008;
  - the "nobody has compiled" sentence.
- `collab/README.md`: the Building paragraph repeated the silent-no-CLAP claim.

**Claims:** I removed mac's stale `mac/adi-surge-bootstrap` row rather than
work around it. That branch merged as #43, and the row was blocking `collab/**`,
`docs/**` and `ARCHITECTURE.md` for a week with nobody behind it.

**Left out on purpose:** the LTO-on-VS-generator note from the entry below.
It is from source and untested, and ARCHITECTURE.md holds only numbers a
command produced.

---

## 2026-09-21 — the CLAP builds, ctest is 145/145, and six corrections

Branch `win/adi-surge-clap-baseline`. Steps 1 to 3 of the onboarding, plus 4.1
to 4.3. Nothing inside `surge/` was modified: `git -C adi-surge/surge diff
upstream/main --stat` is still empty after a full build, so the build writes
nothing into the source tree.

### Toolchain (ADR-0007: recorded before the first build)

| | |
|---|---|
| CMake | **3.31.6-msvc6**, the copy bundled with VS 17.14.41 (`Common7\IDE\CommonExtensions\Microsoft\CMake`) |
| Ninja | 1.12.1, same place |
| MSVC | 19.44.35229, x64 |
| git | 2.55.0.windows.5 |
| Python | 3.14.7 |
| Machine | 16 threads, 16 GB |

### Surge fetch: identical to mac's

`bash adi-surge/tools/fetch_surge.sh` ran clean on the first try. HEAD
`58914e59c`, 27 submodule entries, 0 drift, 22 gitlinks of 23 `.gitmodules`
declarations, 1.5 GB.

### Build: yes

The documented command, unchanged: Visual Studio 17 2022, x64, Release, LTO on.
It compiled 591 objects from a clean tree with 0 errors, and `surge-xt_CLAP`
took about 3.5 minutes. `surge-testrunner` then took another minute.

**The artifact, not the exit code:**

```
build\surge_xt_products\Surge XT.clap     22,141,952 bytes
file      -> PE32+ executable (DLL), x86-64
dumpbin   -> exports clap_entry; machine 8664 (x64)
          -> no VCRUNTIME*/MSVCP* imports, so the static /MT runtime is real
```

No post-build copy was attempted and no admin rights were needed, as §2.5 said.

`tools/build_clap_win.bat` does this whole sequence and fails with a distinct
exit code for each stage, including 4 = "no `.clap` on disk". Its `tests`
argument also builds and runs the suite. I tested it in place.

### ctest: 145/145, and the gate is 145, not 147

```
cd adi-surge\surge\build && ctest -j 4
100% tests passed, 0 tests failed out of 145        (53 s; 37.6 s on a rerun)
```

**The gate number is 145.** There are 147 `TEST_CASE`s in the source, and two
never reach ctest:

- `Modern Oscillator Perf` is tagged `[.]`, so it is hidden
  (`UnitTestsGOLDEN.cpp:327`).
- `NaN Patch From Issue #1514` sits inside `#if 0` (`UnitTestsDSP.cpp:479`).

Neither depends on the platform, so **mac should also get exactly 145**. If
mac's number differs, something is wrong.

Two things about how ctest runs here:

- **Plain `ctest -j 4` works on the Visual Studio generator, with no `-C
  Release`.** I expected a multi-config generator to need `-C`. It does not
  here, because `catch_discover_tests` runs discovery at build time
  (`POST_BUILD`) and writes absolute executable paths. The documented command
  is right.
- **Upstream CI is more lenient than our gate should be.** `build-pr.yml:140`
  runs `ctest -j 4 || ctest --rerun-failed --output-on-failure`, so a test that
  is flaky and passes on its second try goes green upstream. Our gate is the
  first run.

### One Windows trap, and it only hits agents

The first configure died in `libs/luajitlib`:

```
-- Build script exit code: no such file or directory
CMake Error at libs/luajitlib/CMakeLists.txt:47 (message): Failed to build LuaJIT!
```

**Cause: Claude Code sets `NoDefaultCurrentDirectoryInExePath=1`** in its own
process environment. It is not set at User or Machine level. With it set,
Windows will not look in the current directory for an executable. Surge builds
LuaJIT **at configure time** with
`execute_process(COMMAND build-msvc-luajit.bat)`, launching the script by bare
name, so Windows cannot find it.

I reproduced it with a four-line `cmake -P` probe. A bare `.bat` gives
`no such file or directory` with the variable set and runs fine without it. A
person in a Developer Prompt never hits this, so it is **not** a Surge bug. Any
agent building Surge on Windows will hit it. `build_clap_win.bat` clears the
variable for its own process tree only.

§2 of ARCHITECTURE.md does not mention that LuaJIT is built at configure time.
It is the one step that runs an external script before any target exists.

### What ARCHITECTURE.md, ADR-0007 and the onboarding prompt got wrong

**1. ADR-0007's "silent success with no CLAP" cannot happen in a default build,
or with the documented command.** The conclusion (CMake >= 3.22) is right. The
reason it gives is not.

- `src/CMakeLists.txt:27` runs `add_subdirectory(libs/JUCE)` **before** the
  `VERSION_LESS 3.21` warning at `:35`.
- JUCE's own file says `cmake_minimum_required(VERSION 3.22)`
  (`libs/JUCE/CMakeLists.txt:33`). On CMake below 3.22 that is a **hard
  configure error**, so the configure never reaches the warning.
- The `FATAL_ERROR` keyword in `clap-juce-extensions` adds nothing: CMake has
  ignored that keyword since 2.6, and any unmet minimum is fatal anyway.
- The documented build names `--target surge-xt_CLAP`. **I tested** a missing
  target: `cmake --build ... --target surge-xt_CLAP_does_not_exist` gives
  `MSB1009: Project file does not exist`, rc=1. That is loud.

So "exit 0, no CLAP" needs all three of these at once: a bare
`cmake --build build` with no target, CLAP switched off, and JUCE skipped
(`SURGE_SKIP_JUCE_FOR_RACK`). **Not verified with a real CMake 3.20**, which
would need a download. This is source reading plus the missing-target test.

ADR-0007 is append-only, so the fix is a new ADR that supersedes its Context and
keeps its Decision. Asserting the artifact is still cheap and still right. I
have not reserved a number. mac wrote ADR-0007, so mac should say whether they
want to write the correction or want me to.

**2. The onboarding's Windows drift check can never report drift.**
`findstr /V "^ "` splits the quoted string on the space, so it searches for `^`
alone, which matches every line. I tested it with a `+` (drifted) line and a `-`
(uninitialised) line: it printed nothing and returned rc=1. **`findstr /V /R
/C:"^ "` works.** My "0 drift" above came from `grep -vc '^ '` in Git Bash,
not from this command. The file is `collab/WIN-ONBOARDING.md`, under mac's
`collab/**` claim, so I have not edited it.

**3. "147 TEST_CASEs / 409 SECTIONs" (§4.4).** The regression gate is 145 (see
above). I could not reproduce 409 SECTIONs: I count 355 `SECTION(` plus 46
`DYNAMIC_SECTION(`, which is 401. The SECTION count does not affect the gate,
because ctest counts `TEST_CASE`s.

**4. The "golden numeric regression harness" (§4.4) covers one oscillator.**
It is a single test case, `Modern Oscillator Golden`
(`UnitTestsGOLDEN.cpp:313`, tolerance `GOLDEN_TOL = 1e-5f` at `:60`). The
other 11 oscillators, the filters and the FX have no golden-value test. The
cited lines `:41-45` are `#include`s. This matters for how much we trust the
gate: it catches crashes and gross misbehaviour broadly, but numeric drift in
only one oscillator.

**5. The stream round-trip citation (§4.4).** `UnitTestsIO.cpp:568` is
`All Patches Are Loadable`. The DAW round-trip is `:588` alone.

**6. The reason given for the `protocol.file.allow` trap (§2.2, ADR-0004) is
unverified.** It says git "tries the superproject's object store first". Every
URL in every `.gitmodules`, nested ones included, is https. I did not run the
submodule update without the flag, so I have neither confirmed nor refuted
this. The fix is right either way.

**From source, not tested:** on the VS generator, LTO is decided by
`CMAKE_BUILD_TYPE` at configure time (`CMakeLists.txt:96`), not by `--config`.
`:9-11` forces `Release` when no build type is given. So `--config Debug` on a
tree configured the default way should still have IPO on. Use
`-DENABLE_LTO=OFF`.

**Checked and correct:** everything else I checked matched the source:

- the 3.22 / 3.21 minimums
- JUCE 8.0.12, C++20 at `:80`
- `/WX` at `:204-208` and the suppression list after it
- `/MT` via CMP0091, which the binary confirms
- `SURGE_COPY_AFTER_BUILD` OFF at `src/CMakeLists.txt:8`
- `ClapTargetHelpers.cmake:171-189` is Darwin/Linux only
- 641 factory + 2920 third-party patches, 3797 `.fxp` in total

### Housekeeping

- **mac's claims row for `mac/adi-surge-bootstrap` is stale.** PR #43 merged on
  2026-09-20 and the row still claims `collab/**`, `docs/**` and
  `ARCHITECTURE.md`. I added my own row anyway, since that branch is merged.
  mac, please remove yours.
- On this machine the monorepo checkout is shared with an adi_daw session that
  switches branches under me. I commit adi-surge work from a separate git
  worktree so I never move that session's HEAD.

### Not done yet

- **Step 4.4 (load it in a host) is blocked: there is no CLAP host on this
  machine.** There is no Reaper, Bitwig or FL. That needs Adi.
- The OSC address uniqueness sweep has not been started.
- No ADR written. Item 1 above needs one; who writes it is open.
