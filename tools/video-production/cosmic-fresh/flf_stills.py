"""Start and landing stills for first+last-frame shots (DreamShaperXL Turbo on the A2000 ComfyUI).

usage: flf_stills.py <root>        (reads <root>/stills.json, writes <root>/raw/<id>.png and adds them to picks.json)
stills.json: a list of
  {"id": ..., "prompt": ..., "seed": n}                              a new still
  {"id": ..., "from": <id>, "prompt": ..., "denoise": 0.55, "seed": n}  a variant of an earlier still (img2img), so the
                                                                       landing frame keeps the same people and place
Settings as the approved Cosmic Rain stills. Idempotent: existing PNGs are kept.
"""
import json, shutil, sys, time, urllib.parse, urllib.request
from pathlib import Path

API = "http://127.0.0.1:8192"
INPUT = Path("D:/AvasthaAI/VideoEditFour-20261025/input")
R = Path(sys.argv[1])
(R / "raw").mkdir(exist_ok=True)
spec = json.loads((R / "stills.json").read_text(encoding="utf-8"))
STYLE = json.loads((R / "plan.json").read_text(encoding="utf-8"))["style"] if (R / "plan.json").exists() else ""
NEG = ("photo, photorealistic, 3d render, realistic skin, blurry, soft focus, muddy colors, text, letters, watermark, "
       "logo, signature, frame, border, extra limbs, extra fingers, deformed hands, deformed face, ugly, lowres, jpeg "
       "artifacts, cropped head, collage, split panels, open mouth, screaming")


def call(path, body=None):
    req = urllib.request.Request(f"{API}/{path}", data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def graph(x, prefix):
    n = lambda t, **kw: {"class_type": t, "inputs": kw}
    g = {"1": n("CheckpointLoaderSimple", ckpt_name="DreamShaperXL_Turbo_v2.safetensors"),
         "2": n("CLIPTextEncode", text=f"{x['prompt']}, {STYLE}", clip=["1", 1]), "3": n("CLIPTextEncode", text=NEG, clip=["1", 1]),
         "4": n("EmptyLatentImage", width=1360, height=768, batch_size=1),
         "5": n("KSampler", model=["1", 0], positive=["2", 0], negative=["3", 0], latent_image=["4", 0], seed=x["seed"],
                steps=8, cfg=2.0, sampler_name="dpmpp_sde", scheduler="karras", denoise=1.0),
         "6": n("VAEDecode", samples=["5", 0], vae=["1", 2]), "7": n("SaveImage", images=["6", 0], filename_prefix=prefix)}
    if x.get("from"):
        name = f"flf-{x['from']}.png"
        shutil.copy2(R / "raw" / f"{x['from']}.png", INPUT / name)
        g["8"] = n("LoadImage", image=name)
        g["4"] = n("VAEEncode", pixels=["8", 0], vae=["1", 2])
        g["5"]["inputs"]["denoise"] = x.get("denoise", 0.55)
        g["5"]["inputs"]["steps"] = 10
    return g


picks = json.loads((R / "picks.json").read_text(encoding="utf-8")) if (R / "picks.json").exists() else {}
for x in spec:
    out = R / "raw" / f"{x['id']}.png"
    if not out.exists():
        pid = json.loads(call("prompt", {"prompt": graph(x, f"flf/{R.name}/{x['id']}")}))["prompt_id"]
        while True:
            h = json.loads(call(f"history/{pid}")).get(pid)
            if h and h.get("status", {}).get("completed"):
                break
            if h and h.get("status", {}).get("status_str") == "error":
                raise RuntimeError(json.dumps(h["status"])[:2000])
            time.sleep(1)
        img = next(i for o in h["outputs"].values() for i in o.get("images", []))
        q = urllib.parse.urlencode({"filename": img["filename"], "subfolder": img["subfolder"], "type": img["type"]})
        out.write_bytes(call(f"view?{q}"))
        print("still", x["id"], flush=True)
    picks[x["id"]] = out.name
(R / "picks.json").write_text(json.dumps(picks, indent=1), encoding="utf-8")
print("stills done", len(spec))
