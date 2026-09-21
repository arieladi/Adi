# adi-surge — Decision log

Append-only. Newest at the bottom. Superseding an earlier decision means writing
a **new** entry that says so — never edit an old one.

`ARCHITECTURE.md` explains how the system works. This file records *what was
decided and why*, so neither agent re-litigates a settled question or silently
reverses one.

Format: **ADR-NNNN — title**, then Context / Decision / Consequences.

**This project has its own ADR namespace.** It starts at 0001 and does not
continue `adi-vst`'s or `adi_daw`'s. Citations to theirs are always written in
full — "adi-vst ADR-0017", "adi_daw ADR-0052" — because a bare number here means
one of ours.

---

## ADR-0001 — Why a second AI synth, and why Surge XT

**Date:** 2026-09-20 · **Agent:** mac · **Directed by:** Adi

**Context.** `adi-vst` is a fork of Vital with an AI "describe a sound, get a
patch" feature being built into it. It works, the toolchain is verified, and it
ships a VST3. The obvious question is why build the same feature a second time
rather than finishing the first.

Three reasons, in order of how much they actually matter:

1. **Format coverage.** `adi-vst` ships VST3 only, and that is now a property of
   the repo rather than an accident — adi-vst ADR-0017 dropped CLAP there
   because JUCE 6.0.5 predates CLAP entirely, there is no `buildCLAP` flag in
   either `.jucer`, and adding one would mean a second build system against
   adi-vst ADR-0002. Surge XT is natively CLAP: `surge-xt_CLAP` is a first-class
   CMake target in upstream's own CI. The format we could not reach there is
   free here.

2. **Surge ships the machine-facing surface Vital does not.** `adi-vst` had to
   hand-build a headless VST3 validator (its ADR-0009) and a Python schema
   extractor that parses C++ parameter tables, because Vital exposes nothing.
   Surge ships OSC in and out with an address per parameter, `surgepy` Python
   bindings, a headless CLI, and a `ctest` suite. See ADR-0005.

3. **Two synths is a better product than one**, and the second one is cheap
   precisely because the AI design — sparse patch, merge not reset, undo before
   apply — was already worked out next door and is format-independent.

**Decision.** `adi-surge/` is a new project directory in the `arieladi/Adi`
monorepo, a fork of `surge-synthesizer/surge`, targeting CLAP, reusing
`adi-vst`'s AI design where it transfers and departing from it where Surge's
architecture is genuinely different.

**Consequences.** Two forks to keep current instead of one. That cost is real
and is not symmetric: Vital is effectively unmaintained upstream, so `adi-vst`'s
fork delta only grows from our side, whereas Surge is actively developed and we
will want to pull upstream regularly. Keeping our delta small is therefore a
harder constraint here than there, and it is written into `collab/README.md` as
a ground rule rather than left as good intentions.

This decision does **not** couple to `adi_daw`. Integration with the DAW is
intended but undesigned; see ADR-0003's last paragraph.

---

## ADR-0002 — `surge/` is a fork tracked against upstream, not a vendored copy

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge XT's working tree is ~1.5 GB with all 22 submodules checked
out; the repository alone is 1.1 GB. It has to live somewhere, and the two
options are vendoring it into the monorepo or keeping it as its own repository
that the monorepo ignores.

`adi-vst` already answered this question for Vital (its ADR-0001) and the
reasoning transfers without modification: vendoring loses `git diff
upstream/main`, which is the only command that says what we have actually
forked.

**Decision.** `adi-surge/surge/` is its own git repository with a single remote
named **`upstream`** pointing at `https://github.com/surge-synthesizer/surge.git`.
The parent `Adi` repo gitignores `adi-surge/surge/`. Cloned at
`58914e59c` on `main` (Surge XT 1.4.0).

The clone is deliberately **not shallow**. A shallow clone cannot answer
`git diff upstream/main`, which is the entire point of the arrangement.

**Consequences.** The whole fork delta is one command:

```bash
git -C adi-surge/surge diff upstream/main --stat
```

Cost: the fork is not visible to anyone who only has the monorepo, so each agent
clones it themselves. Unlike adi-vst ADR-0011, **this does not block anybody**,
because the upstream is public and cloneable by both agents today. See ADR-0006.

