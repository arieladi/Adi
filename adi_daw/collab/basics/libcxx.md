# Linux libc++ checks

Linux x86-64 with Clang, libc++ and libc++abi 20.1.8. The headless configuration uses Ninja, Debug, `ADI_WITH_PD=ON`, `ADI_WITH_JUCE=OFF` and `ADI_WERROR=ON`. The C++ flags include `-stdlib=libc++`, `-Wimplicit-int-float-conversion`, `-Wshorten-64-to-32`, `-Wunused-lambda-capture`, `-Wrange-loop-construct` and libc++ debug hardening. The runtime probe resolved both libc++ and libc++abi. The checker uses Linux headers and the x86-64 ABI.

Dependencies were verified using `adi_daw/tools/fetch_external.sh --build-only`. The window probe uses `--with-juce`.

## Results

### 28 November 2026, 17:58 IST — seats/stretcher-chain `1f193ff30a4a6e154a8b5eafb68d88be773fca66`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 295 registered non-performance CTest suites passed with `ctest -LE performance`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

The exact `tempo_map.cpp`, `automation.cpp`, `automation_clips.cpp` and `test_clip_envelopes.cpp` passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers. The test file used its CMake target flags.

### 28 November 2026, 17:41 IST — basics/browser-chain `0fd68e875f6d0d36022d30fdead6fd0d9f6f0fa3`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 290 registered non-performance CTest suites passed with `ctest -LE performance`.

JUCE Debug linked libc++ and libc++abi. The UI document and window-state suites passed. `adi_ui_shell_tests` repeated the same 90-pixel Devices, Mixer and Mixer View differences at this documentation-only merge after `b71ec782`. The shared search cleanup is still at `adi_daw/tests/test_ui_shell.cpp:32055`. The `f80910a6` entry below records the fixture cause and correction.

### 28 November 2026, 17:32 IST — seats/stretcher-chain `e53968a5a007328003201abaa97b57697cee3e3e`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 295 registered non-performance CTest suites passed with `ctest -LE performance`.

The full headless run used two parallel test jobs, including the automation, Link Playback and streaming Warp fixtures. The exact `audio/warp.cpp` and `engine/automation_clips.cpp` files passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 17:21 IST — seats/stretcher-chain `b365d79bc6a66dd0e6e322edb007b2ebf227edb6`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 295 registered non-performance CTest suites passed with `ctest -LE performance`.

The full headless run used two parallel test jobs. It passed `adi_streaming_warp_playback_tests`, including the new 96 kHz reverse Complex fixture. The exact `audio/warp.cpp` passed a strict Clang 20 Apple Silicon syntax check using macOS 26.1 SDK headers.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 17:11 IST — basics/browser-chain `b71ec782768782755121cea1ff95019a2aae3c38`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 290 registered non-performance CTest suites passed with `ctest -LE performance`.

JUCE Debug built and linked libc++ and libc++abi. The UI document and window-state suites passed. The peerless `adi_ui_shell_tests` run reproduced the same 90-pixel Devices, Mixer and Mixer View snapshot failures from `f80910a6`. The shared fixture still calls `browserPanel.search.clear()` at `adi_daw/tests/test_ui_shell.cpp:32055`. The existing Browser Escape handler clears the query and refreshes the results before the snapshots.

### 28 November 2026, 16:55 IST — seats/stretcher-chain `058d9b656b0ca00a10258b837f4b7d98e2cad6d6`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 295 registered non-performance CTest suites passed with `ctest -LE performance`.

The full headless run used two parallel test jobs and passed the Link Playback and streaming Warp suites. The exact `audio/warp.cpp` and `engine/clip_playback.cpp` files also passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 16:25 IST — main `629a483418be41ae7725d7c120a2b5bbb39188ca`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 290 registered non-performance CTest suites passed with `ctest -LE performance`.

