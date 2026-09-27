"""
Emits avc.capgonio.maxpat and avc.capscope.maxpat -- the two audio-rate capture
voices, loaded by the parent as `poly~ <name> 1`.

WHY THESE ARE SUBPATCHERS AT ALL.  Between them they hold the only audio-rate
Jitter writes in the device: four jit.poke~ objects, each storing one float per
sample per channel, ~176000 matrix writes a second.  They used to run whether
or not anything read them, because MSP gives no way to switch off a single
object -- the DSP chain is either compiled or it is not.  poly~ does have that
switch (`mute 1 1` stops signal processing inside the instance, `mute 1 0`
starts it again), so a voice of their own is the only way to stop paying for a
goniometer nobody is looking at.  A mute~-driven subpatcher would do the same
job; poly~ was chosen because it already takes `args`, which is how the matrix
name gets in here without hard-coding #0 from the parent.

TWO voices rather than one, because the two chains are wanted at different
times: the goniometer draws in every view except the settings screen, the scope
only in OSC.  Splitting them means SPAN mode stops paying for the scope.

INLETS (both)
  1  signal   left  channel, straight off plugin~
  2  signal   right channel
  3  control  see each builder

ARGUMENT  #1 = the matrix to write.  The parent allocates and reads it; nothing
comes back out of the voice, which is why neither patcher has an outlet.
"""

from maxpat import Patcher

HEAD = ("%s -- audio-rate matrix writes, muted as a unit whenever nothing "
        "reads them; see applyMute() in avc.engine.js.")


def _shell(title):
    p = Patcher(rect=(80, 100, 720, 460), is_subpatcher=True)
    p.comment(HEAD % title, 20, 14, fontsize=11)
    inL = p.obj("in~ 1", 30, 50, nin=0, nout=1)
    inR = p.obj("in~ 2", 110, 50, nin=0, nout=1)
    ctl = p.obj("in 3", 330, 50, nin=0, nout=1)
    return p, inL, inR, ctl


def _index(p, ph, y):
    """phasor~ -> two write indices one cell apart, plus the constant y."""
    a = p.obj("*~ 1024.", 30, y)
    p.connect(ph, 0, a, 0)
    b = p.obj("+~ 1.", 210, y)
    p.connect(a, 0, b, 0)
    return a, b, p.obj("sig~ 0", 390, y)


def build_gonio():
    p, inL, inR, ctl = _shell("avc.capgonio")
    p.comment("`gsr <hz>` sets the sweep: one cycle per 512 samples at the "
              "running sample rate, so idxL steps by exactly 2 and L/R land in "
              "adjacent cells of one plane.  At a FIXED frequency the step is "
              "not an integer at any other rate and the pairing drifts apart.",
              20, 100, fontsize=11)
    rt = p.obj("route gsr", 330, 80)
    p.connect(ctl, 0, rt, 0)
    ph = p.obj("phasor~ 86.1328125", 30, 140, nin=2)
    p.connect(rt, 0, ph, 0)
    gL, gR, gz = _index(p, ph, 172)
    for i, (src, idx) in enumerate(((inL, gL), (inR, gR))):
        pk = p.obj("jit.poke~ #1 2 0", 30 + i * 190, 206, nin=3)
        p.connect(src, 0, pk, 0)
        p.connect(idx, 0, pk, 1)
        p.connect(gz, 0, pk, 2)
    return p


