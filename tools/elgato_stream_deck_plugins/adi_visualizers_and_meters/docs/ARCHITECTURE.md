# Architecture

A short map of how the plugin fits together, for contributors.

## Two consumers, one engine

`com.adi.visualizers-and-meters.sdPlugin/js/engine.js` is the single source of
truth for all signal processing and drawing. It attaches one global, `window.AVM`,
and is loaded as a plain `<script>` (no bundler) by:

- **the plugin host** — `app.html` (run by Stream Deck in its embedded Chromium),
  wired to the device by `js/plugin.js`;
- **the browser demo** — `demo/index.html`, wired to a DOM dashboard by
  `demo/demo.js`.

### `AVM` exports

| Export | Role |
|--------|------|
| `AudioEngine` | One shared stereo capture: `getUserMedia` → AudioWorklet (`meter-processor`) + AnalyserNodes → fills ring buffers + `METER`. |
| `Renderer` | Per-instance analysis scratch + ballistics. `draw(view, ctx, w, h, cfg, dt, resized)` plus per-view methods. |
| `DEFAULTS` | Default config per view; cloned per action instance. |
| `VIEWS` | Canonical list of view ids. |
| `FFT` | Iterative radix-2 Cooley–Tukey (precomputed bit-reversal + twiddles). |
| `SR` / `METER` | Live sample rate and meter values (getters). |
| `clamp`, `lin2db`, `hexA`, `makeWindow`, `fmtHz` | Helpers. |

Shared state (the ring buffers, `METER`, `SR`, the AnalyserNodes) lives once in
the module. Everything that must differ between simultaneous views — FFT scratch,
the spectrum's smoothed column, peak-hold timers — lives on each `Renderer`, so
four dials with four views never clobber one another.

## Plugin bridge (`js/plugin.js`)

1. Stream Deck loads `app.html` and calls the global
   `connectElgatoStreamDeckSocket(port, uuid, registerEvent, info)`.
2. The bridge opens the registration WebSocket and registers.
3. On the first `willAppear` it starts the shared `AudioEngine`.
4. Each instance gets a `Renderer` + an offscreen canvas sized for its controller
   (144×144 key, 200×100 encoder slot).
5. A throttled loop (default 15 fps) draws each instance's view, exports a PNG via
   `toDataURL`, and sends `setImage` (Keypad) or `setFeedback` (Encoder).
6. Input events: `keyDown` / `dialDown` / `touchTap` cycle the view; `dialRotate`
   adjusts the view's main parameter; long-touch resets. Changes persist with
   `setSettings`.

## Property Inspector (`pi/`)

Loads `engine.js` only to reuse `DEFAULTS`/`VIEWS` (never starts audio). It writes
a complete settings object back with `setSettings`; the plugin re-normalizes it on
`didReceiveSettings`. The shared refresh-rate / input-device live in global
settings and are exchanged over `sendToPlugin` / `sendToPropertyInspector`.

## Stream Deck events handled

`willAppear`, `willDisappear`, `didReceiveSettings`, `didReceiveGlobalSettings`,
`keyDown`, `dialRotate`, `dialDown`/`dialPress`, `touchTap`,
`propertyInspectorDidAppear`/`DidDisappear`, `sendToPlugin`.

Commands sent: `setImage`, `setFeedback`, `setSettings`, `setGlobalSettings`,
`getGlobalSettings`, `showAlert`, `logMessage`, `sendToPropertyInspector`.
