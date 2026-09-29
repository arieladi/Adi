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

Kinds: `explain`, `tests`, `docstrings`, `patch`, `review`, `freeform`. Commands: `submit`, `mission`, `collect`, `verify`, `check`, `retry`, `eta`, `status`, `show`, `watch`, `run-once`.
`docstrings` and `patch` use SEARCH/REPLACE edit blocks. The drone applies them
in memory, builds the diff with `difflib` and runs `git apply --check`.

## Trust: names yes, numbers no (measured on 2026-09-27)

Measured over nine missions, 22,000 claims:

- **Names are reliable.** 97–100% of the names it puts in backticks exist in the
  file it summarised.
- **Numbers are not.** Headers got invented parameter ranges. Even in the
  `.cpp` that declares them, 15 of 25 Mixxx defaults were right, and the
  usual mistake was a maximum reported as the default.

Three rules follow, built into the tool:

1. **Read-only jobs quote instead of stating numbers.**
   - `explain`, `review` and `freeform` jobs carry a quote rule in their
     `system` text, so a drone still running older code applies it too. The
     model must never write a number in its own words; it copies the line that
     states it, on a line starting `QUOTE:`.
   - `--no-quote-rule` turns it off.
2. **`collect` grounds every report.**
   - A `QUOTE:` line found verbatim in the source (whitespace aside) is kept;
     any other is replaced by a note that it was removed.
   - The report is stamped **"names reliable, numbers unverified"**, with the
     counts.
   - `verify <mission>` measures any collected report without changing it.
3. **Prefer jobs a machine can verify, and let the build decide.**
   - `--patch` makes a job answer as edit blocks.
   - `check <job-id | mission>` applies each patch in a worktree at
     `D:\adi-drone\verify-tree` (`adi_daw/third_party` linked in, its own
     incremental build), builds with `tools\build.bat werror` and runs CTest.
     The verdict is written beside the patch as `check-N.json` and
     `check-N.log`, and the user's checkout is never touched.
   - **A tests patch must only add:** one that removes a `check(` or adds none
     is rejected before any build. The first pilot showed why: a patch that
     deleted 33 existing checks could still build and pass.

```bash
python tools/adi-drone/drone.py verify ref-dsp-backlog
python tools/adi-drone/drone.py submit --kind tests --patch --files adi_daw/src/x.hpp adi_daw/tests/test_x.cpp --instructions "Add two tests ..."
python tools/adi-drone/drone.py check <job-id>
```

A model that copies the format's example path instead of the real one is caught
too: when the SEARCH text occurs exactly once in exactly one of the job's files,
that file is the target, and the result says so.

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

## Concurrent requests

`watch --parallel N` and `run-once --parallel N` run up to N job workers;
the default is 1 and retains serial ordering and result bytes. For the Ubuntu
Ollama endpoint, use the existing `OLLAMA_HOST_URL` and `ADI_DRONE_HOME`
environment variables and choose 2 or 4 workers to match its configured capacity.
This changes no endpoint or state-folder defaults and installs no scheduled task.

Each job moves atomically from `queue/` to `running/` before its request starts.
A short claim lock also serializes Windows rename calls: two calls can otherwise
open the same source before either move completes. The existing process lock
still excludes a second drone using the same state directory. Requests and
per-job result writing run concurrently; log lines retain their existing format.
The model unloads only after all workers have finished and the queue is empty.
Interrupted jobs remain in `running/`; the existing startup recovery requeues
them and the existing partial-output replacement handles the retry.

Timing records, `eta` and `collect` are unchanged. ETA remains its existing sum
of measured call times; it is not divided by N or presented as a parallel-speedup
prediction. Completion/log ordering naturally follows the workers at N > 1.

Run `python tools/adi-drone/test_parallel.py -v` on Windows. The tests use only
temporary state folders and stub all model calls. The serial regression compares
all result/log bytes against the implementation at `92eb485`; that commit must
be present in the local Git history. No live drone directory is used.
