"""Build a render plan for bars an assembly plan could not fill well (broken takes, bar B not continuing bar A).

usage: make_bar_fix_plan.py <root> <assembly plan> <fixes.json> <out plan> <suffix> <seed base> [--max-takes N]
fixes.json: {"slots": {"<slot>": "<prompt>", ...}, "negative_extra": "..."}
Every listed slot becomes "<slot><suffix>" (gated) with "fixes": <slot> and the scene, key, length, step, composite
and push of the assembly plan's slot. A chained bar B starts from the exact take the assembly chose for its bar A,
so the new B continues the picture that is really on screen. Style, negative and fps come from plan_d2_run1.json.
"""
import json, sys
from pathlib import Path

R = Path(sys.argv[1])
asm = {s["id"]: s for s in json.loads((R / sys.argv[2]).read_text(encoding="utf-8"))["slots"]}
fx = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
OUT, SUFFIX, SEED = R / sys.argv[4], sys.argv[5], int(sys.argv[6])
base = json.loads((R / "plan_d2_run1.json").read_text(encoding="utf-8"))
slots = []
for i, (sid, prompt) in enumerate(fx["slots"].items()):
    s = {k: v for k, v in asm[sid].items() if k not in ("take", "prompt")}
    s.update(id=sid + SUFFIX, fixes=sid, prompt=prompt, seed=SEED + 10 * i, gate=True)
    if s.get("chain_from"):
        a = asm[s["chain_from"]]
        s["chain_from"] = a.get("take", a["id"])
    slots.append(s)
plan = {k: base[k] for k in ("title", "fps", "bpm", "phase", "style")}
plan.update(negative=base["negative"] + ", " + fx["negative_extra"], slots=slots)
if "--max-takes" in sys.argv:
    plan["max_takes"] = int(sys.argv[sys.argv.index("--max-takes") + 1])
assert not OUT.exists(), f"{OUT} exists; never overwrite a render plan"
OUT.write_text(json.dumps(plan, indent=1, ensure_ascii=False), encoding="utf-8")
print(OUT, len(slots), "slots:", ", ".join(f"{s['id']}<-{s.get('chain_from', 'still')}" for s in slots))