`tools/fetch_surge.sh` performs the clone and the submodule init. Nothing about
this arrangement should be discovered by hand.

---

## ADR-0003 — CLAP is the primary target; VST3 and standalone are secondary

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge XT builds CLAP, VST3, AU, LV2 and standalone from one CMake
tree. Building all of them on every change is slower than building one, and
"which one do we actually validate against" needs an answer before anyone writes
a build script.

The project exists in part to cover the format `adi-vst` could not
(ADR-0001), and CLAP is the format `adi_daw` has mandated hosting for
(adi_daw ADR-0052) specifically because of `CLAP_EVENT_PARAM_MOD` —
non-destructive parameter modulation, where a host-side modulator rides on top
of a parameter without overwriting the value the user dialled in.

**Decision.** `surge-xt_CLAP` is the primary build and validation target for
this project. VST3, AU, LV2 and standalone continue to build because they are
upstream's targets and we are not removing them, but they are not what a branch
is judged on. `collab/README.md`'s "what done means" names the CLAP target and
`ctest`, nothing else.

**Consequences.** This is the mirror image of adi-vst ADR-0007 + ADR-0017, which
settled on VST3-only over there. The two projects together therefore cover both
formats, which was the point.

**Scope.** This decides what *this plugin exports*. It says nothing about what
`adi_daw` *hosts*. adi_daw ADR-0052 mandates CLAP hosting and its own
ADR-0041 reasons that "hosting is not identity"; exporting a format and hosting
one are unrelated capabilities. **There is no decision yet about how `adi-surge`
and `adi_daw` integrate**, and nothing in this repo should assume one. When that
is designed it is a new ADR, here and probably there too.

---

## ADR-0004 — Submodules are Surge's own gitlinks, and the `protocol.file.allow` trap

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge vendors its dependencies as git submodules — JUCE, the whole
`sst-*` family, LuaJIT, zstd, simde, PEGTL, fmt, pybind11,
`clap-juce-extensions` and more. `adi_daw` solved the equivalent problem with
`tools/fetch_external.sh`, which pins every external repo **by tag and by the
commit that tag pointed at**, and fails loudly if upstream re-points a tag
(adi_daw ADR-0024).

That machinery is not needed here, and it is worth writing down why rather than
copying it out of habit: `adi_daw`'s externals are independent clones, so "what
did we build against" is only answerable if we record it ourselves. Surge's are
**gitlinks in Surge's own tree**. Git checks out exactly the commit the
superproject names and `git submodule status` prefixes any drift with `+`. The
pin, the record and the verification are all already there.

**Two things were discovered the hard way and are absorbed by
`tools/fetch_surge.sh`:**

1. **`git submodule update --init --recursive` fails on a fresh clone** with
   `fatal: transport 'file' not allowed`, even though every URL in `.gitmodules`
   is `https`. Git ≥ 2.38 refuses the local `file://` transport by default
   (CVE-2022-39253) and git's submodule clone path tries the superproject's
   object store first. Verified on git 2.50.1 (Apple Git-155). The fix is
   `-c protocol.file.allow=always` **on that one command**.

2. **`.gitmodules` declares 23 submodules; only 22 are gitlinks.**
   `src/surge-rs/surge-rs` is a stale declaration with no gitlink behind it, so
   git never fetches it and `git submodule status` never mentions it. That is
   upstream's state, not a broken checkout. Nothing in the CMake build
   references it.

**Decision.** Do not pin or vendor Surge's submodules ourselves. Fetch them with
`bash adi-surge/tools/fetch_surge.sh`, which applies the `protocol.file.allow`
flag scoped to the single command that needs it.

**Do not set `protocol.file.allow=always` globally.** It is a global weakening
of a real mitigation for a problem that is local to this clone.

**Consequences.** Upgrading Surge is `git -C adi-surge/surge fetch upstream` and
a merge; the submodule pins come along with it, because they are part of the
commit. Verified: all 27 checked-out entries (22 top-level + 5 nested) report
clean, with no `+` drift markers.

---

## ADR-0005 — The AI write path is `surgepy` first, then a C++ overlay. OSC is scaffolding, not the product.

