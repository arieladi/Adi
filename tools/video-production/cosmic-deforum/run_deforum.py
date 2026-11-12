"""Cosmic Rain 0:00-1:04 with the APPROVED Avastha Deforum method (Adi 2026-11-12: "go back to the deforum style that
actually worked ... use the images from cosmic rain some of them once in a while to drive the changes but mostly let
deforum just run freely").

usage: run_deforum.py <root> [stills|continuous|ending|remake]     (resumable; D:/AvasthaAI/venv python; the Deforum service on 7865 must be up)
Method, from the archived Avastha project (QuantumAscent-Archive-20261030, deforum-section-050-123 + ending-184s):
  - the actual Deforum extension, SD1.5 DreamShaper8, A2000, D:/AvasthaAI/ActualDeforum/settings-8s.json unchanged
    (848x480 15 fps, DPM++ 2M 28 steps, cfg 6, 2D camera zoom 1.005 / angle 0.06 / x 0.12 / y -0.05, noise 0.02,
    LAB colour coherence, the approved strength envelope 0.62/0.55/0.48/0.55/0.60 every 120 frames)
  - entry exactly as section 050-123: hybrid video compositing of the source with alpha 1.0 -> 0.05 over 45 frames
    and strength 0.98 -> 0.62, camera easing in from still
  - the Cosmic Rain stills ride the same hybrid layer: at a few moments the guide video shows a still and the
    composite alpha rises to STILL_ALPHA for about a second; the prompt turns to that still's subject a second before
  - finishing as the approved ending: the approved LUT (generated-lut-16.npy, sha 847229a3...), RealESRGAN_x4plus +
    RIFE 4.26 (EnhancerV2) to 1280x720 24 fps, after Adi's original 7 s opening; music unchanged
"""
import json, os, subprocess, sys, time, traceback, urllib.request
from pathlib import Path

R = Path(sys.argv[1])
# "stills" (default): the 0:50-2:03 way, stills ride the hybrid layer. "continuous": the 8-9 min ending way - one
# picture deforming all the way, nothing composited after the entry; the stills only lend their subjects as themes.
MODE = sys.argv[2] if len(sys.argv) > 2 else "stills"
BATCH = "CosmicRain_Deforum_104_test" + ("" if MODE == "stills" else "_" + MODE)
os.environ["CUDA_VISIBLE_DEVICES"] = "GPU-45dfd7ed-e572-42ff-2a7e-1c36684593f9"  # A2000, after Deforum is done
import numpy as np, imageio_ffmpeg
from PIL import Image, ImageOps

FF = imageio_ffmpeg.get_ffmpeg_exe()
API = "http://127.0.0.1:7865"
OPENING = Path("D:/AvasthaAI/CosmicRain-20261025/inputs/opening.mp4")
TRACK = Path("D:/AvasthaAI/CosmicRain-20261025/inputs/track.wav")
LUT = Path("D:/AvasthaAI/CosmicRain-20261025/approved-lut.npy")
LUT_SHA = "847229a33fca2012b0a69f453cab6e906d55a343e77287cd1cac3a588586c48a"
FULL = Path("D:/AvasthaAI/CosmicRainFull-20261027")
DRAFT1 = FULL / "Cosmic Rain - full track keyframe edit - with music.mp4"
END, START, FPS = 64.0, 7.0, 15
N = round((END - START) * FPS)
BAR, PHASE = 240 / 143, 0.0215
STILL_ALPHA, HALF = 0.40, 12
SUFFIX = ", visionary psychedelic painting, rich intricate detail, continuous organic transformation"
OPEN_PROMPT = ("A luminous black hole surrounded by silver cosmic rain and cyan arcs, falling crystal droplets spiraling "
               "into the event horizon, deep layered cosmic space")
# "ending" mode (Adi 2026-11-12: no Cosmic Rain photos, a simple test like the end of Quantum Ascent): themes in the
# ending's own style, one every ~14 s.
THEMES = [
    "A luminous black hole drinking silver cosmic rain, cyan light arcs spiraling into a glowing crystalline event horizon, deep layered cosmic space",
    "The silver rain becomes a vast luminous crystal ocean, bioluminescent waves curling into spiral shells, intricate golden reflections, deep layered alien seascape",
    "The ocean spirals grow into an underwater forest of translucent jellyfish bells and coral cathedrals, turquoise light, intricate organic golden filaments",
    "The coral cathedrals open into immense glowing rings of sacred geometry and falling silver rain, luminous violet galaxies, intricate fractal celestial architecture",
]
# (bar, Draft 1 slot whose approved still is shown): story order, on bar lines
STILLS = [(8, "K005"), (16, "K009"), (24, "K011"), (30, "K013"), (32, "K015a"), (36, "K018")]


