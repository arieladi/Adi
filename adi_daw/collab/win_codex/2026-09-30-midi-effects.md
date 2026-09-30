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

## Microtuner reference, before implementation

The supplied Live chapters omit Microtuner. Checked Ableton's own pack description on 2026-09-30: https://www.ableton.com/en/packs/microtuner/ . Checklist: import/export Scala scales, edit individual pitches and reference frequency, generate equal divisions with a selectable period, and blend two tuning decks while retaining per-note expression. Lead/Follow and MTS-ESP interoperability are separate integration concerns; report their implementation status explicitly. This reference describes functionality rather than verified ranges/defaults.

## Implementation and validation

All fourteen are native `internal` devices resolved by Session. Graph transforms complete blocks before forwarding, preserving sample offsets and edge delay. Explicit device/parameter mappings add event-only DAG dependencies; cycles are rejected. Runtime ownership transfers note releases through processors, including rejected/delayed releases. Fixed queues count overflow. Note Echo schedules repeats causally rather than imposing a 64-repeat cap. OneShot now consumes ParamMod without overwriting its base parameter.

MSVC /WX: new MIDI suites 248 and 29 checks; regressions graph 192, MPE input 72, MPE output 91, MIDI clips 165, OneShot 100. All fourteen compare complete event hashes at every integer block size 32 through 4096 with allocation instrumentation. CI, ops, Pd and schema validators pass. No speaker output. No intermediate UI branch pushed.

Scope boundaries for review: these are engine devices, not bespoke Live-style editors. Scale awareness uses explicit per-device masks because project key_map currently has no editing op. Arpeggiator Groove, Note Length sustain-pedal latch semantics, Microtuner Scala export/random generator/MTS-ESP/Lead-Follow, and mixer-target mapping are not implemented. Controller events work on the engine path and MIDI/MPE outputs; the native CLAP-note dialect cannot represent arbitrary CC. Existing hardware parser routing is unchanged. The forthcoming verified Live rows are still required before claiming exact Live parity. Capacities (256 input voices, 8192 pending events, 1024 monitor records, 8 mappings) and the echo termination floor 1/16384 are ADI engineering choices, not Live measurements.

## Unverified against Live — complete parameter table

Every range, default and mapping curve below is an ADI value, **unverified against Live**. Names are native IDs corresponding to the checklist controls. Values labelled native real value retain the control's semantics (MIDI keys/semitones, milliseconds for time/decay/strum/rise/fall, quarter notes for beats, unit interval for amounts); no claimed Live display formatting. Generated from descriptors with `adi_midi_modulation_tests --parameters` so the director can compare the complete list in one session.

