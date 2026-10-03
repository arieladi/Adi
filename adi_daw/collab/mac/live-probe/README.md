# live-probe: measuring a Live device so a clone can be nulled against it

This kit drives **Ableton Live 11 Suite on macOS** to render generated probe
signals through a Live device, then replays the same signals through our C++
core and reports the residual, sample for sample. It is how the Multiband
Dynamics device was measured (`../2026-10-03-multiband-dynamics-pd.md`).
Nothing here ships; it is the evidence behind the numbers in
`tests/test_multiband_dynamics.cpp`.

## What it needs

- macOS with Live 11.2.x Suite at `/Applications/Ableton Live 11 Suite.app`,
  running its audio engine at **48 kHz**.
- `python3` with numpy, `swiftc` and `clang++` (Xcode command-line tools).
- Accessibility and Screen Recording permission for the app that runs the
  scripts. The driver clicks Live's Export dialog and reads its window list.
- Once, by hand, in Live's Export Audio/Video dialog: *Rendered Track: All
  Individual Tracks*, *Sample Rate: 48000*, *Bit Depth: 32*, *Create Analysis
  File: Off*. Live remembers these; the driver only presses Export.

Bulky files never enter the repo. Signals, sets and Live's renders live under
`$ADI_LIVE_PROBE_WORK` (default `~/adi-live-probe`).

## Run it

```bash
cd adi_daw/collab/mac/live-probe
swiftc -O -o ~/adi-live-probe/bin/livectl livectl.swift
clang++ -std=c++20 -O2 -ffp-contract=off -I ../../../src mbd_render.cpp ../../../src/adi/dsp/multiband_dynamics.cpp -o ~/adi-live-probe/bin/mbd_render
python3 battery.py            # battery A -> sets/mbd_a.als + mbd_a.json
python3 battery.py b          # battery B
python3 battery.py c          # battery C
python3 render.py ~/adi-live-probe/sets/mbd_a.als   # Live renders every track, ~1 min
python3 nulltest.py mbd_a mbd_b mbd_c               # residual per probe, dB re Live
```

## The pieces

| File | What it does |
|---|---|
| `als.py` | Writes a Live 11 set (gzipped XML) from Live's own default set: one audio track per probe, the WAV as an unwarped arrangement clip at bar 1, the device from a factory preset with every parameter overwritten, optional sidechain routing and automation. Also float WAV I/O. |
| `battery.py` | The probe batteries: impulses for the crossovers, DC staircases for static curves, DC steps for attack/release, sine stairs, stereo/polarity, sidechain, crossed splits, the gain cap, fine 0.02 dB sweeps for the log approximation, automation jumps. Each battery writes a manifest (track -> signal + parameter overrides). |
| `render.py` | Opens a set in Live, presses Cmd-Shift-R, clicks Export, accepts the save panel, waits, and moves the per-track WAVs from `~/Downloads` into `out/<set>/`. |
| `livectl.swift` | Window list, key and click posting by pid, cursor-restoring real clicks: what `render.py` uses to drive Live. |
| `hiir.py`, `osmodel.py` | The half-band design and the float32 2x resampler model that reproduces Live's bit for bit. |
| `mbd_render.cpp` | Offline renderer: WAV in, the C++ core with `-p Name=value` parameters, WAV out. |
| `nulltest.py` | Replays every probe of a set through `mbd_render`, applying Live's clip declick first, and prints RMS and peak residuals. |
| `extract_refs.py` | Pulls the reference numbers the unit test embeds out of Live's renders. |
| `clipfade.txt` | Live's clip declick, measured: 192 samples in, 192 out. Not the device: Live fades every clip edge, `Fade` off or not. |
| `wsc.py` | Reads Live's own parameter names, values and display text through the AdiVST bridge, if it is running. |

## Traps it already fell into

- **Pointee ids.** Every automation and modulation target in a Live set needs a
  unique id, and automation envelopes point at them. Renumbering the template's
  ids crashes Live on load (`Invalid Pointee ID ... ASSERT`); only the added
  tracks are renumbered, with their references remapped.
- **Devices need `Id`.** A device in a `Devices` list without an `Id`
  attribute is "corrupt (Not all list members have Ids)".
- **Background clicks do not reach Live's dialogs**; posted keys do. The save
  panel is out of process, so Return must be a real key event, sent only
  after checking that Live is frontmost.
- **Clip edges are faded** by Live (see `clipfade.txt`). A probe that starts or
  ends on a non-zero sample must model that or start and end in silence.
- **Live saves only into a Project folder**, so a set opened from a bare path
  asks for Save As.