def save(name, obj):
    t = R / (name + ".tmp"); t.write_text(json.dumps(obj, indent=2)); t.replace(R / name)


def status(stage, **kw):
    save("status.json", dict(stage=stage, time=time.time(), **kw))


def api(path, data=None, timeout=120):
    req = urllib.request.Request(API + path, data=None if data is None else json.dumps(data).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as f:
        return json.load(f)


def prepare():
    plan = json.loads((FULL / "plan.json").read_text(encoding="utf-8"))
    picks = json.loads((FULL / "picks.json").read_text(encoding="utf-8"))
    slot = {s["id"]: s for s in plan["slots"]}
    subject = {s["id"]: s["subject"] for s in plan["scenes"]}
    stills = []
    for bar, sid in STILLS:
        s = slot[sid]
        stills.append({"slot": sid, "frame": round((PHASE + bar * BAR - START) * FPS), "image": str(FULL / "raw" / picks[s["key"]]),
                       "prompt": subject[s["scene"]]})
    # Guide video for the hybrid layer: the opening's 7-8 s, then each still held around its moment.
    src = subprocess.run([FF, "-v", "error", "-ss", str(START), "-i", str(OPENING), "-vf", "fps=15,scale=848:480:flags=lanczos",
                          "-frames:v", "15", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    opening = np.frombuffer(src, np.uint8).reshape(-1, 480, 848, 3)
    assert len(opening) == 15
    Image.fromarray(opening[0]).save(R / "start.png")
    if MODE == "ending":
        # Exactly the Avastha ending (deforum-ending-184s/worker.py): approved settings unchanged, one init image,
        # the approved strength envelope repeated from frame 0, slowly evolving world themes. No photos, no hybrid.
        s = json.loads(Path("D:/AvasthaAI/ActualDeforum/settings-8s.json").read_text())
        s.update(max_frames=N, batch_name=BATCH, init_image=str(R / "start.png"))
        step = N // len(THEMES) + 1
        s["prompts"] = {str(i * step): t + SUFFIX for i, t in enumerate(THEMES)}
        keys = {}
        for base in range(0, N, 120):
            for off, v in ((0, .62), (35, .55), (60, .48), (90, .55), (119, .60)):
                if base + off < N:
                    keys[base + off] = v
        s["strength_schedule"] = ", ".join(f"{k}:({v})" for k, v in sorted(keys.items()))
        save("settings.json", s)
        return s
    if MODE == "remake":
        # As section 0:50-2:03 was made: existing footage of the same minutes rides underneath at alpha 0.05 the
        # whole run. For Cosmic Rain that is Draft 1 (Adi's favourite, the 43 h run) from 0:07, no stills.
        subprocess.run([FF, "-v", "error", "-y", "-ss", str(START), "-i", str(DRAFT1), "-t", f"{END - START:.3f}",
                        "-vf", "fps=15,scale=848:480:flags=lanczos", "-an", "-c:v", "libx264", "-crf", "12",
                        "-pix_fmt", "yuv420p", str(R / "guide.mp4")], check=True)
        stills = []
    imgs =[np.array(ImageOps.fit(Image.open(x["image"]).convert("RGB"), (848, 480), method=Image.Resampling.LANCZOS)) for x in stills]
    p = None if MODE == "remake" else subprocess.Popen(
        [FF, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "848x480", "-r", "15", "-i", "pipe:0",
         "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", str(R / "guide.mp4")], stdin=subprocess.PIPE)
    for i in range(N if p else 0):
        if i < 15:
            f = opening[i]
        else:
            shown = [k for k, x in enumerate(stills) if x["frame"] - HALF - 2 <= i]
            f = imgs[shown[-1]] if shown else opening[-1]
        p.stdin.write(np.ascontiguousarray(f).tobytes())
    if p:
        p.stdin.close(); assert p.wait() == 0

    s = json.loads(Path("D:/AvasthaAI/ActualDeforum/settings-8s.json").read_text())
    alpha = {0: 1.0, 15: 0.70, 30: 0.25, 45: 0.05}
    for x in stills if MODE == "stills" else []:
        k = x["frame"]
        alpha.update({k - HALF: 0.05, k - 4: STILL_ALPHA, k + 4: STILL_ALPHA, k + HALF: 0.05})
    alpha[N - 1] = 0.05
    if MODE == "continuous":
        alpha.update({45: 0.0, N - 1: 0.0})
    strength = {0: .98, 15: .88, 30: .74}
    for base in range(45, N, 120):  # the approved envelope, every 120 frames
        for off, v in ((0, .62), (35, .55), (60, .48), (90, .55), (119, .60)):
            if base + off < N:
                strength[base + off] = v
    prompts = {"0": OPEN_PROMPT + SUFFIX}
    if MODE == "remake":
        prompts = {str(i * (N // len(THEMES) + 1)): t + SUFFIX for i, t in enumerate(THEMES)}
    for x in stills:
        prompts[str(max(1, x["frame"] - 15))] = x["prompt"] + SUFFIX
    sched = lambda d: ", ".join(f"{k}:({v})" for k, v in sorted(d.items()))
    s.update(batch_name=BATCH, max_frames=N, init_image=str(R / "start.png"), use_init=True,
             video_init_path=str(R / "guide.mp4"), hybrid_generate_inputframes=True, hybrid_use_first_frame_as_init_image=True,
             hybrid_composite="Normal", hybrid_motion="None", hybrid_use_init_image=False, overwrite_extracted_frames=True,
             hybrid_comp_alpha_schedule=sched(alpha), strength_schedule=sched(strength),
             noise_schedule=f"0:(0), 45:(0.02), {N - 1}:(0.02)", zoom=f"0:(1.0), 45:(1.005), {N - 1}:(1.005)",
             angle=f"0:(0), 45:(0.06), {N - 1}:(0.06)", translation_x=f"0:(0), 45:(0.12), {N - 1}:(0.12)",
             translation_y=f"0:(0), 45:(-0.05), {N - 1}:(-0.05)", prompts=prompts)
    save("settings.json", s)
    save("stills.json", stills)
    return s


def frames_dir(job):
    return Path(job["outdir"]) if job.get("outdir") else Path("D:/AvasthaAI/ActualDeforum/webui/outputs/img2img-images") / BATCH


def generate():
    s = json.loads((R / "settings.json").read_text()) if (R / "settings.json").exists() else prepare()
    if (R / "receipt.json").exists():
        job_id = json.loads((R / "receipt.json").read_text())["job_ids"][0]
    else:
        if (R / "submission-marker.json").exists():
            raise RuntimeError("Submission outcome uncertain; inspect the Deforum API before resubmitting")
        api("/sdapi/v1/reload-checkpoint", {}, timeout=180)
        save("submission-marker.json", {"time": time.time(), "batch": s["batch_name"]})
        save("receipt.json", api("/deforum_api/batches", {"deforum_settings": s, "options_overrides": {"img2img_fix_steps": True}}, timeout=180))
        job_id = json.loads((R / "receipt.json").read_text())["job_ids"][0]
    while True:
        try:
            job = api(f"/deforum_api/jobs/{job_id}", timeout=30)
        except Exception as e:
            job = {"status": "NOT FOUND", "error": repr(e)}
        if job.get("status") == "NOT FOUND":
            # The service was restarted (Stop/Continue): resume the same run from its frames on disk.
            last = json.loads((R / "last-job.json").read_text()) if (R / "last-job.json").exists() else None
            if not last or not last.get("timestring"):
                raise RuntimeError("Deforum job lost before it reported a timestring; inspect before resubmitting")
            r = dict(s, resume_from_timestring=True, resume_timestring=last["timestring"])
            save("receipt.json", api("/deforum_api/batches", {"deforum_settings": r, "options_overrides": {"img2img_fix_steps": True}}, timeout=180))
            job_id = json.loads((R / "receipt.json").read_text())["job_ids"][0]
            continue
        if job.get("timestring"):
            save("last-job.json", {k: job.get(k) for k in ("id", "outdir", "timestring", "status", "phase", "phase_progress")})
        if job["status"] == "FAILED":
            raise RuntimeError("Deforum job failed: " + str(job.get("message")))
        if job["status"] == "SUCCEEDED":
            break
        status("deforum", phase=job.get("phase"), progress=job.get("phase_progress"))
        time.sleep(20)
    d, ts = frames_dir(job), job["timestring"]
    frames = sorted(d.glob(f"{ts}_?????????.png"))
    assert len(frames) >= N, (str(d), len(frames))
    save("frames.json", {"dir": str(d), "timestring": ts, "count": len(frames)})
    return frames[:N]


def finish(frames):
    import hashlib, torch
    import torch.nn.functional as TF
    out = R / "Cosmic Rain - Deforum 1-04 test - with music.mp4"
    if out.exists():
        return out
    with LUT.open("rb") as f:
        assert hashlib.file_digest(f, "sha256").hexdigest() == LUT_SHA, "not the approved LUT"
    lut = np.load(LUT).astype(np.float32)
    vol = torch.from_numpy(lut.reshape(16, 16, 16, 3).transpose(3, 0, 1, 2).copy()).unsqueeze(0).cuda()

    @torch.inference_mode()
    def grade(a):
        g = torch.from_numpy(a.copy()).cuda().float().div_(127.5).sub_(1).unsqueeze(0).unsqueeze(0)
        return TF.grid_sample(vol, g, mode="bilinear", padding_mode="border", align_corners=True)[0, :, 0].permute(1, 2, 0).mul(255).round().clamp(0, 255).byte().cpu().numpy()
    graded = R / "graded.mkv"
    if not graded.exists():
        status("grading")
        p = subprocess.Popen([FF, "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "848x480", "-r", "15", "-i", "pipe:0",
                              "-c:v", "ffv1", "-level", "3", str(graded)], stdin=subprocess.PIPE)
        for n, fp in enumerate(frames):
            a = np.asarray(Image.open(fp).convert("RGB")); g = grade(a)
            if n < 15:  # the grade fades in after the ungraded opening, as the approved finishers did
                g = np.rint(a.astype(float) * (1 - n / 14) + g.astype(float) * (n / 14)).clip(0, 255).astype(np.uint8)
            p.stdin.write(np.ascontiguousarray(g).tobytes())
        p.stdin.close(); assert p.wait() == 0
    enh = R / "enhanced.mp4"
    if not enh.exists():
        status("upscaling")
        cfg = json.loads(Path("D:/AvasthaAI/EnhancerV2/Avastha-Deforum-New-Renderer-4K60.receipt.json").read_text())["config"]
        cfg.update(input=str(graded), output=str(enh), width=1280, height=720, fps="24", quality=0, flow_scale=1.0,
                   upscale_device="cuda:0", rife_device="cuda:0", queue_depth=2, pinned_gb=.5)
        save("enhance-config.json", cfg)
        with (R / "enhance.log").open("wb") as log:
            subprocess.run(["D:/AvasthaAI/venv/Scripts/python.exe", "-u", "D:/AvasthaAI/EnhancerV2/renderer.py", "--config", str(R / "enhance-config.json")],
                           stdout=log, stderr=subprocess.STDOUT, check=True, env=os.environ.copy())
    status("assembling")
    enc = lambda *a: [*a, "-an", "-c:v", "libx264", "-crf", "15", "-pix_fmt", "yuv420p"]
    opening = R / "opening-7s.mp4"; body = R / "body.mp4"; silent = R / "silent.mp4"
    subprocess.run(enc(FF, "-v", "error", "-y", "-i", str(OPENING), "-t", str(START), "-vf",
                       "scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720,setsar=1,fps=24") + [str(opening)], check=True)
    subprocess.run(enc(FF, "-v", "error", "-y", "-i", str(enh), "-frames:v", str(N * 8 // 5)) + [str(body)], check=True)
    (R / "concat.txt").write_text(f"file '{opening.as_posix()}'\nfile '{body.as_posix()}'\n")
    subprocess.run([FF, "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", str(R / "concat.txt"), "-c", "copy", str(silent)], check=True)
    subprocess.run([FF, "-v", "error", "-y", "-i", str(silent), "-i", str(TRACK), "-map", "0:v", "-map", "1:a", "-c:v", "copy",
                    "-c:a", "aac", "-b:a", "320k", "-t", f"{END:.3f}", "-movflags", "+faststart", str(out)], check=True)
    subprocess.run([FF, "-v", "error", "-xerror", "-i", str(out), "-f", "null", "-"], check=True)
    return out


try:
    R.mkdir(parents=True, exist_ok=True)
    out = finish(generate())
    status("complete", output=str(out))
except Exception:
    status("failed", error=traceback.format_exc()); raise
