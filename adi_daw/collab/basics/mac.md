# Mac checks, 27 November 2026

Local Apple Silicon check with macOS 26.6.2, Xcode 27.0 / Apple Clang 21.0.0, CMake 4.3.3 and Ninja 1.13.2.

## Main baseline

Baseline `00f1f369f3566c0c833390f8af5f0df9ed50f1cf`.

- Apple Silicon Debug with Ninja, JUCE and Pd enabled, `ADI_WERROR=ON` and `ADI_ACCEPTANCE=ON`. All 50 non-test build targets completed. Used line-table debug information and batches of test executables to fit the local disk.
- All five repository validators passed. The native `adi_tool` layouts agree with SPEC §6.3. A separate JUCE-off Debug build of `adi_tool` with hardened libc++ and warnings as errors produced arm64 and x86_64 slices. Its format layouts also agree with SPEC §6.3.
- All 291 registered CTest suites passed. This includes 31/31 window and mixer suites and 3/3 from `ctest -L acceptance`, including the VST3 session on macOS.
- A native `--no-audio --smoke-output` launch rendered a 1200 × 720 image and exited 0. At shutdown, JUCE reported 47 instances each of `CoreGraphicsPixelData`, `MemoryBlock` and `ImagePixelData` still live, reported at the pinned JUCE's `modules/juce_core/memory/juce_LeakedObjectDetector.h:116`. The 47 reports match the 47 preview swatches cached in function-local static arrays at `src/juce/ui_analysis.cpp:64–71` and populated at `src/juce/ui_analysis.cpp:1120–1125`. The same diagnostics appeared in passing window suites. The cache lifetime is shared code in the mixview seat.

## Landing stacks and chains

| Branch | Head | Built | Suites and failures |
|---|---|---|---|
| `origin/land/basics-1127b` | `2ab946f3892dbef49cb822b507f3bfe3a42deb4f` | Yes | Window and mixer 31/31, acceptance 3/3. This head and the main baseline differ only in the Mac queue prompt. |
| `origin/land/basics-1127a` | `b6753499d60f6e31a811fdec47897a23acee2488` | Yes | Passed 122/122. Affected suites 88/88, window and mixer 31/31, acceptance 3/3. |
| `origin/land/basics-1127c` | `e73d84ce7669eff2cd1d19a93b6ba5ca0040e628` | No | Apple Clang at `src/adi/ui/project_document.cpp:184:48` reported `implicit conversion from 'const std::int64_t' (aka 'const long long') to 'long double' may lose precision [-Werror,-Wimplicit-int-float-conversion]`. The build stopped before suites could start. `90f4091d` has the explicit conversion on the newer landing head. |
| `origin/land/basics-1127d` | `c351a856939074d3fd086d55c07b87b3c2fa3b8d` | No | Apple Clang at `src/adi/ui/project_document.cpp:183:48` reported `implicit conversion from 'const std::int64_t' (aka 'const long long') to 'long double' may lose precision [-Werror,-Wimplicit-int-float-conversion]`. The build stopped before suites could start. `90f4091d` has the explicit conversion on the newer landing head. |
| `origin/basics/recording-chain` | `45f0a9b0deafbfa5b444b02929e1a231586a65b8` | No | Apple Clang at `src/adi/ui/project_document.cpp:184:48` reported `implicit conversion from 'const std::int64_t' (aka 'const long long') to 'long double' may lose precision [-Werror,-Wimplicit-int-float-conversion]`. The build stopped before suites could start. `90f4091d` has the explicit conversion on the newer landing head. |
| `origin/basics/tracks-chain` | `35b782d6858d4f0a7bb3cfef6103e4a4208ea6d9` | No | Apple Clang at `src/adi/ui/project_document.cpp:184:48` reported `implicit conversion from 'const std::int64_t' (aka 'const long long') to 'long double' may lose precision [-Werror,-Wimplicit-int-float-conversion]`. The build stopped before suites could start. `90f4091d` has the explicit conversion on the newer landing head. |
| `origin/land/basics-1127d` | `90f4091d241d1ae4b8690e74fafd1124d4611f0c` | Yes | Passed 46/46. The explicit conversion clears the Apple Clang build failure. Affected suites 12/12, window and mixer 31/31, acceptance 3/3. |

## Nine-step window walk

Step 8 recorded in [the walk report](https://github.com/arieladi/Adi/blob/walk/macos/adi_daw/collab/walk/2026-11-27-macos.md).

## Accessibility permission

macOS initially denied Accessibility and event posting to the shell session. After approval in System Settings, both system preflight checks returned true.

## For the director

No outstanding permission request.