| Device / control | ADI range | ADI default | Unit / curve |
|---|---|---|---|
| adi.arpeggiator / style | 0 .. 10 | 0 | native real value / discrete linear |
| adi.arpeggiator / sync | 0 .. 1 | 1 | native real value / discrete linear |
| adi.arpeggiator / time | 1 .. 60000 | 125 | native real value / linear |
| adi.arpeggiator / beats | 0.015625 .. 32 | 0.25 | native real value / linear |
| adi.arpeggiator / gate | 0.01 .. 2 | 0.5 | native real value / linear |
| adi.arpeggiator / hold | 0 .. 1 | 0 | native real value / discrete linear |
| adi.arpeggiator / distance | -36 .. 36 | 12 | native real value / discrete linear |
| adi.arpeggiator / steps | 0 .. 8 | 0 | native real value / discrete linear |
| adi.arpeggiator / offset | 0 .. 127 | 0 | native real value / discrete linear |
| adi.arpeggiator / retrigger | 0 .. 2 | 1 | native real value / discrete linear |
| adi.arpeggiator / repeats | 0 .. 128 | 0 | native real value / discrete linear |
| adi.arpeggiator / target | 0 .. 1 | 1 | native real value / linear |
| adi.arpeggiator / decay | 0 .. 60000 | 0 | native real value / linear |
| adi.arpeggiator / seed | 1 .. 16777215 | 1 | native real value / discrete linear |
| adi.arpeggiator / use_scale | 0 .. 1 | 0 | native real value / discrete linear |
| adi.arpeggiator / scale_root | 0 .. 11 | 0 | native real value / discrete linear |
| adi.arpeggiator / scale_mask | 1 .. 4095 | 2741 | native real value / discrete linear |
| adi.chord / shift1 | -36 .. 36 | 0 | native real value / discrete linear |
| adi.chord / velocity1 | 0.01 .. 2 | 1 | native real value / linear |
| adi.chord / chance1 | 0 .. 1 | 1 | native real value / linear |
| adi.chord / shift2 | -36 .. 36 | 0 | native real value / discrete linear |
| adi.chord / velocity2 | 0.01 .. 2 | 1 | native real value / linear |
| adi.chord / chance2 | 0 .. 1 | 1 | native real value / linear |
| adi.chord / shift3 | -36 .. 36 | 0 | native real value / discrete linear |
| adi.chord / velocity3 | 0.01 .. 2 | 1 | native real value / linear |
| adi.chord / chance3 | 0 .. 1 | 1 | native real value / linear |
| adi.chord / shift4 | -36 .. 36 | 0 | native real value / discrete linear |
| adi.chord / velocity4 | 0.01 .. 2 | 1 | native real value / linear |
| adi.chord / chance4 | 0 .. 1 | 1 | native real value / linear |
| adi.chord / shift5 | -36 .. 36 | 0 | native real value / discrete linear |
| adi.chord / velocity5 | 0.01 .. 2 | 1 | native real value / linear |
| adi.chord / chance5 | 0 .. 1 | 1 | native real value / linear |
| adi.chord / shift6 | -36 .. 36 | 0 | native real value / discrete linear |
| adi.chord / velocity6 | 0.01 .. 2 | 1 | native real value / linear |
| adi.chord / chance6 | 0 .. 1 | 1 | native real value / linear |
| adi.chord / expression | 0 .. 1 | 1 | native real value / discrete linear |
| adi.chord / seed | 1 .. 16777215 | 1 | native real value / discrete linear |
| adi.chord / strum | -400 .. 400 | 0 | native real value / linear |
| adi.chord / tension | -1 .. 1 | 0 | native real value / linear |
| adi.chord / crescendo | -1 .. 1 | 0 | native real value / linear |
| adi.chord / use_scale | 0 .. 1 | 0 | native real value / discrete linear |
| adi.chord / scale_root | 0 .. 11 | 0 | native real value / discrete linear |
| adi.chord / scale_mask | 1 .. 4095 | 2741 | native real value / discrete linear |
| adi.note_echo / sync | 0 .. 1 | 1 | native real value / discrete linear |
| adi.note_echo / time | 1 .. 60000 | 250 | native real value / linear |
| adi.note_echo / sixteenths | 1 .. 32 | 4 | native real value / discrete linear |
| adi.note_echo / fraction | 0.01 .. 2 | 1 | native real value / linear |
| adi.note_echo / input | 0 .. 1 | 0 | native real value / discrete linear |
| adi.note_echo / pitch | -36 .. 36 | 0 | native real value / discrete linear |
| adi.note_echo / delay_velocity | 0 .. 1 | 0.5 | native real value / linear |
| adi.note_echo / feedback | 0 .. 0.99 | 0.5 | native real value / linear |
| adi.note_echo / mpe | 0 .. 1 | 1 | native real value / discrete linear |
| adi.note_echo / press | 0 .. 1 | 1 | native real value / linear |
| adi.note_echo / slide | 0 .. 1 | 1 | native real value / linear |
| adi.note_echo / pitch_feedback | 0 .. 1 | 1 | native real value / linear |
| adi.note_length / trigger | 0 .. 1 | 0 | native real value / discrete linear |
| adi.note_length / sync | 0 .. 1 | 0 | native real value / discrete linear |
| adi.note_length / time | 1 .. 60000 | 100 | native real value / linear |
| adi.note_length / beats | 0.015625 .. 32 | 1 | native real value / linear |
| adi.note_length / gate | 0.01 .. 2 | 1 | native real value / linear |
| adi.note_length / release_velocity | 0 .. 1 | 0 | native real value / linear |
| adi.note_length / decay | 0 .. 60000 | 0 | native real value / linear |
| adi.note_length / key_scale | -1 .. 1 | 0 | native real value / linear |
| adi.note_length / latch | 0 .. 1 | 0 | native real value / discrete linear |
| adi.pitch / pitch | -128 .. 128 | 0 | native real value / discrete linear |
| adi.pitch / lowest | 0 .. 127 | 0 | native real value / discrete linear |
| adi.pitch / range | 0 .. 127 | 127 | native real value / discrete linear |
| adi.pitch / mode | 0 .. 2 | 0 | native real value / discrete linear |
| adi.pitch / use_scale | 0 .. 1 | 0 | native real value / discrete linear |
| adi.pitch / scale_root | 0 .. 11 | 0 | native real value / discrete linear |
| adi.pitch / scale_mask | 1 .. 4095 | 2741 | native real value / discrete linear |
| adi.random / chance | 0 .. 1 | 0 | native real value / linear |
| adi.random / choices | 1 .. 24 | 12 | native real value / discrete linear |
| adi.random / interval | 1 .. 12 | 1 | native real value / discrete linear |
| adi.random / sign | 0 .. 2 | 0 | native real value / discrete linear |
| adi.random / mode | 0 .. 1 | 0 | native real value / discrete linear |
| adi.random / seed | 1 .. 16777215 | 1 | native real value / discrete linear |
| adi.random / use_scale | 0 .. 1 | 0 | native real value / discrete linear |
| adi.random / scale_root | 0 .. 11 | 0 | native real value / discrete linear |
| adi.random / scale_mask | 1 .. 4095 | 2741 | native real value / discrete linear |
| adi.scale / root | 0 .. 11 | 0 | native real value / discrete linear |
| adi.scale / transpose | -36 .. 36 | 0 | native real value / discrete linear |
| adi.scale / fold | 0 .. 1 | 0 | native real value / discrete linear |
| adi.scale / lowest | 0 .. 127 | 0 | native real value / discrete linear |
| adi.scale / range | 0 .. 127 | 127 | native real value / discrete linear |
| adi.scale / map0 | -1 .. 11 | 0 | native real value / discrete linear |
| adi.scale / map1 | -1 .. 11 | 1 | native real value / discrete linear |
| adi.scale / map2 | -1 .. 11 | 2 | native real value / discrete linear |
| adi.scale / map3 | -1 .. 11 | 3 | native real value / discrete linear |
| adi.scale / map4 | -1 .. 11 | 4 | native real value / discrete linear |
| adi.scale / map5 | -1 .. 11 | 5 | native real value / discrete linear |
| adi.scale / map6 | -1 .. 11 | 6 | native real value / discrete linear |
| adi.scale / map7 | -1 .. 11 | 7 | native real value / discrete linear |
| adi.scale / map8 | -1 .. 11 | 8 | native real value / discrete linear |
| adi.scale / map9 | -1 .. 11 | 9 | native real value / discrete linear |
| adi.scale / map10 | -1 .. 11 | 10 | native real value / discrete linear |
| adi.scale / map11 | -1 .. 11 | 11 | native real value / discrete linear |
| adi.velocity / operation | 0 .. 2 | 0 | native real value / discrete linear |
| adi.velocity / mode | 0 .. 2 | 0 | native real value / discrete linear |
| adi.velocity / lowest | 0 .. 1 | 0 | native real value / linear |
| adi.velocity / range | 0 .. 1 | 1 | native real value / linear |
| adi.velocity / out_low | 0 .. 1 | 0 | native real value / linear |
| adi.velocity / out_high | 0 .. 1 | 1 | native real value / linear |
| adi.velocity / drive | -1 .. 1 | 0 | native real value / linear |
| adi.velocity / compand | -1 .. 1 | 0 | native real value / linear |
| adi.velocity / random | 0 .. 1 | 0 | native real value / linear |
| adi.velocity / seed | 1 .. 16777215 | 1 | native real value / discrete linear |
| adi.cc_control / modulation | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / pitch_bend | -48 .. 48 | 0 | native real value / linear |
| adi.cc_control / pressure | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller0 | 0 .. 127 | 64 | native real value / discrete linear |
| adi.cc_control / value0 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller1 | 0 .. 127 | 1 | native real value / discrete linear |
| adi.cc_control / value1 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller2 | 0 .. 127 | 2 | native real value / discrete linear |
| adi.cc_control / value2 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller3 | 0 .. 127 | 3 | native real value / discrete linear |
| adi.cc_control / value3 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller4 | 0 .. 127 | 4 | native real value / discrete linear |
| adi.cc_control / value4 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller5 | 0 .. 127 | 5 | native real value / discrete linear |
| adi.cc_control / value5 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller6 | 0 .. 127 | 6 | native real value / discrete linear |
| adi.cc_control / value6 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller7 | 0 .. 127 | 7 | native real value / discrete linear |
| adi.cc_control / value7 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller8 | 0 .. 127 | 8 | native real value / discrete linear |
| adi.cc_control / value8 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller9 | 0 .. 127 | 9 | native real value / discrete linear |
| adi.cc_control / value9 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller10 | 0 .. 127 | 10 | native real value / discrete linear |
| adi.cc_control / value10 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller11 | 0 .. 127 | 11 | native real value / discrete linear |
| adi.cc_control / value11 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / controller12 | 0 .. 127 | 12 | native real value / discrete linear |
| adi.cc_control / value12 | 0 .. 1 | 0 | native real value / linear |
| adi.cc_control / channel | 0 .. 15 | 0 | native real value / discrete linear |
| adi.cc_control / send_all | 0 .. 1 | 0 | native real value / discrete linear |
| adi.cc_control / learn | 0 .. 13 | 0 | native real value / discrete linear |
| adi.expression_control / source | 0 .. 9 | 0 | native real value / discrete linear |
| adi.expression_control / rise | 0 .. 1000 | 0 | native real value / linear |
| adi.expression_control / fall | 0 .. 1000 | 0 | native real value / linear |
| adi.expression_control / curve_x | 0.001 .. 0.999 | 0.5 | native real value / linear |
| adi.expression_control / curve_y | 0 .. 1 | 0.5 | native real value / linear |
| adi.expression_control / s_curve | 0 .. 1 | 0 | native real value / discrete linear |
| adi.expression_control / steps | 1 .. 32 | 8 | native real value / discrete linear |
| adi.expression_control / random | 0 .. 1 | 1 | native real value / linear |
| adi.expression_control / seed | 1 .. 16777215 | 1 | native real value / discrete linear |
| adi.expression_control / depth | 0 .. 1 | 1 | native real value / linear |
| adi.mpe_control / pitch | 0 .. 4 | 1 | native real value / linear |
| adi.mpe_control / pressure_curve | 0.125 .. 8 | 1 | native real value / linear |
| adi.mpe_control / slide_curve | 0.125 .. 8 | 1 | native real value / linear |
| adi.mpe_control / rise | 0 .. 1000 | 0 | native real value / linear |
| adi.mpe_control / fall | 0 .. 1000 | 0 | native real value / linear |
| adi.mpe_control / slide_mode | 0 .. 2 | 0 | native real value / discrete linear |
| adi.mpe_control / swap | 0 .. 1 | 0 | native real value / discrete linear |
| adi.mpe_control / pressure_to_slide | 0 .. 1 | 0 | native real value / discrete linear |
| adi.mpe_control / slide_to_pressure | 0 .. 1 | 0 | native real value / discrete linear |
| adi.mpe_control / pressure_to_channel | 0 .. 1 | 0 | native real value / discrete linear |
| adi.mpe_control / slide_to_mod | 0 .. 1 | 0 | native real value / discrete linear |
| adi.mpe_control / pitch_to_channel | 0 .. 1 | 0 | native real value / discrete linear |
| adi.mpe_control / pressure_default | 0 .. 1 | 0 | native real value / linear |
| adi.mpe_control / slide_default | 0 .. 1 | 0.5 | native real value / linear |
| adi.microtuner / morph | 0 .. 1 | 0 | native real value / linear |
| adi.microtuner / transpose | -48 .. 48 | 0 | native real value / linear |
| adi.microtuner / reference | 220 .. 880 | 440 | native real value / linear |
| adi.midi_monitor / freeze | 0 .. 1 | 0 | native real value / discrete linear |
| adi.shaper / rate | 0.01 .. 40 | 1 | native real value / linear |
| adi.shaper / sync | 0 .. 1 | 0 | native real value / discrete linear |
| adi.shaper / beats | 0.015625 .. 32 | 1 | native real value / linear |
| adi.shaper / loop | 0 .. 1 | 0 | native real value / discrete linear |
| adi.shaper / velocity | 0 .. 1 | 0 | native real value / linear |
| adi.shaper / offset | -1 .. 1 | 0 | native real value / linear |
| adi.shaper / jitter | 0 .. 1 | 0 | native real value / linear |
| adi.shaper / smooth | 0 .. 1000 | 0 | native real value / linear |
| adi.shaper / depth | 0 .. 1 | 1 | native real value / linear |
| adi.shaper / echo | 0 .. 0.99 | 0 | native real value / linear |
| adi.shaper / echo_time | 1 .. 2000 | 250 | native real value / linear |
