# adi-drone

Offline batch worker. Queued jobs go to a local `qwen2.5-coder:7b` (Ollama, GTX 1070,
16K context, ~32 tok/s, 100% GPU). Standard library only.

**The drone never writes to a repository.** It reads the files a job names and
writes its output to `D:\adi-drone\done\<job-id>\` (`result.md`, `patch-N.diff`,
`meta.json`). A human or Claude reviews it and applies what is worth keeping.

## Use

```bash
python tools/adi-drone/drone.py submit --kind explain --files adi_daw/plugins/rmsc/Source/PluginProcessor.cpp --instructions "Explain processBlock"
python tools/adi-drone/drone.py status
python tools/adi-drone/drone.py show <job-id>
git apply D:/adi-drone/done/<job-id>/patch-0.diff   # only after reading it
```

### Missions

A mission is long-running background work: one job per matched file, at priority
8 by default, so interactive jobs (3–5) still jump the queue. Since one job
handles one file, a failed file does not stop the mission.

```bash
python tools/adi-drone/drone.py mission --name adi-daw-file-map --glob "adi_daw/src/**/*.cpp" --glob "adi_daw/src/**/*.h" --exclude "/build" --instructions "Summarise this file: Purpose, Key types, Threading, Depends on, Open ends"
python tools/adi-drone/drone.py collect adi-daw-file-map   # -> D:\adi-drone\missions\adi-daw-file-map.md
```

`--chunk` splits any file over the prompt budget into line-range jobs of about
7.5K tokens each, cutting after blank lines or closing braces where possible.
`--files` takes explicit paths. Chunk jobs are read-only. `collect` groups a
file's chunks in line order and drops a failure once a later job has covered
that file, so a rerun lands in the same report.

### Sizing a run

`mission ... --dry-run` prints the job count and a measured estimate without
queueing anything. `eta` gives the same for what is already queued. The estimate
has two figures:

- **expected**: the mean of this mission's own finished jobs, else its kind's,
  else all jobs' (at least 10 measured calls each). Backtested on the first
  overnight run: 2h40m expected against 2h59m actual.
- **at most**: every job writes up to its output cap at the measured speed. A
  mission with no history yet can land anywhere between the two, because
  list-style prompts write about 4x as much as summaries.

Measured on the 1070: summaries 15–18 s per job, test ideas ~51 s.

### Output cap and retries

Answers are capped at 2048 tokens (`--max-output` up to 4096, for list-style
jobs such as test ideas). Capped answers are marked incomplete, and `collect`
counts them. `retry <mission> --truncated --max-output 4096` requeues just
those; `--failed` requeues failures. A retry's result replaces the old one in
the mission report.

Policy on the Windows PC: keep a mission queued at all times so the GPU never
idles. Delegate work that fits the table below, and read everything before using it.

Options: `--priority 1..9` (1 runs first), `--per-file` (one model call per file,
for batches over the ~11K-token prompt budget), `--system` (extra rules),
`--repo` (defaults to this monorepo).

Kinds: `explain`, `tests`, `docstrings`, `patch`, `review`, `freeform`.
`docstrings` and `patch` use SEARCH/REPLACE edit blocks. The drone applies them
in memory, builds the diff with `difflib` and runs `git apply --check`.

## What to delegate (measured on 2026-09-26)

| Works | Does not work |
|---|---|
| Explaining or summarising a file | Open-ended bug review: every finding was a false positive |
| Narrow mechanical edits with an exact spec | Anything needing cross-file or real-time-safety judgement |
| Test and boilerplate scaffolds to be finished by hand | Unreviewed patches: one that applied cleanly also deleted an inline `{}` body |

"Applies cleanly" does not mean correct. Read every diff.

## Runtime

- Scheduled task `adi-drone` runs `pythonw drone.py watch` at logon and restarts on failure.
- The drone starts `ollama serve` itself if the server is down.
- VRAM: the model stays loaded between back-to-back jobs (`keep_alive` 10m). The
  moment the queue empties, the drone sends `keep_alive: 0` and the 1070 is free for
  the desktop. The next job reloads it in a few seconds.
- Models are stored in `D:\ollama\models` (user env var `OLLAMA_MODELS`).
- State: `D:\adi-drone\{queue,running,done,failed}`, log in `D:\adi-drone\drone.log`.
- A job left in `running/` after a crash is requeued at the next start.
- Stop: `Stop-ScheduledTask adi-drone`. Remove: `Unregister-ScheduledTask adi-drone`.
