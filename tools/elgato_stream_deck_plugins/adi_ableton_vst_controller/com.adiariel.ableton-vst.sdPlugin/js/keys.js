'use strict';
/* =============================================================================
   keys.js — manages the 36 keys.

   Roles (set per key in the Property Inspector):
     • "eq8"          the context-dependent EQ8 launcher (conditions A/B/C).
                      Long-press opens the EQ8 preset "folder" on the other keys.
     • "preset"       a preset slot (uses its `slot` index). Short = load onto the
                      current EQ8; long = drop a NEW EQ8 with that preset.
     • "track_prev/next", "device_prev/next"  navigation helpers.

   Modes: "normal" and "presets". Long-pressing the EQ8 key toggles into the
   preset folder; pressing the EQ8 key again (now "BACK") exits.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.Keys = (function () {
  var sd = AVC.SD, bridge = AVC.Bridge;
  var keys = {};            // context -> { role, slot }
  var downAt = {};          // context -> timestamp (long-press timing)
  var mode = 'normal';
  var LONG_MS = 500;
  var KS = 144;             // key render size
  var canvas = null, ctx = null;

  function init() { canvas = document.createElement('canvas'); canvas.width = KS; canvas.height = KS; ctx = canvas.getContext('2d'); }

  function register(context, settings) { keys[context] = normalize(settings); paint(context); }
  function unregister(context) { delete keys[context]; delete downAt[context]; }
  function updateSettings(context, settings) { keys[context] = normalize(settings); paint(context); }
  function normalize(s) { s = s || {}; return { role: s.role || 'eq8', slot: (s.slot == null ? 0 : (+s.slot | 0)) }; }

  // -------------------------------------------------------------- input
  function keyDown(context) { downAt[context] = now(); }
  function keyUp(context) {
    var t = downAt[context]; delete downAt[context];
    var long = (t != null) && (now() - t >= LONG_MS);
    var k = keys[context]; if (!k) return;
    if (k.role === 'eq8') return onEq8Key(long);
    if (k.role === 'preset') return onPresetKey(k.slot, long, context);
    if (k.role === 'track_prev') return bridge.cmd.selectTrack(-1);
    if (k.role === 'track_next') return bridge.cmd.selectTrack(1);
    if (k.role === 'device_prev') return bridge.cmd.selectDevice(-1);
    if (k.role === 'device_next') return bridge.cmd.selectDevice(1);
  }

  function onEq8Key(long) {
    if (mode === 'presets') { setMode('normal'); return; }     // EQ8 key acts as BACK
    if (long) { setMode('presets'); bridge.cmd.listPresets(); return; }
    bridge.cmd.eq8Key();                                       // short: A/B/C logic
  }
  function onPresetKey(slot, long, context) {
    if (mode !== 'presets') return;                            // inert until folder open
    var presets = bridge.state().presets || [];
    var p = presets[slot];
    if (!p) { sd.showAlert(context); return; }
    if (long) bridge.cmd.newPreset(p.id); else bridge.cmd.loadPreset(p.id);
    sd.showOk(context);
  }

  function setMode(m) { if (mode === m) return; mode = m; repaint(); }

  // -------------------------------------------------------------- painting
  function repaint() { for (var c in keys) if (keys.hasOwnProperty(c)) paint(c); }

  function paint(context) {
    var k = keys[context]; if (!k) return;
    var st = bridge.state();
    if (k.role === 'eq8') {
      if (mode === 'presets') return sd.setImage(context, renderKey({ title: 'BACK', sub: 'EQ8 presets', glyph: '✕', color: '#ffd166', active: true }));
      var e = st.eq8_state || { count: 0, selected_is_eq8: false };
      return sd.setImage(context, renderKey({
        title: 'EQ8', sub: e.count ? (e.count + ' on track') : 'create', glyph: 'EQ',
        color: '#6fe3c4', active: !!e.selected_is_eq8, badge: e.count ? ('×' + e.count) : '+',
      }));
    }
    if (k.role === 'preset') {
      if (mode !== 'presets') return sd.setImage(context, renderKey({ title: '', sub: '', color: '#2a3138', dim: true }));
      var presets = st.presets || [];
      var p = presets[k.slot];
      return sd.setImage(context, renderKey(p
        ? { title: shortName(p.name), sub: 'load / +new', color: '#9775fa', active: true }
        : { title: '—', sub: 'empty', color: '#2a3138', dim: true }));
    }
    var labels = { track_prev: ['◀ TRK', 'prev track'], track_next: ['TRK ▶', 'next track'],
                   device_prev: ['◀ DEV', 'prev device'], device_next: ['DEV ▶', 'next device'] };
    var lab = labels[k.role] || ['', ''];
    sd.setImage(context, renderKey({ title: lab[0], sub: lab[1], color: '#4dabf7' }));
  }

  function renderKey(o) {
    var g = AVC.gfx, c = ctx;
    c.clearRect(0, 0, KS, KS);
    c.fillStyle = g.bg; c.fillRect(0, 0, KS, KS);
    // panel
    g.roundRect(c, 8, 8, KS - 16, KS - 16, 14);
    c.fillStyle = o.dim ? 'rgba(255,255,255,0.03)' : 'rgba(255,255,255,0.05)'; c.fill();
    if (o.active) { c.lineWidth = 3; c.strokeStyle = o.color || g.accent; g.roundRect(c, 8, 8, KS - 16, KS - 16, 14); c.stroke(); }
    // glyph
    if (o.glyph) g.text2(c, o.glyph, KS / 2, KS / 2 + 4, '800 34px Inter, sans-serif', o.color || g.text, 'center');
    // title
    if (o.title) g.text2(c, o.title, KS / 2, o.glyph ? KS - 34 : KS / 2 + 2, '800 19px Inter, sans-serif', o.dim ? g.dim : g.text, 'center');
    // sub
    if (o.sub) g.text2(c, o.sub, KS / 2, KS - 16, '600 11px Inter, sans-serif', g.dim, 'center');
    // badge
    if (o.badge) {
      c.beginPath(); c.arc(KS - 26, 30, 13, 0, 6.2832); c.fillStyle = o.color || g.accent; c.fill();
      g.text2(c, o.badge, KS - 26, 34, '800 12px Inter, sans-serif', '#06251d', 'center');
    }
    return canvas.toDataURL('image/png');
  }

  function shortName(s) { s = String(s || ''); return s.length > 10 ? s.slice(0, 9) + '…' : s; }
  function now() { return (window.performance && performance.now) ? performance.now() : Date.now(); }

  // repaint when relevant state arrives
  function wire() {
    bridge.on('eq8_state', repaint);
    bridge.on('presets', repaint);
    bridge.on('online', repaint);
  }

  return { init: init, wire: wire, register: register, unregister: unregister, updateSettings: updateSettings,
           keyDown: keyDown, keyUp: keyUp, repaint: repaint, mode: function () { return mode; } };
})();
