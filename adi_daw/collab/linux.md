# linux — log

Ubuntu workstation · GCC/Clang · x86-64 · ChatGPT Codex (terminal).
Only the `linux` agent writes to this file. Newest entry at the top.

Role and boundaries: `collab/README.md` (roster) and ADR-0109. Onboarding and
first tasks: `collab/linux/ONBOARDING.md`.

---

## 2026-09-23 — main baseline and a portable check count

Branch `linux/baseline`, based on main `f6910b1474f306bef70b21f50ce6e4f9475846cf`
(PR #41). No work was based on `agent/win-dev`; there was nothing to discard.
Re-read the collaboration protocol, onboarding, ADR-0109, ADR-0102 and ADR-0120.
Windows drivers and platform/UI work remain outside my scope.

**Repository inventory:** previously cloned `arieladi/Adi`, `arieladi/AdiGuard`,
and `arieladi/adi-vst-synth`. No `adi-surge` repository was cloned. Earlier setup
listed filenames in the other clones; since Adi restricted this assignment to
`arieladi/Adi`, I have not read or modified either other repository and will not.

Ubuntu x86-64; GCC/G++ 15.2.0; Clang 21.1.8; CMake 4.2.3; Ninja 1.13.2;
Python 3.14.4. Dependencies fetched with `tools/fetch_external.sh --build-only`:
SQLiteCpp `59a047b8d`, json `55f93686c`, CLAP `195b42a00`, pins verified.
All builds use C++20, `ADI_WITH_JUCE=OFF`, and three build jobs.

### Baseline finding and correction

Unmodified main's GCC Debug/Release and Clang Debug builds passed all 24 suites
and all five validators, but `test_all.sh` returned 1: 2,274 actual checks versus
2,273 in README. `testClapHostWithoutAnyPlugin` counted one assertion per default
search path, making the total depend on the OS/environment. It now asserts
`all_of(paths, nonempty)` once, keeping the separate nonempty-list assertion.
README's fixed total is **2,272 checks across 24 suites**; no check was removed
from the property being tested. No host implementation was edited.

**Negative control:** compiled a temporary copy of `test_clap.cpp` against the
same GCC Debug core, with an empty path appended immediately after obtaining
the defaults. It exited 1 with `FAIL and none of them is empty` (324 checks,
1 failure). The unmodified fixed source passes all 324 CLAP checks. The plant
never entered the repository working tree.

### Measured wall times (seconds)

Build times are initial configure/build wall times, not incremental rebuilds.
The final Clang Release build already contains the count correction; the other
three required only an incremental rebuild of the CLAP test afterward.

| Configuration | Configure | Initial build | Final test_all.sh | Result |
|---|---:|---:|---:|---|
| gcc-debug | 0.75 | 68.8 | 3.9 | 2,272 checks / 24 suites; five validators PASS |
| gcc-release | 0.74 | 161.81 | 2.86 | 2,272 checks / 24 suites; five validators PASS |
| clang-debug | 1.03 | 82.12 | 3.55 | 2,272 checks / 24 suites; five validators PASS |
| clang-release | 0.94 | 151.59 | 2.83 | 2,272 checks / 24 suites; five validators PASS |

Reproduce with separate build directories:

```sh
cmake -S adi_daw -B /tmp/adi-gcc-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DADI_WITH_JUCE=OFF \
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build /tmp/adi-gcc-debug -j 3
bash adi_daw/tools/test_all.sh /tmp/adi-gcc-debug
```

Repeat with `clang`/`clang++` and Release. The earlier 1,028/13 run was on
`aedf0fa` and is superseded by this baseline.

**Delivery status:** local validation complete; PR/CI/merge pending. The existing
OAuth login is usable for the authorized Adi-only operations; replacing it with
Adi's requested repository-scoped token remains a separate user terminal step.
The authentication note contains no token. No credential was requested in chat,
printed, or read from the gh configuration store. Existing CI on main passed,
but is not evidence for this branch; no claim of branch CI success is made.

Next: ASan+UBSan, then TSan across all 24 suites. No schema, ADR, platform,
driver or other agent's claimed implementation file was changed.
