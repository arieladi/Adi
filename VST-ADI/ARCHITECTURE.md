# VST-ADI — Architecture

Working notes for the AI-powered music production tools. Living document: update
it when a decision changes, don't let it drift.

**Status:** Tool 2 (Vital fork) — toolchain verified, unmodified Vital builds and
outputs clean audio, VST3 target prepared. No AI code written yet.

---

## 1. Repository layout

```
VST-ADI/
├── ARCHITECTURE.md          this file          } tracked in the `Adi` monorepo
├── tools/                   our Python         }
│   ├── extract_schema.py    parameter schema extractor
│   ├── add_vs2019_exporter.py
│   └── out/*.json           generated schema (committed deliberately)
└── vital/                   fork of mtytel/vital — ITS OWN GIT REPO
```

`vital/` is a separate git repository, ignored by the parent `Adi` repo (see its
`.gitignore`). It has an `upstream` remote, so the entire fork delta is always:

```bash
git -C VST-ADI/vital diff upstream/main --stat
```

- `main` — clean mirror of `upstream/main` (currently `636ca0e`). Never commit here.
- `ai-preset-generator` — all our work.

External dependency, outside both repos: **`C:\Users\Adi\Documents\JUCE\JUCE\Projucer.exe`**
(JUCE 6.0.5). See §2.2 — it is the *only* thing used from that download.

---

## 2. Build system

### 2.1 It is Projucer, not CMake

Vital predates JUCE's CMake support. The source of truth is a set of `.jucer`
XML files; Projucer generates the per-platform build files from them:

| `.jucer` | Target | Windows build dir |
|---|---|---|
| `standalone/vital.jucer` | standalone app | `standalone/builds/vs19/` |
| `plugin/vital.jucer` | VST3 + standalone plugin | `plugin/builds/vs19/` |
| `headless/vital.jucer` | CLI renderer | — |
| `tests/vital.jucer` | test runner | — |

**Never hand-edit a `.vcxproj`.** Edit the `.jucer`, then resave. Projucer is a
GUI-subsystem exe, so PowerShell will not wait for it unless you force it:

```powershell
Start-Process -FilePath "C:\Users\Adi\Documents\JUCE\JUCE\Projucer.exe" `
  -ArgumentList "--resave","<path>\vital.jucer" -Wait -NoNewWindow
```

Calling it with `&` returns immediately and silently does nothing.

Projucer does not delete stale generated files. After removing a target, check
for orphaned `.vcxproj` files still on disk but dereferenced from the `.sln`.

### 2.2 The vendored JUCE is patched — do not replace it

`vital/third_party/JUCE/modules` is **not** vanilla JUCE 6.0.5. Verified by diff
against the official release: **39 modified files**, plus an entire LV2 wrapper
Tytel added under `juce_audio_plugin_client/LV2`.

The one that will bite you: `juce_dsp/native/juce_sse_SIMDNativeOps.h` adds
vectorised `div` operations that stock JUCE 6.0.5 does not have. Vital's DSP
calls them. **Building against vanilla JUCE fails to compile.**

Every `.jucer` sets `useGlobalPath="0"` with all `MODULEPATH`s pointing at
`../third_party/JUCE/modules`, so Projucer uses the patched copy automatically.
Don't "fix" this to point at a global JUCE install.

The downloaded `C:\Users\Adi\Documents\JUCE\` tree exists **solely to provide
`Projucer.exe`**. Nothing else from it is used or referenced.

### 2.3 No VS2022 exporter exists

JUCE 6.0.5 (Nov 2020) predates VS2022 (Nov 2021). Projucer 6.0.5's newest MSVC
exporter is **VS2019** (`MSVCProjectExporterVC2019`, toolset v142).

We build those projects with VS2022's **v143** toolset by overriding on the
command line — no project file is pinned to v142, and v142 is not installed:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

# standalone (primary dev target)
& $msbuild "...\vital\standalone\builds\vs19\Vial.sln" `
    /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m

# VST3 plugin
& $msbuild "...\vital\plugin\builds\vs19\Vial.sln" `
    /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m
```

