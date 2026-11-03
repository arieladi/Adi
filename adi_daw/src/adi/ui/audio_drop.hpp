// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
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
// The batch for a file dropped at `track` (0: a new audio track at the end) and `pos`, named
// `name`. Its length is the file's at the tempo in force where it starts, as Warp off plays it.
inline TrackEdit dropAudio(const engine::Snapshot &snapshot, std::int64_t track, std::int64_t pos,
                           const std::string &name, AudioImport import) {
    using namespace track_edits_detail;
    constexpr const char *label = "Insert Audio Clip";
    TrackEdit edit;
    edit.refusal = audioDropRefusal(snapshot, track);
    if (!edit.refusal.empty())
        return edit;
    if (import.sampleRate <= 0 || import.frames <= 0) {
        edit.refusal = "That file holds no audio ADI can play.";
        return edit;
    }
    if (!snapshot.tempo) {
        edit.refusal = "The project has no tempo yet.";
        return edit;
    }
    pos = std::max<std::int64_t>(0, pos);
    const auto &tempo = *snapshot.tempo;
    const double seconds = static_cast<double>(import.frames) / static_cast<double>(import.sampleRate);
    const auto length = std::max<std::int64_t>(1, tempo.secondsToTicks(tempo.ticksToSeconds(pos) + seconds) - pos);
    if (!import.media.opType.empty()) {
        import.media.label = label;
        edit.ops.push_back(std::move(import.media));
    }
    if (track == 0) {
        // As Insert Audio Track names a new one (manual 18.2): '#' shows its number.
        for (const auto &t : snapshot.tracks)
            if (t)
                track = std::max(track, t->id + 1);
        track = std::max<std::int64_t>(track, 1);
        edit.ops.push_back(op("track.create",
                              {{"id", track},
                               {"kind", "audio"},
                               {"name", "# Audio"},
                               {"index", static_cast<std::int64_t>(snapshot.tracks.size())}},
                              label));
    }
    const auto clip = audio_drop_detail::newClipId(snapshot);
    edit.ops.push_back(op("clip.create",
                          {{"id", clip}, {"track", track}, {"kind", "audio"}, {"name", name}, {"pos", pos}, {"length", length}},
                          label));
    edit.ops.push_back(op("clip.attachAudio", {{"id", clip}, {"media", import.mediaId}, {"frames", import.frames}}, label));
    edit.select = track;
    edit.clip = clip;
    return edit;
}
} // namespace adi::ui
