# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/midi_notes.cpp src/adi/dsp/microtuner.cpp src/adi/dsp/midi_modulator.cpp)
add_executable(adi_midi_effects_tests tests/test_midi_effects.cpp)
target_link_libraries(adi_midi_effects_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_midi_effects_tests COMMAND adi_midi_effects_tests)

add_executable(adi_midi_modulation_tests tests/test_midi_modulation.cpp)
target_link_libraries(adi_midi_modulation_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_midi_modulation_tests COMMAND adi_midi_modulation_tests)
