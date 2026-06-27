'use strict';
/* =============================================================================
   SideMinderController — predefined strategy for RJ Studios "SideMinder ME2"
   (SideMinder Mastering Edition — dynamic stereo-width maximizer, VST3/AU).

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
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TAB = [2, 16], MID = [19, 60], BOT = [64, 97];

  function norm(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  function inY(y, sec) { return y >= sec[0] && y <= sec[1]; }

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

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'SideMinder ME2 — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, BOT[0] - 2); ctx.stroke(); }
      this._drawZone(ctx, x, slot);
    }
    this._drawBar(ctx);
  };

  proto._drawTabs = function (ctx, x, color) {
    var pages = P.PAGES_ORDER, tw = (SLOT - 8) / pages.length;
    for (var i = 0; i < pages.length; i++) {
      var act = pages[i] === this.page;
      gfx.roundRect(ctx, x + 4 + i * tw + 1, TAB[0], tw - 2, TAB[1] - TAB[0], 3);
      ctx.fillStyle = act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.PAGE_LABEL[pages[i]], x + 4 + i * tw + tw / 2, TAB[1] - 3.5,
        act ? '800 7px Inter, sans-serif' : '600 7px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var tw = (SLOT - 8) / P.PAGES_ORDER.length, seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < P.PAGES_ORDER.length) ? P.PAGES_ORDER[seg] : null;
  };

  proto._drawZone = function (ctx, x, slot) {
    var color = gfx.bandColors[slot % 8];
    this._drawTabs(ctx, x, color);
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    gfx.text2(ctx, key ? P.LABEL[key] : '—', x + SLOT / 2, MID[0] + 12, '700 9px Inter, sans-serif', role ? color : gfx.dim, 'center');
    gfx.text2(ctx, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, '800 17px "SF Mono", monospace', role ? gfx.text : gfx.dim, 'center');
  };

  proto._drawBar = function (ctx) {
    var L = this.L, n = P.BAR.length, cw = L.W / n;
    for (var i = 0; i < n; i++) {
      var cell = P.BAR[i], r = this._role(cell.key), x = i * cw;
      var on = r ? this._on(r) : false;
      gfx.roundRect(ctx, x + 5, BOT[0], cw - 10, BOT[1] - BOT[0], 5);
      ctx.fillStyle = on ? cell.color : 'rgba(255,255,255,0.06)'; ctx.fill();
      gfx.text2(ctx, cell.label, x + cw / 2, BOT[0] + 11, '700 8px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
      gfx.text2(ctx, r ? this._sw(r) : '—', x + cw / 2, BOT[1] - 5, '800 11px Inter, sans-serif', on ? '#06251d' : gfx.text, 'center');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    if (!role) return;
    if (P.LOG[key]) this.bridge.cmd.deltaLogIndex(role.index, ticks * AVC.STEP);
    else this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };
  proto.onDialPress = function () {
    var order = P.PAGES_ORDER, i = order.indexOf(this.page);
    this.page = order[(i + 1) % order.length];
  };
  proto.onTouch = function (gx, gy, hold) {
    var L = this.L, ly = gy;
    if (inY(ly, TAB)) {
      var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
      var tab = this._tabHit(gx - slot * SLOT, ly); if (tab) this.page = tab;
      return;
    }
    if (inY(ly, BOT)) {
      var n = P.BAR.length, cw = L.W / n, i = Math.floor(gx / cw); if (i < 0 || i >= n) return;
      var cell = P.BAR[i], r = this._role(cell.key); if (!r) return;
      if (cell.kind === 'cycle' || (r.quantized && r.items.length > 2)) this.bridge.cmd.stepIndex(r.index, hold ? -1 : 1, 0);
      else this.bridge.cmd.toggleIndex(r.index);
    }
  };

  proto.dialTitle = function (slot) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    return P.PAGE_LABEL[this.page] + ' ' + (key ? P.LABEL[key] : '—') + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.SideMinderController);
