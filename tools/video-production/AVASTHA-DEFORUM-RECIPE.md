# The approved Deforum method (Avastha – Quantum Ascent, Oct 2026)

This is the Deforum workflow Adi approved for Avastha – Quantum Ascent. It's written up here so it can't get lost again. It was rebuilt from the archive at
`\\orc-vst\D\Avastha-AI\Outputs\QuantumAscent-Archive-20261030`. The same folder on ORC-VST is `D:\Avastha-AI\Outputs\...`.

**Where the source files are**
- The project folder is `local\D\AvasthaAI\Projects\Avastha - Quantum Ascent\`.
- The approved section is `deforum-section-050-123` (the scripts, `finish-review.txt` and the result).
- The continuation is `deforum-ending-184s` (`worker.py` and `production-notes.txt`).
- The project rules are `PROJECT-DECISIONS.txt` and `color-policy.json`.
- The approved masters are in `Approved\`.

## What it is

It's the real Deforum extension, not a custom recursive script and not Wan. Note that the never-approved `CosmicRain-20261025` renderer is a different thing: it's a custom recursive script, not this method.

**Setup**
- **Service:** A1111 webui with the Deforum API, on ADI-PC port 7865.
  - Install: `D:\AvasthaAI\ActualDeforum`.
  - Launcher: `launch_actual.py`.
  - It runs on the A2000.
- **Model:** SD1.5 `DreamShaper_8_pruned.safetensors`.
- **Base settings:** `D:\AvasthaAI\ActualDeforum\settings-8s.json`. These are approved, so keep them unchanged.

| Setting | Value |
|---|---|
| Size and speed | 848×480, 15 fps |
| Sampler | DPM++ 2M, 28 steps, cfg 6, seed 271828 |
| Animation | 2D: zoom 1.005, angle 0.06, translation x 0.12, translation y −0.05, border replicate |
| Noise and colour | noise 0.02, LAB colour coherence, cadence 1 |
| Strength envelope | 0.62 / 0.55 / 0.48 / 0.55 / 0.60 at frames 0 / 35 / 60 / 90 / 119, repeated every 120 frames |
| Submit option | `img2img_fix_steps: true` |

**Joining it to existing footage** (section 050–123, approved)
- Feed the neighbouring footage as `video_init_path` with hybrid compositing:
  - `hybrid_composite Normal`
  - `hybrid_motion None`
  - generate input frames, and use the first frame as the init image.
- Composite alpha `0:(1.0), 15:(0.70), 30:(0.25), 45:(0.05)`, then mirrored at the exit.
- Strength `0.98 → 0.88 → 0.74 → 0.62` over the first 45 frames.
- Camera, noise and zoom ease in from 0 over those 45 frames.

**Prompts**
- Evolving "themes" every ~270 frames (18 s) for long runs, or every ~60 frames for short ones.
- End each with ", visionary psychedelic painting, rich intricate detail, continuous organic transformation".
- No beat-synced motion was used: "Repeat the approved strength envelope without any beat-synced motion".

**Finishing**
1. **Colour:** the approved LUT `color-grading-test/generated-lut-16.npy`, sha256 `847229a3…c48a`. It's applied on the GPU, checked against PIL, to Deforum frames only. Never grade other footage, and never grade twice. The same file is on ADI-PC as `D:\AvasthaAI\CosmicRain-20261025\approved-lut.npy`.
2. **Upscale:** RealESRGAN_x4plus, then RIFE 4.26 (`D:\AvasthaAI\EnhancerV2\renderer.py`) to 1280×720 24 fps.
3. **4K60:** for the final master, the same tools go to 3840×2160 60.

**Rules from Adi**
- Coherent, evolving worlds, not random collage.
- A single rotation is preferred over reversing spins.
- Short manual warp/morph joins were approved.
- Never speed up the music.
- Colour grading goes on Deforum footage only.

## Cosmic Rain test (2026-11-12)

**Script:** `cosmic-deforum/run_deforum.py`. It applies exactly the method above to Cosmic Rain 0:00–1:04:
- Adi's original 7 s opening.
- Then Deforum from the opening's 7 s frame.

**Cosmic Rain stills steering the run**
- Six approved stills come in through the same hybrid layer: the drop to the ocean planet, the jellyfish, the goddess under water, the elders, the drum circle on the drop and the brain.
- Each is composited at alpha 0.40 for about a second, on a bar line.
- The prompt turns to that still's subject a second before.
- In between, Deforum runs freely.
