'use strict';
/* =============================================================================
   SideMinderController — predefined strategy for RJ Studios "SideMinder ME2"
   (SideMinder Mastering Edition — dynamic stereo-width maximizer, VST3/AU).

   NATIVE SVG (L4) — and the LAST controller off the Canvas shim. The ROLE TABLE,
   OVERRIDES and the anchored match patterns (including the `(?! out)` guards
   that keep the width dials off the "<band>-Width Out" toggles) are carried
   across UNCHANGED from 1.5.9.0: verified data, not ink.

   A 3-band (Low / Mid / High) stereo-width processor with a lot of per-band
   params, so the 6 dials are PAGED (like Omnipressor / Blackhole). Tap the
   WIDTH / LIMIT / TRIM tabs — or press a dial — to switch what the dials drive:
     WIDTH : L-Width · M-Width · H-Width · LM Xover · MH Xover · I/O Trim
     LIMIT : L-Release · M-Release · H-Release · L-Ratio · M-Ratio · H-Ratio
     TRIM  : L-Offset · M-Offset · H-Offset · L-Trim · M-Trim · H-Trim
   (L/M/H = Low/Mid/High band. Widths are the Static Width Adjust %, 0–200%;
   Release is the Width-Limiter release slow↔fast; Offset is Side-Mid Offset.)
   The two crossovers are frequencies → log nudge (delta_log_index); everything
   else is a linear nudge (delta_index).

   A full-width bottom bar holds the globals:
     BANDS (#Bands 1/2/3 — cycles) · LINK (Control Link Indep/Relative/Ganged —
     cycles) · MONO (Output Mono) · DELTA (Output Delta) · EXT SC · BYPASS.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable): anchored regex on the Configure names (e.g. /^l width$/, which
   never grabs the "L-Width Out" toggle) + looser fallbacks + an OVERRIDES map.
   Values show Ableton's own str_for_value via AVC.showVal.

   Intentionally NOT mapped (left to the GUI / available via OVERRIDES): the
   per-band Width-Out / Limiter-Out / Band-Solo toggles, the Bass-Narrow/Bass-Mono
   controls, the correlation-meter source, Advanced, and Output/Input monitor.
   See docs/SIDEMINDER.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, the three pages above, all 18 parameters reachable, six bar
   cells at 200 px.

   COMPACT — 4 dials (L22): the three pages are kept and each yields its FIRST
   FOUR parameters. That deliberately ORPHANS L-Ratio (on LIMIT) and L-Trim (on
   TRIM) — their M and H siblings are on dials 5-6 and go. It is the first
   compact layout here to show part of a group on purpose, and it was chosen:
   the alternatives were dropping the whole Release triad, or a six-page layout
   whose tab row would be 32 px per tab.

   The FULL layout is untouched by that compromise, which is the point — this is
   the mirror image of Blackhole and Omnipressor, where Full gave up dials so the
   two layouts could match. Here Full is held perfect and compact takes the hit.

   The compact bar drops BYPASS and EXT SC, keeping BANDS · LINK · MONO · DELTA
   at exactly 200 px each. MONO and DELTA are the two you ride on a width tool;
   bypass is handled in Live and EXT SC is routing setup.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.SideMinderController = function SideMinderController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'width';
};
AVC.SideMinderController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.SideMinderController.prototype.id = 'sideminder';

AVC.SideMinderController.PAGES_ORDER = ['width', 'limit', 'trim'];
AVC.SideMinderController.PAGE_LABEL = { width: 'WIDTH', limit: 'LIMIT', trim: 'TRIM' };
AVC.SideMinderController.PAGES = {
  width: ['l_width', 'm_width', 'h_width', 'lmxover', 'mhxover', 'iotrim'],
  limit: ['l_rel', 'm_rel', 'h_rel', 'l_ratio', 'm_ratio', 'h_ratio'],
  trim:  ['l_offset', 'm_offset', 'h_offset', 'l_trim', 'm_trim', 'h_trim'],
};
AVC.SideMinderController.LABEL = {
  l_width: 'L WIDTH', m_width: 'M WIDTH', h_width: 'H WIDTH', lmxover: 'LM XO', mhxover: 'MH XO', iotrim: 'I/O TRIM',
  l_rel: 'L REL', m_rel: 'M REL', h_rel: 'H REL', l_ratio: 'L RATIO', m_ratio: 'M RATIO', h_ratio: 'H RATIO',
  l_offset: 'L OFFS', m_offset: 'M OFFS', h_offset: 'H OFFS', l_trim: 'L TRIM', m_trim: 'M TRIM', h_trim: 'H TRIM',
};
// dials that are frequencies → geometric (log) nudge
AVC.SideMinderController.LOG = { lmxover: 1, mhxover: 1 };
// bottom bar switches (left→right)
AVC.SideMinderController.BAR = [
  { key: 'bands',  label: 'BANDS',  kind: 'cycle',  color: '#4dd4c8' },
  { key: 'link',   label: 'LINK',   kind: 'cycle',  color: '#9775fa' },
  { key: 'mono',   label: 'MONO',   kind: 'toggle', color: '#4dabf7' },
  { key: 'delta',  label: 'DELTA',  kind: 'toggle', color: '#ffd166' },
  { key: 'extsc',  label: 'EXT SC', kind: 'toggle', color: '#8ce99a' },
  { key: 'bypass', label: 'BYPASS', kind: 'toggle', color: '#ff8a8a' },
];
/* L22 — compact drops BYPASS and EXT SC, leaving four cells at exactly 200 px. */
AVC.SideMinderController.BAR_COMPACT = ['bands', 'link', 'mono', 'delta'];

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.SideMinderController.OVERRIDES = {};

