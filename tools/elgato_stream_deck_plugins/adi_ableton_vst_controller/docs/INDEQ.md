# Analog Obsession INDEQ controller

`IndeqController` (`js/controllers/IndeqController.js`) is a predefined strategy
for **Analog Obsession INDEQ (VST3)** — a fixed 3-band EQ. No dynamic state: 6
knobs map to the 6 dials, 6 toggles map to touch zones. Parameters resolve by
**name** from the bridge's `all_params`; override with `IndeqController.OVERRIDES`.

## Dials

| Dial | Parameter | Type | Channel cmd |
|------|-----------|------|-------------|
| 1 | **Low Gain** | continuous (dB) | `delta_index` |
| 2 | **Low Frequency** | stepped — 35 / 60 / 100 / 220 Hz | `step_index` |
| 3 | **Mid Gain** | continuous (dB) | `delta_index` |
| 4 | **Mid Frequency** | stepped — .2 / .35 / .7 / 1.5 / 3 / 6 kHz | `step_index` |
| 5 | **High Gain** | continuous (dB) | `delta_index` |
| 6 | **Output** | continuous (dB) | `delta_index` |

Pressing a dial mirrors that zone's top toggle.

## Touch zones (above/below each dial)

| Zone | Top | Middle | Bottom |
|------|-----|--------|--------|
| 1 (Low Gain) | **Highpass Filter** (OFF/ON) | Low Gain dB | — |
| 2 (Low Freq) | **Low Band Shape** (Shelf/Peak) | Low Freq | — |
| 3 (Mid Gain) | **Mid Bandwidth** (Normal/High) | Mid Gain dB | — |
| 4 (Mid Freq) | — | Mid Freq | — |
| 5 (High Gain) | **High Band Shape** (Shelf/Peak) | High Gain dB | **High Frequency** (8kHz/16kHz) |
| 6 (Output) | **Bypass** (I/O) | Output dB | — |

All toggles are 2-state → `toggle_index`. Their on-screen label uses Live's own
`value_items` when the parameter is quantized (so it shows the plugin's real
state names), otherwise the controller's fallback labels above.

## Expected Ableton parameter names

Confirmed against a live Ableton INDEQ instance — these are the exact 12 names it
exposes (the resolver matches them, normalized case-insensitively):

`Low Gain` · `Low Frequency` · `Mid Gain` · `Mid Frequency` · `High Gain` · `Output` ·
`Highpass Filter` · `Low Band Shape` · `Mid Bandwidth` · `High Band Shape` ·
`High Frequency` · `Bypass`

GUI ↔ parameter map (the red buttons on the plugin panel): **HPF** = Highpass
Filter · **LPK** = Low Band Shape (Shelf/Peak) · **HIQ** = Mid Bandwidth
(Normal/High) · **HPK** = High Band Shape · **kHz 8/16** = High Frequency ·
**I/O** = Bypass. Stepped freqs read back as `35Hz…220Hz` and `.2kHz…6kHz`.

> `Bypass` is INDEQ's I/O switch; if your build doesn't expose it, the resolver
> falls back to Live's `Device On`. Stepped-freq dials advance one step per dial
> detent. If a name differs in your build, set e.g.
> `IndeqController.OVERRIDES = { output: 'Out', low_freq: 'Low Frequency' }`
> (exact name or numeric index); unresolved roles are logged to Live's `Log.txt`.
> Some VST3s only expose parameters after you **Configure** them in Live.
