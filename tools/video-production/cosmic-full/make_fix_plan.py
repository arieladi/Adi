"""Build a retake plan for Draft 2 slots that the eye review found broken.

usage: make_fix_plan.py <root> <fixes.json> <out plan> [--max-takes N]
fixes.json: {"slots": {"<slot>": "<new prompt or empty to keep the old one>", ...}, "negative_extra": "..."}
Every listed slot becomes "<slot>-d2r" with "fixes": <slot>, a new seed and the same scene, length, step and
composite as its "<slot>-d2" slot (from plan_d2_run1.json / plan_d2_pilot.json). A bar B starts from its bar A's
retake when A is listed too, otherwise from A's accepted Draft 2 take. "fixes" (not "replaces") so the old takes'
frames are never freed: they stay candidates for the assembly.
"""
import json, sys
from pathlib import Path

R = Path(sys.argv[1])
fx = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
OUT = R / sys.argv[3]
plans = [json.loads((R / p).read_text(encoding="utf-8")) for p in ("plan_d2_run1.json", "plan_d2_pilot.json")]
D2 = {}
for p in plans:
    for s in p["slots"]:
        D2.setdefault(s["replaces"], s)
slots = []
for sid, prompt in fx["slots"].items():
    s = dict(D2[sid])
    del s["replaces"]
    s.update(id=f"{sid}-d2r", fixes=sid, prompt=prompt or s["prompt"], seed=s["seed"] + 5000000)
    a = s.get("chain_from")
    if a:
        a0 = a[:-3]  # "<A>-d2" -> "<A>"
        s["chain_from"] = f"{a0}-d2r" if a0 in fx["slots"] else a
    slots.append(s)
plan = {k: plans[0][k] for k in ("title", "fps", "bpm", "phase", "style")}
plan.update(negative=plans[0]["negative"] + ", " + fx["negative_extra"], slots=slots)
if "--max-takes" in sys.argv:
    plan["max_takes"] = int(sys.argv[sys.argv.index("--max-takes") + 1])
assert not OUT.exists(), f"{OUT} exists; never overwrite a render plan"
OUT.write_text(json.dumps(plan, indent=1, ensure_ascii=False), encoding="utf-8")
print(OUT, len(slots), "slots:", ", ".join(f"{s['id']}<-{s.get('chain_from', 'still')}" for s in slots))
