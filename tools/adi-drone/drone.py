"""adi-drone: an offline batch worker that feeds queued jobs to a local Ollama model.

The drone never writes to a repository. It reads the files a job names, asks the
model, and writes everything it produced under STATE_DIR/done/<job-id>/ for a
human (or Claude) to review and apply. Patches are checked with
`git apply --check`, which is read-only.

Usage:
  drone.py submit --kind review --files adi_daw/src/foo.cpp --instructions "..."
  drone.py mission --name rmsc-docs --glob "adi_daw/plugins/rmsc/**/*.cpp" --instructions "..."
  drone.py collect rmsc-docs
  drone.py status
  drone.py show <job-id>
  drone.py watch            # run forever (what the logon task starts)
  drone.py run-once         # drain the queue, then exit
"""

import argparse
import datetime as dt
import difflib
import json
import msvcrt
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

STATE_DIR = Path(os.environ.get("ADI_DRONE_HOME", r"D:\adi-drone"))
DEFAULT_REPO = Path(r"C:\Users\Adi\Documents\GitHub\Adi")
OLLAMA_URL = os.environ.get("OLLAMA_HOST_URL", "http://127.0.0.1:11434")
OLLAMA_EXE = Path(os.environ["LOCALAPPDATA"]) / "Programs" / "Ollama" / "ollama.exe"
OLLAMA_MODELS = r"D:\ollama\models"
MODEL = os.environ.get("ADI_DRONE_MODEL", "qwen2.5-coder:7b")

NUM_CTX = 16384          # fits 100% on the GTX 1070 (5.5 GB)
MAX_PROMPT_TOKENS = 11000  # leaves ~5K for the answer
CHARS_PER_TOKEN = 3.3    # rough, code-heavy estimate
POLL_SECONDS = 10
# Keeps the model loaded between back-to-back jobs; the drone unloads it
# explicitly the moment the queue empties. This only matters if the drone dies mid-queue.
KEEP_ALIVE = "10m"
REQUEST_TIMEOUT = 1800

QUEUE, RUNNING, DONE, FAILED = (STATE_DIR / d for d in ("queue", "running", "done", "failed"))

KINDS = {
    "review": (
        "You are a careful senior C++/Python reviewer. Report only real defects: "
        "correctness bugs, undefined behaviour, real-time-safety violations in audio "
        "code (allocation, locks, I/O on the audio thread), resource leaks. For each: "
        "file, line, what goes wrong, and a concrete fix. If you find nothing, say so. "
        "Do not pad with style advice."
    ),
    "docstrings": (
        "Write missing doc comments for the public functions and classes shown. Match "
        "the existing comment style of the file."
    ),
    "tests": (
        "Write unit tests for the code shown, using the test framework already used "
        "in the repository if visible. Cover edge cases. Output complete test code."
    ),
    "explain": (
        "Explain what this code does, its data flow and its invariants, for a "
        "developer new to the codebase. Be precise and brief."
    ),
    "patch": "Make exactly the change requested and nothing else.",
    "freeform": "You are a precise, terse software engineering assistant.",
}
EDIT_KINDS = {"docstrings", "patch"}

# Small models cannot count diff hunk headers, so edit jobs ask for SEARCH/REPLACE
# blocks; the drone applies them in memory and builds the diff itself.
EDIT_FORMAT = """

Express every change as one or more edit blocks, exactly in this format:

path/relative/to/repo.ext
<<<<<<< SEARCH
lines copied verbatim from the file, enough to be unique
=======
the replacement lines
>>>>>>> REPLACE

Rules: the SEARCH text must match the file character for character, including
indentation. Keep each block small. Output only edit blocks, no other text."""

EDIT_BLOCK = re.compile(
    r"^(?P<path>[^\n<>=`]+?)\s*\n(?:```[^\n]*\n)?<<<<<<< SEARCH\n(?P<search>.*?)\n?=======\n"
    r"(?P<replace>.*?)\n?>>>>>>> REPLACE", re.S | re.M)


