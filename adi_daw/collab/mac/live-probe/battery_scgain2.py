"""S/C gain mapping, round 2 (evidence the scg agents asked for).
  1. staircases: runs of CONSECUTIVE float32 stored gains at five levels (does Live's gain step?)
  2. the off cut bisected inside (-69.9, -69.8] dB, plus linear and normalised candidates
  3. a Min -> Max automation ramp of the S/C gain under Listen (the taper's shape, the interpolation domain)
  4. a device-less track whose MIXER Volume is set to the same stored values (shared volume code?)
Single-band Listen, mix 100 %, as in battery_scgain.py.  battery_scgain2() -> sets/mbd_scg2.als + .json"""
import gzip, os, sys
import xml.etree.ElementTree as ET
import numpy as np
PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)
import battery as B  # noqa: E402
from battery import SINGLE  # noqa: E402
SR = B.SR
f32 = np.float32
MIN, MAX = 0.0003162277571, 15.8489332

def set_mixer_volume(path, volumes):
    root = ET.fromstring(gzip.open(path).read())
    n = 0
    for tr in root.iter("AudioTrack"):
        name = tr.find("Name/EffectiveName").get("Value")
        if name in volumes:
            tr.find("DeviceChain/Mixer/Volume/Manual").set("Value", repr(float(volumes[name])))
            n += 1
    with gzip.open(path, "wb") as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root, encoding="utf-8"))
    return n

def battery_scgain2():
    items = []
    n = SR
    main = 10 ** (-14 / 20) * np.random.default_rng(31).uniform(-1, 1, (n, 2))
    sc = 10 ** (-20 / 20) * np.random.default_rng(32).uniform(-1, 1, (n, 2))
    main_w = B.sig("scg_noise_main", main)
    sc_w = B.sig("scg_noise_sc", sc)
    sid = 100001 + len(items)
    items.append(dict(track="scg2_src", wav=sc_w, device=False))
    def lis(track, gain, automation=None):
        it = dict(track=track, wav=main_w, params=dict(SINGLE),
                  sidechain=dict(track_id=sid, gain=float(f32(gain)), drywet=1.0, listen=True))
        if automation:
            it["automation"] = automation
        items.append(it)
    # 1. staircases of consecutive floats
    for db, count in ((22.0, 64), (5.0, 48), (0.0, 48), (-20.0, 48), (-46.0, 32)):
        g = f32(10 ** (db / 20))
        for k in range(count):
            lis(f"st{db:+.0f}_{k:02d}", g)
            g = np.nextafter(g, f32(np.inf))
    # 2. off cut
    for db in (-69.89, -69.875, -69.86, -69.85, -69.84, -69.825, -69.81):
        lis(f"cut{db}", 10 ** (db / 20))
    for v in (np.nextafter(f32(0.00032), f32(0)), f32(0.00032), np.nextafter(f32(0.00032), f32(1))):
        lis(f"cutlin_{float(v):.9g}", v)
    # 3. a Min -> Max ramp over beats 1..7 (3 s), Listen on
    ramp = {"SideChain/RoutedInput/Volume": [(0.0, MIN), (1.0, MIN), (7.0, MAX), (8.0, MAX)]}
    long_main = B.sig("scg2_main_4s", 10 ** (-14 / 20) * np.random.default_rng(41).uniform(-1, 1, (4 * SR, 2)))
    long_sc = B.sig("scg2_dc_sc_4s", np.full((4 * SR, 2), 0.25))
    sid2 = 100001 + len(items)
    items.append(dict(track="scg2_src_dc", wav=long_sc, device=False))
    items.append(dict(track="ramp_min_max", wav=long_main, params=dict(SINGLE),
                      sidechain=dict(track_id=sid2, gain=1.0, drywet=1.0, listen=True), automation=ramp))
    # 4. mixer Volume (device-less) at the same stored values
    mixers = {}
    for v in (0.5, 2.0, 4.0, 8.0, 0.1, 12.5892544, 1.25, 1.5, 0.25, 3.0):
        name = f"mixer_vol_{v:g}"
        items.append(dict(track=name, wav=sc_w, device=False))
        mixers[name] = float(f32(v))
    path = B.build("mbd_scg2", items)
    nv = set_mixer_volume(path, mixers)
    return path, len(items), nv

if __name__ == "__main__":
    print(battery_scgain2())
