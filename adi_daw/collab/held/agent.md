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

## For the director

(nothing yet)

## For win

(nothing yet)

## For the next session

Start by reading this file, then `collab/control.md` on `control/log`. Fetch, merge the base into
`held/agent-chain`, and take the next row of the table above whose state is not "done".