**Date:** 2026-09-21 · **Agent:** mac

**Context.** Three candidate write paths, and the project's shape depends on
picking right:

- **(a)** a C++ overlay inside the plugin, as `adi-vst` is building for Vital;
- **(b)** driving Surge over its existing OSC surface with zero C++ changes;
- **(c)** headless `surgepy`, plugin unchanged, for offline generation.

The attractive answer was (b) — Surge ships a complete OSC control plane, and I
verified it is more viable than it first looked: OSC can be switched on for a
plugin instance **from the DAW state blob alone**, with no code change
(`SurgeSynthProcessor.cpp:1545-1554`, `:1629-1644`). Discrete selectors are
writable too, because the receiver rescales `vt_int` through
`intScaledToFloat`.

**The fact that decides it is none of those, and it is the polymorphism in
§3.1.** `fxN_pM` and `a_oscN_paramM` have no fixed meaning: their ctrltype,
valtype, min, max, default, display name and even whether they accept a string
all change when the parent `fxN_type` / `a_oscN_type` changes. FX params are
created as `ct_none` / `"Param 1"` (`SurgePatch.cpp:118-124`) and retyped by
each effect's `init_ctrltypes()`.

So **no path can work from a hand-written schema.** All three need a *generated*
per-type parameter table covering 32 FX types × 16 slots and 12 oscillator
types. **Only `surgepy` can generate that table**, and
`scripts/misc/surgepy-params.py` is already a working demonstration of exactly
it. That collapses the choice: (c) is not an alternative to (a), it is a
**precondition** for it.

**Decision.**

1. **(c) first.** Build `surgepy`, generate the contextual per-type schema,
   and use its offline render to audition and score generated patches. This is
   the prerequisite artifact and it is reusable unchanged by everything after.
2. **(a) second**, once there is a schema and a fork `origin` to push to. The
   in-plugin apply seam is `enqueuePatchForLoad` ("safe from any thread",
   `SurgeSynthesizer.h:433`) plus the `setPatchFromUndo` pattern, or in-process
   `setParameter01` keyed on `get_storage_name()`.
3. **(b) is not the product path.** It is legitimate as a throwaway debugging
   tap and a no-build demo while (a) is being written. Treat it as scaffolding
   with a known demolition date, and **do not let its address table leak into
   the patch schema.**

**Why (b) does not carry forward — the key spaces differ.** (a) resolves names
in-process and uses `get_storage_name()` (`a_osc1_pitch`), which is literally the
XML element name the `.fxp` writer emits (`SurgePatch.cpp:4086`). (b) requires
the curated OSC taxonomy (`a/osc/1/pitch`). These are two independent,
hand-maintained namespaces (§3.1) — build the AI's key space on the OSC one and
you must port it wholesale later, having carried an address table with
unverified uniqueness that (a) never needed.

(b)'s other disqualifiers as a *product* path are in §4.1: no atomicity (N
parameters is N lossy UDP datagrams, and `setParameter01` has order-dependent
side effects), no `"440 Hz"` string setting over the wire, no instance identity
with a fixed out-port, and no in-plugin UX at all.

**Two costs removed by checking, which shift the balance toward (a) as the
destination:**

- **(a) needs no new HTTP dependency.** `juce_core/network/` ships `juce_URL`
  and `juce_WebInputStream`. That also dissolves the MSVC `/MT` `LNK2038` hazard
  for this feature specifically (§2.6).
- **The macOS blocker on `surgepy` is illusory.** `setup.py`'s
  `-DSURGE_SKIP_STANDALONE=TRUE` never reaches the broken `if(APPLE)` block,
  because `-DSURGE_SKIP_JUCE_FOR_RACK=TRUE` gates `surge-xt` out at
  `src/CMakeLists.txt:171`.

**Consequences.**

**Sequencing constraint that overrides the above:** publish the fork `origin`
first. Both (a) and the one-line `surgepy` binding addition that (c) wants are
C++ changes to `adi-surge/surge`, which has no `origin` today — so `win` has
nowhere to push. `adi-vst` hit this exact blockage and resolved it in their
ADR-0016; re-learning it here is the avoidable mistake. See ADR-0006.

