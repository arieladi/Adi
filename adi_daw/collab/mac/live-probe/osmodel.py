# SPDX-License-Identifier: GPL-3.0-or-later
"""2x up/down with HIIR-style polyphase allpass halfbands (8 coefs, tbw 0.01), float32 state."""
import numpy as np, hiir
COEFS = hiir.coefs(8, 0.01)
class AP1:
    """chain of first-order allpass sections a + z^-1 / 1 + a z^-1, Direct form as in HIIR:
    y = (x - y_prev)*a + x_prev"""
    def __init__(self, cs, dt=np.float32):
        self.c = np.array(cs, dtype=dt); self.x = np.zeros(len(cs), dt); self.y = np.zeros(len(cs), dt); self.dt = dt
    def step(self, v):
        dt = self.dt; v = dt(v)
        for i in range(len(self.c)):
            o = dt(dt(v - self.y[i]) * self.c[i]) + self.x[i]
            self.x[i] = v; self.y[i] = o; v = o
        return v
def chain(x, dt=np.float32, coefs=COEFS, gain_os=None):
    """Upsample (path0 -> first sample, path1 -> second), optional per-os-sample gain, downsample."""
    u0 = AP1(coefs[0::2], dt); u1 = AP1(coefs[1::2], dt)
    d0 = AP1(coefs[0::2], dt); d1 = AP1(coefs[1::2], dt)
    out = np.zeros(len(x), dt)
    bprev = dt(0)
    for n, v in enumerate(x):
        a = u0.step(v); b = u1.step(v)          # os samples 2n (a) and 2n+1 (b)
        if gain_os is not None:
            a = dt(a * gain_os[2 * n]); b = dt(b * gain_os[2 * n + 1])
        # downsampler pairs (2n-1, 2n): path0 <- a_n, path1 <- b_(n-1)
        s0 = d0.step(a); s1 = d1.step(bprev); bprev = b
        out[n] = dt(dt(0.5) * dt(s0 + s1))
    return out
