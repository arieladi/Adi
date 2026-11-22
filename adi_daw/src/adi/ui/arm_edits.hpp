// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace adi::ui {
// Arm and input monitoring from the window (MX-09; Live 12 manual 18.1 and 21.1, 21.4): the track's
// Arm switch, and its monitoring switch In, Auto and Off. Each change is one batch of ops on the
// tracks the click acts on, built from the state the click lands on, never from a guess.
//
// The snapshot does not carry a track's arm or its monitoring yet (window-fn log: the engine's
// snapshot fields), so the window reads them from the session's rows, `RecordStates` below, and the
// app fills it once per frame. `tracks.monitor_mode` stores 0 Off, 1 In, 2 Auto.
enum class Monitor : std::int64_t { Off = 0, In = 1, Auto = 2 };
// A track the window has no rows for (outside the application) shows Live's default, Auto. A track's
// own rows are always read as they stand: New's tracks and the window's new tracks are written Auto
// (N67); a bare `track.create` still stores Off until the engine's default changes.
struct TrackRecordState {
    bool armed = false;
    Monitor monitor = Monitor::Auto;
    friend bool operator==(const TrackRecordState &, const TrackRecordState &) = default;
};
using RecordStates = std::map<std::int64_t, TrackRecordState>;
inline TrackRecordState recordState(const RecordStates &states, std::int64_t track) {
    const auto it = states.find(track);
    return it == states.end() ? TrackRecordState{} : it->second;
}
// What can be armed: a track that holds clips (audio, MIDI, instrument). A group, a return and Main
// record nothing and have no Arm (manual 18.3).
inline bool armable(const engine::TrackNode &t) noexcept {
    return t.kind == "audio" || t.kind == "midi" || t.kind == "instrument";
}
// Live's Arm (manual 21.1): the clicked track's switch thrown. With Exclusive Arm on (the Record, Warp &
// Launch Settings, `record.exclusiveArm`, on by default as Live's) arming a track disarms every other,
// unless Ctrl/Cmd is held (`add`); with several tracks selected and the clicked one among them, all
// of them go to its new state together. One batch.
inline TrackEdit armTracks(const SnapshotTracks &tracks, const RecordStates &states, std::int64_t clicked,
                           const std::vector<std::int64_t> &selected, bool exclusive, bool add) {
    using namespace track_edits_detail;
    TrackEdit edit;
    const auto *t = find(tracks, clicked);
    if (t == nullptr || !armable(*t)) {
        edit.refusal = t == nullptr ? "Select a track to arm." : "Only audio and MIDI tracks record; this one has no Arm.";
        return edit;
    }
    const bool on = !recordState(states, clicked).armed;
    const char *label = on ? "Arm Track" : "Disarm Track";
    const bool together =
        selected.size() > 1 && std::find(selected.begin(), selected.end(), clicked) != selected.end();
    for (const auto &k : tracks) {
        if (!k || !armable(*k))
            continue;
        const bool acted = together ? std::find(selected.begin(), selected.end(), k->id) != selected.end()
                                    : k->id == clicked;
        const bool armed = recordState(states, k->id).armed;
        if (acted ? armed != on : on && exclusive && !add && armed)
            edit.ops.push_back(op("track.setArm", {{"id", k->id}, {"armed", acted && on}}, label));
    }
    return edit;
}
// The monitoring switch (manual 21.4): In, Auto or Off on the clicked track, or on every selected track
// when the clicked one is among them. A track already on that choice is left alone. One batch.
inline const char *monitorName(Monitor m) noexcept {
    return m == Monitor::In ? "In" : m == Monitor::Auto ? "Auto" : "Off";
}
inline TrackEdit setMonitor(const SnapshotTracks &tracks, const RecordStates &states, std::int64_t clicked,
                            const std::vector<std::int64_t> &selected, Monitor mode) {
    using namespace track_edits_detail;
    TrackEdit edit;
    const auto *t = find(tracks, clicked);
    if (t == nullptr || !armable(*t)) {
        edit.refusal = t == nullptr ? "Select a track to monitor." : "This track has no input to monitor.";
        return edit;
    }
    const bool together =
        selected.size() > 1 && std::find(selected.begin(), selected.end(), clicked) != selected.end();
    const std::string label = std::string("Monitor ") + monitorName(mode);
    for (const auto &k : tracks) {
        if (!k || !armable(*k) || (together ? std::find(selected.begin(), selected.end(), k->id) == selected.end()
                                            : k->id != clicked))
            continue;
        if (recordState(states, k->id).monitor != mode)
            edit.ops.push_back(op("track.setMonitorMode", {{"id", k->id}, {"mode", static_cast<std::int64_t>(mode)}},
                                  label.c_str()));
    }
    return edit;
}
// The tracks a Record takes (manual 21.1: "all armed tracks"): the armed ones the snapshot still holds,
// in its order, audio apart from MIDI, since the recorder takes audio and the engine's live MIDI input
// is not on main yet.
struct ArmedTracks {
    std::vector<std::int64_t> audio, midi;
};
inline ArmedTracks armedTracks(const SnapshotTracks &tracks, const RecordStates &states) {
    ArmedTracks out;
    for (const auto &k : tracks)
        if (k && armable(*k) && recordState(states, k->id).armed)
            (k->kind == "audio" ? out.audio : out.midi).push_back(k->id);
    return out;
}
} // namespace adi::ui
