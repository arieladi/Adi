# Linux libc++ checks

Linux x86-64 with Clang, libc++ and libc++abi 20.1.8. The headless configuration uses Ninja, Debug, `ADI_WITH_PD=ON`, `ADI_WITH_JUCE=OFF` and `ADI_WERROR=ON`. The C++ flags include `-stdlib=libc++`, `-Wimplicit-int-float-conversion`, `-Wshorten-64-to-32`, `-Wunused-lambda-capture`, `-Wrange-loop-construct` and libc++ debug hardening. The runtime probe resolved both libc++ and libc++abi. The checker uses Linux headers and the x86-64 ABI.

Dependencies were verified using `adi_daw/tools/fetch_external.sh --build-only`. The window probe uses `--with-juce`.

## Results

### 27 November 2026 — seats/session-chain `82a6813e`

Two translation units failed the Clang 20 syntax check with `-mlong-double-64`. This gives `long double` 53 mantissa bits, matching Apple Silicon's precision for these expressions. Both errors are `-Wimplicit-int-float-conversion`. The full headless build and suite are pending.

- `adi_daw/src/adi/engine/launch_audio.cpp:90:57` implicitly converts `firstTick_` from `int64_t` to `long double`. Cast `firstTick_` before the subtraction.
- `adi_daw/src/adi/session_performance_capture.cpp:493:29` implicitly converts the integer duration in the loop-frame comparison. Cast the result of the integer subtraction to `long double` so the arithmetic order is preserved.

Both files passed the same Clang precision check and a GCC syntax check after the two casts. One fix commit `6ebcbd5f` is on [libcxx/seats/session-chain](https://github.com/arieladi/Adi/tree/libcxx/seats/session-chain).
