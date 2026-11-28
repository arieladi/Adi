# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned source archive, fetched and hash-checked by tools/fetch_external.sh.
set(ADI_LAME_SOURCE "${ADI_THIRD_PARTY}/lame-3.100" CACHE PATH "LAME 3.100 source for MP3 export")
target_sources(adi_core PRIVATE src/adi/render/mp3_writer.cpp)
if(EXISTS "${ADI_LAME_SOURCE}/include/lame.h")
    # The portable encoder from libmp3lame/Makefile.am, with no frontend,
    # optional assembly, or decoder. ADI verifies with its own PCM reader.
    set(adi_lame_sources
        VbrTag bitstream encoder fft gain_analysis id3tag lame newmdct
        presets psymodel quantize quantize_pvt reservoir set_get tables takehiro
        util vbrquantize version mpglib_interface)
    list(TRANSFORM adi_lame_sources PREPEND "${ADI_LAME_SOURCE}/libmp3lame/")
    list(TRANSFORM adi_lame_sources APPEND ".c")
    add_library(adi_lame STATIC ${adi_lame_sources})
    target_include_directories(adi_lame SYSTEM PUBLIC "${ADI_LAME_SOURCE}/include")
    # The upstream configure step normally declares these native IEEE types.
    set(adi_lame_config "${CMAKE_CURRENT_BINARY_DIR}/generated/lame")
    file(MAKE_DIRECTORY "${adi_lame_config}")
    file(WRITE "${adi_lame_config}/config.h"
        "typedef float ieee754_float32_t;\ntypedef double ieee754_float64_t;\n")
    target_include_directories(adi_lame PRIVATE "${adi_lame_config}")
    target_compile_definitions(adi_lame PRIVATE HAVE_CONFIG_H=1 STDC_HEADERS=1 HAVE_STDINT_H=1
        HAVE_ERRNO_H=1 HAVE_FCNTL_H=1 LAME_LIBRARY_BUILD=1)
    if(MSVC)
        target_compile_definitions(adi_lame PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(NOT WIN32)
        target_link_libraries(adi_lame PRIVATE m)
    endif()
    target_link_libraries(adi_core PRIVATE adi_lame)
    target_compile_definitions(adi_core PRIVATE ADI_HAVE_LAME=1)
endif()
