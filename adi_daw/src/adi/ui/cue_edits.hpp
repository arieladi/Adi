// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Live's cueing (Live 12 manual 18.6; MX-08): the Options menu's Solo Switches and Cue Switches choose
// what the tracks' Solo buttons do. With Cue Switches each becomes a Cue button that sends the track to
// Cue Out (pre-listening, on headphones) without soloing it; Main gains Cue Volume, and Cue Out names
// the stereo pair it plays to. Every cue op is the transport's (`cue.*`, Scope::Transport): saved with
// the project, outside undo, as the catalogue says. No JUCE.
#pragma once
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace adi::ui {
// Whether track `id` is cued (`CueSettings::selectedTracks`, kept sorted).
inline bool cued(const engine::Snapshot &snapshot, std::int64_t id) noexcept {
    const auto &s = snapshot.cue.selectedTracks;
    return std::binary_search(s.begin(), s.end(), id);
}
// A track that can be cued: one that carries audio to monitor, as `cue.setTrack` admits.
inline bool cueable(const engine::TrackNode *t) noexcept {
    return t != nullptr && (t->kind == "audio" || t->kind == "midi" || t->kind == "instrument" || t->kind == "group" ||
                            t->kind == "return");
}
// Solo Switches (`cue` false) or Cue Switches: one op, nothing when already so.
inline TrackEdit cueModeEdit(const engine::Snapshot &snapshot, bool cue) {
    TrackEdit edit;
    if (snapshot.cue.cueMode != cue)
        edit.ops.push_back(track_edits_detail::op("cue.setMode", {{"on", cue}}, cue ? "Cue Switches" : "Solo Switches"));
    return edit;
}
// A track's Cue button: cued or not. Refused, with the reason, for one that cannot be cued.
inline TrackEdit cueTrackEdit(const engine::Snapshot &snapshot, std::int64_t id, bool on) {
    TrackEdit edit;
    if (!cueable(snapshot.findTrack(id))) {
        edit.refusal = "Only a track that plays audio can be cued.";
        return edit;
    }
    if (cued(snapshot, id) != on)
        edit.ops.push_back(track_edits_detail::op("cue.setTrack", {{"id", id}, {"on", on}}, on ? "Cue Track" : "Uncue Track"));
    return edit;
}
// Live's Cue Volume, as the volume fader's range: its floor is silence, its top +6 dB.
inline constexpr double kCueFloorDb = -70.0, kCueSilenceDb = -150.0, kCueTopDb = 6.0;
inline double cueWrittenDb(double db) noexcept {
    return !std::isfinite(db) || db <= kCueFloorDb ? kCueSilenceDb : std::min(db, kCueTopDb);
}
inline TrackEdit cueVolumeEdit(const engine::Snapshot &snapshot, double db) {
    TrackEdit edit;
    const auto written = cueWrittenDb(db);
    if (written != snapshot.cue.volumeDb)
        edit.ops.push_back(track_edits_detail::op("cue.setVolume", {{"db", written}}, "Change Cue Volume"));
    return edit;
}
// Cue Out: the stereo pair whose first channel is `first` (0-based, as the op takes it).
inline TrackEdit cueOutputEdit(const engine::Snapshot &snapshot, int first) {
    TrackEdit edit;
    if (first < 0 || first > 62 || first % 2 != 0) {
        edit.refusal = "Cue Out is a stereo pair of the audio device's outputs.";
        return edit;
    }
    if (first != snapshot.cue.firstOutputChannel)
        edit.ops.push_back(track_edits_detail::op("cue.setOutput", {{"first", first}}, "Change Cue Out"));
    return edit;
}
inline TrackEdit mainOutputEdit(const engine::Snapshot &snapshot, int first) {
    TrackEdit edit;
    if (first < 0 || first > 62 || first % 2 != 0) {
        edit.refusal = "Main Out is a stereo pair of the audio device's outputs.";
        return edit;
    }
    if (first != snapshot.cue.firstMainOutputChannel)
        edit.ops.push_back(track_edits_detail::op("mixer.setMainOutput", {{"first", first}}, "Change Main Out"));
    return edit;
}
} // namespace adi::ui
