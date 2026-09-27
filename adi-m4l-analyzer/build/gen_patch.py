"""
Emits avc.specreduce.genjit -- the jit.gen patcher that turns a linear-frequency
bin matrix into one display point per horizontal pixel.

This is the stage that keeps the drawing layer cheap: everything below runs in
compiled gen code over a W-cell output (W = plot width in pixels), and jsui only
ever receives one already-reduced list per curve per frame.

INPUTS
  in1   W x 1, 2 plane   geometry carrier.  Its only job is to give the output
                         its dimensions and planecount; contents are ignored.
  in2   8192 x 1, 2 plane   plane 0 = primary curve amplitude, plane 1 =
                         secondary, already selected out of the analysis
                         matrix's five planes by jit.matrix @planemap.

OUTPUT
  out1  W x 1, 2 plane   display dB for primary and secondary.

AGGREGATION.  Each output column covers a frequency band [f0,f1].  Where that
band spans one or more whole bins the column takes the MAX over exactly those
bins -- every bin is visited exactly once across the whole output, so the cost
is O(nbins) per frame, not O(W * nbins).  A plain interpolated resample would
alias badly at the top end (at 16384/44.1k one pixel near 20 kHz spans ~95 bins,
and a tone falling between sample points would flicker or vanish).  Where the
band is narrower than one bin -- the low end at small block sizes -- it
interpolates instead, which is what keeps the bottom octaves smooth.

SMOOTHING is octave-proportional, so it is applied simply by widening each
column's band before the bin range is computed, and switching the aggregation
from max to power-mean.  Widening in the log domain IS constant-octave
smoothing, so this costs nothing extra.  Note this reproduces SPAN's documented
side effect: on a stationary sine the reading droops ~6 dB/oct, because a sine
occupies a fixed number of bins while the averaging band grows with frequency.

SLOPE is applied after the dB conversion and pivots at 1 kHz:
    dB_display = dB_raw + slope * log2(f / 1000)
"""

import json
from maxpat import APPVERSION

CODE = r"""// avc.specreduce -- linear bins -> log-frequency display columns.
// See build/gen_patch.py for the derivation of every line here.

Param wid(600);        // number of display columns (plot width in pixels)
Param nbins(2048);     // active bins for the current block size = N/2
Param binhz(10.7666);  // Hz per bin = samplerate / N
Param flo(10);         // display frequency range, low
Param fhi(20000);      // display frequency range, high
Param slope(4.5);      // dB per octave, pivoting at 1 kHz
Param align(0);        // level-alignment offset in dB
Param smooth(0);       // smoothing width in octaves, 0 = off

c   = swiz(cell, 0);                       // this column, 0 .. wid-1

// The output matrix is as wide as the ANALYSIS matrix, not as wide as the
// plot -- see pushGen() in avc.engine.js for why that is not negotiable.
// Cells past the display width simply repeat the last column; jit.spill only
// reads the first `wid` of them.
c   = min(c, wid - 1);
lr  = log(fhi / flo);                      // total span, natural log
oct = lr * 1.4426950408889634;             // total span in octaves

// octave-proportional smoothing == widening the band in the log domain
hw  = (smooth > 0) ? (smooth / oct) * wid * 0.5 : 0;

u0  = (c - hw) / wid;
u1  = (c + 1 + hw) / wid;
f0  = flo * exp(lr * u0);                  // band edges for this column
f1  = flo * exp(lr * u1);
fc  = flo * exp(lr * ((c + 0.5) / wid));   // band centre, used for the slope

// bins whose centre falls inside the band
i0  = ceil(f0 / binhz);
i1  = floor(f1 / binhz);
i0  = max(i0, 1);                          // never display DC
i1  = min(i1, nbins - 1);
// Clamp the TOP end too.  The analysis matrix is allocated for the largest
// block size (8192 cells) but only `nbins` of it is ever written by
// jit.poke~, so an unclamped index reads cells no poke has ever touched.
// samplepix's boundmode is `clamp`, so reads past the far edge return the
// edge cell rather than failing -- silently, as isolated full-scale spikes
// standing above the real spectrum.  i1 is deliberately NOT raised to 1:
// leaving it below i0 at the very bottom is what selects the interpolating
// branch, which is what keeps the lowest octaves smooth.
i0  = min(i0, nbins - 1);

pa = 0; pb = 0; sa = 0; sb = 0; n = 0;

if (i1 >= i0) {
    // one or more whole bins land in this column: take the max over them.
    for (i = i0; i <= i1; i += 1) {
        v  = samplepix(in2, vec(i + 0.5, 0.5));   // pixel centres are at +0.5
        va = swiz(v, 0);
        vb = swiz(v, 1);
        pa = max(pa, va);
        pb = max(pb, vb);
        sa = sa + va * va;
        sb = sb + vb * vb;
        n  = n + 1;
    }
} else {
    // narrower than one bin: interpolate so the low end stays smooth.
    bc = min(max(fc / binhz, 1), nbins - 1);   // same clamp, same reason
    v  = samplepix(in2, vec(bc + 0.5, 0.5));
    pa = swiz(v, 0);
    pb = swiz(v, 1);
    sa = pa * pa;
    sb = pb * pb;
    n  = 1;
}

if (smooth > 0) {
    pa = sqrt(sa / n);                     // power mean across the band
    pb = sqrt(sb / n);
}

kdb = 8.685889638065035;                   // 20 / ln(10)
sl  = slope * log(fc / 1000) * 1.4426950408889634;   // slope, pivot 1 kHz

da = log(max(pa, 1e-11)) * kdb + align + sl;
db = log(max(pb, 1e-11)) * kdb + align + sl;

out1 = vec(da, db);
"""


