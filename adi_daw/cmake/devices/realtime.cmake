# SPDX-License-Identifier: AGPL-3.0-or-later
# The 3 December session's performance pass (WTS-01, ADR-0248 d2): every
# built-in's callback at 64 and 128 frames, alone and in WTS-01's project, for
# allocation, cost and spikes. With the acceptance suite (-DADI_ACCEPTANCE=ON,
# ctest -L acceptance); its timings are checked in an optimised build only.
if(ADI_ACCEPTANCE AND ADI_WITH_PD)
    add_executable(adi_builtin_realtime_tests tests/test_builtin_realtime.cpp)
    target_link_libraries(adi_builtin_realtime_tests PRIVATE adi_core adi_settings adi_warnings)
    add_test(NAME adi_builtin_realtime_tests COMMAND adi_builtin_realtime_tests)
    set_tests_properties(adi_builtin_realtime_tests PROPERTIES LABELS "acceptance;performance" TIMEOUT 900)
endif()