**One thing this ADR does not settle, deliberately.** Whether the ~766 OSC
addresses are **unique** is unasserted, untested, and not statically decidable
(they are `fmt::format` calls inside loops). It does not block this decision,
because (b) is not the product path — but it must be resolved by a runtime
`/q/all_params` sweep before *any* OSC tooling is trusted, including a
throwaway one.

Two further `surgepy` limitations to design around: it does not expose `oscName`
(a one-line binding addition, which needs the fork), and its `constants` module
is **stale** — 8 of 12 oscillator types, missing exactly the interesting ones
(Modern, String, Twist, Alias). Mirror the integers from the C++ enums; never
use `surgepy.constants`.

---

## ADR-0006 — The fork has no `origin` yet, and that does not block anyone

**Date:** 2026-09-20 · **Agent:** mac

**Context.** `adi-surge/surge` currently has exactly one remote, `upstream`,
pointing at `surge-synthesizer/surge`. There is nowhere to push our own commits.

adi-vst hit precisely this and it cost them: its ADR-0011 left the fork with no
`origin`, which meant the `mac` agent could not clone the C++ at all and was
limited to tooling and review until adi-vst ADR-0016 resolved it by creating
`arieladi/adi-vst-synth`. That was a real blockage, caused by the Vital fork
already carrying local modifications that existed nowhere public.

**The situation here is different in the way that matters.** Our clone is
currently byte-identical to `upstream/main` — we have not modified Surge at all
yet. So either agent can obtain the identical tree today, from a public URL,
with `tools/fetch_surge.sh`. Nobody is blocked.

**Decision.** Defer creating a fork origin until there is a fork delta to push.
Until then `tools/fetch_surge.sh` clones from `upstream` and both agents work
from identical trees.

**The moment the first commit lands in `surge/` that is not upstream's, this
becomes blocking and must be resolved in the same session** — do not repeat
adi-vst ADR-0011 by discovering it later. The resolution, when wanted, mirrors
adi-vst ADR-0016: a public GitHub repository under `arieladi`, added as
`origin`, GPLv3. **Creating it is Adi's call**, not an agent's, because it is his
GitHub account.

**Consequences.** Any local commit in `surge/` is unbacked up and invisible to
the other agent until an `origin` exists. Treat `surge/` as read-only until then,
and put work that does not strictly need to live inside Surge's tree — tooling,
schema extraction, the OSC harness — under `adi-surge/tools/`, which is in the
monorepo and therefore already shared.

---

## ADR-0007 — CMake ≥ 3.22 is a hard requirement, and a build must assert the artifact

**Date:** 2026-09-20 · **Agent:** mac

**Context.** Surge's root `CMakeLists.txt:2` says
`cmake_minimum_required(VERSION 3.15)`. That number is wrong in a way that
specifically damages this project.

`src/CMakeLists.txt:35-41` reads:

```cmake
if(${CMAKE_VERSION} VERSION_LESS 3.21)
  message(WARNING "CMake version is lower than 3.21. Skipping CLAP builds. ...")
  set(SURGE_BUILD_CLAP FALSE)
endif()
```

It is a **`WARNING`, not a `FATAL_ERROR`**. On CMake 3.15–3.20 the configure
succeeds, the build succeeds, the exit code is 0, and **there is no CLAP**. For
a project whose primary and only mandated target is the CLAP (ADR-0003), that is
a silent total failure disguised as a green build. It would be found at plugin
scan time, in a DAW, long after the build that caused it.

The true floor is higher than 3.21 anyway: `libs/JUCE/CMakeLists.txt:33` requires
**3.22**, and `libs/clap-juce-extensions/CMakeLists.txt:13` requires 3.21 as a
`FATAL_ERROR`.

**Decision.** Three things, all of them cheap:

1. **CMake ≥ 3.22 is a hard requirement for `adi-surge`**, stated in
   `ARCHITECTURE.md` §2.1 and in the Windows onboarding prompt. The 3.15 in
   Surge's root file is not to be trusted or quoted.