def now():
    return dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def log(msg):
    line = f"{now()} {msg}"
    print(line, flush=True)
    with open(STATE_DIR / "drone.log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def ensure_dirs():
    for d in (QUEUE, RUNNING, DONE, FAILED):
        d.mkdir(parents=True, exist_ok=True)


# ---------------------------------------------------------------- ollama

def ollama_up():
    try:
        with urllib.request.urlopen(f"{OLLAMA_URL}/api/version", timeout=3):
            return True
    except (urllib.error.URLError, OSError):
        return False


def ensure_ollama():
    if ollama_up():
        return
    log("ollama not reachable, starting 'ollama serve'")
    env = dict(os.environ, OLLAMA_MODELS=OLLAMA_MODELS)
    subprocess.Popen(
        [str(OLLAMA_EXE), "serve"], env=env,
        stdout=subprocess.DEVNULL, stderr=open(STATE_DIR / "ollama-serve.log", "a"),
        creationflags=subprocess.CREATE_NO_WINDOW | subprocess.DETACHED_PROCESS,
    )
    for _ in range(60):
        time.sleep(2)
        if ollama_up():
            return
    raise RuntimeError("ollama serve did not come up within 120 s")


def generate(system, prompt):
    body = json.dumps({
        "model": MODEL, "system": system, "prompt": prompt, "stream": False,
        "options": {"num_ctx": NUM_CTX, "temperature": 0.2},
        "keep_alive": KEEP_ALIVE,
    }).encode()
    req = urllib.request.Request(f"{OLLAMA_URL}/api/generate", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as r:
        return json.loads(r.read())


def model_loaded():
    with urllib.request.urlopen(f"{OLLAMA_URL}/api/ps", timeout=5) as r:
        return any(m.get("name") == MODEL or m.get("model") == MODEL
                   for m in json.loads(r.read()).get("models", []))


def unload_model():
    """Free the GPU once the queue is empty.

    Checks /api/ps first: an empty-prompt generate on a model that is not
    loaded would load it from disk just to unload it again.
    """
    try:
        if not ollama_up() or not model_loaded():
            return
        body = json.dumps({"model": MODEL, "prompt": "", "keep_alive": 0}).encode()
        req = urllib.request.Request(f"{OLLAMA_URL}/api/generate", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=60):
            pass
        log(f"queue empty, unloaded {MODEL} from VRAM")
    except (urllib.error.URLError, OSError, ValueError) as e:
        log(f"unload failed: {e}")


# ---------------------------------------------------------------- jobs

def est_tokens(text):
    return int(len(text) / CHARS_PER_TOKEN)


def git(repo, *args):
    p = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def build_prompt(job, repo, files):
    parts = [f"Task: {job['instructions']}", ""]
    rng = job.get("lines")  # [first, last], 1-based inclusive: a chunk of one big file
    if rng:
        parts += [f"This is part {job['chunk'][0]} of {job['chunk'][1]} of the file. "
                  "Describe only what this excerpt shows; do not guess about the rest.", ""]
    for rel in files:
        path = (repo / rel).resolve()
        if repo.resolve() not in path.parents:
            raise ValueError(f"file outside repo: {rel}")
        text = path.read_text(encoding="utf-8", errors="replace")
        label = rel
        if rng:
            all_lines = text.splitlines(True)
            text = "".join(all_lines[rng[0] - 1:rng[1]])
            label = f"{rel} (lines {rng[0]}-{rng[1]} of {len(all_lines)})"
        parts += [f"=== FILE: {label} ===", text, f"=== END FILE: {rel} ===", ""]
    return "\n".join(parts)


CHUNK_CHARS = 24000  # ~7.5K tokens: fits the budget with room for the instructions


def chunk_ranges(text, max_chars=CHUNK_CHARS):
    """Split into 1-based line ranges under max_chars, preferring to cut after a
    top-level closing brace or a blank line so functions stay whole."""
    lines = text.splitlines(True)
    ranges, start, size, cut = [], 0, 0, None
    for i, line in enumerate(lines):
        if size + len(line) > max_chars and i > start:
            end = cut if cut is not None and cut > start else i
            ranges.append((start + 1, end))
            start, cut = end, None
            size = sum(len(x) for x in lines[start:i])
        size += len(line)
        if line.startswith("}") or not line.strip():
            cut = i + 1
    ranges.append((start + 1, len(lines)))
    return ranges


def apply_edit_blocks(text, repo, allowed):
    """Apply SEARCH/REPLACE blocks in memory; return (unified diff, problems)."""
    originals, edited, problems = {}, {}, []
    for m in EDIT_BLOCK.finditer(text):
        rel = m.group("path").strip().strip("`*").replace("\\", "/")
        if rel not in allowed:
            problems.append(f"edit targets a file not in the job: {rel}")
            continue
        if rel not in edited:
            originals[rel] = (repo / rel).read_text(encoding="utf-8")
            edited[rel] = originals[rel]
        search, replace = m.group("search"), m.group("replace")
        count = edited[rel].count(search) if search else 0
        if count != 1:
            problems.append(f"{rel}: SEARCH matched {count} times, skipped:\n{search[:300]}")
            continue
        edited[rel] = edited[rel].replace(search, replace, 1)
    if not originals and not problems:
        problems.append("no edit blocks found")
    diff = "".join(
        "".join(difflib.unified_diff(originals[rel].splitlines(True), edited[rel].splitlines(True),
                                     f"a/{rel}", f"b/{rel}"))
        for rel in originals if edited[rel] != originals[rel])
    return diff, problems


def run_job(job_file):
    job = json.loads(job_file.read_text(encoding="utf-8"))
    job_id = job_file.stem
    # build in a hidden folder so done/ only ever holds finished jobs
    out = DONE / f".{job_id}.partial"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    repo = Path(job.get("repo", DEFAULT_REPO))
    kind = job.get("kind", "freeform")
    edits = kind in EDIT_KINDS or job.get("output") == "patch"
    system = (KINDS[kind] + ("\n\n" + job["system"] if job.get("system") else "")
              + (EDIT_FORMAT if edits else ""))
    files = job.get("files", [])
    if job.get("lines") and (edits or len(files) != 1):
        raise ValueError("line-range jobs are read-only and take exactly one file")
    _, head = git(repo, "rev-parse", "HEAD")

    # per_file splits a big batch into one model call per file
    groups = [[f] for f in files] if job.get("per_file") else [files]
    sections, meta_calls = [], []
    for group in groups:
        prompt = build_prompt(job, repo, group)
        tokens = est_tokens(system + prompt)
        if tokens > MAX_PROMPT_TOKENS:
            raise ValueError(f"prompt ~{tokens} tokens exceeds {MAX_PROMPT_TOKENS}; "
                             f"use per_file or fewer/smaller files ({group})")
        t0 = time.time()
        r = generate(system, prompt)
        answer = r.get("response", "")
        meta_calls.append({
            "files": group, "prompt_tokens": r.get("prompt_eval_count"),
            "output_tokens": r.get("eval_count"), "seconds": round(time.time() - t0, 1),
            "truncated": r.get("done_reason") == "length",
        })
        sections.append((group, answer))

    lines = [f"# {job_id}", "", f"- kind: {kind}", f"- model: {MODEL}",
             f"- repo HEAD at run: {head}", f"- finished: {now()}", "",
             f"**Instructions:** {job['instructions']}", ""]
    diff_checks = []
    for i, (group, answer) in enumerate(sections):
        rng = job.get("lines")
        heading = ", ".join(group) or "(no files)"
        if rng:
            heading += f" (lines {rng[0]}-{rng[1]}, part {job['chunk'][0]}/{job['chunk'][1]})"
        lines += [f"## {heading}", "", answer, ""]
        if edits:
            diff, problems = apply_edit_blocks(answer, repo, set(group))
            check = {"part": i, "problems": problems}
            if diff:
                patch = out / f"patch-{i}.diff"
                patch.write_bytes(diff.encode("utf-8"))
                rc, detail = git(repo, "apply", "--check", str(patch))
                check.update(ok=rc == 0 and not problems, detail=detail or "applies cleanly")
            else:
                check.update(ok=False, detail="no changes produced")
            diff_checks.append(check)
    if diff_checks:
        lines += ["## Patch check (git apply --check)", ""]
        for c in diff_checks:
            lines.append(f"- patch-{c['part']}.diff: {'OK' if c['ok'] else 'FAIL'} - {c['detail']}")
            lines += [f"  - {p}" for p in c["problems"]]

    (out / "result.md").write_text("\n".join(lines), encoding="utf-8")
    (out / "meta.json").write_text(json.dumps(
        {"job": job, "head": head, "calls": meta_calls, "patch_checks": diff_checks},
        indent=2), encoding="utf-8")
    shutil.copy2(job_file, out / "job.json")
    final = DONE / job_id
    shutil.rmtree(final, ignore_errors=True)
    out.rename(final)
    job_file.unlink()
    return final


def next_job():
    jobs = sorted(QUEUE.glob("*.json"))
    return jobs[0] if jobs else None


def process_one():
    job_file = next_job()
    if job_file is None:
        return False
    running = RUNNING / job_file.name
    job_file.replace(running)
    log(f"start {running.stem}")
    try:
        ensure_ollama()
        out = run_job(running)
        log(f"done  {running.stem} -> {out}")
    except Exception as e:  # noqa: BLE001 - one bad job must not stop the drone
        shutil.rmtree(DONE / f".{running.stem}.partial", ignore_errors=True)
        dest = FAILED / running.name
        running.replace(dest)
        dest.with_suffix(".error.txt").write_text(f"{type(e).__name__}: {e}\n", encoding="utf-8")
        log(f"FAIL  {running.stem}: {type(e).__name__}: {e}")
    return True


def acquire_lock():
    fh = open(STATE_DIR / "drone.lock", "a+")
    try:
        msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        sys.exit("another drone is already running")
    return fh


def recover_stale():
    for f in RUNNING.glob("*.json"):
        log(f"requeue stale {f.stem}")
        f.replace(QUEUE / f.name)


# ---------------------------------------------------------------- cli

def cmd_submit(a):
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    job_id = f"{a.priority}-{stamp}-{slugify(a.title or a.instructions)}"
    job = {"kind": a.kind, "instructions": a.instructions, "files": a.files or [],
           "per_file": a.per_file}
    if a.repo:
        job["repo"] = a.repo
    if a.system:
        job["system"] = a.system
    tmp = QUEUE / f".{job_id}.tmp"
    tmp.write_text(json.dumps(job, indent=2), encoding="utf-8")
    tmp.replace(QUEUE / f"{job_id}.json")
    print(job_id)


def slugify(text, n=40):
    return re.sub(r"[^a-z0-9]+", "-", text.lower())[:n].strip("-")


def cmd_mission(a):
    """Expand globs into one low-priority job per file, so the GPU always has work."""
    repo = Path(a.repo or DEFAULT_REPO)
    name = slugify(a.name, 30)
    files = {p.relative_to(repo).as_posix()
             for g in a.glob for p in repo.glob(g) if p.is_file()}
    files |= {f.replace("\\", "/") for f in a.files}
    files = [f for f in sorted(files) if not any(re.search(x, f) for x in a.exclude)]
    if not files:
        sys.exit("mission matched no files")
    if a.chunk and a.kind in EDIT_KINDS:
        sys.exit("--chunk is for read-only kinds")
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    budget = MAX_PROMPT_TOKENS - est_tokens(KINDS[a.kind] + a.instructions + (a.system or "")) - 300
    count = 0
    for i, rel in enumerate(files):
        text = (repo / rel).read_text(encoding="utf-8", errors="replace")
        ranges = (chunk_ranges(text) if a.chunk and est_tokens(text) > budget else [None])
        for k, rng in enumerate(ranges, 1):
            job = {"kind": a.kind, "instructions": a.instructions, "files": [rel],
                   "mission": name}
            if rng:
                job.update(lines=list(rng), chunk=[k, len(ranges)])
            if a.repo:
                job["repo"] = a.repo
            if a.system:
                job["system"] = a.system
            suffix = f"-c{k}" if rng else ""
            job_id = f"{a.priority}-m-{name}-{stamp}-{i:04d}-{slugify(Path(rel).name, 30)}{suffix}"
            tmp = QUEUE / f".{job_id}.tmp"
            tmp.write_text(json.dumps(job, indent=2), encoding="utf-8")
            tmp.replace(QUEUE / f"{job_id}.json")
            count += 1
    print(f"mission {name}: {count} jobs for {len(files)} files queued at priority {a.priority}")


def cmd_collect(a):
    """Merge a mission's results into one report under STATE_DIR/missions/."""
    name = slugify(a.name, 30)
    tag = f"-m-{name}-"
    def key(job):  # group a file's chunks together, in line order
        return (job["files"][0] if job.get("files") else "", (job.get("lines") or [0])[0])

    done = [(key(json.loads((d / "job.json").read_text(encoding="utf-8"))), d)
            for d in DONE.iterdir() if tag in d.name and (d / "result.md").exists()]
    done = [d for _, d in sorted(done)]
    covered = {json.loads((d / "job.json").read_text(encoding="utf-8"))["files"][0] for d in done}
    # a failure is stale once a later job (e.g. its chunks) succeeded for the same file
    failed = [f for f in sorted(FAILED.glob(f"*{tag}*.error.txt"))
              if json.loads(f.with_name(f.name.replace(".error.txt", ".json"))
                            .read_text(encoding="utf-8"))["files"][0] not in covered]
    pending = [*QUEUE.glob(f"*{tag}*.json"), *RUNNING.glob(f"*{tag}*.json")]
    out_dir = STATE_DIR / "missions"
    out_dir.mkdir(exist_ok=True)
    lines = [f"# Mission {name}", "",
             f"- done: {len(done)} results for {len(covered)} files, failed: {len(failed)}, "
             f"still queued: {len(pending)}",
             f"- collected: {now()}", ""]
    if failed:
        lines += ["## Failed", ""]
        lines += [f"- {f.name}: {f.read_text(encoding='utf-8').strip()}" for f in failed]
        lines.append("")
    for d in done:
        body = (d / "result.md").read_text(encoding="utf-8").split("\n", 8)[-1]
        lines += [f"---\n<!-- {d.name} -->", body, ""]
    report = out_dir / f"{name}.md"
    report.write_text("\n".join(lines), encoding="utf-8")
    print(report)


def cmd_status(_):
    for name, d, pat in (("queued", QUEUE, "*.json"), ("running", RUNNING, "*.json"),
                         ("failed", FAILED, "*.json"), ("done", DONE, "*")):
        items = sorted(p.stem if p.is_file() else p.name for p in d.glob(pat)
                       if not p.name.startswith("."))
        print(f"{name} ({len(items)})")
        for it in items[-15:]:
            print(f"  {it}")
    print(f"ollama: {'up' if ollama_up() else 'down'}")


def cmd_show(a):
    for p in (DONE / a.job_id / "result.md", FAILED / f"{a.job_id}.error.txt"):
        if p.exists():
            print(p.read_text(encoding="utf-8"))
            return
    sys.exit(f"no result for {a.job_id}")


def cmd_watch(_):
    lock = acquire_lock()  # noqa: F841 - held for the process lifetime
    recover_stale()
    log(f"watching {QUEUE} with {MODEL}")
    busy = True  # so a model left loaded by a previous run is freed on the first idle poll
    while True:
        if process_one():
            busy = True
            continue
        if busy:  # unload once, on the busy -> empty transition
            unload_model()
            busy = False
        time.sleep(POLL_SECONDS)


def cmd_run_once(_):
    lock = acquire_lock()  # noqa: F841
    recover_stale()
    while process_one():
        pass
    unload_model()


def main():
    ensure_dirs()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("submit")
    s.add_argument("--kind", choices=sorted(KINDS), default="freeform")
    s.add_argument("--instructions", required=True)
    s.add_argument("--files", nargs="*", help="paths relative to the repo root")
    s.add_argument("--repo", help=f"default {DEFAULT_REPO}")
    s.add_argument("--title")
    s.add_argument("--system", help="extra system-prompt text")
    s.add_argument("--priority", default="5", choices=list("123456789"), help="1 runs first")
    s.add_argument("--per-file", action="store_true", help="one model call per file")
    s.set_defaults(fn=cmd_submit)
    m = sub.add_parser("mission", help="one job per file matched by --glob")
    m.add_argument("--name", required=True)
    m.add_argument("--glob", action="append", default=[], help="relative to the repo; repeatable")
    m.add_argument("--files", nargs="*", default=[], help="explicit paths relative to the repo")
    m.add_argument("--chunk", action="store_true",
                   help="split files over the prompt budget into line-range jobs")
    m.add_argument("--exclude", action="append", default=[], help="regex on the relative path")
    m.add_argument("--kind", choices=sorted(KINDS), default="explain")
    m.add_argument("--instructions", required=True)
    m.add_argument("--repo")
    m.add_argument("--system")
    m.add_argument("--priority", default="8", choices=list("123456789"))
    m.set_defaults(fn=cmd_mission)
    c = sub.add_parser("collect", help="merge a mission's results into one report")
    c.add_argument("name")
    c.set_defaults(fn=cmd_collect)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    sh = sub.add_parser("show")
    sh.add_argument("job_id")
    sh.set_defaults(fn=cmd_show)
    sub.add_parser("watch").set_defaults(fn=cmd_watch)
    sub.add_parser("run-once").set_defaults(fn=cmd_run_once)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
