# The AI side of ADI: the held agent stream

**This chain lands only when the director says so.** Chain: `held/agent-chain`. Log: this file, on
`held/agent-log`. No pull request is open for either.

**Mission (the director's "AI integrations").** What an agent can do in a project and what the user sees
of it. It continues where the control seat's log stops (`collab/control.md` on `control/log`, items 1 to
11 and its AGT-05 handoff). Order: first what makes an agent's work safe and visible (consent, the list of
proposed changes heard before they are applied, Stop, the journal's record of who did what), then the
tools an agent lacks to do real work in a project (clips, notes, devices, automation, through the existing
operations), each with tests that drive ADI through its own control interface, headless. The AI mode's
look follows the agreed sheet only (ADR-0262, `collab/window/design/proposals/browser-2.png`).

**Base.** `origin/land/basics-1128c` until it is in `origin/main`, then `origin/main`.
`origin/control/agent-chain` (289611a34) is already inside that base, so the chain starts from it.

**How it is built.** This seat has no compiler of its own: every check runs on the build PC through
`slot.sh` (slot 20), MSVC Debug with warnings as errors. Linux and macOS run later in CI.

## Items

| # | Rows | Chain head | What changed | How it was proved | State |
|---|---|---|---|---|---|
| 1 | AGT-05 (engine side), ADR-0261 d7, ADR-0262 d2 | 586d463a5, e260668df | **The AI mode's proposal cards.** `AppControl::reviewCards(project)`, an owner-thread seam, gives each waiting proposal as a card: the request, who proposed it (`ADI agent` or the client's own label), its origin (marker or direct), why it waits (`tier`, or the always-confirm rules it tripped, now kept by name in `QueuedChangeset::reviewReasons`: `marker`, `bulk`, `egress`, `remark`), every change before and after plus the diff for the floating window, and its **freshness** read from the history tree (`proposal_freshness.*`): `current`; `behind` by N undo steps (Undo N times makes it current); `ahead` by N redo steps; `elsewhere` (another branch, or past a compacted root). `AppControl::askAgain(launch, id)` rejects a moved card as the user's "ask again" (recorded in the agent's log with that source) and runs the same request, with its origin, on a fresh read; it refuses `current`, `not_waiting`, `not_adi_agent` and `project_unavailable` and changes nothing then. `ChangesetQueue::rejectAwaiting` moves only a card still waiting, so Ask again cannot undo an Apply that won a race. | `adi_app_control_tests` gained `testReviewCards`: a faux helper process reads the project and proposes a rename; the card is checked field by field; two user edits make it `behind 2`, Apply refuses the stale head and writes nothing, each Undo counts down, undoing back makes it current, one more Undo says `ahead 1`, Redo makes it current; Ask again replaces it with a card built on the new head; a current card answers `current`, an applied one `not_waiting`. `testFreshnessOnTheTree` checks branches, the redo line and a compacted root on a hand-built tree. Build PC, MSVC Debug, warnings as errors: 17 control, changeset and history suites pass at 586d463a5. | done (engine side); the window's half waits, see "Next" |
| 2 | AGT-12 (its first part), ADR-0259 d1 "who did what" | 4fa760d35 | **The undo step of an applied changeset is named from the request.** The History list and the Edit menu named an agent's step after its first op ("track.rename"). `agent::apply` now gives the first op the request on one line (`undoStepName`: line breaks and runs of spaces read as one space, cut on a character boundary at 64 bytes with an ellipsis); the other ops keep their own labels, and the step's actor (`agent`) and actor detail (client, transport, proposal id) say who made it. The agent's log already records requests, answers, proposals, their outcomes with actor and source, and Stop presses (control item 9). | `adi_changeset_tests`: the step's label, actor, actor detail and op count; the other ops' labels; a 200-byte request and a two-byte script cut on a character boundary. `testReviewCards` checks the Edit menu's step after Apply through the live document. Same build as item 1. | done |
| 3 | ADR-0265 d1, d2 (Stop Agent's reach) | (no change) | **Checked, nothing to build.** The director answered the control seat's A and B on 28 November with "A a, B a". What is built already matches: the one write method, `changeset.propose`, is closed by the agent write gate for every credential, ADI's agent and outside clients alike, while reads stay open; Stop cancels only Agent and Foundry jobs, and an ordinary render keeps running and keeps its own Cancel. | Existing tests: `adi_app_control_tests` (an outside client's proposal is refused while stopped; reads still answer), `adi_jobs_tests` `stopAgentJobs` (an ordinary job is not signalled and can still be enqueued). | done |

