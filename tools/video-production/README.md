# Shared video-production handoff: Codex / Claude

This folder is the durable handoff for Cosmic Rain and Avastha — Quantum Ascent. Read STATE.json first. Its timestamp is a snapshot, not proof of current process state. The user wants to switch assistants when either reaches a limit.

## Safe takeover and synchronization

1. Read this file, STATE.json, and ignored private/access.txt if present. If private access is unavailable, ask for the configuration; do not invent credentials or claim access.
2. Fetch Git and inspect the current branch and working tree. Fast-forward only when safe. Never reset, clean, discard, or stage unrelated work. Stage explicit files in this folder only.
3. Read remote status files, receipts, worker locks, process command lines and Comfy queue/history BEFORE launching or restarting anything. A missing local shell session does not mean a remote job stopped. Lockfiles alone are not proof that a process is alive.
4. Only one assistant may mutate workers. Record owner and timestamp in STATE.json and commit/push after a user-directed takeover; inspect the latest remote Git state before claiming ownership. Git is NOT an atomic GPU/job lock. Existing Codex monitors can still act: coordinate or pause/delete the relevant monitor using the app's automation tools before conflicting changes. If Claude cannot control those monitors, ask the user to pause them in Codex. Read-only checking can continue meanwhile.
5. Never duplicate an API submission or local generation. For ambiguous receipts, check existing job history. Decode failures reuse saved latents; assembly failures reuse verified frames/videos. Preserve partial files for inspection instead of blindly overwriting them.
6. Update STATE.json with verified status, next action, paths, unresolved issues and approval boundaries after meaningful work. Commit and push handoff/script changes. Never commit credentials, private keys, media, checkpoints, generated frames or huge logs.
7. Git synchronizes instructions/code, not videos, model weights, processes or chat history. Another computer needs separate authorized access to the studio/media. These snapshots do not automatically configure a fresh environment.

## Current priorities

### Cosmic Rain: 120-second draft awaiting review

Remote: D:/AvasthaAI/CosmicRainPilot120-20261026
Local: D:/AvasthaAI/Projects/Cosmic Rain/pilot-120s
Output: Cosmic Rain - DRAFT first 120 seconds - musical edit - with music.mp4

Remote finish-status.json reports complete. Review final-receipt.json and actual output, then copy to local D and deliver. Verify 2880 frames, 1280x720, 24 fps, 120 seconds, complete audio/video decode. Inspect first8seconds, musical changes, reference-switch joins and ending. Do not claim exact visual beat synchronization based on settings alone. No continuation past120seconds until the user approves.

Generation: 28 Wan2.2 TI2V5B image-conditioned shots, native1280x736,121frames each. First8seconds retain the original opening. Most shots begin from the previous generated lastframe; planned reference switches introduce a tunnel and ocean. Wan shift8,20steps,CFG5,uni_pc/simple,denoise1. Latents saved BEFORE decode. Full-temporal4096/tile512/overlap64 decoder avoids previously proven periodic flicker from tiny temporal tiles.

Finishing: crop8pixels top/bottom to exact16:9; RIFE4.26 FP32 on RTX4000 retimes shots to the absolute143BPM grid, phase0.0215seconds (estimated). A middle-shot motion-change peak is aligned to a beat via monotonic Pchip mapping. This is a heuristic, NOT semantic action recognition. Longer calm shots before roughly53.7seconds, shorter energetic phrases afterwards, brief break roughly104.5–107.4seconds. Selected6frame circular reveals and brief symmetry accents. No LUT added to Wan. First pass already includes timing interpolation, but raw generated frames remain available for later color/final finishing.

Finisher was fixed during this run: RGB frame-change reduction must use mean(axis=(1,2,3)), not (1,2). Old finisher was stopped while waiting; valid earlier calm shots retained. Restarted process was PID13756. Never rely on that PID without rechecking command lines. Generation PID was13900. Session IDs from Codex are ephemeral and not portable to Claude.

### Avastha: smoothing/upscaling ONLY

Remote: D:/AvasthaAI/Avastha4K60-20261026
Local: D:/AvasthaAI/Projects/Avastha - Quantum Ascent/final-4k60
Existing source: D:/AvasthaAI/AcidRework-20261025/Avastha - DRAFT 9m54 - Acid rework and Aleph boom - with music.mp4
Source SHA256: 85ac4ac704e167bb52defe3fde0dcadeb038fb90e242a70c249f3515629d9339

The words “Acid rework and Aleph boom” are the EXISTING FILE NAME, not authorization for new generative edits. User explicitly wants only higher resolution and smoother motion. No new Aleph, diffusion, morphing, color grade or scene changes.

