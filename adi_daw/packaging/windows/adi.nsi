; SPDX-License-Identifier: AGPL-3.0-or-later
; Run makensis against a Release install of the adi_base component.
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef ADI_STAGE
    !error "Set ADI_STAGE to the staged install directory"
!endif
!ifndef ADI_VERSION
    !error "Set ADI_VERSION to the build's major.minor.patch version"
!endif
!ifndef ADI_SETUP
    !error "Set ADI_SETUP to the output installer path"
!endif

!define ADI_INSTALL "Software\ADI\Installer"
!define ADI_UNINSTALL "Software\Microsoft\Windows\CurrentVersion\Uninstall\ADI"
!define ADI_PROGID "ADI.Project"
!define ADI_CLASSES "Software\Classes"

Name "ADI"
OutFile "${ADI_SETUP}"
InstallDir "$LOCALAPPDATA\Programs\ADI"
InstallDirRegKey HKCU "${ADI_INSTALL}" "InstallLocation"
RequestExecutionLevel user
VIProductVersion "${ADI_VERSION}.0"
VIAddVersionKey "ProductName" "ADI"
VIAddVersionKey "FileDescription" "ADI installer"
VIAddVersionKey "FileVersion" "${ADI_VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (C) ADI contributors"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "ADI"
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    File "${ADI_STAGE}/bin/ADI.exe"
    File "${ADI_STAGE}/bin/adi_plugin_scanner.exe"
    SetOutPath "$INSTDIR\starter-drums"
    File /r "${ADI_STAGE}/share/adi/starter-drums/*"
    SetOutPath "$INSTDIR\ADI Licenses"
    File /r "${ADI_STAGE}/share/doc/adi/licenses/*"

    SetOutPath "$INSTDIR"
    CreateDirectory "$SMPROGRAMS\ADI"
    CreateShortcut "$SMPROGRAMS\ADI\ADI.lnk" "$INSTDIR\ADI.exe"
    WriteUninstaller "$INSTDIR\Uninstall ADI.exe"
    WriteRegStr HKCU "${ADI_INSTALL}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${ADI_UNINSTALL}" "DisplayName" "ADI"
    WriteRegStr HKCU "${ADI_UNINSTALL}" "DisplayVersion" "${ADI_VERSION}"
    WriteRegStr HKCU "${ADI_UNINSTALL}" "DisplayIcon" "$INSTDIR\ADI.exe"
    WriteRegStr HKCU "${ADI_UNINSTALL}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${ADI_UNINSTALL}" "UninstallString" '"$INSTDIR\Uninstall ADI.exe"'
    WriteRegDWORD HKCU "${ADI_UNINSTALL}" "NoModify" 1
    WriteRegDWORD HKCU "${ADI_UNINSTALL}" "NoRepair" 1

    WriteRegStr HKCU "${ADI_CLASSES}\${ADI_PROGID}" "" "ADI Project"
    WriteRegStr HKCU "${ADI_CLASSES}\${ADI_PROGID}\DefaultIcon" "" '"$INSTDIR\ADI.exe",0'
    WriteRegStr HKCU "${ADI_CLASSES}\${ADI_PROGID}\shell\open\command" "" '"$INSTDIR\ADI.exe" "%1"'
    WriteRegStr HKCU "${ADI_CLASSES}\.adi\OpenWithProgids" "${ADI_PROGID}" ""
    ReadRegStr $0 HKCR ".adi" ""
    ${If} $0 == ""
        WriteRegStr HKCU "${ADI_CLASSES}\.adi" "" "${ADI_PROGID}"
    ${EndIf}
    WriteRegStr HKCU "${ADI_INSTALL}\Capabilities" "ApplicationName" "ADI"
    WriteRegStr HKCU "${ADI_INSTALL}\Capabilities" "ApplicationDescription" "Digital audio workstation"
    WriteRegStr HKCU "${ADI_INSTALL}\Capabilities\FileAssociations" ".adi" "${ADI_PROGID}"
    WriteRegStr HKCU "Software\RegisteredApplications" "ADI" "${ADI_INSTALL}\Capabilities"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    ReadRegStr $0 HKCU "${ADI_INSTALL}" "InstallLocation"
    ${If} $0 == "$INSTDIR"
        ReadRegStr $0 HKCU "${ADI_CLASSES}\.adi" ""
        ${If} $0 == "${ADI_PROGID}"
            DeleteRegValue HKCU "${ADI_CLASSES}\.adi" ""
        ${EndIf}
        DeleteRegValue HKCU "${ADI_CLASSES}\.adi\OpenWithProgids" "${ADI_PROGID}"
        DeleteRegKey /ifempty HKCU "${ADI_CLASSES}\.adi\OpenWithProgids"
        DeleteRegKey /ifempty HKCU "${ADI_CLASSES}\.adi"
        DeleteRegKey HKCU "${ADI_CLASSES}\${ADI_PROGID}"
        DeleteRegValue HKCU "Software\RegisteredApplications" "ADI"
        DeleteRegKey HKCU "${ADI_INSTALL}"
        DeleteRegKey HKCU "${ADI_UNINSTALL}"
        Delete "$SMPROGRAMS\ADI\ADI.lnk"
        RMDir "$SMPROGRAMS\ADI"
        System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ${EndIf}
    Delete "$INSTDIR\ADI.exe"
    Delete "$INSTDIR\adi_plugin_scanner.exe"
    Delete "$INSTDIR\Uninstall ADI.exe"
    RMDir /r "$INSTDIR\starter-drums"
    RMDir /r "$INSTDIR\ADI Licenses"
    RMDir "$INSTDIR"
SectionEnd