The landed `session_discovery.cpp`, `consent_scope.cpp` and `app_control.cpp` passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers. The landed `getentropy` call has its direct Darwin declaration.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 16:16 IST — seats/session-chain `539b3c501f4db09667b439d2ea96373da1348e96`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 303 registered non-performance CTest suites passed with `ctest -LE performance`.

The exact `session_performance_capture.cpp` and `engine/session.cpp` files passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers. The headless run passed the Session scene-clock and performance-graph suites.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 16:01 IST — basics/browser-chain `f80910a6eb7f9c3a95658219c9be0889980ae44e`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 290 registered non-performance CTest suites passed with `ctest -LE performance`. `library_query.cpp`, `browser_catalogue.cpp`, `session_discovery.cpp` and `app_control.cpp` passed strict Apple Silicon syntax checks using the macOS 26.1 SDK.

JUCE Debug built and linked libc++ and libc++abi. The UI document and window-state suites passed. `adi_ui_shell_tests` failed the Devices, Mixer and Mixer View snapshot comparisons both peerless and under Xvfb. Each comparison differed at the same 90 pixels, in the 14 × 9 box beginning at `(133, 86)`.

The pixels show the Browser's new no-match notice from `adi_daw/src/juce/ui_browser.cpp:1009–1018`. The shared fixture clears the search widget at `adi_daw/tests/test_ui_shell.cpp:31906`, leaving the Browser results stale. The smallest fixture correction is to call the existing Escape handler, which clears the query and refreshes the results synchronously. The newer Browser tip `62141fcb` is queued.

### 28 November 2026, 15:39 IST — seats/stretcher-chain `7f4b86a67d01f1b5d42a84df3f12201e450c85f5`

Headless Debug compiled with Clang 20, libc++, Pd and the strict flags. The first full `ctest -LE performance` run passed 285 of 286 suites. `adi_link_playback_tests` failed the Complex Link tempo-change checks at `adi_daw/tests/test_link_playback.cpp:493–494`, `503` and `531`. The isolated rerun passed. A complete rerun with `ctest -LE performance --parallel 2` passed all 286 suites, including Link Playback and the streaming Warp suites.

JUCE Debug linked libc++ and libc++abi, and all three offscreen window suites passed. `streaming_warp.cpp`, `clip_playback.cpp`, `play_quantize.cpp` and `live_midi_input.cpp` also passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers.

### 28 November 2026, 15:10 IST — main `e14c080ae4029f99c6d5737d9811bcb520e4c2c7`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered non-performance CTest suites passed with `ctest -LE performance`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 15:00 IST — libcxx/seats/library-chain `b2bd4f792e24d318241d8e3c85c904bd350ba25d`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 295 registered non-performance CTest suites passed with `ctest -LE performance`.

The exact patched `probe.cpp` passed strict Clang 20 Apple Silicon syntax checks with macOS 26.1 SDK headers targeting macOS 14.0 and 26.1. Linux Clang/libc++ and GCC/libstdc++ syntax checks also passed.

The standalone Warp benchmark and its candidate test built against libc++ and libc++abi on Linux. Its CTest run passed, and a generated WAV smoke test accepted `1.2e2` BPM and rejected a leading `+`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 14:50 IST — seats/library-chain `5354d28b5cd9e9978400a24e10d31bd343b4c37d`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 295 registered non-performance CTest suites passed with `ctest -LE performance`.

In the separate Warp benchmark CMake project, the exact `adi_daw/tools/warp_bench/probe.cpp` failed a strict Apple Silicon syntax check with macOS 26.1 SDK headers and a macOS 14 deployment target. At `79:30`, floating `std::from_chars` is unavailable for that deployment target. The call at `100:47` instantiates the floating overload. The same check passed when targeting macOS 26.1.

Keep integral `from_chars` for the meter and parse decimal BPM with a classic-locale stream that consumes the full argument. The one-commit fix is queued for the complete Linux gate.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 14:32 IST — seats/pitch-chain `bbb4fc4aa28a10f0ab81804322a4cd299723ec04`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 301 registered non-performance CTest suites passed with `ctest -LE performance`.

