# adi-surge — Architecture

Working notes for the CLAP half of the AI synth work. Living document: update it
when a decision changes, don't let it drift.

**Status:** bootstrap. The Surge XT fork is cloned and complete (all 22
submodules checked out clean). **`surge-xt_CLAP` builds on Windows** and
`ctest -j 4` passes **145/145** there (2026-09-21, `collab/win.md`). macOS has
not been built yet, and no AI code is written. Every number below was produced
by a command, and the command is written next to it.

**Two agents work on this project.** Read these first:

| File | What it is |
|---|---|
| [`collab/README.md`](collab/README.md) | The protocol — roster, claims table, branch rules, build commands |
| [`collab/win.md`](collab/win.md) / [`collab/mac.md`](collab/mac.md) | Per-agent logs. Only ever write to your own. |
| [`docs/DECISIONS.md`](docs/DECISIONS.md) | Append-only ADR log — what was decided and why |

This file explains *how the system works*. `docs/DECISIONS.md` records *what was
decided*. When something here looks odd, check the ADR log before changing it.

**Sibling projects, and how to cite them.** `adi-vst` (the Vital fork, VST3) and
`adi_daw` (the DAW) live in this same monorepo with their own ADR logs. This
project's ADR numbers start at 0001 and are its own; always write theirs in full
— "adi-vst ADR-0017", "adi_daw ADR-0052".

---

## 1. Repository layout

`adi-surge/` is a project directory in the `arieladi/Adi` monorepo, alongside
`adi_daw/` and `adi-vst/` — same repo, same clone, same public visibility.

```
arieladi/Adi                 the monorepo
├── adi_daw/                 the DAW — a DIFFERENT project, do not touch
├── adi-vst/                 the Vital fork — a DIFFERENT project, do not touch
└── adi-surge/               this project
    ├── ARCHITECTURE.md      this file
    ├── docs/DECISIONS.md    append-only ADR log
    ├── collab/              two-agent protocol + per-agent logs
    ├── tools/
    │   └── fetch_surge.sh   clone + submodule init, with the traps absorbed
    └── surge/               fork of surge-synthesizer/surge — ITS OWN GIT REPO, gitignored
```

**The three projects share a repo and a working tree, so separation is a working
practice, not a structure.** Work on `adi-surge` in its own session, touch only
`adi-surge/**`, and never run a destructive git command without checking
`git status` and `git branch --show-current` as a separate step. That rule exists
because ignoring it destroyed unstaged `adi_daw` work once already (adi-vst
ADR-0014 and ADR-0015).

`surge/` is a separate git repository, ignored by the monorepo. Its only remote
is named `upstream`, so the entire fork delta is always:

```bash
git -C adi-surge/surge diff upstream/main --stat
```

Currently empty — we have not modified Surge at all yet (ADR-0002, ADR-0006).

| | |
|---|---|
| Upstream | `https://github.com/surge-synthesizer/surge.git` |
| Cloned at | `58914e59c` on `main` — "Sort Floaty Delay after Delay" |
| Version | **Surge XT 1.4.0** (`CMakeLists.txt:20`, `project(Surge VERSION 1.4.0 ...)`) |
| Licence | **GPLv3** (`LICENSE`) |
| Size on disk | 1.5 GB with all submodules checked out; 1.1 GB for the repo alone |

---

## 2. Build system

### 2.1 It is CMake, and that is the biggest single difference from `adi-vst`

Over in `adi-vst`, the build is Projucer XML against a **patched JUCE 6.0.5**
that cannot be replaced (adi-vst ADR-0002, ADR-0003), Projucer has no VS2022
exporter (their ADR-0004), and CLAP is unreachable without adding a second build
system (their ADR-0017).

None of that applies here:

| | Value | Evidence |
|---|---|---|
| Build system | CMake — **real floor 3.22, not the 3.15 the root file claims** | `CMakeLists.txt:2` says 3.15; `libs/JUCE/CMakeLists.txt:33` demands **3.22**; `libs/clap-juce-extensions/CMakeLists.txt:13` demands **3.21 FATAL_ERROR** |
| JUCE | **8.0.12**, the `surge-synthesizer/JUCE` fork | `libs/JUCE/CMakeLists.txt`, `project(JUCE VERSION 8.0.12 ...)`; submodule pinned at `a8c7c71` |
| C++ standard | **C++20**, with `-Werror` / `/WX` on our own code | `CMakeLists.txt:80`, `:126`, `:207` |
| macOS deployment target | 10.15 | `CMakeLists.txt:5`, `CMAKE_OSX_DEPLOYMENT_TARGET` |
| Default build type | Release if unset | `CMakeLists.txt:9-11` |
| MSVC runtime | static (`MultiThreaded$<$<CONFIG:Debug>:Debug>`) | `CMakeLists.txt:4` |
| LTO | `ENABLE_LTO` ON by default | `CMakeLists.txt:13-15` |

C++20 and warnings-as-errors are worth noting up front: `adi-vst` is stuck on
C++14 (their §2.7) and tolerates ~1850 pre-existing warnings. Here, code that
warns does not build, and the strictness is asymmetric between MSVC and clang —
so a branch that is clean on one platform can fail on the other for warnings
alone. That is a thing to expect rather than be surprised by.

**CLAP is a first-class target, already wired.** No work is needed to get one:

```cmake
clap_juce_extensions_plugin(TARGET surge-xt
  CLAP_ID "org.surge-synth-team.surge-xt"
  CLAP_SUPPORTS_CUSTOM_FACTORY 1
  CLAP_FEATURES "instrument" "synthesizer" "stereo" "free and open source")
```
— `src/surge-xt/CMakeLists.txt:66-70`

It is `free-audio/clap-juce-extensions` (submodule `libs/clap-juce-extensions`,
pinned at `16e9d4c`). Note for anyone coming from `adi_daw`: that library builds
CLAP **plugins** out of JUCE projects. It is explicitly *not* a CLAP host, which
is the fact adi_daw ADR-0052 already records.

