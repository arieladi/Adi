# drivers/ — OS-level pieces that are their own programs

This directory holds code that runs **outside** the DAW process: kernel or
user-space audio drivers that expose the DAW's audio to the operating system.
Nothing here is linked into `adi_core` or the application, and nothing in
`src/` includes anything from here. The two sides share only the layout of a
lock-free ring buffer, a header of plain C structs that both may copy.

**Licence:** MIT, see [`LICENSE`](LICENSE). The rest of `adi_daw/` is GPLv3
(ADR-0015); MIT code inside a GPLv3 repository is fine in that direction, and
this file plus the licence header on every source file is what keeps the
boundary visible (ADR-0119).

## Planned contents

| Path | What | Base | Status |
|---|---|---|---|
| `adi-virtual-audio/` | The ADI virtual audio device for Windows: a kernel-mode virtual endpoint that presents the DAW's master (or any bus) to the OS as **"ADI DAW Stream Output"** (playback, the DAW routes to it) and **"ADI DAW Stream Input"** (recording, what Zoom, Discord or OBS select). Fed by the sink node of ADR-0074 through a shared ring at the DAW's clock, with a declared latency. | Microsoft's `sysvad` sample from `microsoft/Windows-driver-samples` (MIT) | not started |
| `adi-virtual-audio-mac/` | The macOS equivalent: a user-space AudioServerPlugIn. | A fork of BlackHole (GPL-3.0; the policy pre-authorises it) — **this one is GPL-3.0, not MIT**, and will carry its own licence file | not started |

Linux needs nothing: PipeWire and JACK already route application audio.

## Decisions that govern this directory

- **ADR-0106** — system audio in (no driver needed, WASAPI loopback and Core
  Audio taps) and a virtual device out; "proprietary" and "zero-latency"
  corrected.
- **ADR-0117** — the signing route is secured and confirmed before the driver
  is scheduled; the Windows device ships in the first release only once signed.
- **ADR-0118** — our own `sysvad`-based driver signed through SignPath
  Foundation; bundling VB-CABLE, and renaming any third-party endpoint, is
  rejected. `VirtualDrivers/Virtual-Audio-Driver` is read-only reference: read
  its installer flow and its SignPath pipeline; its MS-PL sample code is not
  copied.
- **ADR-0119** — the endpoint names, this location, and what must exist before
  SignPath Foundation is asked. See [`SIGNING.md`](SIGNING.md).

## What is not here

`reference/` holds the repositories we read. `Virtual-Audio-Driver` and
Synchronous Audio Router are references, not sources; check
`docs/EXTERNAL-CODE.md` before copying a line from anything.
