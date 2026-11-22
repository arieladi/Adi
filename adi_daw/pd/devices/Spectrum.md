# Spectrum: the track device's contract

Live 12's Spectrum as a device on a track (PLAN DEV-22). The devices lane
gives the canonical canvas, the controls and their fixed ids, and the
analysis those controls ask for. The analysis itself is the Mix View's shared
analyser (ADR-0183 d7, MX-12, MX-30): it runs on that worker, the same
`dsp::Spectrum` and `dsp::LinearMagnitudeAverage` the strips use. The device
runs no FFT of its own; CTL-07 forbids it for stream producers. This lands
after 3 December.

The values marked *(reading 34)* are ADI's until a Live 12.4 reading of
Spectrum, on the devices log's ONE list.

## 1. The canvas

`pd/devices/Spectrum.pd`, made like every built-in effect's:
`adc~ 1 2 -> adi.spectrum_device~ -> dac~ 1 2`, with the adi.param boxes of
§2.

- The signal passes untouched, a non-finite sample as it came, with 0
  latency. This holds on the Pd path and natively alike.
- The built-in host recognises the canvas by its bytes and dispatches the
  native core (`dsp::SpectrumDevice`).

## 2. The controls and their ids

One-based, fixed, never reused.

| id | Name | Range | Default | What it does |
|---:|---|---|---|---|
| 1 | `Block` | 0 to 5 | 1 | The FFT's size: 1,024 × 2^Block (1,024 to 32,768). *(reading 34: default)* |
| 2 | `Channel` | 0 to 2 | 2 | L, R, or L+R (their mean). |
| 3 | `Refresh` | 10 to 2,000 ms | 60 | How often an analysis is made: the hop. |
| 4 | `Avg` | 1 to 100 | 1 | Each analysis moves the average 1/Avg of the way. *(reading 34: Live's law)* |
| 5 | `Graph` | 0 to 2 | 0 | Line, Max or Bins: drawing only. |
| 6 | `Scale_X` | 0 to 2 | 1 | Lin, Log or ST: drawing only. |
| 7 | `Auto` | 0 to 1 | 1 | The range follows the signal: drawing only. |
| 8 | `Range_Low` | −120 to 0 dB | −80 | The bottom of the drawn range. |
| 9 | `Range_High` | −120 to +12 dB | 0 | The top of the drawn range. |

## 3. What the analyser is asked for

`dsp::SpectrumDevice::analysis()` turns ids 1 to 4 into one value, which the
worker reads whenever a control moves:

- `fftSize` and `window`: 1,024 × 2^Block, with a Hann window.
- `channel`: which signal is analysed (`SpectrumDevice::pick`).
- `hopSamples`: Refresh in samples at the session rate, over the whole
  range (2 s at 192 kHz is 384,000 samples).
- `average`: the `LinearMagnitudeAverage::Settings`. Its decay is set so
  that each analysis keeps 1 − 1/Avg of the average. Its own hop is at most
  `kMaximumHop` (65,536), with the decay for that hop. The averager's
  coefficient depends only on decay ÷ hop, so this holds at any Refresh.

The magnitudes are calibrated as `dsp::Spectrum` defines them: a full-scale
sine on a bin reads 0 dBFS.

## 4. What the Mix View's side takes over (the hand-off)

1. **The tap.** The track's signal is read at the device's place in the chain
   (post the devices before it), by the shared analyser's tap mechanism, not a
   buffer the device owns.
2. **The worker.** It runs the analysis of §3, and re-prepares its averager
   off the audio thread when Block, Refresh, Avg or the rate change.
3. **The view.** The device's panel draws the averaged magnitudes with ids 5
   to 9. A Max graph holds each bin's peak. *(reading 34: how long Live holds
   it)*
4. **A hidden panel holds no analysis** (MX-12: a hidden strip holds none).

## 5. Tests the build carries

- `adi_spectrum_device_tests`:
  - the settings for every Block, Refresh at 44.1 and 48 kHz, and the
    Channel picks;
  - with the device's settings, the analyser reads a bin-centred sine of 0.5
    at −6.02 dBFS (1,024, 4,096 and 32,768 points), and L+R reads one side at
    half its level;
  - Avg 4 gives 1 − (3/4)^k after k analyses, and Avg 1 replaces;
  - pass-through, no allocation, aliased planes.
- Parity: the canvas's Pd and native outputs agree (max error 0).
