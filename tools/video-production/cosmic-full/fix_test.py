"""Fix test: re-run a frozen Draft 1 workflow with ONE recorded change and measure the result.

usage: fix_test.py <root> <variants.json>
variants.json is a list of {"label", "slot", and any of "seed", "prompt", "image"}. "prompt" replaces the
whole positive text, "image" is a file name in the ComfyUI input folder. Everything else (model, size,
steps, CFG, sampler, decode) stays exactly as the slot's saved anim/<slot>/workflow.json. Outputs go to
cosmicfull-test/<label>/<slot>; nothing existing is overwritten. Results are appended to
<root>/freeze-test.json. A variant whose output folder already has frames is skipped (never resubmitted).
"""
import json, sys, time
from pathlib import Path
import requests

sys.path.insert(0, str(Path(__file__).parent))
from motion_check import motion

API = "http://127.0.0.1:8192"
C = Path("D:/AvasthaAI/VideoEditFour-20261025")
ROOT = Path(sys.argv[1])
variants = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
plan = {s["id"]: s for s in json.loads((ROOT / "plan_v2.json").read_text(encoding="utf-8"))["slots"]}
log_path = ROOT / "freeze-test.json"
for v in variants:
    sid, label = v["slot"], v["label"]
    out = C / "output" / "cosmicfull-test" / label / sid
    if any(out.glob("f_*.png")):
        print("skip, already rendered:", label, sid, flush=True)
        continue
    g = json.loads((ROOT / "anim" / sid / "workflow.json").read_text())
    if "seed" in v:
        g["9"]["inputs"]["seed"] = v["seed"]
    if "prompt" in v:
        g["4"]["inputs"]["text"] = v["prompt"]
    if "image" in v:
        assert (C / "input" / v["image"]).exists(), v["image"]
        g["6"]["inputs"]["image"] = v["image"]
    tag = f"cosmicfull-test/{label}/{sid}"
    g["90"]["inputs"]["filename_prefix"] = tag
    g["11"]["inputs"]["filename_prefix"] = f"{tag}/f"
    t0 = time.time()
    pid = requests.post(API + "/prompt", json={"prompt": g, "client_id": "fix-test"}, timeout=90).json()["prompt_id"]
    while True:
        h = requests.get(API + "/history/" + pid, timeout=90).json()
        if pid in h:
            assert h[pid]["status"]["status_str"] == "success", h[pid]["status"]
            break
        time.sleep(10)
    frames = sorted(out.glob("f_*.png"))
    m = motion(frames, plan[sid]["step"])
    row = {"label": label, "slot": sid, "seed": g["9"]["inputs"]["seed"], "image": g["6"]["inputs"]["image"],
           "change": {k: v[k] for k in ("seed", "prompt", "image") if k in v}, "frames": len(frames),
           "seconds": round(time.time() - t0), **m}
    log = json.loads(log_path.read_text()) if log_path.exists() else []
    log.append(row)
    log_path.write_text(json.dumps(log, indent=1))
    print(json.dumps(row), flush=True)
