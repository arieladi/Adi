"""Animate each approved keyframe of the Cosmic Rain 16s test with Wan2.2 TI2V 5B on the A2000 ComfyUI.

usage: animate.py <root> [slot file, default anim.json]   (root holds the slot file and raw/ keyframes)
Same proven settings as the 120s pilot (shift 8, 20 steps, CFG 5, uni_pc/simple, latent saved before
the full-temporal tiled decode). Differences: every slot starts from its own keyframe (no chaining).
Resumable: a slot with generated.json is skipped; a slot with a receipt but no frames waits on that
prompt instead of resubmitting; submitting.json without a receipt stops for inspection.
"""
import json, shutil, sys, time
from pathlib import Path
import requests
from PIL import Image

API = "http://127.0.0.1:8192"
C = Path("D:/AvasthaAI/VideoEditFour-20261025")
ROOT = Path(sys.argv[1])
anim = json.loads((ROOT / (sys.argv[2] if len(sys.argv) > 2 else "anim.json")).read_text(encoding="utf-8"))
NEG = ("text, watermark, subtitles, collage, split screen, inset pictures, random duplicates, strobe, flicker, "
       "static image, frozen, slow motion, blurry, muddy detail, deformed geometry, abrupt cuts, photorealistic")


def save(p, v):
    q = p.with_suffix(".tmp")
    q.write_text(json.dumps(v, indent=2), encoding="utf-8")
    q.replace(p)


def status(stage, **kw):
    save(ROOT / "anim-status.json", dict(stage=stage, time=time.time(), **kw))


def node(t, **kw):
    return dict(class_type=t, inputs=kw)


def keyframe_1280x736(src, dst):
    # Scale to height 736 and centre-crop the width: same aspect, never stretched.
    im = Image.open(src).convert("RGB")
    w = round(im.width * 736 / im.height)
    im = im.resize((w, 736), Image.LANCZOS)
    x = (w - 1280) // 2
    im.crop((x, 0, x + 1280, 736)).save(dst)


def main():
    lock = ROOT / "anim.lock"
    with lock.open("x") as f:
        f.write("animate.py")
    try:
        q = requests.get(API + "/queue", timeout=30).json()
        assert not q["queue_running"] and not q["queue_pending"], "Comfy queue busy; wait for it"
        for i, s in enumerate(anim["slots"]):
            d = ROOT / "anim" / s["id"]
            d.mkdir(parents=True, exist_ok=True)
            tag = f"cosmickf16-anim/{s['id']}"
            if (d / "generated.json").exists():
                continue
            if shutil.disk_usage(ROOT).free < 8 * 1024 ** 3:
                raise RuntimeError("Less than 8 GiB free on D")
            inp = C / "input" / f"cosmickf16-{s['id']}.png"
            keyframe_1280x736(ROOT / s["keyframe"], inp)
            g = {
                "1": node("UNETLoader", unet_name="wan2.2_ti2v_5B_fp16.safetensors", weight_dtype="default"),
                "2": node("CLIPLoader", clip_name="umt5_xxl_fp8_e4m3fn_scaled.safetensors", type="wan", device="default"),
                "3": node("VAELoader", vae_name="wan2.2_vae.safetensors"),
                "4": node("CLIPTextEncode", clip=["2", 0], text=f"{s['prompt']} {anim['style']}"),
                "5": node("CLIPTextEncode", clip=["2", 0], text=NEG + s.get("negative_extra", "")),
                "6": node("LoadImage", image=inp.name),
                "7": node("Wan22ImageToVideoLatent", vae=["3", 0], width=1280, height=736, length=s["length"],
                          batch_size=1, start_image=["6", 0]),
                "8": node("ModelSamplingSD3", model=["1", 0], shift=8),
                "9": node("KSampler", model=["8", 0], positive=["4", 0], negative=["5", 0], latent_image=["7", 0],
                          seed=s.get("seed", 143270000 + i), steps=20, cfg=5, sampler_name="uni_pc", scheduler="simple", denoise=1),
                "90": node("SaveLatent", samples=["9", 0], filename_prefix=tag),
                "10": node("VAEDecodeTiled", samples=["90", 0], vae=["3", 0], tile_size=512, overlap=64,
                           temporal_size=4096, temporal_overlap=4),
                "11": node("SaveImage", images=["10", 0], filename_prefix=f"{tag}/f"),
            }
            receipt = d / "receipt.json"
            if receipt.exists():
                pid = json.loads(receipt.read_text())["prompt_id"]
            else:
                assert not (d / "submitting.json").exists(), "Ambiguous earlier submission; inspect queue/history"
                save(d / "workflow.json", g)
                save(d / "submitting.json", {"time": time.time()})
                r = requests.post(API + "/prompt", json={"prompt": g, "client_id": "cosmickf16-anim"}, timeout=90)
                save(receipt, r.json())
                r.raise_for_status()
                pid = r.json()["prompt_id"]
            status("rendering", slot=s["id"], n=i + 1, total=len(anim["slots"]), prompt_id=pid)
            while True:
                h = requests.get(API + "/history/" + pid, timeout=90).json()
                if pid in h:
                    save(d / "history.json", h[pid])
                    assert h[pid]["status"]["status_str"] == "success", h[pid]["status"]
                    break
                time.sleep(15)
            frames = sorted((C / "output" / "cosmickf16-anim" / s["id"]).glob("f_*.png"))
            assert len(frames) == s["length"], (s["id"], len(frames))
            save(d / "generated.json", {"frames": [str(p) for p in frames], "native": [1280, 736],
                                         "keyframe": s["keyframe"], "latent_saved": True})
        status("complete", slots=len(anim["slots"]))
    except Exception as e:
        status("failed", error=repr(e))
        raise
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