v143 compiles the whole tree cleanly. ~1850 warnings, all pre-existing float/int
conversion noise in Vital's own code — ignore them, don't "fix" them.

### 2.4 Build flags we changed, and why

All in the `.jucer` exporter `extraDefs` / `JUCEOPTIONS`:

| Change | Reason |
|---|---|
| **remove** `INTEL_IPP=1` | Needs Intel IPP's `ipps.h`, not installed. `fourier_transform.h` then falls through to the `juce_dsp` FFT backend — the same path Vital's own Linux builds use. Audio verified clean with this backend. |
| **remove** `REQUIRE_AUTH=1` | Dead define. Set in the `.jucer`, referenced nowhere in `src/`. |
| **add** `NO_AUTH=1` | The real gate is `#if NDEBUG && !NO_AUTH`. Without it a *Release* build puts a Firebase login wall in front of the synth and calls vital.audio — which upstream's README explicitly asks third-party builds not to do. Also removes the Firebase headers entirely. |
| **remove** `IPPLibrary="Sequential"` | Companion to `INTEL_IPP`. |
| `buildVST` 1 → 0 | **No VST2 SDK exists in this repo** (`third_party/VST_SDK/` contains only `VST3_SDK`), and Steinberg no longer licenses VST2. |
| clear `vst3Folder` | Pointed at `../third_party/VST3_SDK`, which does not exist (real path is `VST_SDK/VST3_SDK`). Cleared so JUCE uses its own bundled VST3 SDK in `juce_audio_processors/format_types/VST3_SDK`. |
| remove `vstLegacyFolder` | Pointed at a nonexistent VST2 SDK. |
| **add** `JUCE_VST3_CAN_REPLACE_VST2=0` | Defaults to **1**. JUCE's *VST3* wrapper includes *VST2* headers (`pluginterfaces/vst2.x/vstfxstore.h`) for the VST3-replaces-VST2 feature. With no VST2 SDK this breaks the VST3 build. |
| `enablePluginBinaryCopyStep` 1 → 0 (Windows only) | The post-build step copies the built `.vst3` into `%CommonProgramW6432%\VST3` (`C:\Program Files\Common Files\VST3`), which needs **admin rights** — so a non-elevated build fails at the very last step even though the plugin linked fine. macOS/iOS/Linux configs left untouched. Re-enable, or point it at the user-level `%LOCALAPPDATA%\Programs\Common\VST3`, once there's a DAW to install into. |

### 2.5 Adding a new source file

Two places must be updated, or you get link errors that look like missing symbols:

1. The `.jucer` file list → `Projucer --resave` (regenerates the `.vcxproj`).
2. The matching unity-build stub in `src/unity_build/` — used by the Linux/Xcode
   builds. For a new GUI section that's `interface_editor_sections2.cpp`.

Note `src/unity_build/effects.cpp` references `effects_editor.cpp` and
`effects_plugin.cpp`, which **do not exist** in the public source. The Vital FX
plugin is stripped from this repo; that unity file cannot build. Ignore it.

Sidestep for early prototyping: make the new component header-only and include it
from an existing `.cpp`. Promote to a real translation unit once the API settles.

### 2.6 Language level

`cppLanguageStandard="14"`. **C++14 only** — no `std::optional`, no structured
bindings, no `if constexpr`. This is a project-level `.jucer` setting; raising it
is possible but would be a gratuitous divergence from upstream.

---

## 3. Synth state — the pipeline we hook into

### 3.1 There is no ValueTree

Vital does **not** use `AudioProcessorValueTreeState`. State is:

- `vital::control_map` — `std::map<std::string, vital::Value*>`
  (`src/common/synth_types.h:100`), the live parameter values.
- **nlohmann::json** (`third_party/json/json.h`) for serialisation.

A `.vital` preset file *is* that JSON. The same code path serves preset load,
DAW session restore (`SynthPlugin::setStateInformation`), and — for us — AI
generation. One path to target, not three.

### 3.2 The load path