run.py waited for Cosmic finishing, then automatically started. Status now enhancing-4k60. Read status.json, worker.lock, enhance.log, effective-config.json and receipts. Uses D:/AvasthaAI/EnhancerV2/renderer.py and models.py, proven RealESRGAN_x4plus followed by RIFE4.26 on RTX4000 alone. FP16 safe warp, tile384/padding32/min64 OOM fallback, flow_scale0.5, libx264CRF17,3840x2160,60fps. Existing model D:/AvasthaAI/Temp/wan30-models/RealESRGAN_x4plus.pth; RIFE D:/AvasthaAI/Tools/RIFE426.

Input14259frames24fps594.125seconds. Expected35648frames60fps594.1333seconds (frame-grid rounding). Copy original AAC stream unchanged; do not re-encode/retime music. No extra sharpening/LUT. Original preserved; distinct output. On completion verify full decode/framecount/geometry/audio and sample detail/motion/joins. Output audio can be about7milliseconds shorter than video from frame-grid rounding.

## User preferences and established findings

- Always deliver16:9. No stretched source ratios.
- Energetic development when kick/bass is present; calm space in breaks. Avoid arbitrary brightness strobes or kick zooms.
- Coherent readable subjects and evolving scenes, not random collages. Prompt targets on beats do not guarantee an object transforms completely every beat.
- Manual short warp/morph transitions were approved; a single rotation preferred over reversing spins. Original music must never speed up.
- DreamShaper8 SD1.5 custom Deforum-style source-guided and recursive workflows produced approved content. Distinguish these from the actual Deforum extension.
- Corrected Wan5B and VACE1.3 diagnostics were approved as high quality. Old tiny temporal tiled decoder caused flicker; same latent with corrected decode was clean. Do not repeat rejected settings.
- VACE14/LTX diagnostics existed but do not assume broad creative approval. LTX video-only resource-adapted testing was not the full official audio/video pipeline.
- RealESRGAN_x4plus and RIFE4.26 are the chosen finishing models. Preserve working settings before experimentation.
- Earlier LUT grading improved Deforum but degraded already-colorful Gemini footage. Grade only approved sections; never automatically grade cloud/Wan footage or double-grade. User may want a short Cosmic color comparison AFTER content approval, then final smoothing/upscale from raw where possible.
- Z-Image and Wan-Alpha experiments were rejected/held. Latest Aleph315–335 was rejected. Do not substitute the newer “both Aleph edits” master for the specified source.
- Original full-track CosmicRain-20261025 renderer remains held. New120-second pilot does not authorize resuming it.
- No paid submissions or model downloads for these jobs. Salon GTX1070 idle; ADI-MOBILE free. All new media on D, not C. Earlier C-to-D migration was partial; never assume C is a junction or delete backups blindly.

## Lessons from the Ayana comparison

Reference: Faders & Burn In Noise — Ayana, https://www.youtube.com/watch?v=N-7r_Zn-VD4 . Treat production-method claims in Gemini's report as hypotheses, not verified facts about the artist's pipeline.

Useful principles: one legible action/subject per shot, coherent chapters, decisive motion aligned in editing to musical accents, short portal/symmetry transitions, retain objects across scene changes where feasible. Do not force a cut every bar.143BPM quarter-note is60/143seconds; bar240/143;2bars480/143. Round absolute timestamps to frames, never repeatedly use40frames/bar at24fps (that drifts). RIFE interpolates requested times; it does not know musical or semantic intent. Do not interpolate across intentional cuts.

## Worker snapshots

worker-snapshots contains reference copies of the new workers and configurations, not a deployable installation. Their runtime filenames are run_pilot.py, finish_pilot.py, plan.json for Cosmic and run.py,config.json for Avastha. They use preexisting studio runtimes/models and absolute paths. Do not run them in the snapshot folder or launch another copy. Live remote scripts/status/receipts take precedence; preserve both versions before reconciling changes. The shared EnhancerV2 renderer/models live remotely and are not bundled here.

## Claude continuation prompt

Read tools/video-production/README.md, STATE.json and private/access.txt (if available) in this Git checkout. Continue the existing Cosmic Rain/Avastha work, not a new render. Verify remote processes and receipts first. Cosmic's120second pilot reportedly finished but needs actual final review/local delivery; Avastha4K60 is already rendering, smoothing/upscaling only. Do not duplicate either job, resume held renders, make paid calls, overwrite masters or extend Cosmic beyond120seconds without approval. Respect the ownership protocol and active Codex monitors. Use Git for durable handoff notes and code; after meaningful work update STATE.json and commit/push only this task's files. Report actual verified results and limitations, never infer completion or visual quality from a prompt/status alone.
