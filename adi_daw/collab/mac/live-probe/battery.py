# SPDX-License-Identifier: GPL-3.0-or-later
"""Probe batteries. Each battery -> one .als + manifest JSON (track -> signal + params)."""
import json, os, numpy as np, als
from paths import WORK
SR = als.SR
HERE = WORK

def sig(name, x):
    p = os.path.join(HERE, "sig", name + ".wav")
    if not os.path.exists(p):
        als.wav_write(p, x)
    return p

def impulse(amp=0.5, secs=1.0, at=0.25, ch="both"):
    x = np.zeros((int(secs * SR), 2)); n = int(at * SR)
    if ch in ("both", "L"): x[n, 0] = amp
    if ch in ("both", "R"): x[n, 1] = amp
    return x

def dc_stairs(lo=-84, hi=6, step=2, dur=0.15, lead=0.1):
    lv = np.arange(lo, hi + 1e-9, step)
    n = int(dur * SR); x = np.zeros(int(lead * SR) + n * len(lv))
    for i, l in enumerate(lv):
        x[int(lead * SR) + i * n: int(lead * SR) + (i + 1) * n] = 10 ** (l / 20)
    return np.stack([x, x], 1), lv

def dc_step(a_db, b_db, t1=0.5, t2=1.5, secs=3.0, base=None):
    x = np.full(int(secs * SR), 10 ** (a_db / 20))
    x[int(t1 * SR):int(t2 * SR)] = 10 ** (b_db / 20)
    return np.stack([x, x], 1)

def build(name, items):
    b = als.SetBuilder(); man = {}
    for it in items:
        tid = b.add(it["track"], it["wav"], it.get("params", {}), sidechain=it.get("sidechain"),
                    device=it.get("device", True), automation=it.get("automation"))
        man[it["track"]] = dict(wav=os.path.relpath(it["wav"], HERE), params=it.get("params", {}),
                               sidechain=it.get("sidechain"), device=it.get("device", True), id=tid,
                               automation=it.get("automation"))
    path = os.path.join(HERE, "sets", name + ".als")
    b.save(path)
    json.dump(man, open(os.path.join(HERE, "sets", name + ".json"), "w"), indent=1)
    return path

SINGLE = dict(SplitLowMidOn=False, SplitMidHighOn=False)
FAST = dict(AttackMid=0.1, ReleaseMid=0.1)

