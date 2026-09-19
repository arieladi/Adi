#!/usr/bin/env python3
"""Validate the op catalogue in docs/OPS.md against the contracts in OPS.md 1-8.

    python adi_daw/tools/validate_ops.py

The catalogue is a spec, not prose, so its invariants get checked rather than
asserted -- the same reason tools/validate_schema.py exists. An earlier draft of
OPS.md claimed 152 ops when the tables held 174.

Checks:
  1. Every catalogue row parses into (name, scope, engine, inverse, priority).
  2. Names are unique and match the rule in OPS.md 3, invariant 4.
  3. Every op has an inverse, or is explicitly non-undoable.
  4. Non-undoable ops live only in the ephemeral scopes (OPS.md 10).
  5. RequiresPause stays a small, named set (OPS.md 4).
  6. Coalescable ops are symmetric -- coalescing a state-capture inverse would
     silently discard intermediate state.
  7. The headline count in the prose matches the tables.
"""

from __future__ import annotations

import collections
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
OPS = HERE.parent / "docs" / "OPS.md"

NAME_RULE = re.compile(r"^[a-z][a-zA-Z0-9]*\.[a-z][a-zA-Z0-9]*$")
ROW = re.compile(
    r"^\| `([a-z][a-zA-Z0-9]*\.[a-zA-Z0-9]+)` \| ([etsh]) \| (\*\*P\*\*|[NSGP]) \| ([^|]+?) \| (P[0-3]) \|$",
    re.M,
)

# OPS.md 4: every member is a stall the user hears, so the set is named here and
# a new one has to be added deliberately in two places.
ALLOWED_PAUSE = {"project.setSampleRate"}

# "s" (session) went with Session View in ADR-0037; transport is all that is
# left that persists nothing. Kept as a set so adding a scope stays a one-liner.
EPHEMERAL_SCOPES = {"t"}


def main() -> int:
    if not OPS.exists():
        print(f"not found: {OPS}")
        return 2

    txt = OPS.read_text(encoding="utf-8")
    rows = ROW.findall(txt)
    problems = 0

    def fail(msg: str) -> None:
        nonlocal problems
        print(f"  FAIL  {msg}")
        problems += 1

    def ok(msg: str) -> None:
        print(f"  ok    {msg}")

    print("[1] catalogue parses")
    if not rows:
        fail("no op rows matched -- did the table format change?")
        return 1
    ok(f"{len(rows)} op rows parsed")

    names = [r[0] for r in rows]

    print("[2] names")
    dupes = sorted(n for n, c in collections.Counter(names).items() if c > 1)
    if dupes:
        fail(f"duplicate op names: {dupes}")
    else:
        ok("no duplicate names")
    bad = [n for n in names if not NAME_RULE.match(n)]
    if bad:
        fail(f"names violating the domain.verb rule: {bad}")
    else:
        ok("all names match ^[a-z][a-zA-Z0-9]*\\.[a-z][a-zA-Z0-9]*$")

    print("[3] inverses")
    missing = [n for n, _s, _e, inv, _p in rows if not inv.strip()]
    if missing:
        fail(f"ops with no inverse cell: {missing}")
    else:
        ok("every op declares an inverse or is explicitly non-undoable")

    print("[4] non-undoable ops")
    nonundo = [(n, s) for n, s, _e, inv, _p in rows if inv.strip() == "—"]
    stray = [n for n, s in nonundo if s not in EPHEMERAL_SCOPES]
    if stray:
        fail(f"non-undoable ops outside the transport scope: {stray}")
    else:
        ok(f"{len(nonundo)} non-undoable ops, all in the transport scope")

    print("[5] RequiresPause stays small")
    pause = {n for n, _s, e, _i, _p in rows if e.strip("*") == "P"}
    unexpected = sorted(pause - ALLOWED_PAUSE)
    if unexpected:
        fail(
            f"RequiresPause ops not in the allow-list: {unexpected} "
            f"-- every one is a stall the user hears; justify it in OPS.md 4 "
            f"and add it to ALLOWED_PAUSE"
        )
    else:
        ok(f"RequiresPause = {sorted(pause) or '[]'}")

    print("[6] coalescable ops are symmetric")
    # Coalescing keeps the FIRST op's inverse (OPS.md 6.4). If the inverse were a
    # state capture, merging would discard the intermediate states it captured.
    bad_coalesce = [
        n for n, _s, _e, inv, _p in rows if "coalescable" in inv and "sym" not in inv
    ]
    if bad_coalesce:
        fail(f"coalescable ops without a symmetric inverse: {bad_coalesce}")
    else:
        n_coal = sum(1 for *_x, inv, _p in rows if "coalescable" in inv)
        ok(f"{n_coal} coalescable ops, all symmetric")

    print("[7] headline count matches the tables")
    m = re.search(r"\*\*(\d+) ops\*\*", txt)
    if not m:
        fail("no '**N ops**' headline found in the prose")
    elif int(m.group(1)) != len(rows):
        fail(f"prose says {m.group(1)} ops, tables contain {len(rows)}")
    else:
        ok(f"prose and tables agree: {len(rows)}")

    by_prio = collections.Counter(p for *_x, p in rows)
    for label, claimed in re.findall(r"(\d+) (P[0-3])", txt[m.start():m.start() + 120] if m else ""):
        if by_prio[claimed] != int(label):
            fail(f"prose claims {label} {claimed}, tables have {by_prio[claimed]}")

    print()
    print(f"  scopes:  {dict(collections.Counter(s for _n, s, *_x in rows))}")
    print(f"  engine:  {dict(collections.Counter(e.strip('*') for _n, _s, e, *_x in rows))}")
    print(f"  priority: {dict(sorted(by_prio.items()))}")
    print()
    print(f"{'FAILED' if problems else 'PASS'} -- {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
