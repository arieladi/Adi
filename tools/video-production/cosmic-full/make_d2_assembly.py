"""Build the Draft 2 assembly plan: Draft 1's timeline (plan_v2.json) with every slot pointing at its best take.

usage: make_d2_assembly.py <root> <eye-review.json> <d2-review.json> <out plan>
Candidates per slot: its Draft 1 take, its redo1 take and its Draft 2 takes (<slot>-d2, -d2t2 ...), each with its eye
verdict (eye-review.json for Draft 1/redo1, d2-review.json {take: verdict} for Draft 2; a Draft 1 take without frames
that the encoded-video check found frozen counts as still). A slot alone takes its best verdict (ties: the newer
render). A chained bar A + bar B are chosen together, only from pairs where B really starts on A's last frame, so the
picture never jumps back at the bar line; the pair with the best combined verdict wins. Prints every slot that does
not end on "moves" and any pair that could not be made consistent. Timing, symmetry, portals and pushes stay as Draft 1.
"""
import json, sys
from itertools import product
from pathlib import Path

R = Path(sys.argv[1])
eye = {x["clip"]: x["final"] for x in json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))}
d2 = json.loads(Path(sys.argv[3]).read_text(encoding="utf-8"))
OUT = R / sys.argv[4]
plan = json.loads((R / "plan_v2.json").read_text(encoding="utf-8"))
diag = {r["id"]: r for r in json.loads((R / "draft1-motion-diagnosis.json").read_text())["rows"]}
redo = {s["replaces"]: s["id"] for s in json.loads((R / "plan_redo1.json").read_text(encoding="utf-8"))["slots"]}
RANK = {"moves": 3, "weak": 2, "still": 1, "broken": 0}
NAME = {v: k for k, v in RANK.items()}


def gen(take):
    g = R / "anim" / take / "generated.json"
    return json.loads(g.read_text()) if g.exists() else None


def candidates(sid):
    out = []  # (rank, age, take); higher age = newer render
    v = eye.get(sid) or ("still" if diag.get(sid, {}).get("frozen") else None)
    if v and gen(sid):
        out.append((RANK[v], 0, sid))
    if sid in redo and redo[sid] in eye and gen(redo[sid]):
        out.append((RANK[eye[redo[sid]]], 1, redo[sid]))
    acc = R / "anim" / f"{sid}-d2" / "accepted.json"
    if acc.exists():
        for i, t in enumerate(json.loads(acc.read_text())["takes"]):
            if t in d2 and gen(t):
                out.append((RANK[d2[t]], 2 + i, t))
    return out


def continues(b, a):
    gb, ga = gen(b), gen(a)
    return bool(gb and ga and Path(gb["keyframe"]) == Path(ga["frames"][-1]))


S = {s["id"]: s for s in plan["slots"]}
choice, notes = {}, []
for s in plan["slots"]:
    sid = s["id"]
    if s.get("reused") or sid in choice:
        continue
    kids = [k for k in plan["slots"] if k.get("chain_from") == sid]
    ca = candidates(sid)
    assert ca, f"{sid}: no usable take"
    if not kids:
        choice[sid] = max(ca)
        continue
    b = kids[0]["id"]
    cb = candidates(b)
    pairs = [(x[0] + y[0], min(x[0], y[0]), x[1] + y[1], x, y) for x, y in product(ca, cb) if continues(y[2], x[2])]
    if pairs:
        _, _, _, x, y = max(pairs)
        choice[sid], choice[b] = x, y
    else:
        choice[sid], choice[b] = max(ca), max(cb)
        notes.append(f"NO CONSISTENT PAIR: {sid} ({choice[sid][2]}) -> {b} ({choice[b][2]}); render a bar B from the chosen bar A")
for sid, (rank, _, take) in choice.items():
    if take != sid:
        S[sid]["take"] = take
assert not OUT.exists(), f"{OUT} exists; never overwrite an assembly plan"
OUT.write_text(json.dumps(plan, indent=1, ensure_ascii=False), encoding="utf-8")
counts = {}
for rank, _, _ in choice.values():
    counts[NAME[rank]] = counts.get(NAME[rank], 0) + 1
print(OUT, counts)
for n in notes:
    print(n)
for sid, (rank, _, take) in choice.items():
    if rank < RANK["moves"]:
        print(f"  {sid}: {take} ({NAME[rank]})")