def battery_a():
    imp = sig("imp05", impulse())
    impL = sig("imp05_L", impulse(ch="L"))
    imp_small = sig("imp1e-4", impulse(1e-4))
    items = [dict(track="n_neutral", wav=imp)]
    for b in ("Low", "Mid", "High"):
        items.append(dict(track=f"n_solo{b}", wav=imp, params={f"Solo{b}": True}))
    items.append(dict(track="x500_2k", wav=imp, params=dict(SplitLowMid=500.0, SplitMidHigh=2000.0)))
    for b in ("Low", "Mid", "High"):
        items.append(dict(track=f"x500_2k_solo{b}", wav=imp, params=dict(SplitLowMid=500.0, SplitMidHigh=2000.0, **{f"Solo{b}": True})))
    items += [
        dict(track="lowbtn_off", wav=imp, params=dict(SplitLowMidOn=False)),
        dict(track="highbtn_off", wav=imp, params=dict(SplitMidHighOn=False)),
        dict(track="single", wav=imp, params=SINGLE),
        dict(track="lowbtn_off_soloHigh", wav=imp, params=dict(SplitLowMidOn=False, SoloHigh=True)),
        dict(track="lowbtn_off_soloMid", wav=imp, params=dict(SplitLowMidOn=False, SoloMid=True)),
        dict(track="lowbtn_off_soloLow", wav=imp, params=dict(SplitLowMidOn=False, SoloLow=True)),
        dict(track="gainLow6_inMidm6_out3", wav=imp, params=dict(GainLow=6.0, InputGainMid=-6.0, OutputGain=3.0)),
        dict(track="gainHigh6_inactive", wav=imp, params=dict(GainHigh=6.0, InputGainHigh=6.0, ActiveHigh=False)),
        dict(track="gainMid6_inactive", wav=imp, params=dict(GainMid=6.0, ActiveMid=False)),
        dict(track="solo_low_mid", wav=imp, params=dict(SoloLow=True, SoloMid=True)),
        dict(track="x30_300", wav=imp, params=dict(SplitLowMid=30.0, SplitMidHigh=300.0)),
        dict(track="x3000_15000", wav=imp, params=dict(SplitLowMid=3000.0, SplitMidHigh=15000.0)),
        dict(track="xcross_3000_300", wav=imp, params=dict(SplitLowMid=3000.0, SplitMidHigh=300.0)),
        dict(track="xcross_3000_300_soloMid", wav=imp, params=dict(SplitLowMid=3000.0, SplitMidHigh=300.0, SoloMid=True)),
        dict(track="xeq_1000_1000", wav=imp, params=dict(SplitLowMid=1000.0, SplitMidHigh=1000.0)),
        dict(track="n_small", wav=imp_small),
        dict(track="n_L_only", wav=impL),
        dict(track="single_dcstep", wav=sig("dc_m12_step", dc_step(-120, -12, 0.25, 1.25, 2.0))),
        dict(track="neutral_dcstep", wav=sig("dc_m12_step", dc_step(-120, -12, 0.25, 1.25, 2.0))),
    ]
    stairs, lv = dc_stairs()
    st = sig("dc_stairs", stairs)
    def stat(t, **p):
        d = dict(SINGLE); d.update(FAST); d.update(p); items.append(dict(track=t, wav=st, params=d))
    stat("st_above-20_r-0.75_rms", AboveThresholdMid=-20.0, AboveRatioMid=-0.75)
    stat("st_above-20_r-0.75_peak", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, EnvelopeIsPeak=True)
    stat("st_above-20_r-0.75_peak_knee", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, EnvelopeIsPeak=True, SoftKnee=True)
    stat("st_above-20_r-0.75_peak_amt0.5", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, EnvelopeIsPeak=True, GlobalAmount=0.5)
    stat("st_above-20_r-0.5_peak", AboveThresholdMid=-20.0, AboveRatioMid=-0.5, EnvelopeIsPeak=True)
    stat("st_above-20_r-1_peak", AboveThresholdMid=-20.0, AboveRatioMid=-1.0, EnvelopeIsPeak=True)
    stat("st_above-20_r0.5_peak", AboveThresholdMid=-20.0, AboveRatioMid=0.5, EnvelopeIsPeak=True)
    stat("st_above-20_r1_peak", AboveThresholdMid=-20.0, AboveRatioMid=1.0, EnvelopeIsPeak=True)
    stat("st_below-50_r-1_peak", BelowThresholdMid=-50.0, BelowRatioMid=-1.0, EnvelopeIsPeak=True)
    stat("st_below-50_r-3_peak", BelowThresholdMid=-50.0, BelowRatioMid=-3.0, EnvelopeIsPeak=True)
    stat("st_below-50_r0.5_peak", BelowThresholdMid=-50.0, BelowRatioMid=0.5, EnvelopeIsPeak=True)
    stat("st_below-50_r1_peak", BelowThresholdMid=-50.0, BelowRatioMid=1.0, EnvelopeIsPeak=True)
    stat("st_below-50_r0.5_peak_knee", BelowThresholdMid=-50.0, BelowRatioMid=0.5, EnvelopeIsPeak=True, SoftKnee=True)
    stat("st_both_peak", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, BelowThresholdMid=-50.0, BelowRatioMid=0.5, EnvelopeIsPeak=True)
    stat("st_ingain6_out-3_peak", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, InputGainMid=6.0, GainMid=-3.0, EnvelopeIsPeak=True)
    # ballistics (DC steps), single band
    stp = sig("dc_step_m40_m6", dc_step(-40, -6))
    def bal(t, **p):
        d = dict(SINGLE); d.update(dict(AboveThresholdMid=-20.0, AboveRatioMid=-0.75)); d.update(p)
        items.append(dict(track=t, wav=stp, params=d))
    bal("bal_a10_r100_peak", AttackMid=10.0, ReleaseMid=100.0, EnvelopeIsPeak=True)
    bal("bal_a10_r100_rms", AttackMid=10.0, ReleaseMid=100.0)
    bal("bal_a1_r50_peak", AttackMid=1.0, ReleaseMid=50.0, EnvelopeIsPeak=True)
    bal("bal_a10_r100_peak_time2", AttackMid=10.0, ReleaseMid=100.0, EnvelopeIsPeak=True, GlobalTime=2.0)
    bal("bal_a100_r1000_peak", AttackMid=100.0, ReleaseMid=1000.0, EnvelopeIsPeak=True)
    bal("bal_a0.1_r0.1_peak", AttackMid=0.1, ReleaseMid=0.1, EnvelopeIsPeak=True)
    bal("bal_a0.1_r0.1_rms", AttackMid=0.1, ReleaseMid=0.1)
    return build("mbd_a", items), lv

