# The agent seat — queue, 27 November 2026

**What this is.** The director wants to meet ADI's agent in the Browser: an "AI" label that turns the
Browser into the place to ask and to read the answer, and a button for the agent in the floating search
(ADR-0261). Nothing of the agent is in the window today, and the label may appear only when a chain of
hidden parts exists under it. You build that chain. His answer of 27 November: it starts now, on a seat of
its own, and lands after the basics.

**Read first.** ADR-0261 (d2 to d8, d21), ADR-0259 d1, ADR-0257 d6 to d9, ADR-0233, ADR-0239, ADR-0168,
ADR-0148 in `docs/DECISIONS.md`; `docs/CONTROL-MCP.md` and `docs/AI-AGENT.md` whole; the plan's rows and
appendix cards for AGT-01 to AGT-09 and CTL-02 to CTL-09 in `docs/PLAN.md`; DF-C1 to DF-C9 in
`docs/DEFERRED.md`. What exists: the control core, the changeset service and the headless `adi-mcp`
(`src/adi/control/`, `src/adi/changeset.*`, `tools/adi-mcp/`), landed in #512.

**How you work.**
- One chain branch, `control/agent-chain`, from origin/main. Your report is `collab/control.md` on the
  branch `control/log`, one row per item: what changed, how it was proved, its state. Push after each item.
- Build and test as the repository does: `cmake` with warnings as errors, the suites with `ctest`, the
  five validators in `tools/`. A new test for each item, which fails before your change.
- The audio thread takes no lock and allocates nothing; no socket is ever on it. No model runs inside
  ADI's process.
- A new op name, a schema minor or a register id is reserved in `collab/RESERVATIONS.md` first: say what
  you need in your report under "For win" and go on with the next item.
- A choice that is the director's goes under "For the director" in your report, in plain words, lettered,
  with your recommendation. Then you continue with what does not depend on it.
- **Not yours:** anything the user sees. The AI label, the conversation's pane, the review of a change
  and the floating search wait for a picture the director has not agreed (DF-W61). Do not touch
  `src/juce/ui_browser.*`, `ui_shell.*` or `ui_palette.*`. Where an item needs a question put to the
  user, build the engine side and a seam the window will call, with a test that drives the seam.
- Work until your limit. Stop only for a broken environment, and say which.

## Queue, in this order

1. **A marker's prompt always waits for review (ADR-0261 d8).** `marker.setPrompt` joins the ops the
   agent may not emit (`agentMayEmit` in `src/adi/changeset.cpp`), and a run that starts from a marker's
   prompt is queued for review at every tier. Small, and first because it closes a hole.
2. **The tier's scope and the always-confirm rules at the boundary (AGT-09),** with the per-minute cap
   that nothing reads today (DF-C6).
3. **The project's identity written by the Store (the rest of AGT-02).** A session's copy of a project
   gets an identity of its own.
4. **The consent ledger and its question (AGT-03, DF-C2).** One ledger for the program, kept on this
   computer and never in the `.adi`. A capability for plug-in hosting on a server belongs in it
   (ADR-0261 d21): the grant is per project and per server, and a copy, a received project or a paste
   never carries one.
5. **The session token (CTL-05, DF-C1), the loopback listener (CTL-04), the app host (CTL-09),** then
   `adi-mcp --connect` (DF-C3). The app host is what makes a write from outside show in the window
   (SC-166): until it exists, a headless apply to a project that ADI has open is refused.
6. **Jobs a client can watch and cancel (AGT-04).**
7. **Stop Agent (ADR-0259 d1):** every write an agent has in flight is rejected, its jobs are cancelled,
   and the agent is read-only until re-armed. Whose writes it covers, and what it means for a run that
   has no agent in it, go under "For the director" if the decisions do not say.
8. **The helper process (AGT-06),** started on demand, with its budgets and its providers behind
   consent. ADR-0261 d2: "ADI can answer" means the helper is set up, consent is given and it answered a
   check at this launch. Build that check.
9. **The agent's log (ADR-0261 d7):** one per project, in ADI's data folder, in time order: requests,
   answers, proposals and what became of them, view changes, Stop presses.
10. **The catalogue for agents (ADR-0261 d7, DF-C7):** devices, plug-ins and presets, read from the same
    lists the Browser reads.
11. **The injection fixtures (AGT-08):** projects whose text tries to make the agent act, each proving
    that nothing commits.

## After these

The agent's view operations (ADR-0257 d6) as one tool under the tier, applied at once and never saved by
the agent. Then say in your report what the window needs from you for AGT-05.
