"""Build a render plan that retakes every slot an assembly plan fills with a weak or still take.

usage: make_weak_plan.py <root> <assembly plan> <eye-review.json> <d2-review.json> <out plan> <suffix> <seed base>
                         [--max-takes N]
Each weak/still slot becomes "<slot><suffix>" (gated, "fixes": <slot>) with a new seed and the prompt of the take on
screen (for a Draft 1 take that has a Draft 2 prompt, the Draft 2 prompt). Chains stay consistent: a weak bar A is
retaken together with its bar B (B starts from the new A, so the pair can replace the old pair), a weak bar B alone
starts from the exact take the assembly chose for its bar A. Weak groups render before still ones, a bar A always
before its bar B. Style, negative and fps come from plan_d2_run1.json plus fixes2.json's negative_extra.
"""
import json, sys
from pathlib import Path

R = Path(sys.argv[1])
asm = json.loads((R / sys.argv[2]).read_text(encoding="utf-8"))["slots"]
eye = {x["clip"]: x["final"] for x in json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))}
d2 = json.loads(Path(sys.argv[4]).read_text(encoding="utf-8"))
OUT, SUFFIX, SEED = R / sys.argv[5], sys.argv[6], int(sys.argv[7])
HERE = Path(__file__).parent
d2p = {p["slot"]: p["prompt"] for p in json.loads((HERE / "d2-prompts.json").read_text(encoding="utf-8"))}
extra = json.loads((HERE / "fixes2.json").read_text(encoding="utf-8"))["negative_extra"]
base = json.loads((R / "plan_d2_run1.json").read_text(encoding="utf-8"))
diag = {r["id"]: r for r in json.loads((R / "draft1-motion-diagnosis.json").read_text())["rows"]}
RANK = {"moves": 3, "weak": 2, "still": 1, "broken": 0}


def gen(take):
    return json.loads((R / "anim" / take / "generated.json").read_text())


def verdict(s):
    t = s.get("take", s["id"])
    v = d2.get(t) or eye.get(t) or ("still" if diag.get(t, {}).get("frozen") else None)
    assert v, f"{s['id']}: no verdict for {t}"
    return v


def prompt(s):
    t = s.get("take", s["id"])
    if t in d2 or s["id"] not in d2p:
        return gen(t)["prompt"]
    return d2p[s["id"]]


S = {s["id"]: s for s in asm if not s.get("reused")}
kid = {s["chain_from"]: s["id"] for s in S.values() if s.get("chain_from")}
v = {sid: verdict(s) for sid, s in S.items()}
groups = []  # (worst verdict rank, position, [slot ids, bar A first])
for n, (sid, s) in enumerate(S.items()):
    if s.get("chain_from"):
        continue  # bar B: handled with its bar A
    b = kid.get(sid)
    if v[sid] in ("weak", "still"):
        g = [sid] + ([b] if b else [])
    elif b and v[b] in ("weak", "still"):
        g = [b]
    else:
        continue
    groups.append((min(RANK[v[x]] for x in g), n, g))
groups.sort(key=lambda x: (-x[0], x[1]))
slots, i = [], 0
for _, _, g in groups:
    for sid in g:
        s = {k: x for k, x in S[sid].items() if k not in ("take", "prompt")}
        s.update(id=sid + SUFFIX, fixes=sid, prompt=prompt(S[sid]), seed=SEED + 10 * i, gate=True)
        if s.get("chain_from"):
            a = s["chain_from"]
            s["chain_from"] = a + SUFFIX if a in g else S[a].get("take", a)
        slots.append(s)
        i += 1
plan = {k: base[k] for k in ("title", "fps", "bpm", "phase", "style")}
plan.update(negative=base["negative"] + ", " + extra, slots=slots)
if "--max-takes" in sys.argv:
    plan["max_takes"] = int(sys.argv[sys.argv.index("--max-takes") + 1])
assert not OUT.exists(), f"{OUT} exists; never overwrite a render plan"
OUT.write_text(json.dumps(plan, indent=1, ensure_ascii=False), encoding="utf-8")
counts = {}
for x in v.values():
    counts[x] = counts.get(x, 0) + 1
print(OUT, len(slots), "slots from", len(groups), "groups; on screen now:", counts)
print("forced bar B (moves now, retaken to follow a new bar A):", [s["fixes"] for s in slots if v[s["fixes"]] == "moves"])
