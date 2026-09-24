// SPDX-License-Identifier: GPL-3.0-or-later
//
// See appdata.hpp. ADR-0149. The one file where the platforms differ: each is
// read from the environment variables its own conventions name, so there is no
// platform API call to link against.

#include "adi/appdata.hpp"

#include <cstdlib>
#include <string>

namespace adi::appdata {

namespace {

namespace fs = std::filesystem;

// One environment read, silenced for MSVC at ONE site (as clap_host.cpp's
// envOr). On Windows the WIDE call: the narrow one returns the ANSI code page,
// and a profile folder with a Hebrew name would come back as question marks.
fs::path env(const char* name) {
#if defined(_WIN32)
    std::wstring wide;
    for (const char* c = name; *c != '\0'; ++c) wide.push_back(static_cast<wchar_t>(*c));   // names are ASCII
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const wchar_t* v = _wgetenv(wide.c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (v == nullptr || *v == L'\0') return {};
    return fs::path(std::wstring(v));
#else
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return {};
    return fs::path(std::string(v));
#endif
}

}  // namespace

const char* displayName(App app) noexcept {
    switch (app) {
        case App::Daw:  return "ADI DAW";
        case App::Live: return "ADI Live";
        case App::DJ:   return "aDiJ";
    }
    return "ADI DAW";
}

Paths pathsFor(App app) {
    Paths p;
    // A portable install, or a test: everything under one folder.
    if (const fs::path home = env("ADI_HOME"); !home.empty()) {
        p.config = home / "config" / displayName(app);
        p.data = home / "data" / "Shared";
        p.cache = home / "cache";
        return p;
    }
#if defined(_WIN32)
    fs::path roaming = env("APPDATA");
    fs::path local = env("LOCALAPPDATA");
    if (const fs::path profile = env("USERPROFILE"); !profile.empty()) {
        if (roaming.empty()) roaming = profile / "AppData" / "Roaming";
        if (local.empty()) local = profile / "AppData" / "Local";
    }
    if (roaming.empty() || local.empty()) return {};
    p.config = roaming / "ADI" / displayName(app);
    p.data = local / "ADI" / "Shared";
    p.cache = local / "ADI" / "Cache";
#elif defined(__APPLE__)
    const fs::path home = env("HOME");
    if (home.empty()) return {};
    const fs::path support = home / "Library" / "Application Support" / "ADI";
    p.config = support / displayName(app);
    p.data = support / "Shared";
    p.cache = home / "Library" / "Caches" / "ADI";
#else
    // XDG Base Directory: the variable when it is set and absolute, else the
    // default under $HOME. A relative XDG value is invalid by the spec.
    const fs::path home = env("HOME");
    auto xdg = [&](const char* var, const char* fallback) -> fs::path {
        fs::path v = env(var);
        if (!v.empty() && v.is_absolute()) return v;
        return home.empty() ? fs::path() : home / fallback;
    };
    const fs::path config = xdg("XDG_CONFIG_HOME", ".config");
    const fs::path data = xdg("XDG_DATA_HOME", ".local/share");
    const fs::path cache = xdg("XDG_CACHE_HOME", ".cache");
    if (config.empty() || data.empty() || cache.empty()) return {};
    const char* folder = app == App::Daw ? "adi-daw" : (app == App::Live ? "adi-live" : "adij");
    p.config = config / "adi" / folder;
    p.data = data / "adi" / "shared";
    p.cache = cache / "adi";
#endif
    return p;
}

fs::path pluginRegistryFile() {
    const Paths p = pathsFor(App::Daw);
    return p.data.empty() ? fs::path() : p.data / "plugins.sqlite";
}

fs::path libraryIndexFile() {
    const Paths p = pathsFor(App::Daw);
    return p.data.empty() ? fs::path() : p.data / "library.sqlite";
}

}  // namespace adi::appdata
