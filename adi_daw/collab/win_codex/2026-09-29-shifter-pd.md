# Shifter — checklist written before implementation

Mission 4, delegated by win; branch `codex/shifter-pd`. Live 12 manual §28.35,
printed pp. 634–639. This is a paraphrase, with no copied manual text/images.

- [ ] Pitch shifts in semitones/cents; Window trades time/frequency resolution.
- [ ] Freq translates frequencies; Ring creates sum/difference sidebands and
  supports Drive only in Ring mode. Dry/Wet mixes the output.
- [ ] Spread and Wide provide channel offsets with opposite signs when Wide
  is enabled, and no Wide difference at zero Spread.
- [ ] Optional feedback delay supports free and tempo-synced time; Tone
  filters high frequencies from its feedback path.
- [ ] Ten LFO shapes, duty cycle, Phase/Spin/Width, sync Offset, Rate and Amount
  modulate the tuning. Random sources are deterministic on reset.
- [ ] Envelope follower has enable, Attack, Release and mode-dependent Amount.
- [ ] Internal/MIDI tuning, Glide and 0–24-semitone bend range; notes received
  through Pd's existing MIDI input, with no new host-side routing.
- [ ] Measured cents/frequency/sidebands, 32–4096 block equivalence, zero audio
  allocations, real LibPdEngine sound with far-bin-first assertions and clean
  console. Offline file renders only.
- [ ] Director's Live side-by-side session; one provisional control table in
  the PR includes every unverified range, default, unit and curve.

## Sources inspected before code

Surge checkout `58914e59c608ed4384ba6002e44c3465c58b2e71`:
`src/common/dsp/effects/FrequencyShifterEffect.cpp`, `RingModulatorEffect.cpp`.
Its SST effects `voice-effects/modulation/FreqShiftMod.h` implements analytic
signal modulation using `HilbertTransformMonoFloat`. The underlying
`sst-basic-blocks` checkout is `a32b8aec14d661e415bb676bb2e2a0a4da4efc96`.
Read `include/sst/basic-blocks/dsp/HilbertTransform.h` directly, including the
pole constants and 25 Hz design floor; numerical values are source-verified.
Adapted source must retain upstream headers and attribution (GPL-3-or-later).

ADR-0187 explicitly says Surge's mapping covers Freq/Ring, not Pitch. Pitch
requires an additional algorithm, and the Live LFO/envelope/delay/MIDI behavior
requires adaptation rather than exposing Surge's original controls. Any such
choice is recorded with its measurements, not advertised as verified Live parity.

## Implementation and parity boundaries

Freq adapts SST's analytic modulation and scalar Hilbert network (upstream
notices retained), with a sine/cosine sign convention verified by positive and
negative sideband measurements. Ring follows Live's sine product with optional
Drive, rather than Surge's diode/unison model. Pitch is an ADI phase-vocoder
using the already-pinned PFFFT, with all six windows prepared off audio. This
avoids introducing another dependency and provides a bounded, allocation-free
pitch path where the Surge-to-Live map has no Pitch equivalent. Pitch reports
its selected FFT size as latency and delays dry to match; Freq/Ring report zero
fixed latency (their allpass phase response is not a fixed delay).

These algorithms are functional approximations pending the director's session,
not a claim of sample-equivalent Live audio. Phase-vocoder transients, ring
Drive, envelope law and LFO curves remain unverified. MIDI uses the existing
Pd notein/bendin routing, last-note priority, no new source chooser in the core.
Sync rates follow the existing io.transport -> adi.transport BPM seam; LFOs
free-run at that rate, with Offset applying phase. Host source selection is
outside this DSP/patch PR. Random Phase behavior is approximate; Width mode
interpolates between a shared and inverted random waveform. These are named
parity gaps for the supplied checklist follow-up, not silently omitted controls.

## One provisional values table

**Every range, default, unit and curve below is unverified against Live.**
PB's 0–24-semitone range is stated in the chapter; its default/curve remain
provisional. The director's supplied rows will replace this table and controls.

