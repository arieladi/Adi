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
| 5 | AGT-08 (devices), AGT-02 | 636103837 | **An agent's work on devices.** `device.move` and `device.insert` take a stored `ord`, and the read gave only the list's order, so an agent could not name a place between two devices. Each device row at level 0 now carries `ord` (beside `chain`). | `adi_control_mcp_tests` `testAgentDeviceWork`: a chain of two plug-ins on Bass; one changeset sets Echo's mix, bypasses and renames Space, moves Echo to ord 2; read back in the new order with the new values (the untouched parameter kept); a second proposal removes Echo, and one Undo brings it back exactly, with mix 0.8, not the plug-in's default. `adi_control_projection_tests` `testLevel0`: each device's `ord` in its own chain. Check: see below. | done, check owed |

**The checks.** The full `core` check of e260668df started at the end of round 1 sat "waiting for a build place"
for over an hour on 28 November (about twenty sessions share the build PC), and slot 20 refuses a new run
until it ends. Round 2 left that run in place: the next session runs `slot.sh wait 20`, then
`slot.sh run 20 origin/held/agent-chain core` on the chain head, which carries items 1 to 5. Until that
passes, items 4 and 5 are written and read but not built: MSVC has not seen them.

## Next, in this order

1. **Prove items 4 and 5 on the build PC** (above), then fix what it finds. Quick turn:
   `slot.sh run 20 origin/held/agent-chain core 'adi_control_(mcp|projection)|adi_automation|adi_changeset'`.
2. **A tool the agent lacks: adding a device.** A built-in device or a plug-in enters a chain through
   `prepareBuiltinInsert` / `preparePluginInsert` (`src/adi/builtin_catalog.hpp`), which compose
   `pluginRef.register`, `chain.create`, `device.move`, `device.insert` (with every parameter) and
   sometimes `track.setArm`. A model cannot write that sequence by hand. Proposal: a read-only tool
   `device_insert_ops {uid | pluginId, track, before?}` that runs the same prepare function on the
   preview snapshot and returns its ops for the agent to put in its changeset, so the result is the same
   as a drop from the Browser and passes the same review. It needs a new method name: reserved first
   (see "For win"). The window's `retainedDeviceId` rule (ids held for Undo) must be honoured: ask the
   live document for it on the owner thread. **Blocked on the reservation:** `collab/RESERVATIONS.md` on
   main had no row for it on 28 November (round 2 looked). Take it as soon as win reserves it.
3. **What the corpus still lacks, found while writing items 4 and 5** (no new names needed):
   - a test of `pointsTruncated`: write one stream of `kMaxPointsPerRead + 1` points with
     `Store::putAutomationData` and check the cap, the flag and that `nextPoint` is still past the top id;
   - a device parameter's lane: `automation.createLane` with `owner_kind` `device` refuses a `param_ref`
     that is not a canonical decimal parameter id, while `plugin_params.param_id` as `project_read`
     shows it can be any text (the projection fixture uses `p1`). Find how the window makes a device
     lane (`ui/automation_mode.*`) and give the agent the same id in the read, then a corpus test;
   - clip envelopes (a stream with `clip` set): the read lists them, no test writes one through the tools.
4. **The window's half of item 1, from the agreed sheet only** (`proposals/browser-2.png`, states C and
   D; ADR-0262 d1, d2): the card (request, "14 changes · late by 12 to 31 ms" line, Apply, Reject,
   Open the list), the floating window with the same two buttons, the freshness notice with "Ask again",
   the head of the pane ("Agent · asks first · local model"). It shows only when ADR-0261 d2's gate
   holds (`agentReadiness(...).canAnswer()`, or a proposal waits). The window files are shared; keep each
   change small and use the view's seams, never a real menu or window in a test.

## For the director

(nothing that blocks this stream)

## For win

- **Reserve (asked in round 1, still open; "Next" item 2 waits for it):** the control method
  `device.insertOps` and its MCP tool `device_insert_ops` (read-only, Propose tier: it returns ops and
  writes nothing). Recommendation: reserve under AGT-05 / this stream.
- **Files of other seats touched in round 2:** `src/adi/automation_edits.cpp` (the optional `clip`),
  `src/adi/control/projection.*` and `docs/CONTROL-MCP.md` (the control seat's), and
  `tests/test_control_projection.cpp`. Each change is small and adds fields; nothing was removed.

## For the next session

Start by reading this file, then `collab/control.md` on `control/log`. Fetch, merge the base into
`held/agent-chain` (the base rule is in the head of this file). The local branch name does not matter:
round 2 worked on a local `r2/agent-chain` pushed as `HEAD:held/agent-chain`, because an earlier worktree
still had `held/agent-chain` checked out. Then take "Next" item 1: the check of the chain head.
