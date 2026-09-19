# VST-ADI — Decision log

Append-only. Newest at the bottom. Superseding an earlier decision means writing
a **new** entry that says so — never edit an old one.

`ARCHITECTURE.md` explains how the system works. This file records *what was
decided and why*, so neither agent re-litigates a settled question or silently
reverses one.

Format: **ADR-NNNN — title**, then Context / Decision / Consequences.

---

## ADR-0001 — `VST-ADI/vital` is a fork tracked against upstream, not a vendored copy

**Date:** 2026-09-16 · **Agent:** win

**Context.** The Vital source arrived as a plain unversioned folder inside the
`Adi` monorepo. With no baseline there was no way to tell our changes from
Tytel's code, and `third_party/` alone is ~168 MB (136 MB of it prebuilt Firebase
binaries), which is too much to vendor into the monorepo.

**Decision.** `VST-ADI/vital/` is its own git repository with an `upstream`
remote at `mtytel/vital`. `main` mirrors upstream (currently `636ca0e`) and is
never committed to; work happens on branches. The parent `Adi` repo ignores
`VST-ADI/vital/`.

**Consequences.** The whole fork delta is one command:
`git -C VST-ADI/vital diff upstream/main`. Verified at setup time that our copy
was byte-identical to upstream apart from our own edits. Cost: the fork is not
visible to anyone who only has the monorepo — see ADR-0011.

---

## ADR-0002 — Stay on Projucer; do not migrate to CMake

**Date:** 2026-09-16 · **Agent:** win

**Context.** Vital predates JUCE's CMake support; the build is driven by
`.jucer` XML. Migrating to modern JUCE + CMake was considered.

**Decision.** Keep Projucer and JUCE 6.0.5. Edit `.jucer` files, then
`Projucer --resave`. Never hand-edit a generated `.vcxproj`.

**Consequences.** Migration would have meant abandoning Vital's patched JUCE
(ADR-0003), which does not build against stock JUCE at all. Projucer 6.0.5 has
no VS2022 exporter, hence ADR-0004. Adding a source file now requires touching
two places: the `.jucer` and the matching `src/unity_build/` stub.

---

## ADR-0003 — Vital's vendored JUCE is patched and must not be replaced

**Date:** 2026-09-16 · **Agent:** win

**Context.** `vital/third_party/JUCE/modules` looked like a normal JUCE 6.0.5
checkout. Diffing it against the official 6.0.5 release found **39 modified
files** plus an LV2 wrapper Tytel added.

**Decision.** Always build against `vital/third_party/JUCE/modules`. The
separately downloaded JUCE tree exists **solely** to provide `Projucer.exe` and
to build host-side tooling (ADR-0009).

**Consequences.** `juce_dsp/native/juce_sse_SIMDNativeOps.h` adds vectorised
`div` that stock JUCE lacks and Vital's DSP calls, so stock JUCE fails to
compile. All `.jucer` files set `useGlobalPath="0"`; do not "fix" this to point
at a global JUCE install.

---

## ADR-0004 — Build VS2019 projects with the v143 toolset

**Date:** 2026-09-16 · **Agent:** win

**Context.** JUCE 6.0.5 (Nov 2020) predates VS2022 (Nov 2021); Projucer's newest
MSVC exporter is VS2019 / v142. Installing the v142 toolset alongside v143 was
attempted and silently did not take.

**Decision.** Use the VS2019 exporter and override on the command line:
`/p:PlatformToolset=v143`. No project file is pinned to v142.

**Consequences.** v143 compiles the whole tree cleanly (~1850 pre-existing
conversion warnings, which are not to be "fixed"). Nothing depends on v142 being
installed.

---

## ADR-0005 — Drop Intel IPP; use the juce_dsp FFT backend

**Date:** 2026-09-16 · **Agent:** win

**Context.** `INTEL_IPP=1` in the exporter defines requires Intel IPP's
`ipps.h`, which is not installed. It was the only compile error in the tree.

**Decision.** Remove `INTEL_IPP=1` and `IPPLibrary="Sequential"`.
`fourier_transform.h` falls through to `juce_dsp`'s FFT — the same backend
Vital's own Linux builds use.

**Consequences.** Audio verified clean by ear and by the validator (ADR-0009).
If FFT throughput ever matters for wavetable rendering, IPP is the lever, but
nothing currently suggests it does.

---

## ADR-0006 — `NO_AUTH=1`; our builds never contact vital.audio

**Date:** 2026-09-16 · **Agent:** win

**Context.** The exporters defined `REQUIRE_AUTH=1`, which is **dead** —
referenced nowhere in `src/`. The real gate is `#if NDEBUG && !NO_AUTH`, so a
Release build put a Firebase login wall in front of the synth and called out to
vital.audio. Upstream's README explicitly asks third-party builds not to.

**Decision.** Replace `REQUIRE_AUTH=1` with `NO_AUTH=1` in all exporters.

**Consequences.** Firebase headers are compiled out entirely. The preset-store
links in `preset_browser.cpp` / `popup_browser.cpp` and the TTWT endpoint in
`oscillator_section.cpp` are **separate paths still present** and must be
neutralised before any distribution.

---

## ADR-0007 — Windows plugin ships VST3 only; VST2 is removed

**Date:** 2026-09-17 · **Agent:** win

**Context.** `plugin/vital.jucer` asked for VST2 (`buildVST=1`) and pointed
`vstLegacyFolder` at a VST2 SDK that is not in this repo. `vst3Folder` also
pointed at a nonexistent path. Separately, JUCE's *VST3* wrapper includes *VST2*
headers unless told otherwise.

