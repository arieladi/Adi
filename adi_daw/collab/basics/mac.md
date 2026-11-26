# Mac checks, 27 November 2026

Local Apple Silicon check with macOS 26.6.2, Xcode 27.0 / Apple Clang 21.0.0, CMake 4.3.3 and Ninja 1.13.2.

## Main baseline

Baseline `00f1f369f3566c0c833390f8af5f0df9ed50f1cf`.

- Apple Silicon Debug with Ninja, JUCE and Pd enabled, `ADI_WERROR=ON` and `ADI_ACCEPTANCE=ON`. All 50 non-test build targets completed. Used line-table debug information and batches of test executables to fit the local disk.
- All five repository validators passed. The native `adi_tool` layouts agree with SPEC §6.3. The `blob.hpp` ABI assertions compiled into an arm64 and x86_64 universal object with hardened libc++.
- Window and mixer suites passed 31/31. `ctest -L acceptance` passed 3/3, including the VST3 session on macOS. The rest of the 291-suite run is in progress.
- A native `--no-audio --smoke-output` launch rendered a 1200 × 720 image and exited 0. At shutdown, JUCE reported 47 instances each of `CoreGraphicsPixelData`, `MemoryBlock` and `ImagePixelData` still live, reported at the pinned JUCE's `modules/juce_core/memory/juce_LeakedObjectDetector.h:116`. The allocation site is under investigation.

## Landing stacks and chains

| Branch | Head | Built | Suites and failures |
|---|---|---|---|

## Nine-step window walk

Pending baseline and initial landing checks.

## Accessibility permission

macOS initially denied Accessibility and event posting to the shell session. After approval in System Settings, both system preflight checks returned true.

## For the director

No outstanding permission request.
