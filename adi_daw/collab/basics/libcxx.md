# Linux libc++ checks

Linux x86-64 with Clang, libc++ and libc++abi 20.1.8. The headless configuration uses Ninja, Debug, `ADI_WITH_PD=ON`, `ADI_WITH_JUCE=OFF` and `ADI_WERROR=ON`. The C++ flags include `-stdlib=libc++`, `-Wimplicit-int-float-conversion`, `-Wshorten-64-to-32`, `-Wunused-lambda-capture`, `-Wrange-loop-construct` and libc++ debug hardening. The runtime probe resolved both libc++ and libc++abi. The checker uses Linux headers and the x86-64 ABI.

Dependencies were verified using `adi_daw/tools/fetch_external.sh --build-only`. The window probe uses `--with-juce`.

## Results

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
