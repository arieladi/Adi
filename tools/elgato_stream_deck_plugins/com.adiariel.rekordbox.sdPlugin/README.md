# Adi Ariel RekordBox MIDI — Stream Deck + XL plugin

A class-compliant **virtual MIDI controller for rekordbox PERFORMANCE mode**, built for the
Stream Deck **+ XL** (device type 13: 9×4 keys, 6 dials, 1200×100 touch strip).
Node plugin (`@elgato/streamdeck` 1.4.x) + `easymidi` → `@julusian/midi` with **prebuilt
native bindings committed in `vendor/`** — end users never run npm or a compiler, on
macOS (Apple Silicon + Intel) and Windows (x64 + arm64) alike.

Dual-deck control surface: 8 hot cues per deck with a **shift layer** (delete), play/cue
transport, jog **nudge** for phase alignment, browser navigation, and per-deck
**volume / filter / BPM** dials with **beat-jump** touch-strip zones.

> ⚠️ **rekordbox plan requirement** — MIDI LEARN needs a **Core plan or higher**, *or* a
> connected Hardware Unlock device (CDJ-3000, XDJ-RX3, DDJ-800, DDJ-FLX10, …).
> The free plan — including the "**Free Plus**" badge you get for registering
> AlphaTheta/Pioneer hardware — does **not** unlock MIDI LEARN. The 30-day rekordbox
> trial includes it, so you can test before subscribing.

---

## How it works

```
Stream Deck + XL ──> this plugin (Node 20, runs inside the Stream Deck app)
                        │  easymidi -> @julusian/midi (prebuilt RtMidi binding)
                        ▼
        macOS: virtual CoreMIDI source "Adi RekordBox Controller"
        Windows: attaches to an existing loopMIDI port (auto-retry)
                        ▼
        rekordbox PERFORMANCE mode — [MIDI] window, MIDI LEARN mapping
```

MIDI is **one-way** (controller → rekordbox). Dials keep an internal absolute value
(0–127) per deck, shown live on the touch strip, and send plain 7-bit CC — so in
rekordbox every knob row uses the simple **Knob/Slider (0h–7Fh)** type.

## Install

1. Copy `com.adiariel.rekordbox.sdPlugin` into the Stream Deck plugins folder
   (or double-click a packaged `.streamDeckPlugin`):
   - macOS: `~/Library/Application Support/com.elgato.StreamDeck/Plugins/`
   - Windows: `%APPDATA%\Elgato\StreamDeck\Plugins\`
2. **Windows only:** install [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html)
   and create a port named exactly **`Adi RekordBox Controller`** (the name is editable in
   any action's property inspector; keep them in sync). Set loopMIDI to autostart.
   macOS needs nothing — the plugin creates the virtual port itself.
3. Restart the Stream Deck app. Verify the port exists: macOS *Audio MIDI Setup → MIDI
   Studio*; Windows — the loopMIDI window.
4. **Start order matters for rekordbox:** Stream Deck app (plugin creates/attaches the
   port) **first**, *then* launch rekordbox. If the port appears mid-session, restart
   rekordbox — its MIDI device list is read at startup.

## Suggested + XL layout (matches the annotated reference photo)

Actions are freely placeable; this is the layout the MIDI map was designed around.

```
col:     0      1      2      3      4      5      6      7      8
row0   [Lnch] [ ▲ ]  [ ▼ ]  [ ⊞ ]  [    ] [    ] [    ] [    ] [    ]   browser (unmarked)
row1   [A1]   [A2]   [A3]   [A4]   [SHIFT][B1]   [B2]   [B3]   [B4]    hot cues (green) + shift (yellow)
row2   [A5]   [A6]   [A7]   [A8]   [SHIFT][B5]   [B6]   [B7]   [B8]
row3   [◀◀A]  [▶▶A]  [PLAY A][CUE A][    ][CUE B][PLAY B][◀◀B] [▶▶B]   nudge (purple) + transport (CDJ colors)

