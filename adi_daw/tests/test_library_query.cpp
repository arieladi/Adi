// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/library_query.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/material_commands.hpp"
#include "adi/store.hpp"
#include "adi/ui/plugin_listing.hpp"
#include "adi/ui/project_library.hpp"
#include "temp_directory.hpp"
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

void currentProjectOrdering() {
    const BrowserLibrarySnapshot shared({{}, {{"Alpha Kick.wav", "a.wav", {}},
                                              {"Other.wav", "same.wav", {}},
                                              {"Middle Kick.wav", "middle.wav", {}},
                                              {"Shared Kick.wav", "same.wav", {}},
                                              {"Late Kick.wav", "late.wav", {}}}, {}});
    const BrowserLibrarySnapshot project({{}, {{"Project Alias Kick.wav", "same.wav", {}},
                                               {"Extra Kick.wav", "extra.wav", {}},
                                               {"Project Late Kick.wav", "late.wav", {}},
                                               {"Second Extra Kick.wav", "extra2.wav", {}},
                                               {"Repeated Alias Kick.wav", "same.wav", {}}}, {}});
    LibraryQueryResults results;
    queryLibrary(LibraryQuery{"kick"}, {&shared, {}, {}, nullptr, &project}, results);
    const auto &indexed = shared.listing().samples;
    const auto &local = project.listing().samples;
    check(results.samples == std::vector{&indexed[0], &indexed[2], &local[0], &local[2], &local[1], &local[3]},
          "SC-134: project names replace the first matching shared paths in place and extra files keep project order");
    queryLibrary(LibraryQuery{"alias project"}, {&shared, {}, {}, nullptr, &project}, results);
    check(results.samples == std::vector{&local[0]}, "SC-134: a project display name can match by itself");
}

void currentProjectAudio() {
    namespace fs = std::filesystem;
    const test::TempDirectory temp("library_query", "current_project");
    const auto folder = fs::canonical(temp.path()) / "Song";
    const auto recorded = folder / "Samples" / "Recorded";
    const auto legacy = folder / "audio";
    fs::create_directories(recorded);
    fs::create_directory(legacy);
    StoreError error{};
    auto store = Store::createNewProject(folder / "Song.adi", error);
    check(store != nullptr, "SC-134: create a song to publish its audio");
    if (!store)
        return;
    const auto take = recorded / "Lead 0001.wav", oldTake = legacy / "recording-7.wav";
    const auto unowned = legacy / "Another song.wav", linked = folder.parent_path() / "Linked.wav";
    const auto write = [](const fs::path &file, float level) {
        std::array<float, 960> samples;
        samples.fill(level);
        audio::WavWriter writer(file, 48000, 1, audio::WavFormat::Float32);
        writer.write(samples.data(), static_cast<std::uint32_t>(samples.size()));
    };
    write(take, .1f);
    write(oldTake, .2f);
    write(unowned, .3f);
    write(linked, .4f);

    ProjectLibrary project;
    const auto before = project.update(*store);
    check(before->listing().samples.empty(), "SC-134: existing files in a shared project folder do not imply ownership");
    const auto publish = [&](const fs::path &file, std::int64_t id) {
        OpRequest request;
        const auto prepared = media::prepareImport(*store, file, id, 0, request);
        return prepared.ok && OpJournal(*store).commit(request).ok;
    };
    check(publish(take, 101) && publish(oldTake, 102) && publish(linked, 103), "SC-134: commit two takes and one external link");
    const auto takes = project.update(*store);
    check(takes != before && takes->listing().samples.size() == 2 && project.update(*store) == takes,
          "SC-134: the media commit refreshes the publisher and an unchanged store reuses its publication");
    if (takes->listing().samples.size() != 2)
        return;
    check(takes->listing().samples[0].path == take && takes->listing().samples[1].path == oldTake,
          "SC-134: the project lists current and legacy recordings with their owned paths");

    const auto model = rows::readModel(*store);
    const auto track = std::ranges::find(model.tracks, "audio", &rows::Track::kind);
    check(track != model.tracks.end(), "SC-134: use the song's first audio track for a bounce");
    if (track == model.tracks.end())
        return;
    clip_edit::BounceOptions bounce;
    bounce.job.session = {2, 48000, 64};
    bounce.job.endSample = 960;
    bounce.job.encoding.format = audio::WavFormat::Float32;
    bounce.destination.track = track->id;
    bounce.destination.mediaId = 104;
    bounce.destination.clipId = 104;
    bounce.destination.at = {1, 0};
    auto pending = clip_edit::prepareBounce(*store, bounce);
    check(pending.edit.ok && project.update(*store) == takes, "SC-134: a prepared bounce stays off the list until commit");
    const auto committed = clip_edit::commitMaterial(*store, pending);
    check(committed.ok, committed.ok ? "SC-134: commit the bounced file" : committed.error.c_str());
    const auto bounced = project.update(*store);
    check(bounced != takes && bounced->listing().samples.size() == 3 && bounced->listing().samples.front().name == "bounce.wav" &&
              bounced->listing().samples.front().path.parent_path() == legacy,
          "SC-134: the same publisher lists the prepared bounce by original name after commit");
    if (bounced->listing().samples.size() != 3)
        return;

    const BrowserLibrarySnapshot shared({{}, {{"Lead 0001.wav", take, "shared"}}, {}});
    LibraryQueryResults results;
    queryLibrary(LibraryQuery{"0001 LEAD"}, {&shared, {}, {}, nullptr, bounced.get()}, results);
    check(results.samples == std::vector{&bounced->listing().samples[1]},
          "SC-134: All searches project recordings and deduplicates their paths against the shared library");
    auto other = Store::createNewProject(folder / "Other.adi", error);
    check(other && project.update(*other)->listing().samples.empty(),
          "SC-134: a different song sharing the same disk folder does not expose the first song's assets");
}
} // namespace

int main() {
    mixedLibrary();
    unicodeWords();
    currentProjectOrdering();
    currentProjectAudio();
    return failures ? 1 : 0;
}
