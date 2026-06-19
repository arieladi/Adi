'use strict';
/* =============================================================================
   SpectreController — predefined strategy for Wavesfactory Spectre (VST3).

   Fixed 5-band layout. Dials 1-5 drive bands 1-5; dial 6 is a DYNAMIC Q control.

   Dial logic:
     • Dials 1-5: each column has its own dialMode cycling FREQ ↔ GAIN (like the
       Pro-Q 3 controller). Turning band dial N also sets this._activeBand = N.
     • Dial 6 (dynamic Q): controls the Q of this._activeBand. Because turning any
       band dial updates activeBand, dial 6 instantly "follows" the last-touched
       band — Target: Band N in zone 6 reflects it live.

   Touchscreen, zones 1-5 (one band each):
     TOP    shape icon/curve — tap cycles Spectre's shapes.
     MIDDLE Freq / Gain / Q stacked; active dialMode highlighted; Q highlighted
            when this is the active band. Tap cycles the dial mode (Freq↔Gain).
     BOTTOM one global setting anchored per column:
            1 Quality · 2 Color · 3 Presets · 4 Mode · 5 Processing. Tap = cycle
            (hold/right = previous).
   Zone 6 (dynamic Q):
     TOP "Target: Band N" (live) · MIDDLE active band's Q value · BOTTOM Bypass.

   Parameter resolution is by NAME from the bridge's all_params (VST3 indexes are
   not version-stable). Override exact names/indexes in SpectreController.OVERRIDES;
   unresolved roles are logged. See docs/SPECTRE.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.SpectreController = function SpectreController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this._dialMode = [0, 0, 0, 0, 0];   // per band column: 0 FREQ, 1 GAIN
  this._activeBand = 1;               // band that dial 6 (Q) targets
};
AVC.SpectreController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.SpectreController.prototype.id = 'spectre';

AVC.SpectreController.BANDS = 5;
AVC.SpectreController.MODES = ['FREQ', 'GAIN'];
// global setting anchored at the bottom of each band column (index = column)
AVC.SpectreController.GLOBALS = ['quality', 'color', 'presets', 'mode', 'processing'];
AVC.SpectreController.GLOBAL_LABELS = { quality: 'QUALITY', color: 'COLOR', presets: 'PRESET', mode: 'MODE', processing: 'PROC' };

/* roleKey -> exact Live parameter NAME or numeric index, e.g.
   AVC.SpectreController.OVERRIDES = { b1_freq: 'Band 1 Frequency', quality: 'Quality' } */
AVC.SpectreController.OVERRIDES = {};

