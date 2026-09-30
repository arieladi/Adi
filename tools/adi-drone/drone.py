"""adi-drone: an offline batch worker that feeds queued jobs to a local Ollama model.

The drone never writes to a repository. It reads the files a job names, asks the
model, and writes everything it produced under STATE_DIR/done/<job-id>/ for a
human (or Claude) to review and apply. Patches are checked with
`git apply --check`, which is read-only.

Usage:
  drone.py submit --kind review --files adi_daw/src/foo.cpp --instructions "..."
  drone.py mission --name rmsc-docs --glob "adi_daw/plugins/rmsc/**/*.cpp" --instructions "..."
  drone.py mission ... --dry-run     # job count + measured ETA, queues nothing
  drone.py collect rmsc-docs
  drone.py retry rmsc-docs --truncated --max-output 4096 [--dry-run]
  drone.py eta                       # measured time left for the queue
  drone.py status
  drone.py show <job-id>
  drone.py watch            # run forever (what the logon task starts)
  drone.py run-once         # drain the queue, then exit
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import difflib
import json
import http.client
import msvcrt
import os
import re
import shutil
import subprocess
import sys
import time
import threading
import urllib.error
import urllib.request
from pathlib import Path

STATE_DIR = Path(os.environ.get("ADI_DRONE_HOME", r"D:\adi-drone"))
# The checkout this file runs from (tools/adi-drone/drone.py -> the repo root), not a fixed
# path: the checkout moved from C:\Users\Adi\Documents\GitHub\Adi (salon-tv) to D: (ORC-VST).
DEFAULT_REPO = Path(__file__).resolve().parents[2]
OLLAMA_URL = os.environ.get("OLLAMA_HOST_URL", "http://127.0.0.1:11434")
OLLAMA_EXE = Path(os.environ["LOCALAPPDATA"]) / "Programs" / "Ollama" / "ollama.exe"
OLLAMA_MODELS = r"D:\ollama\models"
MODEL = os.environ.get("ADI_DRONE_MODEL", "qwen2.5-coder:7b")

NUM_CTX = 16384          # fits 100% on the GTX 1070 (5.5 GB)
MAX_PROMPT_TOKENS = 11000  # leaves ~5K for the answer
# Hard cap on output. Without it a repetition loop never ends (Ollama shifts
# the context) and the job dies at REQUEST_TIMEOUT after 30 min of wasted GPU.
# Summaries need ~600 tokens; list-shaped jobs (test ideas) pass --max-output 4096.
NUM_PREDICT = 2048
MAX_OUTPUT_LIMIT = 4096  # 16K ctx - 11K prompt budget leaves ~5K
TRUNC_MARK = "Treat as incomplete."  # collect counts results carrying it
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
        "in the repository if visible. Cover edge cases. Output complete test code. "
        "Only ADD: never delete, move or rewrite an existing test or check. Anchor each "
        "edit on one short line (for example the line that starts main) and put the new "
        "code next to it. A new check must test something no existing check tests."
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

Rules: the first line of each block is the real path of one of the files shown,
exactly as it appears after "=== FILE:", never the words path/relative/to/repo.ext.
The SEARCH text must match the file character for character, including
indentation. Keep each block small. Output only edit blocks, no other text."""

EDIT_BLOCK = re.compile(
    r"^(?P<path>[^\n<>=`]+?)\s*\n(?:```[^\n]*\n)?<<<<<<< SEARCH\n(?P<search>.*?)\n?=======\n"
    r"(?P<replace>.*?)\n?>>>>>>> REPLACE", re.S | re.M)

# Measured 2026-09-27 over 22,000 claims: the model's names are right 97-100% of
# the time and its numbers are not (15 of 25 Mixxx defaults right in the .cpp it
# read; ranges invented outright for headers that declare none). So read-only jobs
# may not state a number in their own words: they quote the line, and collect
# keeps only the quotes found verbatim in the source. The rule travels in the
# job's `system` text, so a drone still running older code applies it too.
QUOTE_RULE = """Rules for facts. A summary that invents a number is worse than none.
- Never write a number, range, default, size, offset, byte order, constant or
  unit in your own words.
- When a fact comes from the code, quote the line that states it, copied
  character for character from the file, on a line of its own that starts
  with QUOTE: . For example:
  QUOTE:     q->setRange(0.4, 0.707106781, 4.0);
- If the file does not state it, write "not stated in this file". Never fill
  in a value from memory or from another program.
- Names of functions, types, variables and files are fine, in backticks."""
READ_ONLY_KINDS = {"explain", "freeform", "review"}


