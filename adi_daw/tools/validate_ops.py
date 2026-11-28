#!/usr/bin/env python3
"""Validate the op catalogue in docs/OPS.md against the contracts in OPS.md 1-8.

    python adi_daw/tools/validate_ops.py [OPS.md]

The catalogue is a spec, not prose, so its invariants get checked rather than
asserted -- the same reason tools/validate_schema.py exists. An earlier draft of
OPS.md claimed 152 ops when the tables held 174. The optional argument checks
another copy of OPS.md, so a row broken on purpose in a copy shows check 1
failing without touching the real one.

Checks:
  1. Every catalogue row parses into (name, scope, engine, inverse, priority),
     and every line that looks like an op row is one that parses.
  2. Names are unique and match the rule in OPS.md 3, invariant 4.
  3. Every op has an inverse, or is explicitly non-undoable.
  4. Non-undoable ops live only in the ephemeral scopes (OPS.md 10).
  5. RequiresPause stays a small, named set (OPS.md 4).
  6. Coalescable ops are symmetric -- coalescing a state-capture inverse would
     silently discard intermediate state.
  7. The counts are the tables' own. They are printed, never compared with a
     number in the prose: OPS.md keeps none (see check 7 below).
  8. Media ops are registered and retired embedding ops stay struck/unimplemented.
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
# What an op row looks like, loosely: a table line whose first cell is a
# backticked dotted name, however the rest of it is spelled. ROW above is
# strict, so a malformed row (no closing '|', a scope letter that is not one,
# P4) does not match it and simply drops out of every count. While OPS.md wrote
# its total down, the prose count was what noticed; it no longer writes one
# (check 7), so check 1 now fails any such line that ROW cannot parse.
# Leading whitespace is allowed in all of these: Markdown still renders " | ..."
# as a table row, and anchored at '^\|' such a row dropped out of every count
# with no failure (and ended the op-table tracking below for the rows after it).
OP_LIKE = re.compile(r"^\s*\|\s*`[A-Za-z][^`|]*\.[^`|]*`\s*\|")
# The header every op table has; a row in such a table is an op row whatever
# its first cell holds (a name that lost its backticks still counts).
OP_TABLE_HEADER = re.compile(r"^\s*\|\s*Op\s*\|\s*S\s*\|\s*E\s*\|\s*Inv\s*\|\s*P\s*\|\s*$")
# A retired op is struck through (check 8) and has no scope or priority.
STRUCK = re.compile(r"^\s*\|\s*~~`")
SEPARATOR = re.compile(r"^\s*\|[\s:|-]+$")


def unparsed_op_rows(txt: str) -> list[tuple[int, str]]:
    """Every line that is an op row by its look or by its table, and that ROW
    does not parse, as (line number, line)."""
    out = []
    in_table = False
    for no, line in enumerate(txt.splitlines(), 1):
        if OP_TABLE_HEADER.match(line):
            in_table = True
            continue
        is_row = line.lstrip().startswith("|")
        if not is_row:
            in_table = False
        if SEPARATOR.match(line):
            continue
        in_op_table = in_table and is_row and not STRUCK.match(line)
        if (OP_LIKE.match(line) or in_op_table) and not ROW.match(line):
            out.append((no, line))
    return out

# OPS.md 4: every member is a stall the user hears, so the set is named here and
# a new one has to be added deliberately in two places.
ALLOWED_PAUSE = {"project.setSampleRate"}

# ADR-0101 d4 restores the non-undoable Session scope from ADR-0027.
EPHEMERAL_SCOPES = {"t", "s"}


def main(argv: list[str]) -> int:
    ops = pathlib.Path(argv[0]) if argv else OPS
    if not ops.exists():
        print(f"not found: {ops}")
        return 2

    txt = ops.read_text(encoding="utf-8")
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
    unparsed = unparsed_op_rows(txt)
    for no, line in unparsed:
        fail(f"OPS.md line {no} is an op row that does not parse, so no check "
             f"and no count sees it -- want | `domain.verb` | e/t/s/h | N/S/G/P "
             f"| inverse | P0-P3 |: {line}")
    if not unparsed:
        ok("every line that looks like an op row, or sits in an op table, parses")

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
        fail(f"non-undoable ops outside the transport and Session scopes: {stray}")
    else:
        ok(f"{len(nonundo)} non-undoable ops in the transport and Session scopes")

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

    # Counted, never compared (2026-11-19, win). OPS.md's headline used to say
    # "**211 ops** -- 71 P0, ..." and this check failed when the tables
    # disagreed. Every pull request that added an op rewrote that one line to
    # the same new number, so two of them merge cleanly one after the other
    # and leave main red; on 19 November four open pull requests had all
    # written "212". The number now lives here, computed from the tables, and
    # the headline says so. A count written back into the prose fails, because the next merge
    # would make it wrong without anyone touching it.
    print("[7] the op count is computed from the tables, not read from the prose")
    by_prio = collections.Counter(p for *_x, p in rows)
    m = re.search(r"\*\*(\d+) ops\*\*", txt)
    if m:
        fail(f"OPS.md states a count ('**{m.group(1)} ops**'): remove it -- "
             f"the count is this check's output, and a written one goes stale "
             f"with the next merge")
    else:
        ok(f"{len(rows)} ops: " + ", ".join(
            f"{by_prio[p]} {p}" for p in sorted(by_prio)))

    print("[8] media implementation and retired embedding ops (ADR-0143)")
    catalog = (HERE.parent / "src/adi/ops_catalog.cpp").read_text(encoding="utf-8")
    for name in ("media.import", "media.unlink", "media.relink"):
        if name not in names or not re.search(r'\{"' + re.escape(name) + r'",', catalog):
            fail(f"media op missing from catalogue or implementation: {name}")
        else:
            ok(f"{name} documented and implemented")
    for name in ("media.embed", "media.extract"):
        if name in names or f'"{name}"' in catalog or f"~~`{name}`~~" not in txt:
            fail(f"retired embedding op must be struck and never implemented: {name}")
        else:
            ok(f"{name} retired (ADR-0127)")

    print()
    print(f"  scopes:  {dict(collections.Counter(s for _n, s, *_x in rows))}")
    print(f"  engine:  {dict(collections.Counter(e.strip('*') for _n, _s, e, *_x in rows))}")
    print(f"  priority: {dict(sorted(by_prio.items()))}")
    print()
    print(f"{'FAILED' if problems else 'PASS'} -- {problems} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
