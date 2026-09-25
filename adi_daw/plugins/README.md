# Plug-ins

ADI's plug-in line as CLAP (ADR-0166): two of our own, and five open-source
projects built from their upstream code, unchanged. Each is its own binary under
its own licence; none links into the DAW.

| Plug-in | Where | Upstream, pinned | CLAP id | Our build's licence |
|---|---|---|---|---|
| **ADI RMSC**: ring-modulation sidechain ducking | [`rmsc/`](rmsc) | ours; DSP in `src/adi/dsp/rmsc.*` | `com.adi.rmsc` | AGPLv3 (JUCE 9) |
| **ADI Airwindows**: 141 effects, each its own plug-in, auto gain | [`airwindows/`](airwindows) | [airwin2rack](https://github.com/baconpaul/airwin2rack) `b6eef0a`, MIT | `com.adi.airwindows.<name>` | GPLv3 |
| **Smartelectronix**: Smexoscope, Anechoic Room Simulator, Bitmurderer, Bouncy, Crazy Ivan, Cyanide 2, H2O, MadShifta, One Ping Only, SupaPhaser, SupaTrigga | [`external/smartelectronix/`](external/smartelectronix) | [bdejong/smartelectronix](https://github.com/bdejong/smartelectronix) `248d2c4`, GPL-3.0 | `com.adi.smartelectronix.<name>` | AGPLv3 (JUCE 8) |
| **ChowTapeModel** | its own tree | [AnalogTapeModel](https://github.com/jatinchowdhury18/AnalogTapeModel) `604372e`, GPL-3.0 | `org.chowdsp.CHOWTapeModel` (theirs) | GPLv3 |
| **ChowCentaur** | [`external/chowcentaur/`](external/chowcentaur) | [KlonCentaur](https://github.com/jatinchowdhury18/KlonCentaur) `f3bb633`, BSD-3-Clause | `com.adi.chowdsp.chowcentaur` | GPLv3 (JUCE 6) |
| **ZL Equalizer 2**: the baseline for our equalizer | [`external/zlequalizer/`](external/zlequalizer) | [ZLEqualizer](https://github.com/ZL-Audio/ZLEqualizer) `3468a3a`, AGPL-3.0 | `com.adi.zlaudio.zlequalizer2` | AGPLv3 |
| **Dragonfly** Hall, Room, Plate, Early Reflections | [`external/dragonfly/`](external/dragonfly) | [dragonfly-reverb](https://github.com/michaelwillis/dragonfly-reverb) `440ec7b`, GPL-3.0 | `michaelwillis.dragonfly.<name>` (theirs) | GPLv3 |

**The ids say whose build it is.** Where upstream releases a CLAP itself
(ChowTape, Dragonfly), ours keeps its id, so a project saved with theirs opens
with ours. Where it never has, the id is ours: one under their name would claim
a release they never made.

**Upstream is never patched.** An `external/<name>/CMakeLists.txt` adds the
upstream tree as it is and adds a CLAP target: clap-juce-extensions for JUCE
projects, or DPF's own CMake for Dragonfly. Each file says why it looks the way
it does: Centaur's older clap-juce-extensions pin, Dragonfly's symlinks, ZL
under MSVC.

## Airwindows

[`airwindows/CATALOGUE.md`](airwindows/CATALOGUE.md) lists all 524 effects:
- what each does;
- its controls;
- whether it is kept, and why not if it isn't;
- where auto gain starts on.

141 are kept: Chris Johnson's own Recommended and Basic collections, plus
Density3, Pressure5 and Channel9. `tools/airwindows_catalogue.py` writes the
catalogue, the C++ table (`Source/aw_catalogue.inc`) and the source list
(`effects.cmake`). Edit the script, never its output.

**Why not Airwindows Consolidated**, which is already a CLAP with a UI? It is
one plug-in with a selector. Its parameters are slots whose meaning changes with
the selection, so automation, or a stored value, on a slot silently retargets.
In ours every effect is its own plug-in, with parameters that keep their
meaning.

**No editor**, as Airwindows never had one: the host draws the parameter panel.
**Auto gain** (`src/adi/dsp/auto_gain.*`) is a parameter on every effect, and
starts on for the 19 tone effects that get louder as they colour and have no
output control.

## Build

```bash
bash tools/fetch_plugins.sh C:/Users/<you>/Documents/GitHub/Adi-wt/plugins
```

That fetches the sources, pinned by commit, submodules included. On Windows,
keep this directory and the build directories short: JUCE's generated paths
overrun MAX_PATH from deep ones.

**Windows, MSVC and Ninja** (the batch files set up Visual Studio 2022):

| What | Command |
|---|---|
| RMSC, and ADI Airwindows when `ADI_AIRWIN_SOURCE` is set | `tools\build-plugins.bat [target]` |
| One upstream: `smartelectronix`, `chowtape`, `chowcentaur`, `zlequalizer`, `dragonfly` | `tools\build-external-plugin.bat <name> [target]` |

`build-external-plugin.bat` reads `ADI_PLUGIN_SOURCES` (the fetch directory)
and `ADI_PLUGIN_BUILD` (default `build-external`). It lists the `.clap` files it
finds.

| Build | Time on this machine |
|---|---|
| smartelectronix | about 14 minutes (eleven JUCE 8 builds) |
| ChowTape | about 70 s |
| ZL | about 80 s |
| Centaur | about 40 s |
| Dragonfly | about 12 s |

## Check

```bash
build-plugins/rmsc/adi_rmsc_tests
build-plugins/airwindows/adi_airwindows_tests
build-juce/adi_play_artefacts/Debug/adi_play --list --search <folder of .clap files>
```

The two test suites drive the plug-ins as a host does. `adi_play --list` shows
what the DAW's scanner finds. `adi_play <project> --search <dir> --render
<seconds>` plays a project through them offline, with no audio device.