| 4 | AGT-08 (its tasks half), AGT-02 | 879444b72 | **An agent's real work through the four tools: a MIDI track, a clip, notes, automation, a rename.** Writing the test found two gaps an agent could not get past. (a) `project_read` gave an automation lane but none of its points, and `nextIds` had no id for its stream, which `automation.writePoints` and its siblings address (`id` is the `automation_data` row, beside `lane`): an agent could write points it could never read back, and could not name a new stream. Level 1 now gives each lane its `timeBase`, `domain`, `unit`, `min`, `max` and `data`: its streams `{id, clip, points[{id, time, at, value, curve, tension}], nextPoint}`, at most 4,000 points per read in all (`kMaxPointsPerRead`, `pointsTruncated` beyond; ids past the cap still count for `nextPoint`; a stream that does not decode says `unreadable`); `nextIds.automationData` is new. (b) `automation.writePoints`, `deletePoints`, `movePoints`, `setCurve` and `restoreData` read their optional `clip` with `at()`, so a changeset that followed the op's own schema and left it out failed; an absent `clip` now reads as null, the arrangement's stream (`clipOf` in `automation_edits.cpp`). `docs/CONTROL-MCP.md` §4.2 and §6.1 say so. | `adi_control_mcp_tests` `testAgentCorpus`: the payloads read through `ops_describe` (all six ops `agentMayEmit`; `clip` not required), ids from `nextIds`, one `changeset_propose` of eight ops (track, clip, three notes, a volume lane, two points with `clip` left out, a rename) waits for review with the file untouched; applied as the user does it, one agent undo step named from the request; read back: level 0 the track and `nextIds` past every new row, level 1 the renamed clip at `2|1|0` two bars long, the lane's unit and bounds, its stream and two points (`2|1|0` at -24 dB, `4|1|0` at 0 dB, `nextPoint` 3), level 2 the three notes by name, position and length; one Undo restores the text projection exactly. Check: see the note under this table. | done, check owed |
| 5 | AGT-08 (devices), AGT-02 | 636103837, cc2df548a | **An agent's work on devices.** `device.move` and `device.insert` take a stored `ord`, and the read gave only the list's order, so an agent could not name a place between two devices. Each device row at level 0 now carries `ord` (beside `chain`). | `adi_control_mcp_tests` `testAgentDeviceWork`: a chain of two plug-ins on Bass; one changeset sets Echo's mix, bypasses and renames Space, moves Echo to ord 2; read back in the new order with the new values (the untouched parameter kept); a second proposal removes Echo, and one Undo brings it back exactly, with mix 0.8, not the plug-in's default. `adi_control_projection_tests` `testLevel0`: each device's `ord` in its own chain. cc2df548a adds a lane on Space's parameter `2` (a device lane names a plug-in parameter by its canonical decimal id, ADR-0165, as `params` shows it), two points written with `clip: null`, read back in the track's scope. Check: see below. | done, check owed |
| 6 | AGT-02 | feb363468 | **The read's points, at their edges.** | `adi_control_projection_tests` `testAutomationPoints` on an in-memory model: `nextIds.automationData` 1 with no stream, then one past the highest; a stream of `kMaxPointsPerRead + 1` points cut at the cap with `pointsTruncated`, and `nextPoint` 70001 from the top id past the cap; a point's fields and `at`; three bytes that are not a stream say `unreadable` with no points; a clip's envelope gives its clip, both points, no `at`, `nextPoint` 3. | done, check owed |

