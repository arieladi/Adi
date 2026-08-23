'use strict';
/* =============================================================================
   ableton.js — the Ableton Live Hub.

   Ported from adi_ableton_vst_controller 1.5.9.0. The bridge protocol
   (ws://127.0.0.1:9006 to the AdiVST Python Remote Script) is UNCHANGED — the
   remote script is verified and is not being modified, so this speaks exactly
   the wire format in that plugin's docs/PROTOCOL.md.

   THE PORTING DECISION THAT SHAPED THIS FILE — AND HAS NOW BEEN UNWOUND

   All 13 legacy DeviceControllers drew with a Canvas 2D context while Studio OS
   paints SVG strings, so rather than rewrite 2,500 lines of verified layout code
   this file used to port the CANVAS instead: `SOS.SvgCtx` implemented the exact
   Canvas 2D subset they used and serialised it to SVG, and the controllers were
   copied in byte-for-byte. It was the right call — a verified parameter map
   cannot be broken by a port that never edits it.

   L4 then ported all fourteen controllers to native `build()` anyway, one at a
   time, which left the shim serving nobody. **V60 deleted it.** What follows is
   history, kept because "why is the compositor shaped like this?" is otherwise a
   real half hour for the next reader.

   The shim was also exactly what the strip compositor needs: draw ONE 1200x100
   image, then hand each dial a viewBox window into it, so an EQ curve spans all
   six dials as one continuous picture the way the legacy canvas-slicing did.

   D13 — the hub IS the flat surface. Entering it lands directly on the live
   device controller: the 6 dials follow Ableton's selected device and the
   predefined VST layouts resolve automatically. It declares fullScreenCapable
   because the controller is built around owning all six dials, and D15 then
   hands it the whole board on arrival.
   ============================================================================= */

window.SOS = window.SOS || {};
SOS.Modules = SOS.Modules || {};
window.AVC = window.AVC || {};      // the legacy controllers' namespace

/* ===========================================================================
   V60 — SECTION 1, `SOS.SvgCtx`, IS GONE. It was a Canvas-2D context that
   emitted SVG, and it existed so the legacy controllers could be copied in
   unmodified. L4 finished porting all fourteen to native `build()`, so it has
   had no consumer since — measured, not assumed: zero of the fourteen
   *Controller.js files referenced it, and its only remaining callers were the
   tests written for the shim itself. ~258 lines.

   1. AVC compatibility layer — verbatim from the legacy plugin so the copied
      controller files run unmodified.
   =========================================================================== */

AVC.LAYOUT = { slots: 6, slotW: 200, slotH: 100 };

AVC.gfx = {
  bg: '#0c0f12', panel: '#11161b', line: 'rgba(255,255,255,0.07)',
  text: '#c9d2dc', dim: '#6b7682', accent: '#6fe3c4',
  ok: '#4ad27a', warn: '#ffd166', bad: '#ff5d5d', eq: '#6fe3c4',
  bandColors: ['#ff6b6b', '#ffa94d', '#ffd43b', '#8ce99a', '#4dd4c8', '#4dabf7', '#9775fa', '#f783ac'],

  // V60 — `clear`, `roundRect` and `text2` took a Canvas ctx and went with the
  // shim; no controller ever called them. The COLOURS above are live (bg, text
  // and bandColors have 14, 26 and 15 controller references between them).
  clamp: function (v, a, b) { return v < a ? a : v > b ? b : v; },
};

AVC.STEP = 0.02;   // normalized parameter change per dial tick

// Show Ableton's own value string when it carries a unit/label/symbol — letters,
// %, or a ratio colon (e.g. "1:1" for Omnipressor's Function); else fall back to
// the controller's numeric format. Keeps the strip showing exactly what Ableton
// shows, never a reinvented number.
AVC.showVal = function (disp, fallback) {
  return (disp != null && /[a-zA-Z%:]/.test(String(disp))) ? String(disp) : fallback;
};

AVC.DeviceController = function DeviceController(services) {
  this.bridge = services.bridge;
  this.sd = services.sd;
  this.L = services.layout;     // { W, H, slots, slotW, slotH }
};
AVC.DeviceController.prototype = {
  id: 'base',
  onState: function (state) { this.state = state; },
  // V60 — `renderTouch` went with SvgCtx. Every controller implements `build()`.
  onDial: function (slot, ticks) {},
  onDialPress: function (slot) {},
  onTouch: function (x, y, hold) {},
  dialTitle: function (slot) { return ''; },
};

AVC.registry = {
  byClass: {}, byName: [], byHint: {},
  register: function (opts) {
    if (opts.classNames) opts.classNames.forEach(function (c) { AVC.registry.byClass[c] = opts.ctor; });
    if (opts.names) AVC.registry.byName.push({ patterns: opts.names, ctor: opts.ctor });
    if (opts.hint) AVC.registry.byHint[opts.hint] = opts.ctor;
  },
  // Resolve order: native class_name -> plugin name match -> bridge hint ->
  // Generic. VST3 plugins all report class_name "PluginDevice", so they must
  // match by name.
  resolve: function (state) {
    var d = state.device || {};
    if (AVC.registry.byClass[d.class_name]) return AVC.registry.byClass[d.class_name];
    var name = String(d.name || ''), lower = name.toLowerCase();
    for (var i = 0; i < AVC.registry.byName.length; i++) {
      var pats = AVC.registry.byName[i].patterns;
      for (var p = 0; p < pats.length; p++) {
        var pat = pats[p];
        var hit = (pat instanceof RegExp) ? pat.test(name) : lower.indexOf(String(pat).toLowerCase()) >= 0;
        if (hit) return AVC.registry.byName[i].ctor;
      }
    }
    return AVC.registry.byHint[d.controller] || AVC.GenericController;
  },
};

