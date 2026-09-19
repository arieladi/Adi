// SPDX-License-Identifier: GPL-3.0-or-later
//
// Version string, with a fallback so every translation unit compiles without
// the build system.
//
// CMake defines ADI_VERSION_STRING from PROJECT_VERSION, which is the value we
// actually want. But CI's strict -Werror job compiles our translation units
// directly with hand-written flags rather than through CMake, so a source file
// that *requires* a CMake-only define silently drops out of the gate — or, as
// happened here, fails it. A source file should compile with a bare compiler and
// an include path.
//
// The fallback is deliberately obviously wrong rather than plausible: if
// "0.0.0-nobuildsystem" ever appears in a release binary, the build is
// misconfigured and the string says so, instead of quietly reporting a version
// that was never built.

#pragma once

#ifndef ADI_VERSION_STRING
#define ADI_VERSION_STRING "0.0.0-nobuildsystem"
#endif

namespace adi {

inline constexpr const char* kVersion = ADI_VERSION_STRING;

}  // namespace adi
