'use strict';
/* =============================================================================
   ValhallaVintageVerbController — predefined strategy for Valhalla DSP
   "ValhallaVintageVerb" (VST3 reverb, v2.1.x).

   Same paged design as ValhallaRoomController. Tap the MAIN / DAMP / SHAPE tabs
   (or press a dial) to switch what the 6 dials control:
     MAIN  : Mix · Predelay · Decay · Size · High Cut · Low Cut
     DAMP  : High Freq · High Shelf · Bass Xover · Bass Mult · Decay · Mix
     SHAPE : Attack · Early Diffusion · Late Diffusion · Mod Rate · Mod Depth · Size
   A full-width bottom bar holds the two selectors: left = Reverb Mode (the
   algorithm, e.g. Concert Hall), right = Color Mode (the era voicing, e.g.
   1970s/seventies). Tap to cycle, hold/right-tap = previous.

   Parameters resolve by NAME from the bridge's all_params (VST3 indexes aren't
   version-stable). Continuous params use delta_index; the two selectors step.
   Pin exact names/indexes in ValhallaVintageVerbController.OVERRIDES.
   See docs/VALHALLA_VINTAGE_VERB.md.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.ValhallaVintageVerbController = function ValhallaVintageVerbController(services) {
  AVC.DeviceController.call(this, services);
  this._sig = null;
  this._resolved = false;
  this._roles = {};
  this._missing = [];
  this.page = 'main';
};
AVC.ValhallaVintageVerbController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.ValhallaVintageVerbController.prototype.id = 'valhalla-vintageverb';

AVC.ValhallaVintageVerbController.PAGES_ORDER = ['main', 'damp', 'shape'];
AVC.ValhallaVintageVerbController.PAGE_LABEL = { main: 'MAIN', damp: 'DAMP', shape: 'SHAPE' };
AVC.ValhallaVintageVerbController.PAGES = {
  main:  ['mix', 'predelay', 'decay', 'size', 'highcut', 'lowcut'],
  damp:  ['highfreq', 'highshelf', 'bassxover', 'bassmult', 'decay', 'mix'],
  shape: ['attack', 'earlydiffusion', 'latediffusion', 'modrate', 'moddepth', 'size'],
};
AVC.ValhallaVintageVerbController.LABEL = {
  mix: 'MIX', predelay: 'PREDLY', decay: 'DECAY', size: 'SIZE', highcut: 'HI CUT', lowcut: 'LO CUT',
  highfreq: 'HF DAMP', highshelf: 'HF SHLF', bassxover: 'BAS XO', bassmult: 'BAS MUL',
  attack: 'ATTACK', earlydiffusion: 'E DIFF', latediffusion: 'L DIFF', modrate: 'MOD R', moddepth: 'MOD D',
};

/* roleKey -> exact Live parameter NAME or numeric index. */
AVC.ValhallaVintageVerbController.OVERRIDES = {};