def now():
    return dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


_claim_lock = threading.Lock()
_log_lock = threading.Lock()
_ollama_lock = threading.Lock()


def log(msg):
    line = f"{now()} {msg}"
    with _log_lock:
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


class GenerationConnectionError(Exception):
    """Only a transport failure before /api/generate's body completed."""
    def __init__(self, error):
        super().__init__(str(error))
        self.error = error


def retryable_connection(error):
    if isinstance(error, urllib.error.HTTPError):
        return False
    if isinstance(error, urllib.error.URLError):
        error = error.reason
    return isinstance(error, (http.client.RemoteDisconnected, ConnectionResetError,
                              ConnectionAbortedError, ConnectionRefusedError, TimeoutError))


def generate(system, prompt, num_predict=NUM_PREDICT):
    body = json.dumps({
        "model": MODEL, "system": system, "prompt": prompt, "stream": False,
        "options": {"num_ctx": NUM_CTX, "temperature": 0.2,
                    "num_predict": num_predict, "repeat_penalty": 1.1},
        "keep_alive": KEEP_ALIVE,
    }).encode()
    req = urllib.request.Request(f"{OLLAMA_URL}/api/generate", data=body,
                                 headers={"Content-Type": "application/json"})
    body_complete = False
    try:
        with urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT) as r:
            response = r.read()
            body_complete = True
    except Exception as error:
        if isinstance(error, urllib.error.HTTPError):
            error.close()
        if not body_complete and retryable_connection(error):
            raise GenerationConnectionError(error) from error
        raise
    # Parsing (and anything after a fully read body) is never retried.
    return json.loads(response)


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


# ---------------------------------------------------------------- grounding

QUOTE_LINE = re.compile(r"^(\s*(?:[-*>]\s*)?)QUOTE:\s?(.*)$")
SECTION_HEAD = re.compile(r"^## (\S+?)(?: \(lines \d+-\d+, part \d+/\d+\))?\s*$", re.M)
TICKED = re.compile(r"`([^`\n]{2,80})`")
IDENT = re.compile(r"[A-Za-z_~][A-Za-z0-9_]{2,}")
NUMBER = re.compile(r"(?<![\w.])(-?\d+(?:\.\d+)?)(?![\w])")
TRIVIAL_NUMBERS = set("0123456789") | {"10", "100"}
COMMON_WORDS = {"the", "and", "for", "not", "true", "false", "null", "none", "std", "int", "float",
                "double", "bool", "void", "const", "auto", "self"}


def _ws(s):
    return re.sub(r"\s+", " ", s).strip()


def ground_section(body, source):
    """Check one summary against the file it summarises; return (body, counts).

    A QUOTE line found verbatim in the source (whitespace aside) is kept and
    marked; any other QUOTE line is replaced by a note saying it was removed.
    Backticked names and numbers outside quotes are counted, not changed: a name
    in its file is almost always right, and a number in its file may still sit
    in the wrong role (a maximum reported as the default).
    """
    counts = {k: 0 for k in ("quote_ok", "quote_bad", "name_ok", "name_bad", "num_ok", "num_bad")}
    flat = _ws(source)
    out = []
    for line in body.splitlines():
        m = QUOTE_LINE.match(line)
        if m:
            q = m.group(2).strip()
            q = q[1:-1].strip() if len(q) > 1 and q[0] == q[-1] == "`" else q
            if q and _ws(q) in flat:
                counts["quote_ok"] += 1
                out.append(f"{m.group(1)}QUOTE (verbatim in the source): {q}")
            else:
                counts["quote_bad"] += 1
                out.append(f"{m.group(1)}~~QUOTE removed by the drone: not found in the source~~")
            continue
        if not line.lstrip().startswith("<!--"):
            for t in TICKED.findall(line):
                for tok in IDENT.findall(t):
                    if tok.lower() in COMMON_WORDS:
                        continue
                    counts["name_ok" if tok in source else "name_bad"] += 1
                    break
            for n in NUMBER.findall(re.sub(r"^\s*(\d+\.|[-*])\s+", "", line)):
                if n.lstrip("-") not in TRIVIAL_NUMBERS:
                    counts["num_ok" if n.lstrip("-") in source else "num_bad"] += 1
        out.append(line)
    return "\n".join(out), counts