| Live control | ADI range | Default | Unit | ADI curve / interpretation |
|---|---|---|---|---|
| Mode | 0–2 | 0 | - | Pitch Freq Ring |
| Coarse | -48–48 | 0 | - | Linear; semitones in Pitch, kHz in Freq/Ring |
| Fine | -100–100 | 0 | - | Linear; cents in Pitch, Hz in Freq/Ring |
| Spread | 0–100 | 0 | - | Linear; cents in Pitch, Hz in Freq/Ring; opposite sign on R when Wide |
| Wide | 0–1 | 0 | - | int |
| Window | 5–170 | 40 | ms | Log control; nearest power-of-two window, 256–8192 samples; 4x overlap |
| Delay | 0–1 | 0 | - | int |
| Delay Mode | 0–1 | 0 | - | int |
| Delay (free) | 0.1–1000 | 4 | Hz | Log; delay seconds = 1/Hz |
| Delay (synced) | 0.0625–8 | 1 | beats | Log quarter-note multiplier; 60×beats/BPM |
| Feedback | 0–95 | 0 | % | lin |
| Tone | 20–20000 | 10000 | Hz | Log Hz; one-pole feedback lowpass |
| LFO waveform | 0–9 | 0 | - | Sine Triangle TriangleAnalog Triangle8 Triangle16 SawUp SawDown Rectangle Random RandomSH |
| Duty | 1–99 | 50 | % | lin |
| Phase/Spin/Width mode | 0–2 | 0 | - | Phase Spin Width |
| Phase | 0–360 | 0 | deg | lin |
| Spin | 0–20 | 0 | % | Linear; right LFO speed increase |
| Width | 0–100 | 0 | % | Linear; 0 shared random, 100 inverted random |
| LFO Rate mode | 0–1 | 0 | - | int |
| Offset | 0–360 | 0 | deg | lin |
| LFO Rate (free) | 0.01–40 | 1 | Hz | log |
| LFO Rate (synced) | 0.0625–16 | 1 | beats | Log; LFO Hz = BPM/(60×beats) |
| LFO Amount | 0–24 | 0 | - | Linear; semitones in Pitch, kHz in Freq/Ring |
| Env Fol | 0–1 | 0 | - | int |
| Attack | 0.1–1000 | 10 | ms | Log ms; one-pole peak envelope |
| Release | 1–5000 | 100 | ms | Log ms; one-pole peak envelope |
| Envelope Amount | -24–24 | 0 | - | Linear peak-envelope scaling; semitones/kHz by mode |
| Drive enable | 0–1 | 0 | - | int |
| Drive | 0–36 | 0 | dB | Linear dB; tanh drive only in Ring |
| Dry/Wet | 0–100 | 100 | % | lin |
| Internal/MIDI | 0–1 | 0 | - | Toggle; last positive-velocity note, C4=zero Pitch shift; concert-A frequency for Freq/Ring |
| Glide | 0–2000 | 0 | ms | Linear ms; exponential note glide |
| PB | 0–24 | 2 | st | lin |

## Validation

MSVC Debug /WX, Pd enabled: 40 DSP checks and 48 real-Pd checks pass. FFT pitch
measurement at -1200/-700/+13/+700/+1200 cents has worst error 0.04 cents
(tolerance 2 cents). Freq's 250 Hz translation rejects its other sideband;
Ring produces both, and Drive creates harmonics. MIDI note/bend, synced delay,
Wide/Spread, all LFO shapes, Window latency and block equivalence are covered.
Blocks 32–4096 are sample-identical. Core and real-Pd Pitch/Freq allocate zero
(including Windows Debug CRT). Real Pd's 7-to-8 kHz translation measures
0.399999834 amplitude after checking its far bin, with a clean console.

The real-Pd stereo assertion caught an input/output buffer alias: both stereo
inputs are now sampled before either output is written. Exact stereo equality
passes afterward. All audio is rendered offline; shifter-pd-render.wav is a
float file in the test working directory. Human side-by-side remains pending.
