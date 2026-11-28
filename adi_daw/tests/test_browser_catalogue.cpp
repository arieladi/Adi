// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include "adi/control/browser_catalogue.hpp"
#include "adi/device_presets.hpp"
#include "adi/ui/plugin_listing.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace adi;
using control::BrowserCatalogue;
using control::Json;

int failures = 0;

void check(bool value, const char* message) {
    if (value) return;
    ++failures;
    std::printf("FAIL %s\n", message);
}

Json rowNamed(const Json& page, std::string_view name) {
    // A plain loop and a string compared with a string: a json beside a string_view is ambiguous
    // to MSVC under C++20, and the range search could not be resolved with it.
    for (const Json& row : page.at("entries"))
        if (row.at("name").get_ref<const std::string&>() == name)
            return row;
    return Json::object();
}

void browserSources() {
    using Kind = ui::PluginEntry::Kind;
    ui::PluginListing listing;
    listing.plugins = {
        {"vst3", "fixture.effect", "Twin Delay", "Fixture Vendor", "2.0", "/private-module-a.vst3", Kind::Effect, 4},
        {"vst3", "fixture.effect", "Twin Echo", "Fixture Vendor", "2.1", "/private-module-b.vst3", Kind::Effect, 5},
        {"clap", "fixture.note", "Note Shaper", "", "1.0", "/private-module.clap", Kind::MidiEffect},
    };
    listing.complete = false;
    listing.stopped = "scan stopped opening /private-scanner-reason";
    listing.scanFailures.push_back({"/private-scan-failure", "private-error", scan::State::Failed});
    const preset::Identity builtin{"internal", "org.adidaw.builtin.sampler", std::nullopt};
    const auto builtinKey = preset::deviceKey(builtin);
    const auto pluginKey = preset::deviceKey({"vst3", "fixture.effect", 5});
    std::vector<preset::Entry> presets = {
        {builtinKey, "Warm Sampler", "/private-library-a.adipreset"},
        {pluginKey, "Cool Echo", "/private-library-b.adipreset"},
        {preset::deviceKey({"vst3", "fixture.effect", std::nullopt}), "Uninstalled Edition", "/private-orphan"},
    };
    const BrowserCatalogue snapshot(listing, presets);
    listing.plugins.clear();
    presets.clear();

    const auto page = snapshot.page();
    check(page.at("total") == builtinCatalog().size() + 5 && !page.contains("nextOffset"),
          "DF-C7 the snapshot keeps the Browser's built-ins, discovered plug-ins and matching presets");
    const auto sampler = rowNamed(page, "Sampler");
    const auto* samplerInfo = findBuiltin(builtin.uid);
    check(samplerInfo && sampler.value("kind", "") == "device" && sampler.value("format", "") == "internal" &&
              sampler.value("role", "") == "instrument" && sampler.value("available", false) == samplerInfo->available &&
              sampler.value("deviceKey", "") == builtinKey,
          "DF-C7 built-in identity, role and build availability come from builtinCatalog");
    const auto delay = rowNamed(page, "Twin Delay");
    const auto echo = rowNamed(page, "Twin Echo");
    const auto shaper = rowNamed(page, "Note Shaper");
    check(delay.value("shellId", 0) == 4 && echo.value("shellId", 0) == 5 &&
              echo.value("kind", "") == "plugin" && echo.value("role", "") == "effect" &&
              echo.value("vendor", "") == "Fixture Vendor" && echo.value("version", "") == "2.1" &&
              shaper.value("format", "") == "clap" && shaper.value("role", "") == "midi_effect",
          "DF-C7 the Browser's VST3 and CLAP identities and roles survive the read projection");
    const auto warm = rowNamed(page, "Warm Sampler");
    const auto cool = rowNamed(page, "Cool Echo");
    check(warm.value("deviceKey", "") == builtinKey && cool.value("deviceKey", "") == pluginKey &&
              cool.value("kind", "") == "preset" &&
              cool.value("device", Json::object()) == Json{{"format", "vst3"}, {"uid", "fixture.effect"}, {"shellId", 5}} &&
              rowNamed(page, "Uninstalled Edition").empty(),
          "DF-C7 preset pairing uses format, UID and shell member exactly like the Browser");
    check(control::dumpJson(page).find("private-") == std::string::npos,
          "DF-C7 plugin, preset and scan-failure paths and reasons stay out of the catalogue");
    const auto first = snapshot.page("FIXTURE VENDOR", 0, 2);
    const auto second = snapshot.page("fixture vendor", 2, 2);
    check(first.at("total") == 3 && first.at("nextOffset") == 2 && first.at("entries").size() == 2 &&
              second.at("entries").size() == 1 && rowNamed(second, "Cool Echo") == cool && !second.contains("nextOffset"),
          "DF-C7 a search is case-insensitive and matching preset rows follow their device across pages");
    check(snapshot.page("not present", 5).at("entries").empty(), "DF-C7 a query can return an empty page");
}

void boundedPages() {
    ui::PluginListing listing;
    for (int index = 0; index < 30; ++index) {
        listing.plugins.push_back({"clap", "fixture." + std::to_string(index),
                                   std::string(4096, 'x'), "Large Vendor", "1.0", {}});
    }
    const BrowserCatalogue snapshot(listing, {});
    const auto page = snapshot.page("large vendor");
    const auto count = page.at("entries").size();
    check(count > 0 && count < 30 && page.at("total") == 30 && page.at("nextOffset") == count &&
              control::dumpJson(page).size() < 65 * 1024,
          "DF-C7 a catalogue page stays bounded even when scanner labels are long");
    const auto next = snapshot.page("large vendor", count, 1);
    check(next.at("entries").size() == 1 && next.at("nextOffset") == count + 1,
          "DF-C7 byte-capped pages resume at the first row left out");
}
} // namespace

int main() {
    browserSources();
    boundedPages();
    if (failures == 0) std::puts("Browser catalogue checks passed");
    return failures ? 1 : 0;
}
