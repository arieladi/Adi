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
