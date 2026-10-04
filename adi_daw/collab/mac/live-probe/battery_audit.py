"""Probe battery requested by the audit agent (switch automation and host behaviour), in the battery_d.py style.

  battery_audit()          -> sets/mbd_audit.als + mbd_audit.json  (48 kHz; export like A-D: All Individual Tracks,
                                                                   32-bit float, no normalize, no dither)
  battery_audit_sr(rate)   -> sets/mbd_audit_sr44.als / mbd_audit_sr96.als (export THAT set at 44.1 / 96 kHz; the clips
                                                                   are written at that rate so Live does not resample)

Usage: cd $S/probe && python3 $S/agents/audit/battery_next.py [audit | sr44 | sr96 | all]   (--dry DIR as in battery_d.py)

What each group settles (current model in agents/audit/NOTES.md):
- scsw_*  S/C On ramp. Model: trigger = (1-s)*main + s*mix, s = smoothstep(k/72), k = samples since the switch; S/C path
          not processed while off. With Listen on and a single band the output IS the trigger (down(up(trigger))), so
          scsw_dc_listen reads s(k) to float precision: s = (0.2512 - y) / (0.2512 - 0.01585). Events sit off the
          32-sample grid (+7, +13, +1, +41, +31 samples) to show whether a switch acts at its sample or at a block edge;
          the second ON shows whether the S/C path resumes frozen (DC: no transient) or from silence (ringing);
          the OFF 40 samples after an ON shows what Live does with a switch inside the ramp.
- pksw_*  Peak/RMS flip with a sine (DC cannot tell an envelope conversion sqrt/square from other seamless schemes).
- devsw_* Device On: fade alignment off the grid, fade arithmetic with a varying dry signal, which state is cleared
          on re-enable (detector: attack 300 ms from -40 dB is slow and visible; resampler: a sine shows a frozen state),
          and a switch back inside the 48-sample fade.
- lisfz   Detectors while Listen plays: frozen, or running on the main input (main drops 18 dB during the Listen span).
- tail_*  Post-clip cut to exact zero (host): impulse 300 samples before the clip end at 1 / 1e-3 / 1e-6, clip lengths
          off the 32-sample grid, to pin the threshold and the grid.
"""
import os, sys
import numpy as np

PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)
import als  # noqa: E402
import battery as B  # noqa: E402
import battery_d as D  # noqa: E402
from battery import SINGLE  # noqa: E402

SR = B.SR
SPB = 24000  # samples per beat at 120 BPM, 48 kHz


def sig(name, x):
    return B.sig("au_" + name, x)


def secs(s, sr=SR):
    return int(round(s * sr))


def st(x):
    return np.stack([x, x], 1)


def dc(db, n_secs, sr=SR):
    return np.full(secs(n_secs, sr), 10 ** (db / 20))


def sine(f, db, n_secs, sr=SR):
    t = np.arange(secs(n_secs, sr)) / sr
    return 10 ** (db / 20) * np.sin(2 * np.pi * f * t)


def beat(b, extra=0, spb=SPB):
    """beat b plus `extra` samples, as a beat time"""
    return b + extra / spb


def toggles(times, first):
    """bool steps: value `first` before times[0], flipping at each time"""
    pts, v = [], first
    for t in times:
        pts += [(t, v), (t, 1.0 - v)]
        v = 1.0 - v
    return pts


# one detector that follows its input within one oversampled sample (0.1 ms x Time 10% = 0.01 ms -> c = 6.8e-5)
INSTANT = dict(AttackMid=0.1, ReleaseMid=0.1, GlobalTime=0.1)
LIMIT80 = dict(EnvelopeIsPeak=True, AboveThresholdMid=-80.0, AboveRatioMid=-1.0)  # gain = -(L + 80) dB


