// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/library_query.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/ui/plugin_listing.hpp"
#include <array>
#include <cstdio>

using namespace adi;
using namespace adi::ui;
namespace {
int failures = 0;
void check(bool passed, const char *message) {
    if (!passed) {
        std::fprintf(stderr, "%s\n", message);
        ++failures;
    }
}
void mixedLibrary() {
    const auto library = std::filesystem::path("Library");
    const auto drums = library / "Drums";
    const BrowserLibrarySnapshot snapshot({{{"Library", library, "library", true}, {"Drums", drums, "drums", false}},
                                          {{"808 Kick.wav", drums / "808 Kick.wav", "library"},
                                           {"808 Snare.wav", drums / "808 Snare.wav", "library"},
                                           {"Circuit Crunch.wav", library / "Circuit Crunch.wav", "library"},
                                           {"Kick Room.wav", library / "Kick Room.wav", "library"},
                                           {"Room.wav", (library / "808 Kick") / "Room.wav", "library"}},
                                          {}});
    const std::array devices{BuiltinInfo{"crunch", "Crunch Circuit", "Audio Effects", "Drive & Color"}};
    const std::array plugins{PluginEntry{"clap", "circuit", "Circuit", "Crunch Labs", "1", {}, PluginEntry::Kind::Effect},
                             PluginEntry{"vst3", "drive", "Drive", "Crunch Labs", "1", {}, PluginEntry::Kind::Effect},
                             PluginEntry{"vst3", "echo", "Circuit Echo", "Other", "1", {}, PluginEntry::Kind::Effect}};
    const std::map<std::string, std::vector<preset::Entry>> presets{
        {"crunch", {{"crunch", "CrUnCh cIrCuIt", library / "Crunch Circuit.adipreset"}}}};
    const LibraryQuerySources sources{&snapshot, devices, plugins, &presets};
    LibraryQueryResults results;
    queryLibrary(LibraryQuery{"  cIRcuit\tcrunch\n"}, sources, results);
    check(results.samples == std::vector{&snapshot.listing().samples[2]} &&
              results.builtins == std::vector{&devices[0]} && results.plugins == std::vector{&plugins[0]} &&
              results.presets == std::vector{&presets.begin()->second[0]},
          "SC-120 and SC-122: one query matches samples, built-ins, plug-ins and presets by all words");
    queryLibrary(LibraryQuery{"LAbS CIRCUIT"}, sources, results);
    check(results.samples.empty() && results.builtins.empty() && results.presets.empty() &&
              results.plugins == std::vector{&plugins[0]},
          "SC-122: a plug-in matches tokens from both its name and its maker");
    const LibraryQuery kick{"kick 808"};
    queryLibrary(kick, sources, results);
    check(results.samples == std::vector{&snapshot.listing().samples[0]} && results.plugins.empty(),
          "SC-122: reordered words find 808 Kick.wav, without matching directory names");
    const std::string volume = "drums";
    results.samples.clear();
    snapshot.appendSamples(kick, &volume, results.samples);
    check(results.samples == std::vector{&snapshot.listing().samples[0]},
          "SC-421: a nested Place searches a file indexed under its parent");
    snapshot.appendSamples(LibraryQuery{"808 snare"}, &volume, results.samples);
    check(results.samples == std::vector{&snapshot.listing().samples[0], &snapshot.listing().samples[1]},
          "SC-421: scoped matching appends stable references in source order");
    queryLibrary(LibraryQuery{" \t\r\n "}, sources, results);
    check(results.samples.size() == snapshot.listing().samples.size() && results.builtins.size() == devices.size() &&
              results.plugins.size() == plugins.size() && results.presets.size() == 1,
          "SC-120: a blank query lists all published source entries");
    queryLibrary(LibraryQuery{""}, {}, results);
    check(results.samples.empty() && results.builtins.empty() && results.plugins.empty() && results.presets.empty(),
          "SC-120: a repeated query replaces results when input publications are absent");
}

void unicodeWords() {
    const LibraryQuery query{"\xc3\x89" "CHO\xc2\xa0" "kick"};
    check(query.matches("Kick e\xcc\x81" "cho"),
          "SC-122: Unicode whitespace, case and equivalent spellings work in the same query");
    check(!query.matches("Echo Kick"), "SC-122: the query preserves the accent in a word");
}
} // namespace

int main() {
    mixedLibrary();
    unicodeWords();
    return failures ? 1 : 0;
}
