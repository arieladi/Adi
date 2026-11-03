"""Free disk by removing PNG frames of Draft 2 takes the assembly did not choose and that lost by eye (latents stay).

usage: free_unchosen.py <root> <assembly plan> <d2-review.json> <target free GB>
A take <slot>-d2[tN] / -d2r[tN] is freed only when its slot's chosen take (the assembly plan's "take") has a strictly
better d2-review verdict, it is no chain source of any plan, and it is not the chosen take itself; worst verdict first,
until the target is free. Frames deleted, generated.json renamed generated.replaced.json, latent kept for a re-decode.
"""
import json, shutil, sys
from pathlib import Path

R = Path(sys.argv[1])
asm = json.loads((R / sys.argv[2]).read_text(encoding="utf-8"))["slots"]
d2 = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
TARGET = float(sys.argv[4]) * 1024 ** 3
RANK = {"moves": 3, "weak": 2, "still": 1, "broken": 0}
chosen = {s["id"]: s.get("take", s["id"]) for s in asm}
sources = set(chosen.values())
for p in R.glob("plan*.json"):
    for s in json.loads(p.read_text(encoding="utf-8")).get("slots", []):
        if s.get("chain_from"):
            sources.add(s["chain_from"])
lost = []
for sid, take in chosen.items():
    if take not in d2:
        continue
    for suffix in ("-d2", "-d2r"):
        acc = R / "anim" / f"{sid}{suffix}" / "accepted.json"
        if not acc.exists():
            continue
        for t in json.loads(acc.read_text())["takes"]:
            if t in d2 and t not in sources and RANK[d2[t]] < RANK[d2[take]] and (R / "anim" / t / "generated.json").exists():
                lost.append((RANK[d2[t]], t))
freed = 0
for _, t in sorted(lost):
    if shutil.disk_usage(R).free >= TARGET:
        break
    g = R / "anim" / t / "generated.json"
    for f in json.loads(g.read_text())["frames"]:
        Path(f).unlink(missing_ok=True)
    g.rename(g.with_name("generated.replaced.json"))
    freed += 1
    print("freed", t, d2[t])
print(f"{freed} of {len(lost)} losing takes freed; {shutil.disk_usage(R).free / 1024 ** 3:.1f} GB free")
