#!/usr/bin/env python3
"""What land_top.sh does next with a pull request, read from its checks.

    gh pr view N --repo arieladi/Adi --json statusCheckRollup | python -I land_checks.py

Prints one tab-separated line, the verdict first:

    VERDICT  pending  bad-urls  cancelled-urls  legacy-ok  gate  gate-url

    WAIT       something still runs, or nothing decisive has arrived yet
    FAILED     a check failed; bad-urls are its job URLs, so land_top.sh can read
               each log for a runner fault and rerun once
    CANCELLED  nothing failed and nothing runs, but a check was cancelled;
               land_top.sh reruns it once
    GATE       adi_daw's "gate" concluded without SUCCESS and no other check
               says why: land_top.sh stops and names it
    LAND       "gate" is SUCCESS, every "GUI shell / ..." check is SUCCESS or
               SKIPPED, nothing runs and nothing failed
    LEGACY     the newest adi_daw run has no "gate" check (a run from before
               the gate existed): the old rule, nothing runs, nothing failed and
               at least 20 checks passed; land_top.sh wants it twice in a row

Pure stdlib and no network, so test_land_checks.py runs it on sample rollups
without GitHub. It was a jq expression inside land_top.sh until 19 November;
it moved here so the decision can be tested.

How the checks are read:
  - ONLY EACH WORKFLOW'S NEWEST RUN COUNTS. A pull request closed and reopened
    (land_queue.sh's refresh) or dispatched by hand carries the older run's
    checks on the same head, and that run's gate says nothing about this one.
    Run ids grow, and each check's details URL carries its run's id.
  - Within that run the newest attempt of each check counts: a rerun replaces.
  - A name GitHub never expanded ("matrix.name", or one still holding "${{")
    belongs to a matrix job that was skipped or cancelled before its matrix
    existed. It is never a pass, a failure or a cancellation of its own; the
    gate, which needs that job, reports what it means.
  - "gate" is left out of bad-urls and cancelled-urls: it fails BECAUSE a leg
    failed or was cancelled, and the leg is the one to read or rerun.
"""

from __future__ import annotations

import json
import re
import sys

GATE = "gate"
GATE_WORKFLOW = "adi_daw"
GUI_PREFIX = "GUI shell / "
LEGACY_MIN_OK = 20
FAILED = {"FAILURE", "TIMED_OUT", "STARTUP_FAILURE", "ERROR", "ACTION_REQUIRED"}
PASSED = {"SUCCESS", "SKIPPED"}
RUN_ID = re.compile(r"/runs/(\d+)")


def placeholder(name: str) -> bool:
    return name == "matrix.name" or "matrix." in name or "${{" in name


def normalise(c: dict) -> dict:
    """One rollup entry as name, conclusion (None while it runs), workflow, run, when."""
    if c.get("__typename") == "StatusContext" or ("context" in c and "name" not in c):
        state = (c.get("state") or "").upper()
        concl = None if state in ("", "PENDING", "EXPECTED") else state
        return {"name": c.get("context") or "", "conclusion": concl, "workflow": "",
                "run": None, "url": c.get("targetUrl") or "",
                "when": c.get("startedAt") or c.get("createdAt") or ""}
    status = (c.get("status") or "").upper()
    concl = (c.get("conclusion") or "").upper() or None
    if status and status != "COMPLETED":
        concl = None
    url = c.get("detailsUrl") or ""
    m = RUN_ID.search(url)
    # A check that has not started may have no startedAt yet; it still sorts as
    # the newest, so a queued rerun is never hidden behind its finished attempt.
    when = c.get("startedAt") or c.get("completedAt") or ("~" if concl is None else "")
    return {"name": c.get("name") or "", "conclusion": concl,
            "workflow": c.get("workflowName") or "", "run": int(m.group(1)) if m else None,
            "url": url, "when": when}


def decide(rollup: list[dict]) -> list[str]:
    checks = [normalise(c) for c in rollup]

    newest: dict[str, int] = {}
    for c in checks:
        if c["run"] is not None:
            newest[c["workflow"]] = max(newest.get(c["workflow"], c["run"]), c["run"])
    current = [c for c in checks if c["run"] is None or c["run"] == newest.get(c["workflow"])]

    # Newest attempt per name; on a tie the later entry wins (jq's sort_by + last).
    latest: dict[str, dict] = {}
    for c in current:
        prev = latest.get(c["name"])
        if prev is None or c["when"] >= prev["when"]:
            latest[c["name"]] = c
    rows = list(latest.values())
    named = [c for c in rows if not placeholder(c["name"])]

    pending = sum(1 for c in rows if c["conclusion"] is None)
    bad = [c["url"] for c in named if c["name"] != GATE and c["conclusion"] in FAILED]
    cancelled = [c["url"] for c in named if c["name"] != GATE and c["conclusion"] == "CANCELLED"]
    legacy_ok = sum(1 for c in named if c["conclusion"] in PASSED)

    gate = next((c for c in named if c["name"] == GATE
                 and c["workflow"] in (GATE_WORKFLOW, "")), None)
    gate_state = "none" if gate is None else (gate["conclusion"] or "PENDING")
    gate_url = "" if gate is None else gate["url"]
    if gate is not None and gate["conclusion"] == "CANCELLED":
        cancelled.append(gate["url"])  # the whole run was cancelled: rerun it
    gui_ok = all(c["conclusion"] in PASSED for c in named if c["name"].startswith(GUI_PREFIX))

    if pending:
        verdict = "WAIT"
    elif bad:
        verdict = "FAILED"
    elif cancelled:
        verdict = "CANCELLED"
    elif gate is None:
        verdict = "LEGACY" if legacy_ok >= LEGACY_MIN_OK else "WAIT"
    elif gate["conclusion"] == "SUCCESS":
        # A GUI shell check that is neither a pass, a failure nor a cancellation
        # (NEUTRAL, STALE) is not a pass: wait for a run that settles it.
        verdict = "LAND" if gui_ok else "WAIT"
    elif gate["conclusion"] == "SKIPPED":
        # The newest run was a draft's, which builds nothing. land_top.sh marks
        # the pull request ready, and that run is the one to wait for.
        verdict = "WAIT"
    else:
        verdict = "GATE"

    return [verdict, str(pending), " ".join(bad), " ".join(cancelled),
            str(legacy_ok), gate_state, gate_url]


def main() -> int:
    raw = sys.stdin.read()
    try:
        data = json.loads(raw)
    except ValueError:
        print("land_checks.py: no JSON on stdin (gh failed?)", file=sys.stderr)
        return 2
    rollup = data.get("statusCheckRollup") if isinstance(data, dict) else data
    if not isinstance(rollup, list):
        print("land_checks.py: no statusCheckRollup list in the input", file=sys.stderr)
        return 2
    print("\t".join(decide(rollup)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
