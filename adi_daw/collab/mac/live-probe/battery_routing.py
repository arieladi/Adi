"""Probe battery requested by the routing agent: sidechain gain / mix mapping, Listen gains, Listen logic.

    battery_rt()  -> sets/mbd_rt.als + mbd_rt.json   (48 kHz; export like A-D: All Individual Tracks,
                                                       32-bit float, no normalize, no dither)
Usage: cd $S/probe && python3 <this file>            (add --dry DIR to build into DIR instead)

Why single-band Listen: with both split buttons off the band is the signal itself, and Listen plays
    down( up( dry*main + wet*(scgain*sc) ) * inGain * master )
which the core already reproduces BIT-EXACTLY (sc_listen, sc_listen_dw0.5, lis_single_inMid12: zero
mismatching samples once the render runs with flush-to-zero and Live's exact clip fades). So every
probe below pins one float constant (or one operation order) to the last bit:
  A. S/C gain: Live turns a stored linear 2.0 into 1.99999976 (2 ulps low; 1.0 stays 1.0) and -70 dB
     into silence. Which formula, and where does "off" start?
  B. S/C mix law between the measured ends (0, 0.5, 1).
  C. order of S/C gain and mix (wet*(g*sc) or (wet*g)*sc).
  D. dB -> linear for band input gain and master as Live applies them in Listen (12 dB measured:
     3.98107195 = 10^float(12/20)), their product order, and that the band OUTPUT gain is not applied.
  E. Listen logic not yet covered: solo + inactive, a soloed band that does not exist, crossed splits
     with solo Mid, Listen while S/C is OFF (compressing settings, so "normal output" and "Listen of the
     main" differ by 20 dB).
Notes for the harness:
- Every signal name starts with "rt_".  Sidechain sources are device-less tracks (id 100001 + index).
- Tracks listed in SC_OFF get SideChain/OnOff = false in the .als after build (Listen stays on); their
  manifest sidechain dict carries "on": false, so nulltest.to_args must pass SidechainOn=0 for them.
"""
import gzip, math, os, sys
import xml.etree.ElementTree as ET
import numpy as np

PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)
import battery as B  # noqa: E402
from battery import SINGLE  # noqa: E402

SR = B.SR


def sig(name, x):
    return B.sig("rt_" + name, x)


def noise_uniform(peak_db, n, seed):
    """independent L/R uniform noise; float32-representable after the WAV write"""
    return 10 ** (peak_db / 20) * np.random.default_rng(seed).uniform(-1.0, 1.0, (n, 2))


def set_sc_off(path, tracks):
    """SideChain/OnOff/Manual -> false on the devices of the named tracks (Listen and routing stay)."""
    root = ET.fromstring(gzip.open(path).read())
    n = 0
    for tr in root.iter("AudioTrack"):
        if tr.find("Name/EffectiveName").get("Value") not in tracks:
            continue
        for mbd in tr.iter("MultibandDynamics"):
            mbd.find("SideChain/OnOff/Manual").set("Value", "false")
            n += 1
    data = b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="utf-8")
    with gzip.open(path, "wb") as f:
        f.write(data)
    return n


