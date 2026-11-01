"""Controlled freeze test: re-run Draft 1 workflows that froze, unchanged, and measure the result.

usage: freeze_test.py <root> <label> <slot> [<slot> ...]
Each slot's saved anim/<slot>/workflow.json is submitted exactly as it ran in Draft 1 (same seed, model,
settings, keyframe), except that its outputs go to cosmicfull-test/<label>/<slot> so nothing existing
is overwritten. Results are appended to <root>/freeze-test.json.
"""
import json, sys, time
from pathlib import Path
import requests

sys.path.insert(0, str(Path(__file__).parent))
from motion_check import motion

API = "http://127.0.0.1:8192"
C = Path("D:/AvasthaAI/VideoEditFour-20261025")
ROOT, LABEL = Path(sys.argv[1]), sys.argv[2]
plan = {s["id"]: s for s in json.loads((ROOT / "plan_v2.json").read_text(encoding="utf-8"))["slots"]}
log_path = ROOT / "freeze-test.json"
log = json.loads(log_path.read_text()) if log_path.exists() else []
for sid in sys.argv[3:]:
    g = json.loads((ROOT / "anim" / sid / "workflow.json").read_text())
    tag = f"cosmicfull-test/{LABEL}/{sid}"
    g["90"]["inputs"]["filename_prefix"] = tag
    g["11"]["inputs"]["filename_prefix"] = f"{tag}/f"
    t0 = time.time()
    pid = requests.post(API + "/prompt", json={"prompt": g, "client_id": "freeze-test"}, timeout=90).json()["prompt_id"]
    while True:
        h = requests.get(API + "/history/" + pid, timeout=90).json()
        if pid in h:
            assert h[pid]["status"]["status_str"] == "success", h[pid]["status"]
            break
        time.sleep(10)
    frames = sorted((C / "output" / "cosmicfull-test" / LABEL / sid).glob("f_*.png"))
    m = motion(frames, plan[sid]["step"])
    row = {"label": LABEL, "slot": sid, "seed": g["9"]["inputs"]["seed"], "frames": len(frames),
           "seconds": round(time.time() - t0), **m}
    log.append(row)
    log_path.write_text(json.dumps(log, indent=1))
    print(json.dumps(row), flush=True)