/* ===========================================================================
   3. The module
   =========================================================================== */

SOS.Modules.Ableton = (function () {
  var R = SOS.Render, S = SOS.Surface;

  var L = { slots: 6, slotW: 200, slotH: 100, W: 1200, H: 100 };
  var FPS = 15;

  /* ------------------------------------------------------------------ bridge
     Ported from the legacy bridge.js. The wire format is UNCHANGED. */
  var Bridge = (function () {
    var ws = null, url = 'ws://127.0.0.1:9006', connected = false, retry = null;
    var listeners = {};

    var state = {
      online: false,
      track: { name: '—', index: -1 },
      device: { name: '', class_name: '', controller: 'generic', has_device: false, index: -1, param_count: 0 },
      params: [],
      allParams: [],
      pv: {},
      eq8: { focus: 1, output: 0, output_disp: '', scale: 100, scale_disp: '', bands: [] },
      eq8_state: { count: 0, selected_is_eq8: false, selected_index: -1 },
      presets: [],
    };

    function on(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); }
    function emit(ev, data) {
      (listeners[ev] || []).forEach(function (fn) { try { fn(data); } catch (e) {} });
    }
    function setUrl(u) { if (u && u !== url) { url = u; reconnect(); } }

    function connect() {
      try { ws = new WebSocket(url); } catch (e) { scheduleRetry(); return; }
      ws.onopen = function () {
        connected = true; state.online = true;
        send({ c: 'subscribe' });
        emit('online', true);
      };
      ws.onclose = function () {
        connected = false; state.online = false;
        emit('online', false);
        scheduleRetry();
      };
      ws.onerror = function () { try { ws.close(); } catch (e) {} };
      ws.onmessage = function (e) {
        var m; try { m = JSON.parse(e.data); } catch (err) { return; }
        handle(m);
      };
    }
    function reconnect() { try { if (ws) ws.close(); } catch (e) {} connect(); }
    function scheduleRetry() {
      if (retry) return;
      retry = SOS.Timing.after(1500, function () { retry = null; connect(); });
    }
    function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

    function handle(m) {
      switch (m.t) {
        case 'hello': emit('hello', m); break;
        case 'track':
          state.track = { name: m.name, index: m.index, color: m.color };
          emit('track', state.track); emit('state', state); break;
        case 'device':
          state.device = { name: m.name, class_name: m.class_name, controller: m.controller,
                           has_device: m.has_device, index: m.index, param_count: m.param_count };
          state.allParams = []; state.pv = {};   // invalidate the named-param cache
          emit('device', state.device); emit('state', state); break;
        case 'all_params':
          /* V39 — SAY WHAT LIVE ACTUALLY EXPOSES. Every name-resolving controller
             (Pro-Q 3 and nine others) binds parameters BY NAME, so when a device
             shows `?` the only question that matters is "what names did Live
             send?" — and nothing anywhere answered it. One line per device
             change, truncated, is the difference between a guess and a fix. */
          try {
            var _ps = m.params || [];
            SOS.SD.log('ableton: "' + (state.device.name || '?') + '" exposes '
                     + _ps.length + ' params'
                     + (_ps.length ? ': ' + _ps.slice(0, 10).map(function (q) { return q.name; }).join(' | ')
                                     + (_ps.length > 10 ? ' …' : '') : ''));
          } catch (e) {}
          state.allParams = m.params || [];
          state.pv = {};
          for (var ap = 0; ap < state.allParams.length; ap++) {
            var P = state.allParams[ap];
            state.pv[P.i] = { value: P.value, disp: P.disp };
          }
          emit('all_params', state.allParams); emit('state', state); break;
        case 'p':
          state.pv[m.i] = { value: m.value, disp: m.disp };
          emit('p', m); emit('state', state); break;
        case 'params': state.params = m.params || []; emit('params', state.params); emit('state', state); break;
        case 'param':
          for (var i = 0; i < state.params.length; i++) {
            if (state.params[i].slot === m.slot) { state.params[i].value = m.value; state.params[i].disp = m.disp; }
          }
          emit('param', m); emit('state', state); break;
        case 'eq8':
          state.eq8 = { focus: m.focus, output: m.output, output_disp: m.output_disp || '',
                        scale: m.scale, scale_disp: m.scale_disp || '', bands: m.bands || [] };
          emit('eq8', state.eq8); emit('state', state); break;
        case 'eq8_band':
          var matched = false;
          for (var b = 0; b < state.eq8.bands.length; b++) {
            if (state.eq8.bands[b].i === m.i) { state.eq8.bands[b] = m; matched = true; }
          }
          if (!matched) state.eq8.bands.push(m);
          emit('eq8_band', m); emit('state', state); break;
        case 'eq8_globals':
          state.eq8.output = m.output; state.eq8.output_disp = m.output_disp || '';
          state.eq8.scale = m.scale; state.eq8.scale_disp = m.scale_disp || '';
          emit('eq8', state.eq8); emit('state', state); break;
        case 'eq8_state':
          state.eq8_state = { count: m.count, selected_is_eq8: m.selected_is_eq8, selected_index: m.selected_index };
          emit('eq8_state', state.eq8_state); break;
        case 'presets': state.presets = m.items || []; emit('presets', state.presets); break;
        case 'error': emit('error', m.message); break;
        /* V44 — the remote script has always answered `device_loaded`; nothing
           listened, so a load shortcut was fire-and-forget in both directions and
           a name Live's browser does not have produced a press with no visible
           consequence at all. The VST launcher shows the result, so the reply is
           finally emitted. */
        case 'device_loaded': emit('device_loaded', m); break;
        /* V48 — the selected track's volume and pan, for the idle dials. Pushed on
           track change and whenever either parameter moves, INCLUDING when it is
           moved with the mouse: the remote script watches both for the lifetime of
           the track, so the dial readout cannot go stale. */
        case 'mix':
          state.mix = m.has_track ? m : null;
          emit('mix', state.mix); emit('state', state); break;
        case 'device_focused': emit('device_focused', m); break;
        /* V53 — where the selection sits in the FLATTENED device tree, which is the
           only thing that can tell you a Pro-Q 3 is three deep inside a rack. Its
           own message, not a field on `device`: that one is verified protocol
           several controllers key off, and a tree walk on every device change would
           make it pay for something only two keys read. */
        case 'device_pos':
          state.devicePos = { index: m.index, count: m.count };
          emit('device_pos', state.devicePos); emit('state', state); break;
        /* V61 — Live's own transport state, so the keys can LIGHT rather than
           just fire. The remote script pushes it after every transport verb.
           Fields are read defensively: an older Live missing one of them must
           leave the key unlit, not undefined-shaped. */
        case 'transport':
          state.transport = { playing: !!m.playing, loop: !!m.loop };
          emit('transport', state.transport); emit('state', state); break;
        default: break;
      }
    }

    var cmd = {
      paramDelta: function (slot, delta) { send({ c: 'param_delta', slot: slot, delta: delta }); },
      paramSet: function (slot, norm) { send({ c: 'param_set', slot: slot, norm: norm }); },
      eq8FreqDelta: function (band, delta) { send({ c: 'eq8_freq_delta', band: band, delta: delta }); },
      eq8GainDelta: function (band, delta) { send({ c: 'eq8_gain_delta', band: band, delta: delta }); },
      eq8QDelta: function (band, delta) { send({ c: 'eq8_q_delta', band: band, delta: delta }); },
      eq8GlobalDelta: function (which, delta) { send({ c: 'eq8_global_delta', which: which, delta: delta }); },
      eq8ToggleBand: function (band) { send({ c: 'eq8_toggle_band', band: band }); },
      eq8CycleType: function (band, dir) { send({ c: 'eq8_cycle_type', band: band, dir: dir }); },
      eq8Page: function (dir) { send({ c: 'eq8_page', dir: dir }); },
      eq8Key: function () { send({ c: 'eq8_key' }); },
      listPresets: function () { send({ c: 'eq8_list_presets' }); },
      loadPreset: function (id) { send({ c: 'eq8_load_preset', id: id }); },
      newPreset: function (id) { send({ c: 'eq8_new_preset', id: id }); },
      /* V30 — additive: the remote script gained a `load_device` verb and
         nothing else on this protocol changed. An older script simply
         ignores an unknown `c`, so the key degrades to a no-op rather
         than breaking the bridge. */
      loadDevice: function (name) { send({ c: 'load_device', name: name }); },
      /* V48 — THE UNIFIED PLUGIN KEY. `device_key` decides on the LIVE side,
         because only Live can see what is already on the track: nothing there ->
         insert, one -> focus it, several -> focus the next on each press. The long
         press sets `new`, which always appends.

         Deciding in Live rather than here is not an implementation detail. The
         plugin's mirror of the track is a snapshot pushed on change; a key that
         chose between insert and focus from that snapshot would be racing it. */
      deviceKey: function (name) { send({ c: 'device_key', name: name }); },
      deviceKeyNew: function (name) { send({ c: 'device_key', name: name, new: true }); },
      // V48 — the idle-state dials. 0.5 dB per detent is enforced in Live.
      trackVolumeDelta: function (steps) { send({ c: 'track_volume_delta', steps: steps }); },
      trackPanDelta: function (steps) { send({ c: 'track_pan_delta', steps: steps }); },
      getMix: function () { send({ c: 'get_mix' }); },
      /* V61 — the transport. ONE additive verb carrying an action rather than
         three separate ones: the remote script's V30 exception is for purely
         additive verbs, and one addition is a smaller change than three. Live
         must be RESTARTED after the remote script is deployed, or these are
         fire-and-forget messages into a script that has never heard of them. */
      transport: function (action) { send({ c: 'transport', action: action }); },
      // V53 — step through the track's devices, racks included. Live owns the walk.
      deviceStep: function (dir) { send({ c: 'device_step', dir: dir }); },
      devicePos: function () { send({ c: 'device_pos' }); },
      selectTrack: function (dir) { send({ c: 'select_track', dir: dir }); },
      selectDevice: function (dir) { send({ c: 'select_device', dir: dir }); },
      getAllParams: function () { send({ c: 'get_all_params' }); },
      watch: function (indices) { send({ c: 'watch', indices: indices }); },
      setIndex: function (i, norm) { send({ c: 'set_index', i: i, norm: norm }); },
      deltaIndex: function (i, delta) { send({ c: 'delta_index', i: i, delta: delta }); },
      deltaLogIndex: function (i, delta) { send({ c: 'delta_log_index', i: i, delta: delta }); },
      stepIndex: function (i, dir, steps) { send({ c: 'step_index', i: i, dir: dir, steps: steps || 0 }); },
      toggleIndex: function (i) { send({ c: 'toggle_index', i: i }); },
    };

    return {
      connect: connect, setUrl: setUrl, on: on, state: function () { return state; },
      isOnline: function () { return connected; }, cmd: cmd,
    };
  })();

  AVC.Bridge = Bridge;   // some controllers reach for it directly

  /* --------------------------------------------------- the strip compositor
     Draws the active controller ONCE across the full 1200x100 strip, then gives
     each dial a viewBox window into that same drawing. The string is built
     once and re-wrapped six times, so a curve spanning the whole strip costs one
     render, not six — and lands on the dials as one continuous picture, which is
     what the legacy canvas-slicing achieved. */
  var zoneSvg = ['', '', '', '', '', ''];
  var lastZones = 6;

  function composite() {
    if (!active) return;
    // L3b: a docked window borrows dials 5-6, so the strip is only as wide as
    // the dials the module still has. The controller is told how many zones it
    // has and picks its own layout from that.
    var zones = SOS.States.moduleDials();
    lastZones = zones;

    /* V60 — there is only ONE path now. The Canvas-shim fallback that used to
       sit below this went with SvgCtx: a controller without `build()` cannot be
       drawn at all any more, which is the loud failure the shim's own header
       comment asked for. A missing build() logs and leaves the strip alone. */
    if (typeof active.build !== 'function') {
      SOS.SD.log('ableton: ' + (active.id || '?') + ' has no build() — nothing to draw');
      return;
    }
    var bag;
    try { bag = active.build(zones); }
    catch (e) { SOS.SD.log('ableton: build() failed in ' + (active.id || '?') + ' — ' + e.message); return; }
    for (var i = 0; i < zones; i++) zoneSvg[i] = SOS.Svg.serialize(bag, i * L.slotW, L.slotW, L.slotH);
    for (var j = zones; j < L.slots; j++) zoneSvg[j] = '';
  }

  /* ------------------------------------------------------ controller picking */
  var controllers = {};     // ctor key -> reused instance
  var active = null;

  function services() {
    return { bridge: Bridge, sd: { log: function (m) { SOS.SD.log('ableton: ' + m); } }, layout: L };
  }

  function pickController() {
    var st = Bridge.state();
    var Ctor = AVC.registry.resolve(st);
    if (!Ctor) return;
    var key = (Ctor.prototype && Ctor.prototype.id) || Ctor.name || 'C';
    if (!controllers[key]) controllers[key] = new Ctor(services());
    active = controllers[key];
    try { active.onState(st); } catch (e) { SOS.SD.log('ableton: onState failed — ' + e.message); }
    composite();
  }

  /* ------------------------------------------------------------ render pump
     The legacy plugin ran a 15fps loop. SD.image()/setFeedback dedupe unchanged
     frames, so a static device costs nothing on the wire; the pump idles slowly
     when the bridge is down so a disconnected Ableton is not re-rendered 15
     times a second forever. */
  /* V34 — THE PUMP RUNS ON SOS.Timing, and this is the fix for "the strip takes a
     minute to appear and never moves when I turn a dial". It was a
     self-rescheduling setTimeout on a hidden page, so the 66 ms cadence was being
     clamped to roughly one frame a MINUTE. Nothing about the payload was slow and
     nothing was blocking — the pump simply was not being allowed to run, which is
     also why the Pro-Q 3 screen never cleared when the focused device changed.

     Re-armed rather than run on a fixed interval, so a slow composite can never
     stack a backlog of frames behind it. */
  var pumping = false, pumpTimer = null;
  /* V44 — THE PUMP ONLY COMPOSITES FOR THE SCREEN THAT DRAWS THE STRIP.

     Navigating INTO a sub-page does not exit the hub (nav.enter pushes, it does
     not pop), so the pump keeps running underneath the VST launcher. That is
     wanted — the launcher's status zone needs repaints — but `composite()` builds
     the whole 1200x100 controller strip, and on a menu page nothing paints it: the
     zones belong to the menu screen. So it was 15 frames a second of an image
     thrown away.

     Compositing is now gated on the hub actually being the current screen, and the
     cadence drops to 4 fps off it, which is still instant for a status line. The
     alternative — pausing the pump from the menu screens' lifecycle — couples two
     modules and can desynchronise; asking one question here cannot. */
  function pump() {
    if (!pumping) return;
    var live = Bridge.isOnline();
    var onHub = SOS.Nav.current() === hub;
    if (live) {
      if (onHub) composite();
      SOS.States.repaint();
    }
    pumpTimer = SOS.Timing.after(
      live ? (onHub ? Math.max(30, 1000 / FPS) : 250) : 750, pump);
  }
  function startPump() { if (!pumping) { pumping = true; pump(); } }
  function stopPump() {
    pumping = false;
    if (pumpTimer != null) { SOS.Timing.cancel(pumpTimer); pumpTimer = null; }
  }

  /* ------------------------------------------------------------------- keys

     V46 — THE GREAT FLATTENING. Adi's ruling, and it replaces both the V44
     hierarchical launcher and the old three-key device shelf:

       "The Stream Deck XL has 32 keys, which is plenty of room. We want to flatten
        the menu and put the plugin shortcuts directly on the main Ableton hub,
        categorized by columns for fast muscle memory."

     So the hub IS the launcher. Four two-column bands from the plugins.js
     catalogue, each framed in the colour he boxed it with, and the rightmost
     column as a utility strip:

       cols 0-1  RED     EQ         cols 4-5  GREEN  Synths
       cols 2-3  YELLOW  Dynamics   cols 6-7  CYAN   Meters
       (0,0)     BACK — global, out of the Ableton hub
       col 8     MIDI (row 0) · device/load status (row 1) · NEXT (row 3)

     PRESETS IS GONE ENTIRELY — key, folder, mode flag and all. "I never requested
     it" (Adi). The `mode`/`setMode` machinery existed only to open that folder, so
     it went with it; `Bridge.cmd.listPresets/loadPreset/newPreset` are left alone
     because those are protocol against the verified remote script, not UI.

     V29 — THE BROWSER ARROWS ARE GONE, and stay gone. ◀TRK / TRK▶ / ◀DEV / DEV▶
     filled four keys with a generic transport for Live's own selection, which the
     mouse already does well. `selectTrack` / `selectDevice` remain on the Bridge. */

  var page = 0;
  function setPage(p) { page = p; SOS.States.repaint(); }

  function shortName(s) { s = String(s || ''); return s.length > 10 ? s.slice(0, 9) + '…' : s; }

  /* One builder for both breakpoints. `cols` decides how many bands fit beside the
     utility column — four at 9, two at 5 — and plugins.js owns that arithmetic, so
     this function never counts columns itself. */
  function hubKeys(cols) {
    return function (col, row) {
      var util = cols - 1;

      /* (0,0) IS THE GLOBAL BACK. Adi: "The absolute top-left key (0,0) MUST be
         the global BACK button to exit the Ableton Hub."

         It works on a SHORT press because the hub is fullScreenCapable: Button 1
         is the engine's reserved long-press Back anchor only OUTSIDE NAV OFF, and
         inside it the key belongs to the module. The frame under it still comes
         from the EQ band so the red box is not missing a corner. */
      if (col === 0 && row === 0) {
        var back = {
          label: 'Back', badge: '↑', size: 'md', color: R.PALETTE.nav,
          kind: 'tap',
          tap: function () { SOS.Nav.back(); },
        };
        // V55 — Back sits INSIDE the EQ block, so it carries that block's tile as
        // well as its tint. Without this the picture would have one blank corner.
        if (P()) back = Object.assign(back, P().artAt(0, 0, cols, page) || {});
        return back;
      }

      /* V49 — THE UTILITY COLUMN IS MIDI AND NEXT, AND NOTHING ELSE. The device
         readout that used to sit at (8,1) is gone: "I do not know what the Device
         screen you invented is, but it does nothing useful." It was a key that did
         nothing when pressed, and the one job it had left — reporting a load that
         missed — belongs on the key you actually pressed, which is where
         plugins.js puts it now. */
      if (col === util) {
        if (row === 0) return midiKey();
        if (row === 1) return deviceStepKey(-1);   // V53 — previous device
        if (row === 2) return deviceStepKey(1);    // V53 — next device
        if (row === 3) return nextKey(cols);
        return null;
      }

      // Everything else is the plugin block, frames and blanks included.
      return P() ? P().gridKey(col, row, cols, page) : null;
    };
  }

  // Looked up at PAINT time, so plugins.js and this file have no load-order
  // dependency and a build without the catalogue degrades to an empty block.
  function P() { return SOS.Modules.Plugins || null; }

  /* NEXT. Its meaning follows the breakpoint — at 9 columns every band is already
     on screen so it can only page items, and nothing currently overflows, so it
     goes DIM and says 1/1 rather than pretending to be a control. At 5 columns it
     cycles which pair of bands you are looking at. */
  /* V53 — STEP THROUGH THE TRACK'S DEVICES. Adi asked for these in the two empty
     cells between MIDI and NEXT.

     They walk the FLATTENED device tree on the Live side, so they descend into a
     rack and come back out of it rather than stepping over it — "including
     traversing into and out of nested devices/Racks". Live clamps at both ends
     instead of wrapping; stepping off the end of a chain should stop, the way an
     arrow key stops at the end of a list.

     The caption is the position in that tree, which is the only way to tell from
     the surface that you are three devices deep inside a drum rack. It comes from
     the `device_pos` message rather than from a count taken here, because only Live
     can see inside the racks. */
  function deviceStepKey(dir) {
    var pos = Bridge.state().devicePos;
    var on = Bridge.isOnline();
    var at = pos && pos.count ? (pos.index + 1) + '/' + pos.count : 'device';
    return {
      glyph: dir < 0 ? '▲' : '▼', label: dir < 0 ? 'Prev' : 'Next',
      sub: on ? at : 'offline', size: 'md',
      color: R.PALETTE.ableton, dim: !on, kind: 'tap',
      tap: function () { Bridge.cmd.deviceStep(dir); },
    };
  }

  /* NEXT. V49 — plugins.js now guarantees a spare EMPTY page, so this is never
     inert: page 2 is the same four tinted, framed bands with nothing in them, ready
     for the next plugins. At 5 columns the count also folds in which pair of bands
     is showing. */
  function nextKey(cols) {
    var pages = P() ? P().pageCount(cols) : 1;
    var cur = ((page % pages) + pages) % pages;
    return {
      label: 'NEXT', sub: (cur + 1) + '/' + pages, size: 'md',
      color: R.PALETTE.console, dim: pages <= 1, kind: 'tap',
      tap: function () { if (pages > 1) setPage(cur + 1); },
    };
  }

  /* V24 — MIDI Control lives HERE, not on the Root Hub: it is a studio
     instrument that belongs with the DAW rather than a top-level destination
     beside it. V46 pins it to the top-right, where Adi drew it. */
  function midiKey() {
    return {
      label: 'MIDI', glyph: '⌗', size: 'lg', color: R.PALETTE.midi,
      sub: 'controller', kind: 'tap',
      tap: function () { SOS.Nav.enter('midictl.hub'); },
    };
  }

  /* ==========================================================================
     V50 — THE IDLE STATE: TRACK MODE.

     Adi: "when I enter the Ableton hub but no VST is selected/focused, the touch
     screen and dials are completely empty. I want a default Track Mode."

       dials 1-4   the standard OS navigation strip, MIRRORED from the Root Hub
       dial  5     track PAN
       dial  6     track VOLUME, in strictly 0.5 dB steps

     Dials 1-4 are not reimplemented here — they come from `Root.osNavDial`, which
     was extracted for exactly this. Two hand-written copies of the same five dials
     is how "the standard OS navigation strip" quietly stops being standard.

     THE CLOCK LOSES ZONE 6 HERE, deliberately: `States.lastZoneFree` gives the last
     zone to the clock only when nothing else uses it, and now something does. That
     is what Adi asked for ("Replace the Apps and Clock with Ableton Track
     Controls").

     Dial 6's LONG press is the engine's NAV gesture and is untouchable, so neither
     of these two dials takes a press — turning is the whole interaction. Volume is
     the one thing on this strip you must not fire by accident.
     ========================================================================== */
  function deviceFocused() {
    var st = Bridge.state();
    return !!(st.device && st.device.has_device);
  }

  /* ==========================================================================
     V61 — STRIP FOCUS: WHO OWNS THE DIALS, DECOUPLED FROM WHERE YOU ARE.

     Adi: "If I press BACK to return to the Level 1 Ableton Hub, the VST folder
     key MUST remain highlighted to clearly indicate that the dials and touch
     screen are still actively controlling VSTs."

     That is not a tint — it is a decoupling. Until now the strip followed
     NAVIGATION: composite() painted only while the hub's own dials() was being
     asked, so going Back stopped the module owning the strip.

     Ownership is now ONE module-level variable that NAV NEVER TOUCHES:

       'none'  nothing on the strip (the Level 1 default — Adi's ruling)
       'vst'   the device/macro controller owns all six zones
       'mix'   Ableton track controls (Device mode)
       'os'    the Root Hub's OS navigation strip, on explicit request

     THE TINT FALLS OUT FOR FREE. Each mode key paints `active: focus === '...'`,
     and `active` is already a keySpec() field that render.js draws as a lit cap.
     No new render path, no new binding field, and NOTHING added to the three
     hand-written whitelists — which matters, because that trap has bitten twice.

     THIS IS A THIRD ORTHOGONAL STATE MACHINE, beside nav level and the carousel
     state, and this project has been hurt exactly there before (a hardcoded 4 in
     input.js; eight literal 3s in the tests at V59). So: nothing outside this
     file compares `focus` to a literal, the values live in FOCUS, and the tests
     assert the SHAPE — every mode key's focus value must be a member of FOCUS,
     and at most one mode key may be lit at a time.
     ========================================================================== */
  var FOCUS = { NONE: 'none', VST: 'vst', MIX: 'mix', OS: 'os' };
  var focus = FOCUS.NONE;

  function setFocus(f) {
    if (focus === f) return;
    focus = f;
    SOS.SD.log('ableton: strip focus -> ' + f);
    SOS.States.repaint();
  }

  var BLANK = { title: '', value: '' };

  /* V61 — OS mode, on explicit request only.

     Adi: "remove the standard OS Nav controls (Scroll, Zoom, Apps, Tabs) from the
     touch screen and dials whenever we are inside the Ableton Hub". So this is no
     longer the DEFAULT — `focus` starts at NONE and the strip is empty. The OS
     mode key is what brings it back, which is the only reading under which that
     key is not dead on arrival: the other four mode keys are strip-focus
     switches, so this one is too. INTERPRETATION, flagged in DECISIONS — one line
     to change if Adi wants the OS key to navigate to the Root Hub instead.

     Still MIRRORED from Root.osNavDial rather than copied. Two hand-written
     copies of the same five dials is how "the standard OS navigation strip"
     quietly stops being standard. */
  function osDial(dial) {
    var Root = SOS.Modules.Root;
    if (dial > 5) return BLANK;          // 6 stays free, so the clock can have it
    return Root && Root.osNavDial ? Root.osNavDial(dial) : BLANK;
  }

  /* V61 — DEVICE MODE: the Ableton mixer, as far as the remote script can go.

     Volume and Pan work TODAY: `track_volume_delta` and `track_pan_delta` are
     V50's additive verbs and they are live. They keep their exact physical
     positions — Pan on 5, Volume on 6, where they have always been — because
     moving a working control to tidy a layout is not an improvement.

     Dials 1-4 are DELIBERATELY EMPTY. Adi: "Leave those dial/touch slots empty
     for now so we can build dedicated Track/Mixer controls there later." Mute,
     Solo and Record Arm need three more additive remote-script verbs that do not
     exist yet — checked, the script has no mute/solo/arm verb of any kind — and
     those are a sibling-repo commit plus a Live restart.

     Dial 6's LONG press is the engine's NAV gesture and is untouchable, so
     neither of these two takes a press — turning is the whole interaction.
     Volume is the one thing on this strip you must not fire by accident. */
  function mixDial(dial) {
    var mix = Bridge.state().mix;
    var on = Bridge.isOnline();

    if (dial <= 4) return BLANK;         // reserved for Mute / Solo / Arm

    if (dial === 5) {
      return {
        title: 'Pan', value: mix ? (mix.pan_disp || 'C') : '—',
        sub: on ? 'track pan' : 'bridge offline',
        // Live's pan is -1..1; the bar wants 0..1.
        indicator: mix && typeof mix.pan === 'number' ? (mix.pan + 1) / 2 : undefined,
        color: R.PALETTE.ableton, dim: !on,
        rotate: function (t) { Bridge.cmd.trackPanDelta(t > 0 ? 1 : -1); },
      };
    }
    return {
      title: 'Volume', value: mix ? (mix.vol_disp || '—') : '—',
      sub: on ? '0.5 dB steps' : 'bridge offline',
      indicator: mix && typeof mix.vol === 'number' ? mix.vol : undefined,
      color: R.PALETTE.green, dim: !on,
      rotate: function (t) { Bridge.cmd.trackVolumeDelta(t > 0 ? 1 : -1); },
    };
  }

  /* The VST strip: the compositor's slices, one window per dial. */
  function vstDial(dial) {
    var slot = dial - 1;
    if (!deviceFocused()) {
      // Focus is on VSTs but Live has nothing selected. Say so on zone 1 rather
      // than painting six blank zones that look like a broken strip.
      return dial === 1
        ? { title: 'VST', value: '—',
            sub: Bridge.isOnline() ? 'no device focused' : 'bridge offline',
            color: R.PALETTE.ableton, dim: true }
        : BLANK;
    }
    return {
      // The strip image IS the dial's face: the compositor already sliced the
      // one 1200x100 drawing, so a curve spans all six as one picture.
      svg: zoneSvg[slot] || null,
      title: active ? (active.dialTitle(slot) || '') : '',
      value: '',
      rotate: function (t) { if (active) { active.onDial(slot, t); composite(); } },
      press: function () { if (active) { active.onDialPress(slot); composite(); } },
      touch: function (x, y, hold) {
        if (!active) return;
        // Touch arrives per-zone; map back into full-strip space before
        // hit-testing, which is what the controllers expect. y is zone-local
        // already (0-99) and passes straight through — L10.
        active.onTouch(slot * L.slotW + (x || 0), y || 0, !!hold);
        composite();
      },
    };
  }

  /* THE ONE DIAL ENTRY POINT for both screens. Level 1 and Level 2 share it, so
     the strip cannot disagree with itself depending on which screen asked — which
     is the whole point of state retention: BACK changes the keys, never the strip. */
  function focusDial(dial) {
    if (dial - 1 >= lastZones) return BLANK;      // borrowed by a docked window
    if (focus === FOCUS.VST) return vstDial(dial);
    if (focus === FOCUS.MIX) return mixDial(dial);
    if (focus === FOCUS.OS) return osDial(dial);
    return BLANK;                                 // FOCUS.NONE — Adi's default
  }

  /* ==========================================================================
     V61 — LEVEL 1 IS A CONTROL CENTRE, LEVEL 2 IS THE VST PAGE.

     Adi's ruling, and it corrected a misreading of mine that would have cost the
     band artwork:

       "The Mode Selectors are NOT a persistent global row that stays visible
        everywhere. They are simply folder/navigation keys that live exclusively
        on the Ableton Home Page (Level 1). DO NOT shrink the VST layout. DO NOT
        re-slice the images. When I press the VST folder on Level 1, it navigates
        to the VST Page (Level 2), which remains exactly as it is today (full 4
        rows, 8 cells per category)."

     So `ableton.vst` IS the old hub, byte-for-byte in behaviour: the same
     `hubKeys(cols)`, the same four two-column bands, the same 8 cells each, the
     same pagination and the same sliced artwork. `backgrounds.js` is untouched
     and `slice_backgrounds.py` did NOT need re-running.

     THE MODE ROW IS ROW 3, and that is the resolution of blocker B1 rather than a
     workaround for it. Adi's physical intent was "the keys above the touch
     screen"; row 3 is the row nearest the strip. Because the keys are Level-1
     only, putting them there costs NOTHING — Level 1 has no bands to displace.
     The reading that shrank the VST page was the wrong one.

         col 0     col 1    col 2     col 3    col 4    cols 5-8
     r0  BACK      PLAY     STOP      LOOP     ·        ·
     r1  ·         ·        ·         ·        ·        ·
     r2  ·         ·        ·         ·        ·        ·
     r3  VST       MIDI     Device    OS       Delay    ·

     Rows 1-2 and cols 5-8 are deliberately empty. That is the room Adi bought by
     moving the grid down a level, and filling it is his call, not mine.
     ========================================================================== */

  /* The five mode folders. A single table, because the difference between them is
     data: some navigate, some only change strip focus, one docks a window. */
  var MODES = [
    { col: 0, label: 'VST',   sub: 'macros',    focus: FOCUS.VST, screen: 'ableton.vst',
      color: R.PALETTE.green },
    { col: 1, label: 'MIDI',  sub: 'controller', glyph: '⌗',      screen: 'midictl.hub',
      color: R.PALETTE.midi },
    { col: 2, label: 'Device', sub: 'mixer',    focus: FOCUS.MIX,
      color: R.PALETTE.ableton },
    { col: 3, label: 'OS',    sub: 'nav',       focus: FOCUS.OS,
      color: R.PALETTE.nav },
    { col: 4, label: 'Delay', sub: 'calc',      dock: true,
      color: R.PALETTE.console },
  ];

  function modeKey(m) {
    return {
      label: m.label, sub: m.sub, glyph: m.glyph, size: 'md',
      color: m.color,
      /* THE GREEN VST KEY, and it costs nothing: `active` is already a keySpec()
         field and render.js already draws it as a lit cap, so the retained focus
         announces itself with no new render path. A mode that only navigates
         (MIDI) never lights, because it owns no strip. */
      active: !!m.focus && focus === m.focus,
      kind: 'tap',
      tap: function () {
        // Focus first, so a screen we navigate to already has the right strip
        // when its dials() is asked for the first time.
        if (m.focus) setFocus(m.focus);
        if (m.dock) SOS.States.setState(SOS.States.DELAY);
        if (m.screen) SOS.Nav.enter(m.screen);
      },
    };
  }

  /* Transport. DRAWN icons, not glyphs: the proven set has `▶` but no `■` and
     nothing that reliably reads as a loop, and one drawn shape beside two font
     glyphs gives three keys three different optical weights. See icons.js. */
  var TRANSPORT = [
    { col: 1, label: 'Play', icon: 'transportPlay', color: R.PALETTE.green, verb: 'play' },
    { col: 2, label: 'Stop', icon: 'transportStop', color: R.PALETTE.rekordbox, verb: 'stop' },
    { col: 3, label: 'Loop', icon: 'transportLoop', color: R.PALETTE.viz, verb: 'loop' },
  ];

  function transportKey(t) {
    var on = Bridge.isOnline();
    var tp = Bridge.state().transport;
    /* Play and Loop are STATES in Live and light accordingly; Stop is a momentary
       action and has nothing to light. `lit` stays undefined for it rather than
       false, so `active` is simply absent on that key. */
    var lit = t.verb === 'play' ? !!(tp && tp.playing)
            : t.verb === 'loop' ? !!(tp && tp.loop)
            : undefined;
    return {
      icon: t.icon, sub: t.label, size: 'md',
      color: t.color, dim: !on, active: lit,
      kind: 'tap',
      tap: function () { Bridge.cmd.transport(t.verb); },
    };
  }

  /* Level 1's keys. Region-local (col,row), so the same builder serves the 9-col
     and 5-col breakpoints — at 5 columns everything still fits, which is the
     whole advantage of a surface this empty. */
  function level1Keys(cols) {
    return function (col, row) {
      if (col === 0 && row === 0) {
        return {
          label: 'Back', badge: '↑', size: 'md', color: R.PALETTE.nav,
          kind: 'tap', tap: function () { SOS.Nav.back(); },
        };
      }
      if (row === 0) {
        for (var i = 0; i < TRANSPORT.length; i++) {
          if (TRANSPORT[i].col === col) return transportKey(TRANSPORT[i]);
        }
        return null;
      }
      if (row === 3) {
        for (var j = 0; j < MODES.length; j++) {
          // At 5 columns the mode row still holds all five (cols 0-4).
          if (MODES[j].col === col && col < cols) return modeKey(MODES[j]);
        }
        return null;
      }
      return null;
    };
  }

  /* Both screens share onEnter: the bridge, the plugin catalogue, the mix
     snapshot and the device position are needed by either. The PUMP is shared
     too — it paints whatever `focus` says, so it must run on Level 1 as well or
     a retained VST focus would freeze the moment you pressed Back. */
  function enter() {
    Bridge.connect();
    if (SOS.Modules.Plugins) SOS.Modules.Plugins.wire();
    Bridge.cmd.getMix();
    Bridge.cmd.devicePos();       // V53 — populate the step arrows' caption
    pickController();
    startPump();
  }

  var hub = {
    id: 'ableton.hub',
    title: 'Ableton',
    module: 'ableton',
    color: R.PALETTE.ableton,
    // Still fullScreenCapable: the mode row is on row 3 and a docked window would
    // take cols 5-8, so the board is wide enough either way — but D15 handing it
    // all 36 keys on arrival is what keeps Back where Adi expects it.
    fullScreenCapable: true,
    onEnter: enter,
    /* THE PUMP STOPS HERE AND NOWHERE ELSE, and the reason is nav.js's exact
       semantics: `enter()` pushes and calls onEnter on the NEW screen without
       touching the parent, and `pop()` calls onExit on the POPPED screen only.
       So this fires when Level 1 is popped — i.e. going UP to the Root Hub, which
       is the one moment the Ableton module really stops owning the surface.
       Putting stopPump on the VST screen instead would kill the strip on the way
       BACK to Level 1, which is precisely the retention Adi asked for. */
    onExit: function () { stopPump(); },
    layouts: [
      { cols: 9, keys: level1Keys(9) },
      { cols: 5, keys: level1Keys(5) },
    ],
    dials: focusDial,
  };

  /* LEVEL 2 — the VST page. This is the old `ableton.hub` unchanged: same
     hubKeys, same bands, same 8 cells per category, same artwork. */
  var vst = {
    id: 'ableton.vst',
    title: 'VST',
    module: 'ableton',
    color: R.PALETTE.ableton,
    fullScreenCapable: true,
    onEnter: function () {
      // Arriving here IS a VST-focus request, whether you came from the mode key
      // or from a Back out of the MIDI page.
      setFocus(FOCUS.VST);
      enter();
    },
    /* NO onExit. Backing out of here lands on Level 1, still inside the module,
       and the retained VST focus must survive that — see the note on hub.onExit. */
    layouts: [
      { cols: 9, keys: hubKeys(9) },
      { cols: 5, keys: hubKeys(5) },
    ],
    dials: focusDial,
  };

  /* V13 — STATE 3 IS GONE, and with it this module's context strip. ◀TRK / DEV▶
     live on the hub's own board, which is where they belong: the hub is
     fullScreenCapable, so arriving hands it all 36 keys anyway (D15).

     V14 — the CONSUMER of the 4-dial Compact layouts moved to State 2. Docking
     Time Divisions borrows physical dials 5-6, `moduleDials()` returns 4, and
     `composite()` calls `build(4)` — the same path State 3 used to open. */

  // Repaint whenever Live's state moves.
  Bridge.on('state', function () { pickController(); });
  Bridge.on('online', function (up) {
    SOS.SD.log('ableton bridge ' + (up ? 'online' : 'offline'));
    pickController();
    SOS.States.repaint();
  });
  Bridge.on('error', function (msg) { SOS.SD.log('ableton bridge error: ' + msg); });

  return {
    hub: hub, vst: vst, bridge: Bridge,
    _focus: function () { return focus; },
    _setFocus: setFocus, _FOCUS: FOCUS, _modes: MODES, _transport: TRANSPORT,
    // exposed for scripts/test_ableton.mjs
    _composite: composite, _zones: zoneSvg,
    _pick: pickController, _active: function () { return active; },
    _layout: L, _stop: stopPump,
    _page: function (p) { page = p | 0; },
  };
})();
