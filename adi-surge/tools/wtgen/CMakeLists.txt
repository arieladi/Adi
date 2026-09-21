# wtgen: wavetable descriptor, generator and pack tool (ADR-0009 prototype).
# Standalone, no dependencies. See the header of wtgen.cpp.
#
#   cmake -S adi-surge/tools/wtgen -B adi-surge/build-wtgen
#   cmake --build adi-surge/build-wtgen --config Release
#
# The build tree goes under adi-surge/build-*/, which the monorepo already ignores.

cmake_minimum_required(VERSION 3.22)
project(wtgen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wtgen wtgen.cpp)

if(MSVC)
  target_compile_options(wtgen PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(wtgen PRIVATE -Wall -Wextra -Werror)
endif()
