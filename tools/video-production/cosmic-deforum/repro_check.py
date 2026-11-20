"""Proof that the two approved Deforum styles (one-picture and hybrid, Cosmic Rain tests 2026-11-13) can be reproduced (Adi 2026-11-18: "I truly hope we can reproduce
them at any time").

usage: repro_check.py <out.json> <run root> [<run root> ...] [--frames 45]     (F:/AvasthaAI/venv python; Deforum on 7865)
For each run root (a finished deforum run with settings.json + last-job.json) the first N frames are drawn again from
the saved settings on DreamShaper8, then compared with the original frames: mean absolute difference per frame on
0-255 pixels (0 = identical).
"""
import json, sys, time, urllib.request
from pathlib import Path
import numpy as np
from PIL import Image

API = "http://127.0.0.1:7865"
args = sys.argv[1:]
N = 45
if "--frames" in args:
    i = args.index("--frames"); N = int(args[i + 1]); del args[i:i + 2]
OUT, ROOTS = Path(args[0]), [Path(a) for a in args[1:]]


def api(path, data=None, timeout=120, method=None):
    req = urllib.request.Request(API + path, data=None if data is None else json.dumps(data).encode(),
                                 headers={"Content-Type": "application/json"}, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as f:
        return json.load(f)


if api("/sdapi/v1/options")["sd_model_checkpoint"].split(" ")[0] != "DreamShaper_8_pruned.safetensors":
    api("/sdapi/v1/options", {"sd_model_checkpoint": "DreamShaper_8_pruned.safetensors"}, timeout=600)
result = {"frames": N, "runs": []}
for root in ROOTS:
    s = json.loads((root / "settings.json").read_text())
    orig = json.loads((root / "last-job.json").read_text())
    s.update(max_frames=N, batch_name="Repro_" + root.name)
    job_id = api("/deforum_api/batches", {"deforum_settings": s, "options_overrides": {"img2img_fix_steps": True}}, timeout=180)["job_ids"][0]
    t0 = time.time()
    while True:
        job = api(f"/deforum_api/jobs/{job_id}", timeout=30)
        if job["status"] in ("SUCCEEDED", "FAILED", "CANCELLED"):
            break
        time.sleep(10)
    run = {"root": str(root), "status": job["status"], "seconds": round(time.time() - t0)}
    if job["status"] == "SUCCEEDED":
        diffs = []
        for k in range(N):
            a = np.asarray(Image.open(Path(orig["outdir"]) / f"{orig['timestring']}_{k:09d}.png").convert("RGB"), np.float32)
            b = np.asarray(Image.open(Path(job["outdir"]) / f"{job['timestring']}_{k:09d}.png").convert("RGB"), np.float32)
            diffs.append(round(float(np.abs(a - b).mean()), 2))
        run.update(outdir=job["outdir"], timestring=job["timestring"], mean_abs_diff=diffs, worst=max(diffs))
    result["runs"].append(run)
    OUT.write_text(json.dumps(result, indent=1))
result["done"] = True
OUT.write_text(json.dumps(result, indent=1))
