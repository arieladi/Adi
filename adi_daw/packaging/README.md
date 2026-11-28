# ADI base installer

The `adi_base` install component contains ADI, its plug-in scanner, licence files and the original starter drum kit. The scanner stays beside ADI so plug-in discovery works from the installed copy. This is the base tier of ADR-0204. A binary release should accompany the exact corresponding source and build instructions, including the pinned dependencies in `tools/fetch_external.sh`. The kit contains twelve synthesized hits and four one-bar loops. Recreate its WAV files with `python3 adi_daw/tools/generate_starter_drums.py`.

## Linux

Build from the repository root with CMake, Ninja and the audio and X11 development packages listed in `.github/workflows/ui.yml`.

```sh
bash adi_daw/tools/fetch_external.sh --with-juce
cmake -S adi_daw -B b -G Ninja -DCMAKE_BUILD_TYPE=Release -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON
cmake --build b --target adi_app
sh adi_daw/packaging/linux/install.sh b
```

This uses `update-mime-database` and `update-desktop-database` from `shared-mime-info` and `desktop-file-utils`. The default install prefix is `~/.local`. Pass a prefix as the second argument to install elsewhere. The desktop session needs the chosen `bin` directory on its search path and the matching `share` directory in its XDG data search path. The normal system prefixes and the default XDG user data directory already have the data search path.

ADI appears as a handler for `.adi` files. Choose ADI through the file manager or use this command to make it your default.

```sh
xdg-mime default org.adi.daw.desktop application/x-adi-project
```

With ADI closed, opening the file from the desktop launches ADI with that project. Use File > Open while ADI is running on Linux. The Linux JUCE backend does not forward a second launch to the running instance.

For a distribution package, stage `cmake --install b --config Release --prefix /usr --component adi_base` with `DESTDIR` set to the package root. The distribution's package triggers refresh the MIME and desktop databases when the package is installed.

To check a build without changing your desktop, build `adi_ui_document_tests` and run `sh adi_daw/packaging/linux/test-install.sh b`. It uses an isolated prefix and checks the installed binaries, licence, desktop registration and project MIME signature with `gio`. Set `ADI_INSTALL_CONFIG=Debug` to check a Debug build.

## Windows

Use Windows x64 with the Visual Studio 2022 C++ build tools, a Windows SDK, CMake, Ninja, Git Bash for fetching pinned dependencies, and NSIS 3. Open a Visual Studio Developer PowerShell. Build the Release configuration so the installed executable uses the release Visual C++ runtime.

```powershell
bash adi_daw/tools/fetch_external.sh --with-juce
cmake -S adi_daw -B C:/b/adi -G Ninja -DCMAKE_BUILD_TYPE=Release -DADI_WITH_JUCE=ON -DADI_WITH_PD=ON -DADI_WERROR=ON
cmake --build C:/b/adi --target adi_app
$stage = Join-Path $env:TEMP ("adi-stage-" + [guid]::NewGuid())
$setup = Join-Path (Get-Location) 'ADI-Setup.exe'
$makensis = Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'
cmake --install C:/b/adi --config Release --prefix "$stage" --component adi_base
$version = Select-String -Path adi_daw/CMakeLists.txt -Pattern '^project\(adi_daw .* VERSION (\d+\.\d+\.\d+)\)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }
& $makensis "/DADI_STAGE=$stage" "/DADI_VERSION=$version" "/DADI_SETUP=$setup" adi_daw/packaging/windows/adi.nsi
```

The installer runs per user and registers ADI in Open with and Default apps. With no existing `.adi` handler, ADI becomes the fallback. Windows retains an existing default until the user changes it. Uninstall removes ADI's own entries and keeps projects and application data.

On a Windows test machine with the current x64 Visual C++ redistributable installed, check a fresh install and an upgrade. Open a project whose path contains spaces from Explorer with ADI closed and again while ADI is open. Confirm that Windows keeps a pre-existing `.adi` default, that ADI remains selectable in Open with, and that uninstall removes only ADI's entries.
