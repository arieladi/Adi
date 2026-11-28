# External Audio Effect: the canvas and panel contract

Live 12's External Audio Effect sends a track's signal out of the audio
interface to a hardware processor, and brings the processor's output back in
its place. This file is the devices lane's part of it (PLAN DEV-22): the
canonical canvas, the controls and their fixed ids, the latency fields, and
what is handed to the engine. The routing to the interface, the delay
compensation and the real-time render are the engine's (MX-24, and DEV-05's
compensation). Nothing here is built yet; it lands after 3 December.

The values marked *(reading 31)* are ADI's until a Live 12.4 reading of
External Audio Effect, on the devices log's ONE list. Until then the device
claims no parity.

## 1. The canvas

`pd/devices/External Audio Effect.pd`, made like every built-in effect's:

```
adc~ 1 2  ->  adi.external_audio_effect~  ->  dac~ 1 2
              ^ the adi.param boxes of §2, each through a "p <id-1> $1" message
              ^ r $0-query_latency -> latency -> s $0-report_latency (outlet 3)
```

- **No hardware in Pd.** A Pd host has no route to the interface. The
  external passes the dry input at unity, whatever the controls say, and
  reports 0 latency. The track keeps sounding when the canvas is opened
  outside the engine.
- **The engine substitutes it.** The built-in host recognises the canvas by its
  bytes, as for every native device (`native_devices.hpp`), and puts the
  engine's External Audio Effect node in its place (§4). An edited canvas stays
  on the Pd path, and therefore passes the dry signal.
- **One per track position.** It is an ordinary device in the chain. It may sit
  inside a rack chain, where its latency counts for that chain.

## 2. The controls and their ids

The ids are the `adi.param` ids: one-based, fixed, and never reused. A
control is never renumbered. A new control takes the next free id.

| id | Name | Range | Default | Unit | Curve | What it does |
|---:|---|---|---|---|---|---|
| 1 | `Gain` | −70 to +36 | 0 | dB | lin | The level sent to the hardware. −70 is silence. *(reading 31: range and default)* |
| 2 | `Dry/Wet` | 0 to 100 | 100 | % | lin | The return's share of the output. The dry part is delayed to meet the return (§3). |
| 3 | `Hardware_Latency` | 0 to 1,000 | 0 | ms | lin | The processor's own delay, set by the user. It is added to the interface's round trip (§3). *(reading 31: its range, and how fine its steps are)* |
| 4 | `Latency_Unit` | 0 to 1 | 0 | – | menu ` ms samples` | How id 3 is shown and entered. It is display only: id 3 is always stored in ms. |
| 5 | `Invert` | 0 to 1 | 0 | – | toggle | Inverts the return's polarity. |

**The two routing choosers are not parameters.** `Audio To` (the interface
outputs that the send goes to) and `Audio From` (the inputs that the return
comes from) are not `adi.param` controls, and they cannot be automated. They are
routing rows that the engine owns (§4):

- The panel shows them as choosers.
- The set stores them with the routing table, keyed by the device's instance
  id.
- An empty chooser means "none". The device then passes the dry input at unity,
  as on the Pd path, and the panel says "No output" or "No input".
- Live offers mono and stereo channel pairs in the same chooser *(reading 31:
  the list as Live 12.4 shows it)*.

**The panel** shows, left to right:
- the `Audio To` chooser, with the send's peak meter beside it;
- `Gain`;
- the `Audio From` chooser, with the return's peak meter;
- `Dry/Wet`;
- `Hardware_Latency`, with `Latency_Unit` as its context menu;
- `Invert`.

The two meters are readings that the engine publishes (§4); the canvas has no
outlet for them.

## 3. The latency fields

The device reports one number to delay compensation, in samples at the
session rate. It is the sum of three fields, each kept apart so that the panel
and the tests can name it:

| Field | Source | When it changes |
|---|---|---|
| `hardwareLatencySamples` | id 3, in samples: `round(ms / 1000 × rate)` | when the user moves id 3, or the rate changes |
| `roundTripSamples` | the interface driver's reported output latency plus input latency, for the chosen channels | when the driver, its buffer size or the chosen channels change |
| `blockSamples` | the engine's own buffering between the send and the return: one processing block, if the realiser needs one to read an input it wrote in the same callback | when the block size changes |

- **The total.** `latencySamples() = hardwareLatencySamples + roundTripSamples
  + blockSamples`. It is reported while the device is bypassed too, as for
  every built-in (PLAN DEV-22). A bypass therefore does not move the track in
  time.
- **The dry path.** The dry signal is delayed by the same total inside the
  device, so the dry and the return meet at every `Dry/Wet`. *(reading 31: does
  Live align the dry with the return, or leave it early?)*
- **When the total changes**, the engine re-runs compensation, as for any
  device whose latency changes. The change is not glided.

## 4. What the engine takes over (the hand-off)

The devices lane gives the canvas, the ids and the fields above. The engine
seat builds the rest, under MX-24 and DEV-05:

1. **The routing rows.** A send row from the device's send tap to the chosen
   interface outputs, and a return row from the chosen inputs back to the
   device. The rows are keyed by the device's instance id and stored with the
   set. When the device is deleted they are removed, and an undo restores them.
2. **The node.** It takes the canvas's place in the graph:
   - it writes `input × Gain` to the send;
   - it reads the return, inverts it if `Invert` is on, and mixes it with the
     delayed dry by `Dry/Wet`;
   - it reports §3's total.
   The node is the engine's because it reaches the interface. There is no DSP
   core in `src/adi/dsp` for it.
3. **Compensation.** The total enters the planner like any device latency, so
   the track stays aligned with the rest of the set.
4. **A broken path reports itself.** If a chosen channel vanishes (the
   interface changes, or a driver is lost), the node passes the dry at unity.
   The panel shows the chooser as missing, and the session's diagnostics name
   the device. The output is never a silent zero (MX-24).
5. **Renders.** A render through the device runs in real time, with Live's
   silence wait (PLAN, the export contract's item 4). An offline render marks
   the device as unavailable rather than skipping it silently.
6. **The meters.** The send's and the return's peak levels, published to the
   UI the way the Tuner's reading will be. This is the same need, so one
   mechanism serves both.

## 5. Tests the build must carry

- **Canvas.** The canvas loads through the Pd path and passes the dry input
  bit-exactly, with 0 latency, at any control values.
- **Ids.** The catalogue entry and the canvas declare ids 1 to 5 with §2's
  ranges and defaults. A parity fixture moves each of them off its default.
- **Latency.** With a loop-back fixture in place of the interface (send
  written straight back to the return, plus a known delay), an impulse comes
  back at exactly `latencySamples()`. The dry meets it at `Dry/Wet` 50 %.
  Moving id 3 by 1 ms moves the total by `rate / 1000` samples, rounded.
- **Bypass.** A bypassed device reports the same latency.
- **Broken path.** Removing the return channel gives the dry at unity and a
  diagnostic, never silence.
- **Allocation.** The node's audio path allocates nothing.
