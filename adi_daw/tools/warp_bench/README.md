# Warp analysis benchmark

The standalone probe compares the existing ADI Warp timing and energy-rise
transients with the worker candidates for ADR-0226. The benchmark accepts
labelled WAVs from any local corpus. All PCM and model checkpoints remain outside
the repository. The probe uses the same 44.1 kHz PCM bridge as the spectral onset
candidate. It runs the Mixxx `TempoTrackV2` fork for beat positions and the
qm-dsp `DownBeat` detector at a 4/4 prior by default. The separate supplied-meter
pass gives the analyzer the known meter and any known MIDI quarter tempo.

## Build

Use CMake 3.25 and a C++20 compiler. Initialize ADI's existing `libsamplerate`
and `pffft` checkouts. Supply the source paths for the two additional pins.

| Dependency | Pin | Files and license |
|---|---|---|
| qm-dsp | `4d2a4a4e0c2dd0ddf07bf9d2a224ded912712714` (`qm-vamp-plugins-v1.8.0`) | Complex spectral difference, downbeat analysis and their support files under GPL-2.0-or-later. Bundled Kiss FFT under its BSD terms. |
| Mixxx | `902cfc2543d7ef65bda1678e6f3c3e98be4aa39d` | `lib/qm-dsp` TempoTrackV2 fork, its header, `MathUtilities.*` and `maths/nan-inf.h` under GPL-2.0-or-later. |

The explicit CMake source list excludes qm-dsp's original `TempoTrackV2`,
unrelated PCA and separately licensed files. The pins are a proposal for the
production dependency once Q-D selects the suite analyser. [qm-dsp sources](https://github.com/c4dm/qm-dsp/tree/4d2a4a4e0c2dd0ddf07bf9d2a224ded912712714) · [Mixxx sources](https://github.com/mixxxdj/mixxx/tree/902cfc2543d7ef65bda1678e6f3c3e98be4aa39d/lib/qm-dsp)

Run these commands from `adi_daw`.

```sh
cmake -S tools/warp_bench -B /tmp/adi-warp-bench -DCMAKE_BUILD_TYPE=Release \
  -DQM_DSP_ROOT=/path/to/qm-dsp \
  -DMIXXX_QM_ROOT=/path/to/mixxx/lib/qm-dsp
cmake --build /tmp/adi-warp-bench
ctest --test-dir /tmp/adi-warp-bench --output-on-failure
```

## Measure a local corpus

Install `numpy`, `scipy` and `mir_eval==0.8.2` in a separate Python environment.
Provide a JSON manifest whose `tracks` array contains `id`, `collection`,
`audio` (relative WAV path), and strictly ordered `beats` in seconds for each
track. Optional labels are `downbeats`, `onsets` and `bpm`. Each list uses the
source clock. Add `beat_this_model` for the held-out checkpoint appropriate to
the track. `public_corpus.py` can create a pilot manifest from two collections.
Install `mido` for Groove MIDI. The preparation step downloads ZIP byte ranges
and the published labels. The scorer makes local reads only.

```sh
python tools/warp_bench/public_corpus.py /tmp/adi-warp-corpus
python tools/warp_bench/measure.py \
  --probe /tmp/adi-warp-bench/adi_warp_bench \
  --manifest /tmp/adi-warp-corpus/manifest.json \
  --report /tmp/adi-warp-corpus/report.json
```

The public pilot selects sixteen GuitarSet mic recordings across playing styles
and up to sixteen Groove MIDI performances across eight published folds. Selection
uses instrument, meter and style metadata before scoring. GuitarSet uses its JAMS
beat positions and string-note annotations. Groove MIDI uses published grid labels
and aligned MIDI note events. Note starts within 10 ms are coalesced; chord voicings
and near-simultaneous drum hits are therefore one onset for the recorded mix.
[GuitarSet, CC BY 4.0](https://zenodo.org/records/3371780) · [Groove MIDI, CC BY 4.0](https://magenta.tensorflow.org/datasets/groove) · [Published split and annotations](https://github.com/CPJKU/beat_this_annotations/tree/v1.0)

For the independent Beat This! comparison, install CPU `torch` and
`torchaudio` and the `beat-this==1.1.0` package in the benchmark environment.
Download the MIT-released `fold0.ckpt` through `fold7.ckpt` into a local model
directory and pass `--beat-this-checkpoints /path/to/checkpoints`. Every public
manifest row specifies the held-out fold, so it keeps training examples out of
that track's comparison. Essentia is optional with `--essentia` and Python wheel
`essentia==2.1b6.dev1389`, using RhythmExtractor2013 multifeature. Beat This!
models and both comparator runtimes run outside ADI. [Beat This! sources and weights](https://github.com/CPJKU/beat_this#available-models)

The report records per-track scores, a macro aggregate, and a macro aggregate
per collection. ADI's existing beats are reconstructed from its captured Warp
anchors with the original trim reapplied. Essentia and the incumbent report BPM
directly. The candidate and Beat This! use the median beat interval. Beat and downbeat
F-measures use 70 ms, and onset F-measure uses 10 ms. `mir_eval` calculates CMLt
and AMLt. Tempo Acc1 uses ±4 percent, and Acc2 allows one-third, half, double or
triple tempo. A missing estimate counts as zero when a reference exists and the
method supports the metric. The report gives the number of tracks in each mean.

Pass `--meter-prior` for a separate `candidate_meter` comparison with known
meter information. Manifest `meter` is `numerator-denominator` and defaults to
`4-4`. `source_quarter_bpm` is optional source metadata in MIDI quarters per
minute. Compound meters default to the dotted pulse when the source tempo is
unavailable. A known tempo can resolve a tracked subdivision. Repeated
low-frequency attacks can distinguish the downbeat from the other main pulse
when their phase consistently separates across at least three bars.

The two Groove MIDI 6/8 files have six dotted-quarter entries per labelled bar
in the published beat files. Their original MIDI declares a 6/8 time signature
at tick zero, so the same six eighth notes span two dotted quarters. The
preparer computes a second `midi_grid` reference directly from the MIDI time
signature and tempo. The `midi_grid_audit` section reports those downbeat
scores separately while the main table continues to use the published labels.

## Prepare the Live 12.4 workstation readings

Run from `adi_daw` with Python 3. The kit uses only the standard library.

```sh
python tools/warp_bench/live_acceptance_fixtures.py /path/outside/checkout/live-warp-readings
```

Open the generated `manifest.json` on the workstation. Its `workstation_cases`
entries give the inputs, actions and observations to record for M2-A-WARP 1, 6
and 8, L-MEDIA-6, and TM-WT-05. `fixtures` gives the original source frame
labels, exact lengths, ACID fields, and SHA-256 hashes of the WAV and its PCM.
The ACID comparison holds the 8-second PCM constant across the untagged,
120 BPM, and 90 BPM copies. The two tagged files each describe a complete 4/4
loop of that duration. The diagnostic input has a labelled 375 Hz tone region.

Keep each independent import trial in a fresh output folder so generated Live
analysis sidecars stay local to that trial. Keep the exported WAVs, visible
marker observations and saved-map evidence with the respective case. The
manifest labels source material and procedures for a future Live measurement.
