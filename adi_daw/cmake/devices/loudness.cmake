# SPDX-License-Identifier: GPL-3.0-or-later
# DEV-28: the one loudness meter (src/adi/dsp/loudness) and its tests.
target_sources(adi_core PRIVATE src/adi/dsp/loudness.cpp)
add_executable(adi_loudness_tests tests/test_loudness.cpp)
target_link_libraries(adi_loudness_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_loudness_tests COMMAND adi_loudness_tests)

# DEV-28 / LD6: ffmpeg's ebur128 filter, the outside check on the 10 Hz loudness
# range (libebur128 hops at 1 Hz), run as the program on PATH: test-only,
# nothing linked. Without ffmpeg the suite passes as skipped; ADI_REQUIRE_ORACLES=1
# in the environment makes that a failure, for the CI leg that must run it.
add_executable(adi_loudness_ffmpeg_tests tests/test_loudness_ffmpeg.cpp)
target_link_libraries(adi_loudness_ffmpeg_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_loudness_ffmpeg_tests COMMAND adi_loudness_ffmpeg_tests)

# DEV-28 / LD3: libebur128 as an outside oracle, TEST-ONLY and never linked
# into the app. Built when it has been fetched (tools/fetch_external.sh, its
# third_party row); ADI_REQUIRE_LOUDNESS_ORACLE makes its absence an error, for
# the CI leg that must run it. Upstream's warnings are upstream's: no
# adi_warnings on the library, as with pffft.
option(ADI_REQUIRE_LOUDNESS_ORACLE "Fail the configure when libebur128 is not fetched" OFF)
set(_adi_ebur128 "${ADI_THIRD_PARTY}/libebur128/ebur128")
if(EXISTS "${_adi_ebur128}/ebur128.c")
    add_library(adi_test_ebur128 STATIC "${_adi_ebur128}/ebur128.c")
    target_include_directories(adi_test_ebur128 SYSTEM PUBLIC "${_adi_ebur128}")
    if(MSVC)
        # <sys/queue.h> is not in the Windows SDK; upstream ships one.
        target_include_directories(adi_test_ebur128 PRIVATE "${_adi_ebur128}/queue")
        target_compile_definitions(adi_test_ebur128 PRIVATE _USE_MATH_DEFINES)
        target_compile_options(adi_test_ebur128 PRIVATE /wd4244 /wd4267 /wd4996)
    else()
        target_link_libraries(adi_test_ebur128 PRIVATE m)
    endif()
    add_executable(adi_loudness_oracle_tests tests/test_loudness_oracle.cpp)
    target_link_libraries(adi_loudness_oracle_tests PRIVATE adi_core adi_warnings adi_test_ebur128)
    add_test(NAME adi_loudness_oracle_tests COMMAND adi_loudness_oracle_tests)
elseif(ADI_REQUIRE_LOUDNESS_ORACLE)
    message(FATAL_ERROR "ADI_REQUIRE_LOUDNESS_ORACLE: libebur128 is not in ${ADI_THIRD_PARTY}; "
                        "run tools/fetch_external.sh")
endif()