> ### ⚠ CMake below 3.22 fails configure, loudly (ADR-0008 corrects ADR-0007)
>
> Surge's root file says `cmake_minimum_required(VERSION 3.15)`. **Do not trust
> it.** The real floor is **3.22**, because JUCE demands it
> (`libs/JUCE/CMakeLists.txt:33`), and so does the `surge-7.0.12` JUCE that
> upstream's "JUCE 7" CI leg uses.
>
> `src/CMakeLists.txt:35-41` does contain a check that *looks* like a silent
> trap: a `WARNING`, not a `FATAL_ERROR`, that turns off the CLAP on CMake
> below 3.21. **It cannot be reached in a build that could make a CLAP.** JUCE
> is added at line 27, before it, and JUCE's own 3.22 minimum is a hard error.
> So on old CMake the configure **stops at JUCE with an error naming 3.22**. The
> only configuration that skips JUCE (`SURGE_SKIP_JUCE_FOR_RACK`) also skips
> `surge-xt` (line 171), so it never makes a CLAP on any CMake. ADR-0007 said
> the opposite, from source reading; the first build disproved it. Evidence is
> in ADR-0008.
>
> **Check your CMake version before your first build and record it in your log.**
> On Windows, CMake ships inside the Visual Studio install rather than on `PATH`
> (§2.3), so the version you get is whatever VS bundled. Verify it; do not
> assume. Verified: Mac 4.3.3; Windows 3.31.6-msvc6 (VS 17.14.41).
>
> **Still assert the artifact, not the exit code** (ADR-0007 point 3). An exit
> code proves a command ran, not what it produced. Naming
> `--target surge-xt_CLAP` already fails loudly if the target is missing
> (`MSB1009`, rc=1, tested), and checking the file costs one line.

**Surge is an unusually complete CLAP citizen**, which matters because
`adi_daw` mandated CLAP hosting (adi_daw ADR-0052) specifically for
`CLAP_EVENT_PARAM_MOD`. Surge takes over `clap_direct_process`, so it receives
the raw `clap_process` struct and implements:

- `CLAP_EVENT_PARAM_MOD`, both **monophonic and per-note-id polyphonic**
- note expressions
- voice info (128 voices, overlapping notes)
- remote controls
- a custom preset-discovery factory
- and, because `supportsDirectProcess()` / `supportsVoiceInfo()` are true, it
  advertises and **prefers `CLAP_NOTE_DIALECT_CLAP`** on its note port

That is precisely the surface `adi_daw` wants from a hosted CLAP. It is a
coincidence worth noticing but **not** a design commitment: see ADR-0003's scope
paragraph. Nothing here decides how the two projects integrate.

### 2.2 Two submodule traps — read before running any git command by hand

Both are absorbed by `tools/fetch_surge.sh`; this section is why.

**1. `git submodule update --init --recursive` fails outright.**

```
fatal: transport 'file' not allowed
```

...even though every URL in `.gitmodules` is `https`. Git ≥ 2.38 refuses the
local `file://` transport by default (CVE-2022-39253). The failure was
reproduced on git 2.50.1 (Apple Git-155). *Why* a local transport is involved
is **not established**. The earlier explanation, "the submodule clone path
tries the superproject's object store first", is unverified: every URL in every
`.gitmodules`, nested ones included, is `https`. The fix works either way.
Scope the permission to the one command:

```bash
git -C adi-surge/surge -c protocol.file.allow=always \
    submodule update --init --recursive --depth 1 --jobs 4
```

**Do not set `protocol.file.allow=always` globally.** It is a global weakening
of a real mitigation for a problem local to this clone.

**2. `.gitmodules` declares 23 submodules; only 22 are gitlinks.**
`src/surge-rs/surge-rs` is a stale declaration with nothing behind it, so git
never fetches it and `git submodule status` never mentions it. Upstream's state,
not a broken checkout, and nothing in the CMake build references it.

Verified clean afterwards — 27 entries (22 top-level + 5 nested), zero drift:

```bash
git -C adi-surge/surge submodule status --recursive | grep -vc '^ '   # -> 0
```

See ADR-0004 for why we do **not** pin these ourselves the way `adi_daw` pins
its `third_party/` (adi_daw ADR-0024): Surge's submodules are gitlinks, so the
pin, the record and the verification already exist in Surge's own commits.

### 2.3 Build commands

These are upstream's own, lifted from `surge/.github/workflows/build-pr.yml:129-132`,
which is the most reliable build recipe available:

```
cmake -S . -B ./build <config> -DCMAKE_BUILD_TYPE=<Debug|Release>
cmake --build ./build --config <...> --target <target> --parallel 3
```

**Windows (win).** MSVC is not on `PATH` and CMake and Ninja live *inside* the
Visual Studio install, which is why `adi_daw` drives its build from a `.bat` —
`vcvars64.bat` sets up the environment in the *current* shell, so calling it from
bash sets variables in a subshell that exits immediately.

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

