# Bridge protocol (v1)

JSON messages over a WebSocket. The **Ableton Remote Script is the server**
(`ws://127.0.0.1:9006` by default); the **Stream Deck plugin is the client**.
Every message is a single JSON object terminated by one WebSocket text frame.

The plugin also keeps a *second*, unrelated socket open — the Elgato
registration socket — so it talks to two servers at once: Stream Deck (device
I/O) and this bridge (Live state). Keep the two mentally separate.

## Bridge → plugin (state)

| `t` | Payload | Meaning |
|-----|---------|---------|
| `hello` | `version`, `live` | Sent on connect / handshake. |
| `track` | `name`, `index`, `color` | Selected track changed. |
| `device` | `name`, `class_name`, `index`, `has_device`, `controller` (`"eq8"`/`"generic"`), `param_count` | Selected device changed. `controller` tells the client which `DeviceController` strategy to use. |
| `params` | `page`, `pages`, `params:[{slot,pidx,name,value,min,max,disp,quantized}]` | Generic-mode parameter snapshot (the 6 mapped, non-quantized params). |
| `param` | `slot`, `value`, `disp` | Real-time single-parameter update (dial move or automation). |
| `eq8` | `page`, `focus`, `output`, `bands:[{i,on,freq,gain,q,type,type_name,type_items}]` | Full EQ Eight snapshot. `focus` = first band (1-based) the 6 dials control. |
| `eq8_band` | `i`, + any band fields | Real-time single-band update. |
| `eq8_state` | `count`, `selected_is_eq8`, `selected_index` | How many EQ8s on the track + whether the selected device is one (drives the EQ8 key glyph). |
| `presets` | `items:[{id,name}]` | EQ8 preset list (from the configured User Library folder). |
| `error` | `message` | Non-fatal bridge error (shown via the plugin log / alert). |

## Plugin → bridge (commands)

| `c` | Payload | Action |
|-----|---------|--------|
| `subscribe` | — | Request a full state resend (track + device + params/eq8). |
| `param_delta` | `slot`, `delta` | Generic: nudge mapped parameter `slot` by `delta` *normalized* units (ticks × step). Bridge maps to `[min,max]`. |
| `param_set` | `slot`, `norm` | Generic: set mapped parameter `slot` to absolute normalized `norm` (0..1). |
| `eq8_freq_delta` | `band`, `delta` | Nudge band frequency (normalized, log-mapped by the bridge). |
| `eq8_toggle_band` | `band` | Toggle a band's enable. |
| `eq8_cycle_type` | `band`, `dir` (±1) | Step the band's filter type / cutoff mode. |
| `eq8_page` | `dir` (±1) | Shift the 6-dial focus window (bands 1-6 → 2-7 → 3-8). |
| `eq8_key` | — | Context-dependent EQ8 launcher (conditions A/B/C below). |
| `eq8_list_presets` | — | Ask for the preset list. |
| `eq8_load_preset` | `id` | Load preset onto the **current** EQ8 (replace-in-place; see caveat). |
| `eq8_new_preset` | `id` | Drop a **new** EQ8 instance using that preset. |
| `select_track` | `dir` (±1) | Move track selection. |
| `select_device` | `dir` (±1) | Move device selection on the track. |
| `ping` | — | Keep-alive; bridge replies `hello`. |

### EQ8 key conditions

- **A** — selected device *is* an EQ8 → select the **next** EQ8 on the track (cycle).
- **B** — selected device is *not* an EQ8 but the track has ≥1 → select the EQ8 **closest** (by device index) to the current selection.
- **C** — no EQ8 on the track → **create** one (`browser.load_item`).

### Preset caveat

The Live API cannot rewrite an existing device's state from a `.adv` preset in
place. `eq8_load_preset` therefore loads the preset as a new EQ8 right after the
current one and deletes the previous instance (a functional "replace"), while
`eq8_new_preset` simply adds it. Both go through `browser.load_item`.
