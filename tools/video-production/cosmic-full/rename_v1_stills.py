"""One-off: rename stills made under plan v1 scene ids (K###_s#.png) to the stable subject keys of plan v2.

usage: rename_v1_stills.py <root> <plan-v1.json>
Only stills whose subject still exists in plan.json are renamed; the others are left untouched, and an
existing target is never overwritten.
"""
import json, sys
from pathlib import Path

ROOT = Path(sys.argv[1])
v1 = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
v2 = json.loads((ROOT / "plan.json").read_text(encoding="utf-8"))
key_of = {sc["subject"]: sc["key"] for sc in v2["scenes"]}
moved = kept = 0
for sc in v1["scenes"]:
    key = key_of.get(sc["subject"])
    for p in sorted((ROOT / "raw").glob(f"{sc['id']}_s*.png")):
        dst = p.with_name(f"{key}_{p.stem.split('_')[-1]}.png") if key else None
        if dst and not dst.exists():
            p.rename(dst)
            moved += 1
        else:
            kept += 1
print("renamed", moved, "left as-is", kept)