The exact `pitch_midi.cpp` and `media/clip_pitch_render.cpp` files passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers. The full Linux run passed `adi_pitch_midi_tests` and `adi_pitch_compare_tests`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 14:17 IST — seats/session-chain `249b98ca717e92208bdb4982412dd30d3b930511`

Headless Debug compiled with Clang 20, libc++, Pd and the strict warning flags. The first full `ctest -LE performance` run had three failures while the root filesystem was below ADI's decode free-space floor. A full rerun with the same binaries and a RAM-backed test scratch directory passed all 303 suites, including Clip Playback, Warp Playback and AAC Import.

JUCE Debug linked libc++ and libc++abi. All three offscreen window suites passed. `launch_audio_runtime.cpp`, `launch_audio.cpp`, `session.cpp` and `clip_playback.cpp` also passed strict Clang 20 Apple Silicon syntax checks using macOS 26.1 SDK headers.

### 28 November 2026, 13:46 IST — libcxx/control/agent-chain `f11ae67a1ca3542afdb6254898982c87f6a07388`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 290 registered non-performance CTest suites passed with `ctest -LE performance`.

The Darwin-only `<sys/random.h>` include fixed the `getentropy` declaration error. The exact corrected `session_discovery.cpp` passed strict Clang 20 syntax checks against the macOS 26.1 SDK for Apple Silicon, as well as strict Linux Clang/libc++ and GCC/libstdc++ syntax checks.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 13:35 IST — control/agent-chain `289611a34eb8296338a2cf457c25115f8f238514`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 290 registered non-performance CTest suites passed with `ctest -LE performance`.