def build_genjit(path):
    boxes = [
        {"box": {"id": "obj-1", "maxclass": "newobj", "numinlets": 0,
                 "numoutlets": 1, "outlettype": [""],
                 "patching_rect": [40.0, 40.0, 60.0, 22.0], "text": "in 1"}},
        {"box": {"id": "obj-2", "maxclass": "newobj", "numinlets": 0,
                 "numoutlets": 1, "outlettype": [""],
                 "patching_rect": [130.0, 40.0, 60.0, 22.0], "text": "in 2"}},
        {"box": {"id": "obj-3", "maxclass": "codebox", "numinlets": 2,
                 "numoutlets": 1, "outlettype": [""],
                 "patching_rect": [40.0, 90.0, 720.0, 640.0],
                 "fontname": "<Monospaced>", "fontsize": 11.0,
                 "code": CODE}},
        {"box": {"id": "obj-4", "maxclass": "newobj", "numinlets": 1,
                 "numoutlets": 0,
                 "patching_rect": [40.0, 760.0, 60.0, 22.0], "text": "out 1"}},
    ]
    lines = [
        {"patchline": {"source": ["obj-1", 0], "destination": ["obj-3", 0]}},
        {"patchline": {"source": ["obj-2", 0], "destination": ["obj-3", 1]}},
        {"patchline": {"source": ["obj-3", 0], "destination": ["obj-4", 0]}},
    ]
    p = {
        "fileversion": 1,
        "appversion": dict(APPVERSION),
        # REQUIRED.  Without it Max does not resolve in/out/codebox in the
        # jit.gen namespace, the gen patcher silently fails to compile, and
        # jit.gen degrades to passing its left input straight through -- which
        # looks like a plot filled solid with 0 dB.
        "classnamespace": "jit.gen",
        "rect": [60.0, 90.0, 820.0, 860.0],
        "bglocked": 0,
        "openinpresentation": 0,
        "default_fontsize": 12.0,
        "default_fontface": 0,
        "default_fontname": "Arial",
        "gridonopen": 1,
        "gridsize": [15.0, 15.0],
        "gridsnaponopen": 1,
        "objectsnaponopen": 1,
        "statusbarvisible": 2,
        "toolbarvisible": 1,
        "lefttoolbarpinned": 0,
        "toptoolbarpinned": 0,
        "righttoolbarpinned": 0,
        "bottomtoolbarpinned": 0,
        "toolbars_unpinned_last_save": 0,
        "tallnewobj": 0,
        "boxanimatetime": 500,
        "enablehscroll": 1,
        "enablevscroll": 1,
        "devicewidth": 0.0,
        "description": "",
        "digest": "",
        "tags": "",
        "style": "",
        "subpatcher_template": "",
        "boxes": boxes,
        "lines": lines,
        "autosave": 0,
    }
    with open(path, "w") as f:
        json.dump({"patcher": p}, f, indent=1)
    return path


if __name__ == "__main__":
    build_genjit("../device/avc.specreduce.genjit")
    print("ok")
