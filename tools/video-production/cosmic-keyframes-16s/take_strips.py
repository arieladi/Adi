"""Contact strip of every Nth frame for animated takes, for review before assembly.

usage: take_strips.py <root> <take> [<take> ...]   -> <root>/review/<take>.jpg
"""
import json, sys
from pathlib import Path
from PIL import Image

ROOT = Path(sys.argv[1])
(ROOT / "review").mkdir(exist_ok=True)
for take in sys.argv[2:]:
    frames = json.loads((ROOT / "anim" / take / "generated.json").read_text())["frames"]
    pick = [frames[round(i * (len(frames) - 1) / 9)] for i in range(10)]
    tw, th = 426, 245
    sheet = Image.new("RGB", (5 * (tw + 4), 2 * (th + 4)), (12, 12, 12))
    for i, p in enumerate(pick):
        sheet.paste(Image.open(p).convert("RGB").resize((tw, th)), ((i % 5) * (tw + 4), (i // 5) * (th + 4)))
    sheet.save(ROOT / "review" / f"{take}.jpg", quality=85)
    print(take, len(frames))
