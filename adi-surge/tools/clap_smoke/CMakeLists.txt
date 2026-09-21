# clap_smoke: a headless CLAP host that plays one note and checks the output.
# See clap_smoke.cpp. Built by tools/build_clap_win.bat (`tests`), or:
#
#   cmake -S adi-surge/tools/clap_smoke -B adi-surge/build-clap-smoke
#   cmake --build adi-surge/build-clap-smoke --config Release
#
# The build tree goes under adi-surge/build-*/, which the monorepo already ignores.

cmake_minimum_required(VERSION 3.22)
project(clap_smoke CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The CLAP headers Surge already vendors (MIT). No new dependency.
set(CLAP_SMOKE_CLAP_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/../../surge/libs/clap-juce-extensions/clap-libs/clap/include"
    CACHE PATH "Directory containing clap/clap.h")
if(NOT EXISTS "${CLAP_SMOKE_CLAP_INCLUDE}/clap/clap.h")
  message(FATAL_ERROR "clap/clap.h not found under ${CLAP_SMOKE_CLAP_INCLUDE}. "
                      "Run adi-surge/tools/fetch_surge.sh first.")
endif()

add_executable(clap_smoke clap_smoke.cpp)
target_include_directories(clap_smoke PRIVATE "${CLAP_SMOKE_CLAP_INCLUDE}")

if(MSVC)
  target_compile_options(clap_smoke PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(clap_smoke PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(clap_smoke PRIVATE ${CMAKE_DL_LIBS})
endif()