def battery_rt():
    items = []
    n = SR  # 1 s per probe
    main = noise_uniform(-14.0, n, 21)
    main[: int(0.05 * SR)] = 0.0              # 50 ms of silence first (start transient check)
    sc = noise_uniform(-20.0, n, 22)
    sc[int(0.40 * SR): int(0.50 * SR)] = 0.0  # gap: Listen at 100 % must be exactly 0 here
    main_w = sig("noise_main_m14", main)
    sc_w = sig("noise_sc_m20_gap", sc)
    sid = 100001 + len(items)
    items.append(dict(track="rt_src_sc", wav=sc_w, device=False))
    items.append(dict(track="rt_main_nodev", wav=main_w, device=False))  # Live's faded copy of the main

    def lis(track, gain=1.0, mix=1.0, base=SINGLE, **p):
        d = dict(base)
        d.update(p)
        items.append(dict(track=track, wav=main_w, params=d,
                          sidechain=dict(track_id=sid, gain=gain, drywet=mix, listen=True)))

    # A. S/C gain mapping (stored linear, as Live writes it); mix 100 %
    for g in (0.5, 0.25, 4.0, 8.0, 1.5, 0.7, 3.0, 1.25):
        lis(f"rt_g_lin{g}", gain=g)
    for gdb in (-69.99, -69.9, -69.5, -69.0, -66.0, -60.0, -40.0, -20.0, -12.0, -6.0, -3.0, -1.0, -0.1,
                0.1, 1.0, 3.0, 6.0, 9.0, 12.0, 18.0, 23.9, 24.0):
        lis(f"rt_g_db{gdb}", gain=10 ** (gdb / 20))

    # B. mix law, S/C gain 1
    for m in (0.001, 0.01, 0.1, 0.2, 0.25, 0.3, 1 / 3, 0.4, 0.6, 0.7, 0.75, 0.8, 0.9, 0.99, 0.999):
        lis(f"rt_mix{m:.4g}", mix=m)

    # C. order of gain and mix
    for g, m in ((1.5, 0.3), (0.7, 0.8), (3.0, 0.5), (0.5, 0.25), (1.25, 0.6)):
        lis(f"rt_g{g}_mix{m}", gain=g, mix=m)

    # D. band input gain / master / band output gain in Listen (single band, S/C gain 1, mix 100 %)
    for ig in (-24.0, -18.0, -12.0, -6.0, -3.0, -1.0, -0.5, 0.5, 1.0, 3.0, 3.3, 6.0, 9.0, 18.0, 24.0):
        lis(f"rt_in{ig}", InputGainMid=ig)
    for mg in (-24.0, -12.0, -6.0, -2.9, -1.0, 1.0, 3.0, 6.0, 12.0, 24.0):
        lis(f"rt_master{mg}", OutputGain=mg)
    for ig, mg in ((12.0, 6.0), (-6.0, 3.0), (3.3, -2.9), (24.0, -24.0)):
        lis(f"rt_in{ig}_master{mg}", InputGainMid=ig, OutputGain=mg)
    lis("rt_out12", GainMid=12.0)                          # expected: no effect on Listen
    lis("rt_in6_g1.5_mix0.3", gain=1.5, mix=0.3, InputGainMid=6.0)

    # E. Listen logic (three bands unless noted)
    three = {}
    lis("rt_l3_soloMid_inactMid", base=three, SoloMid=True, ActiveMid=False)
    lis("rt_l3_soloLow_soloHigh", base=three, SoloLow=True, SoloHigh=True)
    lis("rt_l3_soloLow_inactHigh", base=three, SoloLow=True, ActiveHigh=False)
    lis("rt_l3_crossed_soloMid", base=three, SplitLowMid=3000.0, SplitMidHigh=300.0, SoloMid=True)
    lis("rt_lowoff_soloLow", base=three, SplitLowMidOn=False, SoloLow=True)   # soloed band does not exist
    lis("rt_highoff_soloHigh", base=three, SplitMidHighOn=False, SoloHigh=True)
    lis("rt_single_inactMid", ActiveMid=False)                                  # single band, inactive
    lis("rt_single_soloMid_inactMid", SoloMid=True, ActiveMid=False)
    lis("rt_l3_inactLow_in6_master3", base=three, ActiveLow=False, InputGainMid=6.0, InputGainHigh=-3.0,
        OutputGain=3.0)
    # Listen with S/C OFF: compressing settings, so normal output (about -26 dB gain) and Listen of the
    # main (trigger = main, no compression) are 20+ dB apart; out gain 12 dB on top in the second.
    comp = dict(EnvelopeIsPeak=True, AboveThresholdMid=-40.0, AboveRatioMid=-1.0, AttackMid=1.0,
                ReleaseMid=10.0)
    lis("rt_scoff_listen_single", **comp)
    lis("rt_scoff_listen_3b_out12", base=three, GainMid=12.0, **comp)
    sc_off = ["rt_scoff_listen_single", "rt_scoff_listen_3b_out12"]
    for it in items:
        if it["track"] in sc_off:
            it["sidechain"] = dict(it["sidechain"], on=False)

    path = B.build("mbd_rt", items)
    nb = set_sc_off(path, sc_off)
    assert nb == len(sc_off), nb
    return path, len(items)


if __name__ == "__main__":
    a = sys.argv[1:]
    if "--dry" in a:
        i = a.index("--dry")
        B.HERE = a[i + 1]
        del a[i:i + 2]
        os.makedirs(os.path.join(B.HERE, "sig"), exist_ok=True)
        os.makedirs(os.path.join(B.HERE, "sets"), exist_ok=True)
    print(battery_rt())
