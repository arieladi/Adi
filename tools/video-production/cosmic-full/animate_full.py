"""Animate every new slot of the full-track plan with Wan2.2 TI2V 5B on a ComfyUI instance.

usage: animate_full.py <root> [comfy url, default http://127.0.0.1:8192] [worker name, default a2000]
                       [comfy data dir holding input/ and output/, default D:/AvasthaAI/VideoEditFour-20261025]
                       [slot file, default plan.json]
A slot with "replaces": <old slot id> is a re-render; when free space is low, the old take's PNG frames are
removed after the new take exists (its latent is kept, so it can be decoded again).
Settings are the proven pilot/test ones (shift 8, 20 steps, CFG 5, uni_pc/simple, latent saved before the
full-temporal tiled decode). Keyframe = the picked still of the slot's scene key (picks.json), or for a drop's
bar B the last generated frame of its bar A (chain_from). Reused test slots are skipped.

Resumable and safe to run as two workers on two GPUs: a slot is claimed by atomically creating
anim/<slot>/claim.<worker>; generated.json marks it done; a receipt without frames is waited on, never
resubmitted; submitting.json without a receipt stops for inspection.

Motion gate (slots with "gate": true): every take is measured with motion_check (subject motion and the
change left after removing global zoom/pan/rotation). A failed take is kept and the slot is rendered again
as take <slot>t2, t3 ... with a new seed, up to MAX_TAKES. anim/<slot>/accepted.json names the passing take
(or, if none passed, the best one with gate_failed). A chained bar B starts from the last frame of its
bar A's accepted take. Passing the gate is screening only; takes are still reviewed by eye.
"""
import json, os, shutil, sys, time
from pathlib import Path
import requests
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
from motion_check import content, motion

ROOT = Path(sys.argv[1])
API = sys.argv[2] if len(sys.argv) > 2 else "http://127.0.0.1:8192"
WORKER = sys.argv[3] if len(sys.argv) > 3 else "a2000"
C = Path(sys.argv[4] if len(sys.argv) > 4 else "D:/AvasthaAI/VideoEditFour-20261025")
plan = json.loads((ROOT / (sys.argv[5] if len(sys.argv) > 5 else "plan.json")).read_text(encoding="utf-8"))
picks = json.loads((ROOT / "picks.json").read_text(encoding="utf-8"))
NEG = ("text, watermark, subtitles, collage, split screen, inset pictures, random duplicates, strobe, flicker, "
       "static image, frozen, slow motion, blurry, muddy detail, deformed geometry, abrupt cuts, photorealistic, "
       "open mouth, screaming, eyes rolled back, extra arms, extra hands, floating hands, melting body")


def save(p, v):
    q = p.with_suffix(".tmp")
    q.write_text(json.dumps(v, indent=2), encoding="utf-8")
    q.replace(p)


def status(stage, **kw):
    save(ROOT / f"anim-status-{WORKER}.json", dict(stage=stage, time=time.time(), worker=WORKER, **kw))


def node(t, **kw):
    return dict(class_type=t, inputs=kw)


def keyframe_1280x736(src, dst):
    # Scale to height 736 and centre-crop the width: same aspect, never stretched.
    im = Image.open(src).convert("RGB")
    w = round(im.width * 736 / im.height)
    im = im.resize((w, 736), Image.LANCZOS)
    x = (w - 1280) // 2
    im.crop((x, 0, x + 1280, 736)).save(dst)


def done(sid):
    return (ROOT / "anim" / sid / "generated.json").exists()


def claim(d):
    # First worker to create its claim file owns the slot; a claim by another worker means skip.
    others = [p for p in d.glob("claim.*") if p.name != f"claim.{WORKER}"]
    if others:
        return False
    try:
        with (d / f"claim.{WORKER}").open("x") as f:
            f.write(str(os.getpid()))
    except FileExistsError:
        pass
    return not [p for p in d.glob("claim.*") if p.name != f"claim.{WORKER}"]


NEG = plan.get("negative", NEG)
BYID = {s["id"]: s for s in plan["slots"]}
MAX_TAKES = plan.get("max_takes", 4)
# Calibrated 2026-11-01 against a two-reviewer eye review of 259 raw clips (eye-review.json): with motion_check's
# mean gate this keeps 90% of clips judged "moves" and rejects 69% of still/weak ones on double-speed bars (100% / 87%
# on calm bars). Screening only; accepted takes are still reviewed by eye.
MIN_CONTENT = {1: 4.0, 2: 8.0}


def finished(sid):
    # A gated slot is finished when a take is accepted; any other slot when its single take exists.
    return (ROOT / "anim" / sid / "accepted.json").exists() if BYID.get(sid, {}).get("gate") else done(sid)


def accepted(sid):
    # The take that stands for the slot: the gate's accepted take, else the slot itself.
    a = ROOT / "anim" / sid / "accepted.json"
    return json.loads(a.read_text())["take"] if a.exists() else sid


def check(frames, step):
    m = motion(frames, step)
    c = content(frames)
    return {**m, "content": c, "passed": m["moves"] and c >= MIN_CONTENT.get(step, MIN_CONTENT[2])}


def free_replaced():
    # Remove PNG frames of old takes that already have a finished replacement (latents stay).
    for s in plan["slots"]:
        old = s.get("replaces")
        if old and finished(s["id"]) and (ROOT / "anim" / old / "generated.json").exists():
            frames = json.loads((ROOT / "anim" / old / "generated.json").read_text())["frames"]
            for f in frames:
                Path(f).unlink(missing_ok=True)
            (ROOT / "anim" / old / "generated.json").rename(ROOT / "anim" / old / "generated.replaced.json")
        if shutil.disk_usage(ROOT).free >= 12 * 1024 ** 3:
            return


