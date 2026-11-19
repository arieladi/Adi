#!/usr/bin/env python3
"""land_checks.py on sample rollups, no GitHub involved.

    python -I adi_daw/collab/win/tools/test_land_checks.py

Each case is a rollup shaped like `gh pr view N --json statusCheckRollup`
prints it, and the verdict land_top.sh must reach from it.
"""

from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import land_checks  # noqa: E402

ADI = "adi_daw"
UI = "Step 7 UI"
LEGS = [
    "which legs run",
    "validators (py3.9 · ubuntu-24.04)", "validators (py3.11 · ubuntu-latest)",
    "validators (py3.13 · ubuntu-latest)", "validators (py3.13 · macos-latest)",
    "validators (py3.13 · windows-latest)",
    "ubuntu i386 · gcc · ILP32 (32-bit size_t)", "big-endian guard actually fires (s390x)",
    "dependency provenance", "fuzz StreamReader (clang · ASan + UBSan)",
    "JUCE on · macos arm64 · apple clang", "JUCE on · windows x86_64 · msvc",
    "ubuntu x86_64 · gcc · libstdc++", "ubuntu x86_64 · clang · libstdc++",
    "ubuntu arm64 · gcc · libstdc++", "windows x86_64 · msvc · ms stl",
    "zero warnings + hardened libstdc++ (gcc)", "zero warnings + hardened libstdc++ (clang)",
]
GUI = ["GUI shell / windows-latest", "GUI shell / macos-latest", "GUI shell / ubuntu-latest"]
UI_PLAN = "ui: which jobs run"
_job = [1000]


def check(name, conclusion="SUCCESS", workflow=ADI, run=500, started="2026-11-19T10:00:00Z"):
    _job[0] += 1
    return {
        "__typename": "CheckRun",
        "name": name,
        "status": "COMPLETED" if conclusion else "IN_PROGRESS",
        "conclusion": conclusion or "",
        "startedAt": started,
        "completedAt": started if conclusion else None,
        "detailsUrl": f"https://github.com/arieladi/Adi/actions/runs/{run}/job/{_job[0]}",
        "workflowName": workflow,
    }


def full_run(run=500, ui_run=600, gate="SUCCESS", ui=True, **over):
    """Every leg of a pull request run, the gate, and Step 7 UI's run: its
    'ui: which jobs run' and the three GUI shell checks (ui=False: Step 7 UI
    never ran, its paths filter left it out)."""
    out = [check(n, over.get(n, "SUCCESS"), ADI, run) for n in LEGS]
    if gate is not False:
        out.append(check("gate", gate, ADI, run, "2026-11-19T11:00:00Z"))
    if ui:
        out.append(check(UI_PLAN, "SUCCESS", UI, ui_run, "2026-11-19T09:00:00Z"))
        out += [check(n, over.get(n, "SUCCESS"), UI, ui_run) for n in GUI]
    return out


def without(rollup, *names):
    return [c for c in rollup if c["name"] not in names]


def markdown_only_run(run=500, ui_run=600):
    """The validators ran; every leg and the GUI shell skipped before expanding."""
    out = [check(n, "SUCCESS", ADI, run) for n in LEGS[:6]]
    out += [check(n, "SKIPPED", ADI, run) for n in LEGS[6:10]]
    out += [check("matrix.name", "SKIPPED", ADI, run),
            check("JUCE on · ${{ matrix.name }}", "SKIPPED", ADI, run),
            check("gate", "SUCCESS", ADI, run, "2026-11-19T11:00:00Z"),
            check(UI_PLAN, "SUCCESS", UI, ui_run),
            check("GUI shell / ${{ matrix.os }}", "SKIPPED", UI, ui_run)]
    return out


