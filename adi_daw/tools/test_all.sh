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
    printf '  %-26s %s\n' "$name" "$line"
    [ "$rc" -ne 0 ] && fail=1
done

if [ "$suites" -eq 0 ]; then
    echo "  no test binaries found in $BUILD -- was it built?"
    exit 2
fi

echo
echo "=== validators ==="
for v in tools/validate_schema.py tools/validate_ops.py; do
    if python "$v" >/dev/null 2>&1; then
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
    if python ../.github/scripts/check_spec_layout.py "$tool" >/dev/null 2>&1; then
        printf '  %-26s PASS\n' "check_spec_layout.py"
    else
        printf '  %-26s FAIL\n' "check_spec_layout.py"
        fail=1
    fi
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS -- $total checks across $suites suites, validators clean"
else
    echo "FAILED -- see above"
fi
exit "$fail"