| 7 | AGT-08, AGT-02 | 61c356867 | **The rest of the corpus.** (a) A clip envelope through the tools: one changeset makes a MIDI track, a clip, a volume lane, the lane's arrangement stream (`clip` left out) and the clip's envelope (`automation.writePoints` with `clip` set) on the same lane; a second moves the envelope's end point with `automation.movePoints`. (b) `device.remove` of a device that owns a lane: read in the code first (`detail::deleteDevice` collects the device's image, which takes `automation_lanes` with `owner_kind='device'` and their streams; its inverse restores them), so Live's way already holds: the automation goes with the device and comes back with its Undo. No source change. | `adi_control_mcp_tests` `testAgentClipEnvelope`: `movePoints` may be emitted; level 1 shows the lane's two streams, the arrangement's (`clip` null) and the clip's, each by its id; the envelope's points carry no `at` and `nextPoint` 3; after the move the end point is at the clip's second bar at -3 dB and the arrangement's point untouched; one Undo restores the text projection exactly and the read agrees. `testAgentDeviceWork` goes on: a third proposal removes Space, which owns the lane of item 5; its lane goes with it; one Undo brings the device, its lane, the stream by its id and both points back exactly. | written, check running |
| 8 | AGT-05 (the window's half, part 1), ADR-0261 d2 d7, ADR-0262 | fe021c0d4 | **The AI label and the cards in the Browser**, from `browser-2.png` states C and D. `BrowserPanel::setAgentMode(AgentMode)` (the root calls it, never a paint) adds the label **AI** as the last label of Categories (after Samples: this Browser lists no Templates yet; the sheet puts it after Templates) only while the agent can answer or a proposal waits, and takes it away otherwise (a chosen AI label then falls back to All, as a vanished Current Project does). The chosen label is lit in the accent with the sheet's four-pointed spark. Choosing it puts a pane in the list's place: the head line (who answers, how far it may go) in the column head, then one card per waiting proposal, oldest first: the request, one line under it ("14 changes"; a moved card adds how far the song moved: "The song moved on: 2 edits came after it; 2 Undos make it current.", or the Redo, other-branch and unreadable cases), and its buttons: **Apply** (lit), **Reject**, **Open list** on a current card; on a moved card no Apply: **Ask again** in its place for ADI's own agent, nothing in its place for another client. Each button calls a seam with the card's id (`onApplyCard`, `onRejectCard`, `onOpenCardList`, `onAskAgain`). | `adi_ui_shell_tests` `browserAgentMode`: no label at first; once the agent can answer, AI right after Samples, before the blank heading; choosing it hides the list and lights the label in the accent (a pixel); three cards (current, behind 2, ahead 1 from another client) in order inside the pane; the detail lines word for word; which buttons each card offers; clicks on Apply, Ask again, Reject and Open list each hand their seam their own card's id, a click on the request's line does nothing; Apply cannot be pressed on a moved card; an empty mode takes the label away and returns to All. | written, `ui` check owed |
| 9 | AGT-05 (the window's half, part 2) | 7dde3048a | **The window feeds it.** `AppControl::liveProject()` names the open project by the handle `reviewCards` and `agentReadiness` take. The app's timer (`syncAgentMode` in `ui_main.cpp`) builds a stamp of the project, the agent's readiness, the waiting proposals' ids and the history head, at most four times a second, and reads the cards only when the stamp changes; it maps `Freshness` to the card's `Moved`. Apply and Reject go to `AppControl::review` (a refusal is said in the Status Bar). Open list and Ask again only say in the Status Bar that they come later (see "Next"). | `adi_app_control_tests` `testReviewCards`: `liveProject()` is the handle the cards take, and after the agent's check `agentReadiness(liveProject())` can answer. The timer itself has no test: it is the app's. | written, `ui` check owed |
| 10 | (none) | (no change) | **The full `core` check of e260668df** (items 1 to 3) ran at last from 18:45: MSVC Debug build OK; ctest 287 of 291 pass. The four that fail touch nothing of this stream and read as the loaded build PC: `adi_agent_log_tests` (three checks of competing child-process writers), `adi_link_playback_tests` (three timing checks of Link's clock), `adi_ui_document_tests` (three render, Save As and slow-load checks, 356 s) and `adi_library_tests` (timeout at 120 s). They are rerun on the chain head below. | build PC slot 20, 28 November 19:08 | done |

**The checks.** Round 3 started `slot.sh run 20 origin/held/agent-chain core` with the quick filter
`adi_control_(mcp|projection)|adi_automation|adi_changeset|adi_app_control|adi_agent_log` on 7dde3048a
(items 4 to 9). Its result is written below when it ends.

## Next, in this order

1. **Prove items 4 to 9 on the build PC**, then fix what it finds: the quick core turn above, then
   `slot.sh run 20 origin/held/agent-chain ui` for items 8 and 9 (`adi_ui_shell_tests` and the app's build),
   then a full `core` on the head.
2. **The AI mode, what is left of round 2's plan** (its steps (a) and (c) are items 8 and 9): (b) choosing AI widens the
   column by itself (ADR-0262 d1, state D): a seam from `choose` to the root, which owns the column's
   width; (d) Open list: the floating window of ADR-0262 d2 behind a seam a test counts; Ask again: it runs
   the agent on a worker (`AppControl::askAgain` takes an `AgentLaunch`), which comes with the
   conversation and the floating search's sending (state F), so it waits for that; the conversation
   itself (the user's lines and the agent's answers, from `agentHistory`), which the sheet draws above
   each card; the head line's model name (the app says "Agent · asks first" for now, and does not read
   the tier: an Apply-tier agent would need its own words); the search field's "Ask..." in AI mode;
   cards past the pane's bottom are cut off (no scrolling yet).
3. **A tool the agent lacks: adding a device.** A built-in device or a plug-in enters a chain through
   `prepareBuiltinInsert` / `preparePluginInsert` (`src/adi/builtin_catalog.hpp`), which compose
   `pluginRef.register`, `chain.create`, `device.move`, `device.insert` (with every parameter) and
   sometimes `track.setArm`. A model cannot write that sequence by hand. Proposal: a read-only tool
   `device_insert_ops {uid | pluginId, track, before?}` that runs the same prepare function on the
   preview snapshot and returns its ops for the agent to put in its changeset, so the result is the same
   as a drop from the Browser and passes the same review. It needs a new method name: reserved first
   (see "For win"). The window's `retainedDeviceId` rule (ids held for Undo) must be honoured: ask the
   live document for it on the owner thread. **Blocked on the reservation:** `collab/RESERVATIONS.md` on
   main had no row for it on 28 November (round 2 looked). Take it as soon as win reserves it.

## For the director

(nothing blocks this stream)

- **A. A proposal whose song moved on (not drawn on the sheet).** `browser-2.png` draws only a current
  card. ADR-0261 d7 says a moved card tells how many edits came after and that undoing back makes it
  current. Built (item 8) in the card's own look: no Apply; one dim line ("3 changes. The song moved on:
  2 edits came after it; 2 Undos make it current."); **Ask again** where Apply was, for ADI's own agent
  only. Choices: a) keep it so; b) keep Apply visible but dimmed beside Ask again; c) another way: tell
  me. Recommendation: **a**, Live has no counterpart and an Apply that cannot apply only invites a click
  that fails. Reply: "A a".

