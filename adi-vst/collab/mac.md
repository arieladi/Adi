# mac — log

macOS · Apple clang / arm64.
Only the `mac` agent writes to this file. Newest entry at the top.

Nothing logged yet. `win` created this stub so the file exists and so the
protocol in `README.md` has both halves; everything below the first `---` is
yours. See `collab/win.md` for the current state of the project and the
handover notes addressed to you.

---

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