```
AI JSON string
  └─ json::parse()
     └─ SynthBase::loadFromJson(data)              synth_base.cpp:340
        ├─ pauseProcessing(true)                   takes the audio CriticalSection
        ├─ LoadSave::jsonToState(...)              load_save.cpp:1027
        │  ├─ loadControls()                       load_save.cpp:153
        │  ├─ loadModulations()                    load_save.cpp:170
        │  ├─ loadSample() / loadWavetables() / loadLfos()
        │  └─ loadSaveState()                      name/author/comments/macros
        └─ pauseProcessing(false)
  └─ SynthGuiInterface::updateFullGui()            synth_gui_interface.cpp:70
```

`updateFullGui()` is **not optional** — without it the engine changes but every
knob on screen still shows the old value.

### 3.3 Three consequences that shape the AI design

**a. Partial JSON is legal — exploit this.**
`loadControls` iterates the *synth's* controls and, for any parameter absent from
the JSON, writes `details.default_value` (`load_save.cpp:153-165`). So a preset
does not need all 794 parameters. Emit the 30–60 that matter; everything else
lands on Init defaults. This is the single biggest lever we have: ~95% fewer
output tokens and a correspondingly smaller hallucination surface.

**b. `synth_version` is mandatory and gates the load.**
`jsonToState` reads `data["synth_version"]` unconditionally and returns `false`
if the preset's *feature* version is newer than the build (`load_save.cpp:1028`).
Inject this server-side — never trust the model to emit it. Older versions route
through `updateFromOldVersion()`, ~750 lines of migration.

**c. Wavetables are a size bomb — the model must never generate them.**
Each `WaveSourceKeyframe` base64-encodes 2048 floats
(`wave_source.cpp:193`, `kWaveformSize = 1 << 11`) ≈ 11 KB per keyframe, times 3
oscillators times N frames. This is why `.vital` files run 50 KB – 1 MB.
Strategy: the model selects a wavetable **by name** from a pre-built library, or
omits the field and C++ keeps the current one. Note `loadWavetables` iterating a
missing/null field leaves the *previous* wavetable in place — decide explicitly
which behaviour we want rather than inheriting it by accident.

### 3.4 Existing precedent in the codebase

Vital already ships text-prompt → HTTP → update-synth: **TTWT** (text-to-wavetable),
`oscillator_section.cpp:900`. An `OpenGlTextEditor`, `textEditorReturnKeyPressed`
fires, URL built, JSON returned, synth state updated.

Copy its *structure*, not its transport: it calls `readEntireTextStream()`
**synchronously on the message thread** (line 903). For a short TTS call that's
merely rude; for a 5–15 s LLM generation it would freeze the DAW. See §5.

---

## 4. Parameter schema

Generated from the C++ source, never hand-maintained:

```bash
python VST-ADI/tools/extract_schema.py
```

It parses the `ValueDetails` tables in `synth_parameters.cpp` and the enum label
tables in `synth_strings.h`, then replays the group expansion
`ValueDetailsLookup`'s constructor performs at runtime.

Many fields are C++ constant *expressions*, not literals (`kMaxPolyphony - 1`,
`kNumFilterModels - 1.0`, `factorial(kNumEffects) - 1`), so the extractor also
scans the tree for constexpr definitions and enum members — 1493 constants — to
resolve them. Currently **794 parameters, 0 unresolved**.

### 4.1 Counts

| Group | Per instance | Instances | Total |
|---|---|---|---|
| Global | 145 | 1 | 145 |
| Envelopes | 9 | 6 | 54 |
| LFOs | 12 | 8 | 96 |
| Random LFOs | 8 | 4 | 32 |
| Filters | 20 | 3 (incl. `filter_fx`) | 60 |
| Oscillators | 29 | 3 | 87 |
| Mod slots | 5 | 64 | 320 |
| | | **Total** | **794** |

`out/vital_schema_llm.json` drops the 320 mod-slot params — modulation *routing*
lives in the top-level `"modulations"` array, not in these — leaving **474
sound-design parameters** as the model-facing contract.