cmake -S adi-surge\surge -B adi-surge\surge\build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build adi-surge\surge\build --config Release --target surge-xt_CLAP --parallel
```

**`adi-surge\tools\build_clap_win.bat` does all of that**, then asserts that
`build\surge_xt_products\Surge XT.clap` exists (exit 4 if not). With the
`tests` argument it also builds `surge-testrunner` and runs `ctest -j 4`. It
also clears the one environment variable that breaks agent-driven builds (§2.6).
On the VS generator, plain `ctest -j 4` needs no `-C Release`: Catch2's
discovery runs at build time (`POST_BUILD`) and records absolute paths.

**macOS (mac).** Upstream's macOS legs use Ninja:

```bash
cmake -S adi-surge/surge -B adi-surge/surge/build -GNinja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64"
cmake --build adi-surge/surge/build --target surge-xt_CLAP --parallel
```

`CMAKE_OSX_ARCHITECTURES="x86_64;arm64"` builds universal; upstream's own macOS
leg does exactly that. Use `arm64` alone while iterating.

### 2.4 Targets

Named in upstream CI, so all of them are known to build:

| Target | What it is |
|---|---|
| **`surge-xt_CLAP`** | the CLAP plugin — **our primary target** (ADR-0003) |
| `surge-xt_Standalone` | standalone app, the quickest dev loop |
| `surge-xt-distribution` | everything, packaged |
| `surge-common` | the engine, no plugin wrapper — fastest compile check |
| `surge-testrunner` | upstream's test suite, run via `ctest` |
| `surgepy` | Python bindings; needs `-DSURGE_BUILD_PYTHON_BINDINGS=TRUE` |
| `surge-xt-cli` | headless CLI player — see §5.3 |
| `surge-fx_Standalone` | the FX-only plugin, not our concern |

Note Surge builds **two complete plugins** — `surge-xt` and `surge-fx`, each
with its own `CLAP_ID`. "Build the plugin" is a much larger graph than in
`adi-vst`, which is why naming a single target matters for iteration speed.

A default `cmake --build build` with no `--target` emits, **per plugin**, a
CLAP, a VST3 and a Standalone, plus an AU on macOS; LV2 and AUv3 are off by
default. Everything is staged into `build/surge_xt_products`.

`SURGE_JUCE_FORMATS` is **not** a cache option you can set — it is assembled by
`list(APPEND)` at `src/CMakeLists.txt:113-143` from the *negative* options
`SURGE_SKIP_VST3` / `SURGE_SKIP_STANDALONE`, an unconditional `AU` on
Apple-not-cross-compiling, `SURGE_XT_BUILD_AUV3`, a `VST2SDK_DIR` env var, and
`SURGE_BUILD_LV2`. **CLAP is deliberately not in that list** — it is a separate
target created by `clap_juce_extensions_plugin()` and gated as described above.

Tests are `ctest -j 4` in the build tree (`build-pr.yml:134-141`). That is the
regression gate for this project — unlike `adi-vst`, which had to build one
(their ADR-0009).

**Of the 22 submodules, 20 are needed for a default build.** Only
`melatonin_inspector` and `pybind11` sit behind default-OFF options.

Two flags that bite:

- `-DSURGE_SKIP_LUA=TRUE` is used on upstream's Windows ARM64 legs. If LuaJIT is
  what breaks your build, that is the escape hatch — at the cost of the formula
  modulator (§4.4).
- `-DSURGE_SKIP_STANDALONE=TRUE` **hard-breaks CMake configure on macOS**,
  because the CLI-into-`.app` copy step at `src/surge-xt/CMakeLists.txt:262-276`
  is a bare `if(APPLE)` with no `TARGET` check. Do not reach for it to speed up
  a CLAP-only build; name the target instead.

  **It does not block `surgepy`**, though it looks like it should.
  `src/surge-python/setup.py` passes that exact flag — alongside
  `-DSURGE_SKIP_JUCE_FOR_RACK=TRUE`, which gates `add_subdirectory(surge-xt)`
  out entirely at `src/CMakeLists.txt:171`, so the pip path never reaches the
  broken block. Upstream CI builds `surgepy` on Windows and Ubuntu but **never
  on macOS**, so `mac` is first to exercise it — the one obvious blocker does
  not apply, but nothing else about that path is proven.

### 2.5 The post-build copy hazard that cost `adi-vst` eight builds is absent here

Worth stating explicitly, because it is the first thing to suspect on Windows
and the answer is "not this time". `adi-vst` lost four Windows builds to a
post-build step copying the plugin into `C:\Program Files\Common Files\VST3`
(needs admin; a green compile and link followed by `Build FAILED`), and four
more on macOS to a build cycle from the same mechanism (their ADR-0007).

Surge pre-disables it: **`SURGE_COPY_AFTER_BUILD` is OFF by default**
(`src/CMakeLists.txt:8`), and its own staging copies target
`build/surge_xt_products` *inside* the build tree, needing no privileges on
either platform. The CLAP copy-after-build is not even implemented on Windows.

Two caveats: `SURGE_COPY_TO_PRODUCTS` does **not** mean what its name suggests,
and the macOS CLI-into-`.app` copy noted above is a real cross-target post-build
step — it does not form a cycle (`Standalone → cli → surge-xt` plus
`Standalone → surge-xt`), but it is the thing that breaks if you skip the
Standalone.

**The CLAP copy-after-build is a no-op on Windows.** `ClapTargetHelpers.cmake:171-189`
has Darwin and Linux branches only. A Windows agent setting
`-DSURGE_COPY_AFTER_BUILD=True` expecting the CLAP to land somewhere scannable
gets an admin-rights VST3 copy attempt and **no CLAP copy at all**. Leave it OFF
on Windows and point the host at `build/surge_xt_products`, or copy
`Surge XT.clap` by hand to `%CommonProgramFiles%\CLAP\`. On macOS it does work,
targeting `~/Library/Audio/Plug-Ins/CLAP`, no admin needed.

### 2.6 Six more build traps worth knowing before you hit them

| Trap | Evidence | What to do |
|---|---|---|
| **LuaJIT is built at *configure* time, by launching a `.bat` by bare name**, and that launch fails under Claude Code. The harness sets `NoDefaultCurrentDirectoryInExePath=1` in its own process environment. With it set, Windows will not look in `WORKING_DIRECTORY` for the script, and configure dies with `Build script exit code: no such file or directory`. A person in a Developer Prompt never has the variable set. | `libs/luajitlib/CMakeLists.txt`: `execute_process(COMMAND build-msvc-luajit.bat ...)`. Reproduced with a `cmake -P` probe: a bare `.bat` fails with the variable set and runs without it (`collab/win.md`, 2026-09-21). | Clear it for the build's process tree only: `set "NoDefaultCurrentDirectoryInExePath="`. `tools/build_clap_win.bat` does. `-DSURGE_SKIP_LUA=TRUE` also avoids it, but costs the formula modulator. |
| **`/WX` has no escape hatch on MSVC.** `SURGE_SKIP_WERROR` suppresses `-Werror` for clang/gcc, but the MSVC branch adds `/WX` with no equivalent guard (skipped only for arm64/arm64ec). Code clean on macOS can fail Windows on an unused variable. | `CMakeLists.txt:126` vs `:204-208`; MSVC-only suppressions at `:213-221` (4244/4305/4267/4018/4388/4065/4702/4005/5105) | Build with the other platform's strictness in mind. Anything outside that suppression list is fatal on Windows only. |
| **MSVC static runtime `/MT` is forced** via CMP0091. Any prebuilt third-party library built `/MD` gives `LNK2038` on Windows only. | `CMakeLists.txt:3-4` | Build every new Windows dependency from source inside the CMake tree so it inherits `CMAKE_MSVC_RUNTIME_LIBRARY`. **Note this does *not* apply to the obvious case**: the AI feature needs no new HTTP dependency, because `juce_core/network/` already ships `juce_URL` and `juce_WebInputStream`. `JUCE_USE_CURL=0` (`src/CMakeLists.txt:85`) only affects the Linux native path. |
| **LTO is on by default for Release**, so every Release link of a very large TU set is slow. | `CMakeLists.txt:13-15`, `:92-99` | Iterate with `-DENABLE_LTO=OFF` explicitly. Note `RelWithDebInfo` also matches the `Release` regex, so it does not disable LTO on its own. |
| **`surge-xt-distribution` fails opaquely on Windows** without 7-Zip and Inno Setup on `PATH`, and needs `SURGE_BUILD_FX=ON` because the portable-zip step copies FX artifacts by literal filename. | `src/cmake/lib.cmake:184,189,191,220`; `src/CMakeLists.txt:184` | Build `surge-staged-assets` (the README's recommendation) for day-to-day work. |
| **`stage-extra-content` writes into the SOURCE tree** and `download-extra-content` git-clones from the network mid-build. | `cmake/stage-extra-content.cmake:11-31` — `copy_directory ... resources/data/skins` with `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}` | Never build these in our fork unless you intend the source-tree mutation. They are not in the default `all`. |

---

## 3. Synth state — the pipeline we hook into

### 3.1 Parameters, and the four names each one has

Surge has no `ValueTree` and no JSON. State is a flat, fixed-size array of
heavyweight `Parameter` objects, built once in the `SurgePatch` constructor:

```cpp
std::vector<Parameter *> param_ptr;                                  // SurgeStorage.h:1376
std::unordered_map<std::string, Parameter *> param_ptr_by_oscname;   // SurgeStorage.h:1359
```

**Counts, derived not estimated** (`SurgeStorage.h:155-161`, `73-84`):

```
n_scene_params      = 273
n_fx_slots          = 16     n_fx_params  = 12     n_osc_params = 7
n_global_params     = 11 + 16 * (12 + 1)        = 219
n_global_postparams = 1
n_total_params      = 219 + 2 * 273 + 1         = 766
```

**766 parameters, confirmed two independent ways.** Arithmetically as above, and
*empirically* from the 3797 `.fxp` files shipped with Surge: 772 distinct XML
element names across the whole corpus, minus exactly 6 retired
`*_osc*_startphase` names, is 766. For scale, `adi-vst` reports 772 real Vital
parameters — the two synths are almost exactly the same size.

**170 control types** — the `ct_*` enum runs `Parameter.h:51` (`ct_none`) to
`Parameter.h:222` (`num_ctrltypes`):

```bash
sed -n '50,222p' src/common/Parameter.h | grep -cE '^\s+ct_[a-z0-9_]+,'   # -> 170
```

`ff_revision = 30` is the current streaming revision (`SurgeStorage.h:155`).
There is no symbol called `current_streaming_revision`.

#### Each parameter has FOUR names and they are not interchangeable

This is the first thing to get right, and getting it wrong would be invisible
until a patch failed to load. Parameters register via `assign()`
(`Parameter.h:385-392`), which takes each of them separately:

| Name | Example | What it is |
|---|---|---|
| **`name_storage`** | `a_osc1_pitch` | **the stable streaming key** — literally the XML element name in the `.fxp` (`SurgePatch.cpp:4086`) |
| `oscName` | `/param/a/osc/1/pitch` | a separate, hand-authored, human-readable OSC address |
| `name` | internal, unprefixed | presentation only |
| `dispname` / `fullname` | `Osc 1 Pitch` | GUI labels, and **dynamic** for some parameters |

Plus `ui_identifier` for the skin engine, and a fifth namespace in `surgepy`
(C++ member names). `adi-vst` has exactly one namespace for this; we have five.

> **The AI-facing key is `get_storage_name()`.** It is the serialization
> contract and it is what the patch writer uses as the XML element name
> (`SurgePatch.cpp:4086`).

**`oscName` is NOT derivable from `name_storage`, and assuming it is would ship
an AI that writes to addresses that do not exist.** The two are independent,
hand-maintained namespaces. `Parameter.cpp:215-216` looks like it has a
fallback —

```cpp
oscName = fmt::format("/param/{}", altOSCname.empty() ? name_storage : altOSCname);
```

— but that fallback is **dead code**: `altOSCname` is a required positional
argument and **zero of the 100 `assign()` call sites** in `SurgePatch.cpp` pass
an empty literal. The address is always hand-authored, and built by separate
`fmt::format` calls:

| Parameter | `name_storage` | actual OSC address |
|---|---|---|
| Osc 1 pitch | `a_osc1_pitch` | `/param/a/osc/1/pitch` (`SurgePatch.cpp:176-178`) |
| Scene A volume | `a_volume` | `/param/a/amp/volume` (`SurgePatch.cpp:231`) |

`/param/a_osc1_pitch` **does not exist.** Keep a mapping; never derive one.

#### Sparse patches are native to Surge — we do not need `adi-vst`'s merge trick

`adi-vst` had to merge an AI patch into a full state object first, because
Vital's `loadControls` resets anything absent to Init defaults (their §3.3a).

Surge does not have that problem. The `.fxp` loader **silently leaves absent
parameters at their current values** (`SurgePatch.cpp:1943-1965`), so a ~40-key
sparse patch is native to Surge's own format.

**Be precise about the mechanism, because it has a cost.** It is *not* a map
lookup. The loader walks `param_ptr` in order doing `p->NextSibling(name)`, with
a `parameters->FirstChild(name)` fallback when the sibling walk misses — an
optimisation that assumes document order matches `param_ptr` order. On a sparse
40-of-766 patch the sibling walk misses constantly, so each of the ~726 absent
parameters costs a full `FirstChild` scan of the `<parameters>` element.

Fine for a one-shot patch apply. **Not** cheap enough for a per-keystroke live
edit loop — use `setParameter01` for that instead.

Two routes exist for applying one parameter at a time:

- `SurgeSynthesizer::setParameter01(ID, float)` (`SurgeSynthesizer.cpp:2875`) —
  sets exactly one parameter by synth-side id and touches nothing else;
- `SurgePatch::parameterFromOSCName(std::string)` (`SurgePatch.cpp:1742-1752`) —
  resolves a stable string name to a `Parameter*` through the prebuilt map.

So the AI should emit **a sparse list of named parameter deltas**, not a
serialized full state.

#### The polymorphism problem — the largest new burden versus Vital

`fxN_pM` and `a_oscN_paramM` **have no fixed meaning, range, valtype or display
name.** Each effect's and oscillator's `init_ctrltypes()` calls `set_type()` on
those generic slots when the FX or oscillator type changes:

```cpp
Parameter p[n_osc_params];   // SurgeStorage.h:750   — 7 generic slots per oscillator
Parameter p[n_fx_params];    // SurgeStorage.h:830   — 12 generic slots per FX
```

Upstream's own `scripts/misc/surgepy-params.py` demonstrates it: with the
oscillator set to Classic, slot contents are Shape / Width 1 / Width 2 / Sub Mix
/ Sync / Unison Detune / Unison Voices; set it to FM3 and the same slots become
M1 Amount / M1 Ratio / M2 Amount / M2 Ratio / M3 Amount / M3 Frequency /
Feedback. You must call `surge.process()` and re-fetch the patch to see it.

**Any AI schema must therefore be conditioned on `fxN_type` / `a_oscN_type`
first.** Vital's names are fixed for the life of the plugin; ours are not. This
is the single largest new complication and it should be designed for explicitly,
not discovered later.

#### Values are not uniformly floats

Vital's values are uniformly floats. Surge's `pdata` union is int, bool **or**
float (`Parameter.h:35-47`). Of the 170 control types, **114 accept a natural
units string** via `set_value_from_string` (`Parameter.cpp:4711`) — so the AI
can emit `"440 Hz"`, `"-12.0 dB"`, `"1/8 D"` rather than a normalised float, and
every parameter has a `get_display` (`Parameter.cpp:3541`) for the reverse. The
other **56 are discrete enum selectors** (oscillator type, filter type, FX type,
waveshaper, booleans) with no string parser, where an integer index is required.

So the schema is `name -> {float | int-enum | bool}` with a per-type enum table,
not `name -> float`.

#### Modulation is an unbounded list, not a fixed matrix

Vital's fixed 64-slot array with the two-place amount/routing coupling has no
analogue here. Surge uses three unbounded `std::vector<ModulationRouting>`
(per-scene voice, per-scene, and global — `SurgeStorage.h:883`, `:1379`).
Routings are created and destroyed dynamically; **setting depth to 0 erases the
entry** (`SurgeSynthesizer.cpp:4122-4127`). They serialize as XML children of the
**destination** parameter rather than as a separate matrix
(`SurgePatch.cpp:4105-4142`).

Measured across the shipped patches: mean 11.7 routings, max 138. The 64-slot
mental model would impose a ceiling Surge does not have.

AI representation should be a sparse list of
`{source_tag, source_index, destination_storage_name, depth}`, with `source_tag`
drawn from `modsource_names_tag` (`ModulationSource.h:202`, explicitly marked
stable-for-streaming).

### 3.2 The patch format

A Surge patch is a **binary chunk wrapping XML** — nothing like Vital's JSON.
`SurgePatch::save_patch` emits:

1. a 32-byte packed `patch_header` — magic `"sub3"`, little-endian `xmlsize`,
   `wtsize[2][3]` (`PatchFileHeaderStructs.h:47-53`)
2. the TinyXML document from `save_xml`
3. raw wavetable blobs
4. a `binn` "arbitrary block storage" tail

A **`.fxp` on disk** is exactly that chunk with a 60-byte big-endian VST2
`fxChunkSetCustom` header prepended (`'CcnK'` / `'FPCh'` / `'cjs3'`).

**DAW state is not the same bytes as a `.fxp`.**
`SurgeSynthProcessor::getStateInformation` returns the chunk **without** the
60-byte header, and with a populated `dawExtraState` block in the XML. Do not
assume one can be fed to the other.

There is **no JSON anywhere** in Surge — `grep -rl nlohmann src/` returns
nothing. The AI's file-level output target is XML, not JSON.
`scripts/patch-tool/patch-tool.py` already parses the container in pure Python.

**Streaming migration is large and inline:** 29 revision-guarded branches in
`SurgePatch.cpp`, a main contiguous migration block at lines 2428-2790 (363
lines), plus `handleStreamingMismatches` spread across 53 files. The consequence
for us is concrete: **any parameter we add to the patch format needs its own
revision bump and migration branch.** Vital's schemaless JSON never required
that, and it is a real ongoing tax on modifying Surge's format — an argument for
keeping AI state out of the patch where possible.

### 3.3 Threading — Surge already solved the hazard `adi-vst` had to design around

`adi-vst` §5 spends a page on this: a network thread, a `MessageManager::callAsync`
hop, a `weak_ptr` guard so a closing editor does not touch freed memory, and
`pauseProcessing(true)` to take the audio lock.

Surge's OSC path already has the equivalent, built and shipped. The receiver
parses on a network thread and hands work to the audio thread through a
lock-free ring buffer, drained in `processBlockOSC()`:

```cpp
// queue packet to audio thread
sspPtr->oscRingBuf.push(SurgeSynthProcessor::oscToAudio(...));
```
— `OpenSoundControl.cpp:381-382`, and ~25 further call sites;
`SimpleRingBuffer<oscToAudio, 4096>` at `SurgeSynthProcessor.h:375`

**Patch loading is not on the audio thread.** The audio thread fades out, sets
`halt_engine`, and spawns a detached `std::thread` (`loadPatchInBackgroundThread`)
under `patchLoadSpawnMutex`, outputting silence until it finishes.

There are **four distinct load paths with different threading**, so pick
deliberately — there is no single funnel the way Vital has `loadFromJson`:

| Path | Threading |
|---|---|
| `loadPatch(int id)` via `patchid_queue` | spawned detached thread (`SurgeSynthesizer.cpp:4771-4797`) |
| `loadPatchByPath` | the caller's thread |
| `loadRaw` via `enqueuePatchForLoad` | drained on the audio thread (`SurgeSynthesizerIO.cpp:419-458`) |
| `setParameter01` | single parameter, immediate |

**`SurgeSynthesizer::enqueuePatchForLoad()` is explicitly documented "safe from
any thread"**, followed by `processAudioThreadOpsWhenAudioEngineUnavailable()`
and `refresh_editor = true`. That is exactly what `SurgeGUIEditor::setPatchFromUndo`
already does, and it is the right seam for applying a generated patch.

### 3.4 Undo — upstream has one, and it is substantial

`adi-vst` had to design its own snapshot stack (their ADR-0010, §3.6) because
Vital has no undo for preset loads at all.

Surge ships `Surge::GUI::UndoManager` — 1499 lines, owned by
`SurgeSynthProcessor`, with 16 push types, a bounded deque with a **25 MB
budget**, and whole-patch snapshotting via `save_patch`
(`UndoManager.h:41-91`, `UndoManager.cpp:38-39`, `245-246`, `921-951`). It covers
parameters, modulation, oscillators, wavetables, FX, LFOs, tuning and whole-patch
snapshots, and surfaces to the user through the `ACTION_HISTORY` overlay.

**Call `undoManager()->pushPatch()` before an AI apply and inherit the rest.**
Do not port `adi-vst`'s undo design.

---

## 4. The machine-facing surfaces Surge already ships

This is the section that most justifies the project, so it is stated with
evidence rather than enthusiasm. `adi-vst` had to build a headless validator and
a C++-parsing schema extractor because Vital exposes nothing.

### 4.1 OSC — an address per parameter, in and out, with a shipped specification

`src/surge-xt/osc/OpenSoundControl.{h,cpp}`, with `juce::juce_osc` linked as a
first-class module (`src/surge-xt/CMakeLists.txt:246`).

| | |
|---|---|
| In | `juce::OSCReceiver`, default port **53280** (`globals.h:70`) |
| Out | `juce::OSCSender`, default port **53270** (`globals.h:71`) |
| Specification | **`resources/surge-shared/oscspecification.html`, 97,591 bytes** |

It is **not a curated subset**: `altOSCname` is a required positional argument to
`assign()`, and all 100 `assign()` call sites in `SurgePatch.cpp` pass an
explicit address. Every parameter has one. Address families:

| Prefix | Purpose |
|---|---|
| `/param/<slash-form name>` | set any parameter by name, e.g. `/param/a/osc/1/pitch` |
| `/param/macro/<n>` | the 8 macros |
| `/mod/...` | modulation routing writes |
| `/patch` | load / save a patch |
| `/tuning/scl`, `/tuning/kbm` | microtuning |
| `/q/all_params` | **full self-describing dump** (`OpenSoundControl.cpp:310`) |
| `/doc<name>`, `/doc<name>/ext` | per-parameter name, type, min, max, and extensions |
| `/error` | errors back to the client |

**`/q/all_params` is the important one.** It emits, for every parameter plus the
8 macros: current value, display string, `/doc` (name, type, min, max), and
`/doc/.../ext` listing which per-parameter extensions apply — `abs`, `enable`,
`tempo_sync`, `extend`, `deform`, and the portamento options
(`OpenSoundControl.cpp:1793-1886`).

That is, almost exactly, the schema `adi-vst/tools/extract_schema.py` had to
reconstruct by parsing C++ tables. Here it is queryable at runtime, over a
socket, by upstream's own code.

#### OSC can be switched on from the DAW state blob, with no C++ changes

`setStateInformation` drives OSC startup directly
(`SurgeSynthProcessor.cpp:1545-1554`): it calls `enqueuePatchForLoad`, then
either sets `oscCheckStartup = true` (audio running) or calls
`tryLazyOscStartupFromStreamedState()`. That function (`:1629-1644`) checks
`storage.oscStartIn && storage.oscPortIn > 0` and calls
`oscHandler.tryOSCStartup()`, behind a `rawLoadEnqueued` guard for load
ordering. `prepareToPlay` sets the flag at `:537`; `processBlockPostFunction`
drains it at `:1089-1092`. `oscStartIn` / `oscPortIn` are read from the patch
XML's `dawExtraState` at `SurgePatch.cpp:3745-3758`.

**So a DAW state blob carrying `oscStartIn=1` and `oscPortIn>0` starts the OSC
receiver on that plugin instance.** The mechanism is deliberate and was recently
hardened for exactly this ordering bug.

> ### ⚠ `startOSCIn` / `startOSCOut` user defaults are live-looking dead config
>
> The keys exist — `UserDefaults.h:181-182` declares the enum and
> `UserDefaults.cpp:393-398` maps them to `"startOSCIn"` / `"startOSCOut"`.
> **Nothing reads them.** `grep -rn "getUserDefaultValue(.*StartOSC" src/`
> returns nothing, and `SurgeStorage.cpp:591-596` reads `OSCPortIn`,
> `OSCPortOut` and `OSCIPOut` from user defaults but *not* the two start flags.
>
> A user who sets `startOSCIn=1` in their preferences gets silence. Do not build
> anything on those keys.

#### What OSC cannot do, which is what decides ADR-0005

- **Addresses are curated, not derivable** (§3.1). The only authoritative source
  is a runtime `/q/all_params` sweep. The shipped spec HTML is labelled 1.3 in a
  1.4.0 tree.
- **You cannot emit `"440 Hz"` over OSC.** `set_value_from_string` covers 114 of
  170 control types but is in-process only and unreachable over the wire. Floats
  go as normalised 0..1, ints raw.
- **No atomicity.** N parameters is N UDP datagrams — lossy, drained per block,
  and `setParameter01` has order-dependent side effects (`ct_filtertype` writes
  `param_ptr[index+1]`).
- **No instance identity.** OSC out defaults to a fixed port 53270, so two Surge
  instances in one DAW produce an indistinguishable merged stream.

> **STILL OPEN, and it blocks trusting any OSC tooling.** Nothing asserts the
> ~766 OSC addresses are **unique**, nothing tests it, and it cannot be settled
> statically because they are `fmt::format` calls inside loops. A collision
> writes the wrong parameter, silently. Resolve it with a runtime
> `/q/all_params` sweep plus a uniqueness check before relying on OSC for
> anything — including a throwaway tool.

### 4.2 `surgepy` — a complete headless synth, and the schema extractor

`src/surge-python/` (`surgepy.cpp`, `setup.py`, `pyproject.toml`, `tests/`),
CMake target `surgepy`, behind `-DSURGE_BUILD_PYTHON_BINDINGS=TRUE`. Exercised
on both Windows and Ubuntu in upstream CI.

It instantiates a synth headlessly, enumerates parameters via
`getPatch()` / `getControlGroup()` / `getParamInfo()`
(`surgepy.cpp:1282-1336`), reads min/max/default/type/display, sets values,
loads and saves patches and wavetables, sets modulation depths, and **renders
into numpy buffers**.

`scripts/misc/surgepy-params.py` is a working schema extractor already in-tree,
including the contextual-parameter refresh idiom (§3.1). **This is the thing
`adi-vst` hand-wrote.** `surgepy` is also the offline renderer.

### 4.3 `surge-xt-cli` — a live player, *not* an offline renderer

`src/surge-xt/cli/cli-main.cpp`. It takes `--osc-in-port`, `--osc-out-port`,
`--osc-out-ipaddr`, `--init-patch`, `--sample-rate`, `--buffer-size`,
`--audio-interface`, `--midi-input`, `--no-stdin` (`cli-main.cpp:326-355`).

Be precise about what it is: it binds to a real audio device and plays. There is
no `--render` / `--wav` / offline flag — `grep -niE "render|offline|wav|bounce"
src/surge-xt/cli/cli-main.cpp` returns nothing. For generating training data
offline, use `surgepy` (§4.2), not the CLI. The CLI's value is being an
OSC-drivable Surge with no DAW in the loop.

### 4.4 Test runner — the regression gate, already written

`src/surge-testrunner/`, wired to `ctest`, green in upstream CI on macOS, Linux
and Windows.

**The gate is 145/145.** There are 147 `TEST_CASE`s in the source, and ctest
registers 145. The other two never run:

- `Modern Oscillator Perf` is hidden with the `[.]` tag
  (`UnitTestsGOLDEN.cpp:327`).
- `NaN Patch From Issue #1514` is inside `#if 0` (`UnitTestsDSP.cpp:479`).