if __name__ == "__main__" and len(__import__("sys").argv) == 1:
    p, lv = battery_a(); print(p)

def sine(f, db, secs, sr=SR, phase=0.0):
    t = np.arange(int(secs * sr)) / sr
    return 10 ** (db / 20) * np.sin(2 * np.pi * f * t + phase)

def stereo(l, r):
    n = max(len(l), len(r)); x = np.zeros((n, 2)); x[:len(l), 0] = l; x[:len(r), 1] = r; return x

def battery_b():
    items = []
    imp = sig("imp05", impulse())
    C = dict(AboveThresholdMid=-20.0, AboveRatioMid=-0.75)
    # 1. DC gain anomaly
    dc12 = sig("dc_m12_const", np.full((int(2 * SR), 2), 10 ** (-12 / 20)))
    stp12 = sig("dc_m12_step", dc_step(-120, -12, 0.25, 1.25, 2.0))
    items += [dict(track="dc_nodev", wav=stp12, device=False),
              dict(track="dc_def_rms", wav=stp12, params=dict(SINGLE)),
              dict(track="dc_def_peak", wav=stp12, params=dict(SINGLE, EnvelopeIsPeak=True)),
              dict(track="dc_fast_rms", wav=stp12, params=dict(SINGLE, **FAST)),
              dict(track="dc_a10_r0.1", wav=stp12, params=dict(SINGLE, AttackMid=10.0, ReleaseMid=0.1)),
              dict(track="dc_a0.1_r100", wav=stp12, params=dict(SINGLE, AttackMid=0.1, ReleaseMid=100.0)),
              dict(track="dc_const_def", wav=dc12, params=dict(SINGLE)),
              dict(track="dc_const_nodev", wav=dc12, device=False),
              dict(track="dc_def_belowm120", wav=stp12, params=dict(SINGLE, BelowThresholdMid=-80.0)),
              dict(track="dc_def_thr_m60", wav=stp12, params=dict(SINGLE, AboveThresholdMid=-60.0, BelowThresholdMid=-70.0))]
    for db in (-60, -40, -1):
        items.append(dict(track=f"dc_step_def_{db}", wav=sig(f"dc_step_{db}", dc_step(-120, db, 0.25, 1.25, 2.0)), params=dict(SINGLE)))
    s1k = sig("sine1k_m12", np.stack([sine(1000, -12, 2.0)] * 2, 1))
    items += [dict(track="sine1k_def", wav=s1k, params=dict(SINGLE)), dict(track="sine1k_nodev", wav=s1k, device=False)]
    # 2. crossed / equal splits
    for b in ("Low", "High"):
        items.append(dict(track=f"xcross_solo{b}", wav=imp, params=dict(SplitLowMid=3000.0, SplitMidHigh=300.0, **{f"Solo{b}": True})))
    for b in ("Low", "Mid", "High"):
        items.append(dict(track=f"x30_300_solo{b}", wav=imp, params=dict(SplitLowMid=30.0, SplitMidHigh=300.0, **{f"Solo{b}": True})))
        items.append(dict(track=f"x3000_15000_solo{b}", wav=imp, params=dict(SplitLowMid=3000.0, SplitMidHigh=15000.0, **{f"Solo{b}": True})))
        items.append(dict(track=f"x1000_8000_solo{b}", wav=imp, params=dict(SplitLowMid=1000.0, SplitMidHigh=8000.0, **{f"Solo{b}": True})))
    items.append(dict(track="highbtn_off_soloLow", wav=imp, params=dict(SplitMidHighOn=False, SoloLow=True)))
    items.append(dict(track="highbtn_off_soloMid", wav=imp, params=dict(SplitMidHighOn=False, SoloMid=True)))
    # 3. stereo link
    lr = sig("dc_L-6_R-40_step", stereo(dc_step(-40, -6)[:, 0], dc_step(-40, -40)[:, 0]))
    items += [dict(track="link_peak", wav=lr, params=dict(SINGLE, EnvelopeIsPeak=True, **C)),
              dict(track="link_rms", wav=lr, params=dict(SINGLE, **C))]
    # 4. sidechain
    scsrc = sig("sc_src_dcstep", dc_step(-60, -6))
    main30 = sig("dc_m30_const", np.full((int(3 * SR), 2), 10 ** (-30 / 20)))
    src_id = 100001 + len(items)
    items.append(dict(track="sc_source", wav=scsrc, device=False))
    for nm, g, dw in (("sc_dw1", 1.0, 1.0), ("sc_dw0.5", 1.0, 0.5), ("sc_dw0", 1.0, 0.0), ("sc_g2", 2.0, 1.0), ("sc_g0.5_dw0.25", 0.5, 0.25)):
        items.append(dict(track=nm, wav=main30, params=dict(SINGLE, EnvelopeIsPeak=True, **C), sidechain=dict(track_id=src_id, gain=g, drywet=dw)))
    items.append(dict(track="sc_listen", wav=main30, params=dict(SINGLE, EnvelopeIsPeak=True, **C), sidechain=dict(track_id=src_id, gain=2.0, drywet=1.0, listen=True)))
    items.append(dict(track="sc_listen_dw0.5", wav=main30, params=dict(SINGLE, EnvelopeIsPeak=True, **C), sidechain=dict(track_id=src_id, gain=1.0, drywet=0.5, listen=True)))
    # sidechain band-split detection: source = 5 kHz sine bursts, main = 3 tones
    sc5k_x = np.zeros(int(3 * SR)); sc5k_x[int(0.5 * SR):int(1.5 * SR)] = sine(5000, -6, 1.0)
    sc5k = sig("sc_src_5k_burst", np.stack([sc5k_x] * 2, 1))
    tones = sine(60, -30, 3.0) + sine(1000, -30, 3.0) + sine(8000, -30, 3.0)
    tw = sig("tones_60_1k_8k_m30", np.stack([tones] * 2, 1))
    src2 = 100001 + len(items)
    items.append(dict(track="sc_source_5k", wav=sc5k, device=False))
    P3 = dict(EnvelopeIsPeak=True)
    for b in ("Low", "Mid", "High"):
        P3.update({f"AboveThreshold{b}": -20.0, f"AboveRatio{b}": -0.75})
    items.append(dict(track="sc_bands_5k", wav=tw, params=P3, sidechain=dict(track_id=src2, gain=1.0, drywet=1.0)))
    items.append(dict(track="sc_bands_5k_listen", wav=tw, params=P3, sidechain=dict(track_id=src2, gain=1.0, drywet=1.0, listen=True)))
    # 5. RMS vs peak with sines, and multiband independence
    lv = np.arange(-40, 0.1, 2.0); seg = int(0.15 * SR); lead = int(0.1 * SR)
    def sine_stairs(f):
        x = np.zeros(lead + seg * len(lv)); t = np.arange(len(x)) / SR
        env = np.zeros(len(x))
        for i, l in enumerate(lv): env[lead + i * seg: lead + (i + 1) * seg] = 10 ** (l / 20)
        return np.stack([env * np.sin(2 * np.pi * f * t)] * 2, 1)
    ss = sig("sine1k_stairs_m40_0", sine_stairs(1000.0))
    for nm, pk in (("sst_rms", False), ("sst_peak", True)):
        for a, r in ((0.1, 0.1), (10.0, 100.0), (1.0, 1000.0)):
            items.append(dict(track=f"{nm}_a{a}_r{r}", wav=ss, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=a, ReleaseMid=r, **C)))
    tones3 = sine(60, -10, 3.0) + sine(1000, -30, 3.0) + sine(8000, -16, 3.0)
    t3 = sig("tones3_m10_m30_m16", np.stack([tones3] * 2, 1))
    items.append(dict(track="mb_indep", wav=t3, params=P3))
    # 6. upward gain limits near silence
    stairs_lo, _ = dc_stairs(-160, -60, 4, 0.15)
    sl = sig("dc_stairs_m160_m60", stairs_lo)
    items += [dict(track="up_r1_bm40", wav=sl, params=dict(SINGLE, BelowThresholdMid=-40.0, BelowRatioMid=1.0, EnvelopeIsPeak=True, **FAST)),
              dict(track="up_r0.5_bm40", wav=sl, params=dict(SINGLE, BelowThresholdMid=-40.0, BelowRatioMid=0.5, EnvelopeIsPeak=True, **FAST)),
              dict(track="up_r1_bm40_rms", wav=sl, params=dict(SINGLE, BelowThresholdMid=-40.0, BelowRatioMid=1.0, **FAST)),
              dict(track="dn_r-3_bm40", wav=sl, params=dict(SINGLE, BelowThresholdMid=-40.0, BelowRatioMid=-3.0, EnvelopeIsPeak=True, **FAST)),
              dict(track="upexp_r1_am100", wav=sl, params=dict(SINGLE, AboveThresholdMid=-80.0, AboveRatioMid=1.0, EnvelopeIsPeak=True, **FAST))]
    # 7. threshold relationships and knees (DC stairs -84..+6)
    st = sig("dc_stairs", dc_stairs()[0])
    def stat(t, **p):
        d = dict(SINGLE); d.update(FAST); d.update(EnvelopeIsPeak=True); d.update(p); items.append(dict(track=t, wav=st, params=d))
    stat("thr_cross_a-40_b-20", AboveThresholdMid=-40.0, AboveRatioMid=-0.75, BelowThresholdMid=-20.0, BelowRatioMid=0.5)
    stat("thr_cross_a-40_b-20_knee", AboveThresholdMid=-40.0, AboveRatioMid=-0.75, BelowThresholdMid=-20.0, BelowRatioMid=0.5, SoftKnee=True)
    stat("thr_close_a-20_b-30_knee", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, BelowThresholdMid=-30.0, BelowRatioMid=0.5, SoftKnee=True)
    stat("thr_close_a-20_b-30_knee_dn", AboveThresholdMid=-20.0, AboveRatioMid=-0.75, BelowThresholdMid=-30.0, BelowRatioMid=-1.0, SoftKnee=True)
    stat("thr_equal_a-30_b-30", AboveThresholdMid=-30.0, AboveRatioMid=-0.5, BelowThresholdMid=-30.0, BelowRatioMid=-1.0)
    stat("thr_equal_a-30_b-30_knee", AboveThresholdMid=-30.0, AboveRatioMid=-0.5, BelowThresholdMid=-30.0, BelowRatioMid=-1.0, SoftKnee=True)
    stat("amt0_r-1", AboveThresholdMid=-20.0, AboveRatioMid=-1.0, GlobalAmount=0.0)
    stat("amt0.25_below_r-3", BelowThresholdMid=-40.0, BelowRatioMid=-3.0, GlobalAmount=0.25)
    stat("amt0.5_knee", AboveThresholdMid=-20.0, AboveRatioMid=-1.0, GlobalAmount=0.5, SoftKnee=True)
    stat("knee_a0_r-0.75", AboveThresholdMid=0.0, AboveRatioMid=-0.75, SoftKnee=True)
    stat("knee_b-80_r-1", BelowThresholdMid=-80.0, BelowRatioMid=-1.0, SoftKnee=True)
    stat("out_global_6", OutputGain=6.0)
    stat("out_global_m24_in24", OutputGain=-24.0, InputGainMid=24.0, AboveThresholdMid=-20.0, AboveRatioMid=-0.5)
    # 8. attack/release mapping (DC steps -40 <-> -6, single band, threshold -20, 4:1)
    stp = sig("dc_step_m40_m6", dc_step(-40, -6))
    stpl = sig("dc_step_m40_m6_long", dc_step(-40, -6, 0.5, 6.5, 14.0))
    for pk in (True, False):
        tag = "pk" if pk else "rms"
        for a in (0.1, 0.3, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0, 1000.0):
            items.append(dict(track=f"att_{tag}_{a}", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=a, ReleaseMid=1.0, **C)))
        for r in (0.1, 0.3, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0, 1000.0):
            items.append(dict(track=f"rel_{tag}_{r}", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=0.1, ReleaseMid=r, **C)))
        items.append(dict(track=f"att_{tag}_5000", wav=stpl, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=5000.0, ReleaseMid=1.0, **C)))
        items.append(dict(track=f"rel_{tag}_5000", wav=stpl, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=0.1, ReleaseMid=5000.0, **C)))
        for tm in (0.1, 0.5, 2.0, 10.0):
            items.append(dict(track=f"time_{tag}_{tm}", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=10.0, ReleaseMid=100.0, GlobalTime=tm, **C)))
    # below-threshold ballistics (expander): step -6 -> -60 -> -6 with below -40, ratio -1
    stpd = sig("dc_step_m6_m60", dc_step(-6, -60))
    for pk in (True, False):
        tag = "pk" if pk else "rms"
        items.append(dict(track=f"exp_bal_{tag}", wav=stpd, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=10.0, ReleaseMid=100.0, BelowThresholdMid=-40.0, BelowRatioMid=-1.0)))
        items.append(dict(track=f"upc_bal_{tag}", wav=stpd, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=10.0, ReleaseMid=100.0, BelowThresholdMid=-40.0, BelowRatioMid=0.5)))
        items.append(dict(track=f"upx_bal_{tag}", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=10.0, ReleaseMid=100.0, AboveThresholdMid=-20.0, AboveRatioMid=0.5)))
    # 9. oversampling factor: 15 kHz sine, fast heavy compression
    s15 = sig("sine15k_m6", np.stack([sine(15000, -6, 2.0)] * 2, 1))
    s19 = sig("sine19k_m6", np.stack([sine(19000, -6, 2.0)] * 2, 1))
    for nm, w in (("os15k", s15), ("os19k", s19)):
        items.append(dict(track=nm + "_fastcomp", wav=w, params=dict(SINGLE, EnvelopeIsPeak=True, AttackMid=0.1, ReleaseMid=0.1, AboveThresholdMid=-40.0, AboveRatioMid=-1.0)))
        items.append(dict(track=nm + "_nodev", wav=w, device=False))
    # 10. three-band ballistics independence with a 60 Hz burst
    burst = np.zeros(int(3 * SR)); burst[int(0.5 * SR):int(1.5 * SR)] = 1.0
    tb = sig("tones_lowburst", np.stack([sine(60, -40, 3.0) * (1 + 30 * burst) + sine(1000, -30, 3.0) + sine(8000, -30, 3.0)] * 2, 1))
    items.append(dict(track="mb_lowburst", wav=tb, params=P3))
    return build("mbd_b", items)

