"""Generate redo stills from a job file (txt2img or img2img) through the A2000 ComfyUI (port 8192).

usage: gen_redo.py <root> <redo json>
Same model/settings as gen_keyframes.py (DreamShaperXL Turbo). Idempotent: existing PNGs are kept.
img2img inits are uploaded from <root>/raw to ComfyUI's input folder under cosmickf16/.
"""
import json, sys, time, urllib.parse, urllib.request, uuid
from pathlib import Path

API = "http://127.0.0.1:8192"
ROOT, SPEC = Path(sys.argv[1]), Path(sys.argv[2])
board = json.loads((ROOT / "storyboard.json").read_text(encoding="utf-8"))
spec = json.loads(SPEC.read_text(encoding="utf-8"))
raw = ROOT / "raw"
M = {"ckpt": "DreamShaperXL_Turbo_v2.safetensors", "w": 1360, "h": 768, "steps": 8, "cfg": 2.0,
     "sampler": "dpmpp_sde", "scheduler": "karras"}


def call(path, body=None, timeout=60, raw_body=None, ctype="application/json"):
    data = raw_body if raw_body is not None else (None if body is None else json.dumps(body).encode())
    req = urllib.request.Request(f"{API}/{path}", data=data, headers={"Content-Type": ctype})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def upload(png):
    b = uuid.uuid4().hex
    parts = (f"--{b}\r\nContent-Disposition: form-data; name=\"subfolder\"\r\n\r\ncosmickf16\r\n"
             f"--{b}\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n"
             f"--{b}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"{png.name}\"\r\n"
             f"Content-Type: image/png\r\n\r\n").encode() + png.read_bytes() + f"\r\n--{b}--\r\n".encode()
    r = json.loads(call("upload/image", raw_body=parts, ctype=f"multipart/form-data; boundary={b}"))
    return f"{r['subfolder']}/{r['name']}" if r.get("subfolder") else r["name"]


def graph(pos, neg, seed, prefix, init=None, denoise=1.0):
    g = {
        "1": {"class_type": "CheckpointLoaderSimple", "inputs": {"ckpt_name": M["ckpt"]}},
        "2": {"class_type": "CLIPTextEncode", "inputs": {"text": pos, "clip": ["1", 1]}},
        "3": {"class_type": "CLIPTextEncode", "inputs": {"text": neg, "clip": ["1", 1]}},
        "4": {"class_type": "EmptyLatentImage", "inputs": {"width": M["w"], "height": M["h"], "batch_size": 1}},
        "5": {"class_type": "KSampler", "inputs": {"model": ["1", 0], "positive": ["2", 0], "negative": ["3", 0],
                                                   "latent_image": ["4", 0], "seed": seed, "steps": M["steps"],
                                                   "cfg": M["cfg"], "sampler_name": M["sampler"],
                                                   "scheduler": M["scheduler"], "denoise": denoise}},
        "6": {"class_type": "VAEDecode", "inputs": {"samples": ["5", 0], "vae": ["1", 2]}},
        "7": {"class_type": "SaveImage", "inputs": {"images": ["6", 0], "filename_prefix": prefix}},
    }
    if init:
        g["8"] = {"class_type": "LoadImage", "inputs": {"image": init}}
        g["9"] = {"class_type": "VAEEncode", "inputs": {"pixels": ["8", 0], "vae": ["1", 2]}}
        g["5"]["inputs"]["latent_image"] = ["9", 0]
        del g["4"]
    return g


def run(name, prompt, seed, init=None, denoise=1.0):
    out = raw / f"{name}.png"
    if out.exists():
        return None
    (ROOT / "status.json").write_text(json.dumps({"time": time.time(), "stage": "generating-redo", "image": name}))
    t = time.time()
    pid = json.loads(call("prompt", {"prompt": graph(f"{prompt}, {board['style']}", board["negative"], seed,
                                                     f"cosmickf16/{name}", init, denoise)}))["prompt_id"]
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
    return {"file": out.name, "prompt_id": pid, "seed": seed, "init": init, "denoise": denoise,
            "seconds": round(time.time() - t, 1)}


receipt_path = ROOT / "receipt.json"
receipt = json.loads(receipt_path.read_text())
log = receipt.setdefault(f"redo_{SPEC.stem}", [])
for job in spec["jobs"]:
    prompt = job["prompt"].replace("{identity}", spec["identity"])
    if job.get("init"):
        init = upload(raw / job["init"])
        for d in job["denoise"]:
            for seed in job["init_seeds"]:
                r = run(f"{job['name']}_xl_d{int(d * 100)}_s{seed}", prompt, seed, init, d)
                if r:
                    log.append(r)
                    receipt_path.write_text(json.dumps(receipt, indent=2))
    else:
        for seed in spec["seeds"]:
            r = run(f"{job['name']}_xl_s{seed}", prompt, seed)
            if r:
                log.append(r)
                receipt_path.write_text(json.dumps(receipt, indent=2))
(ROOT / "status.json").write_text(json.dumps({"time": time.time(), "stage": "complete-redo",
                                              "images": len(list(raw.glob("*.png")))}))
print("done", len(log), "redo images")
