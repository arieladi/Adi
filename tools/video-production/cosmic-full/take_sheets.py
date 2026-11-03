"""Review sheets for rendered takes of a gated plan (e.g. Draft 2): 4 takes per sheet, 9 frames per take.

usage: take_sheets.py <root> <out dir> <plan file> [take ...]
Without take ids, every take listed in the plan's accepted.json files is included (including failed takes).
Writes <out dir>/t-NN.jpg, t-NN.txt (each take's slot, step and prompt, top to bottom) and ids.json {sheet: "a,b,c"}.
CPU only.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw

R, OUT = Path(sys.argv[1]), Path(sys.argv[2])
plan = json.loads((R / sys.argv[3]).read_text(encoding="utf-8"))
OUT.mkdir(parents=True, exist_ok=True)
slot_of = {}
takes = sys.argv[4:]
for s in plan["slots"]:
    acc = R / "anim" / s["id"] / "accepted.json"
    names = json.loads(acc.read_text())["takes"] if acc.exists() else []
    for t in names:
        slot_of[t] = s
    if not sys.argv[4:]:
        takes += names
TW, TH, N, PER = 256, 147, 9, 4
ids = {}
for k in range(0, len(takes), PER):
    batch = takes[k:k + PER]
    name = f"t-{k // PER + 1:02d}"
    sheet = Image.new("RGB", (TW * N, (TH + 18) * len(batch)), (16, 16, 16))
    d = ImageDraw.Draw(sheet)
    lines = []
    for r, t in enumerate(batch):
        g = json.loads((R / "anim" / t / "generated.json").read_text())
        f, s = g["frames"], slot_of[t]
        y = r * (TH + 18)
        d.text((4, y + 3), f"{t}  (step {s['step']}, frames 1..{len(f)} left to right)", fill=(255, 220, 120))
        for i in range(N):
            im = Image.open(f[round(i * (len(f) - 1) / (N - 1))]).convert("RGB")
            sheet.paste(im.resize((TW, TH), Image.BILINEAR), (i * TW, y + 18))
        lines.append(f"- {t} (slot {s.get('replaces') or s['fixes']},step {s['step']}, composite {s['composite']}"
                     + (f", continues from {s['chain_from']}" if s.get("chain_from") else "") + f"): prompt \"{g.get('prompt', s['prompt'])}\"")
    sheet.save(OUT / f"{name}.jpg", quality=88)
    (OUT / f"{name}.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    ids[f"{name}.jpg"] = ",".join(batch)
(OUT / "ids.json").write_text(json.dumps(ids))
print(json.dumps(ids))
