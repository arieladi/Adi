# SPDX-License-Identifier: GPL-3.0-or-later
# Opus 1.6.1 and libopusenc 0.3 are fetched at their reviewed tags.
target_sources(adi_core PRIVATE src/adi/render/opus_writer.cpp)
if(TARGET Ogg::ogg AND EXISTS "${ADI_THIRD_PARTY}/opus/CMakeLists.txt" AND
   EXISTS "${ADI_THIRD_PARTY}/libopusenc/include/opusenc.h")
    function(adi_add_opus)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TESTING OFF)
        set(OPUS_BUILD_PROGRAMS OFF)
        set(OPUS_BUILD_TESTING OFF)
        set(OPUS_BUILD_SHARED_LIBRARY OFF)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
        add_subdirectory("${ADI_THIRD_PARTY}/opus"
            "${CMAKE_BINARY_DIR}/third_party/opus" EXCLUDE_FROM_ALL SYSTEM)
    endfunction()
    adi_add_opus()
    set(opusenc_source "${ADI_THIRD_PARTY}/libopusenc")
    # libopusenc has no CMake project. Use its published Makefile.am source set.
    add_library(adi_opusenc STATIC
        "${opusenc_source}/src/ogg_packer.c"
        "${opusenc_source}/src/opus_header.c"
        "${opusenc_source}/src/opusenc.c"
        "${opusenc_source}/src/picture.c"
        "${opusenc_source}/src/resample.c"
        "${opusenc_source}/src/unicode_support.c")
    target_include_directories(adi_opusenc SYSTEM PUBLIC "${opusenc_source}/include")
    target_compile_definitions(adi_opusenc PRIVATE OPE_BUILD=1 OUTSIDE_SPEEX=1
        RANDOM_PREFIX=libopusenc PACKAGE_NAME="libopusenc" PACKAGE_VERSION="0.3")
    if(MSVC)
        target_compile_definitions(adi_opusenc PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    target_link_libraries(adi_opusenc PUBLIC Opus::opus)
    target_link_libraries(adi_core PRIVATE adi_opusenc Ogg::ogg)
    set_property(SOURCE src/adi/render/opus_writer.cpp APPEND PROPERTY COMPILE_DEFINITIONS ADI_HAVE_OPUS_EXPORT=1)
endif()
