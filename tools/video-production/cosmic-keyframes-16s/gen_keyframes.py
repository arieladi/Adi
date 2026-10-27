"""Generate keyframe stills for the Cosmic Rain 16s test through the A2000 ComfyUI (port 8192).

Runs on the studio box with D:/AvasthaAI/venv python. Idempotent: an existing PNG is never
regenerated. The A1111 webui on 7865 returned HTTP 500 for every txt2img on 2026-10-27, so it is
not used. Checkpoints are hard links in ComfyUI/models/checkpoints to the existing webui files.
"""
import json, sys, time, urllib.parse, urllib.request
from pathlib import Path

API = "http://127.0.0.1:8192"
ROOT = Path(sys.argv[1])
board = json.loads((ROOT / "storyboard.json").read_text(encoding="utf-8"))
raw = ROOT / "raw"
raw.mkdir(exist_ok=True)

MODELS = {
    "xl": {"ckpt": "DreamShaperXL_Turbo_v2.safetensors", "w": 1360, "h": 768, "steps": 8, "cfg": 2.0,
           "sampler": "dpmpp_sde", "scheduler": "karras", "seeds": [11, 22, 33, 44], "scenes": None},
    "sd15": {"ckpt": "DreamShaper_8_pruned.safetensors", "w": 912, "h": 512, "steps": 28, "cfg": 6.5,
             "sampler": "dpmpp_2m", "scheduler": "karras", "seeds": [11, 22], "scenes": ["S01", "S06", "S07"]},
}


def call(path, body=None, timeout=60):
    req = urllib.request.Request(f"{API}/{path}", data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def status(**kw):
    (ROOT / "status.json").write_text(json.dumps({"time": time.time(), **kw}, indent=2))


def graph(m, pos, neg, seed, prefix):
    return {
        "1": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": m["ckpt"]}},
        "2": {"class_type": "CLIPTextEncode", "inputs": {"text": pos, "clip": ["1", 1]}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": neg, "clip": ["1", 1]}},
        "4": {"class_type": "EmptyLatentImage", "inputs": {"width": m["w"], "height": m["h"], "batch_size": 1}},
        "5": {"class_type": "KSampler", "inputs": {"model": ["1", 0], "positive": ["2", 0], "negative": ["3", 0],
                                                   "latent_image": ["4", 0], "seed": seed, "steps": m["steps"],
                                                   "cfg": m["cfg"], "sampler_name": m["sampler"],
                                                   "scheduler": m["scheduler"], "denoise": 1.0}},
        "6": {"class_type": "VAEDecode", "inputs": {"samples": ["5", 0], "vae": ["1", 2]}},
        "7": {"class_type": "SaveImage", "inputs": {"images": ["6", 0], "filename_prefix": prefix}},
    }


receipt_path = ROOT / "receipt.json"
receipt = json.loads(receipt_path.read_text()) if receipt_path.exists() else {}
receipt.setdefault("comfy_images", [])
for key, m in MODELS.items():
    for sc in board["scenes"]:
        if m["scenes"] and sc["id"] not in m["scenes"]:
            continue
        for seed in m["seeds"]:
            name = f"{sc['id']}_{key}_s{seed}"
            out = raw / f"{name}.png"
            if out.exists():
                continue
            status(stage="generating", model=key, scene=sc["id"], seed=seed)
            t = time.time()
            pid = json.loads(call("prompt", {"prompt": graph(m, f"{sc['subject']}, {board['style']}",
                                                             board["negative"], seed, f"cosmickf16/{name}")}))["prompt_id"]
            while True:
                h = json.loads(call(f"history/{pid}")).get(pid)
                if h and h.get("status", {}).get("completed"):
                    break
                if h and h.get("status", {}).get("status_str") == "error":
                    raise RuntimeError(json.dumps(h["status"])[:2000])
                time.sleep(2)
            img = next(i for o in h["outputs"].values() for i in o.get("images", []))
            q = urllib.parse.urlencode({"filename": img["filename"], "subfolder": img["subfolder"], "type": img["type"]})
            out.write_bytes(call(f"view?{q}"))
            receipt["comfy_images"].append({"file": out.name, "prompt_id": pid, "seconds": round(time.time() - t, 1)})
            receipt_path.write_text(json.dumps(receipt, indent=2))
status(stage="complete", images=len(list(raw.glob("*.png"))))
print("done", len(receipt["comfy_images"]), "images in receipt")