def battery_audit():
    items = []

    def src(track, wav):
        tid = 100001 + len(items)
        items.append(dict(track=track, wav=wav, device=False))
        return tid

    # ---------------- A. S/C On ramp (P1) ----------------
    main12 = sig("dc_m12_4s", st(dc(-12.0, 4.0)))
    id_sc36 = src("au_src_dc_m36", sig("dc_m36_4s", st(dc(-36.0, 4.0))))
    sc_times = [beat(1.0, 7), beat(1.5, 13), beat(2.0, 1), beat(2.0, 41), beat(2.5, 0), beat(3.0, 31)]
    sc_auto = {"SideChain/OnOff": toggles(sc_times, 0.0)}
    P = D.M(SINGLE, INSTANT, LIMIT80)
    items.append(dict(track="scsw_dc", wav=main12, params=P, automation=sc_auto,
                      sidechain=dict(track_id=id_sc36, gain=1.0, drywet=1.0)))
    items.append(dict(track="scsw_dc_listen", wav=main12, params=P, automation=sc_auto,
                      sidechain=dict(track_id=id_sc36, gain=1.0, drywet=1.0, listen=True)))
    items.append(dict(track="scsw_dc_listen_mix0.5", wav=main12, params=P, automation=sc_auto,
                      sidechain=dict(track_id=id_sc36, gain=1.0, drywet=0.5, listen=True)))

    # ---------------- B. Peak/RMS flip with a sine (P1) ----------------
    s1k = sig("sine1k_m12_4s", st(sine(1000.0, -12.0, 4.0)))
    PK = D.M(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-40.0, AboveRatioMid=-0.5, AttackMid=10.0, ReleaseMid=100.0)
    pk_auto = {"EnvelopeIsPeak": toggles([beat(2.0, 5), beat(4.0, 11)], 1.0)}
    items.append(dict(track="pksw_sine", wav=s1k, params=PK, automation=pk_auto))
    items.append(dict(track="pksw_sine_rmsfirst", wav=s1k, params=dict(PK, EnvelopeIsPeak=False),
                      automation={"EnvelopeIsPeak": toggles([beat(2.0, 5), beat(4.0, 11)], 0.0)}))
    items.append(dict(track="pksw_sine_instant", wav=s1k, params=D.M(PK, INSTANT), automation=pk_auto))

    # ---------------- C. Device On (P2) ----------------
    DV = D.M(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-40.0, AboveRatioMid=-0.5, AttackMid=300.0, ReleaseMid=300.0)
    items.append(dict(track="devsw_sine", wav=s1k, params=DV, automation={"On": toggles([beat(2.0, 3), beat(4.0, 17)], 1.0)}))
    items.append(dict(track="devsw_dc_short", wav=main12, params=DV, automation={"On": toggles([beat(2.0, 0), beat(2.0, 20)], 1.0)}))

    # ---------------- D. detectors while Listen plays (P2) ----------------
    lz = sig("dc_m12_then_m30_4s", st(np.concatenate([dc(-12.0, 1.5), dc(-30.0, 2.5)])))
    LZ = D.M(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-40.0, AboveRatioMid=-0.5, AttackMid=10.0, ReleaseMid=1000.0)
    items.append(dict(track="lisfz", wav=lz, params=LZ, automation={"SideChain/OnOff": toggles([beat(2.0), beat(4.0)], 0.0)},
                      sidechain=dict(track_id=id_sc36, gain=1.0, drywet=1.0, listen=True)))

    # ---------------- E. post-clip cut (P3, host) ----------------
    for amp, n in ((1.0, 48000), (1e-3, 48000), (1e-6, 48000), (1.0, 48007), (1e-3, 48016), (1e-3, 48023)):
        x = np.zeros((n, 2))
        x[n - 300, :] = amp
        items.append(dict(track=f"tail_imp{amp:g}_n{n}", wav=sig(f"tail_imp{amp:g}_n{n}", x), params=dict(SINGLE)))

    path = B.build("mbd_audit", items)
    nb = D.fix_bools(path)
    return path, len(items), nb


def battery_audit_sr(rate):
    """the S/C ramp and the device fade at another rate: are they 72 / 48 samples or 1.5 / 1 ms?"""
    old = als.SR
    als.SR = rate
    try:
        tag = f"sr{rate // 1000}"
        spb = rate // 2

        def s(name, x):
            p = os.path.join(B.HERE, "sig", f"au_{tag}_{name}.wav")
            if not os.path.exists(p):
                als.wav_write(p, x, sr=rate)
            return p

        items = []
        sid = 100001 + len(items)
        items.append(dict(track=f"{tag}_src_dc_m36", wav=s("dc_m36_4s", st(dc(-36.0, 4.0, rate))), device=False))
        main12 = s("dc_m12_4s", st(dc(-12.0, 4.0, rate)))
        P = D.M(SINGLE, INSTANT, LIMIT80)
        items.append(dict(track=f"{tag}_scsw_dc_listen", wav=main12, params=P,
                          automation={"SideChain/OnOff": toggles([beat(1.0, 7, spb), beat(2.0, 13, spb)], 0.0)},
                          sidechain=dict(track_id=sid, gain=1.0, drywet=1.0, listen=True)))
        DV = D.M(SINGLE, EnvelopeIsPeak=True, AboveThresholdMid=-40.0, AboveRatioMid=-0.5, AttackMid=300.0, ReleaseMid=300.0)
        items.append(dict(track=f"{tag}_devsw_sine", wav=s("sine1k_m12_4s", st(sine(1000.0, -12.0, 4.0, rate))), params=DV,
                          automation={"On": toggles([beat(2.0, 3, spb), beat(4.0, 17, spb)], 1.0)}))
        path = B.build(f"mbd_audit{tag}", items)
        nb = D.fix_bools(path)
        return path, len(items), nb
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
    what = a[0] if a else "audit"
    if what in ("audit", "all"):
        print(battery_audit())
    if what in ("sr44", "all"):
        print(battery_audit_sr(44100))
    if what in ("sr96", "all"):
        print(battery_audit_sr(96000))