Neither exclusion depends on the platform. Measured on Windows: 145/145 in
53 s. `ctest` counts `TEST_CASE`s, so the `SECTION` total does not affect the
gate; there are 355 `SECTION(` and 46 `DYNAMIC_SECTION(` call sites.

Upstream CI runs `ctest -j 4 || ctest --rerun-failed` (`build-pr.yml:140`), so
a test that fails once and then passes still goes green there. **Our gate is
the first run.**

What the suite contains:

- **one** golden-value test: `Modern Oscillator Golden`
  (`UnitTestsGOLDEN.cpp:313`, `GOLDEN_TOL = 1e-5f` at `:60`, `SURGE_GOLDEN=1`
  to regenerate). It covers the Modern oscillator only. The other 11
  oscillators, the filters and the FX have **no** numeric-regression coverage,
  so the suite catches breakage broadly but numeric drift narrowly;
- **"All Patches Are Loadable"** over 641 factory + 2920 third-party `.fxp`s
  (`UnitTestsIO.cpp:568`);
- a DAW **stream/unstream round-trip** (`UnitTestsIO.cpp:588`).

That last one matters to us the way `adi-vst`'s 232,438-byte state round-trip
does: it exercises the exact serialization path an AI apply has to survive.

### 4.5 LUA — text the AI could author, but not reachable over OSC

