# SPDX-License-Identifier: GPL-3.0-or-later
import math, numpy as np
def _num(q, order, c):
    acc, i, j = 0.0, 0, 1
    while True:
        iq = q ** (i * (i + 1)) * math.sin((i * 2 + 1) * c * math.pi / order) * j
        acc += iq; j = -j; i += 1
        if abs(iq) <= 1e-100: return acc
def _den(q, order, c):
    acc, i, j = 0.0, 1, -1
    while True:
        iq = q ** (i * i) * math.cos(i * 2 * c * math.pi / order) * j
        acc += iq; j = -j; i += 1
        if abs(iq) <= 1e-100: return acc
def transition_param(tbw):
    k = math.tan((1 - tbw * 2) * math.pi / 4); k *= k
    kk = (1 - k * k) ** 0.25
    e = 0.5 * (1 - kk) / (1 + kk); e2 = e * e; e4 = e2 * e2
    q = e * (1 + e4 * (2 + e4 * (15 + 150 * e4)))
    return k, q
def coefs(n, tbw):
    k, q = transition_param(tbw); order = 2 * n + 1; out = []
    for idx in range(n):
        c = idx + 1
        num = _num(q, order, c) * q ** 0.25; den = _den(q, order, c) + 0.5
        ww = num / den; w2 = ww * ww
        x = math.sqrt((1 - w2 * k) * (1 - w2 / k)) / (1 + w2)
        out.append((1 - x) / (1 + x))
    return out
def atten(n, tbw):
    k, q = transition_param(tbw); order = 2 * n + 1
    # compute_atten
    a = 0.0
    lam = q
    # use HIIR's formula: atten = -10*log10(... ) approximated via coefficient response instead
    return None
def ap_resp(cs, w):
    z1 = np.exp(-1j * w); h = np.ones_like(z1)
    for a in cs: h *= (a + z1) / (1 + a * z1)
    return h
