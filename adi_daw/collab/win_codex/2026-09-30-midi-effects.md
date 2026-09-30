# Fourteen MIDI effects — pre-code checklist

Sources read: Live 12 MIDI Effect Reference pp.662–674; Max for Live MIDI Effects pp.822–835. Own wording. Mission 10 explicitly selects Microtuner instead of ADR-0169's Envelope MIDI; follow the current mission list.

- Pitch: transpose, input range, Block/Fold/Limit, retained note-off mapping.
- Chord: original plus six shifts, duplicate suppression, per-voice velocity/chance, strum order/time/tension/crescendo and expression copies.
- Random: seeded chance, choices/interval/sign, random or repeatable alternate sequence; matching releases.
- Scale: root-relative twelve-note mapping with disabled notes, transpose, fold and active range.
- Velocity: note-on/release/both; input/output ranges, clip/gate/fixed, drive/compand/random; a rejected attack cannot leave an unmatched release.
- Arpeggiator: held-note pattern, tempo/free rate, gate, hold, offset, transposition steps, retrigger, repeats, seeded random styles; scheduled releases cross callback boundaries.
- Note Length: on/off trigger, free/sync duration multiplied by gate, release-velocity mix, decay/key scaling and latch.
- Note Echo: delayed copies with velocity feedback and pitch steps, Thru/Mute, sync/free time and optional expression repeats.
- CC Control: fixed modulation/pitch/pressure plus assignable controllers; automation produces timestamped control events, not fake note expression.
- Expression Control: expression/velocity/key/random/increment sources, mapped parameter modulation versus replacement, curves/rise/fall.
- MPE Control: independent pitch/pressure/slide curves, rise/fall and per-note identity; optional global conversion.
- Microtuner: native per-note pitch offsets from an explicit tuning table; preserve full-precision expression and project state. Its dedicated reference is not in the supplied manual chapter and must be checked separately.
- MIDI Monitor: bounded observations, Freeze/Clear affect display only; events pass unchanged.
- Shaper: note-triggered breakpoint envelope, velocity influence, free/sync duration, mapped modulation or replacement.

Graph finding: NodeIo.events is read-only; Graph::forwardEvents traverses complete blocks in topological order before audio segments. Add the transform hook there, before forwarding to the next slot. Preserve addressed parameter events locally, sort transformed events before split calculation, retain edge compensation. Scratch and schedules are fixed-capacity; overflow is observable and note lifetimes are tracked. No audio-thread allocation or raw MIDI bytes in the note/expression representation. Parameters/defaults not verified by the manual are ADI choices pending the director's forthcoming rows.

Tests: exact event identity/type/value/frame; release mapping after parameter changes; seeded replay; all integer callback sizes 32..4096; downstream instrument integration; overflow/reset/stop; zero allocations.
