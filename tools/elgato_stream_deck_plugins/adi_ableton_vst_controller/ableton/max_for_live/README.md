# Max for Live bridge (alternative to the Remote Script)

The primary bridge is the Python Remote Script (`../remote_script/AdiVST`). If you
prefer Max for Live, you can serve the **same** JSON protocol (`docs/PROTOCOL.md`)
so the Stream Deck plugin needs no changes.

Sketch:

1. Create an **Audio Effect** Max for Live device (`.amxd`).
2. Add a **`node.script`** object running a `ws` WebSocket server on port 9006
   (Node for Max bundles npm; `npm install ws` in the device's project).
3. Bridge Node ↔ Live API through Max:
   - From Node, send messages to Max outlets; in the Max patch use
     `live.path` / `live.object` / `live.observer` to read/write the LOM
     (`live_set view selected_track`, `… view selected_device`, device
     `parameters N value`, etc.) and `live.observer` to push changes back.
   - Translate to/from the JSON in `docs/PROTOCOL.md` inside the Node script.
4. The EQ8 key logic (conditions A/B/C, create via browser, presets) maps to
   `live.object` calls on `this_device`/`live_set tracks N` and the browser API,
   mirroring `live_bridge.py`.

This folder is a placeholder for that device; contributions welcome. The Remote
Script remains the reference implementation.