def build_scope():
    """
    Three band envelopes of the mono sum, plus a raw trace for the fast scope.

    THE BANDS are what makes the DJ view readable: colouring a waveform by where
    its energy sits is how Serato and Rekordbox let you see a kick, a vocal and
    a hat as different things at a glance, rather than as one green blob.  The
    split is deliberately crude, because the eye is reading proportions and not
    filter design: svf~ gives a lowpass and a highpass from one object each, and
    the middle is whatever is left over.  Complementary subtraction is not
    phase-perfect, but these are envelopes -- nothing is ever summed back.

    LAYOUT is four contiguous blocks in one plane rather than interleaved
    cells: 0..511 the TRUE envelope, then low, mid and high.  The height of the
    waveform comes from that first block and the colour from the proportions of
    the other three, which is how Serato and Rekordbox do it.  Summing the three
    bands for the height does not work: on a loud master each band peaks near
    full scale on its own, the sum runs to two or three, every column clips to
    the top of the plot and the display becomes a solid wall of colour with no
    dynamics left in it at all.  One jit.spill reads the
    whole thing and the drawing layer slices it, so there is no second spill to
    race with the first -- which is the bug that made the goniometer show mono
    and wide as the same picture.

    THE RAW TRACE is separate and much faster: its own phasor~ at a fixed rate
    with no transport locking, writing sample values rather than an envelope,
    which is what an oscilloscope is.
    """
    p, inL, inR, ctl = _shell("avc.capscope")
    p.comment("`phase 0` restarts the bar sweep.  The three band envelopes use "
              "abs~ into an instant-attack slow-decay slide~, which leaves each "
              "cell holding close to its group's PEAK -- that is what keeps "
              "transients from falling between write positions.",
              20, 100, fontsize=11)
    rt = p.obj("route phase", 330, 80)
    p.connect(ctl, 0, rt, 0)

    mono = p.obj("+~", 30, 130, nin=2)
    p.connect(inL, 0, mono, 0)
    p.connect(inR, 0, mono, 1)
    half = p.obj("*~ 0.5", 30, 156)
    p.connect(mono, 0, half, 0)

    ph = p.obj("phasor~ 1n", 250, 130, nin=2)      # one bar, transport-locked
    p.connect(rt, 0, ph, 1)                        # right inlet = phase
    col = p.obj("*~ 512.", 250, 156)
    p.connect(ph, 0, col, 0)
    zero = p.obj("sig~ 0", 600, 156)

    lo = p.obj("svf~ 200. 0.5", 30, 190, nin=3, nout=4)
    p.connect(half, 0, lo, 0)
    hi = p.obj("svf~ 4000. 0.5", 220, 190, nin=3, nout=4)
    p.connect(half, 0, hi, 0)
    # what neither filter claimed is the middle
    m1 = p.obj("-~", 410, 220, nin=2)
    p.connect(half, 0, m1, 0)
    p.connect(lo, 0, m1, 1)                        # outlet 0 = lowpass
    m2 = p.obj("-~", 410, 246, nin=2)
    p.connect(m1, 0, m2, 0)
    p.connect(hi, 1, m2, 1)                        # outlet 1 = highpass

    # 0..511 envelope, then low, mid, high
    for i, (src, out, off) in enumerate(((half, 0, 0), (lo, 0, 512),
                                         (m2, 0, 1024), (hi, 1, 1536))):
        ab = p.obj("abs~", 30 + i * 190, 290)
        p.connect(src, out, ab, 0)
        # 800 samples, ~18 ms.  288 was tuned when this drew a single filled
        # PATH, where the line between two points hides how spiky the samples
        # are.  The DJ view draws each column as its own bar, which hides
        # nothing: at one bar over 512 columns a column is ~3 ms, so a 6 ms
        # decay had each cell holding whatever the envelope happened to be at
        # the end of its column rather than the column's peak, and the result
        # read as a comb of loose vertical lines instead of a waveform.
        env = p.obj("slide~ 1 800", 30 + i * 190, 316)
        p.connect(ab, 0, env, 0)
        idx = col
        if off:
            idx = p.obj("+~ %d." % off, 30 + i * 190, 342)
            p.connect(col, 0, idx, 0)
        pk = p.obj("jit.poke~ #1 2 0", 30 + i * 190, 368, nin=3)
        p.connect(env, 0, pk, 0)
        p.connect(idx, 0, pk, 1)
        p.connect(zero, 0, pk, 2)

    # ---- the fast scope --------------------------------------------------
    p.comment("RAW TRACE for the fast scope: a free-running sweep and the "
              "sample values themselves, not an envelope.  25 Hz is a ~40 ms "
              "window, short enough to see the waveform rather than its shape.",
              20, 410, fontsize=11)
    rph = p.obj("phasor~ 25.", 30, 440, nin=2)
    rcol = p.obj("*~ 512.", 30, 466)
    p.connect(rph, 0, rcol, 0)
    rpk = p.obj("jit.poke~ #2 2 0", 30, 492, nin=3)
    p.connect(half, 0, rpk, 0)
    p.connect(rcol, 0, rpk, 1)
    p.connect(zero, 0, rpk, 2)
    return p


if __name__ == "__main__":
    build_gonio().save_maxpat("../device/avc.capgonio.maxpat")
    build_scope().save_maxpat("../device/avc.capscope.maxpat")
    print("ok")
