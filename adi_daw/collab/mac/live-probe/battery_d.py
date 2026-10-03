"""Probe battery D (completeness critic): the reachable behaviours batteries A-C leave untested.

  battery_d()          -> sets/mbd_d.als + mbd_d.json   (48 kHz, export like A-C: All Individual Tracks,
                                                          32-bit float, no normalize, no dither)
  battery_d_sr(rate)   -> sets/mbd_dsr44.als / mbd_dsr96.als (export THAT set at 44.1 / 96 kHz; its clips
                                                          are written at that rate so Live does not resample)

Usage: cd $S/probe && python3 <this file> [d | sr44 | sr96 | all]   (add --dry DIR to build into DIR instead)

Notes for the harness:
- Every signal name starts with "d_", so nothing collides with the A-C signals in sig/.
- Switch automation (ActiveX, EnvelopeIsPeak, SoftKnee, On, SideChain/OnOff) must be BoolEvent in the .als:
  battery A-C wrote FloatEvent for everything, and mbd_c2/auto_activemid shows the band inactive for the whole
  render (output 0.000 dB while GainMid = -20), i.e. Live did not read FloatEvent 1.0 as "on".
  fix_bools() rewrites those events after build().  nulltest.py skips automation items; replaying them needs
  automation support in mbd_render (step for bools at the event time, Live's ~4 ms S-curve for floats).
- Sidechain sources are device-less tracks; their id is 100001 + their index in items (battery_b's rule).
"""
import gzip, math, os, sys
import xml.etree.ElementTree as ET
import numpy as np

PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)
import als  # noqa: E402
import battery as B  # noqa: E402
from battery import SINGLE, FAST  # noqa: E402

SR = B.SR
C = dict(AboveThresholdMid=-20.0, AboveRatioMid=-0.75)  # the A-C single-band compressor
P3 = dict(EnvelopeIsPeak=True)  # the sc_bands_5k three-band compressor
for _b in ("Low", "Mid", "High"):
    P3.update({f"AboveThreshold{_b}": -20.0, f"AboveRatio{_b}": -0.75})


def M(*ds, **kw):
    """merge several param dicts (dict() takes only one positional mapping)"""
    d = {}
    for x in ds:
        d.update(x)
    d.update(kw)
    return d


def sig(name, x):
    return B.sig("d_" + name, x)


def secs(s, sr=SR):
    return int(round(s * sr))


def noise(db, n_secs, seed, sr=SR):
    return 10 ** (db / 20) * np.random.default_rng(seed).standard_normal(secs(n_secs, sr))


def segs(levels, sr=SR):
    """piecewise DC: [(dB or None for silence, seconds), ...] -> mono"""
    out = []
    for db, s in levels:
        out.append(np.full(secs(s, sr), 0.0 if db is None else 10 ** (db / 20)))
    return np.concatenate(out)


def tones3(dbs=(-30.0, -30.0, -30.0), n_secs=3.0, sr=SR):
    t = np.arange(secs(n_secs, sr)) / sr
    return sum(10 ** (d / 20) * np.sin(2 * np.pi * f * t) for f, d in zip((60.0, 1000.0, 8000.0), dbs))


def mb_bursts(n_secs=3.0, sr=SR, base=-30.0, boost=24.0):
    """60 Hz / 1 kHz / 8 kHz at `base` dB, each raised by `boost` dB in its own window"""
    t = np.arange(secs(n_secs, sr)) / sr
    x = np.zeros_like(t)
    for f, (a, b) in zip((60.0, 1000.0, 8000.0), ((0.5, 1.0), (1.25, 1.75), (2.0, 2.5))):
        env = np.full_like(t, 10 ** (base / 20))
        env[(t >= a) & (t < b)] = 10 ** ((base + boost) / 20)
        x += env * np.sin(2 * np.pi * f * t)
    return x


def st(x):
    return np.stack([x, x], 1)


