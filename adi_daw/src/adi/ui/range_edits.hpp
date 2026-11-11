// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
// Live's edits on a time selection (Live 12 manual 6.9): Delete removes the selected material, and
// 0 deactivates it, "even if it contains multiple clips", on every track the selection covers. Each
// is one batch, so one undo step, from the snapshot the key lands on (ADR-0201).
//
// A clip wholly inside the selection is acted on as it is. A clip that crosses the selection's edge
// would be split there, as Live splits it; there is no split op yet (window log, N17), so such a clip
// is left as it is, and `crossing` says how many were.
struct RangeContent {
    std::vector<const engine::ClipNode *> inside;
    std::size_t crossing = 0;
    std::vector<const engine::ClipNode *> crossers; // the clips that cross an edge, in track order
};
inline RangeContent rangeContent(const SnapshotTracks &tracks, const std::vector<std::int64_t> &selected,
                                 std::int64_t start, std::int64_t end) {
    RangeContent out;
    const auto from = std::min(start, end), to = std::max(start, end);
    if (from == to)
        return out;
    for (const auto id : selected)
        if (const auto *t = track_edits_detail::find(tracks, id))
            for (const auto &c : t->clips) {
                if (!c)
                    continue;
                const auto a = c->posTicks, b = c->posTicks + c->lengthTicks;
                if (a >= from && b <= to)
                    out.inside.push_back(c.get());
                else if (a < to && b > from) {
                    ++out.crossing;
                    out.crossers.push_back(c.get());
                }
            }
    return out;
}
// 0 over a selection that crosses one edge of one clip, holding no whole clip (E1; manual 6.9): the
// clip splits at the selection's edge inside it and the part inside the selection is deactivated.
// None when the selection lies inside the clip (both its edges in it: the two-point split, N17's
// open part) or reaches more than one clip.
struct EdgeSplit {
    std::int64_t clip = 0, at = 0;
    bool insideIsRight = false; // the part after the split is the one in the selection
};
inline std::optional<EdgeSplit> edgeSplit(const RangeContent &content, std::int64_t start, std::int64_t end) {
    if (content.crossers.size() != 1 || !content.inside.empty())
        return std::nullopt;
    const auto *c = content.crossers.front();
    const auto from = std::min(start, end), to = std::max(start, end);
    const auto a = c->posTicks, b = c->posTicks + c->lengthTicks;
    if (from > a && from < b && to >= b)
        return EdgeSplit{c->id, from, true};
    if (to > a && to < b && from <= a)
        return EdgeSplit{c->id, to, false};
    return std::nullopt;
}
namespace range_edits_detail {
inline std::string crossingNote(std::size_t crossing) {
    return std::to_string(crossing) + (crossing == 1 ? " clip crosses" : " clips cross") +
           " the selection's edges and stays as it is: a selection inside a clip, or 0 over more than one clip's "
           "edge, waits for the engine's two-point split (N17).";
}
} // namespace range_edits_detail
// Delete on a time selection: the clips wholly inside it, on every selected track.
inline TrackEdit deleteRange(const SnapshotTracks &tracks, const std::vector<std::int64_t> &selected,
                             std::int64_t start, std::int64_t end) {
    const auto content = rangeContent(tracks, selected, start, end);
    TrackEdit edit;
    if (content.inside.empty()) {
        edit.refusal = content.crossing > 0 ? range_edits_detail::crossingNote(content.crossing)
                                            : std::string("There is nothing in the selection to delete.");
        return edit;
    }
    const char *label = content.inside.size() == 1 ? "Delete Clip" : "Delete Clips";
    for (const auto *c : content.inside)
        edit.ops.push_back(track_edits_detail::op("clip.delete", {{"id", c->id}}, label));
    return edit;
}
// 0 on a time selection: deactivates the clips wholly inside it; when every one of them is already
// deactivated, activates them again.
inline TrackEdit deactivateRange(const SnapshotTracks &tracks, const std::vector<std::int64_t> &selected,
                                 std::int64_t start, std::int64_t end) {
    const auto content = rangeContent(tracks, selected, start, end);
    TrackEdit edit;
    if (content.inside.empty()) {
        edit.refusal = content.crossing > 0 ? range_edits_detail::crossingNote(content.crossing)
                                            : std::string("There is nothing in the selection to deactivate.");
        return edit;
    }
    const bool allOff = std::all_of(content.inside.begin(), content.inside.end(),
                                    [](const engine::ClipNode *c) { return c->muted; });
    const bool plural = content.inside.size() != 1;
    const char *label = allOff ? (plural ? "Activate Clips" : "Activate Clip")
                               : (plural ? "Deactivate Clips" : "Deactivate Clip");
    for (const auto *c : content.inside)
        if (c->muted == allOff)
            edit.ops.push_back(track_edits_detail::op("clip.setMute", {{"id", c->id}, {"muted", !allOff}}, label));
    return edit;
}
// The selected clips by id, in that order: the one clicked, then those picked besides it (Live's
// "Select Multiple Items", Ctrl/Cmd+click, §42.5). Ids no longer in the snapshot are passed over.
inline std::vector<const engine::ClipNode *> clipsById(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids) {
    std::vector<const engine::ClipNode *> out;
    for (const auto id : ids)
        for (const auto &t : tracks)
            if (t)
                for (const auto &c : t->clips)
                    if (c && c->id == id && std::find(out.begin(), out.end(), c.get()) == out.end())
                        out.push_back(c.get());
    return out;
}
// Delete on the selected clips: one batch, "Delete Clip" or "Delete Clips".
inline TrackEdit deleteClips(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids) {
    const auto clips = clipsById(tracks, ids);
    TrackEdit edit;
    if (clips.empty()) {
        edit.refusal = "Select a clip to delete.";
        return edit;
    }
    const char *label = clips.size() == 1 ? "Delete Clip" : "Delete Clips";
    for (const auto *c : clips)
        edit.ops.push_back(track_edits_detail::op("clip.delete", {{"id", c->id}}, label));
    return edit;
}
// 0 on the selected clips, as on a time selection: deactivates them; when every one is already
// deactivated, activates them again. One clip is thrown either way.
inline TrackEdit deactivateClips(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids) {
    const auto clips = clipsById(tracks, ids);
    TrackEdit edit;
    if (clips.empty()) {
        edit.refusal = "Select a clip to deactivate.";
        return edit;
    }
    const bool allOff = std::all_of(clips.begin(), clips.end(), [](const engine::ClipNode *c) { return c->muted; });
    const bool plural = clips.size() != 1;
    const char *label = allOff ? (plural ? "Activate Clips" : "Activate Clip") : (plural ? "Deactivate Clips" : "Deactivate Clip");
    for (const auto *c : clips)
        if (c->muted == allOff)
            edit.ops.push_back(track_edits_detail::op("clip.setMute", {{"id", c->id}, {"muted", !allOff}}, label));
    return edit;
}
// Where the selected clips land when dragged `rows` rows down the lanes (negative: up), as Live
// drags several selected clips across tracks: each clip's track's place among `lanes` (the shown
// tracks, in the lanes' order) plus `rows`, as (clip, track) for each clip that changes track.
// Empty, and none moves across, when any would leave the lanes or land on a track that can't take
// it (`canTakeClip`: MIDI to MIDI, audio to audio).
using ClipTracks = std::vector<std::pair<std::int64_t, std::int64_t>>;
inline std::optional<ClipTracks> clipsAcross(const SnapshotTracks &tracks, const std::vector<std::int64_t> &lanes,
                                             const std::vector<std::int64_t> &ids, std::int64_t rows) {
    ClipTracks out;
    for (const auto id : ids)
        for (const auto &t : tracks)
            if (t)
                for (const auto &c : t->clips)
                    if (c && c->id == id) {
                        const auto at = std::find(lanes.begin(), lanes.end(), t->id);
                        if (at == lanes.end())
                            return std::nullopt;
                        const auto place = (at - lanes.begin()) + rows;
                        if (place < 0 || place >= static_cast<std::ptrdiff_t>(lanes.size()))
                            return std::nullopt;
                        const auto target = lanes[static_cast<std::size_t>(place)];
                        const auto *to = track_edits_detail::find(tracks, target);
                        if (to == nullptr || !canTakeClip(*to, *t))
                            return std::nullopt;
                        if (target != t->id)
                            out.emplace_back(id, target);
                    }
    return out;
}
// The selected clips moved together by `delta` ticks, as Live drags several selected clips (§6.7):
// each keeps its track, or goes to the one `across` names (clipsAcross); none goes before the
// arrangement's start, so the offset stops where the earliest one reaches it. One batch, "Move
// Clip" or "Move Clips"; no ops when nothing moves.
inline TrackEdit moveClips(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids, std::int64_t delta,
                           const ClipTracks &across = {}) {
    TrackEdit edit;
    std::vector<std::pair<const engine::ClipNode *, std::int64_t>> clips;
    for (const auto id : ids)
        for (const auto &t : tracks)
            if (t)
                for (const auto &c : t->clips)
                    if (c && c->id == id)
                        clips.emplace_back(c.get(), t->id);
    if (clips.empty()) {
        edit.refusal = "Select a clip to move.";
        return edit;
    }
    for (const auto &[c, track] : clips)
        delta = std::max(delta, -c->posTicks);
    if (delta == 0 && across.empty())
        return edit;
    const char *label = clips.size() == 1 ? "Move Clip" : "Move Clips";
    for (const auto &[c, track] : clips) {
        auto to = track;
        for (const auto &[id, target] : across)
            if (id == c->id)
                to = target;
        edit.ops.push_back(
            track_edits_detail::op("clip.move", {{"id", c->id}, {"to", c->posTicks + delta}, {"track", to}}, label));
    }
    return edit;
}
// What the Status Bar says after a range edit that left clips crossing its edges; empty when none did.
inline std::string rangeNote(const SnapshotTracks &tracks, const std::vector<std::int64_t> &selected,
                             std::int64_t start, std::int64_t end) {
    const auto content = rangeContent(tracks, selected, start, end);
    return content.crossing > 0 && !content.inside.empty() ? range_edits_detail::crossingNote(content.crossing)
                                                           : std::string{};
}
} // namespace adi::ui
