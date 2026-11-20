"""Any song, approved Deforum style C ("ending"): one picture deforming continuously through text themes.

usage: deforum_video.py <root>        (reads <root>/video.json; resumable; F:/AvasthaAI/venv python; Deforum on 7865)
video.json:
  {"title": "...", "start_image": "<rich, dark, colourful still>", "seconds": 60, "bpm": 142,
   "themes": [[<bar>, "<world or scene>"], ...],          bar 0 = first frame; a theme takes over on its bar line
   "track": "<optional wav>", "track_start": 0.0, "output": "<name>.mp4",
   "hold": 0.08, "camera": "still"}     optional, for people: the start picture stays underneath at alpha "hold" (the
                                          approved 0:50-2:03 hybrid way) and the camera does not travel, so the scene
                                          keeps its people while Deforum transforms them
  Optional, the music-driven cartoon test (Adi 2026-11-18, "anything cool and psychedelic that is light to run"):
   "model": "toonyou_beta6.safetensors"   checkpoint for this run (DreamShaper_8_pruned is put back afterwards)
   "suffix": "..."                        replaces the painterly prompt suffix
   "start_prompt": "..."                  no start_image: the first frame is drawn by txt2img with this prompt
   "hits": "<track-hits.json from cosmic-full/analyse_hits.py>"   frame 0 = track_start on the track's own clock;
        kick on a quarter beat -> zoom pulse, mid (melody/snare) off the beat -> angle flick, accents and the bar
        lines where a theme changes -> a morph burst (strength dip) so the change lands on the music
Same as run_deforum.py mode "ending" (Adi approved test C, 2026-11-13), without the Cosmic Rain opening: the approved
settings-8s.json unchanged, the approved strength envelope from frame 0, the approved LUT, RealESRGAN + RIFE to
1280x720 24 fps. Without a track the video is silent and the music is added later.
"""
import hashlib, json, os, subprocess, sys, time, traceback, urllib.request
from pathlib import Path

R = Path(sys.argv[1])
os.environ["CUDA_VISIBLE_DEVICES"] = "GPU-45dfd7ed-e572-42ff-2a7e-1c36684593f9"  # A2000, after Deforum is done
import numpy as np, imageio_ffmpeg
from PIL import Image, ImageOps

FF = imageio_ffmpeg.get_ffmpeg_exe()
API = "http://127.0.0.1:7865"
LUT = Path("F:/AvasthaAI/CosmicRain-20261025/approved-lut.npy")
LUT_SHA = "847229a33fca2012b0a69f453cab6e906d55a343e77287cd1cac3a588586c48a"
V = json.loads((R / "video.json").read_text(encoding="utf-8"))
SUFFIX = V.get("suffix", ", visionary psychedelic painting, rich intricate detail, continuous organic transformation")
DEFAULT_MODEL = "DreamShaper_8_pruned.safetensors"
FPS = 15
N = round(V["seconds"] * FPS)
BATCH = "Deforum_" + "".join(c if c.isalnum() else "_" for c in V["title"])[:60]


def save(name, obj):
    t = R / (name + ".tmp"); t.write_text(json.dumps(obj, indent=2))
    for i in range(30):  # a reader (status check over SSH) can hold the file for a moment on Windows
        try:
            return t.replace(R / name)
        except PermissionError:
            time.sleep(1)
    t.replace(R / name)


def status(stage, **kw):
    save("status.json", dict(stage=stage, time=time.time(), **kw))