dials   BPM A   FLT A   VOL A  │  VOL B   FLT B   BPM B                 grey · black · red │ red · black · grey
touch   each dial's 200px zone: tap left half = Beat Jump ◀, right half = Beat Jump ▶ (that dial's deck)
```

Set each action's **Deck** (A/B) and role/slot in its property inspector.
`▲/▼` = library scroll (hold to auto-repeat), `⊞` = tree ⇄ track-list focus toggle.
Both SHIFT keys are equivalent — hold either with any hand.
Transport keys use CDJ/OMNIS-DUO button visuals: PLAY/PAUSE is the green ►❚❚,
CUE is the orange-lit round button with a CUE label. (One-way MIDI has no
feedback channel, so the keys can't light with rekordbox's play state.)

### One-time profile export (the launcher key)

**Launch RekordBox Controller** calls `switchToProfile(deviceId, "AdiRekordBox")`. A
`.streamDeckProfile` is a binary file and can't ship as text — create it once: add a
profile named exactly **`AdiRekordBox`** for your Stream Deck + XL, arrange the actions
(grid above), right-click the profile → **Export** → save `AdiRekordBox.streamDeckProfile`
into this plugin's root folder (next to `manifest.json`). Until then the launcher logs a
friendly error instead of switching. Note: `switchToProfile` is fire-and-forget in the
SDK — check the Stream Deck log if nothing happens.

## Mapping in rekordbox (MIDI LEARN)

1. Start the Stream Deck app, then rekordbox. Switch to **PERFORMANCE** mode.
2. Click the **[MIDI]** button in the upper right (next to the gear icon).
3. In the MIDI settings window, pick **Adi RekordBox Controller** in the device
   drop-down (top left) — mappings are per-device.
4. Pick the category tab (DECK / MIXER / BROWSER / OTHERS), click **[ADD]**, choose the
   function, then set the **Type** cell (see table).
5. Select the row, enable **[LEARN]**, press/turn the Stream Deck control — the code
   lands in **MIDI IN** — then toggle LEARN off. *Or skip learning entirely:* click the
   MIDI IN cell and **type the 4-digit hex code** from the table below.
6. Mappings auto-save when the window closes. **[EXPORT]** a `.csv` backup when done —
   **[DEFAULT]** wipes a generic controller's map to empty.

Use **[DUPLICATE]** + the Deck/CH column to make the Deck B row from a Deck A row
(deck B codes are one channel up: `90→91`, `B0→B1`).

### MIDI implementation chart

Channels: **Ch 1** = Deck A, **Ch 2** = Deck B, **Ch 3** = browser/global.
Buttons send Note On (vel 127) on press and Note Off on release — held functions
(nudge, cue audition) behave like real hardware.

| Stream Deck control | MIDI (Deck A / Deck B) | hex A / B | rekordbox function | Type |
|---|---|---|---|---|
| Hot Cue 1–8 | Note 16–23 | `9010`–`9017` / `9110`–`9117` | `PAD1..8 Hot Cue` (Call) | Button (for Pad) |
| **Shift** + Hot Cue 1–8 | Note 24–31 | `9018`–`901F` / `9118`–`911F` | `PAD1..8 Hot Cue Delete` | Button (for Pad) |
| PLAY | Note 32 | `9020` / `9120` | `PlayPause` | Button |
| **Shift** + PLAY | Note 33 | `9021` / `9121` | e.g. `JumpToTrackStart` | Button |
| CUE | Note 34 | `9022` / `9122` | `HeadphoneCue` (or `MasterCue`) | Button |
| **Shift** + CUE | Note 35 | `9023` / `9123` | e.g. `TempoReset` | Button |
| Nudge ◀◀ (held) | Note 36 | `9024` / `9124` | `PitchBendDown` ¹ | Button |
| Nudge ▶▶ (held) | Note 37 | `9025` / `9125` | `PitchBendUp` ¹ | Button |
| Volume dial **push** | Note 38 | `9026` / `9126` | `Load` (set Deck column) | Button |
| Touch strip tap, left half | Note 40 | `9028` / `9128` | `BeatJumpRev` ² | Button |
| Touch strip tap, right half | Note 41 | `9029` / `9129` | `BeatJumpFwd` ² | Button |
| **Volume dial** (red) | CC 20 abs. | `B014` / `B114` | `ChannelFader` (CH1/CH2) | **Knob/Slider (0h–7Fh)** |
| **Filter dial** (black) | CC 21 abs. | `B015` / `B115` | `CFXParameterCH1/CH2` ³ | **Knob/Slider (0h–7Fh)** |
| **BPM dial** (grey) | CC 22 abs. | `B016` / `B116` | `TempoSlider` ⁴ | **Knob/Slider (0h–7Fh)** ⁴ |
| Browse ▲ / ▼ | Note 50/51, **Ch 3** | `9232` / `9233` | `BrowseUp` / `BrowseDown` | Button |
| Tree ⇄ list ⊞ | Note 52, **Ch 3** | `9234` | `SwitchActiveWindow` (or `Back`/`Forward`) | Button |

¹ rekordbox reserves the *jog* functions (JogPitchBend/JogScratch) for Pioneer hardware;
the button functions `PitchBendUp`/`PitchBendDown` are the supported nudge equivalent.
If they don't appear in your ADD menu, use the officially supported CSV route:
[EXPORT] the mapping, add the rows in a text editor, [IMPORT] it back.
² Jumps by the beat-jump size currently selected in the rekordbox GUI (fixed-size
variants like `4BeatJump>>` also exist, and `PageLeft/Right.BeatJump` changes the size).
³ CFX must be set to FILTER in the GUI (or also map the `CFX.FILTER` selector).
⁴ `TempoSlider` defaults to the 14-bit **Knob/Slider (0h–3FFFh)** type — **change it to
(0h–7Fh)** or the dial won't track. Strictly BPM: no key/pitch functions are mapped.
rekordbox does **not** see Program Change messages, and one MIDI code can only drive one
function.

## Dials, shift layer, touch strip — behavior details

- **Endless dials → absolute CC**: the plugin accumulates 0–127 per deck (volume starts
  at 127, filter/tempo at center 64), shows the value on the touch strip, and persists
  it across restarts. Rotate speed × per-dial **sensitivity** (property inspector).
  Because it's absolute, the on-screen fader snaps to the dial's stored value if they
  drift apart (one-way MIDI has no feedback channel).
- **Volume push = Load** the selected track to that deck. **Filter push** snaps the
  filter back to center (64). **BPM push does nothing** — deliberately, so a bumped dial
  never jumps your tempo mid-mix.
- **Shift** is local to the plugin (sends no MIDI itself): while held, hot cue keys
  repaint to **DEL n** and send the delete notes; PLAY/CUE send their shifted notes.
  Release order is safe — a key always sends the Note Off matching its Note On.
- **Touch strip**: every dial owns a 200×100 zone; tapping left/right half beat-jumps
  that dial's deck. With the suggested layout, the three left zones jump Deck A, the
  three right zones Deck B.

## Build from source (dev machine only)

```
npm install            # deps incl. easymidi (prebuilt native binding — no compiler)
npm run build          # rollup: src/*.js -> bin/plugin.js (committed)
npm run vendor         # copy the runtime MIDI stack into vendor/ (committed)
python3 scripts/gen_icons.py   # regenerate imgs/ (stdlib PNG writer; uses Pillow for the CUE label if installed)
python3 scripts/validate.py    # manifest + assets + vendor sanity (exit 0 = OK)
npm run smoke          # REAL virtual-port loopback test (mac/linux): 19 messages
```

`vendor/node_modules` is committed on purpose: `@julusian/midi` ships N-API 7 prebuilds
(`darwin-arm64/x64`, `win32-x64/arm64` — ~1 MB total) that `pkg-prebuilds` resolves via
`__dirname`, so they must exist on disk, unbundled, at runtime. `src/midi-out.js` loads
them with `createRequire(vendor/_resolve_.cjs)`. On Windows RtMidi cannot create virtual
ports — and fails *silently* if asked — which is why the plugin explicitly attaches to a
loopMIDI port there instead (scan → open → retry every 3 s until it appears).

## Troubleshooting

- **rekordbox doesn't list the controller** — it was started before the port existed
  (restart rekordbox), or on Windows loopMIDI isn't running / the port name doesn't
  match the plugin's (see the property inspector's MIDI-port field).
- **MIDI window is locked / LEARN greyed out** — plan gating; see the warning at the top.
- **Dial maps but the fader doesn't move** — the row's Type is still `0h–3FFFh`
  (14-bit); switch it to `Knob/Slider (0h–7Fh)`.
- **Nudge functions missing from ADD** — see note ¹ (CSV export → edit → import).
- **Logs** — Stream Deck app logs + `logs/com.adiariel.rekordbox.*.log` inside the
  plugin folder (port creation/attach messages, reconnects).
- Renaming the port to a Pioneer device name (e.g. `PIONEER DDJ-SX`) makes rekordbox
  load that device's full factory preset instead of a blank map — a known trick for
  unlocking jog functions, but then *this* plugin's map doesn't apply; only do it if you
  know what you're after.

## Version

1.0.0.0 — see `CHANGELOG.md`. Verified: `validate.py`, rollup build boots to the
expected registration handshake, and a 19-message virtual-port loopback smoke test
(hot cue on/off, shift-delete, held nudge, transport, load, beat jump, browse, all
three CC lanes) passed on macOS arm64 through the exact committed vendor tree.