Surge has LUA formula modulators and `.wtscript` wavetables, edited in-plugin
through `LuaEditors`. They are **text**, which makes them an interesting AI
output target — but they are stored base64-encoded inside the patch XML
(`FormulaModulationHelper.cpp:1172-1190`) and there is **no OSC or `surgepy`
route for setting that text**. It must go through the patch file. Note that
before treating LUA as a cheap win.

---

## 5. GUI — where the prompt bar goes

### 5.1 It is plain JUCE, not OpenGL

```bash
grep -rn "OpenGLContext" src/ | wc -l    # -> 0
```

Vital's entire GUI is OpenGL-composited, which is why `adi-vst` must use
`OpenGlTextEditor` rather than `juce::TextEditor`. **That whole class of problem
does not exist here.** Surge links `juce_gui_basics` and `juce_graphics` and
paints in software (`src/surge-xt/CMakeLists.txt:242-248`). JUCE is 8.0.12, so
the modern accessibility stack, keyboard-focus traversal and
`CodeEditorComponent` are all available — none of which `adi-vst` can use.

Text input is well-trodden: stock `juce::TextEditor` in **8 places**, plus a
`juce::CodeEditorComponent` subclass for the LUA editors. There is a macOS-only
accessibility fixup helper that must be called.

The cost of software painting: streaming tokens into a text box needs
coalescing, or every token is a repaint.

