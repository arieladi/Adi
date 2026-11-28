// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/audio/warp.hpp"
#include "adi/settings/store.hpp"
#include <array>
#include <cstdint>
#include <string>
namespace adi::ui {
struct WarpModeChoice {
    audio::WarpMode mode;
    const char *name;
};
inline constexpr std::array<WarpModeChoice, 6> kWarpModeChoices{{
    {audio::WarpMode::Beats, "Beats"}, {audio::WarpMode::Tones, "Tones"}, {audio::WarpMode::Texture, "Texture"},
    {audio::WarpMode::RePitch, "Re-Pitch"}, {audio::WarpMode::Complex, "Complex"}, {audio::WarpMode::ComplexPro, "Complex Pro"},
}};
// ADI's import prompt (E5; Live 12 manual 9.2.1, 9.2.2, 5.7; ADR-0132 d6): what a dropped audio file
// is imported with. All off by default: no Warp, no fades on the clip's edges, the file referred to
// where it is (Live's Collect Files "never"). The choices live in the settings, so "use the last
// ones" reads what the prompt last wrote.
struct ImportChoices {
    bool warp = false;                            // record.importAutoWarp
    audio::WarpMode mode = audio::WarpMode::Beats; // record.defaultWarpMode
    bool edgeFades = false;                       // record.importEdgeFades
    bool copyIn = false;                          // files.copyImportedMedia == "always"
    // Whether the import needs the engine's material import, or the plain drop does.
    bool any() const noexcept { return warp || edgeFades || copyIn; }
    friend bool operator==(const ImportChoices &, const ImportChoices &) = default;
};
inline ImportChoices importChoicesFrom(const settings::AppSettings &settings) {
    ImportChoices out;
    const auto flag = [&](const char *key) {
        const auto v = settings.get(key);
        return v.is_boolean() && v.get<bool>();
    };
    out.warp = flag("record.importAutoWarp");
    out.edgeFades = flag("record.importEdgeFades");
    if (const auto mode = settings.get("record.defaultWarpMode"); mode.is_string())
        if (const auto parsed = audio::parseWarpMode(mode.get<std::string>()))
            out.mode = *parsed;
    const auto copy = settings.get("files.copyImportedMedia");
    out.copyIn = copy.is_string() && copy.get<std::string>() == "always";
    return out;
}
// Whether `files.copyImportedMedia` is "ask": the copy is asked on every drop, and the prompt's
// answer is not kept over it.
inline bool copyAsks(const settings::AppSettings &settings) {
    const auto v = settings.get("files.copyImportedMedia");
    return v.is_string() && v.get<std::string>() == "ask";
}
// The prompt shows while `editing.importOptions` is "ask" (its default), or while the copy is to be
// asked; otherwise the last choices are used without asking.
inline bool importAsks(const settings::AppSettings &settings) {
    const auto v = settings.get("editing.importOptions");
    return !v.is_string() || v.get<std::string>() == "ask" || copyAsks(settings);
}
// Save the last choices. Turning off the prompt also replaces a copy setting of "ask"
// with the chosen copy policy so it does not reopen the prompt.
// `nowUtc` (microseconds) dates the settings' change log, as the Settings window does.
inline bool keepImportChoices(settings::AppSettings &settings, const ImportChoices &choices, std::string &why,
                              std::int64_t nowUtc = 0, bool stopAsking = false) {
    settings::ChangeContext who;
    who.timeUtc = nowUtc;
    return settings.set("record.importAutoWarp", choices.warp, who, why) &&
           settings.set("record.importEdgeFades", choices.edgeFades, who, why) &&
           settings.set("record.defaultWarpMode", std::string(audio::warpModeName(choices.mode)), who, why) &&
           ((!stopAsking && copyAsks(settings)) ||
            settings.set("files.copyImportedMedia", std::string(choices.copyIn ? "always" : "never"), who, why)) &&
           (!stopAsking || settings.set("editing.importOptions", std::string("never"), who, why)) &&
           settings.save(why);
}
} // namespace adi::ui