def ground_text(text, repo):
    """Ground every `## path` section of a result or a mission report."""
    totals = {k: 0 for k in ("quote_ok", "quote_bad", "name_ok", "name_bad", "num_ok", "num_bad")}
    heads = list(SECTION_HEAD.finditer(text))
    if not heads:
        return text, totals
    pieces = [text[:heads[0].start()]]
    for k, h in enumerate(heads):
        end = heads[k + 1].start() if k + 1 < len(heads) else len(text)
        body = text[h.end():end]
        src_path = (Path(repo) / h.group(1)).resolve()
        if src_path.is_file():
            body, c = ground_section(body, src_path.read_text(encoding="utf-8", errors="replace"))
            for key in totals:
                totals[key] += c[key]
        pieces += [h.group(0), body]
    return "".join(pieces), totals


def trust_line(t):
    def part(ok, bad):
        return f"{t[ok]} of {t[ok] + t[bad]}"
    return (f"- trust: **names reliable, numbers unverified.** Backticked names found in their files: "
            f"{part('name_ok', 'name_bad')}. QUOTE lines checked verbatim against the source: "
            f"{t['quote_ok']} kept, {t['quote_bad']} removed. Numbers outside quotes are the model's: "
            f"{part('num_ok', 'num_bad')} appear somewhere in their file, which does not make them right.")


def apply_edit_blocks(text, repo, allowed):
    """Apply SEARCH/REPLACE blocks in memory; return (unified diff, problems)."""
    originals, edited, problems = {}, {}, []
    for m in EDIT_BLOCK.finditer(text):
        rel = m.group("path").strip().strip("`*").replace("\\", "/")
        if rel not in allowed:
            # A 7B model copies the format's example path. The SEARCH text is the
            # evidence: if it occurs exactly once in exactly one of the job's files,
            # that file is the target, and the note says the path was resolved.
            hits = [f for f in allowed
                    if (repo / f).read_text(encoding="utf-8").count(m.group("search")) == 1]
            if len(hits) != 1 or not m.group("search"):
                problems.append(f"edit targets a file not in the job: {rel}")
                continue
            problems.append(f"path '{rel}' resolved to {hits[0]} by its SEARCH text")
            rel = hits[0]
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
        cap = int(job.get("max_output", NUM_PREDICT))
        t0 = time.time()
        for attempt in range(1, 4):
            try:
                r = generate(system, prompt, cap)
                break
            except GenerationConnectionError as failure:
                if attempt == 3:
                    raise failure.error
                error = failure.error
                log(f"retry {job_id} attempt {attempt + 1}/3: {type(error).__name__}: {error}")
                time.sleep((2, 5)[attempt - 1])
        answer = r.get("response", "")
        meta_calls.append({
            "files": group, "prompt_tokens": r.get("prompt_eval_count"),
            "output_tokens": r.get("eval_count"), "seconds": round(time.time() - t0, 1),
            "truncated": r.get("done_reason") == "length", "max_output": cap,
        })
        if meta_calls[-1]["truncated"]:
            answer += (f"\n\n> **drone: output hit the {cap}-token cap and is cut off "
                       f"(a long list or a repetition loop). {TRUNC_MARK}**")
            log(f"truncated output in {job_id} ({group})")
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
                real = [p for p in problems if " resolved to " not in p]  # a resolved path is a note
                check.update(ok=rc == 0 and not real, detail=detail or "applies cleanly")
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
    with _claim_lock:
        while True:
            job_file = next_job()
            if job_file is None:
                return False
            running = RUNNING / job_file.name
            try:
                # Rename is the persistent claim. Serialize the short claim
                # operation too: concurrent Win32 MoveFileEx calls can open
                # the same source before either renames it. The process lock
                # excludes other drones; this lock excludes sibling workers.
                job_file.replace(running)
                break
            except FileNotFoundError:
                if job_file.exists():
                    raise  # missing destination folder, not a lost claim
    log(f"start {running.stem}")
    try:
        with _ollama_lock:
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


