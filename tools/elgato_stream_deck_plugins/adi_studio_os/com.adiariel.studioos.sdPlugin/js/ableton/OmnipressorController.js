'use strict';
/* =============================================================================
   OmnipressorController — predefined strategy for Eventide "Omnipressor"
   (dynamics processor: expander / gate / compressor / limiter, VST3/AU).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES, the anchored name
   patterns and the switch definitions are carried across UNCHANGED from 1.5.9.0
   — those came from Adi's real Ableton Configure screenshot and are data, not
   ink.

   16 exposed params. RE-PAGED to three pages of exactly four (L20, the L17 move
   repeated), so compact and full show the same thing:
     MAIN   : Threshold · Attack · Release · Function
     LIMITS : Atten Limit · Gain Limit · Mix · Function
     I/O    : Input Gain · Output Gain · In Level · Out Level
   `Function` — the signature ratio knob (extreme expansion → gate → 1:1 →
   compression → ∞ limiting) — sits on TWO pages so it is always close. That
   repeat is what makes 11 unique knobs fill 3 × 4 exactly.

   In the FULL layout dials 5 and 6 are deliberately UNMAPPED, carrying the dim
   `press = page` hint: consistency of workflow between layouts was ruled more
   valuable than filling all six dials. Pressing ANY dial — including those two —
   advances the page.

   A full-width bottom bar holds the switches:
     BASS (Norm/Cut) · METER (Input/Gain/Output — cycles) · SC (Sidechain
     Enable) · LINE (In/Out) · POWER (On/Off). Tap to toggle; METER cycles.
   COMPACT DROPS **POWER and LINE** (L20): bypass is handled in Ableton and LINE
   is a routing setup switch, so compact re-tiles BASS · METER · SC to ~266 px
   each rather than shrinking five cells to 160 px.

   Note the deliberate asymmetry: the dial pages are identical across layouts,
   the bar is not. The pages are a workflow muscle memory depends on; the bar is
   a set of independent switches where dropping two costs only reach.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; switches toggle/step.
   Pin exact names/indexes in OmnipressorController.OVERRIDES. See docs/OMNIPRESSOR.md.

   Zero keys in both layouts — all 36 belong to the Ableton hub shell.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.OmnipressorController = function OmnipressorController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'main';
};
AVC.OmnipressorController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.OmnipressorController.prototype.id = 'omnipressor';

/* L20 — three pages of exactly four. */
AVC.OmnipressorController.PAGES_ORDER = ['main', 'limits', 'io'];
AVC.OmnipressorController.PAGE_LABEL = { main: 'MAIN', limits: 'LIMITS', io: 'I/O' };
AVC.OmnipressorController.PAGES = {
  main:   ['threshold', 'attack', 'release', 'function'],
  limits: ['attenlimit', 'gainlimit', 'mix', 'function'],
  io:     ['inputgain', 'outputgain', 'inlevel', 'outlevel'],
};
AVC.OmnipressorController.PAGE_DIALS = 4;      // every page is exactly four wide
AVC.OmnipressorController.LABEL = {
  threshold: 'THRESH', attack: 'ATTACK', release: 'RELEASE', function: 'FUNC', attenlimit: 'ATTEN', gainlimit: 'GAIN LIM',
  inputgain: 'IN GAIN', outputgain: 'OUT GAIN', inlevel: 'IN LVL', outlevel: 'OUT LVL', mix: 'MIX',
};
// bottom bar switches (left→right). BAR_COMPACT drops POWER and LINE (L20).
AVC.OmnipressorController.BAR = [
  { key: 'bass',      label: 'BASS',  kind: 'toggle', color: '#ffd166' },
  { key: 'meter',     label: 'METER', kind: 'cycle',  color: '#9775fa' },
  { key: 'sidechain', label: 'SC',    kind: 'toggle', color: '#4dd4c8' },
  { key: 'line',      label: 'LINE',  kind: 'toggle', color: '#4dabf7' },
  { key: 'power',     label: 'POWER', kind: 'toggle', color: '#ff8a8a' },
];
AVC.OmnipressorController.BAR_COMPACT = ['bass', 'meter', 'sidechain'];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.OmnipressorController.OVERRIDES = {};