**Decision.** `buildVST=0`; drop `vstLegacyFolder`; clear `vst3Folder` so JUCE
uses its bundled VST3 SDK; set `JUCE_VST3_CAN_REPLACE_VST2=0`.

**Consequences.** Steinberg no longer licenses VST2, so this costs nothing real.
`plugin/builds/vs17/Vial_VST.vcxproj{,.filters}` were deleted as orphans.
`enablePluginBinaryCopyStep` is off on Windows because the copy target needs
admin rights and failed the build after a successful link.

---

## ADR-0008 — The AI returns a sparse patch in our own schema, merged not reset

**Date:** 2026-09-17 · **Agent:** win

**Context.** `LoadSave::loadControls` resets every parameter absent from the JSON
to its Init default. Applied naively to a sparse AI response, a prompt like
"give this a pluckier filter envelope" would destroy the user's oscillators and
wavetables. Raw `.vital` is also a poor generation target: parameters nest under
`"settings"`, `modulations` is a fixed 64-slot array, and a connection's routing
(`modulations[i]`) is stored separately from its amount
(`modulation_<i+1>_amount`) correlated only by index.

**Decision.** The model emits a flat patch of our own design (`params`,
`modulations`, `remove_modulations`, `lfos`, `preset_name` — all optional). C++
merges it over a full `LoadSave::stateToJson()` snapshot and passes the complete
result to the existing `loadFromJson`.

**Consequences.** Vital's load path needs **no changes** — nothing is ever
absent, so the reset branch never fires, and preset load / DAW restore / AI
generation keep sharing one code path. Wavetables survive automatically. The
model never sees the slot-index coupling; our merge assigns slots and writes
both places. Cost: `loadWavetables` re-renders three oscillators on every apply.

---

## ADR-0009 — Verify the plugin with a headless validator, not a DAW

**Date:** 2026-09-17 · **Agent:** win

**Context.** No DAW was installed, and the vendored VST3 SDK is a stripped
VST 3.6.10 with no `samples/`, so Steinberg's `validator` could not be built
from it.

**Decision.** `VST-ADI/tools/validator` — a JUCE console app, built with CMake
against the **stock** JUCE download, that loads a built `.vst3` over the real
VST3 ABI and smoke-tests it.

**Consequences.** Baseline on unmodified Vital: **15/15 passed**, state
232,438 bytes. Runs in seconds, so it is the regression gate for every change to
the plugin. It exercises `getStateInformation`/`setStateInformation`, which is
precisely the `stateToJson`/`jsonToState` path ADR-0008 depends on. A host talks
to the plugin over the ABI, so it neither needs nor wants Vital's DSP patches —
this is the one place stock JUCE is correct.

---

## ADR-0010 — The prompt UI is an `Overlay`, not a header bar

**Date:** 2026-09-17 · **Agent:** win

**Context.** The obvious home for a prompt field is `HeaderSection`, but its
`resized()` packs logo, tabs, preset selector, volume and oscilloscope edge to
edge with widths derived from each other and from skin values.

**Decision.** A new `PromptSection : public Overlay` in
`src/interface/editor_sections/`, triggered by one small `OpenGlShapeButton`
added to `HeaderSection`. Undo lives in the overlay.

**Consequences.** No layout surgery, reuses a proven pattern (`SaveSection`,
`DeleteSection`), keeps the fork delta reviewable. The editor must be
`OpenGlTextEditor`, not `juce::TextEditor`, because `FullInterface` is an
`OpenGLRenderer`.

---

## ADR-0011 — Two agents, split by platform, synced only through git

**Date:** 2026-09-18 · **Agent:** win

**Context.** A second Claude agent on macOS joins the project, mirroring the
arrangement already running on `adi_daw`. Neither machine can build for the
other's platform, and iOS builds for Tool 1's mobile client *require* macOS.

**Decision.** Adopt the `adi_daw/collab` protocol for VST-ADI: per-agent log
files, a claims table, branch-per-task, never commit to `main`. Ownership splits
by platform capability rather than by subsystem preference — see
`VST-ADI/collab/README.md`.

**Consequences.** `VST-ADI/tools/`, `docs/` and `ARCHITECTURE.md` already live in
the `arieladi/Adi` monorepo and are visible to both agents today. **The fork
itself is not** — `VST-ADI/vital` has only an `upstream` remote, so there is
nowhere for either agent to push. Resolving that is a prerequisite for any
macOS work on the fork and is the open question at the end of this entry.

**Open:** where `VST-ADI/vital` gets an `origin`. Three candidates:

- **A separate private repo** on the `arieladi` account — preserves the
  `upstream` remote and therefore `git diff upstream/main`, keeps the monorepo
  clean, and private sidesteps both GPLv3 distribution obligations and
  upstream's naming restrictions. Working proposal.
- **A submodule of `Adi`** — pins cleanly, but two agents plus a submodule means
  detached HEADs and an extra pin-bump commit on every change.
- **Vendoring into the monorepo** — simplest sync, but loses `git diff
  upstream/main`, which is currently the only thing that tells us what we have
  actually forked.

Note on size: the working tree is ~180 MB but the **packed repo is 31.5 MiB**
(shallow fetch, and the Firebase binaries compress well). An earlier draft of
this ADR rejected vendoring on size; that reasoning was wrong. Vendoring is
rejected on the loss of upstream tracking, which is the argument that actually
holds.
