'use strict';
/* =============================================================================
   BlackholeController — predefined strategy for Eventide "Blackhole" (H9 series,
   VST3/AU reverb).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES, the anchored name
   patterns and the bar definition are carried across UNCHANGED from 1.5.9.0 —
   those came from Adi's real Ableton Configure screenshot and are data, not ink.

   ONE LAYOUT, TWO WIDTHS (L17). Blackhole is the first controller whose Compact
   ruling changed the FULL layout as well, deliberately: the plugin is re-paged
   to THREE pages of exactly FOUR dials, and in the full layout dials 5 and 6 are
   left unmapped on purpose. The point is that Blackhole feels 100% identical
   docked or not, with zero hidden parameters — workflow consistency bought with
   two hardware dials.

     MAIN   : Mix · Gravity · Size · Predelay
     MOD    : Mod Depth · Mod Rate · Feedback · Resonance
     LEVELS : In Level · Out Level · Low EQ · Hi EQ

   Twelve parameters divide into three pages of four with no remainder, and
   nothing repeats across pages — which is exactly why the "keep two pages of
   six and drop the tail" approach used for the Valhallas was rejected here:
   Blackhole has no repeats to fall back on, so a dropped parameter is genuinely
   gone.

   Tap MAIN / MOD / LEVELS, or press ANY dial (including the two unmapped ones in
   full) to advance the page.

   A full-width bottom bar holds Blackhole's signature performance switches:
     KILL (mute) · FREEZE (hold the tail) · HOTSWITCH (morph) — tap to toggle —
     and TEMPO (TempoSync: Manual / Sync / Off) — tap to cycle.
   All four survive in both layouts, scaled to the current width: 300 px per cell
   at full, 200 px at compact. Every one of them is a live control, so unlike
   ValhallaRoom's unexposed PRESET there is nothing here worth reclaiming.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; switches toggle/step.
   Ribbon Controller and Tempo are left to the plugin GUI. Pin exact names/indexes
   in BlackholeController.OVERRIDES. See docs/BLACKHOLE.md.

   Zero keys in both layouts — all 36 belong to the Ableton hub shell.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.BlackholeController = function BlackholeController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'main';
};
AVC.BlackholeController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.BlackholeController.prototype.id = 'blackhole';

/* L17 — three pages of exactly four. */
AVC.BlackholeController.PAGES_ORDER = ['main', 'mod', 'levels'];
AVC.BlackholeController.PAGE_LABEL = { main: 'MAIN', mod: 'MOD', levels: 'LEVELS' };
AVC.BlackholeController.PAGES = {
  main:   ['mix', 'gravity', 'size', 'predelay'],
  mod:    ['moddepth', 'modrate', 'feedback', 'resonance'],
  levels: ['inlevel', 'outlevel', 'low', 'high'],
};
AVC.BlackholeController.PAGE_DIALS = 4;      // every page is exactly four wide
AVC.BlackholeController.LABEL = {
  mix: 'MIX', gravity: 'GRAVITY', size: 'SIZE', predelay: 'PREDLY', low: 'LOW EQ', high: 'HI EQ',
  moddepth: 'MOD D', modrate: 'MOD R', feedback: 'FDBK', resonance: 'RESO', inlevel: 'IN', outlevel: 'OUT',
};
// bottom bar: signature switches (left→right)
AVC.BlackholeController.BAR = [
  { key: 'kill',      label: 'KILL',  kind: 'toggle', color: '#ff8a8a' },
  { key: 'freeze',    label: 'FREEZE', kind: 'toggle', color: '#4dd4c8' },
  { key: 'hotswitch', label: 'HOTSW', kind: 'toggle', color: '#ffd166' },
  { key: 'temposync', label: 'TEMPO', kind: 'cycle',  color: '#9775fa' },
];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.BlackholeController.OVERRIDES = {};

