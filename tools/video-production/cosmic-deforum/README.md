# Deforum music videos: setup, install and use

These are the two Deforum styles Adi approved for **Cosmic Rain on 2026-11-13**:

| Style | Mode | What it is | Approved |
|---|---|---|---|
| **Test C — "Ending"** | `ending` | One picture deforming continuously, like the end of Avastha – Quantum Ascent. Only text prompts, nothing mixed in. | "very good" |
| **Test D — "Remake"** | `remake` | The Avastha 0:50–2:03 high-quality look on black. An existing video of the same minutes runs underneath at alpha 0.05. | "also good, usable" |

**Where the method comes from:** the approved Avastha Deforum work. See `../AVASTHA-DEFORUM-RECIPE.md` and the archive `\\orc-vst\D\Avastha-AI\Outputs\QuantumAscent-Archive-20261030`.

**Not approved:** `stills` mode (Cosmic Rain photos on the hybrid layer, "a mess") and `continuous` mode with still subjects as themes. They are kept only so the history makes sense; **don't use them**.

## 1. What runs where

All of this runs on ADI-PC, the studio PC. Everything AI-related is on `F:\AvasthaAI`, and `D:\AvasthaAI` is a junction to it.

| Part | Where | Notes |
|---|---|---|
| Deforum service | `D:\AvasthaAI\ActualDeforum` | A1111 webui `82a973c` with the `deforum/sd-webui-deforum` extension `5d63a33`. Python 3.10 venv, torch 2.1.2+cu121. API only, on port **7865**, on the **RTX A2000**. |
| Model | `webui\models\Stable-diffusion\DreamShaper_8_pruned.safetensors` | 2,132,625,894 bytes, sha256 `879db523…abd7fd`. From Hugging Face `Lykon/DreamShaper`. |
| Approved base settings | `D:\AvasthaAI\ActualDeforum\settings-8s.json` | A copy is in `approved/settings-8s.json`. Never change it; change copies. |
| Colour grade | `approved/approved-lut-16.npy` | sha256 `847229a3…c48a`. This is Adi's approved Avastha LUT. On ADI-PC it is `D:\AvasthaAI\CosmicRain-20261025\approved-lut.npy`. |
| Upscale + smooth | `D:\AvasthaAI\EnhancerV2\renderer.py` and `models.py` | Copies are in `approved/enhancer_*.py`. RealESRGAN_x4plus is at `D:\AvasthaAI\Temp\wan30-models\RealESRGAN_x4plus.pth`. RIFE 4.26 is at `D:\AvasthaAI\Tools\RIFE426`. They run in the `D:\AvasthaAI\venv` Python. |
| Runner | `run_deforum.py` (this folder) | Submits the job and waits. Then it grades, upscales, adds the opening and muxes the music. |
| Stop / Continue | `../adi-pc/control/` | The desktop buttons. Jobs launched as below are paused and resumed safely. |

## 2. Install (new machine, or rebuild)

You need Windows, an NVIDIA GPU with 12 GB or more, about 30 GB free, and `uv.exe` somewhere under `D:\AvasthaAI`.

1. **Base install.** Run `approved/setup_actual_deforum.py` with any Python 3. It installs, under `D:\AvasthaAI\ActualDeforum`:
   - portable MinGit
   - Python 3.10 via uv
   - a clone of the A1111 webui and the Deforum extension into `webui\extensions\deforum`
   - the webui venv

   To match the approved versions exactly, check out webui `82a973c04367123ae98bd9abdf80d9eda9b910e2` and deforum `5d63a339dbec8d476657a1f672a4eeb6dc79ed37`.

2. **Model.** Put `DreamShaper_8_pruned.safetensors` (Lykon/DreamShaper on Hugging Face) in `webui\models\Stable-diffusion\`. Check its sha256 against the one above.

3. **Approved settings.** Copy `approved/settings-8s.json` to `D:\AvasthaAI\ActualDeforum\settings-8s.json`.

4. **Start the service.** Run `approved/launch_actual.py` with the `D:\AvasthaAI\venv` Python.
   - It pins the A2000 by UUID. Change `CUDA_VISIBLE_DEVICES` on another PC.
   - It installs CLIP and numpy 1.26.2 into the webui venv.
   - It starts the webui with `--api --deforum-api --nowebui --port 7865 --opt-sdp-attention --no-half-vae --ckpt DreamShaper_8_pruned`.

   On ADI-PC the AI controller starts it for you: the "Continue video AI" button, or after a restart.

   Check it's up: `http://127.0.0.1:7865/deforum_api/jobs` answers `{}`.

