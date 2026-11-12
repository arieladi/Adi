if(TARGET adi_mix_eq)
    return()
endif()
# Authored EQ processing. Caller supplies the exact pinned Highway dependency.
if(NOT TARGET hwy)
    if(NOT ADI_MIX_EQ_HIGHWAY_SOURCE_DIR)
        message(FATAL_ERROR "ADI EQ requires pinned Highway 1.4.0 via ADI_MIX_EQ_HIGHWAY_SOURCE_DIR or existing hwy target")
    endif()
    set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(HWY_FORCE_STATIC_LIBS ON CACHE BOOL "" FORCE)
    add_subdirectory("${ADI_MIX_EQ_HIGHWAY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/highway")
    target_compile_definitions(hwy PUBLIC HWY_COMPILE_ONLY_STATIC)
    set_target_properties(hwy PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
set(eq_zl detail/ThirdParty/ZLEqualizer/source)
add_library(adi_mix_eq STATIC Core.cpp
    ${eq_zl}/zlp/controller.cpp
    ${eq_zl}/dsp/filter/ideal_filter/coeff/ideal_coeff.cpp
    ${eq_zl}/dsp/filter/iir_filter/coeff/ivantsov_coeff.cpp
    ${eq_zl}/dsp/filter/iir_filter/coeff/ivantsov_svf_coeff.cpp
    detail/Source/SpectralDynamics.cpp)
target_compile_features(adi_mix_eq PUBLIC cxx_std_20)
set_target_properties(adi_mix_eq PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(adi_mix_eq PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
target_compile_definitions(adi_mix_eq PRIVATE ZL_EQ_BAND_NUM=24 HWY_COMPILE_ONLY_STATIC)
target_include_directories(adi_mix_eq PRIVATE detail/Source ${eq_zl})
target_link_libraries(adi_mix_eq PRIVATE hwy)
target_include_directories(adi_mix_eq SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/detail"
    "$<TARGET_PROPERTY:hwy,INTERFACE_INCLUDE_DIRECTORIES>")
if(TARGET adi_warnings)
    # Apply the DAW's strict cross-platform policy to the owned facade/provider,
    # leaving the preserved imported numerical translation units unchanged.
    set_source_files_properties(Core.cpp Backend.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
endif()
if(MSVC)
    target_compile_options(adi_mix_eq PRIVATE /fp:precise)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(adi_mix_eq PRIVATE /clang:-ffp-model=precise /clang:-ffp-contract=off)
    endif()
else()
    target_compile_options(adi_mix_eq PRIVATE -ffp-contract=off)
endif()
if(MSVC)
    # Existing upstream diagnostic attributes, intentional analyser narrowing
    # and MSVC's unreachable-code warning in the imported filter switch paths.
    # DSP evaluation is preserved; keep these exceptions scoped to imported TUs.
    set_source_files_properties(
        ${eq_zl}/zlp/controller.cpp
        ${eq_zl}/dsp/filter/ideal_filter/coeff/ideal_coeff.cpp
        ${eq_zl}/dsp/filter/iir_filter/coeff/ivantsov_coeff.cpp
        ${eq_zl}/dsp/filter/iir_filter/coeff/ivantsov_svf_coeff.cpp
        detail/Source/SpectralDynamics.cpp
        PROPERTIES COMPILE_OPTIONS "/wd4100;/wd4127;/wd4244;/wd4334;/wd4702;/wd5030")
    target_compile_options(adi_mix_eq PRIVATE /W4 /WX)
endif()
