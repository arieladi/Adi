# The basics, 27 to 29 November 2026: how every session works

Read this once, then your own queue (`collab/prompts/2026-11-27-basics-<name>.md`).

**What this is.** On 26 November the director found that ADI drew no waveform while audio recorded, and
that the plan had no row for it. A sanity check of the whole program followed: `docs/SANITY-CHECK.md`,
rows SC-01 onward. His rule: the basics come first. Your queue is a part of that list. Each item is a
fix, not a new feature, so it is worked now.

## Setting up

- The repository is public: github.com/arieladi/Adi. The project is in `adi_daw/`.
- Work in a worktree of origin/main at a short path, with the build folder at a short path too: JUCE
  builds fail on long paths.
- Build as `.github/workflows/ui.yml` does: Ninja, `-DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON
  -DADI_ACCEPTANCE=ON`. The app is the target `adi_app`.
- Install what helps you. Build, test and probe freely.

## Each item

1. Read its row in `docs/SANITY-CHECK.md` and the code your queue points at. The pointers come from an
   audit that read the code and did not run it: confirm the fault first.
2. Write a test that fails without the fix, headless wherever one can be written.
3. Fix it the way Ableton Live 12 behaves. Where your queue says "reading", the answer about Live is
   still owed: build the rest and leave that one choice as a named constant.
4. Prove it:
   - the suites it touches, with warnings as errors;
   - `adi_ui_shell_tests` in a **Debug** build if you touched the window. MSVC's Debug containers
     allocate for every string and vector made, and a frame of the window must allocate nothing. An
     optimised build does not show this, and it failed CI on 26 November;
   - **on Linux, the window suites with no display, as CI runs them:** `env -u DISPLAY ctest -R '^adi_ui_'`
     (`.github/workflows/ui.yml` unsets DISPLAY on purpose; an earlier line of these queues said "a virtual
     display", which was wrong). A test that opens a real menu or window passes under a virtual display
     and crashes there: it happened on 27 November. Do not show a menu in a test: use the view's seam
     (`ArrangementView::onContextMenu` for the lanes and names, `onMenu` on a header's Solo) or add one
     like them, and count the request;
   - the session suite: `ctest -L acceptance`;
   - if your session can drive the real window, do the gesture there, with `ADI_HOME` set to a folder
     of its own.
5. **One chain branch per queue: `basics/<your queue>-chain`.** Every item of yours is a commit, or a
   few, on that one branch, with its SC id in the message. Push after each item. **Open no pull
   request:** the one macOS runner cannot carry one per fix. Win merges the chains into a landing stack.
   - Start the chain from the newest landing stack (`origin/land/basics-*`), or from origin/main once
     that stack has merged. Before each push, merge the newest of them into your chain.
   - This replaced "one branch per item" on 27 November: of 34 separate branches, 28 collided with each
     other, most of them with their own session's earlier ones, and could not be stacked.
   - If you already pushed separate item branches: make the chain, merge each of them into it, oldest
     first, resolve the collisions yourself (you wrote both sides), run your tests and push. Your report
     then names the chain's head for each item.
6. Add a line to your report and start the next item in the same turn.

## Your report

`adi_daw/collab/basics/<your queue's name>.md` on the branch `basics/<your queue's name>-log`, pushed
after every item:

| SC | Branch and head | What changed | How it was proved | State |
|---|---|---|---|---|

State is "PR requested", or "blocked" with one line saying on what. A question that only the director can
answer goes under "For the director" in the same file, with your recommendation; then you continue.

## Working beside five other sessions

- Several sessions edit `src/juce/ui_arrangement.cpp`, `ui_shell.cpp`, `ui_mixer.cpp` and
  `tests/test_ui_shell.cpp` at the same time. Keep each change small.
- Put a new test beside the tests of the same feature, and its call beside theirs in `main`. Never
  append at the end of the file: two branches that add at one place cannot be merged by git.
- Merge origin/main into your branch before you push. Win resolves what still collides.
- By the director's order for these days you may edit any file your item needs. Say in your report
  which seat's file you touched; the owners are in `collab/README.md`.

## House rules

- Public repository: nothing committed carries a machine name, a user name, a private path, an address
  or a screenshot of a desktop.
- Stage explicit paths, never `git add -A`. Never commit to main, never force-push, never rebase a
  pushed branch.
- No other maker's artwork, presets or product names in shipped code.
- Commits end with the attribution lines your session gives you.

## When your queue is empty

Your queue file names your second queue. After both, take the wave 2 rows of your queues in `docs/SANITY-CHECK.md`
(round 2 added rows SC-120 onward, each with its queue), in their order. Stop only at your usage
limit or a broken environment, and say which in your report.

**What a user will see.** The look of each surface is agreed with the director from a picture before it is
built (ADR-0243 d1). A fix builds what a control does, in today's look. Where a row makes you add something
to the window that Live does not show, put it under "For the director" with its measured place before it
lands.

**One session per queue.** The director starts more sessions as he can, and a queue named as someone's
second may by then have a session of its own.
- When you start any queue, push its log branch (`basics/<queue>-log`) first, before the first item.
- Before you start a second queue, fetch and look for that queue's log branch. If it exists, another
  session has the queue: leave it, and take the wave 2 rows of your own first queue instead.
- A log branch whose report holds no row a day after it was pushed does not hold a queue. Say so in your own
  report; win names the owner.
- A queue with no file of its own (device, export) is every row of `docs/SANITY-CHECK.md` whose Queue
  cell names it: wave 1 first, then 2, then 3.