5. **Finishing tools.**
   - Put `approved/enhancer_renderer.py` and `approved/enhancer_models.py` in `D:\AvasthaAI\EnhancerV2\` (as `renderer.py` and `models.py`).
   - Add RealESRGAN_x4plus.pth and RIFE 4.26.
   - Install these into `D:\AvasthaAI\venv`: torch, spandrel, av, opencv-python and imageio-ffmpeg.
   - The runner reads the approved enhancer config from `D:\AvasthaAI\EnhancerV2\Avastha-Deforum-New-Renderer-4K60.receipt.json`. A copy is in `approved/enhancer-receipt-config.json`. It overrides the size to 1280×720 at 24 fps.

## 3. Make a video

The runner does four things:
- **Opening:** Adi's original opening, its first 7 s untouched.
- **Deforum:** frames from 7 s to `END`, made with the approved settings.
- **Finishing:** the approved LUT, then RealESRGAN + RIFE to 1280×720 24 fps.
- **Music:** the track, never retimed.

```powershell
# on ADI-PC, only while F:\AvasthaAI\control\state.json says "running"
$R = 'D:\AvasthaAI\<NewFolder>'          # one new folder per run; never reuse one
mkdir $R; copy run_deforum.py $R\
$c = "cmd /c D:\AvasthaAI\venv\Scripts\python.exe -u $R\run_deforum.py $R ending >> $R\run.log 2>&1"
Invoke-CimMethod Win32_Process -MethodName Create -Arguments @{ CommandLine = $c; CurrentDirectory = $R }
```

- Use `ending` for style C and `remake` for style D.
- Launch it this way, as a detached process, so the Stop/Continue controller can see the job, save it and resume it.
- Progress is in `$R\status.json`, with stages `deforum` → `grading` → `upscaling` → `assembling` → `complete`.
- The result is `$R\Cosmic Rain - Deforum 1-04 test - with music.mp4`.
- Make a phone copy with `..\cosmic-full\phone_copy.py <in> <out> <passlog>`.

### Times on the A2000

- Deforum runs at about **4.3 s per frame** (848×480, 28 steps). One minute of video is 855 frames, about **62 minutes**.
- Grading and upscaling take about **10 minutes** per minute of video.
- A full 7:43 track would take roughly **8–9 hours**.

### What to change for a new song or a longer piece

These are at the top of `run_deforum.py`. Run from a copy of the script, not the original.

- **Paths:** `OPENING`, `TRACK` and the remake source `DRAFT1`.
- **Timing:** `START` (when Deforum takes over from the opening) and `END` (seconds).
- **`THEMES`:** the worlds it travels through. They are spread evenly over the run, one about every 14 s for a 1-minute piece. For a long run use more themes, about one every 15–20 s.
  - Write them like the approved ones: a place or world, never people or faces.
  - Make each one grow out of the one before: "The ocean spirals grow into …".
- **`MODE`:**
  - **`ending` (C):** nothing to add. Start from a **rich, dark, colourful** frame. Deforum locks every frame's colours to the first frame, so a pale start frame makes the whole run washed out. That's what happened to the Avastha ending.
  - **`remake` (D):** needs a video of the same minutes to sit underneath (`DRAFT1`). It shapes colour, contrast and depth, not content.

### Don't

- Don't edit `settings-8s.json`.
- Don't add beat-synced zoom or brightness pulses.
- Don't speed up the music.
- Don't put Cosmic Rain stills or other photos on the hybrid layer.
- Don't grade footage that isn't Deforum.
- Don't grade twice.

## 4. Settings actually used (for reference)

- `approved/test-C-ending-settings.json`
- `approved/test-D-remake-settings.json`

These are the exact Deforum settings submitted for the approved tests.

**Shared by both tests (the approved base):**

| Setting | Value |
|---|---|
| Size and speed | 848×480, 15 fps |
| Sampler | DPM++ 2M, 28 steps, cfg 6, seed 271828 |
| Animation | 2D: zoom 1.005, angle 0.06, x 0.12, y −0.05 |
| Noise and colour | noise 0.02, LAB colour coherence |
| Strength | 0.62 / 0.55 / 0.48 / 0.55 / 0.60 every 120 frames |
| Submit option | `img2img_fix_steps` |

**Only in D:**
- Hybrid video compositing of `guide.mp4` with composite alpha `0:(1.0), 15:(0.70), 30:(0.25), 45:(0.05)`, then 0.05 to the end.
- Strength `0.98 → 0.88 → 0.74` over the first 30 frames.
- The camera eases in over the first 45 frames.

## 5. If something goes wrong

- **`Submission outcome uncertain`:** a submission marker exists but no receipt. Check `http://127.0.0.1:7865/deforum_api/jobs` before you submit anything again.
- **The service was restarted mid-run:** the runner resumes from the frames on disk (`resume_from_timestring`). If it can't, it stops rather than starting over.
- **Port 7865 answers HTTP 500 for every request:** restart the service with the Stop then Continue buttons.
- **Washed-out pale colours:** the start frame was pale. Use a darker, more saturated start frame.
