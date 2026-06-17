# Architecture

```
┌─────────────────────────── Ableton Live ───────────────────────────┐
│  AdiVST Remote Script (Live's CPython, main thread)                 │
│    AdiVST.py ──drains──> command queue ──> LiveBridge (LOM access)  │
│        ▲ outbound JSON                         │ listeners          │
│        └────────────── ws_server.py (daemon thread, select loop) ───┼─ ws://127.0.0.1:9006
└─────────────────────────────────────────────────────────────────────┘
                                   │  JSON (docs/PROTOCOL.md)
┌──────────────── Stream Deck plugin (embedded Chromium) ─────────────┐
│  bridge.js  (Ableton WS client + state store)                       │
│  sd-client.js (Elgato registration WS)                              │
│  plugin.js  orchestrator                                            │
│     ├─ AVC.registry.resolve(device) → active DeviceController       │
│     │      • GenericController   (6 non-quantized params → 6 dials)  │
│     │      • EQ8Controller       (split screen, band focus window)   │
│     ├─ touchscreen.js  render full canvas → slice → setFeedback ×6   │
│     └─ keys.js         36 keys, EQ8 launcher + preset folder         │
└─────────────────────────────────────────────────────────────────────┘
```

## Threading (Remote Script)

The WebSocket server runs on its own daemon thread and **never touches the Live
API**. Inbound commands are pushed to a `deque` and executed on Live's main
thread inside `AdiVST.update_display()` (~10 Hz). LOM listeners (selection /
parameter changes) fire on the main thread and queue outbound JSON, which the
socket thread flushes. This is the only thread-safe way to bridge sockets ↔ LOM.

## Strategy pattern

`AVC.DeviceController` is the base strategy. The orchestrator asks
`AVC.registry.resolve(state)` for the right subclass based on the selected
device's `class_name` (then a `controller` hint, then Generic). The active
controller owns rendering (`renderTouch`) and input (`onDial`, `onDialPress`,
`onTouch`) for both the touchscreen and the dials.

### Adding a predefined VST (e.g. Pulsar / Massive)

1. `js/controllers/PulsarController.js` extending `AVC.DeviceController`.
2. `<script>` it in `app.html` before `registry.js`.
3. `AVC.registry.register({ ctor: AVC.PulsarController, classNames: ['<LiveClassName>'] });`
4. (Optional) emit a richer model from the bridge by adding a branch in
   `LiveBridge._on_device_changed` keyed on the class name.

No change to the orchestrator, touchscreen slicing, or key logic is needed.

## Touchscreen slicing

The 6-dial touchscreen is addressed per-dial by the SDK. `touchscreen.js` draws
the active controller onto ONE virtual canvas (`slots × slotW`), then blits each
dial's sub-rect into that dial's `setFeedback` pixmap — so a "split exactly in
half" graph reads as one continuous image across dials. Touch coordinates
(reported per-slot) are mapped back into full-canvas space before hit-testing.