### 5.2 The overlay system is a registry, and adding one is demonstrably cheap

19 headers in `src/surge-xt/gui/overlays/`, **12 direct subclasses** of

```cpp
struct OverlayComponent : juce::Component     // overlays/OverlayComponent.h:39
```

Be precise: 12 is the count of *direct* subclasses, not of instantiable
overlays. One of the 12 — `CodeEditorContainerWithApply` — is itself a base, for
`FormulaModulatorEditor` and `WavetableScriptEditor`. Do not quote "12 overlays
exist".

Registration is an enum plus a factory (`SurgeGUIEditor.h:456-481`): a 14-entry
`OverlayTags` enum (verified: `NO_EDITOR` … `OPEN_SOUND_CONTROL_SETTINGS`, plus
an `n_overlay_tags` sentinel), a `createOverlay()` switch, and
`showOverlay` / `closeOverlay` / `toggleOverlay`.

`OverlayComponent` is a richer contract than Vital's `Overlay`: tear-out into a
`DocumentWindow` with persisted position, move-around, minimum size, a
pre-close confirmation hook, `shownInParent()`, `wantsInitialKeyboardFocus()`
and `getGroupNavigationComponents()` for accessibility — all free.

**How cheap, measured:** upstream commit `920cc6866` added the entire OSC
Settings overlay in **7 files / 426 lines, of which only 25 lines were edits to
4 existing files.** Compare `adi-vst` §6.2, where the same feature costs two new
files plus edits to `full_interface.h/.cpp`, `header_section.h/.cpp`, a
unity-build stub and two `.jucer` files followed by a Projucer resave.

