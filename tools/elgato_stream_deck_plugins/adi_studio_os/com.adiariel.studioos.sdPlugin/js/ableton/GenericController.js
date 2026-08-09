'use strict';
/* =============================================================================
   GenericController — default strategy for any device not in the predefined
   list (native devices AND external VST2/VST3/AU). Maps the first 6 non-quantized
   parameters to the 6 dials and draws the touchscreen as 6 vertical zones, one
   per dial, each showing the parameter name + live value.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.GenericController = function GenericController(services) { AVC.DeviceController.call(this, services); };
AVC.GenericController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.GenericController.prototype.id = 'generic';

AVC.GenericController.prototype._paramFor = function (slot) {
  var ps = (this.state && this.state.params) || [];
  for (var i = 0; i < ps.length; i++) if (ps[i].slot === slot) return ps[i];
  return null;
};

AVC.GenericController.prototype.renderTouch = function (ctx) {
  var L = this.L, g = AVC.gfx;
  g.clear(ctx, L.W, L.H);
  var dev = (this.state && this.state.device) || {};

  for (var slot = 0; slot < L.slots; slot++) {
    var x0 = slot * L.slotW;
    // zone divider
    ctx.strokeStyle = g.line; ctx.lineWidth = 1;
    if (slot > 0) { ctx.beginPath(); ctx.moveTo(x0 + 0.5, 8); ctx.lineTo(x0 + 0.5, L.H - 8); ctx.stroke(); }

    var p = this._paramFor(slot);
    var cx = x0 + L.slotW / 2;
    if (!dev.has_device) {
      g.text2(ctx, slot === 0 ? 'No device selected' : '', x0 + 10, L.H / 2, '600 13px Inter, sans-serif', g.dim);
      continue;
    }
    if (!p) {
      g.text2(ctx, '—', cx, L.H / 2, '600 14px Inter, sans-serif', g.dim, 'center');
      continue;
    }
    // name
    g.text2(ctx, this._short(p.name, 16), cx, 20, '600 11px Inter, sans-serif', g.text, 'center');
    // horizontal value bar
    var bw = L.slotW - 28, bx = x0 + 14, by = 40, bh = 10;
    var t = (p.value - p.min) / ((p.max - p.min) || 1);
    t = g.clamp(t, 0, 1);
    g.roundRect(ctx, bx, by, bw, bh, 4); ctx.fillStyle = 'rgba(255,255,255,0.06)'; ctx.fill();
    g.roundRect(ctx, bx, by, Math.max(2, bw * t), bh, 4); ctx.fillStyle = g.accent; ctx.fill();
    // value text
    g.text2(ctx, p.disp != null ? String(p.disp) : (Math.round(p.value * 100) / 100), cx, L.H - 14,
            '700 16px "SF Mono", monospace', g.text, 'center');
  }
};

AVC.GenericController.prototype.onDial = function (slot, ticks) {
  if (this._paramFor(slot)) this.bridge.cmd.paramDelta(slot, ticks * AVC.STEP);
};

AVC.GenericController.prototype.onDialPress = function (slot) {
  // double-use: press recenters a bipolar-ish param to its midpoint
  var p = this._paramFor(slot);
  if (p) this.bridge.cmd.paramSet(slot, 0.5);
};

AVC.GenericController.prototype.dialTitle = function (slot) {
  var p = this._paramFor(slot);
  return p ? this._short(p.name, 12) : '';
};

AVC.GenericController.prototype._short = function (s, n) {
  s = String(s || '');
  return s.length > n ? s.slice(0, n - 1) + '…' : s;
};
