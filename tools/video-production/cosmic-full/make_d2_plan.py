"""Build the Draft 2 render plan (slot file for animate_full.py) from the checked motion prompts.

usage: make_d2_plan.py <root> <prompts.json> <best-take.json> <out plan> [--only id,id,...] [--max-takes N]
Every slot with a new prompt becomes a gated slot "<id>-d2" that replaces the Draft 1 slot: same scene key, frame
range, length, step and composite as plan_v2, a new seed, the rebuild's negative prompt. A chained bar B starts from
its bar A's Draft 2 slot when A is re-rendered too, otherwise from A's best existing take (best-take.json).
--only limits the plan to a pilot subset (a bar B in the subset pulls in its bar A).
"""
import json, sys
from pathlib import Path

R = Path(sys.argv[1])
prompts = {p["slot"]: p["prompt"] for p in json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))}
best = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
OUT = R / sys.argv[4]
v2 = json.loads((R / "plan_v2.json").read_text(encoding="utf-8"))
S = {s["id"]: s for s in v2["slots"]}


def opt(flag):
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else None


only = set(opt("--only").split(",")) if opt("--only") else None
if only:
    only |= {S[x]["chain_from"] for x in list(only) if S[x].get("chain_from") in prompts}
NEG = ("frozen image, static subjects, slideshow, zoom, camera movement, cuts, collage, flicker, strobe, extra hands, "
       "distorted fingers, melting faces, text, watermark, subtitles, split screen, inset pictures, random duplicates, "
       "photorealistic, open mouth, screaming, eyes rolled back, extra arms, floating hands, melting body")
slots = []
for n, s in enumerate(v2["slots"]):
    sid = s["id"]
    if sid not in prompts or (only and sid not in only):
        continue
    d = {k: s[k] for k in ("scene", "key", "frames", "length", "step", "composite") if k in s}
    d.update(id=f"{sid}-d2", replaces=sid, prompt=prompts[sid], seed=143700000 + n * 10, gate=True)
    for k in ("portal_in", "dissolve_in", "push"):
        if k in s:
            d[k] = s[k]
    a = s.get("chain_from")
    if a:
        d["chain_from"] = f"{a}-d2" if a in prompts else best[a]["take"]
    slots.append(d)
plan = {k: v2[k] for k in ("title", "fps", "bpm", "phase", "style")}
plan.update(negative=NEG, slots=slots)
if opt("--max-takes"):
    plan["max_takes"] = int(opt("--max-takes"))
assert not OUT.exists(), f"{OUT} exists; never overwrite a render plan"
OUT.write_text(json.dumps(plan, indent=1, ensure_ascii=False), encoding="utf-8")
print(OUT, len(slots), "slots")
