// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
// Live's file drop on the arrangement (manual 6.1; the drop area's "Drop Files and Devices Here"):
// an audio file dropped on an audio track makes a clip there, starting where it was dropped; one
// dropped on the drop area below the tracks makes a new audio track holding it. One batch, so one
// undo step: the media row the import captured (#308, N5), the new track if there is one, the clip
// and its audio, Warp off, the whole file. The batch is built from the snapshot the drop lands on
// (ADR-0201). Reading the file and the media pool is the document's (`ProjectDocument::
// prepareImport`): the window never reads the store.
//
// What the document's capture found, for the batch.
struct AudioImport {
    std::int64_t mediaId = 0;
    std::int64_t sampleRate = 0, frames = 0; // the file's own, never the project's (SPEC §4.3)
    OpRequest media;                         // the captured op; no opType when the pool row is reused as it is
    // In: the least id a new pool row may take. Several files dropped at once are captured before
    // their one batch lands, so each after the first starts past the ids the earlier ones took.
    std::int64_t firstFreeId = 0;
};
// One file of a drop: its clip's name and what its capture found.
struct DroppedAudio {
    std::string name;
    AudioImport import;
};
namespace audio_drop_detail {
inline std::int64_t newClipId(const engine::Snapshot &snapshot) noexcept {
    std::int64_t id = 1;
    for (const auto &t : snapshot.tracks)
        if (t)
            for (const auto &c : t->clips)
                if (c)
                    id = std::max(id, c->id + 1);
    return id;
}
} // namespace audio_drop_detail
// Why a file can't be dropped at `track` (0: the drop area, which makes a track); empty when it can.
// Checked before the file is read, so a refused drop costs nothing.
inline std::string audioDropRefusal(const engine::Snapshot &snapshot, std::int64_t track) {
    if (track == 0)
        return {};
    const auto *t = snapshot.findTrack(track);
    if (t == nullptr)
        return "Drop the file on an audio track, or below the tracks for a new one.";
    if (t->kind != "audio")
        return "An audio file goes on an audio track. Drop it on one, or below the tracks for a new one.";
    return {};
}
// The batch for files dropped at `track` (0: a new audio track at the end) and `pos`. One file makes
// one clip there, named after it; its length is the file's at the tempo in force where it starts, as
// Warp off plays it. Several, as Live places them: one after another on that track; with
// `separate` (Ctrl/Cmd held), the first there and each of the others on a new audio track of its
// own at the end, all at `pos`. One batch, one undo step: the pool rows, the new tracks, the clips.
// The first clip and its track are selected.
inline TrackEdit dropAudio(const engine::Snapshot &snapshot, std::int64_t track, std::int64_t pos,
                           std::vector<DroppedAudio> files, bool separate = false,
                           const RecordingDefaults *defaults = nullptr) {
    using namespace track_edits_detail;
    constexpr const char *label = "Insert Audio Clip";
    TrackEdit edit;
    edit.refusal = audioDropRefusal(snapshot, track);
    if (!edit.refusal.empty())
        return edit;
    if (files.empty()) {
        edit.refusal = "Drop an audio file.";
        return edit;
    }
    for (const auto &f : files)
        if (f.import.sampleRate <= 0 || f.import.frames <= 0) {
            edit.refusal = "That file holds no audio ADI can play.";
            return edit;
        }
    if (!snapshot.tempo) {
        edit.refusal = "The project has no tempo yet.";
        return edit;
    }
    pos = std::max<std::int64_t>(0, pos);
    const auto &tempo = *snapshot.tempo;
    for (auto &f : files)
        if (!f.import.media.opType.empty()) {
            f.import.media.label = label;
            edit.ops.push_back(std::move(f.import.media));
        }
    // New tracks take the ids after the project's, and stand after its tracks, in order: as Insert
    // Audio Track names one (manual 18.2), '#' shows its number.
    std::int64_t nextTrack = 1;
    for (const auto &t : snapshot.tracks)
        if (t)
            nextTrack = std::max(nextTrack, t->id + 1);
    auto index = static_cast<std::int64_t>(snapshot.tracks.size());
    auto newTrack = [&]() {
        const auto id = nextTrack++;
        edit.ops.push_back(op("track.create",
                              {{"id", id}, {"kind", "audio"}, {"name", "# Audio"}, {"index", index++}}, label));
        // A new track takes the registry's recording defaults, as Insert Audio Track's does (N31).
        if (defaults != nullptr)
            for (auto &o : recordingDefaultOps(id, *defaults, label))
                edit.ops.push_back(std::move(o));
        return id;
    };
    if (track == 0)
        track = newTrack();
    auto clip = audio_drop_detail::newClipId(snapshot);
    std::int64_t at = pos;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto &f = files[i];
        const auto on = separate && i > 0 ? newTrack() : track;
        const auto start = separate ? pos : at;
        const double seconds = static_cast<double>(f.import.frames) / static_cast<double>(f.import.sampleRate);
        const auto length = std::max<std::int64_t>(1, tempo.secondsToTicks(tempo.ticksToSeconds(start) + seconds) - start);
        edit.ops.push_back(op("clip.create",
                              {{"id", clip}, {"track", on}, {"kind", "audio"}, {"name", f.name}, {"pos", start}, {"length", length}},
                              label));
        edit.ops.push_back(
            op("clip.attachAudio", {{"id", clip}, {"media", f.import.mediaId}, {"frames", f.import.frames}}, label));
        if (i == 0) {
            edit.select = on;
            edit.clip = clip;
        }
        ++clip;
        at = start + length;
    }
    return edit;
}
// One file dropped at `track` and `pos`, named `name`.
inline TrackEdit dropAudio(const engine::Snapshot &snapshot, std::int64_t track, std::int64_t pos,
                           const std::string &name, AudioImport import) {
    std::vector<DroppedAudio> files;
    files.push_back({name, std::move(import)});
    return dropAudio(snapshot, track, pos, std::move(files));
}
} // namespace adi::ui
