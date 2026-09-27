# Bring-up

The device loads and draws; see the status note in the README for the four
bugs the first run exposed. Bring it up in the checkpoint order below rather
than judging the whole thing at once — each stage isolates one layer, so a
failure tells you where to look.

**Replace any device you have already opened** with the regenerated
`AVC Spectrum Meter.amxd`. Hand edits made in Max (a `dim` message box, an
`@adapt 0` attribute on `jit.gen`) are neither needed nor correct now: the
default `@adapt 1` is what makes the reduction output follow the geometry
matrix, and `@adapt 0` without a matching `dim` would freeze it at the wrong
size.

## The settings panel

The spectrum and meters occupy the whole 155 px. The **SET** button in the
plot's top-left corner reveals three rows of parameter controls overlaying the
bottom of the display, and hides them again. They are normal `live.*`
parameters either way, so presets and stored values behave identically whether
the panel is open or closed.

## Diagnostics

Both js helpers answer a `status` message and are otherwise silent. Send
`status` to the `js avc.engine.js` object and it reports whether both pfft~
engines exist, the block size, overlap, sample rate and matrix name; the same
message to `js avc.meters.js` reports the filter objects and ballistics. If an
engine reads `MISSING`, the pfft~ was not created — check that
`avc.fftanalysis.maxpat` sits next to the `.amxd`.

`probe` (to `js avc.engine.js`) reads the analysis matrix directly and reports
the highest non-zero bin, the peak bin and its level in dBFS. That is the
measurement to take when the display disagrees with a reference analyser: it
says whether the FFT is writing every bin and what level it actually recorded,
independently of the whole display chain.

Unlock the device in Max and click the `status` / `probe` message boxes next to
the two `js` objects.

## Install

**The fast way.** Put every file from `device/` into one folder and drag
`AVC Spectrum Meter.amxd` onto an audio track. Live and Max resolve
`avc.fftanalysis.maxpat`, `avc.specreduce.genjit`, `avc.specui.js`,
`avc.engine.js` and `avc.meters.js` from the folder containing the `.amxd`, so
they must stay together. Nothing needs to go in your Max search path.

**The by-hand way**, if you would rather build it from the JSON:

1. In Live: drag **Max Audio Effect** onto a track, click the wrench to open it
   in the Max editor.
2. Select all in the new device and delete it.
3. Open `device/AVC Spectrum Meter.maxpat` in a text editor, copy all of it,
   and paste into the Max patcher window (Max accepts patcher JSON from the
   clipboard directly — `⌘V` in an unlocked patcher).
4. Copy the other five files into the same folder as the `.amxd` you are about
   to save.
5. Save the device.

## Set the device width

The width presets send `size` to `thispatcher`, but Live also stores a device
width of its own:

1. Set the zoom to **100%** (View → Zoom → 100%) — the width is measured in
   screen pixels, so any other zoom stores the wrong number.
2. In the Max editor, **View → Set Device Width**. Do this with the patcher at
   the width you want as the default (960 px for Normal). The generated device
   already carries devicewidth 960, so this is only needed if you change it.
3. Save. Live now opens the device at that width.

Repeat if you change the default width preset.

## Checkpoints

Work down this list. Stop at the first one that fails.

**(a) Passthrough and an empty UI at the right size.**
Play audio through the track. It must pass unchanged, and a null test against a
dry duplicate must cancel to silence. The device should be 155 px tall with a
dark plot area and a control strip; nothing needs to move yet. If the UI is
blank, the `.js` files are not next to the `.amxd`.

**(b) FFT reaching the display.**
Play pink noise. The green curve should move. If the plot stays flat at the
floor: open the Max console. `avc.engine.js` creates the `pfft~` objects when
`live.thisdevice` fires — if it cannot find `srcA`/`srcB` it says so. Check that
`avc.fftanalysis.maxpat` is in the folder.

**(c) Axes, slope and calibration.**
Send a 1 kHz sine at −18 dBFS from a test-tone generator. The peak must sit on
the 1 kHz vertical gridline and on the top (−18 dB) horizontal one. Then set
Slope 0, Range Hi 0, and send a full-scale sine: it must read 0 dB at every
block size. Put the tone on a bin centre — a tone between bins scallops down by
up to 1.42 dB with Hann.

**(d) Fill, second spectrum, Hold.**
Second spectrum defaults to Max, drawn in the darker green. Play something, stop
— the Max curve must stay. Press play — it must clear. Click the plot mid-play —
it must clear. Engage Hold and confirm the display freezes while the meters keep
moving.

**(e) The meter block.**
Sine wave: Dynamic ≈ 3.0 dB. Square wave: ≈ 0 dB. These two catch nearly every
metering scaling error. Then check the hold bars drop about 3 seconds after the
level falls.

**(f) Statistics, correlation, balance.**
Mono material: correlation pins to +1. Polarity-flip one channel: −1.
Uncorrelated noise in L and R: hovering around 0. Pan the material and watch the
balance bar.

**(g) Presets, settings, gating.**
Step through the six presets. Switch widths and confirm the meters collapse at
Compact and come back at Normal with the accumulated values intact. Then select
a different track and watch CPU in Live's meter — it should drop sharply.

## Freezing

Once it works: **File → Freeze Device** in the Max editor. That packs the
subpatcher, the gen patcher and all three `.js` files into the `.amxd`, making
it a single portable file. Freeze *after* bring-up, not before — a frozen device
is awkward to debug, and `autowatch` in the JS (which reloads the file when you
save it) stops being useful.

To keep developing, unfreeze, edit, and re-freeze.

## If something is wrong

The patchers are **generated**. Fix `build/*.py` and re-run:

```bash
cd build && python3 fft_analysis.py && python3 gen_patch.py && python3 device.py
cd .. && python3 test/validate.py
```

The three `.js` files are hand-written — edit those directly. `autowatch = 1` is
set in all of them, so Max reloads them the moment you save.

Most likely first failures, in rough order of probability:

- A `js`/`jsui` file not found → blank UI, console message naming the file.
- `jit.gen` failing to load `avc.specreduce.genjit` → flat curve, console error.
- `jit.gen`'s output geometry not following its left inlet. If the curve is the
  wrong length or the object complains about dimensions, the geometry matrix
  (`geomA`/`geomB`, left inlet) is not driving the output dim as assumed; adding
  `@adapt 0` plus a `dim` message to the `jit.gen` is the fix.
- `pfft~ … args` not reaching `#1`/`#2` in the subpatcher → the matrix stays
  empty. Open the `pfft~` (double-click) and check the `jit.poke~` object boxes
  show a resolved name rather than the literal `#1`.