def api(path, data=None, timeout=120):
    req = urllib.request.Request(API + path, data=None if data is None else json.dumps(data).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as f:
        return json.load(f)


def music_schedules(s):
    """Per-frame zoom / angle / strength from the hit map, on top of the approved camera and strength envelope."""
    H = json.loads(Path(V["hits"]).read_text())
    six, t0 = H["sixteenth_seconds"], V.get("track_start", 0.0)
    frame = lambda t: round((t - t0) * FPS)
    zoom, angle = np.full(N, 1.005), np.full(N, 0.06)
    env = {}
    for base in range(0, N, 120):
        for off, v in ((0, .62), (35, .55), (60, .48), (90, .55), (119, .60)):
            if base + off < N:
                env[base + off] = v
    ks = sorted(env)
    strength = np.interp(np.arange(N), ks, [env[k] for k in ks])
    flick = 1
    for b in H["bars"]:
        for i in range(16):
            f = frame(b["t"] + i * six)
            if not 0 <= f < N:
                continue
            if b["kick_hits"][i] and i % 4 == 0:  # on the beat: the picture breathes in
                for d, z in ((0, 1.03), (1, 1.018), (2, 1.01)):
                    if f + d < N:
                        zoom[f + d] = max(zoom[f + d], z)
            if b["mid_hits"][i] and i % 4 != 0:  # melody / snare off the beat: a small turn, alternating
                angle[f] = 0.9 * flick; flick = -flick
    bursts = [frame(t) for t in H["accent_seconds"]]
    bar = 240 / V["bpm"]
    bursts += [round(bb * bar * FPS) for bb, _ in V["themes"] if bb > 0]
    for f in bursts:
        for d, v in ((0, .36), (1, .42), (2, .50)):
            if 0 <= f + d < N:
                strength[f + d] = min(strength[f + d], v)
                zoom[f + d] = max(zoom[f + d], 1.045 if d == 0 else zoom[f + d])
    sched = lambda a: ", ".join(f"{i}:({v:.4f})" for i, v in enumerate(a) if i == 0 or v != a[i - 1] or (i + 1 < N and a[i + 1] != v))
    s.update(zoom=sched(zoom), angle=sched(angle), strength_schedule=sched(strength))
    save("music.json", {"zoom_pulses": int((zoom > 1.02).sum()), "flicks": int((angle != 0.06).sum()), "bursts": sorted(set(bursts))})


def prepare():
    if V.get("start_image"):
        ImageOps.fit(Image.open(V["start_image"]).convert("RGB"), (848, 480), method=Image.Resampling.LANCZOS).save(R / "start.png")
    elif not (R / "start.png").exists():
        import base64
        r = api("/sdapi/v1/txt2img", {"prompt": V["start_prompt"] + SUFFIX, "negative_prompt": "text, watermark, logo, blurry, muddy, dull, low quality",
                                      "width": 848, "height": 480, "steps": 28, "cfg_scale": 6, "sampler_name": "DPM++ 2M",
                                      "seed": 271828}, timeout=600)
        (R / "start.png").write_bytes(base64.b64decode(r["images"][0]))
    s = json.loads(Path("F:/AvasthaAI/ActualDeforum/settings-8s.json").read_text())
    s.update(max_frames=N, batch_name=BATCH, init_image=str(R / "start.png"))
    bar = 240 / V["bpm"]
    s["prompts"] = {str(min(N - 1, round(b * bar * FPS))): t + SUFFIX for b, t in V["themes"]}
    keys = {}
    for base in range(0, N, 120):  # the approved strength envelope
        for off, v in ((0, .62), (35, .55), (60, .48), (90, .55), (119, .60)):
            if base + off < N:
                keys[base + off] = v
    s["strength_schedule"] = ", ".join(f"{k}:({v})" for k, v in sorted(keys.items()))
    if V.get("hits"):
        music_schedules(s)
    if V.get("hold"):
        guide = R / "guide.mp4"
        subprocess.run([FF, "-v", "error", "-y", "-loop", "1", "-i", str(R / "start.png"), "-frames:v", str(N), "-r", "15",
                        "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", str(guide)], check=True)
        h = V["hold"]
        s.update(video_init_path=str(guide), hybrid_generate_inputframes=True, hybrid_use_first_frame_as_init_image=True,
                 hybrid_composite="Normal", hybrid_motion="None", hybrid_use_init_image=False, overwrite_extracted_frames=True,
                 hybrid_comp_alpha_schedule=f"0:(1.0), 15:(0.70), 30:(0.25), 45:({h}), {N - 1}:({h})")
    if V.get("camera") == "still":
        s.update(zoom="0:(1.0)", angle="0:(0)", translation_x="0:(0)", translation_y="0:(0)")
    save("settings.json", s)
    return s


def use_model(name):
    if api("/sdapi/v1/options").get("sd_model_checkpoint", "").split(" ")[0] != name:
        api("/sdapi/v1/options", {"sd_model_checkpoint": name}, timeout=600)


def generate():
    # Every run sets its own checkpoint before submitting, and the default is put back only after the job has
    # finished: switching it while Deforum is still drawing changes the model mid-video (2026-11-18 toon test).
    use_model(V.get("model", DEFAULT_MODEL))
    frames = _generate()
    use_model(DEFAULT_MODEL)
    return frames


def _generate():
    s = json.loads((R / "settings.json").read_text()) if (R / "settings.json").exists() else prepare()
    if (R / "receipt.json").exists():
        job_id = json.loads((R / "receipt.json").read_text())["job_ids"][0]
    else:
        if (R / "submission-marker.json").exists():
            raise RuntimeError("Submission outcome uncertain; inspect the Deforum API before resubmitting")
        api("/sdapi/v1/reload-checkpoint", {}, timeout=180)
        save("submission-marker.json", {"time": time.time(), "batch": BATCH})
        save("receipt.json", api("/deforum_api/batches", {"deforum_settings": s, "options_overrides": {"img2img_fix_steps": True}}, timeout=180))
        job_id = json.loads((R / "receipt.json").read_text())["job_ids"][0]
    while True:
        try:
            job = api(f"/deforum_api/jobs/{job_id}", timeout=30)
        except Exception as e:
            job = {"status": "NOT FOUND", "error": repr(e)}
        if job.get("status") in ("NOT FOUND", "CANCELLED"):  # lost or cancelled: continue the same frames
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
    frames = sorted(Path(job["outdir"]).glob(f"{job['timestring']}_?????????.png"))
    assert len(frames) >= N, (job["outdir"], len(frames))
    return frames[:N]


def finish(frames):
    import torch
    import torch.nn.functional as TF
    out = R / V["output"]
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
        for fp in frames:
            p.stdin.write(np.ascontiguousarray(grade(np.asarray(Image.open(fp).convert("RGB")))).tobytes())
        p.stdin.close(); assert p.wait() == 0
    enh = R / "enhanced.mp4"
    if not enh.exists():
        status("upscaling")
        cfg = json.loads(Path("F:/AvasthaAI/EnhancerV2/Avastha-Deforum-New-Renderer-4K60.receipt.json").read_text())["config"]
        cfg.update(input=str(graded), output=str(enh), width=1280, height=720, fps="24", quality=0, flow_scale=1.0,
                   upscale_device="cuda:0", rife_device="cuda:0", queue_depth=2, pinned_gb=.5)
        save("enhance-config.json", cfg)
        with (R / "enhance.log").open("wb") as log:
            subprocess.run(["F:/AvasthaAI/venv/Scripts/python.exe", "-u", "F:/AvasthaAI/EnhancerV2/renderer.py", "--config", str(R / "enhance-config.json")],
                           stdout=log, stderr=subprocess.STDOUT, check=True, env=os.environ.copy())
    status("assembling")
    n24 = N * 8 // 5
    cmd = [FF, "-v", "error", "-y", "-i", str(enh)]
    if V.get("track"):
        cmd += ["-ss", f"{V.get('track_start', 0):.6f}", "-i", V["track"], "-map", "0:v", "-map", "1:a", "-c:a", "aac", "-b:a", "320k"]
    else:
        cmd += ["-an"]
    cmd += ["-frames:v", str(n24), "-t", f"{n24 / 24:.6f}", "-c:v", "libx264", "-crf", "15", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out)]
    subprocess.run(cmd, check=True)
    subprocess.run([FF, "-v", "error", "-xerror", "-i", str(out), "-f", "null", "-"], check=True)
    return out


try:
    out = finish(generate())
    status("complete", output=str(out))
except Exception:
    status("failed", error=traceback.format_exc()); raise
