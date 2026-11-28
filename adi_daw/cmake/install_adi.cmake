# SPDX-License-Identifier: AGPL-3.0-or-later
include(GNUInstallDirs)

# Install only the base DAW component. The scanner is found beside the executable.
install(TARGETS adi_app adi_plugin_scanner
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT adi_base)

set(_adi_starter_drums_data "${CMAKE_INSTALL_DATAROOTDIR}/adi/starter-drums")
install(DIRECTORY "${_adi_starter_drums}/" DESTINATION "${_adi_starter_drums_data}" COMPONENT adi_base
    FILES_MATCHING PATTERN "*.wav")
file(RELATIVE_PATH _adi_starter_drums_relative "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_DATAROOTDIR}/adi/starter-drums")
target_compile_definitions(adi_app PRIVATE ADI_STARTER_DRUMS_RELATIVE="${_adi_starter_drums_relative}")

set(_adi_license_dir "${CMAKE_INSTALL_DATAROOTDIR}/doc/adi/licenses")
install(FILES LICENSE docs/EXTERNAL-CODE.md DESTINATION "${_adi_license_dir}" COMPONENT adi_base)
install(DIRECTORY third_party_notices/ DESTINATION "${_adi_license_dir}/notices" COMPONENT adi_base)
install(FILES src/adi/mix/eq/detail/ThirdParty/ZLEqualizer/LICENSE.md
    DESTINATION "${_adi_license_dir}" RENAME ZLEqualizer.md COMPONENT adi_base)
install(FILES src/adi/mix/eq/detail/ThirdParty/ZLEqualizer/zldsp_fft/LICENSE.md
    DESTINATION "${_adi_license_dir}" RENAME zldsp_fft.md COMPONENT adi_base)

function(adi_install_dependency_notice relative name)
    install(FILES "${ADI_THIRD_PARTY}/${relative}" DESTINATION "${_adi_license_dir}"
        RENAME "${name}" COMPONENT adi_base)
endfunction()
adi_install_dependency_notice(JUCE/LICENSE.md JUCE.md)
adi_install_dependency_notice(JUCE/JUCE.spdx.json JUCE.spdx.json)
adi_install_dependency_notice(SQLiteCpp/LICENSE.txt SQLiteCpp.txt)
adi_install_dependency_notice(json/LICENSE.MIT nlohmann-json.txt)
adi_install_dependency_notice(link/LICENSE.md Link.md)
adi_install_dependency_notice(link/modules/asio-standalone/LICENSE_1_0.txt Asio.txt)
adi_install_dependency_notice(BLAKE3/LICENSE_CC0 BLAKE3.txt)
adi_install_dependency_notice(airwin2rack/LICENSE.md airwin2rack.md)
adi_install_dependency_notice(highway/LICENSE Highway.txt)
adi_install_dependency_notice(rubberband/COPYING RubberBand.txt)
adi_install_dependency_notice(libsamplerate/COPYING libsamplerate.txt)
adi_install_dependency_notice(clap/LICENSE CLAP.txt)
adi_install_dependency_notice(miniz/LICENSE miniz.txt)
adi_install_dependency_notice(utf8proc/LICENSE.md utf8proc.md)
adi_install_dependency_notice(dr_libs/LICENSE dr_libs.txt)
# The pinned PFFFT tree carries its notices at the head of the compiled files.
install(FILES "${ADI_THIRD_PARTY}/pffft/pffft.c" "${ADI_THIRD_PARTY}/pffft/fftpack.c"
    DESTINATION "${_adi_license_dir}/pffft" COMPONENT adi_base)
if(ADI_WITH_PD)
    adi_install_dependency_notice(libpd/LICENSE.txt libpd.txt)
    adi_install_dependency_notice(libpd/pure-data/LICENSE.txt PureData.txt)
    if(WIN32 AND TARGET adi_pthreads4w)
        adi_install_dependency_notice(pthreads4w/LICENSE pthreads4w.txt)
        adi_install_dependency_notice(pthreads4w/NOTICE pthreads4w-NOTICE.txt)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    install(FILES packaging/linux/org.adi.daw.desktop
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications" COMPONENT adi_base)
    install(FILES packaging/linux/org.adi.daw.xml
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/mime/packages" COMPONENT adi_base)
    install(FILES src/juce/assets/program.png RENAME org.adi.daw.png
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/512x512/apps" COMPONENT adi_base)
endif()
