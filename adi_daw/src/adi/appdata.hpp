// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where application data lives -- ADR-0149.
//
// Three kinds, each where its platform expects it:
//
//   config   settings a user chose. Per APPLICATION: ADI DAW, ADI Live and
//            aDiJ never read each other's (ADR-0145 d10).
//   data     what the suite learned on this machine, SHARED by the three
//            applications: the library index (ADR-0145 d11), the plugin
//            capabilities registry, the AudioGridder catalogue.
//   cache    what can be rebuilt: decoded audio, peaks. Shared.
//
//   Windows  config %APPDATA%\ADI\<App>     data %LOCALAPPDATA%\ADI\Shared
//            cache  %LOCALAPPDATA%\ADI\Cache
//   macOS    config ~/Library/Application Support/ADI/<App>
//            data   ~/Library/Application Support/ADI/Shared
//            cache  ~/Library/Caches/ADI
//   Linux    config $XDG_CONFIG_HOME/adi/<app>   (~/.config)
//            data   $XDG_DATA_HOME/adi/shared    (~/.local/share)
//            cache  $XDG_CACHE_HOME/adi          (~/.cache)
//
// `ADI_HOME`, when set, puts all three under one folder -- tests, and a
// portable install on a stick. Nothing here creates a directory: a caller that
// writes creates what it writes into.
#pragma once

#include <filesystem>
#include <string>

namespace adi::appdata {

enum class App { Daw, Live, DJ };

/// "ADI DAW", "ADI Live", "aDiJ": the folder names on Windows and macOS.
[[nodiscard]] const char* displayName(App app) noexcept;

struct Paths {
    std::filesystem::path config;   ///< this application's own settings
    std::filesystem::path data;     ///< shared by the suite
    std::filesystem::path cache;    ///< shared, and safe to delete
};

/// The folders for `app` on this machine. Empty paths when the platform gives
/// no home at all (a service account with no profile): the caller then keeps
/// its data in memory and says so, rather than writing somewhere surprising.
[[nodiscard]] Paths pathsFor(App app);

/// The shared data folder's two files that exist today.
[[nodiscard]] std::filesystem::path pluginRegistryFile();   ///< data/plugins.sqlite
[[nodiscard]] std::filesystem::path libraryIndexFile();     ///< data/library.sqlite

}  // namespace adi::appdata