def fix_bools(path):
    """FloatEvent -> BoolEvent for envelopes whose target is an on/off parameter."""
    root = ET.fromstring(gzip.open(path).read())
    n = 0
    for tr in root.iter("AudioTrack"):
        boolean = set()
        for e in tr.iter():
            at, m = e.find("AutomationTarget"), e.find("Manual")
            if at is not None and m is not None and m.get("Value") in ("true", "false"):
                boolean.add(at.get("Id"))
        for env in tr.iter("AutomationEnvelope"):
            if env.find("EnvelopeTarget/PointeeId").get("Value") not in boolean:
                continue
            for ev in env.find("Automation/Events"):
                if ev.tag == "FloatEvent":
                    ev.tag = "BoolEvent"
                    ev.set("Value", "true" if float(ev.get("Value")) >= 0.5 else "false")
                    n += 1
    data = b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="utf-8")
    with gzip.open(path, "wb") as f:
        f.write(data)
    return n


def step_auto(on_beat, off_beat, first=1.0, second=0.0):
    """value `first` until on_beat, `second` until off_beat, then `first` again (120 BPM: beat = 0.5 s)"""
    return [(on_beat, first), (on_beat, second), (off_beat, second), (off_beat, first)]


def battery_d():
    items = []

    def src(track, wav):
        """device-less sidechain source; returns its track id"""
        tid = 100001 + len(items)
        items.append(dict(track=track, wav=wav, device=False))
        return tid

    # ---------------- sources and shared signals ----------------
    n25 = secs(2.5)
    scx = np.zeros((n25, 2))
    scx[secs(0.1), :] = 0.5
    a, b = secs(0.5), secs(1.5)
    scx[a:b, 0] = noise(-20, 1.0, 1)
    scx[a:b, 1] = noise(-20, 1.0, 2)  # decorrelated L/R
    t = np.arange(secs(0.5)) / SR
    scx[secs(1.75):secs(2.25), :] = (0.5 * np.sin(2 * np.pi * 5000 * t))[:, None]
    id_bb = src("d_src_broadband", sig("sc_imp_noise_5k", scx))
    id_step = src("d_src_dcstep", sig("sc_dcstep_m60_m6", B.dc_step(-60, -6)))
    lonly = B.dc_step(-60, -6)
    lonly[:, 1] = 0.0
    id_lonly = src("d_src_dcstep_Lonly", sig("sc_dcstep_Lonly", lonly))
    id_sil = src("d_src_silent", sig("sc_silent_3s", np.zeros((secs(3.0), 2))))
    id_5k = src("d_src_5k_4s", sig("sc_5k_m6_4s", st(B.sine(5000, -6, 4.0))))
    main_t3 = sig("tones3_m30_2p5s", st(tones3(n_secs=2.5)))
    main30 = sig("dc_m30_3s", np.full((secs(3.0), 2), 10 ** (-30 / 20)))

    # ---------------- A. sidechain Listen (P1) ----------------
    # Measured (critic): with 3 bands Live's Listen = the band-split trigger summed back
    # (AP(fL)*AP(fH)), not the raw trigger. Unknown: whether solo / activator / band gains /
    # master / split buttons change what Listen plays.
    def lis(track, mix=1.0, gain=1.0, **p):
        d = dict(P3)
        d.update(p)
        items.append(dict(track=track, wav=main_t3, params=d,
                          sidechain=dict(track_id=id_bb, gain=gain, drywet=mix, listen=True)))
    lis("lis_3b")
    for bnd in ("Low", "Mid", "High"):
        lis(f"lis_3b_solo{bnd}", **{f"Solo{bnd}": True})
    lis("lis_3b_inactMid", ActiveMid=False)
    lis("lis_3b_inactAll", ActiveLow=False, ActiveMid=False, ActiveHigh=False)
    lis("lis_3b_inMid12", InputGainMid=12.0)
    lis("lis_3b_inLowm12_inHigh6", InputGainLow=-12.0, InputGainHigh=6.0)
    lis("lis_3b_outMid12_master6", GainMid=12.0, OutputGain=6.0)
    lis("lis_lowonly", SplitMidHighOn=False)
    lis("lis_highonly", SplitLowMidOn=False)
    lis("lis_crossed_3000_300", SplitLowMid=3000.0, SplitMidHigh=300.0)
    lis("lis_3b_mix0.5", mix=0.5)
    lis("lis_3b_mix0", mix=0.0)
    lis("lis_3b_x30_15000", SplitLowMid=30.0, SplitMidHigh=15000.0)
    lis("lis_3b_g0.25", gain=0.25)
    # Listen while S/C is off (Listen lives in the S/C section; the core ignores it when S/C is off).
    # S/C On is forced off by automation for the whole clip, then toggled on for 1.0-2.0 s.
    for nm, pts in (("lis_scoff", [(0.0, 0.0)]), ("lis_scon_1_2s", step_auto(2.0, 4.0, 0.0, 1.0))):
        items.append(dict(track=nm, wav=main_t3, params=dict(P3), automation={"SideChain/OnOff": pts},
                          sidechain=dict(track_id=id_bb, gain=1.0, drywet=1.0, listen=True)))
    items.append(dict(track="lis_single_inMid12", wav=main_t3, params=dict(SINGLE, EnvelopeIsPeak=True, InputGainMid=12.0, **C),
                      sidechain=dict(track_id=id_bb, gain=1.0, drywet=1.0, listen=True)))

    # ---------------- B. sidechain detection (P1: input gain; P2: the rest) ----------------
    def sc(track, sid=id_step, gain=1.0, mix=1.0, wav=main30, base=None, **p):
        d = dict(SINGLE, EnvelopeIsPeak=True, **C) if base is None else dict(base)
        d.update(p)
        items.append(dict(track=track, wav=wav, params=d, sidechain=dict(track_id=sid, gain=gain, drywet=mix)))
    sc("sc_in12", InputGainMid=12.0)                       # does the band input gain scale the trigger?
    sc("sc_inm12", InputGainMid=-12.0)
    sc("sc_in12_rms", InputGainMid=12.0, EnvelopeIsPeak=False)
    sc("sc_in12_dw0.5", mix=0.5, InputGainMid=12.0)
    sc("sc_3b_inHigh12", sid=id_bb, wav=main_t3, base=P3, InputGainHigh=12.0)
    sc("sc_gm70", gain=10 ** (-70 / 20), AboveThresholdMid=-80.0, AboveRatioMid=-0.5)  # S/C gain minimum
    sc("sc_gp24", gain=10 ** (24 / 20))                                                   # S/C gain maximum
    sc("sc_rms", EnvelopeIsPeak=False)
    sc("sc_below_up", AboveRatioMid=0.0, BelowThresholdMid=-40.0, BelowRatioMid=0.5)
    sc("sc_Lonly_pk", sid=id_lonly)
    sc("sc_Lonly_rms", sid=id_lonly, EnvelopeIsPeak=False)
    sc("sc_silent_up", sid=id_sil, AboveRatioMid=0.0, BelowThresholdMid=-30.0, BelowRatioMid=0.5)
    sc("sc_inactMid", ActiveMid=False)
    sc("sc_3b_rms_below", sid=id_bb, wav=main_t3, base=dict(
        EnvelopeIsPeak=False, BelowThresholdLow=-50.0, BelowRatioLow=0.5, BelowThresholdMid=-45.0,
        BelowRatioMid=-1.0, BelowThresholdHigh=-40.0, BelowRatioHigh=1.0))

    # ---------------- C. envelope initial state (P1) ----------------
    # Measured (critic): every detector starts at env = 0.01 (peak -40 dB, RMS -20 dB).
    # These pin it per band, per mode, and through a release decay before the first sample.
    t3s = sig("tones3_m20_from0", st(tones3((-20.0, -20.0, -20.0), 2.0)))
    I3 = dict(EnvelopeIsPeak=True)
    for bnd in ("Low", "Mid", "High"):
        I3.update({f"AboveThreshold{bnd}": -60.0, f"AboveRatio{bnd}": -1.0, f"Attack{bnd}": 300.0, f"Release{bnd}": 300.0})
    items.append(dict(track="init_3b_pk", wav=t3s, params=I3))
    items.append(dict(track="init_3b_rms", wav=t3s, params=dict(I3, EnvelopeIsPeak=False)))
    # Release 5 s, so env0 = 0.01 is still well above the -60 dB signal after a 0.05 / 0.5 s lead
    # (peak: -40.4 / -48 dB; RMS power: -20.4 / -24 dB) and the upward gain follows the decay for
    # seconds; after a 2 s lead (-72 dB) the attack takes over (control case).
    UP = dict(SINGLE, EnvelopeIsPeak=True, BelowThresholdMid=-30.0, BelowRatioMid=1.0, AttackMid=1.0, ReleaseMid=5000.0)
    for lead in (0.05, 0.5, 2.0):
        w = sig(f"lead{lead}_dc_m60", st(segs([(None, lead), (-60.0, 4.0 - lead)])))
        items.append(dict(track=f"init_lead{lead}_up_pk", wav=w, params=UP))
    items.append(dict(track="init_lead0.5_up_rms", wav=sig("lead0.5_dc_m60", st(segs([(None, 0.5), (-60.0, 3.5)]))),
                      params=dict(UP, EnvelopeIsPeak=False)))

    # ---------------- D. switches moved mid-stream (P1; BoolEvent via fix_bools) ----------------
    # Detector / filter state while a band is inactive, the device is off, S/C is off, or the
    # detector mode flips (env is a power in RMS, an amplitude in Peak).
    dyn3 = sig("dc_m12_m24_m12_4s", st(segs([(-12.0, 1.0), (-24.0, 2.0), (-12.0, 1.0)])))
    dc12 = sig("dc_m12_4s", np.full((secs(4.0), 2), 10 ** (-12 / 20)))
    K = dict(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-32.0, AboveRatioMid=-1.0, AttackMid=10.0, ReleaseMid=1000.0)
    items.append(dict(track="auto_active_dyn", wav=dyn3, params=K, automation={"ActiveMid": step_auto(1.0, 4.0)}))
    items.append(dict(track="auto_devon_dyn", wav=dyn3, params=K, automation={"On": step_auto(1.0, 5.0)}))
    R = dict(SINGLE, AboveThresholdMid=-30.0, AboveRatioMid=-0.75, AttackMid=10.0, ReleaseMid=100.0)
    items.append(dict(track="auto_rms_to_peak", wav=dc12, params=dict(R, EnvelopeIsPeak=False),
                      automation={"EnvelopeIsPeak": step_auto(4.0, 6.0, 0.0, 1.0)}))
    items.append(dict(track="auto_peak_to_rms", wav=dc12, params=dict(R, EnvelopeIsPeak=True),
                      automation={"EnvelopeIsPeak": step_auto(4.0, 6.0, 1.0, 0.0)}))
    items.append(dict(track="auto_knee", wav=dc12, params=M(SINGLE, FAST, EnvelopeIsPeak=True, AboveThresholdMid=-10.0, AboveRatioMid=-1.0),
                      automation={"SoftKnee": step_auto(2.0, 6.0, 0.0, 1.0)}))
    mb4 = sig("mb_bursts_4s", st(mb_bursts(4.0)))
    items.append(dict(track="auto_active_3b", wav=mb4, params=dict(P3, AttackHigh=30.0, ReleaseHigh=300.0),
                      automation={"ActiveHigh": step_auto(3.0, 4.5)}))
    items.append(dict(track="auto_devon_3b", wav=mb4, params=P3, automation={"On": step_auto(1.5, 4.5)}))
    t3_4 = sig("tones3_m30_4s", st(tones3(n_secs=4.0)))
    items.append(dict(track="auto_scon_3b", wav=t3_4, params=P3, sidechain=dict(track_id=id_5k, gain=1.0, drywet=1.0),
                      automation={"SideChain/OnOff": step_auto(2.0, 6.0, 0.0, 1.0)}))

    # ---------------- E. silence, denormals (P1) ----------------
    gap = sig("gap_m20", st(segs([(-20.0, 0.5), (None, 3.0), (-20.0, 0.5), (None, 0.5)])))
    G = dict(SINGLE, EnvelopeIsPeak=True, AttackMid=1.0, ReleaseMid=0.1)
    items.append(dict(track="gap_up_pk_rel0.1", wav=gap, params=dict(G, BelowThresholdMid=-40.0, BelowRatioMid=1.0)))
    items.append(dict(track="gap_up_rms_rel0.1", wav=gap, params=dict(G, EnvelopeIsPeak=False, BelowThresholdMid=-40.0, BelowRatioMid=1.0)))
    items.append(dict(track="gap_up_pk_rel100", wav=gap, params=dict(G, ReleaseMid=100.0, BelowThresholdMid=-40.0, BelowRatioMid=1.0)))
    items.append(dict(track="gap_exp_pk_rel0.1", wav=gap, params=dict(G, BelowThresholdMid=-40.0, BelowRatioMid=-3.0)))
    items.append(dict(track="gap_comp_pk_rel0.1", wav=gap, params=dict(G, AttackMid=10.0, AboveThresholdMid=-30.0, AboveRatioMid=-1.0)))
    den = np.concatenate([np.full(secs(1.0), v) for v in (1e-40, 1e-38, 1e-30, 1e-6)])
    dw = sig("denorm_1e-40_1e-38_1e-30_1e-6", st(den))
    items.append(dict(track="denorm_up", wav=dw, params=M(SINGLE, FAST, EnvelopeIsPeak=True, BelowThresholdMid=-40.0, BelowRatioMid=1.0)))
    items.append(dict(track="denorm_nodev", wav=dw, device=False))

    # ---------------- F. per-band coverage (P2) ----------------
    mb3 = sig("mb_bursts_3s", st(mb_bursts(3.0)))
    F = dict(EnvelopeIsPeak=True,
             AboveThresholdLow=-25.0, AboveRatioLow=-0.75, AttackLow=1.0, ReleaseLow=300.0,
             AboveThresholdMid=-30.0, AboveRatioMid=-0.5, AttackMid=30.0, ReleaseMid=30.0,
             AboveThresholdHigh=-20.0, AboveRatioHigh=-1.0, AttackHigh=300.0, ReleaseHigh=1.0)
    items += [
        dict(track="mb_times_pk", wav=mb3, params=F),
        dict(track="mb_times_rms", wav=mb3, params=dict(F, EnvelopeIsPeak=False)),
        dict(track="mb_times_t3", wav=mb3, params=dict(F, GlobalTime=3.0)),
        dict(track="mb_gains", wav=mb3, params=dict(F, InputGainLow=6.0, InputGainMid=3.0, InputGainHigh=-12.0,
                                                    GainLow=-3.0, GainMid=-2.0, GainHigh=6.0)),
        dict(track="mb_below", wav=mb3, params=dict(EnvelopeIsPeak=True, BelowThresholdLow=-40.0, BelowRatioLow=0.5,
                                                    BelowThresholdMid=-35.0, BelowRatioMid=-1.0,
                                                    BelowThresholdHigh=-45.0, BelowRatioHigh=1.0)),
        dict(track="mb_knee_rms", wav=mb3, params=dict(F, EnvelopeIsPeak=False, SoftKnee=True)),
        dict(track="mb_lowonly_dyn", wav=mb3, params=dict(F, SplitMidHighOn=False)),
        dict(track="mb_highonly_dyn", wav=mb3, params=dict(F, SplitLowMidOn=False)),
        dict(track="mb_x200_5000_dyn", wav=mb3, params=dict(F, SplitLowMid=200.0, SplitMidHigh=5000.0)),
        dict(track="mb_inactLow_dyn", wav=mb3, params=dict(F, ActiveLow=False)),
    ]
    sx = np.zeros((secs(3.0), 2))
    tt = np.arange(secs(3.0)) / SR
    eL = np.where((tt >= 0.5) & (tt < 1.0), 10 ** (-6 / 20), 10 ** (-30 / 20))
    eR = np.where((tt >= 2.0) & (tt < 2.5), 10 ** (-6 / 20), 10 ** (-30 / 20))
    sx[:, 0] = eL * np.sin(2 * np.pi * 60 * tt)
    sx[:, 1] = eR * np.sin(2 * np.pi * 8000 * tt)
    items.append(dict(track="mb_stereo_split", wav=sig("mb_stereo_L60_R8k", sx), params=F))

    # ---------------- G. gain-computer extremes (P2) ----------------
    loud, _ = B.fine_stairs(-20.0, 30.0, 2.0, 0.1)
    lw = sig("loud_stairs_m20_p30", loud)
    S1 = M(SINGLE, FAST, EnvelopeIsPeak=True)
    items += [dict(track="loud_comp_pk", wav=lw, params=dict(S1, AboveThresholdMid=0.0, AboveRatioMid=-0.5)),
              dict(track="loud_upx_cap", wav=lw, params=dict(S1, AboveThresholdMid=-10.0, AboveRatioMid=1.0)),
              dict(track="loud_knee_rms", wav=lw, params=dict(S1, EnvelopeIsPeak=False, AboveThresholdMid=0.0, AboveRatioMid=-1.0, SoftKnee=True))]
    stw = sig("dc_stairs_m84_p6", B.dc_stairs()[0])
    items += [dict(track="knee_rms_above", wav=stw, params=M(SINGLE, FAST, AboveThresholdMid=-20.0, AboveRatioMid=-0.75, SoftKnee=True)),
              dict(track="knee_rms_below", wav=stw, params=M(SINGLE, FAST, BelowThresholdMid=-50.0, BelowRatioMid=0.5, SoftKnee=True))]

    # ---------------- H. low-crossover float artefacts: amplitude / sign dependence (P3) ----------------
    X30 = dict(SplitLowMid=30.0, SplitMidHigh=300.0, SoloLow=True)
    items += [dict(track="x30_soloLow_imp1e-3", wav=sig("imp1e-3", B.impulse(1e-3)), params=X30),
              dict(track="x30_soloLow_imp1e-6", wav=sig("imp1e-6", B.impulse(1e-6)), params=X30),
              dict(track="x30_soloLow_impneg", wav=sig("imp_m05", B.impulse(-0.5)), params=X30),
              dict(track="x60_soloLow_imp", wav=sig("imp05", B.impulse()), params=dict(X30, SplitLowMid=60.0)),
              dict(track="x30_lowonly_dcstep", wav=sig("dc_m12_step", B.dc_step(-120, -12, 0.25, 1.25, 2.0)),
                   params=dict(SplitLowMid=30.0, SplitMidHighOn=False, SoloLow=True)),
              dict(track="x30_soloLow_sine40", wav=sig("sine40_m12", st(B.sine(40.0, -12, 2.0))), params=X30)]

    # ---------------- I. long-time coefficient precision (P3) ----------------
    dn = sig("dc_m6_then_m80_10s", st(segs([(-6.0, 1.0), (-80.0, 9.0)])))
    up = sig("dc_m80_then_m6_10s", st(segs([(-80.0, 1.0), (-6.0, 9.0)])))
    L80 = dict(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-80.0, AboveRatioMid=-0.5)
    items += [dict(track="coef_rel5000_pk", wav=dn, params=dict(L80, AttackMid=0.1, ReleaseMid=5000.0)),
              dict(track="coef_rel5000_rms", wav=dn, params=dict(L80, EnvelopeIsPeak=False, AttackMid=0.1, ReleaseMid=5000.0)),
              dict(track="coef_rel2500_t2_pk", wav=dn, params=dict(L80, AttackMid=0.1, ReleaseMid=2500.0, GlobalTime=2.0)),
              dict(track="coef_rel500_t10_pk", wav=dn, params=dict(L80, AttackMid=0.1, ReleaseMid=500.0, GlobalTime=10.0)),
              dict(track="coef_att5000_pk", wav=up, params=dict(L80, AttackMid=5000.0, ReleaseMid=1.0)),
              dict(track="coef_att5000_rms", wav=up, params=dict(L80, EnvelopeIsPeak=False, AttackMid=5000.0, ReleaseMid=1.0))]

    path = B.build("mbd_d", items)
    nb = fix_bools(path)
    return path, len(items), nb