AVC.BlackholeController.ROLES = [
  { key: 'mix',       match: [/^mix$/, 'mix'] },
  { key: 'gravity',   match: [/^gravity$/, 'gravity'] },
  { key: 'size',      match: [/^size$/, 'size'] },
  { key: 'predelay',  match: [/^predelay$/, 'predelay', 'pre delay'] },
  { key: 'low',       match: [/^low level$/, 'low level', 'low'] },
  { key: 'high',      match: [/^hi level$/, 'hi level', 'high level', 'high'] },
  { key: 'moddepth',  match: [/^mod depth$/, 'mod depth', 'moddepth'] },
  { key: 'modrate',   match: [/^mod rate$/, 'mod rate', 'modrate'] },
  { key: 'feedback',  match: [/^feedback$/, 'feedback'] },
  { key: 'resonance', match: [/^resonance$/, 'resonance'] },
  { key: 'inlevel',   match: [/^in level$/, 'in level', 'input level'] },
  { key: 'outlevel',  match: [/^out level$/, 'out level', 'output level'] },
  { key: 'kill',      kind: 'toggle', match: [/^kill$/, 'kill'] },
  { key: 'freeze',    kind: 'toggle', match: [/^freeze$/, 'freeze'] },
  { key: 'hotswitch', kind: 'toggle', match: [/^hotswitch$/, 'hot switch', 'hotswitch'] },
  { key: 'temposync', kind: 'cycle',  match: [/^temposync$/, 'tempo sync', 'temposync'] },
];

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var TAB = [2, 16], MID = [19, 60], BOT = [64, 97];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  // ------------------------------------------------- resolution (UNCHANGED)
  proto.onState = function (state) {
    this.state = state;
    var d = state.device || {};
    var sig = d.index + '|' + d.class_name + '|' + d.name;
    if (sig !== this._sig) {
      this._sig = sig; this._resolved = false; this._roles = {}; this._missing = [];
      if (d.has_device) this.bridge.cmd.getAllParams();
    }
    if (!this._resolved && state.allParams && state.allParams.length) this._resolve(state.allParams);
  };

  proto._resolve = function (params) {
    var roles = {}, missing = [], overrides = P.OVERRIDES || {};
    P.ROLES.forEach(function (role) {
      var found = null;
      if (overrides[role.key] != null) {
        var ov = overrides[role.key];
        found = (typeof ov === 'number') ? params[ov] : firstByName(params, norm(ov));
      }
      for (var pi = 0; !found && pi < role.match.length; pi++) {
        var pat = role.match[pi];
        for (var k = 0; k < params.length; k++) {
          var nm = norm(params[k].name);
          if (pat instanceof RegExp ? pat.test(nm) : nm.indexOf(pat) >= 0) { found = params[k]; break; }
        }
      }
      if (found) {
        roles[role.key] = { index: found.i, name: found.name, min: found.min, max: found.max,
          quantized: !!found.quantized, items: found.items || [], kind: role.kind || 'cont' };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('Blackhole unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set BlackholeController.OVERRIDES');
    }
  };
  function firstByName(params, n) { for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i]; return null; }

  // ---------------------------------------------------------- value access
  proto._role = function (key) { return this._roles[key] || null; };
  proto._value = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null) return pv[role.index].value;
    return role ? role.min : 0;
  };
  proto._disp = function (role) { var pv = this.state && this.state.pv; return (pv && role && pv[role.index]) ? pv[role.index].disp : null; };
  proto._on = function (role) { return !!role && this._value(role) > (role.min + role.max) / 2; };
  proto._fmt = function (role) {
    if (!role) return '—';
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };
  proto._stepName = function (role) {
    if (!role) return '—';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role) - role.min)] || '');
    return this._fmt(role);
  };
  // undefined for slots 4-5 in the full layout — those dials are unmapped by design.
  proto._pageRoleKey = function (slot) { return P.PAGES[this.page][slot]; };

  // ------------------------------------------------------------ layout mode
  /* Every page is four dials wide in BOTH layouts, so compact is not a reduced
     view of full — it is the same view on a shorter strip. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'Blackhole — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      // Dividers stop above the bar, so the bar reads as one continuous element.
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, BOT[0] - 2, gfx.line, 1);
      this._buildZone(b, x, slot);
    }
    this._buildBar(b, W);
    return b;
  };

  proto._buildTabs = function (b, x, color) {
    var pages = P.PAGES_ORDER, tw = (SLOT - 8) / pages.length;
    for (var i = 0; i < pages.length; i++) {
      var act = pages[i] === this.page, tx = x + 4 + i * tw + 1;
      Svg.rrect(b, tx, TAB[0], tw - 2, TAB[1] - TAB[0], 3,
                act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)');
      Svg.text(b, P.PAGE_LABEL[pages[i]], tx + (tw - 2) / 2, TAB[1] - 3.5,
               act ? 8 : 7, act ? 800 : 600, act ? '#06251d' : gfx.dim, 'middle');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var tw = (SLOT - 8) / P.PAGES_ORDER.length, seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < P.PAGES_ORDER.length) ? P.PAGES_ORDER[seg] : null;
  };

  proto._buildZone = function (b, x, slot) {
    var color = gfx.bandColors[slot % 8];
    // The tab row is drawn in EVERY zone, including the unmapped ones, so a page
    // can be tapped anywhere along the strip.
    this._buildTabs(b, x, color);

    var key = this._pageRoleKey(slot);
    if (!key) {
      // Unmapped by design (full layout, dials 5-6). Say what the dial still
      // does rather than painting a bare em-dash that reads as broken.
      Svg.text(b, 'press = page', x + SLOT / 2, MID[1] - 10, 9, 600, gfx.dim, 'middle', 0.55);
      return;
    }
    var role = this._role(key);
    Svg.text(b, P.LABEL[key], x + SLOT / 2, MID[0] + 12, 9, 700, role ? color : gfx.dim, 'middle');
    Svg.mono(b, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, 18, 800, role ? gfx.text : gfx.dim, 'middle');
  };

  /* All four cells in both layouts, tiled across the CURRENT width: 300 px each
     at full, 200 px at compact. */
  proto._buildBar = function (b, W) {
    var n = P.BAR.length, cw = W / n, h = BOT[1] - BOT[0];
    for (var i = 0; i < n; i++) {
      var cell = P.BAR[i], r = this._role(cell.key), x = i * cw;
      var isToggle = cell.kind === 'toggle';
      var on = isToggle && r ? this._on(r) : false;
      Svg.rrect(b, x + 5, BOT[0], cw - 10, h, 5, on ? cell.color : 'rgba(255,255,255,0.06)');
      Svg.text(b, cell.label, x + cw / 2, BOT[0] + 11, 8, 700, on ? '#06251d' : gfx.dim, 'middle');
      var state = r ? (isToggle ? (on ? 'ON' : 'OFF') : this._stepName(r)) : '—';
      Svg.text(b, state, x + cw / 2, BOT[1] - 5, 12, 800, on ? '#06251d' : gfx.text, 'middle');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    if (role) this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };

  // Any dial advances the page — including the two unmapped ones in full.
  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    var order = P.PAGES_ORDER, i = order.indexOf(this.page);
    this.page = order[(i + 1) % order.length];
  };

  proto.onTouch = function (gx, gy, hold) {
    var n = this._zones(), W = n * SLOT, ly = gy;
    if (gx < 0 || gx >= W) return;
    if (inY(ly, TAB)) {
      var slot = Math.floor(gx / SLOT);
      var tab = this._tabHit(gx - slot * SLOT, ly); if (tab) this.page = tab;
      return;
    }
    if (inY(ly, BOT)) {
      // Cells tile the CURRENT width, so the hit test has to as well.
      var cells = P.BAR.length, cw = W / cells, i = Math.floor(gx / cw);
      if (i < 0 || i >= cells) return;
      var cell = P.BAR[i], r = this._role(cell.key); if (!r) return;
      if (cell.kind === 'toggle') this.bridge.cmd.toggleIndex(r.index);
      else this._cycle(cell.key, hold ? -1 : 1);
    }
  };
  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.kind === 'cycle') this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';                 // borrowed by a window
    var key = this._pageRoleKey(slot);
    if (!key) return 'press = page';                      // unmapped, but not dead
    var role = this._role(key);
    return P.PAGE_LABEL[this.page] + ' ' + P.LABEL[key] + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.BlackholeController);
