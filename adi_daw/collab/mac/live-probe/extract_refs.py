# SPDX-License-Identifier: GPL-3.0-or-later
"""Pull reference numbers out of Live's renders for tests/test_multiband_dynamics.cpp."""
import os, sys, json, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import als, osmodel
from paths import WORK
P = WORK + '/'
def rd(s, n): return als.wav_read(P + f'out/{s}/{n}.wav')[0]
out = {}
# A. single-band neutral impulse response, first 64 samples (0.5 impulse at sample 12000)
h = rd('mbd_a', 'single')[12000:12064, 0].astype(np.float32)
out['single_ir'] = [float(v).hex() for v in h]
# B. band magnitudes (dB) of solo impulse responses
F = [50, 120, 500, 1000, 2500, 5000, 10000]
def mag(s, n):
    x = rd(s, n)[12000:12000 + 16384, 0] / 0.5
    k = np.arange(len(x))
    return [float(20 * np.log10(abs(np.sum(x * np.exp(-2j * np.pi * f * k / 48000))))) for f in F]
out['freqs'] = F
for s, n in [('mbd_a', 'n_soloLow'), ('mbd_a', 'n_soloMid'), ('mbd_a', 'n_soloHigh'), ('mbd_a', 'n_neutral'),
             ('mbd_b', 'x1000_8000_soloLow'), ('mbd_b', 'x1000_8000_soloMid'), ('mbd_b', 'x1000_8000_soloHigh')]:
    out[n] = mag(s, n)
# C. static gains from DC stairs (lead 0.1 s, 0.15 s per 2 dB step from -84)
lv = np.arange(-84, 6 + 1e-9, 2); seg = 7200; lead = 4800
s_in = als.wav_read(P + 'sig/dc_stairs.wav')[0]
def stat(n, levels):
    y = rd('mbd_a' if n.startswith('st_') else 'mbd_b', n); r = []
    for L in levels:
        i = int(round((L + 84) / 2)); a = lead + i * seg + seg // 2; b = lead + (i + 1) * seg - 200
        r.append(float(20 * np.log10(abs(np.mean(y[a:b, 0])) / np.mean(s_in[a:b, 0]))))
    return r
out['static'] = {n: dict(levels=L, gain=stat(n, L)) for n, L in [
    ('st_above-20_r-0.75_peak', [-24, -16, -8, 0, 6]), ('st_above-20_r-0.75_peak_knee', [-30, -24, -20, -16, -8]),
    ('st_below-50_r-3_peak', [-80, -60, -52]), ('st_below-50_r0.5_peak_knee', [-62, -56, -50, -44]),
    ('st_above-20_r1_peak', [-10, 0]), ('thr_cross_a-40_b-20', [-60, -30, -10]),
    ('st_above-20_r-0.75_peak_amt0.5', [-10, 0])]}
# D. ballistics: gain dB = live / neutral resampled input, at given ms after the step
x = als.wav_read(P + 'sig/dc_step_m40_m6.wav')[0][:, 0].astype(np.float32)
ref = osmodel.chain(x[:100000], np.float32).astype(np.float64)
def bal(n, t0, ms):
    y = rd('mbd_b', n)[:100000, 0]
    return [float(20 * np.log10(abs(y[t0 + int(round(t * 48))] / ref[t0 + int(round(t * 48))]))) for t in ms]
out['ballistics'] = {
    'att_pk_10.0': dict(t0=24000, ms=[1, 2, 5, 10], gain=bal('att_pk_10.0', 24000, [1, 2, 5, 10])),
    'att_rms_10.0': dict(t0=24000, ms=[1, 2, 5], gain=bal('att_rms_10.0', 24000, [1, 2, 5])),
    'att_pk_100.0': dict(t0=24000, ms=[5, 10, 20, 50], gain=bal('att_pk_100.0', 24000, [5, 10, 20, 50])),
    'rel_pk_100.0': dict(t0=72000, ms=[5, 10, 15], gain=bal('rel_pk_100.0', 72000, [5, 10, 15])),
    'rel_pk_1000.0': dict(t0=72000, ms=[50, 100, 150], gain=bal('rel_pk_1000.0', 72000, [50, 100, 150])),
}
json.dump(out, open(os.path.join(WORK, 'refs.json'), 'w'), indent=1)
print(json.dumps({k: v for k, v in out.items() if k != 'single_ir'}, indent=0)[:3000])