AVC.OmnipressorController.ROLES = [
  { key: 'threshold',  match: [/^threshold$/, 'threshold'] },
  { key: 'attack',     match: [/^attack$/, 'attack'] },
  { key: 'release',    match: [/^release$/, 'release'] },
  { key: 'function',   match: [/^function$/, 'function', 'ratio'] },
  { key: 'attenlimit', match: [/^atten limit$/, 'atten limit', 'attenuation limit'] },
  { key: 'gainlimit',  match: [/^gain limit$/, 'gain limit'] },
  { key: 'inputgain',  match: [/^input gain$/, 'input gain'] },
  { key: 'outputgain', match: [/^output gain$/, 'output gain'] },
  { key: 'inlevel',    match: [/^in level$/, 'in level'] },
  { key: 'outlevel',   match: [/^out level$/, 'out level'] },
  { key: 'mix',        match: [/^mix$/, 'mix'] },
  { key: 'bass',      kind: 'toggle', match: [/^bass switch$/, 'bass switch', 'bass'] },
  { key: 'meter',     kind: 'cycle',  match: [/^meter select$/, 'meter select', 'meter'] },
  { key: 'sidechain', kind: 'toggle', match: [/^sidechain enable$/, 'sidechain enable', 'sidechain'] },
  { key: 'line',      kind: 'toggle', match: [/^line$/, 'line'] },
  { key: 'power',     kind: 'toggle', match: [/^power$/, 'power'] },
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
      this.sd.log('Omnipressor unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton or set OmnipressorController.OVERRIDES');
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
  // short switch state (last token of value string, e.g. "Norm"/"Cut"/"Gain")
  proto._sw = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length) { var s = String(role.items[Math.round(this._value(role) - role.min)] || ''); return s.split(' ').pop() || s; }
    return this._on(role) ? 'ON' : 'OFF';
  };
  // undefined for slots 4-5 in the full layout — those dials are unmapped by design.
  proto._pageRoleKey = function (slot) { return P.PAGES[this.page][slot]; };

  // ------------------------------------------------------------ layout mode
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  /* The bar is the ONE thing that differs between layouts: compact carries three
     cells rather than five (L20). The pages do not differ at all. */
  proto._bar = function () {
    if (!this._compact()) return P.BAR;
    return P.BAR.filter(function (c) { return P.BAR_COMPACT.indexOf(c.key) >= 0; });
  };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'Omnipressor — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
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
               act ? 7 : 7, act ? 800 : 600, act ? '#06251d' : gfx.dim, 'middle');
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
      // Unmapped by design (full layout, dials 5-6) — L17's hint, reused.
      Svg.text(b, 'press = page', x + SLOT / 2, MID[1] - 10, 9, 600, gfx.dim, 'middle', 0.55);
      return;
    }
    var role = this._role(key);
    Svg.text(b, P.LABEL[key], x + SLOT / 2, MID[0] + 12, 9, 700, role ? color : gfx.dim, 'middle');
    Svg.mono(b, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, 17, 800, role ? gfx.text : gfx.dim, 'middle');
  };

  /* Cells tile the CURRENT width over the CURRENT cell list: five at 240 px in
     full, three at ~266 px in compact. */
  proto._buildBar = function (b, W) {
    var bar = this._bar(), n = bar.length, cw = W / n, h = BOT[1] - BOT[0];
    for (var i = 0; i < n; i++) {
      var cell = bar[i], r = this._role(cell.key), x = i * cw;
      var on = r ? this._on(r) : false;
      Svg.rrect(b, x + 5, BOT[0], cw - 10, h, 5, on ? cell.color : 'rgba(255,255,255,0.06)');
      Svg.text(b, cell.label, x + cw / 2, BOT[0] + 11, 8, 700, on ? '#06251d' : gfx.dim, 'middle');
      Svg.text(b, r ? this._sw(r) : '—', x + cw / 2, BOT[1] - 5, 12, 800, on ? '#06251d' : gfx.text, 'middle');
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
      // Hit-test the SAME cell list the bar drew, tiled the same way.
      var bar = this._bar(), cells = bar.length, cw = W / cells, i = Math.floor(gx / cw);
      if (i < 0 || i >= cells) return;
      var cell = bar[i], r = this._role(cell.key); if (!r) return;
      if (cell.kind === 'cycle' || (r.quantized && r.items.length > 2)) this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, 0);
      else this.bridge.cmd.toggleIndex(r.index);
    }
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';                 // borrowed by a window
    var key = this._pageRoleKey(slot);
    if (!key) return 'press = page';                      // unmapped, but not dead
    var role = this._role(key);
    return P.PAGE_LABEL[this.page] + ' ' + P.LABEL[key] + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.OmnipressorController);