The exact `adi_daw/src/adi/control/session_discovery.cpp` translation unit failed a Clang 20 syntax check on Linux targeting `arm64-apple-macos14.0` with macOS 26.1 SDK headers. The diagnostic at `117:21` is `no member named 'getentropy' in the global namespace`. Darwin declares it in `<sys/random.h>`. The smallest fix adds that direct include under `__APPLE__`. [Apple XNU declaration](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/random.h#L33-L42)

The same Darwin syntax configuration passed `agent_process.cpp`, `loopback_listener.cpp`, `consent_file.cpp` and `app_control.cpp` at this revision. The header fix is queued for the complete Linux gate.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 13:16 IST — seats/stretcher-chain `d6b090b109aa224bba8b51734302157af39e2c05`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 286 registered non-performance CTest suites passed with `ctest -LE performance`.

All 23 registered C++ translation units mentioning `long double` passed the Apple-width syntax check. `adi_warp_launch_tests` and the Warp playback suite passed in the full headless run.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 12:56 IST — seats/server-chain `bc7877193c782d37e0c6c4a6b85863e70855b711`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 292 registered non-performance CTest suites passed with `ctest -LE performance`.

`adi_plugin_admission_tests` passed with the explicit preserve-policy fixture and the separate default share-policy case. This tip also includes the portable AudioGridder parser fix `5cce0417`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 12:46 IST — seats/server-chain `dd26236dd049820217d431e0c1a77a9787e16ca3`

Headless Debug compiled with Clang 20, libc++, Pd and the strict warning flags. The full `ctest -LE performance` run passed 291 of 292 suites. `adi_plugin_admission_tests` failed at `adi_daw/tests/test_plugin_admission.cpp:204` when the copy received a new UUID. The same assertion failed on a direct rerun. The gate ended before the JUCE phase.

The fixture calls `saveCopy` at line `198` with the default `StoreCopyPolicy::share` from `adi_daw/src/adi/store.hpp:186–187`. To test preservation, pass `StoreCopyPolicy::preserve` explicitly. The Server author already made that change and added a separate share-policy test in `6d313f591`. The newer Server tip `bc787719` includes it and is queued for the full gate. [Fixture correction](https://github.com/arieladi/Adi/commit/6d313f591)

### 28 November 2026, 12:32 IST — main `6aeeed1b19b6f45cb15ec1bb65ce1084058bb127`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered non-performance CTest suites passed with `ctest -LE performance`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 02:58 IST — main `bdcccb3e077af8bcf3b32e7210537721a9294cb2`, test and tool Apple-width check

All 296 C++ translation units under `adi_daw/tests` and `adi_daw/tools` covered by `-Werror` in this JUCE and Pd build passed the strict Clang 20/libc++ syntax check with `-mlong-double-64`. Together with the production check below, this covers 629 translation units on this revision. The registered suites ran in the native Linux ABI as recorded below.

### 28 November 2026, 02:51 IST — main `bdcccb3e077af8bcf3b32e7210537721a9294cb2`, production Apple-width check

All 333 production C++ translation units under `adi_daw/src` covered by `-Werror` in this JUCE and Pd build passed strict syntax checks with Clang 20, libc++ and `-mlong-double-64`. This expands the 53-bit `long double` precision check to sources that use types declared in headers. The native Linux build and suite results for this revision are recorded below.

### 28 November 2026, 02:36 IST — main `bdcccb3e077af8bcf3b32e7210537721a9294cb2`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered CTest suites passed.

This revision updates two Markdown files after `a9c19691`, whose 22 Apple-width syntax checks passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 02:26 IST — main `a9c19691752bfdbec6027db98751b82d03da5aa7`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered CTest suites passed.

All 22 registered ADI translation units mentioning `long double` passed strict syntax checks with `-mlong-double-64`. The landing includes Win's two explicit sample-rate casts in `adi_daw/src/adi/clip_commands.cpp`. [Landed fix](https://github.com/arieladi/Adi/commit/7c59f39e536edfe980b6df584edf44cf1646fb59)

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 01:42 IST — libcxx/seats/pitch-chain `0ef9c557e1736315c12ede28346c1393e5e32ff0`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 298 registered CTest suites passed.

All 22 registered ADI translation units mentioning `long double` passed strict syntax checks with `-mlong-double-64`, including `adi_daw/src/adi/pitch_midi.cpp`. `adi_pitch_midi_tests` passed in the headless run.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 01:30 IST — libcxx/basics/integrate-2 `39afd389608148eb927b8c2158fd32cf318ebed1`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered CTest suites passed.

All 22 registered ADI translation units mentioning `long double` passed strict syntax checks with `-mlong-double-64`, including `adi_daw/src/adi/clip_commands.cpp`. `adi_warp_actions_tests` exercised clip source replacement.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 01:22 IST — main `fbf541ee625ad4761102f1ed0bb500c7b3a6f420`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 279 registered CTest suites passed.

All 20 registered ADI translation units mentioning `long double` passed strict syntax checks with `-mlong-double-64`.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 01:11 IST — basics/integrate-2 `f02bd171caca744ea91898f6c4a8d733488f2e3a`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered CTest suites passed.

Win reported the same rescaling diagnostic on macOS. The landing stack carries the equivalent two-cast fix in [`7c59f39e`](https://github.com/arieladi/Adi/commit/7c59f39e536edfe980b6df584edf44cf1646fb59).

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 00:59 IST — basics/integrate-2 `f02bd171caca744ea91898f6c4a8d733488f2e3a`, Apple-width check

With `-mlong-double-64`, Clang 20 and libc++ rejected two implicit conversions in `adi_daw/src/adi/clip_commands.cpp`. The new sample rate at `509:73` and the old sample rate at `509:84` are both `int64_t`. The other 21 registered translation units mentioning `long double` passed. The full Linux x86-64 build is in progress.

Cast both rates to `long double` explicitly. The multiply, divide and `std::llround` keep their existing order. The patched file passed strict syntax checks with native Clang 20/libc++, Clang 20/libc++ at 53-bit `long double` precision, and GCC 14/libstdc++. Both expressions produced identical code in Clang's optimized intermediate representation at each precision. One fix commit `39afd389` is on [libcxx/basics/integrate-2](https://github.com/arieladi/Adi/tree/libcxx/basics/integrate-2).

### 28 November 2026, 00:53 IST — libcxx/control/agent-chain `a0e5dfc3daffe115ec9630bdaeae2ba6f7b265ac`

The raw `control/agent-chain` head `d68937dc5f4413e3f55d44070da33135710938c2` failed the full headless build in `adi_daw/src/juce/app_control.cpp:292:58`. This was the only failed compile target. The raw head could not run the suites.

The one-commit snapshot publication fix built in strict Debug with Clang 20, libc++ and Pd. All 287 headless CTest suites passed. JUCE Debug linked libc++ and libc++abi, and all three offscreen window suites passed.

### 28 November 2026, 00:33 IST — main `facf39b4a23f7fbffaf3d0d52ecc7de1ed3fd1dd`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 279 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 00:23 IST — basics/browser-chain `862cea353f80808d208ce54b773b82cf6a20a3e3`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered CTest suites passed.

All 21 registered ADI translation units mentioning `long double` passed strict syntax checks with `-mlong-double-64`, which matches Apple Silicon's `long double` precision for the checked expressions.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 28 November 2026, 00:13 IST — control/agent-chain `d68937dc5f4413e3f55d44070da33135710938c2`

The headless `adi_control` translation unit fails under Clang 20 and libc++. At `adi_daw/src/juce/app_control.cpp:292:58`, libc++ rejects `std::atomic<std::shared_ptr<const BrowserCatalogue>>` because the primary atomic template requires a trivially copyable type. The callback error at `88:34` follows from that instantiation. Full build and suites are queued.

A mutex now protects copying and swapping the immutable snapshot pointer. The owner constructs the new catalogue before locking and releases the old snapshot after unlocking. The affected file passed strict syntax checks with Clang 20/libc++ and GCC 14/libstdc++. GCC 14 treats the older shared-pointer atomic free functions as deprecated under `-Werror`. One fix commit `a0e5dfc3` is on [libcxx/control/agent-chain](https://github.com/arieladi/Adi/tree/libcxx/control/agent-chain).

### 28 November 2026, 00:03 IST — seats/library-chain `081971490757cceb72037e5b29cb16efedb980c3`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 293 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026, 23:50 IST — libcxx/seats/server-chain `5cce04174ef3834163f3bd66ece335cb10440c13`

The fixed Server branch built with Clang 20, libc++, Pd and the strict flags. All 290 headless suites passed, including the AudioGridder transport fixture. JUCE Debug also linked libc++ and libc++abi, and all three offscreen window suites passed.

### 27 November 2026 — seats/server-chain `548025a7`

The headless build failed while compiling `adi_daw/src/juce/audiogridder_transport.cpp`. The parsers at `315:16`, `316` and `687` instantiate nlohmann input adapters for `std::byte`. Pinned nlohmann derives those traits from libc++'s undefined `std::char_traits<std::byte>` at `adi_daw/third_party/json/include/nlohmann/detail/meta/type_traits.hpp:191:22`. The subsequent missing `int_type` errors have the same cause.

The smallest fix supplies a zero-copy `std::span<const unsigned char>` to the two MessagePack passes and the JSON parser. Both Clang 20/libc++ and GCC/libstdc++ accepted the fixed translation unit under strict syntax checks. The full fixed-branch suites are queued. One fix commit `5cce0417` is on [libcxx/seats/server-chain](https://github.com/arieladi/Adi/tree/libcxx/seats/server-chain).

### 27 November 2026, 23:31 IST — seats/pitch-chain `34e21e163684e556f25f5b9496d78afcb3b1285a`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 298 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026 — seats/pitch-chain `34e21e16`, Apple-width follow-up

The new head still fails the `-mlong-double-64` syntax check. Both errors are `-Wimplicit-int-float-conversion` in `adi_daw/src/adi/pitch_midi.cpp`. They moved to `196:84` for the source-frame delta and `197:68` for the tick delta.

The existing fix `0ef9c557e` applied cleanly to this head in a temporary worktree. With the two explicit casts, the file passed both the native and Apple-width syntax checks. The full Linux suites are in progress. [Pitch fix branch](https://github.com/arieladi/Adi/tree/libcxx/seats/pitch-chain)

### 27 November 2026, 23:13 IST — seats/session-chain `beb7ebc87a5e273b9faa0455732555d74d0ee66b`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 301 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026, 22:54 IST — seats/stretcher-chain `ecc23ed923b08c21990b4b0c62d4386669ef2374`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 284 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026, 22:37 IST — basics/integrate-2 `e503a8bc33f5b3d60ee7c1ab0693742db0dbec6e`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 281 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026, 22:18 IST — control/agent-chain `c90a276d9abbfc6715586c6adb66bf80b31e16f3`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 284 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026, 22:00 IST — main `ed3da057c1b32166f3acb3743449037a4be32173`

Headless Debug built with Clang 20, libc++, Pd and the strict warning flags. All 279 registered CTest suites passed.

JUCE Debug also built and linked libc++ and libc++abi. The three offscreen window suites passed.

### 27 November 2026 — seats/pitch-chain `e772da3e`

The modified file passed native x86-64 Clang syntax checking. It failed with `-mlong-double-64` on the following implicit conversions. The full headless build and suite are pending.

- `adi_daw/src/adi/pitch_midi.cpp:195:84` converts the source-frame delta to `long double`. Explicitly cast the result of the integer subtraction before dividing.
- `adi_daw/src/adi/pitch_midi.cpp:196:68` converts the tick delta to `long double`. Explicitly cast the result of the integer subtraction before multiplying.

The fixed file passed both Clang syntax checks and GCC syntax checking. One fix commit `0ef9c557e` is on [libcxx/seats/pitch-chain](https://github.com/arieladi/Adi/tree/libcxx/seats/pitch-chain).

### 27 November 2026 — main `6f96847b`, JUCE

JUCE Debug built with Clang 20, Pd and the same strict warnings. Both `juceaide` and the ADI executable resolved libc++ and libc++abi. The UI document, window state and UI shell suites passed offscreen. No source fix was needed.

### 27 November 2026 — main `6f96847b`

Headless Debug built with Pd, libc++ and the strict warning flags. All 278 CTest suites passed, including a rerun of `adi_clip_playback_tests`. The first run of that suite hit `decode.no_space` because the build left the host below the cache's minimum free space. It passed with the same binaries after reclaiming generated build files. No source fix was needed.

The pinned JUCE probe configured. An application build with the libc++-linked JUCE helper is in progress.

### 27 November 2026 — seats/session-chain `82a6813e`

Two translation units failed the Clang 20 syntax check with `-mlong-double-64`. This gives `long double` 53 mantissa bits, matching Apple Silicon's precision for these expressions. Both errors are `-Wimplicit-int-float-conversion`. The full headless build and suite are pending.

- `adi_daw/src/adi/engine/launch_audio.cpp:90:57` implicitly converts `firstTick_` from `int64_t` to `long double`. Cast `firstTick_` before the subtraction.
- `adi_daw/src/adi/session_performance_capture.cpp:493:29` implicitly converts the integer duration in the loop-frame comparison. Cast the result of the integer subtraction to `long double` so the arithmetic order is preserved.

Both files passed the same Clang precision check and a GCC syntax check after the two casts. One fix commit `6ebcbd5f` is on [libcxx/seats/session-chain](https://github.com/arieladi/Adi/tree/libcxx/seats/session-chain).
