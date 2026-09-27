# Deviations, inferences and gaps

Everything here differs from the brief, or rests on an inference rather than a
known fact. Nothing in this list is a surprise waiting to be found later.

Three categories:

- **DEVIATION** — built differently from the brief, with the reason.
- **INFERRED** — the brief (or SPAN's manual) does not define it; this is a
  reasoned choice that could be wrong.
- **GAP** — asked for, not built. With what it would take.

---

## The big one

**GAP — the device is not verified end-to-end.** No Max application is
installed on this machine; only the Max 8.3.1 runtime bundled inside Ableton
Live 11 Suite, which cannot be driven headlessly. Every object, argument,
attribute and inlet/outlet index was checked against that runtime's own
reference documentation, the patcher JSON is structurally validated, and the
DSP maths is verified numerically.

The device has now been loaded once in Live. It draws, and that run exposed
four faults — a missing gen `classnamespace`, a gen patcher referenced by bare
argument instead of `@gen`, two `expr` objects emitting floats into int-only
`select`/`selector~` inlets, and `adstatus` read from the wrong outlet. All are
fixed and all are now build-time errors in `test/validate.py`. Behaviour beyond
"loads and draws" is still unproven.

Also note this machine has **Live 11**, not the Live 12 in the brief; nothing
used is Live-12-specific, but it is untested there.

---

## Analysis front end

**DEVIATION — the window is applied by convolving the complex spectrum, not by
`fftin~`.**
`fftin~` offers only square/hanning/triangle/hamming/blackman, and the choice is
a *creation argument* — it cannot change at runtime. A runtime window menu
would therefore need one `pfft~` per (block size × window), and every
instantiated `pfft~` allocates signal buffers whether muted or not. Instead the
FFT is taken rectangular and the window applied afterwards as a 7-tap
convolution across bins, which is exactly equivalent for any cosine-sum window
and makes the window a runtime coefficient change.
`test/test_spectrum.js` proves the equivalence to machine precision (max
relative error 1.1e-15).

**DEVIATION — frame-edge bins.** A convolution tap at a frame boundary would
reach into the previous frame, so out-of-range taps are gated to zero. The cost
is that the first *k* bins lack their mirror term (X[−k] = conj(X[k])), where
k = 1 for Hann, 2 for Blackman, 3 for Hi-Res. **For the default Hann window the
only affected bin is bin 0 (DC), which is never displayed** — so the default
path is exact on every visible bin. The top edge is likewise affected, but
those bins sit above 20 kHz at every supported sample rate and are off-screen.

**OPEN — SPAN's window is "Dome", which is not one of the three offered here.**
Read directly off the reference instance's Spectrum Mode Editor: Window DOME,
Type RT MAX, 2nd Type MAX, Block 4096, Overlap 80.0, Avg Time 194, Align 0 dB
on, Anti-Alias on, Smooth off, 10 Hz-20 kHz, Range -80.0/-13.0, Slope 4.50.
Everything there is now matched except the window and the overlap.

The window matters for one specific symptom: SPAN's comb notches between
100 Hz and 1 kHz are visibly deeper than ours on the same signal, and with the
same block size and the same slope the only thing that sets notch depth is the
window's mainlobe width. Hann's mainlobe is 4 bins. A window narrower than
Hann would cut deeper notches, and a sine ("cosine") window -- 3 bins -- is the
obvious candidate for something called Dome, though a raised cosine *is* also
literally dome-shaped, in which case Dome is Hann and this is not the cause.

This device applies the window by convolving the spectrum, which is exact only
for a cosine-sum window. A sine window is not a finite cosine-sum, so it cannot
be added as another 7-tap kernel; it would need the time-domain path and one
`pfft~` per window. Not attempted without knowing Voxengo's actual definition.

**DEVIATION — "Hi-Res" is 4-term Nuttall, not Nuttall-squared.** Squaring the
window convolves the 7-tap kernel with itself into 13 taps, i.e. six more delay
taps per part in the DSP. Plain Nuttall already puts its sidelobes at −93 dB,
below the −80 dB floor of the default range, so the extra taps buy nothing
visible. Measured worst-case scalloping: Hann −1.42 dB, Blackman −1.10 dB,
Hi-Res −0.81 dB.

**DEVIATION — the `pfft~` objects are created at runtime by JS.** Block size and
overlap are creation arguments. Instantiating all five sizes × two overlaps and
muting the unused ones costs roughly 8 MB per device instance in signal buffers
(~2.5 MB for a single 16384 engine alone), which is unreasonable at twenty
instances. Exactly one `pfft~` exists per engine and is rebuilt on change.
Expect a brief audible-analysis gap — not an audio gap — when changing block
size or overlap.

**INFERRED — "Align 0 dB" when switched off.** This device calibrates sines
exactly, so with Align **on** a full-scale sine reads 0.00 dBFS at every block
size (verified). SPAN's switch exists because its default scaling is not
sine-referenced; the manual does not state what the un-aligned reference is.
Align **off** here applies a bin-density normalisation relative to 4096, so a
broadband noise floor stays put as the block size changes. 4096 reads identically
either way. This is a guess at SPAN's intent, not a measured match.

**DEVIATION — overlap.** As the brief already noted, `pfft~` needs a power of
two, so SPAN's 80% is unreachable; 75% (4) and 87.5% (8) are offered.

**DEVIATION — Underlay.** SPAN overlays an arbitrary second channel group. Here
the second FFT engine is shared between "L+R overlaid" and Underlay, and
Underlay overlays the *complementary* channel group (L↔R, Mid↔Side, Sum→Side).
That is the pairing that is actually useful and it needs no extra menu or a
third FFT engine. In L+R overlaid mode the Underlay switch is ignored, because
the engine is already in use.

**DEVIATION — smoothing.** Implemented by widening each display column's
frequency band and switching its aggregation from per-pixel max to power-mean.
Widening in the log domain *is* constant-octave smoothing, so it costs nothing
extra. This faithfully reproduces the documented side effect: on a stationary
sine the reading droops with frequency, because the tone occupies a fixed number
of bins while the averaging band grows.

---

## Cursor readout

**GAP — right-click does not copy to the clipboard.** Vanilla Max and its JS
engine expose no clipboard API (confirmed against the Max 8.3.1 JS
documentation and the `max` object's message list). Right-click — which is
ctrl-click on macOS and the right button on Windows, both reaching `jsui` as
the same `mod2` flag, so one code path serves both — instead **pins** the
readout at that frequency so the number stays on screen to be read and typed
into an EQ. This is the closest achievable behaviour, and it is a real
shortfall against the brief.

**Implemented as specified**: the readout row is overlaid on the top edge of the
plot and costs the graph no height; frequency comes from the inverse of the log
mapping; the level readout is the cursor's own Y position, not the curve value;
DELTA is the primary-to-secondary distance at the cursor's frequency and blanks
to `--` when the second spectrum is off; note/cents/delta are dropped at Compact
width when the row would otherwise collide. Cursor movement never triggers a
repaint — `onidle` only records the position, and the next scheduled frame draws
it.

---

## Metering

**GAP — True Peak.** Not implemented. The clipping counter is sample-peak only
and the TP/SP switch is not in the UI, because shipping a control that silently
reads the same either way is worse than not shipping it. Adding it means a
`poly~ … up 4` wrapper containing `peakamp~`; Max's `poly~` upsampling filter is
not a spec-grade 4× interpolator, so it would be an approximation and should be
labelled as one.

**GAP — Integrated LUFS.** Momentary (400 ms) and Short-term (3 s) are
implemented per BS.1770-4, with true un-normalised K-weighting and the −0.691 dB
offset. Integrated is not: it needs the absolute −70 LUFS gate *and* the −10 LU
relative gate over 400 ms blocks with 75% overlap, which is a substantial
mechanism on its own. The `meters` message already reserves a slot for it.

**GAP — Density mode.** Not implemented. It was optional in the brief.

**DEVIATION — the Dynamic BARS were removed; the Float switch is now inert.**
Dynamic is still measured per channel and still shown, as the two numbers in the
meter block's second row. The pair of vertical bars that also showed it was
dropped at the user's request: the numbers say the same thing, and two stubby
cyan blocks earned none of the height they took. `Float` positioned those bars
against the level scale, so it no longer changes anything drawn. The parameter
is still accepted and stored rather than removed, so existing sets and presets
stay loadable.

**The goniometer** occupies the space they left. It is not in the brief; it was
added on request, modelled on RME's Totalyser — L against R rotated 45°, mono
reading as a vertical line up the M axis, with the continuous trace, the
afterglow and the AGC that RME's own notes single out as what makes one
readable.

**DEVIATION — window lengths are capped by `average~`'s allocation.** The
creation argument of `average~` is its *maximum* interval, and an interval
longer than that is discarded silently rather than clamped. The RMS windows are
allocated 192000 samples and the LUFS windows 38400 (momentary) and 288000
(short-term), which covers every window at up to 96 kHz. Above 96 kHz the
requested window is clamped to the allocation rather than being ignored, so
short-term LUFS falls back to 1.5 s at 192 kHz. Raising the allocation costs
8 bytes per sample per object per instance, which is why it is capped rather
than sized for the worst case.

**INFERRED — "dBFS.30" and "dBFS.15".** SPAN's manual does not define these.
They are implemented as meter scale-span variants — a 30 dB and a 15 dB scale
instead of the default 48 dB — which is the reading that fits how they sit
alongside dBFS and dBFS+3. Could easily be wrong.

**INFERRED — Max Crest Factor.** Implemented as the running maximum of
(50 ms RMS − integrated RMS), taking the larger channel. As the brief notes, a
shorter peak-RMS window yields a larger crest value, so comparisons against
another meter require matching that window.

**DEVIATION — A/C weighting accuracy near Nyquist.** The filters are bilinear
transforms of the IEC 61672 analogue prototypes with pre-warped pole
frequencies. Measured error at 10 kHz: +0.74 dB at 44.1 kHz, +0.60 dB at
48 kHz, +0.13 dB at 96 kHz. Below 4 kHz every checked point is within 0.1 dB at
every rate. This residue is inherent to a biquad cascade at 44.1 kHz and is well
inside IEC class 1 tolerance.

**DEVIATION — K-weighting is normalised to 0 dB at 1 kHz** for the RMS/Dynamic
meter, so a 1 kHz tone still reads its true level. The LUFS path uses the
un-normalised filter, as the standard requires. In K-System bias modes (K-20 /
K-14 / K-12) the weighting selector is overridden to Off and the RMS
integration/release are fixed at 600 ms, per the K-System specification.

**The clipping COUNT is measured but not displayed.** It used to be appended to
the left Peak box as a "!", which read as a glitch rather than a reading -- it
appeared on one side only and nothing explained it. What survives is the part
that matters: the Peak boxes and their caption turn red while any sample has
reached 0 dBFS since the last reset. The count is still computed per channel and
still occupies its slots in the meters message, so surfacing it properly later
costs nothing.

**Clipping counter counts over *events***, not over samples — a rising edge of
the ≥ 0 dBFS condition. Counting samples would report a number that scales with
sample rate for the same musical event.

---

## Reset semantics

**DEVIATION — re-pressing Play while already playing does not reset.** The reset
fires on the transport's stopped → playing edge, observed through the Live API
(`live_set is_playing`). Live does not clear `is_playing` when Play is pressed
again during playback, and the only other signal — song time jumping backwards —
is indistinguishable from a loop boundary, which must *not* reset. So: play from
stopped resets, looping does not, stopping never does, and a re-trigger during
playback does not. Use the Reset button or click the plot for that case.

Everything else is as specified: clicking anywhere on the plot, clicking
anywhere in the metering block, the Reset button, and the transport edge all
clear the same set — Max/Avg spectrum curves, hold bars, numeric maxima, over
indicators, clip counters, Max Crest Factor. Hold freezes the display only;
resetting while Hold is engaged clears the underlying data.

---

## UI and performance

**What still runs when the device's track is not selected.** The visibility gate
mutes both `pfft~` engines and stops the redraw clock, which removes the FFT
(the dominant cost) and all drawing and message traffic. What remains is 78
signal objects -- ten `biquad~`, six `average~`, thirteen `snapshot~`, nine
`slide~`, four `jit.poke~` and the rest -- all cheap per-sample operations, plus
Live's own per-device overhead.

MSP has no per-object mute. The documented way to switch a group of signal
objects off is to put them in a subpatcher and drive it from `mute~`, which
would mean moving the whole measurement chain behind one boundary: about 25
control outlets for the meters, correlation, LUFS and statistics. That is a
mechanical but wide change, and every one of those crossings is a chance to
break a working meter, so it is deliberately not done yet.

**DEVIATION — visibility gating is a heuristic.** Neither `live.thisdevice` nor
the Live API reports whether a device is currently drawn in the chain strip.
Max only calls `paint()` on an object that needs drawing, so the jsui counts
paints against redraw requests: several requests with no paint means nothing is
looking, and Max drops the redraw clock to a 2 Hz heartbeat — still frequent
enough to notice becoming visible again. If Max paints off-screen objects
anyway, the gate simply never engages and CPU stays at the normal figure; it
cannot misfire in the harmful direction.

**DEVIATION — the Anti-Alias switch snaps to the pixel grid.** `mgraphics` in
this Max version has no antialias toggle (its full method list was checked);
curves and grid lines are always drawn antialiased. With the switch off,
coordinates are snapped to half-pixel positions, which gives the crisp aliased
look the setting is for.

**DEVIATION — presets are message boxes, not `pattrstorage`.** `pattrstorage`
keeps presets in a separate `.json` that has to be frozen alongside the device
and can fall out of sync with it. Message boxes need no extra file and survive
freezing and duplication unchanged. All six required presets are present, and
`autopattr` is still in the patch so the parameter set stays coherent and
scriptable. The preset menu is deliberately excluded from the load-time
parameter broadcast — otherwise re-emitting the stored preset index would
re-apply that preset on every set load and discard any edits made after choosing
it.

**All 36 parameters are reachable.** The spectrum and meters own the full
155 px; the parameter rows are an overlay toggled by the SET button in the
plot's top-left corner, so nothing competes with the display for height. The
controls are ordinary `live.*` objects shown and hidden with `thispatcher`'s
`script show` / `script hide`, so they remain live parameters — and keep their
stored values — whether the panel is open or not.

**CONSTRAINT — Live parameters may not use the Int data type for wide ranges.**
Live's native integer representation holds only 256 values, and declaring
`parameter_type: 1` makes it silently rewrite `mmax` to `mmin + 255`. That is
not a warning, it is a silent rewrite of the saved device. Every numeric
parameter here is therefore Float type with an Int unit style, as Max's own
documentation prescribes. `test/validate.py` fails the build if any parameter
is declared Int with a range wider than 256.

**Display resolution is capped at 1024 points per curve**, which is the
`jit.spill` list length. At Wide (1140 px) the plot is about 996 px, so this is
never reached in practice; if the device is stretched wider by hand the curve is
interpolated across the extra pixels rather than truncated.

**All 35 parameters are "Stored Only"** (`parameter_invisible: 2`) — saved with
the set, absent from automation and MIDI-map lists. The validator fails the
build if any parameter is not.
