"""Wan 2.2 I2V A14B (Q8 GGUF, high-noise + low-noise) clips on the A2000 ComfyUI, quality first (Adi 2026-11-13:
"I need a better end result, it can take longer").

usage: wan14_job.py <root>        (reads <root>/jobs.json; resumable; D:/AvasthaAI/venv python)
jobs.json: [{"id": ..., "start": <png or "@prev" = last frame of the previous job>, "end": <png or null>, "prompt": ..., "length": 81, "seed": n,
             "width": 1280, "height": 720}]
With "end", WanFirstLastFrameToVideo pins the last frame (a loop when end == start); without it, WanImageToVideo.
Official Wan 2.2 two-stage sampling (shift 8, cfg 3.5, euler/simple, high-noise model for the first half of the steps,
low-noise for the rest) with 30 steps instead of 20. Layers that do not fit the 12 GB card are kept in RAM by ComfyUI.
Same receipt convention as animate_full.py (anim/<id>/receipt.json, generated.json), so Stop/Continue can park and
resume it.
"""
import json, sys, time
from pathlib import Path
import requests
from PIL import Image, ImageOps

R = Path(sys.argv[1])
API = "http://127.0.0.1:8192"
C = Path("D:/AvasthaAI/VideoEditFour-20261025")
STEPS, SPLIT, CFG, SHIFT = 30, 15, 3.5, 8
DONOR = "cuda:1,3.5gb;cpu,*"  # per half: 2 x 3.5 GB on the 8 GB RTX4000 (it also drives the display)
NEG = ("text, watermark, subtitles, collage, split screen, static image, frozen, blurry, muddy detail, deformed geometry, "
       "photorealistic skin pores, extra arms, extra hands, extra fingers, melting body, bad anatomy, flicker")


def save(p, v):
    t = p.with_suffix(".tmp"); t.write_text(json.dumps(v, indent=2)); t.replace(p)


def status(stage, **kw):
    save(R / "status.json", dict(stage=stage, time=time.time(), **kw))


def node(t, **kw):
    return {"class_type": t, "inputs": kw}


def put(src, name, w, h):
    ImageOps.fit(Image.open(src).convert("RGB"), (w, h), method=Image.Resampling.LANCZOS).save(C / "input" / name)
    return name


def start_of(j):
    # "@prev": continue from the last frame of the previous job (chained segments of a long loop).
    if j["start"] != "@prev":
        return j["start"]
    prev = jobs[[x["id"] for x in jobs].index(j["id"]) - 1]["id"]
    return json.loads((R / "anim" / prev / "generated.json").read_text())["frames"][-1]


def graph(j):
    w, h, L = j.get("width", 1280), j.get("height", 720), j["length"]
    tag = f"wan14/{R.name}/{j['id']}"
    g = {
        # The A2000 (cuda:0) computes; part of each half is parked on the RTX4000 (cuda:1), the rest in RAM
        # (ComfyUI-MultiGPU DisTorch2). The text encoder runs once per clip on the CPU so the RTX4000 holds model layers.
        "1": node("UnetLoaderGGUFDisTorch2MultiGPU", unet_name="wan2.2_i2v_high_noise_14B_Q8_0.gguf", compute_device="cuda:0",
                  virtual_vram_gb=0.0, donor_device="cpu", expert_mode_allocations=DONOR, eject_models=False),
        "2": node("UnetLoaderGGUFDisTorch2MultiGPU", unet_name="wan2.2_i2v_low_noise_14B_Q8_0.gguf", compute_device="cuda:0",
                  virtual_vram_gb=0.0, donor_device="cpu", expert_mode_allocations=DONOR, eject_models=False),
        "3": node("CLIPLoaderMultiGPU", clip_name="umt5_xxl_fp8_e4m3fn_scaled.safetensors", type="wan", device="cpu"),
        "4": node("VAELoader", vae_name="wan_2.1_vae.safetensors"),
        "5": node("CLIPTextEncode", clip=["3", 0], text=j["prompt"]),
        "6": node("CLIPTextEncode", clip=["3", 0], text=NEG),
        "7": node("LoadImage", image=put(start_of(j), f"wan14-{R.name}-{j['id']}-start.png", w, h)),
        "8": node("ModelSamplingSD3", model=["1", 0], shift=SHIFT),
        "9": node("ModelSamplingSD3", model=["2", 0], shift=SHIFT),
    }
    common = dict(positive=["5", 0], negative=["6", 0], vae=["4", 0], width=w, height=h, length=L, batch_size=1, start_image=["7", 0])
    if j.get("end"):
        g["12"] = node("LoadImage", image=put(j["end"], f"wan14-{R.name}-{j['id']}-end.png", w, h))
        g["10"] = node("WanFirstLastFrameToVideo", end_image=["12", 0], **common)
    else:
        g["10"] = node("WanImageToVideo", **common)
    g["20"] = node("KSamplerAdvanced", model=["8", 0], add_noise="enable", noise_seed=j["seed"], steps=STEPS, cfg=CFG,
                   sampler_name="euler", scheduler="simple", positive=["10", 0], negative=["10", 1], latent_image=["10", 2],
                   start_at_step=0, end_at_step=SPLIT, return_with_leftover_noise="enable")
    g["21"] = node("KSamplerAdvanced", model=["9", 0], add_noise="disable", noise_seed=j["seed"], steps=STEPS, cfg=CFG,
                   sampler_name="euler", scheduler="simple", positive=["10", 0], negative=["10", 1], latent_image=["20", 0],
                   start_at_step=SPLIT, end_at_step=STEPS, return_with_leftover_noise="disable")
    g["30"] = node("SaveLatent", samples=["21", 0], filename_prefix=tag)
    g["31"] = node("VAEDecodeTiled", samples=["21", 0], vae=["4", 0], tile_size=512, overlap=64, temporal_size=4096, temporal_overlap=4)
    g["32"] = node("SaveImage", images=["31", 0], filename_prefix=f"{tag}/f")
    return g, tag


jobs = json.loads((R / "jobs.json").read_text(encoding="utf-8"))
try:
    for n, j in enumerate(jobs, 1):
        d = R / "anim" / j["id"]; d.mkdir(parents=True, exist_ok=True)
        if (d / "generated.json").exists():
            continue
        g, tag = graph(j)
        if (d / "receipt.json").exists():
            pid = json.loads((d / "receipt.json").read_text())["prompt_id"]
        else:
            assert not (d / "submitting.json").exists(), f"Ambiguous earlier submission for {j['id']}; inspect queue/history"
            save(d / "workflow.json", g); save(d / "submitting.json", {"time": time.time()})
            r = requests.post(API + "/prompt", json={"prompt": g, "client_id": "wan14"}, timeout=90)
            save(d / "receipt.json", r.json()); r.raise_for_status()
            pid = r.json()["prompt_id"]
        status("rendering", job=j["id"], n=n, total=len(jobs), prompt_id=pid)
        t0 = time.time()
        while True:
            h = requests.get(f"{API}/history/{pid}", timeout=90).json()
            if pid in h:
                save(d / "history.json", h[pid])
                assert h[pid]["status"]["status_str"] == "success", h[pid]["status"]
                break
            time.sleep(20)
        frames = sorted((C / "output" / tag).glob("f_*.png"))
        assert len(frames) == j["length"], (j["id"], len(frames))
        save(d / "generated.json", {"frames": [str(p) for p in frames], "job": j, "seconds": round(time.time() - t0),
                                    "steps": STEPS, "split": SPLIT, "cfg": CFG, "shift": SHIFT})
    status("complete", jobs=len(jobs))
except Exception as e:
    status("failed", error=repr(e)); raise
