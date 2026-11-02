// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/settings/catalogue.hpp"
#include "adi/settings/registry.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace adi::ui {
// The Settings window's tabs (ADR-0203 d11; review A6-19, A7-29): Live's ten tab names in Live's
// order, a setting Live has on the tab where Live has it, and ADI's own pages after the ten. Look &
// Feel splits into Display & Input and Theme & Colors, Link has its own tab, and Updates and Privacy
// become Licenses & Updates with a Privacy group.
//
// The registry still names its pages its own way (window log, N14), so until it carries Live's
// names the window lays the registry's settings out on Live's tabs from what the registry and the
// catalogue already say, one rule each:
//  - a setting that answers one of the catalogue's Part II rows (Live 12's ten tabs, setting by
//    setting) sits on that row's tab, in the catalogue's order, which is Live's order on the tab;
//  - any other setting on a page Live has goes on Live's tab for that page, after Live's own;
//  - the rest keep their pages, ADI's own, after the ten, in the registry's order; but the LAN
//    audio node's defaults go on the Link tab, as ADR-0203 d11 places them.
// When the registry takes Live's names, the second rule maps each page to itself and nothing here
// changes.
inline constexpr std::array<const char *, 10> kLiveTabs{"Display & Input",  "Theme & Colors", "Audio",
                                                        "Link",             "Tempo & MIDI",   "File & Folder",
                                                        "Library",          "Plug-Ins",       "Record, Warp & Launch",
                                                        "Licenses & Updates"};
// A titled group of settings on a tab, as Live's sections.
struct SettingsGroup {
    std::string heading;
    std::vector<const settings::Setting *> settings;
};
struct SettingsTab {
    std::string name;
    bool live = false; // one of Live's ten
    std::vector<SettingsGroup> groups;
};
namespace settings_tabs_detail {
constexpr int kNone = -1;
// Live's tab for a registry page Live has, or kNone for a page that is ADI's own.
inline int tabForPage(std::string_view page) noexcept {
    for (std::size_t i = 0; i < kLiveTabs.size(); ++i)
        if (page == kLiveTabs[i])
            return static_cast<int>(i);
    // The registry's names for pages Live has under another name.
    if (page == "Look & Feel")
        return 0; // Display & Input, beside Theme & Colors, whose settings the catalogue names
    if (page == "MIDI")
        return 4;
    if (page == "Plug-ins")
        return 7;
    if (page == "Record")
        return 8;
    if (page == "Updates" || page == "Privacy")
        return 9;
    return kNone;
}
// The catalogue's Part II tab a row names ("1. Display & Input / Language"), or kNone.
inline int tabForRow(std::string_view row) noexcept {
    const auto section = row.substr(0, row.find(" / "));
    const auto dot = section.find(". ");
    if (dot == std::string_view::npos)
        return kNone;
    const auto name = section.substr(dot + 2);
    for (std::size_t i = 0; i < kLiveTabs.size(); ++i)
        if (name == kLiveTabs[i])
            return static_cast<int>(i);
    return kNone;
}
// The LAN audio node's defaults sit on the Link tab, labelled plainly as not Link Audio (ADR-0203
// d11, which comes after the catalogue's "the Sync page's LAN row").
inline bool lanAudio(const settings::Setting &s) noexcept { return s.key.rfind("sync.lan", 0) == 0; }
// A tab's groups: its own first, as Live heads a page with its name; then PTP beside Link (ADR-0145
// d7), the LAN audio node, and Privacy on Licenses & Updates (ADR-0203 d11).
inline std::string groupFor(const settings::Setting &s, const std::string &tab) {
    if (s.page == "Privacy")
        return "Privacy";
    if (s.key.rfind("sync.ptp", 0) == 0)
        return "PTP";
    if (lanAudio(s))
        return "LAN Audio (not Link Audio)";
    return tab;
}
inline std::vector<SettingsTab> build() {
    // Where Live has each setting: the first Part II row it answers, and that row's place.
    std::map<std::string, std::pair<int, int>> live;
    int place = 0;
    for (const auto &row : settings::answeredRows()) {
        const int tab = tabForRow(row.row);
        if (tab == kNone)
            continue;
        for (const auto &key : row.keys)
            live.emplace(key, std::make_pair(tab, place));
        ++place;
    }
    std::vector<SettingsTab> tabs;
    for (const char *name : kLiveTabs)
        tabs.push_back({name, true, {}});
    auto add = [](SettingsTab &tab, const std::string &heading, const settings::Setting *s) {
        auto group = std::find_if(tab.groups.begin(), tab.groups.end(),
                                  [&heading](const SettingsGroup &g) { return g.heading == heading; });
        if (group == tab.groups.end()) {
            tab.groups.push_back({heading, {}});
            group = tab.groups.end() - 1;
        }
        group->settings.push_back(s);
    };
    // Live's settings first, in Live's order on each tab.
    std::vector<std::pair<int, const settings::Setting *>> ordered;
    for (const auto &s : settings::registry())
        if (const auto at = live.find(s.key); at != live.end())
            ordered.emplace_back(at->second.second, &s);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[ignored, s] : ordered) {
        auto &tab = tabs[static_cast<std::size_t>(live.at(s->key).first)];
        add(tab, groupFor(*s, tab.name), s);
    }
    // Then the rest of each page Live has, on its tab; ADI's own pages after the ten.
    for (const auto &page : settings::pages()) {
        const int pageTab = tabForPage(page);
        for (const auto &s : settings::registry()) {
            if (s.page != page || live.count(s.key) != 0)
                continue;
            const int tab = lanAudio(s) ? 3 : pageTab;
            if (tab != kNone) {
                auto &onto = tabs[static_cast<std::size_t>(tab)];
                add(onto, groupFor(s, onto.name), &s);
                continue;
            }
            auto own = std::find_if(tabs.begin() + static_cast<std::ptrdiff_t>(kLiveTabs.size()), tabs.end(),
                                    [&page](const SettingsTab &t) { return t.name == page; });
            if (own == tabs.end()) {
                tabs.push_back({page, false, {}});
                own = tabs.end() - 1;
            }
            add(*own, page, &s);
        }
    }
    // A tab's own group comes first, whatever was added to it first.
    for (auto &tab : tabs)
        std::stable_partition(tab.groups.begin(), tab.groups.end(),
                              [&tab](const SettingsGroup &g) { return g.heading == tab.name; });
    return tabs;
}
} // namespace settings_tabs_detail
// Every tab, Live's ten first. Built once from the registry and the catalogue, which never change
// while the application runs.
inline const std::vector<SettingsTab> &settingsTabs() {
    static const std::vector<SettingsTab> tabs = settings_tabs_detail::build();
    return tabs;
}
inline const SettingsTab *settingsTab(std::string_view name) {
    for (const auto &tab : settingsTabs())
        if (tab.name == name)
            return &tab;
    return nullptr;
}
// The tab a setting is shown on; empty for none.
inline std::string tabOf(const settings::Setting &s) {
    for (const auto &tab : settingsTabs())
        for (const auto &group : tab.groups)
            if (std::find(group.settings.begin(), group.settings.end(), &s) != group.settings.end())
                return tab.name;
    return {};
}
} // namespace adi::ui
