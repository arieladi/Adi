# Basics Browser

Queue: `collab/prompts/2026-11-27-basics-browser.md`. Taken by a Linux cloud session on 26 November; the
core and the window's headless suites are built and run on Linux, the window suites under a virtual
display. Windows is checked by Win before a branch lands.

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

## For the director

The desktop Codex session cloned the repository and read the queue. It stopped before SC-121 because the checkout was on Windows, WSL was unavailable, and the configured Linux host did not respond. No item has been verified or pushed by that session.

If the desktop request was meant as a handoff, which session should own the queue? Recommendation: keep it with the Linux session that claimed this report so it can run the required JUCE and acceptance suites.
