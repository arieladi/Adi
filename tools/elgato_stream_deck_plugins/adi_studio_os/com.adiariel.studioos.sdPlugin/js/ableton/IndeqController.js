'use strict';
/* =============================================================================
   IndeqController — predefined strategy for Analog Obsession INDEQ (VST3).

   NATIVE SVG (L4). The drawing is emitted as SVG directly instead of being
   replayed through the Canvas shim. The ROLE TABLE, OVERRIDES, the match
   patterns, the step labels and the toggle label fallbacks are carried across
   UNCHANGED from 1.5.9.0 — those 12 names were confirmed against a live Ableton
   INDEQ instance, and they are data rather than ink.

   Fixed layout: 6 continuous/stepped knobs on the 6 dials + 6 toggle switches on
   touch zones above/below them. **No dynamic state** — no modes, no tabs, no
   focus, no pagination. This is a hardware panel, not a navigable surface.

   Dials:  1 Low Gain · 2 Low Freq (stepped) · 3 Mid Gain · 4 Mid Freq (stepped)
           5 High Gain · 6 Output
   Toggles (touch): Highpass Filter (z1 top), Low Band Shape (z2 top),
           Mid Bandwidth (z3 top), High Band Shape (z5 top),
           High Frequency 8/16k (z5 bottom), Bypass (z6 top).
   Dial press mirrors that zone's top toggle.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   stable); pin overrides in IndeqController.OVERRIDES. See docs/INDEQ.md.

   TWO LAYOUTS (L6). Zero keys in both — all 36 belong to the Ableton hub shell.

   FULL — 6 dials, the panel above, one zone per dial.

   COMPACT — 4 dials (L14), when a nav window borrows dials 5-6:
     dial 1 = Low Gain · 2 = Mid Gain · 3 = High Gain · 4 = Output.
     The two stepped corner-frequency dials are dropped: on a fixed-frequency EQ
     the corners are set-and-forget setup decisions, while the gains are what a
     hand reaches for mid-listen — which is the situation compact exists for.

     NO tab is invented to hide them behind. Pulsar and Spectre earned their
     fourth tab because they already HAD a strip-wide tab row; INDEQ has none,
     and bolting one on would give a stateless plugin a mode concept it does not
     have. Compact is the same four zones drawn verbatim — SELECTED, not
     redesigned — which is why High Gain keeps its bottom row and the 8/16 kHz
     switch survives. Of the six toggles only Low Band Shape is lost, with the
     Low Freq zone that carried it.
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

/* L14 — which FULL zone each compact dial carries. Indexing the same tables
   rather than duplicating them is the whole point: a compact zone is the full
   zone, verbatim, so nothing can drift between the two layouts. */
AVC.IndeqController.COMPACT_SLOTS = [0, 2, 4, 5];   // Low Gain · Mid Gain · High Gain · Output

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
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;
  var TOP = [3, 25], MID = [30, 74], BOT = [77, 97];

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

  // ------------------------------------------------------------ layout mode
  /* There is no mode to fall back and no state to validate — the only thing
     setZones changes is WHICH of the six fixed zones are on screen. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };
  proto._compact = function () { return this._zones() < 6; };
  /* Dial slot -> the FULL zone it carries. L14: compact selects zones, it does
     not redesign them. */
  proto._slotFor = function (slot) {
    if (!this._compact()) return slot;
    var s = P.COMPACT_SLOTS[slot];
    return s == null ? -1 : s;
  };

  // ============================================================== rendering
  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!this._resolved) {
      Svg.text(b, 'INDEQ — reading parameters…', 12, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }
    for (var slot = 0; slot < n; slot++) {
      var x = slot * SLOT;
      if (slot > 0) Svg.line(b, x + 0.5, 4, x + 0.5, H - 4, gfx.line, 1);
      this._buildZone(b, x, this._slotFor(slot));
    }
    return b;
  };

  proto._pill = function (b, x, y, w, h, label, on, color) {
    Svg.rrect(b, x, y, w, h, 4, on ? (color || gfx.accent) : 'rgba(255,255,255,0.06)');
    Svg.text(b, label, x + w / 2, y + h / 2 + 3.5, 9, 700, on ? '#06251d' : gfx.dim, 'middle');
  };

  /* `zone` is a FULL zone index 0-5, whichever dial happens to be showing it. */
  proto._buildZone = function (b, x, zone) {
    if (zone < 0) return;
    var color = gfx.bandColors[zone % 8];

    // TOP toggle
    var topKey = P.TOP[zone];
    if (topKey) {
      var tr = this._role(topKey);
      var isByp = (topKey === 'bypass');
      var on = tr ? this._on(tr) : false;
      var label = tr ? (isByp ? this._toggleText(tr) : (tr.tag + ' ' + this._toggleText(tr))) : (topKey.toUpperCase() + ' ?');
      this._pill(b, x + 6, TOP[0], SLOT - 12, TOP[1] - TOP[0], label, on, isByp ? '#ff8a8a' : color);
    }

    // MIDDLE name + value
    var role = this._role(P.DIAL[zone]);
    var isStep = role && role.kind === 'step';
    Svg.text(b, P.NAME[zone], x + SLOT / 2, MID[0] + 11, 10, 600, gfx.dim, 'middle');
    Svg.mono(b, role ? (isStep ? this._fmtStep(role) : this._fmtDb(role)) : '—',
             x + SLOT / 2, MID[1] - 2, 18, 800, role ? gfx.text : gfx.dim, 'middle');

    // BOTTOM toggle (zone 5: High Frequency)
    var botKey = P.BOT[zone];
    if (botKey) {
      var br = this._role(botKey);
      var blabel = br ? (br.tag + ' ' + this._toggleText(br)) : (botKey.toUpperCase() + ' ?');
      this._pill(b, x + 6, BOT[0], SLOT - 12, BOT[1] - BOT[0], blabel, br ? this._on(br) : false, '#4dabf7');
    }
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;                    // dial on loan to a window
    var zone = this._slotFor(slot); if (zone < 0) return;
    var role = this._role(P.DIAL[zone]); if (!role) return;
    if (role.kind === 'step') this.bridge.cmd.stepIndex(role.index, ticks >= 0 ? 1 : -1, role.quantized ? 0 : role.steps);
    else this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };

  // dial press mirrors the zone's top toggle — in BOTH layouts, since a compact
  // zone IS the full zone.
  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    var zone = this._slotFor(slot); if (zone < 0) return;
    var key = P.TOP[zone]; if (!key) return;
    this._toggle(key);
  };

  proto.onTouch = function (gx, gy) {
    var slot = Math.floor(gx / SLOT);
    if (slot < 0 || slot >= this._zones()) return;
    var zone = this._slotFor(slot); if (zone < 0) return;
    var ly = gy;
    if (inY(ly, TOP) && P.TOP[zone]) { this._toggle(P.TOP[zone]); return; }
    if (inY(ly, BOT) && P.BOT[zone]) { this._toggle(P.BOT[zone]); return; }
  };
  proto._toggle = function (key) { var r = this._role(key); if (r) this.bridge.cmd.toggleIndex(r.index); };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    var zone = this._slotFor(slot); if (zone < 0) return '';
    var role = this._role(P.DIAL[zone]);
    if (!role) return P.NAME[zone];
    return P.NAME[zone] + ' ' + (role.kind === 'step' ? this._fmtStep(role) : this._fmtDb(role));
  };
})(AVC.IndeqController);
