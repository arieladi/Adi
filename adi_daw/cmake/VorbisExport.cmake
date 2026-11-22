# SPDX-License-Identifier: GPL-3.0-or-later
# EX-04: Xiph Ogg and Vorbis releases fetched by tools/fetch_external.sh.
target_sources(adi_core PRIVATE src/adi/render/vorbis_writer.cpp)
if(EXISTS "${ADI_THIRD_PARTY}/ogg/CMakeLists.txt" AND
   EXISTS "${ADI_THIRD_PARTY}/vorbis/CMakeLists.txt")
    include(FetchContent)
    function(adi_add_vorbis)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_FRAMEWORK OFF)
        set(INSTALL_DOCS OFF)
        set(INSTALL_CMAKE_PACKAGE_MODULE OFF)
        # libvorbis's FindOgg searches installed libraries. Redirect it to the
        # reviewed in-tree Ogg target, including its generated headers.
        FetchContent_Declare(Ogg SOURCE_DIR "${ADI_THIRD_PARTY}/ogg"
            OVERRIDE_FIND_PACKAGE SYSTEM)
        FetchContent_MakeAvailable(Ogg)
        add_subdirectory("${ADI_THIRD_PARTY}/vorbis"
            "${CMAKE_BINARY_DIR}/third_party/vorbis" EXCLUDE_FROM_ALL SYSTEM)
    endfunction()
    adi_add_vorbis()
    target_link_libraries(adi_core PRIVATE vorbisenc)
    target_compile_definitions(adi_core PRIVATE ADI_WITH_VORBIS_EXPORT=1)
endif()