if __name__ == "__main__" and len(__import__("sys").argv) > 1 and __import__("sys").argv[1] == "b":
    print(battery_b())

def fine_stairs(lo, hi, step, dur, lead=0.05):
    lv = np.arange(lo, hi + 1e-9, step); n = int(dur * SR); x = np.zeros(int(lead * SR) + n * len(lv))
    for i, l in enumerate(lv):
        x[int(lead * SR) + i * n: int(lead * SR) + (i + 1) * n] = 10 ** (l / 20)
    return np.stack([x, x], 1), lv

def battery_c():
    items = []
    imp = sig("imp05", impulse())
    C = dict(AboveThresholdMid=-20.0, AboveRatioMid=-0.75)
    # C1 approximation sweeps
    fs_, lv = fine_stairs(-30.0, -18.0, 0.02, 0.012)
    fw = sig("dc_fine_m30_m18", fs_)
    def fine(t, **p):
        d = dict(SINGLE); d.update(FAST); d.update(EnvelopeIsPeak=True); d.update(p); items.append(dict(track=t, wav=fw, params=d))
    fine("fine_a-60_r-1", AboveThresholdMid=-60.0, AboveRatioMid=-1.0)
    fine("fine_a-60_r-0.5", AboveThresholdMid=-60.0, AboveRatioMid=-0.5)
    fine("fine_a-31_r1", AboveThresholdMid=-31.0, AboveRatioMid=1.0)
    fine("fine_b0_r1", BelowThresholdMid=0.0, BelowRatioMid=1.0)
    fine("fine_b0_r0.3", BelowThresholdMid=0.0, BelowRatioMid=0.3)
    fine("fine_a-60_r-1_rms", AboveThresholdMid=-60.0, AboveRatioMid=-1.0, EnvelopeIsPeak=False)
    fine("fine_a-60_r-1_amt0.5", AboveThresholdMid=-60.0, AboveRatioMid=-1.0, GlobalAmount=0.5)
    fine("fine_a-60.3_r-0.77", AboveThresholdMid=-60.3, AboveRatioMid=-0.77)
    fine("fine_gain_only_in3.3_out-1.7", InputGainMid=3.3, GainMid=-1.7)
    fine("fine_out_global_-2.9", OutputGain=-2.9)
    # C2 polarity
    def dcc(db): return np.full(int(2.0 * SR), 10 ** (db / 20))
    opp = sig("dc_opp_m6", stereo(dcc(-6), -dcc(-6)))
    lr2 = sig("dc_L-6_R-12", stereo(dcc(-6), dcc(-12)))
    lr3 = sig("dc_L-6_Rneg-12", stereo(dcc(-6), -dcc(-12)))
    for nm, w in (("pol_opp", opp), ("pol_L6_R12", lr2), ("pol_L6_Rn12", lr3)):
        items.append(dict(track=nm + "_pk", wav=w, params=dict(SINGLE, EnvelopeIsPeak=True, **C)))
        items.append(dict(track=nm + "_rms", wav=w, params=dict(SINGLE, **C)))
    # C3 crossed splits
    for fl, fh in ((2000.0, 1000.0), (1000.0, 999.0), (500.0, 500.0), (600.0, 400.0), (3000.0, 2999.0)):
        for b in ("Low", "Mid", "High"):
            items.append(dict(track=f"xc_{int(fl)}_{int(fh)}_solo{b}", wav=imp, params=dict(SplitLowMid=fl, SplitMidHigh=fh, **{f"Solo{b}": True})))
    # C8 solo / active combos
    items += [dict(track="solo_inactive_mid", wav=imp, params=dict(SoloMid=True, ActiveMid=False, GainMid=6.0)),
              dict(track="solo_all", wav=imp, params=dict(SoloLow=True, SoloMid=True, SoloHigh=True)),
              dict(track="lowoff_activeLow_false_gain", wav=imp, params=dict(SplitLowMidOn=False, ActiveLow=False, GainLow=6.0, GainMid=-6.0))]
    # C10 cap
    st2, lv2 = dc_stairs(-140, 6, 4, 0.15)
    s2 = sig("dc_stairs_m140_6", st2)
    def cap(t, **p):
        d = dict(SINGLE); d.update(FAST); d.update(EnvelopeIsPeak=True); d.update(p); items.append(dict(track=t, wav=s2, params=d))
    cap("cap_in24_up", InputGainMid=24.0, BelowThresholdMid=-40.0, BelowRatioMid=1.0)
    cap("cap_out24_up", GainMid=24.0, BelowThresholdMid=-40.0, BelowRatioMid=1.0)
    cap("cap_upexp_a-80_r1", AboveThresholdMid=-80.0, AboveRatioMid=1.0)
    cap("cap_both_up", AboveThresholdMid=-60.0, AboveRatioMid=1.0, BelowThresholdMid=-60.0, BelowRatioMid=1.0)
    cap("cap_in-24_down", InputGainMid=-24.0, AboveThresholdMid=-80.0, AboveRatioMid=-1.0)
    # C11 extreme times
    stp = sig("dc_step_m40_m6", dc_step(-40, -6))
    for pk in (True, False):
        tag = "pk" if pk else "rms"
        items.append(dict(track=f"ext_a0.1_t0.1_{tag}", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=0.1, ReleaseMid=0.1, GlobalTime=0.1, **C)))
        items.append(dict(track=f"ext_a5000_t10_{tag}", wav=sig("dc_step_m40_m6_xl", dc_step(-40, -6, 0.5, 20.5, 25.0)), params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=5000.0, ReleaseMid=5000.0, GlobalTime=10.0, **C)))
        items.append(dict(track=f"att_{tag}_0.15", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=0.15, ReleaseMid=1.0, **C)))
        items.append(dict(track=f"att_{tag}_2", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=2.0, ReleaseMid=1.0, **C)))
        items.append(dict(track=f"att_{tag}_7.3", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=7.3, ReleaseMid=1.0, **C)))
        items.append(dict(track=f"rel_{tag}_7.3", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=0.1, ReleaseMid=7.3, **C)))
        items.append(dict(track=f"rel_{tag}_55", wav=stp, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=0.1, ReleaseMid=55.0, **C)))
    # C13 automation smoothing (beat 2 = 1.0 s at 120 BPM)
    s1k = sig("sine1k_m12_4s", np.stack([sine(1000, -12, 4.0)] * 2, 1))
    dcw = sig("dc_m12_4s", np.full((int(4 * SR), 2), 10 ** (-12 / 20)))
    J = lambda a, b: [(2.0, a), (2.0, b), (6.0, b), (6.0, a)]
    items += [
        dict(track="auto_gainmid", wav=dcw, params=dict(SINGLE), automation={"GainMid": J(0.0, -20.0)}),
        dict(track="auto_ingainmid", wav=dcw, params=dict(SINGLE), automation={"InputGainMid": J(0.0, -20.0)}),
        dict(track="auto_outgain", wav=dcw, params=dict(SINGLE), automation={"OutputGain": J(0.0, -20.0)}),
        dict(track="auto_thresh", wav=dcw, params=dict(SINGLE, AboveRatioMid=-1.0, EnvelopeIsPeak=True, **FAST), automation={"AboveThresholdMid": J(0.0, -32.0)}),
        dict(track="auto_ratio", wav=dcw, params=dict(SINGLE, AboveThresholdMid=-32.0, EnvelopeIsPeak=True, **FAST), automation={"AboveRatioMid": J(0.0, -1.0)}),
        dict(track="auto_amount", wav=dcw, params=dict(SINGLE, AboveThresholdMid=-32.0, AboveRatioMid=-1.0, EnvelopeIsPeak=True, **FAST), automation={"GlobalAmount": J(1.0, 0.0)}),
        dict(track="auto_split", wav=s1k, params=dict(SoloLow=True), automation={"SplitLowMid": J(120.0, 2000.0)}),
        dict(track="auto_attack", wav=dcw, params=dict(SINGLE, AboveThresholdMid=-32.0, AboveRatioMid=-1.0, EnvelopeIsPeak=True), automation={"AttackMid": J(10.0, 1000.0)}),
        dict(track="auto_activemid", wav=dcw, params=dict(SINGLE, GainMid=-20.0), automation={"ActiveMid": [(2.0, 1.0), (2.0, 0.0), (6.0, 0.0), (6.0, 1.0)]}),
        dict(track="auto_gainmid_ramp", wav=dcw, params=dict(SINGLE), automation={"GainMid": [(2.0, 0.0), (4.0, -20.0)]}),
    ]
    if __import__("os").environ.get("AUTO_ONLY"):
        return build("mbd_c2", [it for it in items if it.get("automation")])
    return build("mbd_c", items)

if __name__ == "__main__" and len(__import__("sys").argv) > 1 and __import__("sys").argv[1] == "c":
    print(battery_c())
