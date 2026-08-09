'use strict';
/* =============================================================================
   SpectreController — predefined strategy for Wavesfactory Spectre (VST3).

   Fixed 5-band enhancer/EQ. The bands are NAMED (not numbered) and their shapes
   are fixed: LowShelf · Peak 01 · Peak 02 · Peak 03 · HighShelf. Each band exposes
   Frequency, Gain, Q, Switch (on/off), Color (saturation) and Processing (stereo
   placement). Real Ableton Configure names, anchored:
     "<Band> Frequency", "<Band> Gain", "<Band> Q", "<Band> Switch",
     "<Band> Color", "<Band> Processing"
   plus globals "Output", "Dry Wet" (Mix) and "Mode".

   Layout — 5 band zones + a globals zone, with a strip-wide dial MODE (like the
   EQ Eight / Pro-Q 3 / Pulsar controllers), tap the GAIN / FREQ / Q tabs:
     dials 1-5 = the focused mode's param for the 5 bands
     dial 6    = Output             (press dial 6 = cycle Mode)
   Per band: dial press = Switch (on/off); tap bottom-left = cycle Color,
   bottom-right = cycle Processing. Zone 6: tap top = Mode, bottom = Mix step.

   VST3 indexes aren't version-stable, so each role resolves by NAME from the
   bridge's all_params; pin exact names/indexes in SpectreController.OVERRIDES.
   See docs/SPECTRE.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.SpectreController = function SpectreController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.mode = 'gain';          // strip-wide band dial mode: gain | freq | q
};
AVC.SpectreController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.SpectreController.prototype.id = 'spectre';

AVC.SpectreController.BAND_NAMES = ['LowShelf', 'Peak 01', 'Peak 02', 'Peak 03', 'HighShelf'];
AVC.SpectreController.LABELS = ['Lo Shelf', 'Peak 1', 'Peak 2', 'Peak 3', 'Hi Shelf'];
AVC.SpectreController.SHAPES = ['lowshelf', 'bell', 'bell', 'bell', 'highshelf'];
AVC.SpectreController.MODES = ['gain', 'freq', 'q'];
AVC.SpectreController.MODE_LABEL = { gain: 'GAIN', freq: 'FREQ', q: 'Q' };

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.SpectreController.OVERRIDES = {};

AVC.SpectreController.ROLES = (function () {
  function n(s) { return s.toLowerCase().replace(/[^a-z0-9 ]+/g, ' ').replace(/\s+/g, ' ').trim(); }
  var roles = [];
  AVC.SpectreController.BAND_NAMES.forEach(function (bn, i) {
    var b = i + 1, m = n(bn);
    roles.push({ key: 'b' + b + '_freq',  match: [new RegExp('^' + m + ' frequency$'), m + ' frequency'] });
    roles.push({ key: 'b' + b + '_gain',  match: [new RegExp('^' + m + ' gain$'), m + ' gain'] });
    roles.push({ key: 'b' + b + '_q',     match: [new RegExp('^' + m + ' q$'), m + ' q'] });
    roles.push({ key: 'b' + b + '_switch', match: [new RegExp('^' + m + ' switch$'), m + ' switch', m + ' on'] });
    roles.push({ key: 'b' + b + '_color', match: [new RegExp('^' + m + ' color$'), m + ' color'] });
    roles.push({ key: 'b' + b + '_proc',  match: [new RegExp('^' + m + ' processing$'), m + ' processing'] });
  });
  roles.push({ key: 'output', match: [/^output$/, 'output gain'] });
  roles.push({ key: 'mix',    match: [/^dry wet$/, 'dry wet', 'mix'] });
  roles.push({ key: 'mode',   match: [/^mode$/, 'mode'] });
  return roles;
})();

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TAB = [2, 17], MID = [20, 60], BOT = [63, 96];          // band zone rows
  var GTOP = [3, 28], GMID = [33, 62], GBOT = [66, 96];        // globals zone (6)

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
          quantized: !!found.quantized, items: found.items || [] };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('Spectre unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set SpectreController.OVERRIDES');
    }
  };
  function firstByName(params, n) { for (var i = 0; i < params.length; i++) if (norm(params[i].name) === n) return params[i]; return null; }

  // ---------------------------------------------------------- value access
  proto._role = function (key) { return this._roles[key] || null; };
  proto._bandRole = function (b, suffix) { return this._roles['b' + b + '_' + suffix] || null; };
  proto._value = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null) return pv[role.index].value;
    return role ? role.min : 0;
  };
  proto._disp = function (role) { var pv = this.state && this.state.pv; return (pv && role && pv[role.index]) ? pv[role.index].disp : null; };
  proto._on = function (role) { return !!role && this._value(role) > (role.min + role.max) / 2; };
  proto._stepName = function (role) {
    if (!role) return '—';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return AVC.showVal(this._disp(role), (Math.round(this._value(role) * 100) / 100) + '');
  };
  proto._fmtGain = function (role) {
    if (!role) return '—';
    var v = this._value(role), fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10);
    return AVC.showVal(this._disp(role), fb);
  };
  proto._bandText = function (b, mode) {
    var r = this._bandRole(b, mode); if (!r) return '—';
    if (mode === 'gain') return this._fmtGain(r);
    if (mode === 'freq') { var v = this._value(r); return AVC.showVal(this._disp(r), v >= 1000 ? (Math.round(v / 10) / 100) + 'k' : Math.round(v) + ''); }
    return AVC.showVal(this._disp(r), (Math.round(this._value(r) * 1000) / 1000) + '');   // q
  };

  // small fixed shape glyph
  proto._shapeGlyph = function (ctx, x, y, w, h, kind, color) {
    var midY = y + h / 2;
    ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.beginPath();
    for (var px = 0; px <= w; px += 3) {
      var t = px / w, yy = midY;
      if (kind === 'lowshelf') yy = midY - (1 - t) * (h * 0.34) + (h * 0.17);
      else if (kind === 'highshelf') yy = midY - t * (h * 0.34) + (h * 0.17);
      else yy = midY - Math.exp(-Math.pow((t - 0.5) / 0.16, 2)) * (h * 0.36);   // bell
      if (px === 0) ctx.moveTo(x, yy); else ctx.lineTo(x + px, yy);
    }
    ctx.stroke();
  };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'Spectre — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, L.H - 4); ctx.stroke(); }
      if (slot < 5) this._drawBand(ctx, x, slot);
      else this._drawGlobals(ctx, x);
    }
  };

  proto._drawTabs = function (ctx, x, color) {
    var modes = P.MODES, tw = (SLOT - 8) / modes.length;
    for (var i = 0; i < modes.length; i++) {
      var act = modes[i] === this.mode;
      gfx.roundRect(ctx, x + 4 + i * tw + 1, TAB[0], tw - 2, TAB[1] - TAB[0], 3);
      ctx.fillStyle = act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.MODE_LABEL[modes[i]], x + 4 + i * tw + tw / 2, TAB[1] - 4,
        act ? '800 8px Inter, sans-serif' : '600 7px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
    }
  };
  proto._tabHit = function (lx, ly) {
    if (!inY(ly, TAB)) return null;
    var tw = (SLOT - 8) / P.MODES.length, seg = Math.floor((lx - 4) / tw);
    return (seg >= 0 && seg < P.MODES.length) ? P.MODES[seg] : null;
  };
  proto._pill = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 4);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 3.5, '700 9px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };
  proto._btn = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 5);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 4, '700 10px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1, color = gfx.bandColors[slot % 8];
    var sw = this._bandRole(b, 'switch'), col = this._bandRole(b, 'color'), pr = this._bandRole(b, 'proc');
    var on = sw ? this._on(sw) : true;
    this._drawTabs(ctx, x, color);
    // MID — shape glyph + band name + active-mode value
    ctx.globalAlpha = on ? 1 : 0.4;
    this._shapeGlyph(ctx, x + 10, MID[0] + 2, 22, 12, P.SHAPES[slot], color);
    gfx.text2(ctx, P.LABELS[slot], x + SLOT / 2 + 8, MID[0] + 11, '700 9px Inter, sans-serif', on ? color : gfx.dim, 'center');
    gfx.text2(ctx, this._bandText(b, this.mode), x + SLOT / 2, MID[1] - 3, '800 17px "SF Mono", monospace', gfx.text, 'center');
    ctx.globalAlpha = 1;
    // BOT — Color | Processing (cycle)
    var hw = (SLOT - 14) / 2;
    this._pill(ctx, x + 5, BOT[0], hw, BOT[1] - BOT[0], col ? this._stepName(col) : 'COLOR?', false, '#9775fa');
    this._pill(ctx, x + 9 + hw, BOT[0], hw, BOT[1] - BOT[0], pr ? this._stepName(pr) : 'PROC?', false, '#4dabf7');
  };

  proto._drawGlobals = function (ctx, x) {
    var output = this._role('output'), mix = this._role('mix'), mode = this._role('mode');
    this._btn(ctx, x + 4, GTOP[0], SLOT - 8, GTOP[1] - GTOP[0], mode ? ('MODE ' + this._stepName(mode)) : 'MODE?', false, '#4dabf7');
    gfx.text2(ctx, 'Output', x + SLOT / 2, GMID[0] + 8, '600 10px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, output ? this._fmtGain(output) : '—', x + SLOT / 2, GMID[1], '700 17px "SF Mono", monospace', gfx.accent, 'center');
    // MIX step row
    gfx.text2(ctx, '◂', x + 12, GBOT[1] - 2, '700 13px Inter, sans-serif', gfx.accent, 'center');
    gfx.text2(ctx, '▸', x + SLOT - 12, GBOT[1] - 2, '700 13px Inter, sans-serif', gfx.accent, 'center');
    gfx.text2(ctx, 'MIX', x + SLOT / 2, GBOT[0] + 8, '600 8px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, mix ? this._stepName(mix) : '—', x + SLOT / 2, GBOT[1] - 1, '700 12px "SF Mono", monospace', gfx.text, 'center');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot < 5) {
      var r = this._bandRole(slot + 1, this.mode); if (!r) return;
      if (this.mode === 'gain') this.bridge.cmd.deltaIndex(r.index, ticks * AVC.STEP);
      else this.bridge.cmd.deltaLogIndex(r.index, ticks * AVC.STEP);            // freq + Q (log)
      return;
    }
    var o = this._role('output'); if (o) this.bridge.cmd.deltaIndex(o.index, ticks * AVC.STEP);
  };
  proto.onDialPress = function (slot) {
    if (slot < 5) { var sw = this._bandRole(slot + 1, 'switch'); if (sw) this.bridge.cmd.toggleIndex(sw.index); }
    else this._cycle('mode', 1);
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var lx = gx - slot * SLOT, ly = gy;
    if (slot < 5) {
      var tab = this._tabHit(lx, ly);
      if (tab) { this.mode = tab; return; }
      if (inY(ly, BOT)) {
        var b = slot + 1;
        if (lx < SLOT / 2) this._cycle('b' + b + '_color', hold ? -1 : 1);
        else this._cycle('b' + b + '_proc', hold ? -1 : 1);
      }
      return;
    }
    if (inY(ly, GTOP)) { this._cycle('mode', hold ? -1 : 1); return; }
    if (inY(ly, GBOT)) { this._step('mix', lx < SLOT / 2 ? -1 : 1); return; }
  };

  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };
  proto._step = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 1.5);
  };

  proto.dialTitle = function (slot) {
    if (slot < 5) return P.LABELS[slot] + ' ' + P.MODE_LABEL[this.mode] + ' ' + this._bandText(slot + 1, this.mode);
    var o = this._role('output'); return 'Output' + (o ? ' ' + this._fmtGain(o) : '');
  };
})(AVC.SpectreController);
