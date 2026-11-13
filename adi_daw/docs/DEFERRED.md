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
| DF-C4 | The Gemini security round on the MCP server (prompt ready: `D:\Gemini\2026-11-12-round4\1-mcp-security.txt`), each finding verified | win | when Gemini is available; before DF-C3 |

### Mix View (ADR-0236, ADR-0237, PRs #537 onward)

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-M1 | Live checks of the seat's builds (briefs in the seat's outputs for #537, #541, #542), run off the director's desktop or by him | director or a Claude tester | before each slice is called done |
| DF-M2 | The decision guide (`NowWeReverse/ADI-View/mixview-seat/DECISION-GUIDE.md`) for every open Mix View choice, ADI-View D1 to D13 included | Mix View seat | now |
| DF-M3 | CPU and sound comparison of every Mix View function against the plug-in it comes from (the VST3 builds); the director deferred CPU collection for the first slices | Mix View seat + a Claude tester | before the Mix View is called done |
| DF-M4 | The analyser after a sample-rate change: safe tap replacement (MX-30); today the Main analyser stays unavailable until the project reopens | Mix View seat | MX-30 |
| DF-M5 | Seek: mark the analysis epoch where the audio thread applies Locate, so no pre-seek window publishes as post-seek | Mix View seat | follow-up to #537 |
| DF-M6 | #537: a STACK parameter gesture stays open when Begin is taken but the older gesture's commit is refused; tests for the stale-tile Delete/Backspace guards | Mix View seat | follow-up to #537 |
| DF-M7 | #540: tap storage piles up while the engine is stopped | Mix View seat | follow-up to #540 |
| DF-M8 | Held PRs: #541 (automatable latency-changing controls abort export; a 2 ms background thread per limiter), #542 (≈3 ms of UI-thread work per 20 ms per EQ while unseen; Carve/Focus tabs before they are enabled), #545 (Focus FIR over the callback budget; fixed 44.1 kHz) | Mix View seat | before they merge |
| DF-M9 | Minor findings on #543, #544, #546 (mono Pan/Audition in Carve; three tests that cannot fail) | Mix View seat | follow-up |
| DF-M10 | STACK pins, search and row size persist across launches | Mix View seat | before the Mix View is called done |
| DF-M11 | DEV-05/MX-24 device sidechain keys (assigned by win to the Mix View seat on 13 November): routing tap storage as schema minor 1.22, destination device identity, key width, route lifecycle, planner and realizer, reversible cleanup on device delete; Carve and Focus stay unavailable until then; the engine seat reviews | Mix View seat | before Carve/Focus ship |
| DF-M12 | Focus's audible compatibility choice, for the director through the decision guide | Mix View seat → director | with DF-M2 |

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
| DF-E8 | The Gemini round on #518, #528, #516 (prompt ready: `D:\Gemini\2026-11-12-round4\2-engine-merged.txt`) | win | when Gemini is available |

### Repository

| Id | What | Owner | Trigger / before |
|---|---|---|---|
| DF-R1 | Clean the repository's text and history of the private research-tool references the director named on 13 November (his option 3: back up, rewrite history clean, he recreates the GitHub repository, win pushes, reopens the open PRs, restores the site and re-points every worktree) | win, with the director for the GitHub steps | after the Mix View seat's current run, when he says go |

## Done

| Id | What | Closed by |
|---|---|---|
