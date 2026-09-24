# Settings — the store behind the Settings window

The core library every Settings page reads and writes: `src/adi/settings/**`,
tested by `tests/test_settings.cpp`. It has no UI. The rulings are ADR-0125
(R-01 to R-07, d1 to d3), ADR-0127 d5, ADR-0145 and ADR-0149. The decisions
this library adds are ADR-0152.

## 1. The registry (`registry.hpp`)

One typed table of every setting:

- **key**, e.g. `audio.bufferSize`. Permanent, like an op name.
- **type**: bool, int, real, text, choice, path or path list.
- **scope** (R-03 badge): App, Project or Device.
- **page**: the left-hand list (R-01).
- **label** and **help**, which the Find box searches (R-02). Every word of the
  query must match somewhere in the label, help, key or page, ignoring case.
- **default**, the legal **choices** or **range**, the **op** for a Project
  setting, and whether the **agent** may change it.

| Scope | Where the value lives | Who writes it |
|---|---|---|
| App | `settings.json` of this application (§2) | the store |
| Project | a row in the `.adi` | only its op, named in the registry (OPS.md §9); the store refuses it and names the op |
| Device | the device's own state | the device |

A setting that is not in the registry does not exist: the store refuses an
unknown key from a caller.

### The settings of ADR-0145

| Key | Page | Default | Ruling |
|---|---|---|---|
| `lookfeel.zoomOnSelection` | Look & Feel | off | d2: Ctrl/Cmd + wheel zooms around the selection, as Live does |
| `audio.silentResampling` | Audio | off | d3: resample without the mismatch bar |
| `audio.bufferSize` | Audio | 256 | d5: 64, 128, 256, 512, 1024, 2048 or 4096. No 32 and no automatic switching |
| `audio.driverType` | Audio | system default | d5: ASIO is one choice, opened through JUCE's wrapper. No raw bypass is offered |
| `audio.linuxBackend` | Audio | PipeWire/JACK | d4: ALSA or PipeWire/JACK |
| `audio.jackTransportSync` | Audio | off | d4 |
| `plugins.clapFolders`, `plugins.lv2Folders` | Plug-ins | none | d4: custom folders on Linux |

The buffer default of 256 is a choice, not a ruling. ADR-0145 names the sizes
offered, not the default. 256 is the middle of ADR-0102's safe range; the
tracking sizes (64 and 128) are one click away.

## 2. The file (`store.hpp`)

```
<appdata::pathsFor(app).config>/settings.json
{"schema": 1, "app": "ADI DAW", "values": {"audio.bufferSize": 256, ...}}
```

- **One file per application.** ADI DAW, ADI Live and aDiJ each have their own
  folder (ADR-0149), and the file names its application. A file naming another
  application is refused and never overwritten (ADR-0145 d10).
- **Unknown keys survive.** A key a newer build wrote is kept and written back
  exactly. A newer schema number is never lowered.
- **A corrupt file is kept.** It moves aside to `settings.json.corrupt-N`, the
  application starts from defaults, and the next save writes a fresh file.
  Nothing is overwritten silently.
- **Saves are atomic:** a temporary file beside it, then a rename.
- **A stored value this build finds illegal** (a 32-sample buffer from an older
  build) reads as the default, and stays in the file until the setting changes.

## 3. The change log

`settings-changes.jsonl`, beside the file: one JSON line per change, with
`time`, `actor` (`user` or `agent`), `key`, `before`, `after`, and `detail` for
the agent's model. The time is passed in by the caller. **An agent's change is
always logged.** A person's is logged too, by default: ADR-0125 left that open
and recommended it, and ADR-0152 d3 decides it. There is no undo (ADR-0125 d3);
recovery is a bundle or the page's Defaults button (`resetPage`).

## 4. The agent's pipeline

`agentSet(store, tier, key, value, model, time)` is the only path by which the
agent changes an application setting (ADR-0125 d1):

- **Apply tier only.**
- **A whitelist:** `agentMayChange` in the registry, off for every setting
  unless someone turned it on for that setting. Today: theme, knob mode,
  tooltip delay, follow playhead, Info View, meter peak hold, Zoom on
  Selection, modifier keys, snap.
- **A structural "never" that no mark overrides** (`agentForbidden`): any path,
  anything on the Audio, Plug-ins, Privacy or AI pages, and anything not App
  scope. The test checks that no whitelisted setting is one of these.
- Every accepted change is logged (§3).

## 5. Presets (R-04)

A preset is a name, a set of pages, and the values those pages held.

- `makePreset(store, name, pages)` captures them. A preset holding every page
  is a whole preset; any other is partial.
- `applyPreset` changes the preset's pages only. A key from a page it does not
  hold is skipped and reported, even if the preset file carries it.
- Presets are JSON (`toJson`, `presetFromJson`).

## 6. Settings bundles (R-05, ADR-0127 d5)

```
bundle.zip
  manifest.json    {"kind": "adi-settings-bundle", "schema": 1, "app": "ADI DAW"}
  settings.json    App-scope settings the registry knows
  presets/N.json
```

- **Paths travel as roles.** The roles are the roots a user's material lives
  under: `user library`, and `content folder 1`, `content folder 2`, and so
  on. A path is written as `role:<name>/<rest>`, using the longest root that
  holds it. The record folder under the user library travels as
  `role:user library/Recordings`.
- **A path under no role is left out and reported.** Export then checks the
  whole bundle for any absolute path (POSIX, `C:\`, UNC) and refuses to write
  if one is left.
- **Import resolves each role against the importing machine's own folders.** A
  role that machine lacks is reported and nothing is invented.
- **A bundle from another application is refused.**
- **Unknown keys do not travel:** this build cannot tell whether they hold a
  path.
