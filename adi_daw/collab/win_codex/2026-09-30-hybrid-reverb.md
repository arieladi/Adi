# Hybrid Reverb — checklist before code

Read Live 12 Audio Effect Reference pp.590–596. Own wording; mission explicitly chooses convolution plus Surge reverb rather than copying Live's five proprietary algorithms.

- Convolution and algorithmic reverb combine in Serial, Parallel, Algorithm-only and Convolution-only routes; Blend weights their contributions.
- Send changes only the wet excitation; predelay can use milliseconds or note duration at host tempo.
- Import an actual time-domain impulse, retaining stereo channels; prepare its attack/decay/time-scale off audio, then publish through [adi.sample]'s existing host mechanism.
- Use the existing GPL-3 Surge Reverb1 adaptation for the algorithmic engine, with decay, room size, algorithm delay and freeze/input-freeze controls.
- Dry/Wet and stereo width act at the output. Compensate dry/algorithm timing for the convolution's fixed intrinsic latency and report it to the host.
- Exact convolution oracle, routing, latency, all block sizes 32–4096, allocation audit; LibPdEngine top-level patch, clean console, floor-bin-before-peak proof, sample replacement while rendering; float WAV only.

Scope choice: implement the requested two-engine core. Live's five algorithm identities, four-band EQ, Vintage and Bass Mono are follow-up parity controls, not names applied to a different algorithm. IR attack/decay/size are host preparation options; rebuilding a long kernel on the audio thread is forbidden. All unverified control values get one ADI table.
