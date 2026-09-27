"""
Builds avc.fftanalysis.maxpat -- the pfft~ subpatcher that does all spectral
analysis for one channel ("engine").

Loaded as:
    pfft~ avc.fftanalysis <fftsize> <overlap> 0 0
          args <matrixname> <ctlname> <framesize> <reportname>
        #1 = name of the 5-plane float32 jit.matrix to write into
        #2 = name of the receive used for control messages
        #3 = spectral frame size (N/2), used as vectral~'s creation argument
        #4 = send name the subpatch reports its real frame size back on

THE WINDOW
----------
fftin~ applies it, from the envelope name passed in as #5.  It is a creation
argument, so changing the window rebuilds the engine -- which the parent already
does for block size and overlap, so this costs nothing.

This replaced a 7-tap spectral convolution that took the FFT rectangular and
applied the window afterwards by convolving the complex spectrum.  That is
mathematically exact and was verified to 1e-15 in test/test_spectrum.js, and it
made the window a runtime coefficient change.  It also did not work: measured
against SPAN on a steady 437 Hz sine, the peak matched to 0.6 dB while every
off-peak bin sat a flat +12.5 dB high, and the plot showed the regularly spaced
sinc sidelobes of a RECTANGULAR window.  Switching the window changed the
picture, so the coefficients were arriving and the taps were connected -- the
convolution was running and simply not producing the window it was told to.
Rather than keep hunting inside it, the job is now done by the object whose
documented purpose is to do it.

CALIBRATION.  A real sine of amplitude A at a bin centre gives |X| = A*N*CG/2,
where CG is the window's coherent gain (= a0).  So amplitude = 2|X|/(N*CG);
that scaling is sent in as `gcal`.  DC gets no single-sided x2, applied here as
a x0.5 correction at b==0.

BALLISTICS run in the signal domain, one state per bin, via vectral~ -- so every
FFT frame is seen.  Doing them at UI rate instead would drop 30-80% of frames
and RT Max would miss peaks.  All five analysis types are computed at once and
written to five planes, so switching primary/secondary type is instant and never
restarts an accumulation.

    plane 0  RT Avg     sqrt of one-pole-averaged power
    plane 1  RT Max     instant attack, Avg-Time fall-off
    plane 2  Max        cumulative maximum (one-pole with ~infinite fall)
    plane 3  Avg        cumulative mean power, frameaccum~ / frame count
    plane 4  RT Sigma   sqrt(E[m^2] - E[m]^2), same time constant as RT Avg
"""

from maxpat import Patcher, ARITY

ARITY.update({
    "fftin~": (1, 3), "fftinfo~": (1, 4), "vectral~": (3, 1),
    "frameaccum~": (2, 1), "cartopol~": (2, 2), "delay~": (2, 1),
    "sig~": (1, 1), "clip~": (3, 1), "sqrt~": (1, 1),
    "*~": (2, 1), "+~": (2, 1), "-~": (2, 1), "/~": (2, 1),
    ">=~": (2, 1), "<~": (2, 1), "==~": (2, 1),
    "r": (1, 1), "receive": (1, 1), "s": (1, 0), "send": (1, 0),
    "jit.poke~": (2, 1), "loadbang": (0, 1), "t": (1, 1),
})

# Frequency-domain kernels c = [c0, c1, c2, c3]; c0 = a0, ck = (-1)^k * a_k / 2.
# Coherent gain (mean of w[n]) equals a0 for every cosine-sum window.
WINDOWS = {
    #  name        a0          a1         a2         a3
    "hann":     (0.5,       0.5,       0.0,       0.0),
    "blackman": (0.42,      0.5,       0.08,      0.0),
    "nuttall":  (0.355768,  0.487396,  0.144232,  0.012604),
}


def window_kernel(name):
    a0, a1, a2, a3 = WINDOWS[name]
    return [a0, -a1 / 2.0, a2 / 2.0, -a3 / 2.0], a0   # kernel, coherent gain


