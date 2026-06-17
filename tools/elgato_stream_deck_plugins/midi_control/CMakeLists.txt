cmake_minimum_required(VERSION 3.20)
project(StreamDeckMidiHelper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Stream Deck application only runs on Windows and macOS, so the helper does too.
if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR
    "Unsupported platform. The Stream Deck app (and this helper) target Windows and macOS only.")
endif()

include(FetchContent)

# ---- IXWebSocket (websocket server). We only use ws:// on localhost, so no TLS/zlib. ----
set(USE_TLS  OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
)
FetchContent_MakeAvailable(ixwebsocket)

# ---- nlohmann/json (header-only) ----
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(StreamDeckMidiHelper main.cpp)
target_link_libraries(StreamDeckMidiHelper PRIVATE
  ixwebsocket
  nlohmann_json::nlohmann_json
)

# --------------------------------------------------------------------------- #
# Windows: teVirtualMIDI (virtual port) + SendInput (keystrokes)
# --------------------------------------------------------------------------- #
if(WIN32)
  # Place the teVirtualMIDI SDK under third_party/teVirtualMIDI:
  #   include/teVirtualMIDI.h
  #   lib/x64/teVirtualMIDI64.lib   (link)
  #   bin/x64/teVirtualMIDI64.dll   (copied next to the exe)
  set(TEVM_ROOT    "${CMAKE_SOURCE_DIR}/third_party/teVirtualMIDI")
  set(TEVM_INCLUDE "${TEVM_ROOT}/include")
  set(TEVM_LIB     "${TEVM_ROOT}/lib/x64/teVirtualMIDI64.lib")
  set(TEVM_DLL     "${TEVM_ROOT}/bin/x64/teVirtualMIDI64.dll")

  if(NOT EXISTS "${TEVM_INCLUDE}/teVirtualMIDI.h")
    message(FATAL_ERROR
      "teVirtualMIDI.h not found in ${TEVM_INCLUDE}. "
      "Install loopMIDI / the teVirtualMIDI SDK and copy include/lib/bin into third_party/teVirtualMIDI.")
  endif()

  target_include_directories(StreamDeckMidiHelper PRIVATE "${TEVM_INCLUDE}")
  target_link_libraries(StreamDeckMidiHelper PRIVATE "${TEVM_LIB}" ws2_32 user32)
  target_compile_definitions(StreamDeckMidiHelper PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

  if(EXISTS "${TEVM_DLL}")
    add_custom_command(TARGET StreamDeckMidiHelper POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${TEVM_DLL}" "$<TARGET_FILE_DIR:StreamDeckMidiHelper>")
  endif()

# --------------------------------------------------------------------------- #
# macOS: CoreMIDI virtual source (no driver) + CGEvent (keystrokes)
# --------------------------------------------------------------------------- #
elseif(APPLE)
  find_library(COREMIDI_LIB        CoreMIDI            REQUIRED)
  find_library(COREFOUNDATION_LIB  CoreFoundation      REQUIRED)
  find_library(APPSERVICES_LIB     ApplicationServices REQUIRED)
  target_link_libraries(StreamDeckMidiHelper PRIVATE
    ${COREMIDI_LIB} ${COREFOUNDATION_LIB} ${APPSERVICES_LIB})
endif()