Naming: `<prefix>_<instance>_<param>`, e.g. `osc_1_on`, `env_3_decay`,
`filter_2_cutoff`. Filters are `filter_1`, `filter_2`, **`filter_fx`** (not a number).

### 4.2 Value scales

`scale` is not cosmetic — it is how the UI maps a knob position to the stored
value. The stored value is always what goes in the JSON.

| Scale | Count | Note |
|---|---|---|
| Linear | 223 | |
| Indexed | 178 | integers; 144 have resolved enum labels |
| Exponential | 30 | stored as an exponent, e.g. `portamento_time` −10..4 |
| Quartic | 30 | envelope times: `env_1_decay` 0..2.37842 |
| Quadratic | 12 | |
| SquareRoot | 1 | |

Non-obvious ones worth remembering: `filter_*_cutoff` is in **semitones**
(8..136, default 60), not Hz. Envelope times are **quartic** — 1.0 is not
"one second" and not the midpoint.

### 4.3 Validator rules

The server must clamp/validate before anything reaches C++:

1. Reject unknown parameter names outright (do not silently drop — that hides
   model drift we want to see).
2. Clamp to `[min, max]`.
3. Round `Indexed` params to integers.
4. **`destination` has an off-by-one in Vital's own table.** Its max is
   `kNumSourceDestinations + kNumEffects` = 14, but `kDestinationNames` has
   exactly 14 entries, so index 14 reads out of bounds. Clamp to **13**.
   Applies to `osc_N_destination` and `sample_destination`.
5. Inject `synth_version` server-side.
6. Strip any `wavetables` the model tried to emit.

---

## 5. Threading model

Three threads. Getting this wrong crashes the DAW, so it is worth stating
precisely.

| Thread | Role |
|---|---|
| **Audio** | Never touched by us. `pauseProcessing(true)` takes the callback lock, which is how the load is made safe. |
| **Network** | A `juce::Thread` doing the HTTP POST. Owns no synth state, touches no JUCE components. |
| **Message** | Receives the result via `MessageManager::callAsync`, then calls `loadFromJson` + `updateFullGui`. |

Pattern to copy: `DownloadSection::DownloadThread` (`download_section.h:56`) — a
small `juce::Thread` subclass holding a back-pointer.

**Lifetime safety is the real hazard.** If the editor or plugin is destroyed
while a request is in flight, the `callAsync` lambda would touch freed memory —
and a user closing the plugin window mid-generation is an entirely normal thing
to do. Vital already solves this: `SynthBase` keeps
`std::shared_ptr<SynthBase*> self_reference_`, and `ValueChangedCallback` holds a
`std::weak_ptr` it locks before use (`synth_base.h`). **Use the same weak_ptr
guard** — do not capture raw `this` in the async callback.

Also: the network thread must be joined or cleanly abandoned in the destructor.
`stopThread(timeout)` before teardown.

---

## 6. GUI — where the prompt bar goes

### 6.1 Why not the header

`HeaderSection::resized()` (`header_section.cpp:231`) packs logo, tab selector,
preset selector (`width/3`, centred), volume section and oscilloscope edge to
edge with no slack. The widths are computed from skin values and from each
other — `volume_width = (width - preset_right - 2*padding) / 2`, oscilloscope
derived from that. Dropping an always-visible text field in there means
rewriting tuned layout maths and fighting every skin.

### 6.2 The plan: an `Overlay` subclass + one trigger button

Vital has a purpose-built modal overlay base class: `Overlay : SynthSection`
(`src/interface/editor_components/overlay.h`), with a dimmed GL background, a
shown/hidden `Listener`, and existing users in `SaveSection` and `DeleteSection`.
It is exactly the right shape, needs no layout surgery, and keeps our diff small.

**New files**

| Path | Contents |
|---|---|
| `src/interface/editor_sections/prompt_section.h` | `class PromptSection : public Overlay, public TextEditor::Listener` |
| `src/interface/editor_sections/prompt_section.cpp` | layout, request kickoff, result handling |