def process_queue(parallel=1):
    def drain():
        busy = False
        while process_one():
            busy = True
        return busy

    # Keep the serial path on the calling thread, with identical per-job
    # execution, logs and result bytes. No executor is created for N=1.
    if parallel == 1:
        return drain()
    with ThreadPoolExecutor(max_workers=parallel, thread_name_prefix="adi-drone") as workers:
        futures = [workers.submit(drain) for _ in range(parallel)]
        # Read every result (no short circuit), including a worker's fatal
        # exception. Never unload Ollama until all workers have joined.
        results = [future.result() for future in futures]
    return any(results)


def positive_parallel(value):
    n = int(value)
    if n < 1:
        raise argparse.ArgumentTypeError("--parallel must be at least 1")
    return n


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
    system = job_system(a.kind, a.system, a.no_quote_rule)
    if system:
        job["system"] = system
    if a.max_output:
        job["max_output"] = min(a.max_output, MAX_OUTPUT_LIMIT)
    if a.patch:
        job["output"] = "patch"
    write_jobs([(job_id, job)])
    print(job_id)


def job_system(kind, extra, no_quote_rule=False):
    """The job's extra system text: the quote rule first for read-only kinds."""
    parts = [] if no_quote_rule or kind not in READ_ONLY_KINDS else [QUOTE_RULE]
    if extra:
        parts.append(extra)
    return "\n\n".join(parts)


def slugify(text, n=40):
    return re.sub(r"[^a-z0-9]+", "-", text.lower())[:n].strip("-")


# ---------------------------------------------------------------- estimates

STAMP = re.compile(r"-(\d{8}-\d{6})-")