**The trigger is the expensive half, and the answer is a menu item.** Overlays
sit outside the skin layout system and every skin colour has a compiled-in
default, so a custom skin cannot break a new overlay. But a new *button on the
main frame* would need a `Skin::Connector` in `SkinModel.cpp` — 172 compiled
connectors, which third-party skins override by id (135 `<control>` overrides in
the bundled dark skin alone) — and would be mispositioned in them. The
Surge-idiomatic v1 trigger is a menu item: four lines, zero skin risk, exactly
how OSC Settings is reached (`SurgeGUIEditorMenuStructures.cpp:1880`).

> ### ⚠ A prompt box will play MIDI notes as the user types
>
> `SurgeSynthEditor::keyPressed` forwards keystrokes to the on-screen MIDI
> keyboard unless `vkbForward > 0`, and that counter is only bumped for overlays
> listed in `overlayConsumesKeyboard()` — **a three-entry switch that the OSC
> Settings template does not update.**
>
> So an overlay built by copying that template verbatim will type into the box
> *and* trigger notes. Vital has no on-screen keyboard competing for keystrokes,
> so any focus logic ported from `adi-vst` will be missing exactly the piece
> that matters most here. Add the new tag to `overlayConsumesKeyboard()`.

**The async template to copy** is `FilterAnalysisEvaluator` — small, in-overlay,
and exactly the right shape for a slow LLM call: `std::thread` + `mutex` +
`condition_variable`, results returned via `juce::MessageManager::callAsync`
guarded by a `juce::Component::SafePointer`. Use it rather than inventing one.