2. **Both agents record their CMake version in their log before the first
   build.** On Windows this is not a formality: CMake ships *inside* the Visual
   Studio install rather than on `PATH`, so the version is whatever VS bundled
   and has to be looked up rather than assumed.
3. **A build is not "successful" until `surge-xt_CLAP` exists on disk.** Any CI
   or build script we write asserts the artifact, never the exit code.

**Consequences.** Point 3 is the one that generalises, and it is the same
principle `adi_daw`'s CI header states — a green tick should prove a specific
claim rather than that something compiled. Here the specific claim is "a CLAP
came out", and it is precisely the claim the exit code does not make.

This is also a reminder that `cmake_minimum_required` in a vendored tree is a
floor for *that file*, not for the build. Check what the submodules demand.

---

## ADR-0008 — ADR-0007's context is wrong: old CMake fails loudly. Its decision stands.

**Date:** 2026-09-21 · **Agent:** win · **Supersedes:** the *Context* of
ADR-0007, not its *Decision*

**Context.** ADR-0007 says that on CMake 3.15–3.20, "the configure succeeds,
the build succeeds, the exit code is 0, and **there is no CLAP**". It was
written from source, before anyone had built. The first Windows build tested
the claim, and it does not hold. **Nothing in Surge's CMake produces the
silent no-CLAP outcome in any configuration that could produce a CLAP.** There
are two reasons.

**1. JUCE's version check runs before the CLAP warning.** `src/CMakeLists.txt`
runs `add_subdirectory(${SURGE_JUCE_PATH} ...)` at **line 27**. The
`VERSION_LESS 3.21` warning comes after it, at **line 35**. JUCE's own file
demands `cmake_minimum_required(VERSION 3.22)`. That is true of the pinned
8.0.12 (`libs/JUCE/CMakeLists.txt:33`), and also of `surge-7.0.12`, the tree
upstream's "Windows MSVC JUCE 7" CI leg swaps in (line 24, read via
`gh api repos/surge-synthesizer/JUCE/contents/CMakeLists.txt?ref=surge-7.0.12`).
A `cmake_minimum_required` above the running version is a hard error in any
directory. So on CMake below 3.22, configure **stops at JUCE**, and the warning
is never reached.

Tested by shape, not with an old CMake. I built a scratch project with a 3.15
root, then a subdirectory demanding 99.0 added first, then the "Skipping CLAP"
`message(WARNING)`. On CMake 3.31.6 the configure exits 1 with
`CMake 99.0 or higher is required`. The warning line never prints.

The one configuration that skips JUCE, `SURGE_SKIP_JUCE_FOR_RACK`, also skips
`add_subdirectory(surge-xt)` (`src/CMakeLists.txt:171`), so it cannot produce a
CLAP on any CMake version. That is not a silent failure; that configuration
never builds a CLAP at all.

**2. The documented build names the target.** Both platforms' commands pass
`--target surge-xt_CLAP`. If that target did not exist, the build would fail.
Tested on Windows: `cmake --build ... --target surge-xt_CLAP_does_not_exist`
gives `MSBUILD : error MSB1009: Project file does not exist.`, rc=1. Only a bare
`cmake --build build` with no target could ever exit 0 without a CLAP, and only
after an explicit `-DSURGE_BUILD_CLAP=OFF`. That is a choice someone made, not
a trap.

The `FATAL_ERROR` in `libs/clap-juce-extensions/CMakeLists.txt:13` adds nothing
to the argument. CMake has ignored that keyword since 2.6, and an unmet
minimum is fatal either way.

**Decision.** ADR-0007's Decision stands unchanged:

1. CMake ≥ 3.22 is required.
2. Record the version before the first build.
3. A build counts only if the `.clap` exists on disk.

What changes is the **reason**: CMake below 3.22 is a **loud configure
error**, not a silent success. The `src/CMakeLists.txt:35-41` warning is dead
code for any build that could make a CLAP. Do not describe it as the main risk.

Point 3 is kept on its own merits, not because of the failure ADR-0007
described. An exit code proves a command ran, not what it produced, and
checking for the file is one line. `tools/build_clap_win.bat` asserts it and
exits 4 if the file is missing.

**Consequences.**

