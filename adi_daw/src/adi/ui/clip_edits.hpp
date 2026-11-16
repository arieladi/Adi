// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Clip edits the Clip View and the arrangement share, headless (STEP-7-PLAN 7.7): each builds one
// batch from the snapshot the action lands on (ADR-0201), for the window to commit as one undo step.
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/track_edits.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace adi::ui {
// Live's Insert Empty MIDI Clip(s) (Create menu, Ctrl/Cmd+Shift+M; manual 10.3): an empty MIDI clip
// across the time `start` to `end` on each of `tracks` that is a MIDI track. A track that already
// holds a clip in that time is refused, so nothing is overwritten (Live replaces what is there:
// to be measured before ADI does).
inline TrackEdit insertMidiClips(const engine::Snapshot &snapshot, const std::vector<std::int64_t> &tracks,
                                 std::int64_t start, std::int64_t end) {
    using track_edits_detail::op;
    TrackEdit edit;
    if (end <= start) {
        edit.refusal = "Select a time range on a MIDI track to insert a clip.";
        return edit;
    }
    auto id = audio_drop_detail::newClipId(snapshot);
    for (const auto &t : snapshot.tracks) {
        if (!t || std::find(tracks.begin(), tracks.end(), t->id) == tracks.end() || !isMidiTrack(*t))
            continue;
        for (const auto &c : t->clips)
            if (c && c->posTicks < end && c->posTicks + c->lengthTicks > start) {
                edit.ops.clear();
                edit.refusal = "There is already a clip in that time on " + t->name + ".";
                return edit;
            }
        edit.ops.push_back(op("clip.create",
                              {{"id", id}, {"track", t->id}, {"kind", "midi"}, {"pos", start}, {"length", end - start}},
                              "Insert MIDI Clip"));
        if (edit.clip == 0) {
            edit.select = t->id;
            edit.clip = id;
        }
        ++id;
    }
    if (edit.ops.empty())
        edit.refusal = "Insert Empty MIDI Clip needs a MIDI track.";
    return edit;
}
} // namespace adi::ui
