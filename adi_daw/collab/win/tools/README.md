# win's landing tools

Scripts win uses to land the lanes' PRs on `arieladi/Adi`. They need `gh` signed in and a POSIX shell
(Git Bash on Windows). They find each other through their own folder, so run them from here or by full
path.

| Script | Use |
|---|---|
| `land_queue.sh N...` | Land PRs in order: refresh each (close and reopen, so its CI uses main's current workflow), wait for green through `land_top.sh`, merge. It never cancels a run on main (since 19 November): main's runs are the only ones with the heavy macOS legs. Stops with `CONFLICT N` when a PR conflicts with main. |
| `land_top.sh N` | Wait for every check of N to finish. A draft is marked ready first (a draft runs no CI). Reruns, once, a cancelled job and a job whose runner died (shutdown, exit 143, lost communication); any other failure stops with its URL. Merges through `merge_stack.sh` when adi_daw's `gate` check is SUCCESS, nothing is pending or failed, and Step 7 UI either never ran or shows all three `GUI shell / ...` checks SUCCESS or SKIPPED (or, for a Markdown-only change, its one skipped placeholder beside `ui: which jobs run` SUCCESS); a check not created yet is absent, not pending, so fewer than three waits. A stack top (base not main) whose head has no Step 7 UI run gets one: it dispatches `ui.yml` on the head branch, once per head commit, and lands only on that run's whole answer. A PR whose newest adi_daw run predates the gate falls back to the old rule, at least 20 OK checks and the same GUI shell rule, on two polls in a row. |
| `land_checks.py` | `land_top.sh`'s decision, from `gh pr view N --json statusCheckRollup,baseRefName,headRefOid` on stdin, followed by a stack top's dispatched Step 7 UI run (`gh run view ID --json ...`) when there is one: WAIT, FAILED, CANCELLED, GATE, LAND, LEGACY or UI (dispatch Step 7 UI). Only each workflow's newest run counts, and a check is its workflow and name together (`console-plugin` and `midi-helper` both have `build (<os>)`). `test_land_checks.py` runs it on sample rollups, without GitHub, and checks its workflow and job names against `ci.yml` and `ui.yml`. |
| `merge_stack.sh N...` | Ready, retarget to main, wait for mergeability, merge. A hand merge lands in the PR's **base**, so the retarget comes first. |
| `land_window_poll.sh` | Poll for `window/*` PRs (not `window/log`) and land each in number order, for `HOURS` hours. |
| `open_engine_prs2.py DIR` | Open a PR for every "PR requested" entry in `origin/engine/log`'s `engine.md` whose branch has none. The body is the entry, verbatim. Base: the requested one, or the topmost dependency that is an ancestor. |
| `chain_run.sh START` | Build the engine chain in `D:/wt-refresh`, one merge per link, tagging `chain/engine/<b>`. `CHAIN_ORDER`, `CHAIN_PREV` and `CHAIN_RESOLVE=1` override. It resets the worktree on start, so commit and tag a hand resolution before rerunning. |
| `chain_step.py` | One chain link: diff3 merge with the conflict rules in its docstring, schema-minor renumbering, frozen schema history. |
| `chain_push.sh` | Push the chain tags fast-forward only and retarget each PR to its predecessor (`CHAIN_FIRST_BASE`). |

Traps they encode:
- A PR's CI takes its workflow from the merge ref of head plus base, so retarget a stack's top to main before its final run.
- A collab-only PR gets no checks at all.
- A draft PR runs no checks until it is marked ready, and a PR into main that changes only Markdown no build reads runs the validators and the gate, nothing else (`.github/scripts/markdown_only.sh`, tested by `test_markdown_only.sh` beside it).
- A stack's top is retargeted and merged by `merge_stack.sh` with no new run, and the runs of the PRs below it are cancelled, so the top's own runs are the only build of everything below it. adi_daw covers that: a stacked PR (base not main) runs every leg, Markdown-only or not. Step 7 UI does not on its own: its `paths` filter sees only the top's diff against the PR below, so a top that changes only `adi_daw/collab/**`, a workflow or a `.github` script starts no Step 7 UI run. `land_top.sh` then dispatches `ui.yml` on the top's branch and waits for it. A dispatched run is not in the PR's `statusCheckRollup` (#507 carried one and its rollup did not list it), so `land_top.sh` reads it with `gh run list` and `gh run view`.
- Verify a run by its head SHA.
- A commit pushed while GitHub still shows an older head can miss the merge (#507, then #510).
