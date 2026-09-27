# mac (analyser session) — log

macOS · Apple clang · arm64 · Claude Opus 5.

The **second session on the Mac**, on the Pd spectrum analyser (ADR-0116,
ADR-0183). It commits as `mac`, because two sessions on one machine are one
agent, and it keeps to its own branch, its own worktree and this file so that
the mission session's `collab/mac.md` and its open PR are never touched.
Newest entry at the top.

---

## 2026-09-27 — round 4 part B: no Pd external loads from disk, and neither of the two ways to stop it works

win's round-4 brief (`collab/prompts/2026-09-27-mac-round4.md`), part B. #138 is
rebased on main at `5295c79` and both new rules are implemented rather than
noted. ADR-0183 gains d18–d23.

**Rebased.** The conflicts were the four win predicted: `DECISIONS.md` (ADR-0183
appended after ADR-0190 — the log is append-only and numbers need not be in
order), the README count (188 on main, 189 with this), the 0183 row (`reserved`
to `used`, now that #136 has merged), and the collab logs. `validate_schema.py`
passes, checks 7 and 8 included.

**ADR-0188 d8, and the finding that matters: neither mechanism in the brief
works.** Both were read in Pd 0.56's `s_loader.c` at the pinned commit.

- **A registered loader cannot refuse.** `sys_register_loader` APPENDS, and
  `sys_do_load_lib` is the list's static head. Anything the host registers runs
  only after the default has already searched every path and loaded what it
  found. There is no hook that runs first.
- **libpd without dynamic loading does not cover Windows.** `HAVE_LIBDL` gates
  the `dlopen` branch only; the `#ifdef _WIN32` branch above it calls
  `LoadLibrary` whatever `HAVE_LIBDL` says. A guard that holds on two platforms
  of three is the kind that looks done.

**So the guarantee is made where it can be made portably: nothing loadable is
ever on a path Pd will search.** Before `libpd_openfile`, `LibPdEngine` refuses
the patch's own directory and every search path it was given if either holds a
file with an extension Pd would try, and it refuses a patch whose text carries
`[declare -lib]`, `-stdlib`, `-path` or `-stdpath` — the one object that
defeats the discipline from inside the patch, found by the same static parse the
declarations use, with Pd not running.

**The danger is measured, not argued.** With `adi_probe_external.pd_darwin`
dropped beside a patch that names that object, **Pd called `dlopen` on it** —
the console carries the path and dlopen's reply, "slice is not valid mach-o
file". The only reason nothing ran is that the planted bytes are not a library.
That test drives raw libpd deliberately *around* the engine, because what it
proves is that the refusal is refusing something that would otherwise happen.

**Abstractions are untouched, and that is Pd's ordering, not luck.**
`sys_loadlib_iter` runs every loader first and calls `sys_do_load_abs` only
when they have all failed. So `adi.array.pd` and `adi.param.pd` keep resolving
through exactly the paths whose externals are refused — asserted, not assumed.

**ADR-0188 d3, and a second finding. `libpd_float` cannot be used on the audio
thread, twice over.** `libpd_dofloat` is `gensym(name)->s_thing` inside a
`sys_lock()`: a mutex, and for a name Pd has not seen an allocation — which
happens even in the failing case, because the symbol is created before the null
`s_thing` is noticed. **win, this is why your d3 sentence "only to receive names
the patch declared" is load-bearing** rather than tidy: sending by name from the
audio thread is a dropout waiting for the first unrecognised parameter.

So `bindParameters` resolves each declared parameter's `t_symbol*` once on the
message thread and `sendParameter` reads `s_thing` from the audio thread — no
`gensym`, no lock. Cheaper *and* safer than the by-name call, not a trade. A
value sent that way comes back out of Pd through a published array and follows
when it changes.

**ADR-0177 d5's vanilla-Pd promise, measured.** `adi.param.pd` does not exist
yet, so `tests/pd/adi-param-proof.pd` is exactly d5's case: the `[adi.param]`
box does not create, Pd says so, **the patch opens anyway**, that parameter is
silent, and the declaration is still readable text. All four asserted.

**Pd reports almost everything only to its console,** so `LibPdEngine` now keeps
one, per instance — the hooks live in the instance under PDINSTANCE and `$0`
cannot tell two apart. Without it the engine's only report is the return value
of `open`, and a patch that opened missing half its objects looks exactly like
one that opened whole.

**Transport: a follow-up, and the analyser does need it.** The DJ-style scope's
whole behaviour is a waveform scrolling locked to the bar, and the Max for Live
build syncs that scroll to the host tempo. It is not built here because the
abstraction sits beside `adi.param.pd`, which is win's and unwritten, and the
host half is d21's audio-thread send with a different payload — building it
against an abstraction that does not exist would be building half of it twice.
The field list is recorded in ADR-0183 d23 so the follow-up starts from it.
**d3's range rule is not this engine's**: it is the device layer's, where the
parameter rows and the op live.

**Names:** nothing this session added says "ADI DAW" or "ADI Live"; the old
names in `DECISIONS.md` are in historic entries, which ADR-0190 keeps.
`adi-m4l-analyzer` keeps its own, as the brief says.

**Verified:** build clean, **50 of 50 suites**, `adi_pd_engine_tests` 74 checks
and `adi_pd_declaration_tests` 76. Every new guard was planted and confirmed to
fail its test when removed — the directory scan, the search-path scan, the
`[declare]` refusal, the print hook, and `sendParameter`'s send.

---

## 2026-09-27 — renumbered to ADR-0183, moved off the mission session's branch, rebased onto main

win's six corrections, all applied. Nothing had been pushed, so all of it was
cheap.

**1. ADR-0179 → ADR-0183.** The reservation table decides (ADR-0051), and 0179
was held for the mission session's CLAP host contract before this session spent
it. Renamed in the `DECISIONS.md` heading and in every reference across the
four commits — including one written `ADR-0177/0179`, which a search for
`ADR-0179` alone would have left behind. `collab/README.md` carries the 0183
row as `used`; 0179's row is untouched. `adi_daw/README.md` now says 182
entries. `validate_schema.py` passes, checks 7 and 8 included.

Note for whoever merges second: the 0183 row is added here as `used`, and win's
open PR #136 adds the same row as `reserved`. They will conflict on that one
line and the resolution is `used` — check 8 fails otherwise, because ADR-0183
is in `DECISIONS.md`.

**2. Its own worktree.** `Adi-wt/pd-analyser` on `mac/pd-analyser-wip`.
`git branch --show-current` before every commit from here on.

**3. Rebased onto main.** The four commits sat on top of the mission session's
`e07b8a6` and `39b5aee`; `git rebase --onto origin/main e07b8a6` moved them off
and they now sit on `76ddcc9`. Every `collab/mac.md` hunk was dropped in the
rebase and moved here instead — that file is the mission session's.

**The local `mac/ci-render` is left alone.** `origin/mac/ci-render` is
`e07b8a6`, which is exactly what PR #134 shows, so nothing of this session's is
on the pushed branch or in that PR. The local branch is still four commits
ahead, and all four are this session's; resetting it to `origin/mac/ci-render`
by name clears it, as win already told the mission session. That is theirs to
do, not this session's.

**Checked against ADR-0177, which merged while this ran.** It governs
unchanged, and `DECISIONS.md` now says so in its own section rather than
leaving two documents to be compared:
- nothing in `[adi.param]`'s grammar is amended;
- `[adi.array]` is a sibling with its own grammar, its own id space and its own
  receive stem (`-adiarr-` against `-adi-`), so a patch cannot address one
  where the other is meant and have it half work;
- **ADR-0177 shipped no code** — nothing on main reads or writes an
  `[adi.param]` — so `parsePdDeclarations()` is the first implementation of its
  grammar as well as the array's, which is the argument for one scanner rather
  than two;
- `adi.param.pd` (d5) and `tools/gen_pd_patches.py` (d6) stay win's;
- d15 applies to `[adi.param]` too: `$0` does not expand in a message box;
- the one real risk to ADR-0177's grammar is d11, and it is flagged, not taken.

**4. libpd recorded under ADR-0024.** `docs/EXTERNAL-CODE.md` gained the row it
was missing: **0.16.1 / `ba0dc63262901d658af8bbda5e619a60fa975e78`**, its
`pure-data` submodule at **`f009fd8d7b537e209e09898d487fdf1bf547da2b`** (Pd
0.56-5), fetched by `--build-only`, built `PD_MULTI` and static. Licences read
at the pinned commits, not off GitHub's labels: both `LICENSE.txt` files are
the Standard Improved BSD License, and `pure-data/src`, `pure-data/extra`,
`libpd_wrapper` and `cpp` contain no GPL or LGPL text at all — so nothing
compiled from this dependency is stronger than BSD-3, as ADR-0035 requires.
`third_party/` stays git-ignored and no libpd source is tracked.

**Verified after the rebase, not before it:** configure and build clean,
**50 of 50 suites pass**, `adi_pd_declaration_tests` 66 checks and
`adi_pd_engine_tests` 41. The M4L project's own `test/run_all.sh` passes too,
and its generators are deterministic — a full regeneration left every tracked
byte identical.

---

## 2026-09-27 — [adi.array] built as [adi.param]'s sibling; the M4L analyser imported

**[adi.array] (ADR-0183 d13–d17).** ADR-0177 is approved, so this is its
sibling, built to the instruction that the two move as identical siblings:
ONE scanner, two schemas. One tokeniser, one `$0` check, one id rule, one
problem list. `[adi.param]` is implemented here too rather than left for a
second parser to get subtly different later — win, the grammar is yours and
this follows it exactly.

    [adi.array $0 <id> <length> <rate> <min> <max> <unit> <name>]

min/max/unit are ADR-0177's fix 2 applied, not an addition: a renderer given
only a length and a rate still guesses the value range, and a guess about a dB
floor draws a picture that is wrong in a way nobody can see. Ids are a SEPARATE
space from parameters', because a parameter's id is `plugin_params.param_id`
and an array is never automated — one space would make adding a display change
what a lane points at.

The parse is static and includes no libpd at all: **all 66 declaration checks
run with Pd absent**, which is the property ADR-0177's fix 3 rests on.

**Four traps, all now in the ADR:**

1. **`$0` does not expand in a MESSAGE box**, only an object box. A message box
   `; $0-adiarr-1 0 0.5` targets `0-adiarr-1`; Pd says "no such object", the
   patch loads anyway and the array stays at zero. The `$0` has to live in
   `[s $0-adiarr-1]`. **`tools/gen_pd_patches.py` needs this before it emits a
   declaration**, and it applies to `[adi.param]` identically.
2. **Under PDINSTANCE the search path is per instance**, not process-wide. A
   path added before the instance exists goes nowhere, and the only symptom is
   the abstraction failing to create.
3. The double buffer's slot must come from ONE expression — writer and reader
   derived it separately, disagreed, and every read returned the previous
   array: a display one frame behind for ever, with nothing to show for it.
4. `#X obj <x> <y> <class>` puts the class at atom 4. Reading atom 3 gets the y
   coordinate and makes every declaration invisible.

`pd/adi.array.pd` ships as a vanilla MIT abstraction holding
`[table $1-adiarr-$2 $3]`. **`adi.param.pd` is still win's to write**; the
parser accepts its declarations today.

**The Max for Live analyser is imported** at `adi-m4l-analyzer/`, as its own
project beside adi-surge and adi-vital, with `PROJECT.md` separating what is
worth porting from what is not. History stays in the `AVC-Spectrum-Meter` repo.
The JS drawing layer is throwaway; the measurement is not, and `PROJECT.md`
tabulates it against the tests that hold it — coherent-gain calibration, the
4.5 dB/oct slope, bx-matched RMS, the goniometer's one-sample pairing, the
period detector's octave-error rule. The findings section is there so the same
days are not spent twice: `jit.gen` resampling mismatched dims, Max `expr`
having no ternary and no working exponent notation, undefined fan-out order,
and **Live's CPU meter measuring audio processing and excluding interface
redrawing** — two costs conflated for days until a macOS profile put
`jsui_paint` at 52% of the process with the device's MSP at zero samples.

---

## 2026-09-27 — libpd is in, running, and proved; three findings, one of them win's to settle

ADR-0183 decisions 9–12 are what building it found; all three are asserted in
`tests/test_pd_engine.cpp`.

**What landed.** libpd pinned at 0.16.1 (`ba0dc6326`, BSD-3-Clause) with its
pure-data submodule, built static, linked into `adi_core` so the Pd tier tests
on every ABI rather than in one JUCE job. `LibPdEngine` implements the
`PdPatchEngine` the contract left pure virtual. Audio in one end, out the
other, through Pd's own DSP.

**PD_MULTI is not optional.** Plain libpd is one instance per process: every
patch shares one DSP graph, one sample rate, one block size and one
`libpd_process_float`. A device has to be processed when the graph reaches its
track, at that node's block size, with its own latency.

**Three findings:**

1. **`$0` is per INSTANCE, not per process.** `canvas_getdollarzero()` is
   per-instance state, so two devices opening the same patch both get 1003 and
   both answer on `1003-report_latency`. A table keyed on `$0` cannot tell them
   apart — ADR-0095 d1 assumed otherwise. It is per instance now and the float
   hook asks `libpd_this_instance()`. Inside an instance ADR-0095's reasoning
   is untouched.

2. **The `[loadbang]` latency report cannot be received, by anyone.** The name
   needs `$0`, `$0` comes from the opened patch, `[loadbang]` fires inside
   `libpd_openfile`. ADR-0095 d2's query after `prepare` is therefore not a
   correction for a value sent at the wrong sample rate — it is the only report
   a host ever gets, which makes it load-bearing rather than belt-and-braces.

3. **The patches in `pd/` are abstractions and libpd cannot render one.**
   `inlet~`/`outlet~`, no `adc~`/`dac~`. `libpd_openfile` opens a top-level
   canvas where `inlet~` connects to nothing: `adi-rmsc.pd` opens, Pd renders
   its blocks, and every output sample is zero with no error anywhere to say
   why. **This one is win's** — `tools/gen_pd_patches.py` and ADR-0096 own
   those patches. Three ways out and one is a trap: a per-device wrapper
   instantiating the abstraction between `adc~` and `dac~` breaks the latency
   protocol, because the abstraction's `$0` is not the wrapper's and
   `libpd_getdollarzero` returns the wrapper's. Top-level device patches keep
   the protocol intact.

**Contract change, small:** `PdPatchEngine::adapterLatencySamples()`, defaulting
to 0 so the contract's own fakes are unaffected, added to
`PdDevice::latencySamples()`. `NodeIo` carries a segment and not a block, so an
event at frame 100 splits 512 into 100 and 412 and the engine practically never
sees a multiple of 64; the adapter between them costs one Pd block. A patch
reporting 0 through an engine that delays 64 is the misalignment ADR-0058
exists to remove.

---

## 2026-09-27 — the analyser: ADR-0183, four rulings, and two prerequisites that are not the analyser

**The director's instruction:** build the flagship analyser — the Max for Live
spectrum meter ported to a Pd DSP tier with a C++ UI, big-window mode, an EXO
tab cloned from sMexoscope, and a VISU-grade spectrogram. Five repositories
named to clone.

**Nothing was built.** Four questions went to him first and he ruled on all
four; ADR-0183 records them. The reason nothing was built is worth repeating:

- **libpd was not vendored** and was not in `CMakeLists.txt`. `pd_device.hpp`
  said so itself. The patches in `pd/` are artefacts of ADR-0035's direction;
  nothing opened them.
- **Published arrays did not exist.** ADR-0116 d2 says the contract gains them;
  no spelling of `publishedArray` was in `src/`.

So the analyser had two prerequisites and neither was the analyser. ADR-0183 d8
says it does not lead.

**His four rulings:**
1. OpenGL approved for the analyser now, overriding ADR-0050's step-7 deferral.
   The deferral still stands for `ArrangementCanvas`.
2. One clock. Rendering locked to `VBlankAttachment`; the 120 Hz timer refused.
3. The EXO tab uses sMexoscope's own buffer, not `scope.hpp`'s tap — against
   the recommendation, and for the better reason: the tab's value is being a
   behavioural clone, and the behaviour comes from the buffer. Audio-to-UI
   paths go to three and ADR-0183 d5 names all three so they stay counted.
4. AGPL-3.0 accepted at project level. PerceptoMap is AGPL; the binary already
   carries AGPLv3 through JUCE's grant, so the GPLv3 boundary being protected
   was not there. `LICENSE` says GPL-3.0 and now understates it — correcting it
   is part of this work.

**Licences, checked before anything was cloned:** sMexoscope GPL-3.0,
PerceptoMap AGPL-3.0, libtfr `GPL-2.0-or-later` — that last one reads as
"GPL-2.0" on GitHub's label and would have looked like a hard blocker; its
headers carry the `-or-later`. tfr and juce-spectroscope19 are reference-only
in the brief and were not checked.

---