def render(s, take, src, seed, prompt, n, total):
    """Render one take of slot s (or reuse it if it already exists) and return its generated.json."""
    d = ROOT / "anim" / take
    d.mkdir(parents=True, exist_ok=True)
    if (d / "generated.json").exists():
        return json.loads((d / "generated.json").read_text())
    if shutil.disk_usage(ROOT).free < 8 * 1024 ** 3:
        free_replaced()
    if shutil.disk_usage(ROOT).free < 8 * 1024 ** 3:
        raise RuntimeError("Less than 8 GiB free on D; preserve outputs and inspect")
    tag = f"cosmicfull-anim/{take}"
    inp = C / "input" / f"cosmicfull-{take}.png"
    keyframe_1280x736(src, inp)
    g = {
        "1": node("UNETLoader", unet_name="wan2.2_ti2v_5B_fp16.safetensors", weight_dtype="default"),
        "2": node("CLIPLoader", clip_name="umt5_xxl_fp8_e4m3fn_scaled.safetensors", type="wan", device="default"),
        "3": node("VAELoader", vae_name="wan2.2_vae.safetensors"),
        "4": node("CLIPTextEncode", clip=["2", 0], text=f"{prompt} {s.get('style', plan['style'])}"),
        "5": node("CLIPTextEncode", clip=["2", 0], text=NEG),
        "6": node("LoadImage", image=inp.name),
        "7": node("Wan22ImageToVideoLatent", vae=["3", 0], width=1280, height=736, length=s["length"],
                  batch_size=1, start_image=["6", 0]),
        "8": node("ModelSamplingSD3", model=["1", 0], shift=8),
        "9": node("KSampler", model=["8", 0], positive=["4", 0], negative=["5", 0], latent_image=["7", 0],
                  seed=seed, steps=20, cfg=5, sampler_name="uni_pc", scheduler="simple", denoise=1),
        "90": node("SaveLatent", samples=["9", 0], filename_prefix=tag),
        "10": node("VAEDecodeTiled", samples=["90", 0], vae=["3", 0], tile_size=512, overlap=64,
                   temporal_size=4096, temporal_overlap=4),
        "11": node("SaveImage", images=["10", 0], filename_prefix=f"{tag}/f"),
    }
    receipt = d / "receipt.json"
    if receipt.exists():
        pid = json.loads(receipt.read_text())["prompt_id"]
    else:
        assert not (d / "submitting.json").exists(), f"Ambiguous earlier submission for {take}; inspect queue/history"
        save(d / "workflow.json", g)
        save(d / "submitting.json", {"time": time.time(), "worker": WORKER, "api": API})
        r = requests.post(API + "/prompt", json={"prompt": g, "client_id": f"cosmicfull-{WORKER}"}, timeout=90)
        save(receipt, r.json())
        r.raise_for_status()
        pid = r.json()["prompt_id"]
    status("rendering", slot=s["id"], take=take, n=n, total=total, prompt_id=pid)
    t0 = time.time()
    while True:
        h = requests.get(API + "/history/" + pid, timeout=90).json()
        if pid in h:
            save(d / "history.json", h[pid])
            assert h[pid]["status"]["status_str"] == "success", h[pid]["status"]
            break
        time.sleep(15)
    frames = sorted((C / "output" / "cosmicfull-anim" / take).glob("f_*.png"))
    assert len(frames) == s["length"], (take, len(frames))
    gen = {"frames": [str(p) for p in frames], "native": [1280, 736], "keyframe": str(src), "seed": seed,
           "prompt": prompt, "latent_saved": True, "worker": WORKER, "seconds": round(time.time() - t0)}
    if s.get("gate"):
        gen["gate"] = check(frames, s["step"])
    save(d / "generated.json", gen)
    return gen


def main():
    slots = [s for s in plan["slots"] if not s.get("reused")]
    for i, s in enumerate(slots):
        sid = s["id"]
        d = ROOT / "anim" / sid
        d.mkdir(parents=True, exist_ok=True)
        if finished(sid) or not claim(d):
            continue
        if s.get("chain_from"):
            while not finished(s["chain_from"]):
                status("waiting-chain", slot=sid, on=s["chain_from"])
                time.sleep(20)
            src = Path(json.loads((ROOT / "anim" / accepted(s["chain_from"]) / "generated.json").read_text())["frames"][-1])
        else:
            src = ROOT / "raw" / picks[s["key"]]
        seed0 = s.get("seed", 143300000 + i)
        prompts = s.get("take_prompts", [])
        takes = []
        for t in range(1, (MAX_TAKES if s.get("gate") else 1) + 1):
            take = sid if t == 1 else f"{sid}t{t}"
            prompt = prompts[min(t, len(prompts)) - 1] if prompts else s["prompt"]
            gen = render(s, take, src, seed0 + 7919 * (t - 1), prompt, i + 1, len(slots))
            if not s.get("gate"):
                break
            takes.append((take, gen["gate"]))
            if gen["gate"]["passed"]:
                break
        if s.get("gate"):
            best = next((x for x in takes if x[1]["passed"]), None) or max(takes, key=lambda x: x[1]["content"])
            save(d / "accepted.json", {"take": best[0], "gate": best[1], "gate_failed": not best[1]["passed"],
                                       "takes": [x[0] for x in takes], "time": time.time()})
    status("complete", slots=len(slots))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        status("failed", error=repr(e))
        raise
