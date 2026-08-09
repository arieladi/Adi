'use strict';
/* =============================================================================
   GenericController — the fallback for any device not in the predefined list
   (native devices AND external VST2/VST3/AU). The bridge maps the first N
   non-quantized parameters to slots 0..N-1; this draws one zone per slot with
   the parameter name, a value bar and Ableton's own value string.

   NATIVE SVG (L4). Parameter selection is untouched — it is the bridge that
   decides which parameters are exposed and in what order, so there is nothing
   device-specific here to get wrong.

   TWO LAYOUTS (L6):

     FULL     6 dials — parameters 1-6, one per zone.
     COMPACT  4 dials — parameters 1-4, one per zone. Parameters 5 and 6 are
              simply DROPPED.

   The compact rule is deliberately the dumbest possible one: a blind chop of the
   last two, with the first four mapped linearly. This controller is a catch-all
   whose parameter choice is already arbitrary (whatever the device happens to
   expose first), so ranking them for a small screen would be inventing meaning
   that is not there. Adi is overhauling the generic logic later; when the
   selection becomes intentional, the compact view can become intentional too.
   ============================================================================= */

window.AVC = window.AVC || {};

AVC.GenericController = function GenericController(services) {
  AVC.DeviceController.call(this, services);
};
AVC.GenericController.prototype = Object.create(AVC.DeviceController.prototype);
AVC.GenericController.prototype.id = 'generic';

(function (P) {
  var proto = P.prototype;
  var Svg = SOS.Svg, gfx = AVC.gfx;
  var SLOT = 200, H = 100;

  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  proto._short = function (s, n) {
    s = String(s || '');
    return s.length > n ? s.slice(0, n - 1) + '…' : s;
  };

  proto._paramFor = function (slot) {
    var ps = (this.state && this.state.params) || [];
    for (var i = 0; i < ps.length; i++) if (ps[i].slot === slot) return ps[i];
    return null;
  };

  /* Zones = dials available. Because slots are already a linear 0..N index, the
     compact "chop the last two" rule needs no special case: rendering and
     driving fewer zones simply never touches slots 4 and 5. */
  proto.setZones = function (z) { this.zones = clamp(z | 0, 1, 6); };
  proto._zones = function () { return this.zones || 6; };

  proto.build = function (zones) {
    this.setZones(zones);
    var b = Svg.bag(), n = this._zones(), W = n * SLOT;
    var dev = (this.state && this.state.device) || {};
    Svg.rect(b, 0, 0, W, H, gfx.bg);

    if (!dev.has_device) {
      Svg.text(b, 'No device selected', 10, H / 2, 13, 600, gfx.dim, 'start');
      return b;
    }

    for (var slot = 0; slot < n; slot++) {
      var x0 = slot * SLOT, cx = x0 + SLOT / 2;
      if (slot > 0) Svg.line(b, x0 + 0.5, 8, x0 + 0.5, H - 8, gfx.line, 1);

      var p = this._paramFor(slot);
      if (!p) {
        Svg.text(b, '—', cx, H / 2, 14, 600, gfx.dim, 'middle');
        continue;
      }

      Svg.text(b, this._short(p.name, 16), cx, 20, 11, 600, gfx.text, 'middle');

      var bw = SLOT - 28, bx = x0 + 14, by = 40, bh = 10;
      var t = clamp((p.value - p.min) / ((p.max - p.min) || 1), 0, 1);
      Svg.rrect(b, bx, by, bw, bh, 4, 'rgba(255,255,255,0.06)');
      Svg.rrect(b, bx, by, Math.max(2, bw * t), bh, 4, gfx.accent);

      Svg.mono(b, p.disp != null ? String(p.disp) : (Math.round(p.value * 100) / 100),
               cx, H - 14, 16, 700, gfx.text, 'middle');
    }
    return b;
  };

  // ==================================================================== input
  proto.onDial = function (slot, ticks) {
    if (slot >= this._zones()) return;              // dial is on loan to a window
    if (this._paramFor(slot)) this.bridge.cmd.paramDelta(slot, ticks * AVC.STEP);
  };

  proto.onDialPress = function (slot) {
    if (slot >= this._zones()) return;
    // Press recenters a bipolar-ish parameter to its midpoint.
    if (this._paramFor(slot)) this.bridge.cmd.paramSet(slot, 0.5);
  };

  proto.dialTitle = function (slot) {
    if (slot >= this._zones()) return '';
    var p = this._paramFor(slot);
    return p ? this._short(p.name, 12) : '';
  };
})(AVC.GenericController);
