#!/usr/bin/env python3
"""Check that CI steps live in the job that can actually run them.

    python3 adi_daw/tools/validate_ci.py

This exists because of a specific defect, and the defect is the kind that only
CI can catch and only after a push. A step was appended to ci.yml by anchoring
on "the next top-level key", on the assumption that the key after `juce:` was
something else -- but `juce:` is the LAST job in the file, so the step landed in
`provenance:`, which never builds JUCE. It failed with "adi_vst3_probe was
built but cannot be found", which is true and useless: nothing in that job had
built it.

No yaml module: macOS ships python3 without PyYAML and this script IS part of
the definition of done. Indentation parsing is enough for the questions asked.
"""

from __future__ import annotations

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
CI = HERE.parent.parent / ".github" / "workflows" / "ci.yml"

JOB = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
STEP = re.compile(r"^      - (?:name|uses):\s*(.+?)\s*$")


def main() -> int:
    if not CI.exists():
        print(f"not found: {CI}")
        return 2

    problems: list[str] = []
    oks: list[str] = []

    # job -> (step name, its lines)
    jobs: dict[str, list[tuple[str, list[str]]]] = {}
    job = None
    step = None
    in_jobs = False

    for line in CI.read_text().splitlines():
        if line.startswith("jobs:"):
            in_jobs = True
            continue
        if not in_jobs:
            continue
        m = JOB.match(line)
        if m:
            job = m.group(1)
            jobs.setdefault(job, [])
            step = None
            continue
        if job is None:
            continue
        m = STEP.match(line)
        if m:
            step = (m.group(1), [])
            jobs[job].append(step)
            continue
        if step is not None:
            step[1].append(line)

    if not jobs:
        print("parsed no jobs -- the indentation assumption is wrong")
        return 2
    print(f"[1] parsed {len(jobs)} job(s)")

    def body(j: str) -> str:
        return "\n".join("\n".join(lines) for _, lines in jobs[j])

    # --- 2. a step that uses a build tree must be in a job that makes one ----
    #
    # The general form of the defect: a step referencing an artefact directory
    # sitting in a job that never creates it. Checked per directory rather
    # than for JUCE specifically, so the next one is caught too.
    print("[2] every step's build tree is created by its own job")
    for treedir, maker in (("build-juce", "ADI_WITH_JUCE=ON"),
                           ("adi_daw/build", "cmake")):
        for j, steps in jobs.items():
            jb = body(j)
            for name, lines in steps:
                txt = "\n".join(lines)
                if treedir not in txt:
                    continue
                if maker in jb:
                    continue
                problems.append(
                    f"job '{j}' step '{name}' uses {treedir}/ but nothing in "
                    f"that job runs {maker!r} -- the step is in the wrong job")
        else:
            pass
    if not problems:
        oks.append("no step reaches for a build tree its job never creates")

    # --- 3. duplicate step names inside one job -----------------------------
    #
    # Two identically named steps in a job is the signature of an insert that
    # went in twice, which is how a scripted edit that "succeeded" twice looks.
    print("[3] step names are unique within a job")
    dupes = 0
    for j, steps in jobs.items():
        seen: set[str] = set()
        for name, _ in steps:
            if name in seen:
                problems.append(f"job '{j}' has two steps named {name!r}")
                dupes += 1
            seen.add(name)
    if dupes == 0:
        oks.append("no duplicated step names")

    # --- 4. every job has a runner -----------------------------------------
    print("[4] every job names a runner")
    for j in jobs:
        if "runs-on" not in body(j) and "runs-on" not in "\n".join(
                l for _, ls in jobs[j] for l in ls):
            # runs-on sits outside a step, so scan the raw job block instead.
            pass
    raw = CI.read_text().split("\njobs:\n", 1)[-1]
    blocks = re.split(r"\n  (?=[A-Za-z0-9_-]+:\s*\n)", raw)
    for b in blocks:
        m = re.match(r"\s*([A-Za-z0-9_-]+):", b)
        if not m:
            continue
        if "runs-on" not in b:
            problems.append(f"job '{m.group(1)}' has no runs-on")
    if not any("runs-on" in p for p in problems):
        oks.append("every job names a runner")

    for o in oks:
        print(f"  ok    {o}")
    for p in problems:
        print(f"  FAIL  {p}")

    print(f"\n{'FAILED' if problems else 'PASS'} -- {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
