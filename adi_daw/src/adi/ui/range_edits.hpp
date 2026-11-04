// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/track_edits.hpp"
#include <algorithm>
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
                else if (a < to && b > from)
                    ++out.crossing;
            }
    return out;
}
namespace range_edits_detail {
inline std::string crossingNote(std::size_t crossing) {
    return std::to_string(crossing) + (crossing == 1 ? " clip crosses" : " clips cross") +
           " the selection's edges and stays as it is: splitting it waits for the engine's split op.";
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
// What the Status Bar says after a range edit that left clips crossing its edges; empty when none did.
inline std::string rangeNote(const SnapshotTracks &tracks, const std::vector<std::int64_t> &selected,
                             std::int64_t start, std::int64_t end) {
    const auto content = rangeContent(tracks, selected, start, end);
    return content.crossing > 0 && !content.inside.empty() ? range_edits_detail::crossingNote(content.crossing)
                                                           : std::string{};
}
} // namespace adi::ui