## For win

- **The build PC, 28 November:** from about 16:00 to 18:45 no slot got a build place (slot 1 showed
  `UI CTEST` from 15:31). It moved again at 18:45. The full core run of e260668df then failed four suites
  this stream does not touch, each a timing or load check (item 10): worth a look if they fail on main too.
- **Reserve (asked in round 1, still open; "Next" item 2 waits for it):** the control method
  `device.insertOps` and its MCP tool `device_insert_ops` (read-only, Propose tier: it returns ops and
  writes nothing). Recommendation: reserve under AGT-05 / this stream.
- **Files of other seats touched in round 3:** `src/juce/ui_browser.*` (the AI label and pane, item 8),
  `src/juce/ui_main.cpp` (`syncAgentMode`, item 9), `tests/test_ui_shell.cpp` (`browserAgentMode`),
  `src/juce/app_control.*` (`liveProject()`). Each adds; nothing was removed or renamed.
- **Files of other seats touched in round 2:** `src/adi/automation_edits.cpp` (the optional `clip`),
  `src/adi/control/projection.*` and `docs/CONTROL-MCP.md` (the control seat's), and
  `tests/test_control_projection.cpp`. Each change is small and adds fields; nothing was removed.

## For the next session

Start by reading this file, then `collab/control.md` on `control/log`. Fetch, merge the base into
`held/agent-chain` (the base rule is in the head of this file). The local branch name does not matter:
rounds 2 and 3 worked on local `r2/agent-chain` and `r3/agent-chain` (and `r3/agent-log`) pushed as
`HEAD:held/agent-chain` (`HEAD:held/agent-log`), because other worktrees still had the plain names checked
out. Then take "Next" item 1: the checks of the chain head. Read item 8's seams before the window's next
step; the sheet is `collab/window/design/proposals/browser-2.png`, its text in the design README.