def draft_run(run=400, ui_run=450):
    """A draft's run: plan, validators and the gate all skipped."""
    return [check("which legs run", "SKIPPED", ADI, run),
            check("validators (py${{ matrix.python }} · ${{ matrix.os }})", "SKIPPED", ADI, run),
            check("matrix.name", "SKIPPED", ADI, run),
            check("gate", "SKIPPED", ADI, run),
            check(UI_PLAN, "SKIPPED", UI, ui_run),
            check("GUI shell / ${{ matrix.os }}", "SKIPPED", UI, ui_run)]


def legacy_run(n_ok=21, run=300):
    """A run of the workflow before the gate existed: no gate check at all."""
    out = [check(n, "SUCCESS", ADI, run) for n in LEGS]
    out += [check(n, "SUCCESS", UI, run + 1) for n in GUI]
    return out[:n_ok]


def cases():
    yield "all green", full_run(), "LAND"
    yield "Markdown only: validators and the gate", markdown_only_run(), "LAND"
    yield "gate still running", full_run(gate=None), "WAIT"
    yield "a leg still running", full_run(**{"windows x86_64 · msvc · ms stl": None}), "WAIT"
    yield "a GUI shell still running", full_run(**{GUI[1]: None}), "WAIT"

    r = full_run(gate="FAILURE", **{"ubuntu x86_64 · gcc · libstdc++": "FAILURE"})
    yield "a leg failed: the leg is named, not the gate", r, "FAILED"

    r = full_run(gate="FAILURE", **{"JUCE on · macos arm64 · apple clang": "CANCELLED"})
    yield "a leg cancelled: rerun it, the gate's failure is its echo", r, "CANCELLED"

    yield "the gate failed and nothing says why", full_run(gate="FAILURE"), "GATE"
    yield "gate green, a GUI shell failed", full_run(**{GUI[0]: "FAILURE"}), "FAILED"
    yield "gate green, a GUI shell cancelled", full_run(**{GUI[2]: "CANCELLED"}), "CANCELLED"
    yield "gate green, a GUI shell neutral", full_run(**{GUI[2]: "NEUTRAL"}), "WAIT"
    yield "gate green, Step 7 UI never ran", full_run(ui=False), "LAND"

    # Review of 19 November: Step 7 UI has no gate, and a check GitHub has not
    # created yet is absent, not pending. "Every GUI shell check passed" is
    # true of none, so these landed with the GUI shell never built.
    yield "Step 7 UI planned, no GUI shell check created yet", without(full_run(), *GUI), "WAIT"
    yield "one GUI shell check of three", without(full_run(), GUI[0], GUI[1]), "WAIT"
    yield "two GUI shell checks of three", without(full_run(), GUI[2]), "WAIT"
    r = without(full_run(), *GUI) + [check(n, "SKIPPED", UI, 600) for n in GUI]
    yield "three GUI shell checks, all skipped", r, "LAND"
    r = without(full_run(), *GUI) + [check("GUI shell / ${{ matrix.os }}", "SKIPPED", UI, 600)]
    yield "Markdown only on Step 7 UI, the legs built on adi_daw", r, "LAND"
    r = without(full_run(), UI_PLAN, *GUI) + [check("GUI shell / ${{ matrix.os }}", "SKIPPED", UI, 600)]
    yield "a skipped placeholder with no 'ui: which jobs run' beside it", r, "WAIT"
    r = without(full_run(), *GUI) + [check("GUI shell / ${{ matrix.os }}", "CANCELLED", UI, 600)]
    yield "a cancelled placeholder is not the Markdown answer", r, "WAIT"
    # Marked ready: adi_daw's ready run is green, but Step 7 UI's newest run is
    # still the draft's (every job skipped). That run says nothing about this one.
    r = full_run(run=700, ui=False) + [
        check(UI_PLAN, "SKIPPED", UI, 450),
        check("GUI shell / ${{ matrix.os }}", "SKIPPED", UI, 450)]
    yield "adi_daw ready and green, Step 7 UI's newest run still the draft's", r, "WAIT"
    r = without(full_run(), UI_PLAN)
    yield "a Step 7 UI run from before 'ui: which jobs run' existed", r, "LAND"

    # Review of 19 November: console-plugin.yml and midi-helper.yml both name
    # their jobs "build (<os>)". Keyed by name, the later pass hid the failure.
    r = full_run() + [
        check("build (windows-latest)", "FAILURE", "console-plugin", 800, "2026-11-19T10:00:00Z"),
        check("build (windows-latest)", "SUCCESS", "midi-helper", 810, "2026-11-19T10:30:00Z")]
    yield "one workflow's build failed, another's of the same name passed later", r, "FAILED"
    r = full_run() + [
        check("build (macos-latest)", "CANCELLED", "midi-helper", 810, "2026-11-19T10:00:00Z"),
        check("build (macos-latest)", "SUCCESS", "console-plugin", 800, "2026-11-19T10:30:00Z")]
    yield "one workflow's build cancelled, another's of the same name passed later", r, "CANCELLED"
    r = full_run() + [
        check("build (ubuntu-latest)", None, "console-plugin", 800, "2026-11-19T10:00:00Z"),
        check("build (ubuntu-latest)", "SUCCESS", "midi-helper", 810, "2026-11-19T10:30:00Z")]
    yield "one workflow's build still running, another's of the same name passed", r, "WAIT"
    r = full_run() + [
        check("build (windows-latest)", "SUCCESS", "console-plugin", 800),
        check("build (windows-latest)", "SUCCESS", "midi-helper", 810)]
    yield "both workflows' builds of the same name passed", r, "LAND"

    yield "a draft's run is never a pass", draft_run(), "WAIT"
    r = draft_run() + [check("which legs run", None, ADI, 700, "2026-11-19T12:00:00Z")]
    yield "marked ready: the new run is the one that counts", r, "WAIT"
    r = draft_run() + full_run(run=700, ui_run=701)
    yield "marked ready and green", r, "LAND"

    # land_queue.sh's refresh: run 500 was green, run 700 has finished every leg
    # but its gate has not been created yet. The old green gate must not count.
    r = full_run(run=500) + full_run(run=700, ui_run=701, gate=False)
    yield "a refresh whose gate has not appeared", r, "LEGACY"
    r = full_run(run=500, gate="FAILURE", **{"dependency provenance": "FAILURE"}) \
        + full_run(run=700, ui_run=701)
    yield "an older run's failure is not this run's", r, "LAND"

    r = full_run(gate="FAILURE", **{"ubuntu arm64 · gcc · libstdc++": "FAILURE"})
    r.append(check("ubuntu arm64 · gcc · libstdc++", "SUCCESS", ADI, 500, "2026-11-19T12:00:00Z"))
    r.append(check("gate", "SUCCESS", ADI, 500, "2026-11-19T12:05:00Z"))
    yield "a rerun attempt replaces the failed one", r, "LAND"
    r = full_run(gate="FAILURE", **{"ubuntu arm64 · gcc · libstdc++": "FAILURE"})
    q = check("ubuntu arm64 · gcc · libstdc++", None, ADI, 500, None)
    r.append(q)
    yield "a queued rerun with no start time still waits", r, "WAIT"

    r = full_run() + [check("matrix.name", "CANCELLED", ADI, 500, "2026-11-19T09:00:00Z")]
    yield "an unexpanded name is not a cancellation", r, "LAND"

    yield "an old run without a gate, 21 passed", legacy_run(21), "LEGACY"
    yield "an old run without a gate, 18 passed", legacy_run(18), "WAIT"
    r = legacy_run(21)
    r[3]["conclusion"] = "FAILURE"
    yield "an old run without a gate, one failed", r, "FAILED"
    # Review of 19 November: the old count is no answer about the GUI shell.
    # 21 passed legs, Step 7 UI planned, and not one GUI shell check created
    # yet: LEGACY landed it with the shell unbuilt.
    r = [check(n, "SUCCESS", ADI, 300) for n in LEGS + ["extra leg 1", "extra leg 2", "extra leg 3"]]
    r.append(check(UI_PLAN, "SUCCESS", UI, 301))
    yield "an old run without a gate, 21 passed, Step 7 UI planned, no GUI shell yet", r, "WAIT"
    yield "no checks at all", [], "WAIT"


