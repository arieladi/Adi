#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""WTS-09: parity bookkeeping that cannot drift (docs/PLAN.md; ADR-0108 d1).

Reads every collab/window/parity-*.md table and the UI tests, and fails when:
  - a row's cells do not match its table's header, or its id is malformed or used twice in its sheet;
  - a status cell does not start with one of the words parity-7.2-shell.md defines;
  - a [P:<sheet>#<id>] tag in tests/ names no row;
  - more built rows are untagged than the floor allows (the count only falls);
  - a sheet holds fewer built rows than its floor (a floor changes with the sheet, never silently).

Usage: validate_parity.py [--root adi_daw] [--write-floors] [--self-test]
The floors live beside this script in parity-floors.json. The window lane drafted it; it moves into
tools/ when win lends the path and wires it into ci.yml's validator step.
"""
import argparse
import json
import pathlib
import re
import sys
import tempfile

STATUSES = ("built", "partial", "open", "measure", "waits", "withdrawn")
ID = re.compile(r"^[A-Z]?\d+[a-z]?$")
TAG = re.compile(r"\[P:([A-Za-z0-9.\-]+)#([A-Za-z0-9]+)\]")


def cells(line):
    """A table row's cells, `\\|` kept as a literal pipe."""
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|") and not body.endswith("\\|"):
        body = body[:-1]
    return [c.replace("\\|", "|").strip() for c in re.split(r"(?<!\\)\|", body)]


def sheets(root):
    out = {}
    for path in sorted((root / "collab" / "window").glob("parity-*.md")):
        out[path.stem[len("parity-"):]] = path
    return out


def read_sheet(path, errors):
    """{id: status word} for the sheet's rows, in order; problems appended to errors."""
    rows, width, name = {}, None, path.name
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("|"):
            width = None if not line.strip() else width
            continue
        c = cells(line)
        if width is None:  # a header
            width = len(c)
            continue
        if set(line.replace("|", "").strip()) <= set("-: "):
            continue  # the rule under the header
        where = f"{name}:{number}"
        if len(c) != width:
            errors.append(f"{where}: {len(c)} cells where the header has {width} (an unescaped | ?)")
            continue
        rid = c[0].strip("~* ")
        if not ID.match(rid):
            errors.append(f"{where}: row id {c[0]!r} is not a number, a lettered number or a section letter and number")
            continue
        if rid in rows:
            errors.append(f"{where}: row id {rid} is used twice")
        word = re.match(r"[~*]*([A-Za-z]+)", c[-1])
        status = word.group(1).lower() if word else ""
        if status not in STATUSES:
            errors.append(f"{where}: row {rid}'s status starts {c[-1][:30]!r}, not one of {', '.join(STATUSES)}")
        rows[rid] = status
    return rows


def tags(root):
    found = []
    for path in sorted((root / "tests").glob("*.cpp")) + sorted((root / "tests").glob("*.hpp")):
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for m in TAG.finditer(line):
                found.append((f"{path.name}:{number}", m.group(1), m.group(2)))
    return found


def check(root, floors_path, write_floors=False):
    errors = []
    table = {name: read_sheet(path, errors) for name, path in sheets(root).items()}
    tagged = set()
    for where, sheet, rid in tags(root):
        if sheet not in table or rid not in table[sheet]:
            errors.append(f"{where}: [P:{sheet}#{rid}] names no parity row")
        else:
            tagged.add((sheet, rid))
    built = {name: sorted(r for r, s in rows.items() if s == "built") for name, rows in table.items()}
    untagged = sum(1 for name, ids in built.items() for r in ids if (name, r) not in tagged)
    counts = {name: len(ids) for name, ids in built.items()}
    if write_floors:
        floors_path.write_text(json.dumps({"untagged_built": untagged, "built": counts}, indent=2, sort_keys=True) + "\n")
    elif floors_path.exists():
        floors = json.loads(floors_path.read_text())
        if untagged > floors.get("untagged_built", untagged):
            errors.append(f"{untagged} built rows have no [P:] tag, more than the floor's {floors['untagged_built']}: "
                          "tag the new rows' checks, or lower nothing")
        for name, least in floors.get("built", {}).items():
            if counts.get(name, 0) < least:
                errors.append(f"parity-{name}.md holds {counts.get(name, 0)} built rows, fewer than its floor of {least}: "
                              "change the floor with the sheet, saying why")
    return errors, untagged, counts


def self_test(root, floors_path):
    """AT-19: the script passes on the tree as it is and fails on a planted bad tag."""
    errors, _, _ = check(root, floors_path)
    if errors:
        print("self-test: the tree itself fails:\n  " + "\n  ".join(errors))
        return False
    with tempfile.TemporaryDirectory() as temp:
        planted = pathlib.Path(temp)
        (planted / "collab").mkdir()
        (planted / "collab" / "window").symlink_to((root / "collab" / "window").resolve(), target_is_directory=True)
        (planted / "tests").mkdir()
        (planted / "tests" / "planted.cpp").write_text('check(true, "a planted tag [P:7.6-mixer#999]");\n')
        bad, _, _ = check(planted, floors_path)
        if not any("[P:7.6-mixer#999]" in e for e in bad):
            print("self-test: a planted bad tag passed")
            return False
        (planted / "tests" / "planted.cpp").write_text('check(true, "a good tag [P:7.6-mixer#1]");\n')
        good, _, _ = check(planted, floors_path)
        if any("names no parity row" in e for e in good):
            print("self-test: a good tag failed: " + "; ".join(good))
            return False
    print("self-test: passes on the tree, fails on a planted bad tag")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    here = pathlib.Path(__file__).resolve().parent
    parser.add_argument("--root", default=str(here.parent.parent), help="the adi_daw directory")
    parser.add_argument("--floors", default=str(here / "parity-floors.json"))
    parser.add_argument("--write-floors", action="store_true", help="write today's counts as the floors")
    parser.add_argument("--self-test", action="store_true")
    a = parser.parse_args()
    root, floors = pathlib.Path(a.root), pathlib.Path(a.floors)
    if a.self_test:
        return 0 if self_test(root, floors) else 1
    errors, untagged, counts = check(root, floors, a.write_floors)
    for e in errors:
        print(e)
    total = sum(counts.values())
    print(f"parity: {len(counts)} sheets, {total} built rows, {untagged} untagged; "
          + ("FAIL" if errors else "clean"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
