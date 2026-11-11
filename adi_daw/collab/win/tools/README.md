# win's landing tools

Scripts win uses to land the lanes' PRs on `arieladi/Adi`. They need `gh` signed in and a POSIX shell
(Git Bash on ORC-VST). They find each other through their own folder, so run them from here or by full
path.

| Script | Use |
|---|---|
| `land_queue.sh N...` | Land PRs in order: refresh each (close and reopen, so its CI uses main's current workflow), wait for green through `land_top.sh`, merge, then cancel main runs of **older** heads only. Stops with `CONFLICT N` when a PR conflicts with main. |
| `land_top.sh N` | Wait for every check of N to finish. Reruns, once, a job whose runner died (shutdown, exit 143, lost communication); any other failure stops with its URL. Merges through `merge_stack.sh` when green with at least 20 OK checks. A workflow-only PR runs only 18, so merge it by hand. |
| `merge_stack.sh N...` | Ready, retarget to main, wait for mergeability, merge. A hand merge lands in the PR's **base**, so the retarget comes first. |
| `land_window_poll.sh` | Poll for `window/*` PRs (not `window/log`) and land each in number order, for `HOURS` hours. |
| `open_engine_prs2.py DIR` | Open a PR for every "PR requested" entry in `origin/engine/log`'s `engine.md` whose branch has none. The body is the entry, verbatim. Base: the requested one, or the topmost dependency that is an ancestor. |
| `chain_run.sh START` | Build the engine chain in `D:/wt-refresh`, one merge per link, tagging `chain/engine/<b>`. `CHAIN_ORDER`, `CHAIN_PREV` and `CHAIN_RESOLVE=1` override. It resets the worktree on start, so commit and tag a hand resolution before rerunning. |
| `chain_step.py` | One chain link: diff3 merge with the conflict rules in its docstring, schema-minor renumbering, frozen schema history. |
| `chain_push.sh` | Push the chain tags fast-forward only and retarget each PR to its predecessor (`CHAIN_FIRST_BASE`). |

Traps they encode:
- A PR's CI takes its workflow from the merge ref of head plus base, so retarget a stack's top to main before its final run.
- A collab-only PR gets no checks at all.
- Verify a run by its head SHA.
- A commit pushed while GitHub still shows an older head can miss the merge (#507, then #510).