def workflow_names() -> list[tuple[str, bool]]:
    """land_checks.py's names against the workflow files that produce them. A
    renamed workflow or job, or a fourth GUI shell, would otherwise make every
    landing WAIT forever, or worse, stop looking."""
    root = HERE.parents[3]
    ci = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    ui = (root / ".github/workflows/ui.yml").read_text(encoding="utf-8")

    def top_name(txt):
        m = re.search(r"^name:\s*['\"]?(.+?)['\"]?\s*$", txt, re.M)
        return m.group(1) if m else None

    def job_names(txt):
        return [m.group(1) for m in re.finditer(r"^    name:\s*['\"]?(.+?)['\"]?\s*$", txt, re.M)]

    shell = re.search(r"^  shell:\n(?:    .*\n|\s*\n)*?\s+os:\s*\[([^\]]*)\]", ui, re.M)
    oses = [s.strip() for s in shell.group(1).split(",") if s.strip()] if shell else []
    return [
        (f"ci.yml is the workflow {land_checks.GATE_WORKFLOW!r}",
         top_name(ci) == land_checks.GATE_WORKFLOW),
        (f"ci.yml has a job named {land_checks.GATE!r}", land_checks.GATE in job_names(ci)),
        (f"ui.yml is the workflow {land_checks.GUI_WORKFLOW!r}",
         top_name(ui) == land_checks.GUI_WORKFLOW),
        (f"ui.yml has a job named {land_checks.GUI_PLAN!r}", land_checks.GUI_PLAN in job_names(ui)),
        (f"ui.yml's shell job is named {land_checks.GUI_PREFIX!r}...",
         any(n.startswith(land_checks.GUI_PREFIX) for n in job_names(ui))),
        (f"ui.yml's shell matrix has GUI_SHELLS = {land_checks.GUI_SHELLS} systems "
         f"(found {oses})", len(oses) == land_checks.GUI_SHELLS),
    ]