`PromptSection` owns:
- an `OpenGlTextEditor` for the prompt (`open_gl_image_component.h:111`) — must be
  the GL-wrapped variant, because `FullInterface` is an `OpenGLRenderer` and a
  bare `juce::TextEditor` will not composite correctly;
- a `PlainTextComponent` for status/error, mirroring `ttwt_error_text_`;
- the network `juce::Thread`.

**Modified files**

| Path | Change |
|---|---|
| `src/interface/editor_sections/full_interface.h` | add `std::unique_ptr<PromptSection> prompt_section_` |
| `src/interface/editor_sections/full_interface.cpp` | construct it; `addSubSection(ptr, false)` + `addChildComponent(ptr)`; `setBounds(bounds)` in `resized()`; `toFront(true)` alongside the other overlays — mirror `save_section_` at lines 155-158, 192, 485 |
| `src/interface/editor_sections/header_section.h/.cpp` | one `OpenGlShapeButton` trigger; in `resized()` place it just left of the preset selector and shrink `tabs_width` by its width + padding. The tab strip holds 3–4 short labels and has room to give. |
| `src/unity_build/interface_editor_sections2.cpp` | `#include "prompt_section.cpp"` |
| `plugin/vital.jucer`, `standalone/vital.jucer` | add both new files, then Projucer resave |

**Why an overlay rather than an inline bar:** room for prompt + status + errors +
eventual history, no skin-layout fights, one proven pattern, and a small
reviewable diff against upstream.

### 6.3 Control flow

```
HeaderSection button  →  Listener  →  FullInterface::showPromptSection()
                                        → prompt_section_->setVisible(true)

PromptSection::textEditorReturnKeyPressed()
  → spawn network thread (prompt text copied by value)
  → show "generating…"

[network thread]  POST → JSON string
  → MessageManager::callAsync( weak_ptr guard )

[message thread]
  → SynthGuiInterface::getSynth()->loadFromJson(parsed)
  → updateFullGui()
  → setVisible(false)   or   show error, mirroring TTWT's kShowErrorMs timer
```

`SynthSection::findParentComponentOfClass<FullInterface>()` is the idiom Vital
uses to reach up the hierarchy (see `OscillatorSection::loadBrowserState`).
`SynthGuiInterface` is reachable from any `SynthSection` and is the correct
seam for touching the synth.

### 6.4 Keyboard input inside a DAW

`pluginEditorRequiresKeys="1"` is already set in `plugin/vital.jucer`, so typing
works in a hosted plugin. Nothing to change — but don't let a future Projucer
edit clear it.

---

## 7. Licensing and naming constraints

- Vital is **GPLv3**. A distributed fork must be GPLv3 too. Private use is
  unrestricted.
- Upstream's README forbids using the names **"Vital", "Vital Audio", "Tytel",
  "Matt Tytel"** for any distribution built from this source.
- The public `.jucer` is already named **"Vial"** — Tytel's trademark-stripped
  name. The built binary is `Vial.exe`. That is expected, not a misconfiguration.
- Do not connect to `vital.audio`, `account.vital.audio` or `store.vital.audio`
  from our builds. `NO_AUTH=1` handles the login path; the preset-store links in
  `preset_browser.cpp` / `popup_browser.cpp` and the TTWT endpoint are separate
  and should be neutralised before any distribution.
- **No factory presets ship in this repo** — they are under a separate
  non-redistributable licence. Training data must come from our own library plus
  permissively-licensed community packs.

---

## 8. Open items

- [ ] Prompt bar + async bridge (§6) — next up.
- [ ] Count available `.vital` presets for training. Under ~500 pairs, discuss
      synthetic augmentation.
- [ ] Server-side validator implementing §4.3.
- [ ] **Tesla P100 is Pascal (SM 6.0): no bfloat16, no Flash Attention 2**
      (Ampere+ only). Llama 3 8B + LoRA runs on fp16 with xformers-style
      attention, slower than most guides assume. Not a blocker; confirm before
      building a pipeline around it.
- [ ] Tool 1 (sample engine) — not started.
