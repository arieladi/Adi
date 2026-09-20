# mac — log

macOS · Apple clang / arm64.
Only the `mac` agent writes to this file. Newest entry at the top.

Nothing logged yet. `win` created this stub so the file exists and so the
protocol in `README.md` has both halves; everything below the first `---` is
yours. See `collab/win.md` for the current state of the project and the
handover notes addressed to you.

---

## 2026-09-20 (later still) — INSTRUCTIONS FOR win: the macOS exporter in `plugin/vital.jucer`

Adi's call: **win is sole owner of `Vital.jucer`.** I am not building Projucer
locally and not touching the file. Everything below is a request, verified
against the file as it stands at `ai-preset-generator`.

Every value is quoted verbatim. `&#10;` is the newline entity the `.jucer` uses —
keep it. Resave with `Projucer --resave` afterwards; a resave rewrites *all*
exporters, so keep this in its own short-lived PR (README's own advice).

### A. `<XCODE_MAC …>` tag attributes

| Attribute | Current | Change to | Why |
|---|---|---|---|
| `extraDefs` | `JUCE_OPENGL3=1&#10;REQUIRE_AUTH=1` | `JUCE_OPENGL3=1&#10;NO_AUTH=1&#10;JUCE_VST3_CAN_REPLACE_VST2=0` | `REQUIRE_AUTH` is dead (ADR-0006). **`NO_AUTH=1` is mandatory on arm64** — without it the link fails outright, see below. `JUCE_VST3_CAN_REPLACE_VST2=0` mirrors ADR-0007. |
| `extraCustomFrameworks` | `firebase.framework&#10;firebase_auth.framework` | *(empty)* | **This one line is 561 of the 797 warnings.** |
| `frameworkSearchPaths` | `../../../third_party/firebase_cpp_sdk/frameworks/darwin/` | *(empty)* | Companion to the above. |
| `externalLibraries` | `gssapi_krb5` | *(empty)* | Firebase dependency only. |
| `extraFrameworks` | `Security,GSS` | *(empty)* | Firebase dependencies only. **Verify the link after** — if anything else pulled them in, put `Security` back alone. |
| `iosDevelopmentTeamID` | `EFXDM6K3KJ` | *(empty)* | Tytel's team. It is a hard build blocker for anyone without that cert, and it is third-party identity. |
| `postbuildCommand` | `if [ -f ~/Library/Audio/Plug-Ins/Components/Vital.component ]; then&#10;  auval -v aumu Vita Tyte&#10;fi` | *(empty)* | Validates an AU we no longer build, and names a restricted mark. |
| `customPList` | the `NSAppTransportSecurity` block for **`tytel.org`** | *(empty)* | Grants `NSTemporaryExceptionAllowsInsecureHTTPLoads` + TLS 1.1 to a domain §7 forbids contacting. |
| `vst3Folder` | `../third_party/VST3_SDK` | *(empty)* | Path does not exist (real one is `VST_SDK/VST3_SDK`). ADR-0007 already cleared this on Windows; macOS was missed. |
| `vstLegacyFolder` | `../third_party/VST_SDK/VST2_SDK` | **delete the attribute** | Nonexistent VST2 SDK. ADR-0007 removed it on Windows. |

### B. Both `<CONFIGURATION>` elements inside `<XCODE_MAC>` (Debug **and** Release)

| Attribute | Current | Change to | Why |
|---|---|---|---|
| `enablePluginBinaryCopyStep` | `1` | `0` | **The blocker that cost me four builds.** VS2017/VS2019 are already `0`; ADR-0007 says macOS was "left untouched". On macOS it does not fail politely — Xcode gates an `MkDir` of the real `~/Library/Audio/Plug-Ins/VST3/` against build-directory creation and reports `Cycle inside a single target … rooted at /`, naming neither the copy step nor the path in the headline. |
| `osxCompatibility` | `10.12 SDK` | `10.13 SDK` | Xcode 16: *"deployment target is set to 10.12, but the range of supported versions is 10.13 to 15.5.99."* |
| `fastMath` | `1` | **leave at `1`** | See §D. |

### C. `<JUCERPROJECT>` format flags — read the caveat first

| Attribute | Current | Change to |
|---|---|---|
| `buildAU` | `1` | `0` |
| `buildAUv3` | `1` | `0` |
| `buildStandalone` | `1` | `0` |

**The caveat, because this is the one that could bite you.** These are
project-level, so they hit Windows too. `buildStandalone` here is the *Standalone
Plugin wrapper inside `plugin/vital.jucer`* — **not** your primary dev target.
That is `standalone/vital.jucer`, a separate file this request does not touch, and
it stays exactly as it is. I have verified they are two distinct files. If you
disagree, leave `buildStandalone` alone and say so — losing your dev loop is worse
than one extra target.

`buildVST=0`, `buildRTAS=0`, `buildAAX=0` are already correct. Naming
(`pluginName="Vial"`, `pluginManufacturer="Vial Audio"`, `bundleIdentifier=
"audio.vial.synth"`) is fine — `Vial` is upstream's own trademark-stripped name
(§7), so nothing there needs changing.

### D. The remaining 133 warnings — my recommendation is to do nothing

`-Wnan-infinity-disabled` × 133 comes from `fastMath="1"` → `GCC_FAST_MATH=YES`.
Two ways to silence it, and I recommend **neither, for now**:

- `fastMath="0"` would remove them, but that is a real DSP behaviour change and it
  is the *same* change entangled with the open question of whether
  `isnan()`/`isinf()` get folded to constant false — which would make ADR-0009's
  "no NaN or Inf in the output" check vacuous. Adi has explicitly deferred that to
  a NaN-injection test. Do not pre-empt it with a build-flag change.
- `-Wno-nan-infinity-disabled` would hide exactly the signal that test needs.

They are also harmless as they stand: **all 133 are third-party** —
`juce_CharacterFunctions.h` (120), `json.h` (8), `juce_Javascript.cpp` (4),
`juce_VST3_Wrapper.cpp` (1) — text parsing using infinity as a sentinel.
**Zero fire anywhere in `vital/src/`.** Revisit after the NaN test, not before.

The other residue needs no action: 63 `-Wunused-function` is unity-build noise
(`spectral_morph.h`'s header-inline morph helpers are unused in most of the 5 TUs
that include them); 25 `-Wdeprecated-ofast` is clang 17 deprecating `-Ofast` and
will matter at some future clang, not now; 2 `-Wshorten-64-to-32` are inside JUCE.

### E. Why `NO_AUTH=1` is not optional on macOS

Worth putting in ADR-0006's consequences. Without it the arm64 link fails:

```
ld: warning: ignoring file '.../firebase.framework/firebase(..._forkunsafe.o)':
    found architecture 'x86_64', required architecture 'arm64'
Undefined symbols for architecture arm64:
  "firebase::g_auth_initializer", "firebase::App::GetInstance()",
  "firebase::auth::Auth::GetAuth(...)", "firebase::auth::User::GetToken(bool)", …
```

`lipo -info` on `third_party/firebase_cpp_sdk/frameworks/darwin/firebase.framework/firebase`
returns **`Non-fat file: … is architecture: x86_64`**. arm64 slices exist only
under `libs/ios/`. The vendored SDK predates Apple Silicon. So on Apple Silicon
`NO_AUTH=1` is not a don't-phone-home preference — **it is the only way the macOS
VST3 links.** Anyone restoring `REQUIRE_AUTH` for cross-platform parity breaks
the macOS build outright.

### F. How to check you have it right

After the resave, this should build with **no overrides at all**:

```bash
cd adi-vst/vital/plugin/builds/osx
xcodebuild -project Vial.xcodeproj -target "Vial - VST3" -configuration Release build
```

That is the acceptance test: today the same build needs six command-line
overrides, and each one is a defect in the file. Expect ~236 warnings rather than
797, and `xcodebuild -list` should show only `Vial - VST3` and `Vial - Shared Code`.

I will re-run it on arm64 and report. I have **not** changed `plugin/vital.jucer`,
`standalone/vital.jucer`, or anything else in the fork — the fork's working tree
is clean and nothing has been committed to `adi-vst-synth`.


## 2026-09-20 (later) — macOS VST3 sprint: cloned, and the first wave

Unblocked by ADR-0016. Cloned `arieladi/adi-vst-synth` into `adi-vst/vital`;
`git check-ignore` confirms the monorepo ignores it and `git status` at the root
stays clean, so no C++ can leak into `Adi`. Working on `ai-preset-generator`
(fork `main` is the upstream mirror, per ADR-0001).

**Added the `upstream` remote back.** A fresh clone of `adi-vst-synth` has only
`origin`, so `git diff upstream/main` — the one command ADR-0001 exists to keep
working — was dead on arrival for anyone cloning after me. ADR-0016 says "the
fork keeps both remotes"; that is true of win's working copy, not of a clone.
Worth a line in the README's clone instructions.

Toolchain: Xcode 16.4, Apple clang 17.0.0, cmake 4.3.3, arm64, macOS 15.5 SDK.

### First wave — and it is not a compile error

`xcodebuild -target "Vial - VST3"` fails **before compiling anything**:

```
error: No signing certificate "Mac Development" found: No "Mac Development"
signing certificate matching team ID "EFXDM6K3KJ" with a private key was found.
```

That is Tytel's team ID, baked into the exporter. Rebuilt with
`CODE_SIGN_IDENTITY="" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO
DEVELOPMENT_TEAM=""` to get past it. The team ID should come out of the `.jucer` regardless — it is both a
build blocker and third-party identity we should not be carrying.

### The build progression — four blockers, all config, none in Vital's own code

Each fix is a **command-line override**, not a project edit: ADR-0002 rules out
hand-editing `Vial.xcodeproj`, and there is no Projucer here to resave with. So
these are the diagnosis, not the patch; the patch goes in the `.jucer`.

| # | Blocker | Override that cleared it |
|---|---|---|
| 1 | Signing: no cert for team `EFXDM6K3KJ` (Tytel's) | `CODE_SIGNING_ALLOWED=NO DEVELOPMENT_TEAM=""` |
| 2 | Xcode 16 dependency cycle at `CreateBuildDirectory`, rooted at `/` | see 3 — this was a symptom, not the cause |
| 3 | **`DEPLOYMENT_LOCATION=YES` + `INSTALL_PATH="$(HOME)/Library/Audio/Plug-Ins/VST3/"`** | `DEPLOYMENT_LOCATION=NO SKIP_INSTALL=YES` |
| 4 | **Firebase auth link failure on arm64** | `-DNO_AUTH=1` |

Blocker 3 is the plugin binary copy step — the same one ADR-0007 disabled on
Windows because it "needs admin rights and failed the build after a successful
link", and which that ADR says was **"left untouched"** on macOS. On macOS it
fails differently and earlier: Xcode gates an `MkDir` of the real
`~/Library/Audio/Plug-Ins/VST3/` against build-directory creation and reports a
cycle rooted at `/`, with no mention of the copy step anywhere in the message.
Three separate wrong guesses (`EAGER_LINKING=NO`, relocating `SYMROOT`,
overriding `CONFIGURATION_BUILD_DIR`) before reading the trace properly. The
cycle trace names the install path in its first `node:` — read that first.

Blocker 4 is the one worth an ADR amendment. With the copy step gone, 25 files
compiled and it reached the linker:

```
ld: warning: ignoring file '.../firebase.framework/firebase(..._forkunsafe.o)':
    found architecture 'x86_64', required architecture 'arm64'
Undefined symbols for architecture arm64:
  "firebase::g_auth_initializer", "firebase::App::GetInstance()",
  "firebase::auth::Auth::GetAuth(...)", "firebase::auth::User::GetToken(bool)", ...
```

`lipo` confirms it: `third_party/firebase_cpp_sdk/frameworks/darwin/firebase.framework/firebase`
is **`Non-fat file: ... is architecture: x86_64`**. arm64 slices exist only under
`libs/ios/`. The vendored Firebase SDK predates Apple Silicon.

**So on Apple Silicon `NO_AUTH=1` is not a licensing preference, it is the only
way the macOS VST3 links at all.** ADR-0006 justifies it on Windows as "don't
phone home"; on arm64 macOS it is a hard build requirement, and the macOS
exporter is the one place it was never set. Worth adding to ADR-0006's
consequences, because anyone who "restores" `REQUIRE_AUTH` for parity breaks the
macOS build outright.

### Result: the macOS arm64 VST3 builds

```
** BUILD SUCCEEDED **
plugin/builds/osx/build/Release/Vial.vst3/Contents/MacOS/Vial
  Mach-O 64-bit bundle arm64   (Non-fat, 7.8 MB)
  nm -u | grep -ci firebase  ->  0
  nm -g | grep GetPluginFactory  ->  T _GetPluginFactory
```

Native Apple Silicon, no Firebase symbols linked, VST3 entry point exported.
Reproduce from `plugin/builds/osx`:

```bash
xcodebuild -project Vial.xcodeproj -target "Vial - VST3" \
  -configuration Release -arch arm64 \
  CODE_SIGN_IDENTITY="" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO \
  DEVELOPMENT_TEAM="" DEPLOYMENT_LOCATION=NO SKIP_INSTALL=YES \
  OTHER_CPLUSPLUSFLAGS='$(inherited) -DNO_AUTH=1' build
```

**Read that command as a diagnosis, not a build recipe.** A clean checkout still
does not build — every one of those overrides is a `.jucer` defect that has to be
fixed at source. Nothing is green until the command is just
`xcodebuild -target "Vial - VST3"`.

Not yet done, and not claimed: the plugin has **not** been loaded in a host, and
`tools/validator` is a Windows-side artifact I have not built here — so "it links
and exports a factory" is the whole of what is verified. No audio has been
rendered on macOS.

### The predicted Clang 17 / arm64 compiler errors did not happen

Clean-room rebuild — `rm -rf build ~/Library/Developer/Xcode/DerivedData/Vial-*`
then `xcodebuild clean build`:

```
** BUILD SUCCEEDED **     errors=0     warnings=797     TUs=25
```

25 translation units is not a partial build: `src/unity_build/` holds 10 unity
files and the rest are JUCE module TUs plus `BinaryData`. That is the whole
codebase.

No missing standard-library headers, and no implicit-conversion *errors*. The
Windows build's ~1850 conversion warnings (ADR-0004) do not have a macOS
counterpart — Apple clang 17 at Vital's warning level emits a different and much
smaller set. Full corpus:

| Count | Warning | Where |
|---|---|---|
| **561** | `ignoring file … found architecture 'x86_64', required 'arm64'` | **all 561 Firebase** |
| 133 | `-Wnan-infinity-disabled` | JUCE + json only; **0 in Vital's DSP** |
| 63 | `-Wunused-function` | `spectral_morph.h` (50), `synth_constants.h` (12) |
| 25 | `-Wdeprecated-ofast` | `-Ofast` deprecated in clang 17 |
| 3 | `-Wdeprecated-declarations` | — |
| 2 | `-Wshorten-64-to-32` | JUCE `gui_basics` / `graphics` |
| 2 | `MACOSX_DEPLOYMENT_TARGET` 10.12 below supported 10.13 | project setting |

**70% of all warnings are one root cause.** Every one of the 561 is an object
from `firebase_auth.framework` / `firebase.framework` being skipped as x86_64.
`NO_AUTH=1` compiles the Firebase *headers* out but the frameworks are still on
the **link line**. The macOS exporter should stop linking them entirely under
`NO_AUTH`, not merely stop calling them. That is a one-line `.jucer` change and
it removes 561 warnings.

**The `-Wnan-infinity-disabled` cluster is not the DSP hazard it first looks
like.** All 133 are third-party: `juce_CharacterFunctions.h` (120), `json.h` (8),
`juce_Javascript.cpp` (4), `juce_VST3_Wrapper.cpp` (1) — text parsing using
infinity as a sentinel. **Zero fire in `vital/src/synthesis` or `vital/src/common`.**

It does raise one question I am *not* claiming an answer to. `GCC_FAST_MATH = YES`
is set in all 10 build configurations (upstream's own setting, inherited). Under
`-ffast-math`, `-ffinite-math-only` permits the compiler to fold `isnan()` /
`isinf()` to constant false. ADR-0009's validator asserts **"no NaN or Inf in the
output"**. If that check is compiled away, the assertion is vacuous — and the
Windows baseline would not transfer, because MSVC `/fp:fast` is less aggressive
here than clang's `-ffast-math`. Worth one experiment before the macOS validator
is trusted: feed a deliberately NaN-producing state and confirm the check still
fails. I have not run it.

The `-Wunused-function` count is unity-build noise: `spectral_morph.h`'s morph
helpers are header-inline and unused in most of the 5 TUs that include them.

### Four latent breakages in the macOS exporter, all of the class win predicted

The `XCODE_MAC` block never received the fixes ADR-0005/0006/0007 applied to the
Windows exporters. Reading `plugin/vital.jucer:844-877`:

1. **`extraDefs="JUCE_OPENGL3=1&#10;REQUIRE_AUTH=1"`** — macOS still sets the
   dead `REQUIRE_AUTH` and **never sets `NO_AUTH=1`**. So a Release macOS build
   gets exactly the Firebase login wall ADR-0006 removed on Windows. This is the
   single most important fix and it is required by the build mandate.
2. **`vst3Folder="../third_party/VST3_SDK"`** — the nonexistent path ADR-0007
   cleared on Windows (real path is `VST_SDK/VST3_SDK`). Still here.
3. **`customPList` carries an `NSAppTransportSecurity` exception for `tytel.org`**
   with `NSTemporaryExceptionAllowsInsecureHTTPLoads` and a TLS 1.1 minimum. We
   must not contact that domain at all (§7), and an insecure-HTTP exemption is
   not something to ship by inheritance.
4. **`postbuildCommand` runs `auval -v aumu Vita Tyte`** against
   `~/Library/Audio/Plug-Ins/Components/Vital.component` — an AU validation step
   for a format the mandate disables, referencing a restricted mark.

Also `buildAU="1"`, `buildAUv3="1"`, `buildStandalone="1"` — all three must go to
VST3-only, and `JUCE_VST3_CAN_REPLACE_VST2=0` is absent on macOS.

### Two blockers on the mandate as written

**CLAP is not a configuration toggle — it does not exist here.** JUCE 6.0.5
predates CLAP entirely: zero occurrences in
`third_party/JUCE/modules/juce_audio_plugin_client/`, zero in `plugin/vital.jucer`,
and `third_party/` has no `clap-juce-extensions`. Shipping CLAP means vendoring
`free-audio/clap-juce-extensions` plus the CLAP SDK and adding a CMake path
alongside the Projucer build — a real piece of work with an ADR attached, not a
flag. I have not started it. VST3 first; CLAP as its own branch once VST3 is green.

**There is no Projucer on this machine**, and ADR-0002 forbids hand-editing
generated projects — fixes go in the `.jucer` then `Projucer --resave`. The
vendored JUCE ships no Projucer source (`third_party/JUCE/extras/` has none), so
there is nothing to build locally either. Options, in the order I would take them:
build Projucer from a separate JUCE 6.0.5 download (ADR-0003's host-side-tooling
carve-out already permits a stock JUCE for tooling); or, if that stalls, edit the
`.jucer` by hand and regenerate on win's machine. I will not hand-edit
`Vial.xcodeproj` — that is the one thing ADR-0002 rules out, and it would be
silently reverted by the next resave.

### Unrelated, but it needs saying before anyone resets a branch

`origin/main` was force-pushed to a history with **no common ancestor** with my
local `main` (`git merge-base` returns nothing; `git pull` refused with
"unrelated histories"). Two consequences:

- **My local `main` holds Stream Deck work that is on no remote.** V66, V67, the
  V65 revert, the Batch 37 post-mortem, the V65 re-apply, V68 and V69 — all
  present locally, all absent from the new `origin/main`, which stops at V65. I
  have left `main` untouched and branched from `origin/main` instead. Anyone who
  runs `git reset --hard origin/main` on this clone destroys that work.
- **My review of 2026-09-20 did not survive the rewrite.** `adi-vst/collab/mac.md`
  on the new `main` is the original stub. The review and ADR-0012..0015 are on
  `mac/vst-adi-review` (`4b2ec1a`) at the old `VST-ADI/` paths.

And the ADR numbers now collide, which is the exact failure `adi_daw` ADR-0051
exists to prevent — I carried its reservation table into VST-ADI in that same
commit and the rewrite dropped it. win has since written 0012–0016. Two of those
are findings from my review, now landed: **ADR-0012 (phantom parameters excluded
from the model-facing schema)** and **ADR-0013 (keep the MIDI-CC emulation; MPE
depends on it)**. Good. The three that have not landed are the ADR-0008
supersession (projection + server-side resolve), the ADR-0010 re-justification,
and the schema-artifact gate. I will re-land those as **0017–0019** once the
build is moving, and re-add the reservation table in the same commit.

