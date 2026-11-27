2026-11-27 — Codex took the pitch engine seat.

## For win

- Please reserve a P1 plan row and appendix card for ADR-0241 d1 and ADR-0242 d2. TM-WT-09 Melody can use the same monophonic analysis.
- Please reserve the next available project schema minor for clip-owned pitch edit tables and the proposed op name `clip.setPitchEdits`. One batch replaces a clip's edit rows and captures the previous rows for Undo. Store stable note identities against source hash and analysis version/configuration. Authored edits need their own project rows because ADIA is evictable.
- Please record `PNOT` as the optional ADIA section for source pitch notes. Its analysis version and configuration are independent of the project schema.

| Item | Change | Proof | State |
| --- | --- | --- | --- |
| Typed note edits | Atomic worker commands for pitch, straightening, tilt, scale quantization, timing, formant, gain/mute, drift and transition. | 35 checks passed under strict GCC and ASan/UBSan. Making correction a no-op failed five contour, drift and transition checks. The fixture measured +100.25 cents with 30 cents of tilt, a 12.50 ms move and a 1.50× stretch. | Engine seam verified. Project ops, persistent rows and undo wiring depend on win's reservations. |
| Monophonic analysis | A worker-only difference-function pitch tracker with source-frame notes, 5 ms contour points, fitted drift and residual vibrato. Sustained pitch plateaus separate connected notes. | A continuous-legato regression went from one merged note to three. All 17 VocalSet notes matched, with maximum errors of 4.97 cents, 34.06 ms onset and 22.45 ms offset. The synthetic scale passed at 8, 44.1, 48 and 96 kHz. | Verified in the chain. Long scoops can divide a note. Vibrato can mask a weak connected change. |
| Clip analysis cache | Clip-to-source read seam and a versioned `PNOT` section in the existing content-addressed ADIA files. Reuses the decoded media path and carries the clip's source-frame window, direction and analysis identity. | 18 CTest checks passed, including cache corruption and rebuild, legacy metadata, shared reuse after source removal, and a clip-to-edit-to-render proof. The 0.5 s stereo fixture measured 17.44 ms cold and 0.11 ms warm in GCC Debug. | Verified in the chain. Worker PCM limit 256 MiB and cached pitch payload limit 16 MiB. |
| Offline render | Offline monophonic note rendering with latency compensation, pitch-preserving time stretch, contour automation, formant and gain. Source and render use the same total frame count. | 29 checks passed under strict GCC and ASan/UBSan. Passthrough failed ten checks. In the sung scale, the edited note measured 633.029→670.690 Hz, +100.042 cents, with at most 0.326 cents of error across five independent FFT windows. Samples outside the edit were bit-identical. The timing fixture stretched 14,400→21,600 frames with 0.01 cents of pitch error. | Offline engine seam verified. DSP up to 192 kHz and pitch/time ratios from 1/16 to 16. |

## Evidence and reuse

- The sung scale is the VocalSet 13.67 s female 2 straight vowel-e exercise. The reference was the published `extended 4` annotation for that file. Its median pitch values describe the performed notes. The audio and annotations are CC BY 4.0 and the run used copies in scratch. [Audio](https://zenodo.org/records/1442513) · [Annotations](https://zenodo.org/records/7061507)
- A second, separately articulated sung scale exposed two extra note divisions within an extended scoop. The analyzer currently searches 55–1,800 Hz and requires roughly 30 ms of isolated voicing or 100 ms of support for a connected change. A model could improve octave and voicing decisions in noise and distinguish an expressive scoop from a target note. [Second recording](https://freesound.org/people/mooncubedesign/sounds/442571/)
- TM-WT-09 Melody can read the same cached notes, then use clip direction, Warp markers and transient boundaries when projecting them to MIDI. Harmony calls for a polyphonic tracker and Drums can use onset analysis. [Live 12 manual](https://www.ableton.com/en/live-manual/12/converting-audio-to-midi/)
- Shared edits on the chain are CMake registration, the ADIA section description, and a cancellable overload of the existing media resolver. The rest is in pitch-specific files. The engine returns through worker seams.

## Readings owed

- Cubase side-by-side gate for exact tilt anchor/range rules, adjacent segment timing, interaction with Warp, and how correcting a segment behaves after split or glue. The current engine takes explicit destination bounds and can apply adjacent changes in one batch. [Cubase manual](https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/sample_editor_variaudio/sample_editor_variaudio_segment_editing_c.html)
- Reason's exact incoming transition curve and default. The engine blends adjacent transpose offsets with a smoothstep taper and uses the documented limit of 200 ms or half the note. [Reason 14 manual](https://docs.reasonstudios.com/reason14/audio-editing-in-the-sequencer)
- Live 12.4 track and rack names, quantization, velocities, Warp/transient mapping and Harmony's polyphony ceiling are still measurements for TM-WT-09. [Live 12 manual](https://www.ableton.com/en/live-manual/12/converting-audio-to-midi/)

## Chain gate

- `seats/pitch-chain` at `83601c9e`, based on `origin/main` at `17a68880`.
- GCC 14.2 Debug with `ADI_WERROR=ON` and the headless Pd tier built successfully. The cloud disk filled during the first link pass. The completed build stripped executable debug sections at link time.
- CTest passed 282 of 282 suites in 117.48 s. All five `tools/validate_*.py` validators and the SPEC layout check passed. Disabling pitch-cache publication in a scratch build failed the ADIA regression as expected.
- The clip-owned schema and operation reservations are the outstanding integration dependency. All five seat log branches are present on origin.