def history():
    """Measured seconds per model call from every finished job, keyed by mission and kind."""
    by_mission, by_kind, rows = {}, {}, []
    for meta in DONE.glob("*/meta.json"):
        try:
            m = json.loads(meta.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        job = m.get("job", {})
        for c in m.get("calls", []):
            row = (c["seconds"], c.get("output_tokens") or 0, bool(c.get("truncated")))
            by_mission.setdefault(job.get("mission"), []).append(row)
            by_kind.setdefault(job.get("kind", "freeform"), []).append(row)
            rows.append(row)
    return by_mission, by_kind, rows


MIN_SAMPLES = 10       # fewer measured calls than this is not a mean worth trusting
PROMPT_SECONDS = 5.0   # prompt eval of a full ~7K-token chunk on the 1070, for the upper bound


def gen_tokens_per_second(rows):
    """Generation speed, from long answers where output time dominates."""
    rates = sorted(r[1] / r[0] for r in rows if r[1] >= 1000 and r[0] > 0)
    return rates[len(rates) // 2] if rates else 30.0


def estimate_seconds(jobs, hist=None):
    """Return (expected, upper bound, sources) in seconds for jobs.

    Expected: the job's mission's measured mean, else its kind's, else all jobs'
    (each needs MIN_SAMPLES calls). It is only as good as that history: a new
    prompt shape (list-style output) can run several times longer than the
    average, so the upper bound assumes every job writes up to its output cap.
    """
    by_mission, by_kind, rows = hist or history()
    tok_s = gen_tokens_per_second(rows)
    total, upper, sources = 0.0, 0.0, {}
    for job in jobs:
        for label, ref in ((f"mission {job.get('mission')}", by_mission.get(job.get("mission"))),
                           (f"kind {job.get('kind')}", by_kind.get(job.get("kind"))),
                           ("all jobs", rows)):
            if ref and len(ref) >= MIN_SAMPLES:
                break
        else:
            label, ref = "no history (30 s guess)", [(30.0, 0, False)]
        mean = sum(r[0] for r in ref) / len(ref)
        cap = int(job.get("max_output", NUM_PREDICT))
        if cap > NUM_PREDICT:
            # calls that hit the old cap can now keep writing
            capped = sum(r[2] for r in ref) / len(ref)
            mean += capped * (cap - NUM_PREDICT) / tok_s
        total += mean
        upper += max(mean, PROMPT_SECONDS + cap / tok_s)
        sources[label] = (len(ref), sum(r[0] for r in ref) / len(ref))
    return total, upper, sources


def fmt_hours(sec):
    return f"{int(sec // 3600)}h{int(sec % 3600 // 60):02d}m"


def report_estimate(jobs, what):
    sec, upper, sources = estimate_seconds(jobs)
    end = (dt.datetime.now() + dt.timedelta(seconds=sec)).strftime("%a %H:%M")
    print(f"{what}: {len(jobs)} jobs, expected {fmt_hours(sec)} (done ~{end} if started now), "
          f"at most {fmt_hours(upper)} if every job writes to its cap")
    for label, (n, mean) in sources.items():
        print(f"  based on {label}: {n} measured calls, mean {mean:.1f}s")
    if not any(label.startswith("mission ") for label in sources):
        print("  note: no history for this mission yet; list-style prompts can run several "
              "times the expected figure")
    return sec


def write_jobs(jobs):
    for job_id, job in jobs:
        tmp = QUEUE / f".{job_id}.tmp"
        tmp.write_text(json.dumps(job, indent=2), encoding="utf-8")
        tmp.replace(QUEUE / f"{job_id}.json")


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
    if a.chunk and (a.kind in EDIT_KINDS or a.patch):
        sys.exit("--chunk is for read-only kinds")
    if a.max_output and not 256 <= a.max_output <= MAX_OUTPUT_LIMIT:
        sys.exit(f"--max-output must be 256..{MAX_OUTPUT_LIMIT}")
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    system = job_system(a.kind, a.system, a.no_quote_rule)
    budget = MAX_PROMPT_TOKENS - est_tokens(KINDS[a.kind] + a.instructions + system) - 300
    jobs = []
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
            if system:
                job["system"] = system
            if a.max_output:
                job["max_output"] = a.max_output
            if a.patch:
                job["output"] = "patch"
            suffix = f"-c{k}" if rng else ""
            jobs.append((f"{a.priority}-m-{name}-{stamp}-{i:04d}-"
                         f"{slugify(Path(rel).name, 30)}{suffix}", job))
    report_estimate([j for _, j in jobs], f"mission {name} ({len(files)} files)")
    if a.dry_run:
        print("dry run: nothing queued")
        return
    write_jobs(jobs)
    print(f"queued at priority {a.priority}")


def cmd_retry(a):
    """Requeue a mission's truncated results and/or failed jobs, optionally with a bigger cap."""
    name = slugify(a.mission, 30)
    tag = f"-m-{name}-"
    if not (a.truncated or a.failed):
        sys.exit("pass --truncated and/or --failed")
    if a.max_output and not 256 <= a.max_output <= MAX_OUTPUT_LIMIT:
        sys.exit(f"--max-output must be 256..{MAX_OUTPUT_LIMIT}")
    latest = latest_results(tag)
    picked = []  # (old id, job, failed file or None)
    if a.truncated:
        for d in latest.values():
            meta = json.loads((d / "meta.json").read_text(encoding="utf-8"))
            if any(c.get("truncated") for c in meta.get("calls", [])):
                picked.append((d.name, json.loads((d / "job.json").read_text(encoding="utf-8")), None))
    if a.failed:
        for f in sorted(FAILED.glob(f"*{tag}*.json")):
            job = json.loads(f.read_text(encoding="utf-8"))
            if result_key(job) not in latest:
                picked.append((f.stem, job, f))
    if not picked:
        print("nothing to retry")
        return
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    jobs = []
    for old_id, job, _ in picked:
        if a.max_output:
            job["max_output"] = a.max_output
        rest = STAMP.split(old_id, maxsplit=1)[-1]
        jobs.append((f"{a.priority}-m-{name}-{stamp}-r-{rest}", job))
    report_estimate([j for _, j in jobs], f"retry {name}")
    if a.dry_run:
        for job_id, _ in jobs:
            print(f"  would queue {job_id}")
        print("dry run: nothing queued")
        return
    write_jobs(jobs)
    for _, _, failed_file in picked:
        if failed_file:  # the retry replaces the failure
            failed_file.with_name(failed_file.stem + ".error.txt").unlink(missing_ok=True)
            failed_file.unlink()
    print(f"queued {len(jobs)} retries at priority {a.priority}")


def cmd_eta(_):
    pending = [*RUNNING.glob("*.json"), *QUEUE.glob("*.json")]
    if not pending:
        print("queue empty")
        return
    groups = {}
    for p in pending:
        job = json.loads(p.read_text(encoding="utf-8"))
        groups.setdefault(job.get("mission") or "(single jobs)", []).append(job)
    hist = history()
    total = top = 0.0
    for mission, jobs in sorted(groups.items()):
        sec, upper, _ = estimate_seconds(jobs, hist)
        total += sec
        top += upper
        print(f"  {mission:28} {len(jobs):5} jobs  expected {fmt_hours(sec)}  at most {fmt_hours(upper)}")
    end = (dt.datetime.now() + dt.timedelta(seconds=total)).strftime("%a %H:%M")
    print(f"total {len(pending)} jobs, expected {fmt_hours(total)} (done ~{end}), "
          f"at most {fmt_hours(top)}")


def result_key(job):
    """One report slot: a whole file, or one line range of it."""
    return (job["files"][0] if job.get("files") else "", tuple(job.get("lines") or ()))


def latest_results(tag):
    """Newest finished result per slot, so a retry replaces what it retried."""
    best = {}
    for d in DONE.iterdir():
        if tag not in d.name or not (d / "result.md").exists():
            continue
        key = result_key(json.loads((d / "job.json").read_text(encoding="utf-8")))
        m = STAMP.search(d.name)
        stamp = m.group(1) if m else ""
        if key not in best or stamp > best[key][0]:
            best[key] = (stamp, d)
    return {k: d for k, (_, d) in best.items()}


def cmd_collect(a):
    """Merge a mission's results into one report under STATE_DIR/missions/."""
    name = slugify(a.name, 30)
    tag = f"-m-{name}-"
    latest = latest_results(tag)
    # a file's chunks together, in line order
    done = [latest[k] for k in sorted(latest, key=lambda k: (k[0], k[1][:1] or (0,)))]
    covered = {k[0] for k in latest}

    def superseded(job):
        # a whole-file failure is stale once any later result (e.g. its chunks) exists;
        # a failed chunk only once that same line range has succeeded
        if job.get("lines"):
            return result_key(job) in latest
        return job["files"][0] in covered

    failed = [f for f in sorted(FAILED.glob(f"*{tag}*.error.txt"))
              if not superseded(json.loads(f.with_name(f.name.replace(".error.txt", ".json"))
                                           .read_text(encoding="utf-8")))]
    pending = [*QUEUE.glob(f"*{tag}*.json"), *RUNNING.glob(f"*{tag}*.json")]
    incomplete = sum(TRUNC_MARK in (d / "result.md").read_text(encoding="utf-8") for d in done)
    out_dir = STATE_DIR / "missions"
    out_dir.mkdir(exist_ok=True)
    totals = {k: 0 for k in ("quote_ok", "quote_bad", "name_ok", "name_bad", "num_ok", "num_bad")}
    bodies = []
    for d in done:
        body = (d / "result.md").read_text(encoding="utf-8").split("\n", 8)[-1]
        job = json.loads((d / "job.json").read_text(encoding="utf-8"))
        body, t = ground_text(body, job.get("repo", DEFAULT_REPO))
        for key in totals:
            totals[key] += t[key]
        bodies += [f"---\n<!-- {d.name} -->", body, ""]
    lines = [f"# Mission {name}", "",
             f"- done: {len(done)} results for {len(covered)} files, incomplete (hit the output "
             f"cap): {incomplete}, failed: {len(failed)}, still queued: {len(pending)}",
             f"- collected: {now()}", trust_line(totals), ""]
    if failed:
        lines += ["## Failed", ""]
        lines += [f"- {f.name}: {f.read_text(encoding='utf-8').strip()}" for f in failed]
        lines.append("")
    lines += bodies
    report = out_dir / f"{name}.md"
    report.write_text("\n".join(lines), encoding="utf-8")
    print(report)
    print(trust_line(totals)[2:])


def cmd_verify(a):
    """Measure how grounded a collected mission report is, without changing it."""
    report = STATE_DIR / "missions" / f"{slugify(a.name, 30)}.md"
    if not report.exists():
        sys.exit(f"no report {report}; run collect first")
    _, t = ground_text(report.read_text(encoding="utf-8"), a.repo or DEFAULT_REPO)
    print(trust_line(t)[2:])


# ---------------------------------------------------------------- the build decides

VERIFY_TREE = STATE_DIR / "verify-tree"
# CTest runs every test binary and reports each one; tools/test_all.sh would also
# compare the total check count with the README, which a patch adding tests changes.
CTEST = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
             r"\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe")


def _run(cmd, cwd, log_file, timeout):
    env = {k: v for k, v in os.environ.items() if k != "NoDefaultCurrentDirectoryInExePath"}
    p = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True, errors="replace",
                       env=env, timeout=timeout)
    with open(log_file, "a", encoding="utf-8") as f:
        f.write(f"$ {' '.join(map(str, cmd))}  (in {cwd})\n{p.stdout}{p.stderr}\nexit {p.returncode}\n\n")
    return p.returncode


