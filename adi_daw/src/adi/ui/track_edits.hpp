// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ops.hpp"
#include "adi/track_recording.hpp"
#include "adi/ui/track_order.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
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
// The name a rename stores (Live 12 manual 18.2; walk-A A34, DF-W57). An empty name is no name: Live
// gives the track its default name back. For a MIDI, audio or group track that is "# MIDI", "# Audio" or
// "# Group", whose '#' shows the track's number. A return, a VCA and Main, whose names are made when
// they are, keep the name they have.
inline std::string renamedTrackName(const engine::TrackNode &track, const std::string &text) {
    if (text.find_first_not_of(" \t") != std::string::npos)
        return text;
    if (track.kind == "midi" || track.kind == "instrument")
        return "# MIDI";
    if (track.kind == "audio")
        return "# Audio";
    if (track.kind == "group")
        return "# Group";
    return track.name;
}
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
// A reorder for each track of `order` whose index is not its place, Main's too: it shares the
// parent's index_in_parent, so an index it kept could be one an ungrouped or inserted track takes.
inline void renumber(std::vector<OpRequest> &ops, const std::vector<const engine::TrackNode *> &order, const char *label) {
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i);
        if (order[i]->indexInParent != place)
            ops.push_back(op("track.reorder", {{"id", order[i]->id}, {"index", place}}, label));
    }
}
} // namespace track_edits_detail
// Live groups audio and MIDI tracks and groups; Main and the returns stay where they are.
// What Group Tracks takes: every track but Main. A return may go into a group (ADR-0245 d9, ADR-0247
// d2): it then takes sends only from inside it, and plays into it (#677, DF-E22).
inline bool groupable(const engine::TrackNode &t) noexcept {
    return t.kind == "audio" || t.kind == "midi" || t.kind == "instrument" || t.kind == "group" || t.kind == "return";
}
inline bool canGroup(const SnapshotTracks &tracks, std::int64_t id) noexcept {
    const auto *t = track_edits_detail::find(tracks, id);
    return t != nullptr && groupable(*t);
}
// Live's Track Activator and its keys (Live 12 manual 18.1; §42.19 "Deactivate Selected Track", 0, and
// "Activate/Deactivate Track 1…8", F1…F8): the clicked track's switch thrown; with several tracks
// selected and the clicked one among them, all of them to its new state. Main has none. One batch.
inline TrackEdit activateTracks(const SnapshotTracks &tracks, std::int64_t clicked, const std::vector<std::int64_t> &selected) {
    using namespace track_edits_detail;
    TrackEdit edit;
    const auto *t = find(tracks, clicked);
    if (t == nullptr || t->kind == "master") {
        edit.refusal = t == nullptr ? "Select a track to deactivate." : "Main has no Track Activator.";
        return edit;
    }
    const bool mute = !t->muted;
    const char *label = mute ? "Deactivate Track" : "Activate Track";
    const bool together =
        selected.size() > 1 && std::find(selected.begin(), selected.end(), clicked) != selected.end();
    for (const auto &k : tracks)
        if (k && k->kind != "master" && k->muted != mute &&
            (together ? std::find(selected.begin(), selected.end(), k->id) != selected.end() : k->id == clicked))
            edit.ops.push_back(op("track.setMute", {{"id", k->id}, {"muted", mute}}, label));
    return edit;
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
// Live's Solo in Place (Live 12 manual 18.6; the Solo button's menu and the Options menu's entry):
// soloing leaves the returns audible. One project field for both entries (`mixer.setSoloInPlace`,
// #412), on by default (the engine's default; Live's factory default is unmeasured, F-02 NOT RUN,
// the director's profile reads it ticked, F-07). The manual gives the Options entry a second role,
// "the default behavior", which ADI has no field for until it is measured (parity-7.6 row 7). The
// op is outside undo, as the catalogue says (ephemeral, in the transport scope), and saved with
// the project. No op when the snapshot already holds `on`.
inline TrackEdit soloInPlaceEdit(const engine::Snapshot &snapshot, bool on) {
    TrackEdit edit;
    if (snapshot.cue.soloInPlace != on)
        edit.ops.push_back(track_edits_detail::op("mixer.setSoloInPlace", {{"on", on}}, "Solo in Place"));
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
// Live's auto-colour for a new track (ADR-0211 d2; manual 2.3.2): the previous track's palette index
// plus one, stored by the batch whose first op creates it, so no track's colour moves as others are
// inserted or grouped. The previous track is the one before its place in its parent (Main aside; a
// track before the returns), else the group that holds it: its own colour in `tracks`, else
// `colourOf`'s, the index the window shows for it by place.
template <class ColourOf>
inline void colourNewTrack(const SnapshotTracks &tracks, std::vector<OpRequest> &ops, ColourOf &&colourOf) {
    using namespace track_edits_detail;
    if (ops.empty() || ops.front().opType != "track.create")
        return;
    const auto &made = ops.front();
    const auto &p = made.payload;
    const auto parent = p.contains("parent") && p["parent"].is_number_integer() ? p["parent"].get<std::int64_t>() : 0;
    const bool ret = p.value("kind", std::string()) == "return";
    auto order = children(tracks, parent);
    order.erase(std::remove_if(order.begin(), order.end(), [ret](const auto *k) { return rank(*k) > (ret ? 1 : 0); }),
                order.end());
    const auto index = std::clamp<std::int64_t>(p.value("index", std::int64_t{0}), 0, static_cast<std::int64_t>(order.size()));
    const auto previous = index > 0 ? order[static_cast<std::size_t>(index - 1)]->id : parent;
    const auto *before = find(tracks, previous);
    const auto colour = before == nullptr ? -1
                        : before->color && *before->color >= 0 && *before->color < kPaletteColours
                            ? *before->color
                            : std::max<std::int64_t>(colourOf(previous), -1);
    auto set = op("track.setColor", {{"id", p.at("id")}, {"color", (colour + 1) % kPaletteColours}}, made.label.c_str());
    ops.insert(ops.begin() + 1, std::move(set));
}
namespace track_edits_detail {
// Whether group `id` holds `t` at any depth. A parent that loops back, which the project should never
// hold, ends the walk after as many steps as there are tracks, as `TrackTree::within` does (DF-W35 (c)),
// rather than hanging the window.
inline bool insideGroup(const SnapshotTracks &tracks, const engine::TrackNode &t, std::int64_t id) noexcept {
    std::size_t steps = 0;
    for (const auto *up = find(tracks, t.parent); up != nullptr && steps <= tracks.size();
         up = find(tracks, up->parent), ++steps)
        if (up->id == id)
            return true;
    return false;
}
} // namespace track_edits_detail
// The tracks group `id` holds, at every depth, in the snapshot's order.
inline std::vector<std::int64_t> heldBy(const SnapshotTracks &tracks, std::int64_t id) {
    std::vector<std::int64_t> out;
    for (const auto &t : tracks)
        if (t && track_edits_detail::insideGroup(tracks, *t, id))
            out.push_back(t->id);
    return out;
}
// Whether a soloed track stands anywhere inside group `id`, nested groups too: Live then shows the
// group's Solo half coloured (manual 18.3).
inline bool holdsSoloed(const SnapshotTracks &tracks, std::int64_t id) noexcept {
    for (const auto &t : tracks)
        if (t && t->soloed && t->id != id && track_edits_detail::insideGroup(tracks, *t, id))
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
// `newId`, when given, is the group's id from the caller's `RowIds` (DF-E16); else past the live tracks.
inline TrackEdit groupTracks(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids, std::int64_t newId = 0) {
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
            edit.refusal = "Main can't be grouped.";
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
    if (newId > 0)
        group = newId;
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
    // The group stands at `at`; the tracks after it move up one place, the returns and Main too: a
    // Main left behind shared the index the last return moved up to (walk-A A23).
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i < at ? i : i + 1);
        if (rest[i]->indexInParent != place)
            edit.ops.push_back(op("track.reorder", {{"id", rest[i]->id}, {"index", place}}, label));
    }
    edit.select = group;
    return edit;
}
// The selected track into a new group, which takes its place and is selected.
inline TrackEdit groupTrack(const SnapshotTracks &tracks, std::int64_t id) { return groupTracks(tracks, {id}); }
// Live's Insert Audio Track and Insert MIDI Track (Ctrl/Cmd+T, Ctrl/Cmd+Shift+T; manual 18.2): a new
// track right after the selected one, in the group that holds it, selected. The tracks after it, the
// returns and Main too, move down one place in the same batch, as `track.create` moves none. With
// nothing selected, or Main or a return, it comes after the last track at the top. Named "# Audio"
// or "# MIDI": '#' shows its number.
// The registry's recording defaults for a new track (#397, N31): `record.quantize` ("off" or a grid)
// and `audio.keepMonitoringLatency`, captured into the creating batch as ops, so a replay never
// reads the settings again.
struct RecordingDefaults {
    std::string quantize = "1/16";
    bool keepMonitoringLatency = true;
};
inline std::vector<OpRequest> recordingDefaultOps(std::int64_t track, const RecordingDefaults &defaults, const char *label) {
    auto ops = trackRecordingDefaults(track, defaults.quantize, defaults.keepMonitoringLatency);
    for (auto &o : ops)
        o.label = label;
    return ops;
}
// `newId`, when given, is the track's id from the caller's `RowIds` (DF-E16); else past the live tracks.
inline TrackEdit insertTrack(const SnapshotTracks &tracks, std::int64_t selected, bool midi,
                             const RecordingDefaults *defaults = nullptr, std::int64_t newId = 0) {
    using namespace track_edits_detail;
    const char *label = midi ? "Insert MIDI Track" : "Insert Audio Track";
    TrackEdit edit;
    std::int64_t id = 1;
    for (const auto &k : tracks)
        if (k)
            id = std::max(id, k->id + 1);
    if (newId > 0)
        id = newId;
    const auto *t = find(tracks, selected);
    if (t != nullptr && rank(*t) != 0)
        t = nullptr;
    const auto parent = t != nullptr ? t->parent : 0;
    // What the group holds, in the arrangement's order: its tracks, then its returns, then Main; the
    // new track comes before the returns. They share the parent's index_in_parent (the default
    // project numbers them 0..6, Main last), so every one after the new track moves down one, the
    // returns and Main too, as a bounce moves them: an index left behind would be shared, and what
    // orders by index alone (the control projection, the engine's snapshot) would misorder.
    const auto order = children(tracks, parent);
    std::size_t at = static_cast<std::size_t>(
        std::count_if(order.begin(), order.end(), [](const auto *k) { return rank(*k) == 0; }));
    if (t != nullptr)
        at = static_cast<std::size_t>(std::find(order.begin(), order.end(), t) - order.begin()) + 1;
    edit.ops.push_back(op("track.create",
                          {{"id", id},
                           {"kind", midi ? "midi" : "audio"},
                           {"name", midi ? "# MIDI" : "# Audio"},
                           {"parent", parentValue(parent)},
                           {"index", static_cast<std::int64_t>(at)}},
                          label));
    if (midi)
        edit.ops.push_back(op("track.setMonitorMode", {{"id", id}, {"mode", 2}}, label));
    if (defaults != nullptr)
        for (auto &o : recordingDefaultOps(id, *defaults, label))
            edit.ops.push_back(std::move(o));
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i < at ? i : i + 1);
        if (order[i]->indexInParent != place)
            edit.ops.push_back(op("track.reorder", {{"id", order[i]->id}, {"index", place}}, label));
    }
    edit.select = id;
    return edit;
}
// Where dragged tracks land: in `parent` (0 at the top), before its track `before`; with `before`
// 0, after the rest it holds (before the returns and Main at the top).
struct TrackDrop {
    std::int64_t parent = 0, before = 0;
};
// Live's track drag (Live 12 manual, Arrangement View: "Tracks can be reordered by selecting and
// dragging them above or below other tracks"; 18.3: "tracks can be dragged into or out of the
// group"): the selected tracks, in the arrangement's order, to `to`, side by side. A track whose
// group is selected as well goes with its group. The returns move among themselves, at the foot or
// in a group (`to.before` a return, or 0 for after the last); Main stays last; a group never goes inside
// itself. Tracks moved into or out of a group are routed as Group Tracks routes
// them (`track.setParent`). One batch, so one undo step; no ops when nothing would move.
inline TrackEdit moveTracks(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids, TrackDrop to) {
    using namespace track_edits_detail;
    TrackEdit edit;
    auto picked = [&ids](std::int64_t id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    // Every track in the arrangement's order (TrackOrder's): each group followed by what it holds.
    std::vector<const engine::TrackNode *> all, stack;
    for (auto top = children(tracks, 0); !top.empty(); top.pop_back())
        stack.push_back(top.back());
    while (!stack.empty() && all.size() < tracks.size()) {
        const auto *t = stack.back();
        stack.pop_back();
        all.push_back(t);
        auto held = children(tracks, t->id);
        for (auto k = held.rbegin(); k != held.rend(); ++k)
            stack.push_back(*k);
    }
    // The selected tracks no selected group holds, in that order.
    std::vector<const engine::TrackNode *> chosen;
    for (const auto *t : all) {
        if (!picked(t->id))
            continue;
        bool held = false;
        for (const auto *up = find(tracks, t->parent); up != nullptr && !held; up = find(tracks, up->parent))
            held = picked(up->id);
        if (!held)
            chosen.push_back(t);
    }
    auto moving = [&chosen](const engine::TrackNode *t) {
        return std::find(chosen.begin(), chosen.end(), t) != chosen.end();
    };
    if (chosen.empty()) {
        edit.refusal = "Select a track to move.";
        return edit;
    }
    // Live reorders the returns among themselves, at the foot; Main stays last.
    const auto returns = static_cast<std::size_t>(
        std::count_if(chosen.begin(), chosen.end(), [](const auto *t) { return t->kind == "return"; }));
    for (const auto *t : chosen)
        if (t->kind == "master") {
            edit.refusal = "Main stays last.";
            return edit;
        }
    // A return goes into a group too, among its returns (ADR-0247 d2), and out again to the foot.
    if (returns != 0 && returns != chosen.size()) {
        edit.refusal = "Return tracks move among the returns only.";
        return edit;
    }
    if (to.parent != 0) {
        const auto *g = find(tracks, to.parent);
        if (g == nullptr || g->kind != "group") {
            edit.refusal = "Tracks go into a group track only.";
            return edit;
        }
        for (const auto *up = g; up != nullptr; up = find(tracks, up->parent))
            if (moving(up)) {
                edit.refusal = "A group can't go inside itself.";
                return edit;
            }
    }
    const char *label = chosen.size() > 1 ? "Move Tracks" : "Move Track";
    // What the parent will hold: the rest, the moved tracks before `before` (past it while it is
    // one of them), or after the rest's tracks and groups. Returns, among the returns alone.
    auto list = children(tracks, to.parent);
    if (returns != 0)
        list.erase(std::remove_if(list.begin(), list.end(), [](const auto *k) { return k->kind != "return"; }), list.end());
    std::vector<const engine::TrackNode *> rest;
    std::size_t at = 0;
    bool placed = false, passed = false;
    for (const auto *k : list) {
        passed = passed || k->id == to.before;
        if (moving(k))
            continue;
        if (!placed && passed) {
            at = rest.size();
            placed = true;
        }
        rest.push_back(k);
    }
    if (!placed) {
        at = returns != 0 ? rest.size() : 0;
        while (at < rest.size() && rank(*rest[at]) == 0)
            ++at;
    }
    auto order = rest;
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(at), chosen.begin(), chosen.end());
    // Dropped where they stand: nothing moves, and no step is made.
    const bool inPlace = std::all_of(chosen.begin(), chosen.end(), [&to](const auto *t) { return t->parent == to.parent; });
    if (inPlace && order == list)
        return edit;
    // The groups they leave, each once, renumbered without them.
    std::vector<std::int64_t> left;
    for (const auto *t : chosen)
        if (t->parent != to.parent) {
            edit.ops.push_back(op("track.setParent", {{"id", t->id}, {"parent", parentValue(to.parent)}}, label));
            if (std::find(left.begin(), left.end(), t->parent) == left.end())
                left.push_back(t->parent);
        }
    renumber(edit.ops, order, label);
    for (const auto parent : left) {
        auto stay = children(tracks, parent);
        stay.erase(std::remove_if(stay.begin(), stay.end(), moving), stay.end());
        renumber(edit.ops, stay, label);
    }
    return edit;
}
// Move selected tracks one sibling slot without changing their parents. A selected group carries
// its descendants, and each run of selected siblings passes one unselected sibling of its rank.
inline TrackEdit nudgeTracks(const SnapshotTracks &tracks, const std::vector<std::int64_t> &ids, bool down) {
    using namespace track_edits_detail;
    TrackEdit edit;
    const std::set<std::int64_t> picked(ids.begin(), ids.end());
    std::set<std::int64_t> moving, parents;
    for (const auto &track : tracks) {
        if (!track || rank(*track) == 2 || !picked.contains(track->id))
            continue;
        auto *parent = find(tracks, track->parent);
        for (std::size_t depth = 0; parent && depth < tracks.size() && !picked.contains(parent->id); ++depth)
            parent = find(tracks, parent->parent);
        if (!parent) {
            moving.insert(track->id);
            parents.insert(track->parent);
        }
    }
    const char *label = moving.size() > 1 ? "Move Tracks" : "Move Track";
    for (const auto parent : parents) {
        auto order = children(tracks, parent);
        bool changed = false;
        for (std::size_t step = 1; step < order.size(); ++step) {
            const auto from = down ? order.size() - step - 1 : step;
            const auto to = down ? from + 1 : from - 1;
            if (moving.contains(order[from]->id) && !moving.contains(order[to]->id) &&
                rank(*order[from]) == rank(*order[to])) {
                std::swap(order[from], order[to]);
                changed = true;
            }
        }
        if (changed)
            renumber(edit.ops, order, label);
    }
    return edit;
}
// Live's Track Delay (manual 18.7): a track's output delayed, or pre-delayed by a negative value, in
// milliseconds. The format keeps samples at the project's rate (`TrackNode::delaySamples`,
// `Snapshot::sampleRate`; ADR-0172): the window writes ms × rate / 1000, to the nearest sample, and
// reads back 1000 × samples / rate, so a typed value shows what the sample grid kept.
inline double trackDelayMs(std::int64_t samples, int sampleRate) noexcept {
    return sampleRate > 0 ? 1000. * static_cast<double>(samples) / sampleRate : 0.;
}
inline std::int64_t trackDelaySamples(double ms, int sampleRate) noexcept {
    return std::llround(ms * sampleRate / 1000.);
}
// One `mixer.setDelay` on track `id`, one undo step; no ops when the sample grid would keep the
// delay it has. Main's is shown and written as every track's (Master Reference 8.1).
inline TrackEdit setTrackDelay(const SnapshotTracks &tracks, int sampleRate, std::int64_t id, double ms) {
    TrackEdit edit;
    const auto *t = track_edits_detail::find(tracks, id);
    if (t == nullptr) {
        edit.refusal = "Select a track to delay.";
        return edit;
    }
    if (!std::isfinite(ms)) {
        edit.refusal = "Track Delay takes a number of milliseconds.";
        return edit;
    }
    if (sampleRate <= 0) {
        edit.refusal = "The project has no sample rate to count the delay in.";
        return edit;
    }
    const auto samples = trackDelaySamples(ms, sampleRate);
    if (samples != t->delaySamples)
        edit.ops.push_back(
            track_edits_detail::op("mixer.setDelay", {{"id", id}, {"samples", samples}}, "Change Track Delay"));
    return edit;
}
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
// Live's Insert Return Track (Ctrl/Cmd+Alt+T; manual 18.2): a new return after the others, lettered
// as Live letters them, selected. Inside a group (Insert Return Track in Group, ADR-0247 d2), after
// what the group holds, playing into it; its letter is the group's initial and its place among the
// group's returns ("DA"). At the top it takes Main's place and Main moves down one, in the same
// batch: they share index_in_parent, and a return given the index past every track stood after
// Main by index, or shared one with it after Group Tracks (walk-A A23). The rest of the parent is
// renumbered to its order too, so a file that already shares an index is put right. `id` is the new
// track's, from the caller's `RowIds` (DF-E16).
inline TrackEdit insertReturnTrack(const SnapshotTracks &tracks, std::int64_t group, std::int64_t id) {
    using namespace track_edits_detail;
    TrackEdit edit;
    const auto *into = group != 0 ? find(tracks, group) : nullptr;
    if (group != 0 && (into == nullptr || into->kind != "group")) {
        edit.refusal = "Choose a group to put the return in.";
        return edit;
    }
    const char *label = into != nullptr ? "Insert Return Track in Group" : "Insert Return Track";
    std::int64_t returns = 0;
    for (const auto &t : tracks)
        if (t && t->kind == "return" && t->parent == group)
            ++returns;
    // What the parent holds in TrackOrder's order: its tracks, its returns, then Main; the new
    // return comes before Main.
    const auto order = children(tracks, group);
    const auto at = static_cast<std::size_t>(
        std::count_if(order.begin(), order.end(), [](const auto *k) { return rank(*k) < 2; }));
    std::string letter(1, static_cast<char>('A' + returns % 26));
    if (into != nullptr) {
        char initial = 'G';
        for (const char c : into->name)
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                initial = static_cast<char>(c >= 'a' ? c - 'a' + 'A' : c);
                break;
            }
        letter.insert(letter.begin(), initial);
    }
    Payload create = {{"id", id}, {"kind", "return"}, {"name", letter + " Return"}, {"index", static_cast<std::int64_t>(at)}};
    if (into != nullptr)
        create["parent"] = group;
    edit.ops.push_back(op("track.create", std::move(create), label));
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto place = static_cast<std::int64_t>(i < at ? i : i + 1);
        if (order[i]->indexInParent != place)
            edit.ops.push_back(op("track.reorder", {{"id", order[i]->id}, {"index", place}}, label));
    }
    edit.select = id;
    return edit;
}
} // namespace adi::ui