AVC.SpectreController.ROLES = (function () {
  var roles = [];
  for (var b = 1; b <= 5; b++) {
    roles.push({ key: 'b' + b + '_freq', kind: 'log', match: ['band ' + b + ' frequency', 'band ' + b + ' freq', 'frequency ' + b, 'freq ' + b] });
    roles.push({ key: 'b' + b + '_gain', kind: 'lin', match: ['band ' + b + ' gain', 'band ' + b + ' amount', 'gain ' + b] });
    roles.push({ key: 'b' + b + '_q', kind: 'log', match: ['band ' + b + ' q', 'band ' + b + ' bandwidth', 'band ' + b + ' resonance', 'q ' + b] });
    roles.push({ key: 'b' + b + '_shape', kind: 'cycle', match: ['band ' + b + ' shape', 'band ' + b + ' type', 'band ' + b + ' mode', 'shape ' + b] });
  }
  roles.push({ key: 'quality', kind: 'cycle', match: ['quality', 'oversampling', 'os'] });
  roles.push({ key: 'color', kind: 'cycle', match: ['color', 'colour', 'character', 'flavor'] });
  roles.push({ key: 'presets', kind: 'cycle', match: ['preset', 'program', 'curve'] });
  roles.push({ key: 'mode', kind: 'cycle', match: ['mode', 'algorithm', 'style'] });
  roles.push({ key: 'processing', kind: 'cycle', match: ['processing', 'channel mode', 'stereo mode', 'routing', 'channels'] });
  roles.push({ key: 'bypass', kind: 'toggle', match: ['bypass', 'enabled', 'active', 'on off', 'power'] });
  return roles;
})();

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TOP = [3, 27], MID = [29, 74], BOT = [77, 98];
  var L1 = 40, L2 = 55, L3 = 70;   // the three stacked value lines in MID

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
          quantized: !!found.quantized, items: found.items || [], kind: role.kind };
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
  proto._disp = function (role) {
    var pv = this.state && this.state.pv;
    if (pv && role && pv[role.index] != null && pv[role.index].disp != null) return String(pv[role.index].disp);
    if (role && role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return role ? (Math.round(this._value(role) * 100) / 100) + '' : '—';
  };
  proto._on = function (role) { return !!role && this._value(role) > (role.min + role.max) / 2; };
  proto._stepName = function (role) {
    if (role && role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    return this._disp(role);
  };
  proto._fmtFreq = function (role) {
    if (!role) return '—';
    var v = this._value(role), fb = v >= 1000 ? (Math.round(v / 10) / 100) + 'k' : Math.round(v) + '';
    return AVC.showVal((this.state.pv[role.index] || {}).disp, fb);
  };
  proto._fmtGain = function (role) {
    if (!role) return '—';
    var v = this._value(role), fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10);
    return AVC.showVal((this.state.pv[role.index] || {}).disp, fb);
  };
  proto._fmtQ = function (role) {
    if (!role) return '—';
    return AVC.showVal((this.state.pv[role.index] || {}).disp, (Math.round(this._value(role) * 1000) / 1000) + '');
  };

  function kindOf(name) {
    var n = String(name || '').toLowerCase();
    if (n.indexOf('low shelf') >= 0) return 'lowshelf';
    if (n.indexOf('high shelf') >= 0) return 'highshelf';
    if (n.indexOf('low cut') >= 0 || n.indexOf('high pass') >= 0) return 'highpass';
    if (n.indexOf('high cut') >= 0 || n.indexOf('low pass') >= 0) return 'lowpass';
    if (n.indexOf('notch') >= 0) return 'notch';
    return 'bell';
  }

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
      else this._drawQ(ctx, x);
    }
  };

  proto._pill = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 4);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 3.5, '700 9px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };

  // small shape curve glyph inside [x,y,w,h]
  proto._shapeGlyph = function (ctx, x, y, w, h, kind, color) {
    var midY = y + h / 2;
    ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.beginPath();
    for (var px = 0; px <= w; px += 3) {
      var t = px / w, yy = midY;
      switch (kind) {
        case 'bell': yy = midY - Math.exp(-Math.pow((t - 0.5) / 0.16, 2)) * (h * 0.32); break;
        case 'lowshelf': yy = midY - (1 - t) * (h * 0.30) + (h * 0.15); break;
        case 'highshelf': yy = midY - t * (h * 0.30) + (h * 0.15); break;
        case 'highpass': yy = midY + (t < 0.4 ? (0.4 - t) * (h * 0.7) : 0); break;
        case 'lowpass': yy = midY + (t > 0.6 ? (t - 0.6) * (h * 0.7) : 0); break;
        case 'notch': yy = midY + Math.exp(-Math.pow((t - 0.5) / 0.12, 2)) * (h * 0.32); break;
        default: yy = midY;
      }
      if (px === 0) ctx.moveTo(x, yy); else ctx.lineTo(x + px, yy);
    }
    ctx.stroke();
  };

  proto._drawBand = function (ctx, x, slot) {
    var b = slot + 1, color = gfx.bandColors[slot % 8];
    var shape = this._bandRole(b, 'shape'), freq = this._bandRole(b, 'freq'),
        gain = this._bandRole(b, 'gain'), q = this._bandRole(b, 'q');
    var activeHere = (this._activeBand === b);
    var mode = this._dialMode[slot];

    // band tag
    gfx.text2(ctx, 'B' + b, x + 11, TOP[1] - 1, '800 9px Inter, sans-serif', color, 'center');
    // TOP — shape icon + label
    gfx.roundRect(ctx, x + 22, TOP[0], SLOT - 28, TOP[1] - TOP[0], 4);
    ctx.fillStyle = 'rgba(255,255,255,0.05)'; ctx.fill();
    if (shape) {
      this._shapeGlyph(ctx, x + 28, TOP[0] + 2, 40, TOP[1] - TOP[0] - 4, kindOf(this._stepName(shape)), color);
      gfx.text2(ctx, this._stepName(shape), x + 78, TOP[1] - 8, '700 9px Inter, sans-serif', gfx.text, 'left');
    } else gfx.text2(ctx, 'SHAPE?', x + (SLOT) / 2 + 10, TOP[1] - 8, '700 9px Inter, sans-serif', gfx.dim, 'center');

    // MIDDLE — Freq / Gain / Q stacked, active highlighted
    this._valueLine(ctx, x, L1, 'FREQ', this._fmtFreq(freq), mode === 0, color);
    this._valueLine(ctx, x, L2, 'GAIN', this._fmtGain(gain), mode === 1, color);
    this._valueLine(ctx, x, L3, 'Q', this._fmtQ(q), false, color, activeHere); // Q highlighted when active band

    // BOTTOM — one global setting anchored here
    var gkey = P.GLOBALS[slot], grole = this._role(gkey);
    var glabel = P.GLOBAL_LABELS[gkey];
    this._pill(ctx, x + 6, BOT[0], SLOT - 12, BOT[1] - BOT[0], grole ? (glabel + ': ' + this._stepName(grole)) : (glabel + ' ?'), false, '#4dabf7');
  };

  proto._valueLine = function (ctx, x, y, label, val, active, color, qActive) {
    if (active || qActive) {
      gfx.roundRect(ctx, x + 6, y - 11, SLOT - 12, 14, 3);
      ctx.fillStyle = active ? color : 'rgba(111,227,196,0.18)'; ctx.fill();
    }
    var lblColor = active ? '#06251d' : (qActive ? gfx.accent : gfx.dim);
    var valColor = active ? '#06251d' : gfx.text;
    gfx.text2(ctx, label, x + 12, y, active ? '800 9px Inter, sans-serif' : '700 9px Inter, sans-serif', lblColor, 'left');
    gfx.text2(ctx, val, x + SLOT - 12, y, '700 11px "SF Mono", monospace', valColor, 'right');
  };

  proto._drawQ = function (ctx, x) {
    var b = this._activeBand, color = gfx.bandColors[(b - 1) % 8];
    var q = this._bandRole(b, 'q'), byp = this._role('bypass');
    // TOP — target
    gfx.roundRect(ctx, x + 6, TOP[0], SLOT - 12, TOP[1] - TOP[0], 4);
    ctx.fillStyle = 'rgba(255,255,255,0.05)'; ctx.fill();
    gfx.text2(ctx, 'TARGET', x + SLOT / 2, TOP[0] + 9, '600 8px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, 'Band ' + b, x + SLOT / 2, TOP[1] - 6, '800 12px Inter, sans-serif', color, 'center');
    // MIDDLE — big Q
    gfx.text2(ctx, 'Q', x + SLOT / 2, MID[0] + 12, '600 9px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, q ? this._fmtQ(q) : '—', x + SLOT / 2, MID[1] - 4, '800 22px "SF Mono", monospace', color, 'center');
    // BOTTOM — bypass
    this._pill(ctx, x + 6, BOT[0], SLOT - 12, BOT[1] - BOT[0], byp ? (this._on(byp) ? 'BYPASSED' : 'ACTIVE') : 'BYPASS?', byp ? this._on(byp) : false, '#ff8a8a');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot < 5) {
      this._activeBand = slot + 1;                 // dynamic follow
      var b = slot + 1, mode = this._dialMode[slot];
      if (mode === 0) { var f = this._bandRole(b, 'freq'); if (f) this.bridge.cmd.deltaLogIndex(f.index, ticks * AVC.STEP); }
      else { var g = this._bandRole(b, 'gain'); if (g) this.bridge.cmd.deltaIndex(g.index, ticks * AVC.STEP); }
    } else {
      var q = this._bandRole(this._activeBand, 'q');  // dial 6 -> active band Q
      if (q) this.bridge.cmd.deltaLogIndex(q.index, ticks * AVC.STEP);
    }
  };
  // press a band dial: focus it + flip its Freq/Gain mode
  proto.onDialPress = function (slot) {
    if (slot < 5) { this._activeBand = slot + 1; this._dialMode[slot] = (this._dialMode[slot] + 1) % 2; }
  };

  proto.onTouch = function (gx, gy, hold) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var ly = gy;
    if (slot < 5) {
      if (inY(ly, TOP)) { this._activeBand = slot + 1; this._cycle('b' + (slot + 1) + '_shape', hold ? -1 : 1); return; }
      if (inY(ly, MID)) { this._activeBand = slot + 1; this._dialMode[slot] = (this._dialMode[slot] + 1) % 2; return; }
      if (inY(ly, BOT)) { this._cycle(P.GLOBALS[slot], hold ? -1 : 1); return; }   // global setting
    } else {
      if (inY(ly, BOT)) { this._toggle('bypass'); return; }
    }
  };

  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized) this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };
  proto._toggle = function (key) { var r = this._role(key); if (r) this.bridge.cmd.toggleIndex(r.index); };

  proto.dialTitle = function (slot) {
    if (slot < 5) {
      var b = slot + 1, m = this._dialMode[slot];
      var role = m === 0 ? this._bandRole(b, 'freq') : this._bandRole(b, 'gain');
      return 'B' + b + ' ' + P.MODES[m] + ' ' + (m === 0 ? this._fmtFreq(role) : this._fmtGain(role));
    }
    var q = this._bandRole(this._activeBand, 'q');
    return 'Q→B' + this._activeBand + ' ' + this._fmtQ(q);
  };
})(AVC.SpectreController);