AVC.ValhallaVintageVerbController.ROLES = [
  { key: 'mix',            match: [/^mix$/, 'mix'] },
  { key: 'predelay',       match: [/^predelay$/, 'predelay', 'pre delay'] },
  { key: 'decay',          match: [/^decay$/, 'decay'] },
  { key: 'size',           match: [/^size$/, 'size'] },
  { key: 'attack',         match: [/^attack$/, 'attack'] },
  { key: 'highfreq',       match: [/^highfreq$/, 'high freq', 'highfreq'] },
  { key: 'highshelf',      match: [/^highshelf$/, 'high shelf', 'highshelf'] },
  { key: 'bassxover',      match: [/^bassxover$/, 'bass xover', 'bassxover', 'bass freq'] },
  { key: 'bassmult',       match: [/^bassmult$/, 'bass mult', 'bassmult'] },
  { key: 'earlydiffusion', match: [/^earlydiffusion$/, 'early diffusion', 'earlydiffusion'] },
  { key: 'latediffusion',  match: [/^latediffusion$/, 'late diffusion', 'latediffusion'] },
  { key: 'modrate',        match: [/^modrate$/, 'mod rate', 'modrate'] },
  { key: 'moddepth',       match: [/^moddepth$/, 'mod depth', 'moddepth'] },
  { key: 'highcut',        match: [/^highcut$/, 'high cut', 'highcut'] },
  { key: 'lowcut',         match: [/^lowcut$/, 'low cut', 'lowcut'] },
  { key: 'reverbmode',     kind: 'cycle', match: [/^reverbmode$/, 'reverb mode', 'reverbmode'] },
  { key: 'colormode',      kind: 'cycle', match: [/^colormode$/, 'color mode', 'colormode', /^color$/] },
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
      this.sd.log('ValhallaVintageVerb unresolved roles: ' + missing.join(', ') +
        ' — check param names in Live Log.txt and set ValhallaVintageVerbController.OVERRIDES');
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

  // ============================================================== rendering
  proto.renderTouch = function (ctx) {
    var L = this.L; gfx.clear(ctx, L.W, L.H);
    if (!this._resolved) {
      gfx.text2(ctx, 'ValhallaVintageVerb — reading parameters…', 12, L.H / 2, '600 13px Inter, sans-serif', gfx.dim);
      return;
    }
    for (var slot = 0; slot < SLOTS; slot++) {
      var x = slot * SLOT;
      if (slot > 0) { ctx.strokeStyle = gfx.line; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x + 0.5, 4); ctx.lineTo(x + 0.5, BOT[0] - 2); ctx.stroke(); }
      this._drawZone(ctx, x, slot);
    }
    this._drawGlobalBar(ctx);
  };

  proto._drawTabs = function (ctx, x, color) {
    var pages = P.PAGES_ORDER, tw = (SLOT - 8) / pages.length;
    for (var i = 0; i < pages.length; i++) {
      var act = pages[i] === this.page;
      gfx.roundRect(ctx, x + 4 + i * tw + 1, TAB[0], tw - 2, TAB[1] - TAB[0], 3);
      ctx.fillStyle = act ? (color || gfx.accent) : 'rgba(255,255,255,0.05)'; ctx.fill();
      gfx.text2(ctx, P.PAGE_LABEL[pages[i]], x + 4 + i * tw + tw / 2, TAB[1] - 3.5,
        act ? '800 7px Inter, sans-serif' : '600 6px Inter, sans-serif', act ? '#06251d' : gfx.dim, 'center');
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
    gfx.text2(ctx, role ? this._fmt(role) : '—', x + SLOT / 2, MID[1] - 3, '800 18px "SF Mono", monospace', role ? gfx.text : gfx.dim, 'center');
  };

  proto._drawGlobalBar = function (ctx) {
    var L = this.L, half = L.W / 2;
    var mode = this._role('reverbmode'), color = this._role('colormode');
    gfx.roundRect(ctx, 6, BOT[0], half - 12, BOT[1] - BOT[0], 5);
    ctx.fillStyle = 'rgba(151,117,250,0.18)'; ctx.fill();
    gfx.text2(ctx, 'MODE', 16, BOT[0] + 9, '600 8px Inter, sans-serif', gfx.dim, 'left');
    gfx.text2(ctx, mode ? this._stepName(mode) : '— (configure ReverbMode)', half / 2, BOT[1] - 5, '800 13px Inter, sans-serif', mode ? '#c9b8ff' : gfx.dim, 'center');
    gfx.roundRect(ctx, half + 6, BOT[0], half - 12, BOT[1] - BOT[0], 5);
    ctx.fillStyle = 'rgba(255,169,77,0.16)'; ctx.fill();
    gfx.text2(ctx, 'COLOR', half + 16, BOT[0] + 9, '600 8px Inter, sans-serif', gfx.dim, 'left');
    gfx.text2(ctx, color ? this._stepName(color) : '— (configure ColorMode)', half + half / 2, BOT[1] - 5, '800 13px Inter, sans-serif', color ? '#ffcf99' : gfx.dim, 'center');
  };

  // ================================================================= input
  proto.onDial = function (slot, ticks) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    if (role) this.bridge.cmd.deltaIndex(role.index, ticks * AVC.STEP);
  };
  proto.onDialPress = function () {
    var order = P.PAGES_ORDER, i = order.indexOf(this.page);
    this.page = order[(i + 1) % order.length];
  };
  proto.onTouch = function (gx, gy, hold) {
    var L = this.L, slot = Math.floor(gx / SLOT); if (slot < 0 || slot > 5) return;
    var lx = gx - slot * SLOT, ly = gy;
    var tab = this._tabHit(lx, ly);
    if (tab) { this.page = tab; return; }
    if (inY(ly, BOT)) {
      if (gx < L.W / 2) this._cycle('reverbmode', hold ? -1 : 1);
      else this._cycle('colormode', hold ? -1 : 1);
    }
  };
  proto._cycle = function (key, dir) {
    var r = this._role(key); if (!r) return;
    if (r.quantized || r.kind === 'cycle') this.bridge.cmd.stepIndex(r.index, dir, 0);
    else this.bridge.cmd.deltaIndex(r.index, dir * AVC.STEP * 2);
  };

  proto.dialTitle = function (slot) {
    var key = this._pageRoleKey(slot), role = key ? this._role(key) : null;
    return P.PAGE_LABEL[this.page] + ' ' + (key ? P.LABEL[key] : '—') + ' ' + (role ? this._fmt(role) : '');
  };
})(AVC.ValhallaVintageVerbController);