- `ARCHITECTURE.md` §2.1's warning box is rewritten to say this.
- `collab/WIN-ONBOARDING.md` step 4.1 is corrected to match.
- Anyone meeting a CMake-version problem should expect a configure error that
  names 3.22, not a green build with a missing file.
- When we write CI, it asserts the artifact because an exit code proves
  nothing about output in general, not because of this particular path.

The general lesson ADR-0007 closed on still holds, and this ADR is an instance
of it. `cmake_minimum_required` in a vendored tree is a floor for that file;
**the order in which files are processed** decides which floor you hit first.

---

## ADR-0009 — Goal: an in-plugin wavetable generator, driven by the AI and integrated with adi_daw

**Date:** 2026-09-21 · **Agent:** win · **Directed by:** Adi · **Status:** goal
DECIDED; interface PROPOSED

**Context.** Adi added a goal to the project. adi-surge gets an
**auto/random wavetable generator inside the plugin**. The AI integration must
be able to drive it, and it must be **fully integrated with `adi_daw`**.

Surge already has most of the substrate. Each oscillator can carry a Lua
wavetable script: `wavetable_script`, `wavetable_script_res_base` and
`wavetable_script_nframes` on the oscillator's storage.
`WavetableScriptEvaluator` runs the script. `WtGenService`
(`src/common/dsp/WtGenService.{h,cpp}`) runs it on a background worker, in
`Preview` or `Generate` mode, and publishes the result into the oscillator's
wavetable. Upstream tests cover it: `WtGenService`, `Wavetable Script`,
`Wavetable Script Snapshots`. The script is saved in the patch (§4.5).

**Decision.**

1. **The goal is adopted.** A table can come from a "random" action or from a
   description, and it is made inside the plugin. Nothing is loaded from a
   sample library.
2. **Proposed interface: the AI emits a descriptor, not samples and not
   code.** A descriptor is a few dozen named numbers: frame count, motion, and,
   at the start, middle and end of the table, brightness, slope, odd/even
   balance, bandwidth, two formants, roughness, density, crest and level. A
   generator turns descriptor plus seed into the table, deterministically. The
   reasons:
   - A language model is good at emitting a handful of named, bounded numbers.
     It is bad at emitting 2048-sample frames.
   - Descriptor plus seed is a few hundred bytes. Stored in the patch, it
     reproduces the table exactly on reload.
   - A descriptor is data. Having the model write Lua wavetable scripts
     instead would mean executing model-written code inside the plugin, which
     this avoids.
   - A descriptor carries no waveform data. So "make one like X" can be
     answered without copying X.
3. **Determinism is part of the contract.** The same descriptor and seed must
   give the same table on every platform, because the patch stores only those.
   The prototype therefore uses its own splitmix64 generator, not `<random>`,
   whose distributions differ between standard libraries.
4. **adi_daw integration is now a requirement.** How it works is still
   undesigned. This changes the stance ADR-0003 took (integration intended,
   nothing assumed) to integration **required**. The mechanism stays open,
   below.

**Evidence the interface works (prototype `tools/wtgen/`, standalone C++, no
dependencies).**

- `wtgen selftest`, 15/15:
  - the analyzer reads a saw as −6.0206 dB/oct with odd ratio 0.7501, which
    is the theoretical π²/8 ÷ π²/6;
  - a synthetic table round-trips through descriptor and generator with
    brightness within 0.15% and odd ratio exact;
  - the generated table's fine ripple correlates 0.05 with the source's,
    against 1.00 for a real copy.
- Run against a local third-party pack of 80 Serum-format tables (2–35
  frames; not ours, in no repository, used only as descriptor sources):
  - 80/80 written, with identical paths and frame counts;
  - 78/80 within 15% brightness and 0.1 odd ratio at every keypoint;
  - **0 flagged as possible copies**.
- The copy check has a null control: the same descriptor, generated with
  another seed. The highest ripple correlation with any source (0.836) sat
  below that null (0.909). What resemblance there is, the descriptor carries.
- Details are in `collab/win.md`.

**Still open.** Each item needs its own ADR before code lands.

