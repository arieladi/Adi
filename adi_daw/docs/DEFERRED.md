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

### Mix View (ADR-0236, ADR-0237, PRs #537 onward)

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-M1 | Live checks of the seat's builds (briefs in the seat's outputs for #537, #541, #542), run off the director's desktop or by him | director or a Claude tester | before each slice is called done |
| DF-M2 | The decision guide (`NowWeReverse/ADI-View/mixview-seat/DECISION-GUIDE.md`) for every open Mix View choice, ADI-View D1 to D13 included | Mix View seat | now |
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

### Repository

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-R1 | Clean the repository's text and history of the private research-tool references the director named on 13 November (his option 3: back up, rewrite history clean, he recreates the GitHub repository, win pushes, reopens the open PRs, restores the site and re-points every worktree) | win, with the director for the GitHub steps | after the Mix View seat's current run, when he says go |

## Done

| Id | What | Closed by |
|---|---|---|
| DF-M5 | Seek: mark the analysis epoch where the audio thread applies Locate, so no pre-seek window publishes as post-seek | #541 (7368447e): each capture span carries the transport revision the audio thread applied |
| DF-M6 | #537: a STACK parameter gesture stays open when Begin is taken but the older gesture's commit is refused; tests for the stale-tile Delete/Backspace guards | #541 (7368447e): the refused Begin's owner is cancelled; keysLeaveRows tests the stale-tile keys |
| DF-M7 | #540: tap storage piles up while the engine is stopped | #541 (7368447e): one deferred plan kept while audio has not adopted the last |
| DF-M8 | Held PRs: #541 (automatable latency-changing controls abort export; a 2 ms background thread per limiter), #542 (≈3 ms of UI-thread work per 20 ms per EQ while unseen; Carve/Focus tabs before they are enabled), #545 (Focus FIR over the callback budget; fixed 44.1 kHz) | #541, #542, #545 merged after their repairs (15 Nov); #545's 192 kHz remainder is DF-M13 |
| DF-M9 | Minor findings on #543, #544, #546 (mono Pan/Audition in Carve; three tests that cannot fail) | #543, #544, #546 merged with their findings fixed (15 Nov) |
| DF-E8 | The Gemini round on #518, #528, #516 | #518 clean; #528 fixed in #575; #516 became DF-E9 (14 Nov) |
| DF-C4 | The Gemini security round on the MCP server, each finding verified | #512: ddd704bd and bb97ce23 (14 Nov) |
