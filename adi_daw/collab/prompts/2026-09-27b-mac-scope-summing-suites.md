# 2026-09-27b — for mac on its return: the UI for the scope, group summing, the Airwindows suites and track delay

Between 2026-09-25 and mac's return, win built the engine side of four features
whose UI is mac's. Each engine side is merged or in review, with its API
named below. This brief collects the UI half in one place. It touches `src/juce/**` and
UI-ARCHITECTURE, which are mac's. It follows `2026-09-27a` (the device-host half of
plug-in automation), whose item 6 — the generic parameter panel — the suites
below also rely on until their own GUI exists.

The reference-scope research (Audija OScope, fx23 PsyScope Pro, 2026-09-25)
is kept at the end, because it is in no other file.

```text
You are agent mac on arieladi/Adi, project adi_daw. While you were away win merged the engine side of
four features whose UI is yours. Read the ADRs named in each item first; they are the source of truth,
and this brief only collects the UI half.

1. THE SCOPE PANEL (ADR-0167 approved by the director, engine ADR-0175).
   Engine: Session::openScope(trackId, seconds) -> shared_ptr<const ScopeTap> (the strip's output, after
   its fader; the same tap on a second call; closeScope(trackId) detaches). ScopeTap::read(l, r,
   firstStamp) copies the newest frames; false means "lapped, read again". Every frame is stamped with
   the timeline position at which it is HEARD, so two taps compare as heard by matching stamps -- a
   track delayed +100 samples stamps 100 later; do not correct anything by hand.
   Analysis, pure, for the UI thread: correlation(a, b) (-1..+1), bestOffset(a, b, maxLag) -> {samples,
   correlation} (positive: b is late). True peak: dsp::TruePeakMeter (BS.1770-4 Annex 2's method;
   resetPeak() keeps the filter).
   Build:
   - A panel in the bottom area, not a device. Open it from a track header or mixer strip; "Compare in
     Scope" on a second selected track (or Alt-click its scope button) adds it as layer B. No routing.
   - Layers up to 8: channel view L, R, Mid, Side or L+R; a polarity flip; the track's own name and
     colour.
   - Views: overlay, stacked, difference (A-B) and sum (A+B).
   - Window in bars/beats from the tempo map (1/16 to 4 bars) or ms, locked to bar lines; triggers:
     grid, free run, level edge.
   - Readouts: correlation over the window; the offset in ms and samples; the view's true peak (dBTP)
     beside the sample peak, and markers where an inter-sample peak exceeds the sample peak.
   - The fix: one click proposes mixer.setDelay on B by minus the offset (project samples: the tap's
     rate may differ from the project's -- convert).
   - Freeze; zoom and pan on time and dB; a cursor readout (bars/beats, ms, samples, dB).
   - Draw on the one UI clock (ADR-0050 d1); read taps once per frame.

2. GROUP SUMMING IN THE GROUP HEADER (ADR-0173, ADR-0174).
   Model: rows::Model::summing (trackId, enabled, flavor, driveDb). Flavours: adi::summingFlavors()
   (key, menu name): console9, console.la, console.mc, console.md, purest3, pd, c5raw, atmosphere.
   Ops: group.setSumming {id, enabled}, group.setSummingFlavor {id, flavor}, group.setSummingDrive
   {id, db} (coalescable: one undo step per drag). All refuse a track that is not a group.
   Build, on GROUP headers only: a toggle; a drive dial (-12 to +24 dB, 0 at the top, double-click
   resets to 0); right-click on the dial opens the flavour menu (names from summingFlavors(), the
   current one ticked). The level does not jump when summing turns on -- each flavour is level-matched
   in the engine -- so no meter recalibration is needed.

3. THE AIRWINDOWS SUITE GUI (ADR-0171, ADR-0173).
   Eleven CLAP plug-ins, "ADI Airwindows - <group>" (plugins/airwindows/SUITES.md lists each one's
   algorithms in Algorithm-parameter order). Parameter ids never change: 0 Algorithm (stepped, 0..N-1,
   value text = the algorithm's name), 1 Auto Gain, and algorithm a's parameter k = 100 + 64a + k with
   CLAP module = the algorithm's name. Only the ACTIVE algorithm's parameters should be shown; the rest
   exist and stay automatable. A switch is a 5 ms crossfade in the plug-in; the GUI only sets the
   parameter.
   Build: the suites' own GUI (a CLAP gui extension in plugins/airwindows) -- an algorithm browser by
   name with each one's description, the active algorithm's knobs, and the Auto Gain switch. Keep it
   light; Airwindows never had editors, and until this exists the generic panel of 2026-09-27a item 6
   is the UI.

4. TRACK DELAY IN THE MIXER (ADR-0172).
   Op: mixer.setDelay {id, samples} -- project samples, positive late, negative early, +-1 s; not
   played on the master (named as a problem). Build: a delay field on every strip but the master,
   shown in ms and samples, dragging coalesced into one undo step.

Also: Live's Simpler is "OneShot" in ADI (ADR-0169, the director's ruling).

DONE MEANS
The scope compares two tracks as heard, with a working one-click offset fix; group headers carry the
summing toggle, dial and menu; the suites have their GUI; strips have a delay field. adi_play renders
unchanged, CI is green by head SHA, and collab/mac.md records it.
```

## Reference: what the two plug-in scopes do (researched 2026-09-25)

For the panel's design, not to copy wholesale. Behaviour may be cloned; names
and artwork may not be.

**Audija OScope 1.2.5.** It sells for €8, and a freeware version has no
sidechain.
- **Display:** a beat-grid-synced, true-peak oscilloscope, drawn as a line or
  filled.
- **Grid:** adjustable spacing and loop length, in beats or ms.
- **Sidechain** (paid version only): a stereo overlay or separate views,
  "great for kick-bass alignment".
- **Inspection:** freeze; zoom and pan on the oscillogram and the dB scale;
  a max-dB readout.

**fx23 PsyScope Pro** (the director owns it):
- **Views:** multi-track waveform, a frequency oscilloscope, a spectrogram
  (a "sharp" mode, Wigner-Ville) and an isometric spectrogram.
- **Stereo:** a vectorscope and stereo analyser, and a correlometer with five
  modes (Lissajous, polar LCR, spectral pan, width and phase).
- **Comparing:** a delta mode comparing two sources in both waveform and FFT,
  a ballistics/envelope mode, and stacked or overlapped layers.
- **Sync:** beat sync, or free Hz/note, or Hz auto-synced from an FFT or MIDI.
  Latency compensation for DAWs lacking PPQ.
- **Sources:** up to 16 signals shown, and up to 128 linked instances in named,
  coloured groups. Each layer shows L, R, mono, mid+side or L+R, with a phase
  flip.
- **Phase:** correlation between two tracks, a wavecycle tracker (note, Hz,
  dB), and a kick/bass phase correlometer.
- **Metering:** VU (peak and RMS), true peak, EBU R128 LUFS, and an
  inter-sample-peak overview.
- **Tools:** custom dB and time reference lines; LP, HP, BP and AP filters (IIR
  or linear phase) with listen; sinc or Lanczos interpolation.
- **Capture:** freeze, a drag-and-drop WAV bounce, and a PNG capture.
- **Housekeeping:** it reads the DAW's track names and colours over VST3, and
  offers skins and stored sets.

ADI's advantage over both is decided in ADR-0167. It needs no routing, it
compares tracks as heard without manual latency correction, and its offset
readout comes with a one-click fix.