AVC.SideMinderController.ROLES = [
  // per-band Static Width % — every pattern excludes " out" so the "<band>-Width Out"
  // toggle is never grabbed (even on a word-named build like "Low-Width" + "Low-Width Out")
  { key: 'l_width', match: [/^l width$/, /^l width(?! out)/, /^low width(?! out)/] },
  { key: 'm_width', match: [/^m width$/, /^m width(?! out)/, /^mid width(?! out)/] },
  { key: 'h_width', match: [/^h width$/, /^h width(?! out)/, /^high width(?! out)/] },
  // Width-Limiter release (slow↔fast)
  { key: 'l_rel', match: [/^l release$/, 'low release'] },
  { key: 'm_rel', match: [/^m release$/, 'mid release'] },
  { key: 'h_rel', match: [/^h release$/, 'high release'] },
  // Width-Limiter ratio
  { key: 'l_ratio', match: [/^l ratio$/, 'low ratio'] },
  { key: 'm_ratio', match: [/^m ratio$/, 'mid ratio'] },
  { key: 'h_ratio', match: [/^h ratio$/, 'high ratio'] },
  // Side-Mid Offset (dB)
  { key: 'l_offset', match: [/^l offset$/, 'low offset'] },
  { key: 'm_offset', match: [/^m offset$/, 'mid offset'] },
  { key: 'h_offset', match: [/^h offset$/, 'high offset'] },
  // Level Trim (dB)
  { key: 'l_trim', match: [/^l trim$/, 'low trim'] },
  { key: 'm_trim', match: [/^m trim$/, 'mid trim'] },
  { key: 'h_trim', match: [/^h trim$/, 'high trim'] },
  // crossovers (Hz)
  { key: 'lmxover', match: [/^lmxovr$/, /^lm ?xover$/, 'mid low', 'low mid'] },
  { key: 'mhxover', match: [/^mhxovr$/, /^mh ?xover$/, 'high mid', 'mid high'] },
  { key: 'iotrim',  match: [/^io trim$/, 'i o trim'] },
  // globals (switch bar)
  { key: 'bands',  kind: 'cycle',  match: [/^bands$/, 'num bands', 'band count'] },
  { key: 'link',   kind: 'cycle',  match: [/^bandlink$/, 'control link', /^link$/] },
  { key: 'mono',   kind: 'toggle', match: [/^output mono$/, /^mono$/, 'out mono'] },
  { key: 'delta',  kind: 'toggle', match: [/^norm delta$/, /^delta$/, 'output delta'] },
  { key: 'extsc',  kind: 'toggle', match: [/^extsc$/, 'ext sc', 'external sidechain'] },
  { key: 'bypass', kind: 'toggle', match: [/^bypass$/] },
];

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var TAB = [2, 16], MID = [19, 60], BOT = [64, 97];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }
  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  // ------------------------------------------------------------- resolution
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
      this.sd.log('SideMinder unresolved roles: ' + missing.join(', ') +
        ' — Configure these in Ableton or set SideMinderController.OVERRIDES');
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
  // full state word for a switch (Ableton's own label, e.g. "3-Bands" / "Independent")
  proto._sw = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role) - role.min)] || '');
    return this._on(role) ? 'On' : 'Off';
  };
  proto._pageRoleKey = function (slot) { return P.PAGES[this.page][slot]; };

  // ------------------------------------------------------------ layout mode
  /* The pages are unchanged between layouts — compact simply has fewer dials to
     hand them to, so each page yields its first N parameters (L22). The BAR is
     the one thing that differs: four cells instead of six. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
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
      Svg.text(b, 'SideMinder ME2 — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
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
               7, act ? 800 : 600, act ? '#06251d' : gfx.dim, 'middle');
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
    Svg.mono(b, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, 17, 800, role ? gfx.text : gfx.dim, 'middle');
  };

  /* Cells tile the CURRENT width over the CURRENT cell list — six at 200 px in
     full, four at 200 px in compact. The pitch happens to match because compact
     drops exactly two cells with the two dropped dials; the code does not rely
     on that. */
  proto._buildBar = function (b, W) {
    var bar = this._bar(), n = bar.length, cw = W / n, h = BOT[1] - BOT[0];
    for (var i = 0; i < n; i++) {
      var cell = bar[i], r = this._role(cell.key), x = i * cw;
      var on = r ? this._on(r) : false;
      Svg.rrect(b, x + 5, BOT[0], cw - 10, h, 5, on ? cell.color : 'rgba(255,255,255,0.06)');
      Svg.text(b, cell.label, x + cw / 2, BOT[0] + 11, 8, 700, on ? '#06251d' : gfx.dim, 'middle');
      Svg.text(b, r ? this._sw(r) : '—', x + cw / 2, BOT[1] - 5, 11, 800, on ? '#06251d' : gfx.text, 'middle');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    if (!role) return;
    if (P.LOG[key]) this.bridge.cmd.deltaLogIndex(role.index, ticks * AVC.STEP);
    else this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };

  // Any dial advances the page — the same in both layouts.
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
    if (slot >= this._zones()) return '';
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    return P.PAGE_LABEL[this.page] + ' ' + (key ? P.LABEL[key] : '—') + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.SideMinderController);
