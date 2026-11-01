"""Contact sheets for a full eye review: every Draft 1 take that still has frames, plus redo1 takes.

usage: review_sheets.py <root> <out dir>
Writes <out dir>/d1-NN.jpg and redo-NN.jpg (4 clips each, timeline order, 5 frames per clip first to last)
and <out dir>/manifest.json: {sheet: [{clip, slot, source, step, prompt}]}. CPU only.
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw

R, OUT = Path(sys.argv[1]), Path(sys.argv[2])
OUT.mkdir(parents=True, exist_ok=True)
v2 = json.loads((R / "plan_v2.json").read_text(encoding="utf-8"))["slots"]
redo = json.loads((R / "plan_redo1.json").read_text(encoding="utf-8"))["slots"]
TW, TH, N, PER = 320, 184, 5, 4


def frames_of(take):
    g = R / "anim" / take / "generated.json"
    if not g.exists():
        return None
    f = json.loads(g.read_text())["frames"]
    return f if f and all(Path(p).exists() for p in f) else None


groups = {"d1": [(s["id"], s["id"], "draft1", s) for s in v2 if not s.get("reused")],
          "redo": [(s["id"], s["replaces"], "redo1", s) for s in redo]}
manifest = {}
for prefix, clips in groups.items():
    clips = [(c, slot, src, s, f) for c, slot, src, s in clips if (f := frames_of(c))]
    for k in range(0, len(clips), PER):
        batch = clips[k:k + PER]
        name = f"{prefix}-{k // PER + 1:02d}.jpg"
        sheet = Image.new("RGB", (TW * N, (TH + 18) * len(batch)), (16, 16, 16))
        d = ImageDraw.Draw(sheet)
        for r, (c, slot, src, s, f) in enumerate(batch):
            y = r * (TH + 18)
            d.text((4, y + 3), f"{c}  ({src}, step {s['step']})", fill=(255, 220, 120))
            for i in range(N):
                im = Image.open(f[round(i * (len(f) - 1) / (N - 1))]).convert("RGB")
                sheet.paste(im.resize((TW, TH), Image.BILINEAR), (i * TW, y + 18))
        sheet.save(OUT / name, quality=88)
        manifest[name] = [{"clip": c, "slot": slot, "source": src, "step": s["step"], "prompt": s["prompt"],
                           "composite": s["composite"]} for c, slot, src, s, f in batch]
        print(name, [b[0] for b in batch], flush=True)
(OUT / "manifest.json").write_text(json.dumps(manifest, indent=1))
