# SPDX-License-Identifier: GPL-3.0-or-later
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef id)
set(sandbox "${CMAKE_CURRENT_BINARY_DIR}/quit-export-${id}")
file(MAKE_DIRECTORY "${sandbox}/home")
set(project "${sandbox}/fixture.adi")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "ADI_HOME=${sandbox}/home" "${FIXTURE}" --fixture "${project}"
                RESULT_VARIABLE fixture_status TIMEOUT 10)
if(NOT fixture_status STREQUAL "0")
    file(REMOVE_RECURSE "${sandbox}")
    message(FATAL_ERROR "Creating the archive quit fixture failed (${fixture_status})")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "ADI_HOME=${sandbox}/home" "${APP}"
                        --control-test --no-audio --quit-export-probe "${sandbox}/fixture.zip" "${project}"
                RESULT_VARIABLE app_status TIMEOUT 15)
file(REMOVE_RECURSE "${sandbox}")
if(NOT app_status STREQUAL "0")
    message(FATAL_ERROR "Archive quit probe failed (${app_status})")
endif()
