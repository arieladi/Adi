# drivers/ — OS-level pieces that are their own programs

This directory holds code that runs **outside** the DAW process: kernel or
user-space audio drivers that expose the DAW's audio to the operating system.
Nothing here is linked into `adi_core` or the application, and nothing in
`src/` includes anything from here. The two sides share only the layout of a
lock-free ring buffer, a header of plain C structs that both may copy.

## Licences, stated exactly (ADR-0120)

- **Our own files here** — this README, `SIGNING.md`, the build script, the
  workflow, and any source we write — are **MIT**, see [`LICENSE`](LICENSE).
  The rest of `adi_daw/` is GPLv3 (ADR-0015); MIT inside a GPLv3 repository is
  fine in that direction.
- **The Windows driver is derived from Microsoft's `sysvad` sample**, whose
  licence is the **Microsoft Public License (MS-PL)** — the single licence at
  the root of `microsoft/Windows-driver-samples`; sysvad has none of its own.
  MS-PL is OSI-approved and not GPL-compatible, and `OPEN_SOURCE_POLICY.md`
  does not pre-authorise copying it. So **nothing from the sample is committed
  here**: `build.ps1` fetches it at a pinned commit into an ignored `.build/`
  directory and rewrites only the INF strings. The built package carries the
  MS-PL text and a provenance file. Whether a shipped driver may be
  MS-PL-derived is the director's ruling (ADR-0120, open).
- Earlier entries (ADR-0117 to ADR-0119) called sysvad "MIT". That was wrong
  and is corrected by ADR-0120; the entries stay as written, per ADR-0028.

## Contents

| Path | What | Base | Status |
|---|---|---|---|
| `adi-virtual-audio/` | The ADI virtual audio device for Windows: a kernel-mode virtual endpoint that presents the DAW's master (or any bus) to the OS as **"ADI DAW Stream Output"** (playback, the DAW routes to it) and **"ADI DAW Stream Input"** (recording, what Zoom, Discord or OBS select). Fed by the sink node of ADR-0074 through a shared ring at the DAW's clock, with a declared latency. | Microsoft `sysvad` (MS-PL), fetched at build time | **build pipeline only**: `build.ps1` and `.github/workflows/driver-build.yml` build the unmodified sample with our INF strings and package it unsigned. No ADI driver code exists yet. |
| `adi-virtual-audio-mac/` | The macOS equivalent: a user-space AudioServerPlugIn. | A fork of BlackHole (GPL-3.0; the policy pre-authorises it) — **GPL-3.0, not MIT**, with its own licence file when it exists | not started |

Linux needs nothing: PipeWire and JACK already route application audio.

## The build (`adi-virtual-audio/build.ps1`, `.github/workflows/driver-build.yml`)

```
pwsh ./adi_daw/drivers/adi-virtual-audio/build.ps1 -Configuration Release -Platform x64
```

1. Sparse, blob-less clone of `microsoft/Windows-driver-samples` into
   `.build/`, checked out at the commit pinned in the script. The script
   refuses to continue if the checkout is not that commit or if the sample's
   licence file is no longer MS-PL.
2. The INF strings are rewritten: provider, manufacturer, device description,
   and the friendly names of the sample's two always-present endpoints
   (internal speaker and front microphone array) become "ADI DAW Stream
   Output" and "ADI DAW Stream Input". Every key must exist exactly once, or
   the build fails, so an upstream rename is noticed rather than silently kept.
   The sample's other, jack-detected endpoints keep their names until the real
   driver exposes exactly two.
3. Exactly what the driver INF ships, with the WDK toolset and `SignMode=Off`:
   `EndpointsCommon.vcxproj` (static library), then
   `KeywordDetectorContosoAdapter.vcxproj` (a user-mode COM DLL the INF's copy
   list names, so Inf2Cat requires it), then `TabletAudioSample.vcxproj` (the
   driver). The sample's APOs have their own INF and NuGet dependencies and
   are not built. The runner's kit ships no `InfVerif.dll`, so the WDK's
   in-build INF verification prints errors that do not fail MSBuild; the
   script searches the kit and puts any it finds on `PATH`, and Inf2Cat does
   the real validation.
4. `Inf2Cat` writes `adi-virtual-audio.cat`; the package under `out/` is
   `TabletAudioSample.sys`, `KeywordDetectorContosoAdapter.dll`, the stamped
   `ComponentizedAudioSample.inf`, the `.cat`, `LICENSE-MS-PL.txt` and
   `PROVENANCE.txt`.

The workflow runs on `windows-2022`, which ships the WDK 10.1.26100 with its
Visual Studio extension, and uploads the package as
`adi-virtual-audio-x64-<configuration>-unsigned`. It runs on pushes and pull
requests touching `adi_daw/drivers/**` or the workflow, and on demand.
x64 only for now; ARM64 is a matrix entry away once the driver exists.

**Local builds need the WDK.** Visual Studio 2022 with the C++ workload,
Spectre-mitigated libraries, and the WDK with its VS extension. The
development machine that wrote this had no WDK, so the pipeline was proven in
CI, not locally.

## Decisions that govern this directory

- **ADR-0106** — system audio in (no driver needed) and a virtual device out;
  "proprietary" and "zero-latency" corrected.
- **ADR-0117** — a signing route is secured before the driver is scheduled; the
  Windows device ships in the first release only once signed.
- **ADR-0118** — our own `sysvad`-based driver signed through SignPath
  Foundation; bundling VB-CABLE and renaming any third-party endpoint is
  rejected. `VirtualDrivers/Virtual-Audio-Driver` is read-only reference.
- **ADR-0119** — the endpoint names, this location, and what must exist before
  SignPath Foundation is asked. See [`SIGNING.md`](SIGNING.md).
- **ADR-0120** — sysvad is MS-PL, fetched at build time and never vendored; the
  build workflow; the licence ruling that is still the director's.

## What is not here

`reference/` holds the repositories we read. `Virtual-Audio-Driver` and
Synchronous Audio Router are references, not sources; check
`docs/EXTERNAL-CODE.md` before copying a line from anything.