---
## 6. Licensing and naming

- Surge XT is **GPLv3** (`LICENSE`). A distributed fork must be GPLv3 too.
  Private use is unrestricted.
- `libs/clap-juce-extensions` is MIT. CLAP itself is MIT with no vendor
  gatekeeper, no SDK agreement and no registration — a point adi_daw ADR-0052
  makes and that applies here too.
- Surge's README records that **VST2 builds may not be redistributed** for
  licensing reasons. We do not build VST2 (ADR-0003), so this is inert, but do
  not turn it on.
- "Surge", "Surge XT" and "Surge Synth Team" are theirs. Settle the naming
  question before any distribution, the way `adi-vst` had to for "Vital" /
  "Vial".
- Factory patches ship with Surge under its own terms — check them before using
  any as AI training data.

---

## 7. Open items

**Decided since the bootstrap:** ADR-0005 settles the AI write path — `surgepy`
first (it is the only thing that can generate the contextual per-type schema
§3.1 requires), then a C++ overlay. OSC is scaffolding, not the product.

- [ ] **Publish the fork `origin`** (ADR-0006). This now blocks ADR-0005's
      step 2 and the one-line `surgepy` `oscName` binding. Adi's call.
- [ ] **Verify the ~766 OSC addresses are unique** with a runtime
      `/q/all_params` sweep. Unasserted and untested upstream; a collision
      writes the wrong parameter silently. Blocks trusting any OSC tool.
- [x] Build `surge-xt_CLAP` on Windows and record the result. Done 2026-09-21:
      builds, `Surge XT.clap` asserted on disk (`collab/win.md`).
- [ ] Build `surgepy` on macOS — upstream CI never does, so `mac` is first.
- [ ] Generate the contextual per-type schema: 32 FX types × 16 slots, 12
      oscillator types. `scripts/misc/surgepy-params.py` is the starting point.
- [ ] Build on macOS/arm64 and record it.
- [ ] Run `ctest -j 4` on both platforms and record the baseline count, the way
      `adi-vst` records "15/15". Windows: **145/145** (§4.4). macOS: pending,
      and anything other than 145 means something differs.
- [ ] Resolve the §3.1 parameter-naming question.
- [ ] Fill in §3.4 (patch format) and §4.1's OSC defaults.
- [ ] Decide whether the schema extractor is an OSC client or a `surgepy`
      script, and write it.
- [ ] Create a fork `origin` — needed the moment `surge/` carries a commit of
      ours (ADR-0006). Adi's call.
- [ ] CI. There is none for this project yet. Follow `.github/workflows/ci.yml`'s
      philosophy: a green tick should prove a specific claim, not merely that it
      compiled.
- [ ] Integration with `adi_daw` — intended, undesigned, and deliberately not
      assumed anywhere in this repo (ADR-0003).
