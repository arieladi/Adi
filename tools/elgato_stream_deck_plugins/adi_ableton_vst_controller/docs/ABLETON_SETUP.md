# Ableton setup (AdiVST Remote Script)

## 1. Install the Remote Script

`scripts/install-mac.sh` / `scripts/install-windows.ps1` copy it for you. Manual:

- **macOS:** copy `ableton/remote_script/AdiVST` →
  `~/Music/Ableton/User Library/Remote Scripts/AdiVST`
- **Windows:** copy it →
  `…\Documents\Ableton\User Library\Remote Scripts\AdiVST`

(You can also drop it into the app's bundled `MIDI Remote Scripts` folder, but the
User Library location survives Live updates.)

## 2. Select it in Live

Restart Live, then **Settings → Link/Tempo/MIDI → MIDI**: set a free **Control
Surface** slot to **AdiVST**. Input/Output can stay *None* — this script doesn't
use MIDI ports, it opens a local WebSocket. You'll see
`AdiVST: Stream Deck bridge on port 9006` in Live's status bar.

## 3. Port

Defaults to **9006** on both sides. To change it, edit `PORT` in
`AdiVST/AdiVST.py` and set the same value in the plugin's Property Inspector
("Ableton bridge → WebSocket port"). The bridge only listens on `127.0.0.1`.

## 4. EQ8 presets folder

Create a folder named **`EQ8 Presets`** in your Live **User Library** and save
your EQ Eight `.adv` presets there. The plugin's preset keys list its loadable
items. Change the folder name via `PRESET_FOLDER` in `AdiVST.py`.

> Replace caveat: the Live API can't rewrite an existing device from a preset in
> place, so "load onto current EQ8" inserts the preset as a new EQ8 after the
> selected one and deletes the old instance. "New instance" just inserts it.

## External VST2/VST3/AU (e.g. Pulsar, Massive)

Generic mode works on plugin devices through the same `device.parameters` API —
the first 6 **non-quantized** parameters map to the 6 dials. Caveat: some VST3s
expose only a subset (or generically named) parameters until you **Configure**
them in Live's device view (click the device title-bar wrench / "Configure", move
the knobs you want, then they appear by name). After configuring, those become
the parameters the dials pick up.

## Troubleshooting

- **Nothing connects** — confirm AdiVST is selected as a Control Surface and the
  status bar showed the "bridge on port 9006" message; confirm the plugin's port
  matches. Check Live's `Log.txt` for `AdiVST` lines.
- **Logs** — `log_message` output goes to Live's `Log.txt`
  (macOS `~/Library/Preferences/Ableton/Live x.x/Log.txt`).
- **Multiple Lives** — only one process can bind the port; the second logs a bind
  failure.

## Max for Live alternative

If you prefer not to use a Remote Script, the same JSON protocol (docs/PROTOCOL.md)
can be served from a Max for Live device using **Node for Max** (`node.script`
running a `ws` server) bridged to `live.object`/`live.path`. The client side is
unchanged. A starter is sketched in `ableton/max_for_live/README.md`.
