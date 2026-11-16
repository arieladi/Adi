# Deferred work — the register

Every piece of work that a ruling, a review or a slice postpones is a row here until it is done.
The director does not track these; win does (ADR-0239 d3).

**Rules**
- A ruling, review or PR that defers something adds its row in the same change.
- A row closes only with the PR that does the work; the closing PR names the row's id, and win moves
  it to "Done" with that PR number.
- **Release gate:** no release, and no milestone marked complete, while a row that belongs to it is
  open — unless the director re-rules that row by name.
- Win re-reads this register at every milestone gate, and in every hourly check that lands work.

Plan rows (PLAN.md's P2/P3, the milestones) are the plan, not deferrals; this register holds what was
cut out of something that already shipped or was ruled.

## Open

### Control (ADR-0232, ADR-0233, ADR-0239)

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-C1 | The session token, **both ways, the user chooses** (ADR-0239 d1): shown in ADI to paste into each AI tool, or an owner-only file in the user's profile that AI tools find on their own (only while the listener is on) | control seat | with the app listener (CTL-04, CTL-05) |
| DF-C2 | AGT-03, the in-app consent before `project_read` answers a non-local destination; closes ADR-0039's open consent item | control seat + window | before DF-C3 ships |
| DF-C3 | `adi-mcp --connect` to a running ADI, the app's loopback listener (CTL-04) and the in-app host (CTL-09) | control seat | after DF-C1 and DF-C2 |
| DF-C5 | One of each function per channel (ADR-0237 d1) is checked only in the window's edit path (`mix_function_edit.cpp`); the `device.insert` operation does not check it, so a control client could add a second EQ or limiter to a channel. Refuse it in the operation | control seat | now: #512 merged on 16 November |

### Mix View (ADR-0236, ADR-0237, PRs #537 onward)

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-M1 | Live checks of the seat's builds (briefs in the seat's outputs for #537, #541, #542), run off the director's desktop or by him | director or a Claude tester | before each slice is called done |
| DF-M2 | The decision guide (the seat's `DECISION-GUIDE.md` in the director's private research folder) for every open Mix View choice, ADI-View D1 to D13 included | Mix View seat | now |
| DF-M3 | CPU and sound comparison of every Mix View function against the plug-in it comes from (the VST3 builds); the director deferred CPU collection for the first slices | Mix View seat + a Claude tester | before the Mix View is called done |
| DF-M4 | The analyser after a sample-rate change: safe tap replacement (MX-30); today the Main analyser stays unavailable until the project reopens | Mix View seat | MX-30 |
| DF-M10 | STACK pins, search and row size persist across launches | Mix View seat | before the Mix View is called done |
| DF-M11 | DEV-05/MX-24 device sidechain keys (assigned by win to the Mix View seat on 13 November): routing tap storage as schema minor 1.22, destination device identity, key width, route lifecycle, planner and realizer, reversible cleanup on device delete; Carve and Focus stay unavailable until then; the engine seat reviews | Mix View seat | before Carve/Focus ship |
| DF-M12 | Focus's audible compatibility choice, for the director through the decision guide | Mix View seat → director | with DF-M2 |
| DF-M13 | Focus FIR at 192 kHz still overruns callbacks (10 nodes: ~4.4 ms worst vs 2.67 ms budget; 1 node: 4–9 % of callbacks over); pace the remaining work over the remaining samples instead of the worst-case per-sample rate | Mix View seat | before Focus ships |
| DF-M14 | Focus: after a missed FIR deadline `launchFir` returns false without resetting `firFill`, and the next sample writes past `firInput` on the audio thread; clear the FIR state and finish the block dry | Mix View seat | before Focus ships |
| DF-M15 | Limiter: a claimed latency offer is admitted only while the device processes, so switching it off in that window locks its structural edits and stalls the project's committed-delivery queue until it is switched on; release the offer on bypass | Mix View seat | follow-up to #541 |
| DF-M16 | Non-finite input latches the gate open and the de-esser at full attenuation, and leaves NaN in the key EQ and matched filter sections; zero non-finite samples once per block before any state changes (the engine does not sanitise plug-in output) | Mix View seat | before those functions are registered |
| DF-M17 | De-esser, gate, opto and clean-key cores refuse 176.4/192 kHz (96 kHz caps); ADR-0157 makes ADI 44.1–768 kHz, with tests at 192 and 384 kHz | Mix View seat | before those functions are registered |
| DF-M18 | Key EQ near Nyquist: BandPass loses up to −10.4 dB at 20 kHz, a Q40 Bell reaches +6.4 of +12 dB; add magnitude tests against the 384 kHz-built response, then compare with the reference before changing the policy | Mix View seat | before the key EQ ships |
| DF-M19 | The Mix View's Overview spectrum and analyser dock ignore the spectrum-slope setting | Mix View seat | follow-up to #565 |
| DF-M21 | One meaning for a native function's switch: a Mix View row's dot bypasses it (`device.setEnabled`), the device area's activator removes it (`device.remove`), so a function bypassed in the Mix View shows off in the device area where the only click removes it. ADR-0237 d3 says disabling removes, as deleting a Live device does | Mix View seat, the director if the two readings need a ruling | before the Mix View is called done |
| DF-M22 | Adding a function from the Mix View (ADR-0237 d3): today only the device area's + adds one; the Mix View's empty text sends the user there | Mix View seat | before the Mix View is called done |
| DF-M23 | Presets at both levels (ADR-0237 d4): each function, and a channel's whole set. Loading a set replaces the channel's native functions in one undo step (ADR-0240 d3). Nothing a user can reach exists yet; only the state save/load underneath. Win still owes the ADR for where preset files live, their format, and where a set's functions sit among ordinary plug-ins, from the seat's proposal | Mix View seat + win | after the first complete compressor |
| DF-M24 | The compare view of the analyser tab (ADR-0237 d5): the comparison, one-second average and triggered-scope parts are merged as tested libraries with nothing on screen using them | Mix View seat | before the Mix View is called done |
| DF-M25 | The analyser window (ADR-0237 d9) shows one mode at a time and its button back to the Mix View lands on the analysis tab, not on the channel's row | Mix View seat | before the Mix View is called done |
| DF-M26 | The withdrawn slots (ADR-0212) are still in the row model (`stack_model.cpp` default kinds, the Ducking kind included) and described in `mix_view_provider.hpp`; nothing draws them. Remove them | Mix View seat | follow-up |
| DF-M28 | The compressor, gate and de-esser are merged or open as cores no user can load. First a complete compressor a user can add (internal key only, no device keys needed), then DF-M23, then DF-M11, then the other styles and the gate and de-esser | Mix View seat, on the director's go | the seat's next run |
| DF-M29 | #548: the native EQ passes dry audio after a processing-precision switch (float to double inside a prepared stream is rejected, the same-format rebuild skips the prepare and the refusal is discarded); reproduced, a regression against main. Force a backend prepare on a width change, with a test | Mix View seat | before #548 lands |
| DF-M30 | #548: a latency-neutral EQ plan edit now reaches audio 2 to 32 blocks later depending on the scheduler (always 1 block before), and offline render does not wait for it, so two exports of an automated Structure change can differ. Offline render waits for the plan it asked for | Mix View seat | before #548 lands |
| DF-M31 | #548: the parked-failure text says "Reload the device to retry" but no reload command exists and reverting the edit does not clear the failure. Give the failure a way to clear, or say what does | Mix View seat | follow-up to #548 |
| DF-M32 | #579 held: the Clean compressor's envelope decays into subnormal doubles after about two minutes below threshold and costs about 2.2 times the CPU until the signal next crosses it (3.63 to 7.84 ms per second of audio at 48 kHz, measured 16 November). Zero the state below a floor or scope flush-to-zero in the adapter, with a long-silence test | Mix View seat | before #579 merges |
| DF-M33 | #580 held: a native function that is already off can no longer be switched on, because both switches now remove it. It arises from projects saved after the old Mix View bypass, from any control client (`device.setEnabled` is still accepted for a native function) and from Bounce in Place. Re-enable on click, or refuse the disable in the operation and re-enable on load (ADR-0237 d3) | Mix View seat | before #580 merges |
| DF-M34 | The native EQ, Limiter and Compressor adapters report no finite tail, so a silent channel never suspends them: the compressor costs about 0.36 % of a core at 48 kHz for ever (ADR-0237 d6, ADR-0043). Report a finite tail and reset on wake | Mix View seat | before the Mix View is called done |
| DF-M35 | The compressor's Auto Gain make-up is recomputed and applied in one sample on a control edit while gain reduction follows the envelope: Threshold −18 to −24 dB steps the gain by +1.19 dB, Auto Gain off by −4.84 dB (probe, 16 November). Compare with the plug-in it comes from and listen; ramp the make-up if they differ | Mix View seat + a listening check | with DF-M3 |
| DF-M36 | `prepare` takes the reactivation Hold before the unchanged-format return in the compressor, EQ and built-in hosts (`mix_compressor_host.cpp:97`, `mix_eq_host.cpp:181`, `builtin_host.cpp:96`), against the gate's contract (`device_model.hpp:636`): a graph rebuild during playback can pass a segment unprocessed. Shown possible by probe, not measured in the engine | Mix View seat, the engine seat reviews | follow-up to #579 |
| DF-M37 | The compressor adapter writes no output when processed before any prepare (no `prepared_` guard), and discards the core's status, so a refused segment would leave dry audio until re-prepare (not reachable through the device today) | Mix View seat | follow-up to #579 |
| DF-M38 | Tests that cannot fail for their stated reason: #579's "remove and Undo restore the native function" (asserts only that the ops succeeded) and its `exactAudio` (the adapter against the same core); #580's "one Undo removes only the function added in Mix View" (`instanceFor` also returns retired instances) and no test of the Mix View dot itself; #552's suite has one check against an independent expectation and none for long silence, attack and release times or the Auto Gain value | Mix View seat | follow-up to #579 and #580 |
| DF-M39 | A commit that lands while the Mix View's add menu is open makes the click do nothing, with no message; with the control slice merged this can happen. Say so in the Status Bar | Mix View seat | follow-up to #580 |

### Window (ADR-0208, ADR-0240; the lane's own needs table is in `collab/window.md`)

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-W1 | The Clip View and the MIDI Note Editor (STEP-7-PLAN 7.7, worklist G1 to G3), approved 16 November (ADR-0240 d1). On main: the view, MIDI clip creation, note editing and 0 to deactivate (#586, #588). In review: the Main panel's fields and the loop brace (#589), the Pitch and Time panel and the Scale chooser (#591) | win | closes when #589 and #591 merge |
| DF-W2 | The Clip View's later parts, as STEP-7-PLAN 7.7 step 5 places them: the Pitch and Time utilities beyond Fit to Scale, note preview through the track, the Envelopes and MPE tabs, the Chance lane, multi-clip editing, and the Sample Editor's warp controls | win, then the window lane | after DF-W1 |
| DF-W3 | C5, remark pips and the Info View's split: ADI's own addition, so its picture goes to the director before any of it is built. The lane was asked on 16 November to draw it | window lane → director | the picture |
| DF-W4 | B8, Main's loudness meter in the mixer, held for the Live 12 meter readings | window lane | the readings |
| DF-W5 | A home for `midi.port.remote` (every `midi.port.*` row is Device scope, which the app settings refuse), so the window can take MIDI in for MIDI Learn and MIDI Map Mode (N10) | win | before the window's MIDI Map Mode |
| DF-W6 | The nullable `tracks.role` column with its four values in the schema and SPEC, a schema minor and its migration (N38); until then `track.setRole` refuses and every role reads null, so the ★ Reference cannot be chosen | win | before the Mix View's Reference; the minor after the sidechain keys' 1.22 |
| DF-W7 | The lane's three notes for win of 10 November: the ZIP export's progress, which Live shows and `collectExport` does not report; the browser sidebar's labels beyond Samples and Places; the place of Rec-Q and Play-Q, which is ADI's own and Live has no header boxes for | win, the director for the third | next window review |
| DF-W10 | The Clip View beyond Live, what is still not ruled: the part of §3 that §3.5 does not cover (relative automation, modulators aimed at clip and note parameters), and the research `docs/CLIP-VIEW-ADDITIONS.md` §5 says is missing | the director for the research and any further picks | his word |
| DF-W11 | The four note-editor additions from Cubase (ADR-0241 d2): line tools in the Velocity and controller lanes; chord editing; the trim tool and the length functions; handles on a selection (tilt, compress, scale) | win | now, after the Live baseline's open PRs |
| DF-W12 | Per-note gain and pan curves in the MPE tab beside pitch, slide and pressure (ADR-0241 d3) | win for the editor; the engine seat for `noteExpression` ops and playback (MI-08) | with the MPE tab |
| DF-W13 | Pitch editing of audio, P1 (ADR-0241 d1): its plan card, an ADI name, the pitch tracker (shared with TM-WT-09), the segment tables, the engine and the editor; the director's research on the source DAW first (`docs/CLIP-VIEW-ADDITIONS.md` §5) | win for the card and the design; the engine seat; the director for the research | after the Sample Editor's warp markers (TM-WT-02, TM-WT-06) |
| DF-W14 | Bitwig's operators as note properties (ADR-0241 d4): repeats, occurrence, recurrence, with chance; a side table, playback, and the editor's controls; PLAN MI-07 and MI-09 reworded | the engine seat for the table and playback; win for the editor and the plan's wording | P2, with MI-09 |
| DF-W15 | Win's picks of §3.5, taken (ADR-0242 d1): N1 note colours, N2 the quantize panel, N3 quantize that can be taken back, N4 velocity functions, N5 delete doubles and restrict polyphony, N6 several controller lanes, N7 scale help; A1 harmony voices, A2 Detect Silence, A3 clip gain in the editor, A4 expression curves per audio event; M1 take envelopes for pitch and mute, M2 a shape tool that writes points. N4 and N5 are built (the Functions tab, 16 Nov); of N2, swing is built with Live's Quantize settings (17 Nov) and the rest of the panel is open. The window's next: N1, the rest of N2; the lanes' with the Envelopes tab; those that need tables or the engine (N3, A1, A4, M1) with them | win; the engine seat for what needs tables | after DF-W11 |
| DF-W16 | The rest of §3.5, taken on condition that none impairs a function of Live's (ADR-0242 d4): N8, N9, N10, N12, N13, N14, N16 to N19; A5 to A8, A10, A11; M3 to M5. One that would change a Live function goes back to the director first; those known only from memory (FL Studio, Logic, Studio One) are researched first | win; the director for the research | after DF-W15 |
| DF-W17 | The audit of Live's own clip editing against `collab/window/parity-7.7-clip-view.md`, from the director's Ableton research and the manual (ADR-0242 d5), and what it finds missing; editing several clips together, each in its own colour, built now | win | now |
| DF-W18 | `adi_ui_shell_tests` failed seven checks once in the reference-audition case ("a tap under 200 ms latches it" and those after it) on win's machine under load, and passed on two reruns: the case measures real time. Give it the input clock seam (ADR-0231) | window lane | follow-up |
| DF-W9 | A MIDI clip cannot be heard from the window. No instrument can be put on a MIDI track: the browser's Plug-Ins category lists nothing, though the scanner and the registry are on main (`plugin_scanner.*`, `plugin_registry.*`), and no built-in instrument is on main (#278 is open). The browser's Plug-Ins: list the registry's VST3 and CLAP plug-ins and load one onto the selected track, as D4 loads ADI's own devices | window lane; the engine seat if the registry lacks what the browser needs | next, ahead of the lane's list: the Note Editor (DF-W1) is silent until then |

### Engine (win's log of 12 November)

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-E1 | End-marker rounding in the tempo map's seconds↔ticks conversion; ClipPlayback's pinned `playEnd` from absolute endpoints (≈12.5 % of linear clips lose a tail sample today) | engine seat | next engine session |
| DF-E2 | #530 held: which End wins when `end_marker_ticks` and `source_end_ns` disagree; keep both in sync on Start/End edits | engine seat | before #530 merges |
| DF-E3 | `ProgressiveDecoder`: drop a still-queued job when its last lease goes | engine seat | follow-up |
| DF-E4 | `Index::scan`: a throw after commit reported as a rollback; a vanished subdirectory still fails the scan | engine seat | follow-up to #533 |
| DF-E5 | #530's linear sample-defaults test with a nonzero ns offset | engine seat | with DF-E2 |
| DF-E6 | Count-in clicks on the latency-compensated clock; when latency exceeds a beat, measure Live first | engine seat | follow-up to #527 |
| DF-E7 | On main: `resizeFrozenEnd` refuses `end <= first`; split's `keepRight` End-marker check | engine seat | follow-up |
| DF-E9 | Automation Undo deletes and re-inserts lane and data rows: a later minor's CASCADE child or extra column is lost or refused. Restore in place (UPDATE named columns, insert only missing data rows); named-column INSERT/SELECT; test with a CASCADE child and extra columns (outside review E3.2, verified) | engine seat | next engine session |
| DF-E10 | `adi_link_session_tests` failed once on ubuntu arm64 (#548, 14 November: "shared musical timeline continues while local transport is stopped") and passed on re-run; the tests open real Link sessions while ctest runs suites in parallel on one host. Isolate them or run them serially | engine seat | next engine session |
| DF-E11 | The audio callback sets no flush-to-zero or denormals-are-zero, although the settings catalogue says denormals are "flushed for the callback (ADR-0042 d6)"; only three cores scope it themselves (`mix/eq/Core.cpp`, `mix/clip/Core.cpp`, `dsp/multiband_dynamics.cpp`), and the render workers copy the callback's environment. Found by reading on 16 November after DF-M32's measurement. Set it once for the callback and its workers | engine seat | next engine session |
| DF-E12 | `adi_project_files_tests` failed once on ubuntu arm64 (#587, a docs-only change, 16 November): `Store::create` of `Cached.adi` beside a copied project returned null ("cached replay fixture") after 0.38 s, while main's own run on the same merge passed that leg. The fixture throws without the `StoreError`; print it, then find the cause (the suites run in parallel) | engine seat | next engine session |

### Repository

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-R1 | Clean the repository's text and history of the private research-tool references the director named on 13 November (his option 3: back up, rewrite history clean, he recreates the GitHub repository, win pushes, reopens the open PRs, restores the site and re-points every worktree) | win, with the director for the GitHub steps | after the Mix View seat's current run, when he says go |

## Done

| Id | What | Closed by |
|---|---|---|
| DF-W8 | The window could not make a MIDI clip | #588 (067fb55f): a double-click on an empty MIDI lane, and Insert Empty MIDI Clip(s) on Ctrl/Cmd+Shift+M (16 Nov) |
| DF-M20 | The native limiter measured loudness nobody reads | #581 (2fb959ec): the measurement is skipped while recording is off (16 Nov) |
| DF-M27 | The director's answers that were only in the Mix View seat's log (rows default to channels with Mix View tools; the optional analyser dock; and seven more the seat listed) | ADR-0240 d4 (16 Nov) |
| DF-M5 | Seek: mark the analysis epoch where the audio thread applies Locate, so no pre-seek window publishes as post-seek | #541 (7368447e): each capture span carries the transport revision the audio thread applied |
| DF-M6 | #537: a STACK parameter gesture stays open when Begin is taken but the older gesture's commit is refused; tests for the stale-tile Delete/Backspace guards | #541 (7368447e): the refused Begin's owner is cancelled; keysLeaveRows tests the stale-tile keys |
| DF-M7 | #540: tap storage piles up while the engine is stopped | #541 (7368447e): one deferred plan kept while audio has not adopted the last |
| DF-M8 | Held PRs: #541 (automatable latency-changing controls abort export; a 2 ms background thread per limiter), #542 (≈3 ms of UI-thread work per 20 ms per EQ while unseen; Carve/Focus tabs before they are enabled), #545 (Focus FIR over the callback budget; fixed 44.1 kHz) | #541, #542, #545 merged after their repairs (15 Nov); #545's 192 kHz remainder is DF-M13 |
| DF-M9 | Minor findings on #543, #544, #546 (mono Pan/Audition in Carve; three tests that cannot fail) | #543, #544, #546 merged with their findings fixed (15 Nov) |
| DF-E8 | The Gemini round on #518, #528, #516 | #518 clean; #528 fixed in #575; #516 became DF-E9 (14 Nov) |
| DF-C4 | The Gemini security round on the MCP server, each finding verified | #512: ddd704bd and bb97ce23 (14 Nov) |
