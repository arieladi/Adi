'use strict';
/* =============================================================================
   ValhallaRoomController — predefined strategy for Valhalla DSP "ValhallaRoom"
   (VST3 reverb, v1.6.x).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES, the anchored name
   patterns, the page tables and the labels are carried across UNCHANGED from
   1.5.9.0 — those names came from Adi's real Ableton Configure screenshot, and
   they are data rather than ink.

   A reverb has no band structure, so the 6 dials are PAGED. Tap the MAIN / EARLY
   / LATE / RT tabs to switch what the dials control:
     MAIN  : Mix · Predelay · Decay · High Cut · Diffusion · Early/Late Mix
     EARLY : Early Size · Early Cross · Early Mod Rate · Early Mod Depth ·
             Early Send · (Mix)
     LATE  : Late Size · Late Cross · Late Mod Rate · Late Mod Depth ·
             (Decay) · (Mix)
     RT    : Bass Mult · Bass Xover · High Mult · High Xover · (Decay) · (Mix)
   Mix and Decay repeat on the deeper pages so the two you always want are never
   more than a turn away. **Pressing ANY dial advances the page** — there is no
   per-dial press action, in either layout.

   A bottom bar holds the two globals: left = Reverb Mode (the algorithm — tap to
   cycle, hold = previous), right = Preset (tap ◀ / ▶ to step, if the build
   exposes a preset parameter; ValhallaRoom usually does not, and the bar says so
   rather than pretending).

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). All continuous reverb params use delta_index — Valhalla's VST3
   normalisation already carries the right taper, so a linear move in normalised
   space feels right. Pin exact names/indexes in ValhallaRoomController.OVERRIDES.
   See docs/VALHALLA_ROOM.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, the four pages above, bar split MODE | PRESET.

   COMPACT — 4 dials (L15):
     ALL FOUR PAGES SURVIVE. Each uses the FIRST FOUR parameters of its own page;
     dials 5 and 6 are dropped. LATE and RT come through whole (RT is exactly
     four parameters); MAIN loses Diffusion and Early/Late Mix, EARLY loses Early
     Send. Mix and Decay stay reachable everywhere because they already repeat.

     The bar is the part that actually needed a decision: it is the only
     full-width element any controller has, and at 800 px it would be sliced
     through the middle, stranding PRESET in the borrowed half where it can never
     be drawn or touched. So in compact the **PRESET half is dropped entirely and
     MODE spans the full width** — which costs nothing real, since ValhallaRoom
     does not reliably expose a preset parameter, and buys MODE a target twice
     the size.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.ValhallaRoomController = function ValhallaRoomController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'main';
};
AVC.ValhallaRoomController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.ValhallaRoomController.prototype.id = 'valhalla-room';

AVC.ValhallaRoomController.PAGES_ORDER = ['main', 'early', 'late', 'rt'];
AVC.ValhallaRoomController.PAGE_LABEL = { main: 'MAIN', early: 'EARLY', late: 'LATE', rt: 'RT' };
AVC.ValhallaRoomController.PAGES = {
  main:  ['mix', 'predelay', 'decay', 'highcut', 'diffusion', 'earlylatemix'],
  early: ['earlysize', 'earlycross', 'earlymodrate', 'earlymoddepth', 'earlysend', 'mix'],
  late:  ['latesize', 'latecross', 'latemodrate', 'latemoddepth', 'decay', 'mix'],
  rt:    ['bassmult', 'bassxover', 'highmult', 'highxover', 'decay', 'mix'],
};
AVC.ValhallaRoomController.LABEL = {
  mix: 'MIX', predelay: 'PREDLY', decay: 'DECAY', highcut: 'HI CUT', diffusion: 'DIFF', earlylatemix: 'E/L MIX',
  earlysize: 'E SIZE', earlycross: 'E CROSS', earlymodrate: 'E MOD R', earlymoddepth: 'E MOD D', earlysend: 'E SEND',
  latesize: 'L SIZE', latecross: 'L CROSS', latemodrate: 'L MOD R', latemoddepth: 'L MOD D',
  bassmult: 'BAS MUL', bassxover: 'BAS XO', highmult: 'HI MUL', highxover: 'HI XO',
};

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.ValhallaRoomController.OVERRIDES = {};

AVC.ValhallaRoomController.ROLES = [
  { key: 'mix',          match: [/^mix$/, 'mix'] },
  { key: 'predelay',     match: [/^predelay$/, 'predelay', 'pre delay'] },
  { key: 'decay',        match: [/^decay$/, 'decay'] },
  { key: 'highcut',      match: [/^highcut$/, 'high cut', 'highcut'] },
  { key: 'diffusion',    match: [/^diffusion$/, 'diffusion'] },
  { key: 'earlylatemix', match: [/^earlylatemix$/, 'early late mix', 'earlylatemix', 'early/late'] },
  { key: 'earlysize',    match: [/^earlysize$/, 'early size', 'earlysize'] },
  { key: 'earlycross',   match: [/^earlycross$/, 'early cross', 'earlycross'] },
  { key: 'earlymodrate', match: [/^earlymodrate$/, 'early mod rate', 'earlymodrate'] },
  { key: 'earlymoddepth',match: [/^earlymoddepth$/, 'early mod depth', 'earlymoddepth'] },
  { key: 'earlysend',    match: [/^earlysend$/, 'early send', 'earlysend'] },
  { key: 'latesize',     match: [/^latesize$/, 'late size', 'latesize'] },
  { key: 'latecross',    match: [/^latecross$/, 'late cross', 'latecross'] },
  { key: 'latemodrate',  match: [/^latemodrate$/, 'late mod rate', 'latemodrate'] },
  { key: 'latemoddepth', match: [/^latemoddepth$/, 'late mod depth', 'latemoddepth'] },
  { key: 'bassmult',     match: [/^rtbassmultiply$/, 'bass mult', 'bassmult', 'rtbassmultiply'] },
  { key: 'bassxover',    match: [/^rtxover$/, 'bass xover', 'bassxover', 'rtxover'] },
  { key: 'highmult',     match: [/^rthighmultiply$/, 'high mult', 'highmult', 'rthighmultiply'] },
  { key: 'highxover',    match: [/^rthighxover$/, 'high xover', 'highxover', 'rthighxover'] },
  { key: 'reverbmode',   kind: 'cycle', match: [/^type$/, 'reverb mode', 'reverbmode', /^mode$/] },
  { key: 'preset',       kind: 'cycle', match: [/^preset$/, 'preset', 'program'] },
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
      this.sd.log('ValhallaRoom unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set ValhallaRoomController.OVERRIDES');
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
  proto._fmt = function (role) {
    if (!role) return '—';
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };
  proto._stepName = function (role) {
    if (!role) return '—';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role) - role.min)] || '');
    return this._fmt(role);
  };
  proto._pageRoleKey = function (slot) { return P.PAGES[this.page][slot]; };

  // ------------------------------------------------------------ layout mode
  /* The pages are unchanged between layouts — compact simply has fewer dials to
     hand them to, so each page yields its first N parameters (L15). There is no
     mode to fall back and no page that stops existing. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'ValhallaRoom — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      // Dividers stop above the bar, so the bar reads as one continuous element.
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, BOT[0] - 2, gfx.line, 1);
      this._buildZone(b, x, slot);
    }
    this._buildGlobalBar(b, W);
    return b;
  };

  proto._buildTabs = function (b, x, color) {
    var pages = P.PAGES_ORDER, tw = (SLOT - 8) / pages.length;
    for (var i = 0; i < pages.length; i++) {
      var act = pages[i] === this.page, tx = x + 4 + i * tw + 1;
      Svg.rrect(b, tx, TAB[0], tw - 2, TAB[1] - TAB[0], 3,
                act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)');
      Svg.text(b, P.PAGE_LABEL[pages[i]], tx + (tw - 2) / 2, TAB[1] - 3.5,
               act ? 7 : 6, act ? 800 : 600, act ? '#06251d' : gfx.dim, 'middle');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var tw = (SLOT - 8) / P.PAGES_ORDER.length, seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < P.PAGES_ORDER.length) ? P.PAGES_ORDER[seg] : null;
  };

  proto._buildZone = function (b, x, slot) {
    var color = gfx.bandColors[slot % 8];
    this._buildTabs(b, x, color);
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    Svg.text(b, key ? P.LABEL[key] : '—', x + SLOT / 2, MID[0] + 12, 9, 700, role ? color : gfx.dim, 'middle');
    Svg.mono(b, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, 18, 800, role ? gfx.text : gfx.dim, 'middle');
  };

  /* FULL: MODE on the left half, PRESET on the right.
     COMPACT (L15): PRESET is dropped and MODE takes the whole width. */
  proto._buildGlobalBar = function (b, W) {
    var mode = this._role('reverbmode'), h = BOT[1] - BOT[0];

    if (this._compact()) {
      Svg.rrect(b, 6, BOT[0], W - 12, h, 5, 'rgba(151,117,250,0.18)');
      Svg.text(b, 'MODE', 16, BOT[0] + 9, 8, 600, gfx.dim, 'start');
      Svg.text(b, mode ? this._stepName(mode) : '— (configure "type")', W / 2, BOT[1] - 5,
               13, 800, mode ? '#c9b8ff' : gfx.dim, 'middle');
      return;
    }

    var half = W / 2, preset = this._role('preset');
    // left = Reverb Mode
    Svg.rrect(b, 6, BOT[0], half - 12, h, 5, 'rgba(151,117,250,0.18)');
    Svg.text(b, 'MODE', 16, BOT[0] + 9, 8, 600, gfx.dim, 'start');
    Svg.text(b, mode ? this._stepName(mode) : '— (configure "type")', half / 2, BOT[1] - 5,
             13, 800, mode ? '#c9b8ff' : gfx.dim, 'middle');
    // right = Preset
    Svg.rrect(b, half + 6, BOT[0], half - 12, h, 5, 'rgba(77,171,247,0.16)');
    Svg.text(b, '◂', half + 16, BOT[1] - 6, 12, 700, preset ? gfx.accent : gfx.dim, 'middle');
    Svg.text(b, '▸', W - 16, BOT[1] - 6, 12, 700, preset ? gfx.accent : gfx.dim, 'middle');
    Svg.text(b, 'PRESET', half + 28, BOT[0] + 9, 8, 600, gfx.dim, 'start');
    Svg.text(b, preset ? this._stepName(preset) : '— (not exposed)', half + half / 2, BOT[1] - 5,
             13, 800, preset ? '#9fd0ff' : gfx.dim, 'middle');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    if (role) this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };

  // Any dial advances the page — the same in both layouts.
  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    var order = P.PAGES_ORDER, i = order.indexOf(this.page);
    this.page = order[(i + 1) % order.length];
  };

  proto.onTouch = function (gx, gy, hold) {
    var n = this._zones(), W = n * SLOT;
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= n) return;
    var lx = gx - slot * SLOT, ly = gy;
    var tab = this._tabHit(lx, ly);
    if (tab) { this.page = tab; return; }
    if (inY(ly, BOT)) {
      // Compact has no PRESET half — the whole bar is MODE.
      if (this._compact() || gx < W / 2) this._cycle('reverbmode', hold ? -1 : 1);
      else this._cycle('preset', hold ? -1 : 1);
    }
  };
  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    // reverbmode / preset are selectors (kind 'cycle') — always step, even if a
    // build reports them non-quantized; other params fall back to a fine nudge.
    if (r.quantized || r.kind === 'cycle') this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    return P.PAGE_LABEL[this.page] + ' ' + (key ? P.LABEL[key] : '—') + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.ValhallaRoomController);
