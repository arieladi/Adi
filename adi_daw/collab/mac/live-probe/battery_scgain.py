"""S/C gain mapping battery (lead, for the routing agent's open item A).
Single-band Listen, mix 100 %: the output is down(up(gain*sc))*master, so Live's effective float S/C gain
reads off to the last bit. Every stored gain is float32-exact (the XML decimal IS a float).
  battery_scgain() -> sets/mbd_scg.als + mbd_scg.json"""
import os, sys
import numpy as np
PROBE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, PROBE)
import battery as B  # noqa: E402
from battery import SINGLE  # noqa: E402
SR = B.SR
f32 = lambda v: float(np.float32(v))

def battery_scgain():
    items = []
    n = SR
    main = 10 ** (-14 / 20) * np.random.default_rng(31).uniform(-1, 1, (n, 2))
    sc = 10 ** (-20 / 20) * np.random.default_rng(32).uniform(-1, 1, (n, 2))
    main_w = B.sig("scg_noise_main", main)
    sc_w = B.sig("scg_noise_sc", sc)
    sid = 100001 + len(items)
    items.append(dict(track="scg_src", wav=sc_w, device=False))
    def lis(track, gain, automation=None):
        it = dict(track=track, wav=main_w, params=dict(SINGLE),
                  sidechain=dict(track_id=sid, gain=f32(gain), drywet=1.0, listen=True))
        if automation:
            it["automation"] = automation
        items.append(it)
    # 1. scale dependence: powers of two, 1.25 * 2^k, 1.5 * 2^k
    for k in range(-11, 4):
        lis(f"scg_p2_{k}", 2.0 ** k)
    for k in range(-10, 4):
        lis(f"scg_1.25p2_{k}", 1.25 * 2.0 ** k)
        lis(f"scg_1.5p2_{k}", 1.5 * 2.0 ** k)
    # 2. dense grid in dB (0.5 dB) and irrational values
    for i in range(int((24 + 20) / 0.5) + 1):
        db = -20 + 0.5 * i
        lis(f"scg_db{db:+.1f}", 10 ** (db / 20))
    for j, v in enumerate((0.7071067811865476, 1.4142135623730951, 2.718281828459045, 0.36787944117144233,
                           3.141592653589793, 0.3183098861837907, 1.618033988749895, 0.6180339887498949,
                           1.7320508075688772, 0.5773502691896258, 2.23606797749979, 0.4472135954999579,
                           5.0, 0.2, 7.0, 0.142857142857, 9.0, 0.1111111111, 11.0, 0.0909090909)):
        lis(f"scg_irr{j}_{v:.6g}", v)
    # 3. the "off" cut-off between -69.9 (silent) and -69.5 dB (on)
    for db in (-69.8, -69.75, -69.7, -69.65, -69.6, -69.55):
        lis(f"scg_cut{db}", 10 ** (db / 20))
    # 4. the same values delivered by an automation envelope held constant: load path vs DSP path
    for g in (0.5, 2.0, 10 ** (-20 / 20)):
        lis(f"scg_auto{f32(g):.6g}", 1.0, automation={"SideChain/RoutedInput/Volume": [(0.0, f32(g)), (40.0, f32(g))]})
    return B.build("mbd_scg", items)

if __name__ == "__main__":
    print(battery_scgain())