def build():
    p = Patcher(rect=(60, 80, 1500, 1000), is_subpatcher=True)
    X = 40      # left margin
    p.comment("avc.fftanalysis -- one spectral analysis engine. "
              "args: #1 = matrix name, #2 = control receive name.",
              X, 10, fontsize=12)
    p.comment("Rectangular FFT; window applied by exact spectral convolution. "
              "See build/fft_analysis.py for the maths.", X, 30, fontsize=10)

    # ---------------------------------------------------------------- input
    # The window is applied by fftin~ itself.  #5 is its envelope name, passed
    # in by the parent, so changing the window rebuilds the engine -- which the
    # parent already does for block size and overlap.
    fftin = p.obj("fftin~ 1 #5", X, 70)
    # The frame size comes from fftinfo~, i.e. the size pfft~ ACTUALLY gave
    # this subpatch -- never from what the parent asked for.  If a creation
    # argument is ignored (pfft~ silently falls back to 512) the parent has to
    # find out, or the reduction maps bins to the wrong frequencies and the
    # whole spectrum shifts.  #4 is the name the parent listens on.
    info = p.obj("fftinfo~", X + 420, 70)
    fsize = p.obj("t i i", X + 420, 100)
    p.connect(info, 1, fsize, 0)     # outlet 1 = spectral frame size (N/2)
    rep_ = p.obj("prepend framesize", X + 620, 132)
    p.connect(fsize, 0, rep_, 0)
    p.connect(rep_, 0, p.obj("s #4", X + 620, 162, nout=0), 0)
    p.comment("report the real frame size back to the parent", X + 760, 162)

    # Bin index, straight from fftin~.  Nothing needs aligning any more: with
    # the convolution gone there is no delayed centre tap to line up with.
    bD = p.obj("+~ 0.", X + 320, 110)
    p.connect(fftin, 2, bD, 0)
    p.comment("bD = FFT bin index", X + 420, 110)

    # ------------------------------------------------------------ magnitude
    c2p = p.obj("cartopol~", X, 770)
    p.connect(fftin, 0, c2p, 0)
    p.connect(fftin, 1, c2p, 1)

    # calibration: gcal * (1 - 0.5*[b==0])
    isdc = p.obj("==~ 0", X + 320, 770)
    p.connect(bD, 0, isdc, 0)
    dchalf = p.obj("*~ -0.5", X + 320, 796)
    p.connect(isdc, 0, dchalf, 0)
    dcfix = p.obj("+~ 1", X + 320, 822)
    p.connect(dchalf, 0, dcfix, 0)

    gcal = p.obj("*~ 1.", X, 800)
    p.connect(c2p, 0, gcal, 0)
    mag = p.obj("*~", X, 830)
    p.connect(gcal, 0, mag, 0)
    p.connect(dcfix, 0, mag, 1)
    p.comment("mag = calibrated amplitude, 1.0 == full-scale sine", X + 120, 830)

    magsq = p.obj("*~", X, 860)
    p.connect(mag, 0, magsq, 0)
    p.connect(mag, 0, magsq, 1)

    # ------------------------------------------------------------ ballistics
    def vectral(x, y, label):
        v = p.obj("vectral~ #3", x, y)
        p.connect(bD, 0, v, 0)      # output index sync
        p.connect(bD, 0, v, 1)      # input index sync
        p.comment(label, x + 130, y)
        return v

    YB = 900
    vE2 = vectral(X, YB, "E[m^2] - RT Avg + Sigma")
    p.connect(magsq, 0, vE2, 2)
    vE1 = vectral(X, YB + 30, "E[m] - Sigma only")
    p.connect(mag, 0, vE1, 2)
    vMax = vectral(X, YB + 60, "RT Max (instant attack)")
    p.connect(mag, 0, vMax, 2)
    vCMax = vectral(X, YB + 90, "cumulative Max (fall ~ infinite)")
    p.connect(mag, 0, vCMax, 2)

    # plane 0: RT Avg = sqrt(E[m^2])
    rtavg = p.obj("sqrt~", X + 340, YB)
    p.connect(vE2, 0, rtavg, 0)

    # plane 4: RT Sigma = sqrt(max(0, E[m^2] - E[m]^2))
    e1sq = p.obj("*~", X + 340, YB + 30)
    p.connect(vE1, 0, e1sq, 0)
    p.connect(vE1, 0, e1sq, 1)
    var = p.obj("-~", X + 340, YB + 56)
    p.connect(vE2, 0, var, 0)
    p.connect(e1sq, 0, var, 1)
    varc = p.obj("clip~ 0. 1000000000.", X + 340, YB + 82)
    p.connect(var, 0, varc, 0)
    rtsig = p.obj("sqrt~", X + 340, YB + 108)
    p.connect(varc, 0, rtsig, 0)

    # plane 3: cumulative Avg = sqrt( accum(m^2) / framecount )
    one = p.obj("sig~ 1", X + 640, YB)
    nacc = p.obj("frameaccum~", X + 640, YB + 26)
    p.connect(one, 0, nacc, 0)
    nclip = p.obj("clip~ 1. 1000000000.", X + 640, YB + 52)
    p.connect(nacc, 0, nclip, 0)
    sacc = p.obj("frameaccum~", X + 800, YB + 26)
    p.connect(magsq, 0, sacc, 0)
    cdiv = p.obj("/~", X + 800, YB + 78)
    p.connect(sacc, 0, cdiv, 0)
    p.connect(nclip, 0, cdiv, 1)
    cavg = p.obj("sqrt~", X + 800, YB + 104)
    p.connect(cdiv, 0, cavg, 0)

    # ------------------------------------------------------------ poke out
    # Matrices are 2D (nbins x 1) throughout the device so that gen's
    # samplepix() coordinates are unambiguous, hence dim_inputcount 2 and a
    # constant 0 for dim[1].
    YP = YB + 190
    zero = p.obj("sig~ 0", X + 1040, YP - 30)
    planes = [(rtavg, 0, "RT Avg"), (vMax, 1, "RT Max"), (vCMax, 2, "Max"),
              (cavg, 3, "Avg"), (rtsig, 4, "RT Sigma")]
    for src, pl, label in planes:
        pk = p.obj("jit.poke~ #1 2 %d" % pl, X + pl * 200, YP, nin=3)
        p.connect(src, 0, pk, 0)
        p.connect(bD, 0, pk, 1)
        p.connect(zero, 0, pk, 2)
        p.comment(label, X + pl * 200, YP + 24)

    # ------------------------------------------------------------- control
    YC = YP + 70
    rc = p.obj("r #2", X, YC)
    rt = p.obj("route gcal slide rtmax cmax clear setup", X, YC + 30)

    p.connect(rc, 0, rt, 0)

    # gcal <f>
    p.connect(rt, 0, gcal, 1)

    # slide <f> -> symmetric one-pole on the two averaging vectral~
    msl = p.msg("slide $1 $1", X + 300, YC + 60)
    p.connect(rt, 1, msl, 0)
    p.connect(msl, 0, vE2, 0)
    p.connect(msl, 0, vE1, 0)

    # rtmax <f> -> instant attack, given fall
    mrm = p.msg("slide 1 $1", X + 470, YC + 60)
    p.connect(rt, 2, mrm, 0)
    p.connect(mrm, 0, vMax, 0)

    # cmax <f> -> cumulative max fall (normally astronomically large)
    mcm = p.msg("slide 1 $1", X + 620, YC + 60)
    p.connect(rt, 3, mcm, 0)
    p.connect(mcm, 0, vCMax, 0)

    # clear -> reset every cumulative accumulator
    mcl = p.msg("clear", X + 770, YC + 60)
    p.connect(rt, 4, mcl, 0)
    for tgt in (vCMax, nacc, sacc):
        p.connect(mcl, 0, tgt, 0)

    # `setup` (outlet 6) and loadbang both re-poll fftinfo~, which configures
    # the edge gates and the vectral~ vector size and reports back upstream.
    lb = p.obj("loadbang", X + 300, YC + 110)
    p.connect(lb, 0, info, 0)
    p.connect(rt, 5, info, 0)

    msz = p.obj("prepend size", X + 300, YC + 140)
    p.connect(fsize, 1, msz, 0)
    for v in (vE2, vE1, vMax, vCMax):
        p.connect(msz, 0, v, 0)
    p.comment("frame size -> edge gates and vectral~ vector size", X + 440, YC + 140)

    return p


if __name__ == "__main__":
    build().save_maxpat("../device/avc.fftanalysis.maxpat")
    print("ok")
