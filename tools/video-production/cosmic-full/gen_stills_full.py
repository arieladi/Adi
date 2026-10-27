"""Keyframe stills for every scene of the full-track plan (DreamShaperXL Turbo on the A2000 ComfyUI).

usage: gen_stills_full.py <root> [seeds, default 1,2]   (root holds plan.json; writes raw/<scene>_s<seed>.png)
Same settings as the approved 16s test stills. Idempotent: existing PNGs are kept. Writes stills-status.json.
"""
import json, sys, time, urllib.parse, urllib.request
from pathlib import Path

API = "http://127.0.0.1:8192"
ROOT = Path(sys.argv[1])
SEEDS = [int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "1,2").split(",")]
plan = json.loads((ROOT / "plan.json").read_text(encoding="utf-8"))
raw = ROOT / "raw"
raw.mkdir(exist_ok=True)
NEG = ("photo, photorealistic, 3d render, realistic skin, blurry, soft focus, muddy colors, text, letters, watermark, "
       "logo, signature, frame, border, extra limbs, extra fingers, deformed hands, deformed face, ugly, lowres, jpeg "
       "artifacts, cropped head, collage, split panels, open mouth, screaming")


def call(path, body=None, timeout=60):
    req = urllib.request.Request(f"{API}/{path}", data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def graph(pos, seed, prefix):
    n = lambda t, **kw: {"class_type": t, "inputs": kw}
    return {"1": n("CheckpointLoaderSimple", ckpt_name="DreamShaperXL_Turbo_v2.safetensors"),
            "2": n("CLIPTextEncode", text=pos, clip=["1", 1]), "3": n("CLIPTextEncode", text=NEG, clip=["1", 1]),
            "4": n("EmptyLatentImage", width=1360, height=768, batch_size=1),
            "5": n("KSampler", model=["1", 0], positive=["2", 0], negative=["3", 0], latent_image=["4", 0], seed=seed,
                   steps=8, cfg=2.0, sampler_name="dpmpp_sde", scheduler="karras", denoise=1.0),
            "6": n("VAEDecode", samples=["5", 0], vae=["1", 2]), "7": n("SaveImage", images=["6", 0], filename_prefix=prefix)}


done = 0
for i, sc in enumerate(plan["scenes"]):
    for seed in SEEDS:
        out = raw / f"{sc['id']}_s{seed}.png"
        if out.exists():
            continue
        (ROOT / "stills-status.json").write_text(json.dumps({"time": time.time(), "stage": "stills", "scene": sc["id"],
                                                             "n": i + 1, "total": len(plan["scenes"])}))
        pid = json.loads(call("prompt", {"prompt": graph(f"{sc['subject']}, {plan['style']}", 9000 + 97 * i + seed,
                                                         f"cosmicfull/{sc['id']}_s{seed}")}))["prompt_id"]
        while True:
            h = json.loads(call(f"history/{pid}")).get(pid)
            if h and h.get("status", {}).get("completed"):
                break
            if h and h.get("status", {}).get("status_str") == "error":
                raise RuntimeError(json.dumps(h["status"])[:2000])
            time.sleep(1)
        img = next(x for o in h["outputs"].values() for x in o.get("images", []))
        q = urllib.parse.urlencode({"filename": img["filename"], "subfolder": img["subfolder"], "type": img["type"]})
        out.write_bytes(call(f"view?{q}"))
        done += 1
(ROOT / "stills-status.json").write_text(json.dumps({"time": time.time(), "stage": "stills-complete", "new": done}))
print("stills done", done)
