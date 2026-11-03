// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ops.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
// Live 12 manual 18.3 and the Edit menu (§41.5): Group Tracks puts the selected track in a new
// group track where it stood; Ungroup Tracks puts a group's tracks back where the group stood and
// removes the group. Each is one batch of ops, so one undo step, built from the snapshot the
// action lands on (ADR-0201), never from the store.
//
// The window keeps one selected track, so Group Tracks groups that one; Live groups every selected
// track. Indexes are put in order as part of the batch wherever the edit needs it: tracks that
// share an index_in_parent fall to their order in the snapshot, and a reorder for each makes the
// order the edit leaves the one the file says, whatever indexes it held before.
struct TrackEdit {
    std::vector<OpRequest> ops;
    std::int64_t select = 0; // the track to select once the batch lands
    std::int64_t clip = 0;   // the clip to select with it; 0 for none
    std::string refusal;     // why nothing is done; empty when the ops are ready
};
// The snapshot's tracks, as `engine::Snapshot::tracks` holds them.
using SnapshotTracks = std::vector<std::shared_ptr<const engine::TrackNode>>;
namespace track_edits_detail {
using Tracks = SnapshotTracks;
inline const engine::TrackNode *find(const Tracks &tracks, std::int64_t id) noexcept {
    for (const auto &t : tracks)
        if (t && t->id == id)
            return t.get();
    return nullptr;
}
// Where TrackOrder puts a track among what its parent holds: the returns after the rest, Main last.
inline int rank(const engine::TrackNode &t) noexcept { return t.kind == "master" ? 2 : t.kind == "return" ? 1 : 0; }
// What `parent` holds, in the arrangement's order (TrackOrder's): by rank, then by
// index_in_parent, then by place in the snapshot.
inline std::vector<const engine::TrackNode *> children(const Tracks &tracks, std::int64_t parent) {
    std::vector<std::pair<std::size_t, const engine::TrackNode *>> held;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i] && tracks[i]->parent == parent)
            held.emplace_back(i, tracks[i].get());
    std::sort(held.begin(), held.end(), [](const auto &a, const auto &b) {
        if (rank(*a.second) != rank(*b.second))
            return rank(*a.second) < rank(*b.second);
        if (a.second->indexInParent != b.second->indexInParent)
            return a.second->indexInParent < b.second->indexInParent;
        return a.first < b.first;
    });
    std::vector<const engine::TrackNode *> out;
    out.reserve(held.size());
    for (const auto &h : held)
        out.push_back(h.second);
    return out;
}
inline Payload parentValue(std::int64_t parent) { return parent == 0 ? Payload(nullptr) : Payload(parent); }
inline OpRequest op(const char *type, Payload payload, const char *label) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(payload);
    r.label = label;
    return r;
}
// A reorder for each track of `order` whose index is not its place. Main keeps its own: it stands
// last whatever its index says.
inline void renumber(std::vector<OpRequest> &ops, const std::vector<const engine::TrackNode *> &order, const char *label) {
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i);
        if (order[i]->kind != "master" && order[i]->indexInParent != place)
            ops.push_back(op("track.reorder", {{"id", order[i]->id}, {"index", place}}, label));
    }
}
} // namespace track_edits_detail
// Live groups audio and MIDI tracks and groups; Main and the returns stay where they are.
inline bool groupable(const engine::TrackNode &t) noexcept {
    return t.kind == "audio" || t.kind == "midi" || t.kind == "instrument" || t.kind == "group";
}
inline bool canGroup(const SnapshotTracks &tracks, std::int64_t id) noexcept {
    const auto *t = track_edits_detail::find(tracks, id);
    return t != nullptr && groupable(*t);
}
// Live's Solo switch and its S key (Live 12 manual 18.1, §42.19 "Solo Selected Tracks"): the clicked
// track's Solo thrown; with several tracks selected and the clicked one among them, all of them
// together. Soloing unsolos every other track while Exclusive Solo is on (the Record, Warp & Launch
// Settings, `record.exclusiveSolo`, on by default as Live's), unless Ctrl/Cmd is held (`add`). Main
// has no Solo. One batch.
inline TrackEdit soloTracks(const SnapshotTracks &tracks, std::int64_t clicked, const std::vector<std::int64_t> &selected,
                            bool exclusive, bool add) {
    using namespace track_edits_detail;
    TrackEdit edit;
    const auto *t = find(tracks, clicked);
    if (t == nullptr || t->kind == "master") {
        edit.refusal = t == nullptr ? "Select a track to solo." : "Main has no Solo.";
        return edit;
    }
    const bool on = !t->soloed;
    const char *label = on ? "Solo Track" : "Unsolo Track";
    const bool together =
        selected.size() > 1 && std::find(selected.begin(), selected.end(), clicked) != selected.end();
    for (const auto &k : tracks) {
        if (!k || k->kind == "master")
            continue;
        const bool acted = together ? std::find(selected.begin(), selected.end(), k->id) != selected.end()
                                    : k->id == clicked;
        if (acted ? k->soloed != on : on && exclusive && !add && k->soloed)
            edit.ops.push_back(op("track.setSolo", {{"id", k->id}, {"soloed", acted && on}}, label));
    }
    return edit;
}
// Live's colour chooser (Live 12 manual 2.3.2): the colour at `index` of Live's palette (ADR-0211) on
// every track of `ids` whose colour differs, as one batch; no ops when none would change.
inline TrackEdit colourTracks(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids, std::int64_t index,
                              const char *label) {
    TrackEdit edit;
    if (ids.empty())
        edit.refusal = "Select a track to colour.";
    for (const auto id : ids)
        if (const auto *t = track_edits_detail::find(tracks, id); t != nullptr && t->color != index)
            edit.ops.push_back(track_edits_detail::op("track.setColor", {{"id", id}, {"color", index}}, label));
    return edit;
}
// The tracks group `id` holds, at every depth, in the snapshot's order.
inline std::vector<std::int64_t> heldBy(const SnapshotTracks &tracks, std::int64_t id) {
    std::vector<std::int64_t> out;
    for (const auto &t : tracks)
        if (t)
            for (const auto *up = track_edits_detail::find(tracks, t->parent); up != nullptr;
                 up = track_edits_detail::find(tracks, up->parent))
                if (up->id == id) {
                    out.push_back(t->id);
                    break;
                }
    return out;
}
// Whether a soloed track stands anywhere inside group `id`, nested groups too: Live then shows the
// group's Solo half coloured (manual 18.3).
inline bool holdsSoloed(const SnapshotTracks &tracks, std::int64_t id) noexcept {
    for (const auto &t : tracks)
        if (t && t->soloed && t->id != id)
            for (const auto *up = track_edits_detail::find(tracks, t->parent); up != nullptr;
                 up = track_edits_detail::find(tracks, up->parent))
                if (up->id == id)
                    return true;
    return false;
}
inline bool canUngroup(const SnapshotTracks &tracks, std::int64_t id) noexcept {
    const auto *t = track_edits_detail::find(tracks, id);
    return t != nullptr && t->kind == "group";
}
// Live's Group Tracks (manual 18.3): the selected tracks, in their order, into a new group that
// takes the first one's place and is selected. A track whose group is selected as well goes with
// its group. They stand in one group, or at the top; Main and the returns are never grouped. The
// group's name is "# Group": '#' shows its number, as a new track's does (manual 18.2).
inline TrackEdit groupTracks(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids) {
    using namespace track_edits_detail;
    constexpr const char *label = "Group Tracks";
    TrackEdit edit;
    auto picked = [&ids](std::int64_t id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    // The selected tracks no selected group holds.
    std::vector<const engine::TrackNode *> chosen;
    for (const auto id : ids) {
        const auto *t = find(tracks, id);
        if (t == nullptr)
            continue;
        bool held = false;
        for (const auto *up = find(tracks, t->parent); up != nullptr && !held; up = find(tracks, up->parent))
            held = picked(up->id);
        if (!held)
            chosen.push_back(t);
    }
    if (chosen.empty()) {
        edit.refusal = "Select a track to group.";
        return edit;
    }
    const auto parent = chosen.front()->parent;
    for (const auto *t : chosen) {
        if (!groupable(*t)) {
            edit.refusal = "Main and return tracks can't be grouped.";
            return edit;
        }
        if (t->parent != parent) {
            edit.refusal = "Select tracks in one group to group them together.";
            return edit;
        }
    }
    std::int64_t group = 1;
    for (const auto &k : tracks)
        if (k)
            group = std::max(group, k->id + 1);
    // Into the group, in their order; the rest of their group's tracks around it, the group at the
    // first one's place.
    std::vector<const engine::TrackNode *> inside, rest;
    std::size_t at = 0;
    for (const auto *k : children(tracks, parent)) {
        if (std::find(chosen.begin(), chosen.end(), k) == chosen.end()) {
            rest.push_back(k);
            continue;
        }
        if (inside.empty())
            at = rest.size();
        inside.push_back(k);
    }
    edit.ops.push_back(op("track.create",
                          {{"id", group},
                           {"kind", "group"},
                           {"name", "# Group"},
                           {"parent", parentValue(parent)},
                           {"index", static_cast<std::int64_t>(at)}},
                          label));
    for (std::size_t i = 0; i < inside.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i);
        edit.ops.push_back(op("track.setParent", {{"id", inside[i]->id}, {"parent", group}}, label));
        if (inside[i]->indexInParent != place)
            edit.ops.push_back(op("track.reorder", {{"id", inside[i]->id}, {"index", place}}, label));
    }
    // The group stands at `at`; the tracks after it move up one place.
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i < at ? i : i + 1);
        if (rest[i]->kind != "master" && rest[i]->indexInParent != place)
            edit.ops.push_back(op("track.reorder", {{"id", rest[i]->id}, {"index", place}}, label));
    }
    edit.select = group;
    return edit;
}
// The selected track into a new group, which takes its place and is selected.
inline TrackEdit groupTrack(const SnapshotTracks &tracks, std::int64_t id) { return groupTracks(tracks, {id}); }
// A group's tracks back where the group stood, in their order, and the group removed; the first
// of them is selected. An empty group is just removed.
inline TrackEdit ungroupTrack(const SnapshotTracks &tracks, std::int64_t id) {
    using namespace track_edits_detail;
    constexpr const char *label = "Ungroup Tracks";
    TrackEdit edit;
    const auto *g = find(tracks, id);
    if (g == nullptr || g->kind != "group") {
        edit.refusal = "Select a group track to ungroup.";
        return edit;
    }
    const auto inner = children(tracks, id);
    auto order = children(tracks, g->parent);
    const auto at = std::find(order.begin(), order.end(), g);
    const auto pos = order.erase(at);
    order.insert(pos, inner.begin(), inner.end());
    for (const auto *c : inner)
        edit.ops.push_back(op("track.setParent", {{"id", c->id}, {"parent", parentValue(g->parent)}}, label));
    renumber(edit.ops, order, label);
    edit.ops.push_back(op("track.delete", {{"id", id}}, label));
    edit.select = inner.empty() ? 0 : inner.front()->id;
    return edit;
}
} // namespace adi::ui
