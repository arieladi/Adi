#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prove that what THIS build compiled matches what SPEC 6.3 published.
#
# `adi_tests` proves the struct layouts are self-consistent, and blob.hpp's
# static_asserts prove they equal a set of numbers hard-coded in a header. Neither
# proves those numbers are the ones in docs/format/SPEC.md — and SPEC.md is the
# source of truth a third-party implementer reads. This script closes that gap: it
# parses the sizes out of the spec prose, parses them out of `adi_tool layout`, and
# fails if they disagree.
#
# It exists because this project has twice published a confident number that was
# false (a "152 ops" claim against 174 rows; a "960 PPQ cannot express a
# quintuplet" claim that it can). The standing rule is: if you write a number into
# a doc, add it to a validator. These four numbers had no validator.
#
#   python3 .github/scripts/check_spec_layout.py <path-to-adi_tool>

import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
SPEC = HERE.parent.parent / "adi_daw" / "docs" / "format" / "SPEC.md"

# struct name in `adi_tool layout` -> how SPEC 6.3 states its size
SPEC_PATTERNS = {
    "StreamHeader": (
        r"Every core-tier BLOB begins with a \*\*(\d+)-byte stream header\*\*",
        "SPEC 6.3 prose",
    ),
    "NoteRecord": (
        r"^#### 6\.3\.\d+ `ANOT`.*?,\s*(\d+) bytes\s*$",
        "SPEC 6.3.1 heading",
    ),
    "ExpressionPoint": (
        r"^#### 6\.3\.\d+ `AEXP`.*?,\s*(\d+) bytes\s*$",
        "SPEC 6.3.2 heading",
    ),
    "AutomationPoint": (
        r"^#### 6\.3\.\d+ `AAUT`.*?,\s*(\d+) bytes\s*$",
        "SPEC 6.3.3 heading",
    ),
}

# `adi_tool layout` line:  "  NoteRecord          40 bytes  (ANOT)"
TOOL_LINE = re.compile(r"^\s{2}(\w+)\s+(\d+) bytes")


def fail(msg):
    print("  FAIL  " + msg)
    return 1


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        print("usage: check_spec_layout.py <path-to-adi_tool>", file=sys.stderr)
        return 2

    tool = pathlib.Path(argv[1])
    if not tool.is_file():
        print("no such executable: %s" % tool, file=sys.stderr)
        return 2
    if not SPEC.is_file():
        print("cannot find the spec at %s" % SPEC, file=sys.stderr)
        return 2

    spec_text = SPEC.read_text(encoding="utf-8")

    print("[1] sizes published in %s" % SPEC.name)
    published = {}
    problems = 0
    for struct, (pattern, where) in SPEC_PATTERNS.items():
        hits = re.findall(pattern, spec_text, re.MULTILINE)
        if len(hits) != 1:
            problems += fail(
                "%s: expected exactly one size in %s, found %d. The spec's wording "
                "changed and this validator no longer reads it — fix the validator, "
                "do not delete the check." % (struct, where, len(hits))
            )
            continue
        published[struct] = int(hits[0])
        print("  ok    %-16s %2d bytes  (%s)" % (struct, published[struct], where))

    if problems:
        return problems

    print()
    print("[2] sizes this build compiled")
    out = subprocess.run(
        [str(tool), "layout"], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        print(out.stdout)
        print(out.stderr, file=sys.stderr)
        return fail("adi_tool layout exited %d" % out.returncode)

    compiled = {}
    for line in out.stdout.splitlines():
        m = TOOL_LINE.match(line)
        if m:
            compiled[m.group(1)] = int(m.group(2))
    if not compiled:
        print(out.stdout)
        return fail("parsed no sizes out of `adi_tool layout` — its output format changed")
    for struct in sorted(compiled):
        print("  ok    %-16s %2d bytes" % (struct, compiled[struct]))

    print()
    print("[3] the spec and the binary agree")
    for struct, want in sorted(published.items()):
        got = compiled.get(struct)
        if got is None:
            problems += fail(
                "%s is specified at %d bytes but `adi_tool layout` never printed it"
                % (struct, want)
            )
        elif got != want:
            problems += fail(
                "%s: SPEC 6.3 says %d bytes, this build compiled %d. Do NOT change "
                "the static_assert to match the compiler — either the struct is "
                "wrong for this ABI, or the spec is wrong." % (struct, want, got)
            )
        else:
            print("  ok    %-16s %2d bytes, spec and build agree" % (struct, want))

    unspecified = set(compiled) - set(published)
    if unspecified:
        print()
        print("  note  printed but not size-specified in SPEC 6.3: %s"
              % ", ".join(sorted(unspecified)))

    print()
    print("%s -- %d problem(s)" % ("FAILED" if problems else "PASS", problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