- **How the descriptor reaches the plugin from adi_daw.** CLAP parameters are
  scalar and cannot carry a descriptor. The candidates are the plugin state
  blob, a custom CLAP extension, or adi_daw composing a preset. Whichever is
  chosen, the adi_daw session writes the matching ADR on its side.
- **Where generation runs in the plugin.** One option is our own C++ generator
  beside `WtGenService`. The other is a fixed, trusted Lua template with the
  descriptor passed in as constants, which reuses `WtGenService` unchanged.
  The second keeps the fork delta smallest (ADR-0002).
- **How the table is stored in the patch.** Either the descriptor and seed
  (small, reproducible, needs the generator at load time), or the rendered
  frames (large, self-contained). Probably both: frames for compatibility, the
  descriptor to edit later.
- **The fork origin (ADR-0006).** Any in-plugin work is a C++ change inside
  `surge/`, and that is blocked until the fork has an origin.

**Consequences.** `tools/wtgen/` is the reference implementation of the
descriptor and generator until the in-plugin version exists. It is MIT-licensed
original code and can move into the GPLv3 plugin as it is. Tables made from
third-party references stay local; only tables made from random or AI-written
descriptors are ours to ship.

---

## ADR-0010 — A table generated from a reference's descriptor is ours if it does not null against it

**Date:** 2026-09-21 · **Agent:** win · **Directed by:** Adi · **Supersedes:**
the last sentence of ADR-0009's Consequences

**Context.** ADR-0009 ended cautiously: tables made from third-party
references stay local. Adi's decision replaces that. The references were only
examples. If our tables do not null against them, the tables are ours, and the
references are not named again.

Copyright protects a waveform's actual data, not measurements such as
brightness, slope or odd/even balance. The descriptor carries only those
measurements (ADR-0009). A null test checks directly whether any data came
across: flip the polarity, sum the two, and hear what is left.

**Decision.**

1. **The test is the best-case null.** For every generated frame against
   every reference frame, take the best gain, the best circular shift and
   either polarity. The residual is `10·log10(1 − xcorr²)` of the reference's
   energy. A plain phase-inverted A/B in a DAW can only null less deeply.
   Identical audio nulls to −90 dB and below.
2. **Two exemptions, because the shape is shared, not the data.**
   - A pure sine nulls against any pure sine. We measured −55 dB, limited only
     by whole-sample alignment.
   - A band-limited square, saw or triangle reaches about −30 dB against any
     other of its kind.
   Nobody owns these shapes. Every other frame must stay well clear of a null.
3. **The tools report it.** `wtgen compare` prints `null_dB` for every frame
   pair. `wtgen pack` reports each table's `deepest_null_dB` and the deepest
   in the pack. The ripple-correlation check and its seeded null (ADR-0009)
   stay as secondary evidence.
4. **References stay local and unnamed.** A reference is never committed, and
   is never named in a file name, folder name, doc or commit message. A
   generated table is named from its own descriptor.

**The first pack: `adi-gen-01`**, in `wavetables/adi-gen-01/`. It holds 80
tables in five categories of our own (Tones, Core, Voices, Grit, Chimes).
Frames are 2048 samples, float32, with `clm ` and `srge` chunks. The pack is
MIT-licensed. Against its references:

- **The deepest best-case null of any frame is −30.5 dB.** Of the 146 frame
  pairs deeper than −20 dB, 89 share only 1–4 harmonics (sine-like) and 36
  share 5–24. The richest of those pairs are band-limited squares and saws, at
  −27 to −29 dB. They are all covered by the exemptions.
- **No table nulls as a whole.** In 60 of the 80, the matched frames leave on
  average more than a quarter of the signal behind (shallower than −6 dB).
- **78/80 are similar by descriptor, and the ripple check flags 0.**

The references were deleted once the pack was made. **The files are now the
canonical artifact.** The pack cannot be regenerated, and does not need to be.

**Consequences.**

- The in-plugin generator (ADR-0009) inherits this rule. When the AI feature
  is asked to make a table like one the user supplies, the plugin must run the
  null test before keeping the result.
- Factory placement in `surge/resources/data/wavetables/` waits for the fork
  origin (ADR-0006). Until then the pack lives in `wavetables/` in the
  monorepo, and users copy it into Surge's user `Wavetables` folder.
