# Linux libc++ checks

Linux x86-64 with Clang, libc++ and libc++abi 20.1.8. The headless configuration uses Ninja, Debug, `ADI_WITH_PD=ON`, `ADI_WITH_JUCE=OFF` and `ADI_WERROR=ON`. The C++ flags include `-stdlib=libc++`, `-Wimplicit-int-float-conversion`, `-Wshorten-64-to-32`, `-Wunused-lambda-capture`, `-Wrange-loop-construct` and libc++ debug hardening. The runtime probe resolved both libc++ and libc++abi. The checker uses Linux headers and the x86-64 ABI.

Dependencies were verified using `adi_daw/tools/fetch_external.sh --build-only`. The window probe uses `--with-juce`.

## Results