def main() -> int:
    fails = 0
    for label, good in workflow_names():
        fails += not good
        print(f"  {'ok  ' if good else 'FAIL'}  {label}")
    for label, rollup, want in cases():
        got = land_checks.decide(rollup)[0]
        mark = "ok  " if got == want else "FAIL"
        fails += got != want
        print(f"  {mark}  {label}: {got}" + ("" if got == want else f" (wanted {want})"))

    # And through stdin, exactly as land_top.sh calls it.
    out = subprocess.run([sys.executable, "-I", str(HERE / "land_checks.py")],
                         input=json.dumps({"statusCheckRollup": full_run()}),
                         capture_output=True, text=True, encoding="utf-8")
    fields = out.stdout.rstrip("\n").split("\t")
    if out.returncode != 0 or len(fields) != 7 or fields[0] != "LAND" or fields[5] != "SUCCESS":
        print(f"  FAIL  stdin round trip: rc={out.returncode} {fields!r} {out.stderr.strip()}")
        fails += 1
    else:
        print("  ok    stdin round trip: seven tab-separated fields, LAND first")
    bad = subprocess.run([sys.executable, "-I", str(HERE / "land_checks.py")],
                         input="", capture_output=True, text=True)
    if bad.returncode == 0:
        print("  FAIL  empty input (gh failed) must exit non-zero so land_top.sh retries")
        fails += 1
    else:
        print("  ok    empty input exits non-zero")

    print(f"\n{'FAILED' if fails else 'PASS'} -- {fails} problem(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