def prepare_tree(repo):
    """A detached worktree of the monorepo on D:, with adi_daw/third_party linked in.

    third_party is git-ignored and fetched, so a fresh worktree has none; a
    directory junction lends it the main checkout's copy, read only in use. The
    tree keeps its own build folder, so each check rebuilds incrementally.
    """
    if not (VERIFY_TREE / ".git").exists():
        rc, out = git(repo, "worktree", "add", "--detach", str(VERIFY_TREE), "HEAD")
        if rc:
            sys.exit(f"worktree add failed: {out}")
    link = VERIFY_TREE / "adi_daw" / "third_party"
    if not link.exists():
        subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(Path(repo) / "adi_daw" / "third_party")],
                       capture_output=True, check=True)


def cmd_check(a):
    """Apply each patch of a job or mission in the verify tree, build, run the tests.

    The verdict is the build's and the tests', written beside the patch as
    check-N.json and check-N.log. The user's checkout is never touched.
    """
    repo = Path(a.repo or DEFAULT_REPO)
    target = DONE / a.target
    dirs = [target] if target.is_dir() else sorted(latest_results(f"-m-{slugify(a.target, 30)}-").values())
    dirs = [d for d in dirs if any(d.glob("patch-*.diff"))]
    if not dirs:
        sys.exit("no patches to check")
    prepare_tree(repo)
    summary = []
    for d in dirs:
        head = json.loads((d / "meta.json").read_text(encoding="utf-8"))["head"]
        for patch in sorted(d.glob("patch-*.diff")):
            n = patch.stem.split("-")[-1]
            log_file = d / f"check-{n}.log"
            log_file.write_text("", encoding="utf-8")
            git(VERIFY_TREE, "checkout", "--detach", "--force", head)
            git(VERIFY_TREE, "clean", "-fd")          # not -x: the build folder survives
            verdict = {"patch": patch.name, "head": head, "checked": now()}
            kind = json.loads((d / "job.json").read_text(encoding="utf-8")).get("kind")
            diff = patch.read_text(encoding="utf-8")
            removed = [l for l in diff.splitlines() if l.startswith("-") and not l.startswith("---")]
            added = [l for l in diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
            if kind == "tests" and (any("check(" in l for l in removed) or not any("check(" in l for l in added)):
                # A test patch that deletes checks can still build and pass.
                verdict.update(applies=None, build=None, tests=None,
                               guard="a tests patch must only add checks: it removes one or adds none")
            elif git(VERIFY_TREE, "apply", str(patch))[0]:
                verdict.update(applies=False, build=None, tests=None)
            else:
                verdict["applies"] = True
                verdict["build"] = _run(["cmd", "/c", str(VERIFY_TREE / "adi_daw" / "tools" / "build.bat"), "werror"],
                                        VERIFY_TREE / "adi_daw", log_file, 3600) == 0
                verdict["tests"] = (_run([str(CTEST), "--test-dir", "build", "--output-on-failure", "-j", "4"],
                                         VERIFY_TREE / "adi_daw", log_file, 3600) == 0) if verdict["build"] else None
            verdict["pass"] = bool(verdict["applies"] and verdict["build"] and verdict["tests"])
            (d / f"check-{n}.json").write_text(json.dumps(verdict, indent=2), encoding="utf-8")
            git(VERIFY_TREE, "checkout", "--force", head)
            git(VERIFY_TREE, "clean", "-fd")
            summary.append((d.name, patch.name, verdict))
            log(f"check {d.name}/{patch.name}: {'PASS' if verdict['pass'] else 'FAIL'}")
    for job_id, name, v in summary:
        why = v.get("guard") or ("does not apply" if not v["applies"] else (
            "build fails" if not v["build"] else ("tests fail" if not v["tests"] else "builds, tests pass")))
        print(f"{'PASS' if v['pass'] else 'FAIL'}  {job_id}/{name}: {why}")


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


def cmd_watch(a):
    lock = acquire_lock()  # noqa: F841 - held for the process lifetime
    recover_stale()
    log(f"watching {QUEUE} with {MODEL}")
    busy = True  # so a model left loaded by a previous run is freed on the first idle poll
    while True:
        if process_queue(a.parallel):
            busy = True
            continue
        if busy:  # unload once, on the busy -> empty transition
            unload_model()
            busy = False
        time.sleep(POLL_SECONDS)


def cmd_run_once(a):
    lock = acquire_lock()  # noqa: F841
    recover_stale()
    process_queue(a.parallel)
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
    s.add_argument("--max-output", type=int, help=f"output token cap (default {NUM_PREDICT})")
    s.add_argument("--patch", action="store_true", help="answer as edit blocks, e.g. tests to check")
    s.add_argument("--no-quote-rule", action="store_true", help="let a read-only job state numbers")
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
    m.add_argument("--max-output", type=int, help=f"output token cap (default {NUM_PREDICT})")
    m.add_argument("--dry-run", action="store_true", help="show job count and measured ETA only")
    m.add_argument("--patch", action="store_true", help="answer as edit blocks, e.g. tests to check")
    m.add_argument("--no-quote-rule", action="store_true", help="let a read-only job state numbers")
    m.set_defaults(fn=cmd_mission)
    r = sub.add_parser("retry", help="requeue a mission's truncated and/or failed jobs")
    r.add_argument("mission")
    r.add_argument("--truncated", action="store_true", help="results that hit the output cap")
    r.add_argument("--failed", action="store_true", help="jobs in failed/")
    r.add_argument("--max-output", type=int)
    r.add_argument("--priority", default="7", choices=list("123456789"))
    r.add_argument("--dry-run", action="store_true")
    r.set_defaults(fn=cmd_retry)
    sub.add_parser("eta", help="measured time left for the queue").set_defaults(fn=cmd_eta)
    c = sub.add_parser("collect", help="merge a mission's results into one report, grounded")
    c.add_argument("name")
    c.set_defaults(fn=cmd_collect)
    v = sub.add_parser("verify", help="how grounded a collected report is (names, quotes, numbers)")
    v.add_argument("name")
    v.add_argument("--repo")
    v.set_defaults(fn=cmd_verify)
    k = sub.add_parser("check", help="apply, build and test a job's or mission's patches in a worktree")
    k.add_argument("target", help="a job id in done/, or a mission name")
    k.add_argument("--repo")
    k.set_defaults(fn=cmd_check)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    sh = sub.add_parser("show")
    sh.add_argument("job_id")
    sh.set_defaults(fn=cmd_show)
    for command, function in (("watch", cmd_watch), ("run-once", cmd_run_once)):
        worker = sub.add_parser(command)
        worker.add_argument("--parallel", type=positive_parallel, default=1,
                            help="concurrent job workers (default: 1)")
        worker.set_defaults(fn=function)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
