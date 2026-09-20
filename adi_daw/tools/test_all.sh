#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run everything: every test binary, both schema validators, and the
# spec-vs-binary layout check.
#
#   bash adi_daw/tools/test_all.sh [build-dir]
#
# Exists because "run the tests" had grown to nine binaries plus three Python
# scripts, and a list that long in prose is a list someone runs four of. The
# binaries are DISCOVERED rather than named, so a new suite is covered the
# moment it builds — the same reason CI globs its sources rather than listing
# them.

set -uo pipefail

cd "$(dirname "$0")/.."
BUILD="${1:-build}"

if [ ! -d "$BUILD" ]; then
    echo "no build directory at $BUILD"
    echo "  Windows:      tools\\build.bat"
    echo "  macOS/Linux:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build"
    exit 2
fi

fail=0
total=0
suites=0

echo "=== test binaries ==="
# *tests* rather than a list: adi_tool is not a test, everything else is.
# One glob, not two -- "*tests*" already matches Windows' .exe, and globbing
# both forms counted every binary twice and cheerfully reported 18 suites.
for exe in "$BUILD"/*tests*; do
    [ -f "$exe" ] && [ -x "$exe" ] || continue
    case "$exe" in
        *.pdb|*.ilk|*.lib|*.obj|*.manifest) continue ;;
    esac
    name=$(basename "$exe" .exe)
    out=$("$exe" 2>&1)
    rc=$?
    # Captured BEFORE piping. `x=$(cmd | tail -1); rc=$?` reads tail's exit
    # status, not the test's, so a failing suite reports success -- which is
    # how a green summary hides a red run.
    line=$(printf '%s' "$out" | tail -1)
    n=$(printf '%s' "$line" | grep -oE '[0-9]+ checks' | grep -oE '[0-9]+' || echo 0)
    total=$((total + n))
    suites=$((suites + 1))
    # A crashed binary prints nothing, and a blank line here reads as a harness
    # glitch rather than as the segfault it is -- which is exactly how
    # adi_device_tests' overrun first presented, twice, before anyone ran it
    # directly. Say so instead.
    if [ -z "$line" ] && [ "$rc" -ne 0 ]; then
        line="CRASHED -- exit $rc, no output (run it directly)"
    fi
    printf '  %-26s %s\n' "$name" "$line"
    [ "$rc" -ne 0 ] && fail=1
done

if [ "$suites" -eq 0 ]; then
    echo "  no test binaries found in $BUILD -- was it built?"
    exit 2
fi

echo
# macOS has shipped without a bare `python` since 12.3 removed python2, so
# `python foo.py` cannot work on any Mac -- and this script IS the project's
# definition of done. Windows is usually the other way round: `python` exists
# and `python3` often does not. Detect rather than assume, and say which was
# chosen so a surprising validator result is one line from being explained.
# Probe by RUNNING it, not with `command -v`. Windows ships an App Execution
# Alias stub named python3.exe that is on PATH, is found by `command -v`,
# and is not Python -- it prints a Microsoft Store advert and exits non-zero.
# Detecting with `command -v` therefore picks it, and all three validators
# fail with no hint that the interpreter was never Python at all.
PY=""
for candidate in python3 python py; do
    if "$candidate" --version >/dev/null 2>&1; then PY="$candidate"; break; fi
done
if [ -z "$PY" ]; then
    echo "FAILED -- no python3 or python on PATH; the validators cannot run" >&2
    exit 1
fi

echo "=== validators ==="
echo "  interpreter: $PY ($("$PY" --version 2>&1))"
# DISCOVERED, not listed. A hardcoded list is a list somebody forgets to add
# to -- the same defect that made the -Werror gate skip new files, and that
# made this script miss two suites. tools/validate_ci.py was added after a CI
# step landed in the wrong job, and it was picked up here without touching
# this line, which is the point.
for v in tools/validate_*.py; do
    [ -f "$v" ] || continue
    if "$PY" "$v" >/dev/null 2>&1; then
        printf '  %-26s PASS\n' "$(basename "$v")"
    else
        printf '  %-26s FAIL\n' "$(basename "$v")"
        fail=1
    fi
done

# The spec's published record sizes against what the compiler actually produced.
tool="$BUILD/adi_tool"
[ -f "$tool.exe" ] && tool="$tool.exe"
if [ -x "$tool" ] && [ -f ../.github/scripts/check_spec_layout.py ]; then
    if "$PY" ../.github/scripts/check_spec_layout.py "$tool" >/dev/null 2>&1; then
        printf '  %-26s PASS\n' "check_spec_layout.py"
    else
        printf '  %-26s FAIL\n' "check_spec_layout.py"
        fail=1
    fi
fi

echo
# The README states these two numbers and nothing checked them: it claimed 627
# checks across 8 suites while the real figures were 962 and 12. `validate_ops`
# has checked its own headline since the 152-vs-174 drift, and check 7 does it
# for tables and ADRs; this is the same rule for the one pair of numbers only
# this script can know.
readme="README.md"
if [ -f "$readme" ]; then
    claimed=$(grep -oE '\*\*[0-9]+ checks across [0-9]+ suites\*\*' "$readme" | head -1)
    want="**$total checks across $suites suites**"
    if [ -n "$claimed" ] && [ "$claimed" != "$want" ]; then
        echo
        echo "  README says '$claimed', this run is '$want'"
        fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    echo "PASS -- $total checks across $suites suites, validators clean"
else
    echo "FAILED -- see above"
fi
exit "$fail"
