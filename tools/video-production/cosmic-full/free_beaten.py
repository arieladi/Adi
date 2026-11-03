"""Free disk by removing PNG frames of Draft 1 takes that a Draft 2 take beats by eye (latents stay).

usage: free_beaten.py <root> <eye-review.json> <d2-review.json> <target free GB>
Only a Draft 1 take whose slot has a Draft 2 take with a strictly better eye verdict is freed (the assembly could
never pick it), worst Draft 1 verdict first, until the target is free. Same effect as animate_full's free_replaced:
frames deleted, generated.json renamed generated.replaced.json, latent kept for a re-decode.
"""
import json, shutil, sys
from pathlib import Path

R = Path(sys.argv[1])
eye = {x["clip"]: x["final"] for x in json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))}
d2 = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
TARGET = float(sys.argv[4]) * 1024 ** 3
RANK = {"moves": 3, "weak": 2, "still": 1, "broken": 0}

beaten = []
for p in ("plan_d2_run1.json", "plan_d2_pilot.json"):
    for s in json.loads((R / p).read_text(encoding="utf-8"))["slots"]:
        old = s["replaces"]
        gen = R / "anim" / old / "generated.json"
        acc = R / "anim" / s["id"] / "accepted.json"
        if not (gen.exists() and acc.exists() and old in eye):
            continue
        best = max((RANK[d2[t]] for t in json.loads(acc.read_text())["takes"] if t in d2), default=-1)
        if best > RANK[eye[old]]:
            beaten.append((RANK[eye[old]], old))
freed = 0
for _, old in sorted(beaten):
    if shutil.disk_usage(R).free >= TARGET:
        break
    g = R / "anim" / old / "generated.json"
    for f in json.loads(g.read_text())["frames"]:
        Path(f).unlink(missing_ok=True)
    g.rename(g.with_name("generated.replaced.json"))
    freed += 1
    print("freed", old, eye[old])
print(f"{freed} of {len(beaten)} beaten takes freed; {shutil.disk_usage(R).free / 1024 ** 3:.1f} GB free")
