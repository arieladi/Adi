'use strict';
/* =============================================================================
   IndeqController — predefined strategy for Analog Obsession INDEQ (VST3).

   Fixed layout: 6 continuous/stepped knobs on the 6 dials + 6 toggle switches on
   touch zones above/below them. No dynamic state.

   Dials:  1 Low Gain · 2 Low Freq (stepped) · 3 Mid Gain · 4 Mid Freq (stepped)
           5 High Gain · 6 Output
   Toggles (touch): Highpass Filter (z1 top), Low Band Shape (z2 top),
           Mid Bandwidth (z3 top), High Band Shape (z5 top),
           High Frequency 8/16k (z5 bottom), Bypass (z6 top).

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   stable). The match patterns use the exact names from INDEQ's Ableton parameter
   list; pin overrides in IndeqController.OVERRIDES if your build differs.
   See docs/INDEQ.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.IndeqController = function IndeqController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
};
AVC.IndeqController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.IndeqController.prototype.id = 'indeq';

AVC.IndeqController.OVERRIDES = {};   // roleKey -> exact Live name or numeric index

// dial slot -> role; zone label; top/bottom toggle role per slot
AVC.IndeqController.DIAL = ['low_gain', 'low_freq', 'mid_gain', 'mid_freq', 'high_gain', 'output'];
AVC.IndeqController.NAME = ['Low Gain', 'Low Freq', 'Mid Gain', 'Mid Freq', 'High Gain', 'Output'];
AVC.IndeqController.TOP = ['hpf', 'low_shape', 'mid_bw', null, 'high_shape', 'bypass'];
AVC.IndeqController.BOT = [null, null, null, null, 'high_freq', null];

AVC.IndeqController.ROLES = [
  { key: 'low_gain',  kind: 'cont', match: ['low gain'] },
  { key: 'low_freq',  kind: 'step', steps: 4, labels: ['35', '60', '100', '220'], unit: 'Hz',
    match: ['low frequency', 'low freq'] },     // Ableton exposes it as "Low Frequency"
  { key: 'mid_gain',  kind: 'cont', match: ['mid gain'] },
  { key: 'mid_freq',  kind: 'step', steps: 6, labels: ['.2', '.35', '.7', '1.5', '3', '6'], unit: 'kHz',
    match: ['mid frequency', 'mid freq'] },      // Ableton exposes it as "Mid Frequency"
  { key: 'high_gain', kind: 'cont', match: ['high gain'] },
  { key: 'output',    kind: 'cont', match: ['output', 'out gain', 'output gain', 'out level'] },
  { key: 'hpf',        kind: 'toggle', tag: 'HPF',    labels: ['OFF', 'ON'],     match: ['highpass filter', 'high pass filter', 'hpf', 'highpass', 'high pass'] },
  { key: 'low_shape',  kind: 'toggle', tag: 'SHAPE',  labels: ['SHELF', 'PEAK'], match: ['low band shape', 'low shape', 'low shelf', 'low peak'] },
  { key: 'mid_bw',     kind: 'toggle', tag: 'BW',     labels: ['NORMAL', 'HIGH'], match: ['mid bandwidth', 'mid bw', 'mid q', 'bandwidth'] },
  { key: 'high_shape', kind: 'toggle', tag: 'SHAPE',  labels: ['SHELF', 'PEAK'], match: ['high band shape', 'high shape'] },
  { key: 'high_freq',  kind: 'toggle', tag: 'HF',     labels: ['8kHz', '16kHz'], match: ['high frequency', 'high freq'] },
  { key: 'bypass',     kind: 'toggle', tag: 'BYP',    labels: ['IN', 'BYP'],     match: ['bypass', 'device on', 'i o', 'io', 'on off', 'power'] },
];

(function (P) {
  var proto = P.prototype, gfx = AVC.gfx;
  var SLOT = 200, SLOTS = 6;
  var TOP = [3, 25], MID = [30, 74], BOT = [77, 97];

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
          if (norm(params[k].name).indexOf(pat) >= 0) { found = params[k]; break; }
        }
      }
      if (found) {
        roles[role.key] = {
          index: found.i, name: found.name, min: found.min, max: found.max,
          quantized: !!found.quantized, items: found.items || [],
          kind: role.kind, steps: role.steps || 0, labels: role.labels || null,
          unit: role.unit || '', tag: role.tag || '',
        };
      } else { missing.push(role.key); }
    });
    this._roles = roles; this._missing = missing; this._resolved = true;
    var watch = Object.keys(roles).map(function (k) { return roles[k].index; });
    if (watch.length) this.bridge.cmd.watch(watch);
    if (missing.length && this.sd && this.sd.log) {
      this.sd.log('INDEQ unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set IndeqController.OVERRIDES');
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
  proto._on = function (role) { return !!role && this._value(role) > (role.min + role.max) / 2; };

  // gain / output (dB)
  proto._fmtDb = function (role) {
    if (!role) return '—';
    var v = this._value(role), fb = (v >= 0 ? '+' : '') + (Math.round(v * 10) / 10) + ' dB';
    return AVC.showVal((this.state && this.state.pv && (this.state.pv[role.index] || {}).disp), fb);
  };
  // stepped frequency (uses Live's value_items if present, else our labels)
  proto._fmtStep = function (role) {
    if (!role) return '—';
    if (role.quantized && role.items.length) return String(role.items[Math.round(this._value(role))] || '');
    if (role.labels && role.labels.length) {
      var t = (this._value(role) - role.min) / ((role.max - role.min) || 1);
      var idx = Math.round(gfx.clamp(t, 0, 1) * (role.labels.length - 1));
      return role.labels[idx] + (role.unit ? ' ' + role.unit : '');
    }
    return (Math.round(this._value(role) * 100) / 100) + '';
  };
  // toggle state text (Live's item names if present, else our 2 labels)
  proto._toggleText = function (role) {
    if (!role) return '?';
    if (role.quantized && role.items.length >= 2) return String(role.items[Math.round(this._value(role))] || '');
    var labs = role.labels || ['OFF', 'ON'];
    return labs[this._on(role) ? 1 : 0];
  };

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'INDEQ — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, L.H - 4); ctx.stroke(); }
      this._drawZone(ctx, x, slot);
    }
  };

  proto._pill = function (ctx, x, y, w, h, label, on, color) {
    gfx.roundRect(ctx, x, y, w, h, 4);
    ctx.fillStyle = on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)'; ctx.fill();
    gfx.text2(ctx, label, x + w / 2, y + h / 2 + 3.5, '700 9px Inter, sans-serif', on ? '#06251d' : gfx.dim, 'center');
  };

  proto._drawZone = function (ctx, x, slot) {
    var color = gfx.bandColors[slot % 8];
    // TOP toggle
    var topKey = P.TOP[slot];
    if (topKey) {
      var tr = this._role(topKey);
      var isByp = (topKey === 'bypass');
      var on = tr ? this._on(tr) : false;
      var label = tr ? (isByp ? this._toggleText(tr) : (tr.tag + ' ' + this._toggleText(tr))) : (topKey.toUpperCase() + ' ?');
      this._pill(ctx, x + 6, TOP[0], SLOT - 12, TOP[1] - TOP[0], label, on, isByp ? '#ff8a8a' : color);
    }
    // MIDDLE name + value
    var dialKey = P.DIAL[slot], role = this._role(dialKey);
    var isStep = role && role.kind === 'step';
    gfx.text2(ctx, P.NAME[slot], x + SLOT / 2, MID[0] + 11, '600 10px Inter, sans-serif', gfx.dim, 'center');
    gfx.text2(ctx, role ? (isStep ? this._fmtStep(role) : this._fmtDb(role)) : '—',
      x + SLOT / 2, MID[1] - 2, '800 18px "SF Mono", monospace', role ? gfx.text : gfx.dim, 'center');
    // BOTTOM toggle (zone 5: High Frequency)
    var botKey = P.BOT[slot];
    if (botKey) {
      var br = this._role(botKey);
      var blabel = br ? (br.tag + ' ' + this._toggleText(br)) : (botKey.toUpperCase() + ' ?');
      this._pill(ctx, x + 6, BOT[0], SLOT - 12, BOT[1] - BOT[0], blabel, br ? this._on(br) : false, '#4dabf7');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var role = this._role(P.DIAL[slot]); if (!role) return;
    if (role.kind === 'step') this.bridge.cmd.stepIndex(role.index, ticks >= 0 ? 1 : -1, role.quantized ? 0 : role.steps);
    else this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };
  // dial press mirrors the zone's top toggle
  proto.onDialPress = function (slot) {
    var key = P.TOP[slot]; if (!key) return;
    var r = this._role(key); if (r) this.bridge.cmd.toggleIndex(r.index);
  };
  proto.onTouch = function (gx, gy) {
    var slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var ly = gy;
    if (inY(ly, TOP) && P.TOP[slot]) { this._toggle(P.TOP[slot]); return; }
    if (inY(ly, BOT) && P.BOT[slot]) { this._toggle(P.BOT[slot]); return; }
  };
  proto._toggle = function (key) { var r = this._role(key); if (r) this.bridge.cmd.toggleIndex(r.index); };

  proto.dialTitle = function (slot) {
    var role = this._role(P.DIAL[slot]);
    if (!role) return P.NAME[slot];
    return P.NAME[slot] + ' ' + (role.kind === 'step' ? this._fmtStep(role) : this._fmtDb(role));
  };
})(AVC.IndeqController);