# ---------------- sample-rate sets (P1): export each at its own rate ----------------
def battery_d_sr(rate):
    old = als.SR
    als.SR = rate  # SetBuilder.add reads als.SR at call time (assert, clip length, DefaultSampleRate)
    try:
        tag = f"sr{rate // 1000}"

        def s(name, x):
            p = os.path.join(B.HERE, "sig", f"d_{tag}_{name}.wav")
            if not os.path.exists(p):
                als.wav_write(p, x, sr=rate)
            return p

        def imp(amp=0.5, n_secs=1.0, at=0.25):
            x = np.zeros((secs(n_secs, rate), 2))
            x[secs(at, rate), :] = amp
            return x

        def step(a_db, b_db, t1=0.5, t2=1.5, n_secs=3.0):
            x = np.full(secs(n_secs, rate), 10 ** (a_db / 20))
            x[secs(t1, rate):secs(t2, rate)] = 10 ** (b_db / 20)
            return st(x)

        def sine_r(f, db, n_secs):
            t = np.arange(secs(n_secs, rate)) / rate
            return st(10 ** (db / 20) * np.sin(2 * np.pi * f * t))

        items = []
        items.append(dict(track=f"{tag}_nodev_dc", wav=s("dc_m12_1s", np.full((secs(1.0, rate), 2), 10 ** (-12 / 20))), device=False))
        bb = np.zeros((secs(2.0, rate), 2))
        bb[secs(0.1, rate), :] = 0.5
        bb[secs(0.5, rate):secs(1.5, rate), 0] = noise(-20, 1.0, 1, rate)
        bb[secs(0.5, rate):secs(1.5, rate), 1] = noise(-20, 1.0, 2, rate)
        sid = 100001 + len(items)
        items.append(dict(track=f"{tag}_src_broadband", wav=s("sc_imp_noise", bb), device=False))
        im = s("imp05", imp())
        items.append(dict(track=f"{tag}_neutral", wav=im))
        items.append(dict(track=f"{tag}_single", wav=im, params=dict(SINGLE)))
        for bnd in ("Low", "Mid", "High"):
            items.append(dict(track=f"{tag}_solo{bnd}", wav=im, params={f"Solo{bnd}": True}))
        items.append(dict(track=f"{tag}_x500_2k_soloMid", wav=im, params=dict(SplitLowMid=500.0, SplitMidHigh=2000.0, SoloMid=True)))
        items.append(dict(track=f"{tag}_x30_300_soloLow", wav=im, params=dict(SplitLowMid=30.0, SplitMidHigh=300.0, SoloLow=True)))
        items.append(dict(track=f"{tag}_x3000_15000_soloHigh", wav=im, params=dict(SplitLowMid=3000.0, SplitMidHigh=15000.0, SoloHigh=True)))
        sw = s("dc_step_m40_m6", step(-40, -6))
        for nm, pk, a, r in (("att_pk_1", True, 1.0, 1.0), ("att_pk_10", True, 10.0, 1.0), ("att_rms_10", False, 10.0, 1.0),
                             ("rel_pk_100", True, 0.1, 100.0), ("rel_rms_100", False, 0.1, 100.0)):
            items.append(dict(track=f"{tag}_{nm}", wav=sw, params=dict(SINGLE, EnvelopeIsPeak=pk, AttackMid=a, ReleaseMid=r, **C)))
        items.append(dict(track=f"{tag}_time_pk_2", wav=sw, params=dict(SINGLE, EnvelopeIsPeak=True, AttackMid=10.0, ReleaseMid=100.0, GlobalTime=2.0, **C)))
        lv = np.arange(-84, 6.1, 2.0)
        x = np.zeros(secs(0.1, rate) + secs(0.15, rate) * len(lv))
        for i, l in enumerate(lv):
            x[secs(0.1, rate) + i * secs(0.15, rate): secs(0.1, rate) + (i + 1) * secs(0.15, rate)] = 10 ** (l / 20)
        items.append(dict(track=f"{tag}_st_peak", wav=s("dc_stairs", st(x)), params=M(SINGLE, FAST, EnvelopeIsPeak=True, **C)))
        for frac in (0.3125, 0.40):
            items.append(dict(track=f"{tag}_os{frac}_fastcomp", wav=s(f"sine{frac}fs_m6", sine_r(frac * rate, -6, 2.0)),
                              params=dict(SINGLE, EnvelopeIsPeak=True, AttackMid=0.1, ReleaseMid=0.1, AboveThresholdMid=-40.0, AboveRatioMid=-1.0)))
        items.append(dict(track=f"{tag}_init_pk", wav=s("dc_m12_2s", np.full((secs(2.0, rate), 2), 10 ** (-12 / 20))),
                          params=dict(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-32.0, AboveRatioMid=-1.0)))
        items.append(dict(track=f"{tag}_lis_3b", wav=s("tones3_m30", st(tones3(n_secs=2.0, sr=rate))), params=P3,
                          sidechain=dict(track_id=sid, gain=1.0, drywet=1.0, listen=True)))
        items.append(dict(track=f"{tag}_mb_bursts", wav=s("mb_bursts", st(mb_bursts(3.0, sr=rate))), params=P3))
        return B.build(f"mbd_d{tag}", items), len(items)
    finally:
        als.SR = old


if __name__ == "__main__":
    a = sys.argv[1:]
    if "--dry" in a:
        i = a.index("--dry")
        B.HERE = a[i + 1]
        del a[i:i + 2]
        os.makedirs(os.path.join(B.HERE, "sig"), exist_ok=True)
        os.makedirs(os.path.join(B.HERE, "sets"), exist_ok=True)
    what = a[0] if a else "d"
    if what in ("d", "all"):
        print(battery_d())
    if what in ("sr44", "all"):
        print(battery_d_sr(44100))
    if what in ("sr96", "all"):
        print(battery_d_sr(96000))
