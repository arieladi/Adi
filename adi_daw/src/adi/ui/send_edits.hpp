// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
namespace adi::ui {
// The sends, Live's way (ADR-0203 d3, which reverses ADR-0072): one send knob per return on every
// clip track and group, the returns in the arrangement's order; Main has none. A send is a routing
// row of kind 'send' from the track to the return (`engine::Snapshot::sends`, #309). A track with no
// row for a return shows that knob fully down, where a new send starts (ADR-0204 d5; AWAITING row 7
// measures it), and turning it up writes the row and its level as one batch, so one undo step. A
// return's own sends are off until Enable Send, as Live's; once on, a return may feed another
// return, or itself (Live's feedback loop, which the engine plays).
//
// Everything here reads the snapshot an action lands on (ADR-0201), never the store.
using SnapshotSends = std::vector<std::shared_ptr<const engine::SendRoute>>;
// Live's send knob runs from -inf to 0 dB. Its bottom stop reads -inf, and what it writes there is
// silence as the format defines it: at or below -150 dB (SPEC §6.9, `volume_db`; the engine's
// kSilenceDb). A level a converted file holds above 0 dB plays as written and shows at the top.
inline constexpr double kSendFloorDb = -70, kSendTopDb = 0, kSendSilenceDb = -150;
// Whether a track has send knobs: every track but Main.
inline bool hasSends(const engine::TrackNode &t) noexcept { return t.kind != "master"; }
// The returns, in the arrangement's order (TrackOrder's: by index_in_parent, then by place in the
// snapshot). Refilled in place, so the same returns allocate nothing.
inline void returnsInOrder(const SnapshotTracks &tracks, std::vector<std::int64_t> &out) {
    out.clear();
    for (const auto &t : tracks)
        if (t && t->kind == "return")
            out.push_back(t->id);
    // An insertion sort: stable, so returns that share an index keep the snapshot's order, and
    // unlike std::stable_sort it takes no buffer. A project has a handful of returns.
    auto placeOf = [&tracks](std::int64_t id) { return track_edits_detail::find(tracks, id)->indexInParent; };
    for (std::size_t i = 1; i < out.size(); ++i)
        for (std::size_t j = i; j > 0 && placeOf(out[j]) < placeOf(out[j - 1]); --j)
            std::swap(out[j], out[j - 1]);
}
// The send from track `source` to track `destination`: of its rows, the first by `order`, then by
// id (the snapshot keeps them by id). Null when there is none.
inline const engine::SendRoute *findSend(const SnapshotSends &sends, std::int64_t source,
                                         std::int64_t destination) noexcept {
    const engine::SendRoute *found = nullptr;
    for (const auto &s : sends)
        if (s && s->sourceKind == "track" && s->destinationKind == "track" && s->sourceId == source &&
            s->destinationId == destination && (found == nullptr || s->order < found->order))
            found = s.get();
    return found;
}
// What a knob shows: its row's level, or fully down without one.
inline double sendLevel(const engine::SendRoute *s) noexcept { return s != nullptr ? s->gainDb : kSendSilenceDb; }
// Whether a knob is on: a track's unless its row is switched off; a return's only once Enable
// Send has made its row.
inline bool sendOn(const engine::TrackNode &source, const engine::SendRoute *s) noexcept {
    return s != nullptr ? s->enabled : source.kind != "return";
}
namespace send_edits_detail {
// A new row's id: one past the highest send's. The snapshot holds the send rows only, so a main,
// sidechain, VCA or cue row with that id refuses the batch (window log, N39).
inline std::int64_t newId(const SnapshotSends &sends) noexcept {
    std::int64_t id = 1;
    for (const auto &s : sends)
        if (s)
            id = std::max(id, s->id + 1);
    return id;
}
inline OpRequest connect(std::int64_t id, std::int64_t source, std::int64_t destination, const char *label) {
    return track_edits_detail::op("routing.connect",
                                  {{"id", id},
                                   {"srcKind", "track"},
                                   {"src", source},
                                   {"dstKind", "track"},
                                   {"dst", destination},
                                   {"kind", "send"}},
                                  label);
}
// What a knob's value writes: silence at its bottom stop, never past its top.
inline double written(double db) noexcept { return db <= kSendFloorDb ? kSendSilenceDb : std::min(db, kSendTopDb); }
} // namespace send_edits_detail
// A return's Pre/Post (ADR-0203 d3: one switch on each return, not on each send): Pre when every send
// into it taps before the fader. The format keeps the tap on each send row, so a return no send reaches
// yet has no switch of its own to hold and reads Post, where a new send starts (window log, N42).
inline bool returnPre(const SnapshotSends &sends, std::int64_t ret) noexcept {
    bool any = false;
    for (const auto &s : sends)
        if (s && s->destinationKind == "track" && s->destinationId == ret) {
            if (!s->preFader)
                return false;
            any = true;
        }
    return any;
}
// The return's switch thrown: every send into it moved before or after the fader, one batch.
inline TrackEdit setReturnPre(const SnapshotSends &sends, std::int64_t ret, bool pre) {
    constexpr const char *label = "Change Pre/Post";
    TrackEdit edit;
    bool any = false;
    for (const auto &s : sends)
        if (s && s->destinationKind == "track" && s->destinationId == ret) {
            any = true;
            if (s->preFader != pre)
                edit.ops.push_back(track_edits_detail::op("routing.setPreFader", {{"id", s->id}, {"pre", pre}}, label));
        }
    if (!any)
        edit.refusal = "No track sends to this return yet, so there is nothing to switch.";
    return edit;
}
// The send from `source` to `destination` set to `db`, as one batch: the row's level when the row
// exists; else the row, then its level (a new row starts at the format's 0 dB, and the batch lands
// whole or not at all). No ops when the level would not change, so a click that moves nothing
// leaves no undo step; nor for a send without a row left fully down.
inline TrackEdit setSend(const SnapshotSends &sends, std::int64_t source, std::int64_t destination, double db) {
    using namespace send_edits_detail;
    constexpr const char *label = "Change Send Level";
    TrackEdit edit;
    const auto *s = findSend(sends, source, destination);
    if (written(db) == (s != nullptr ? s->gainDb : kSendSilenceDb))
        return edit;
    const auto id = s != nullptr ? s->id : newId(sends);
    if (s == nullptr) {
        edit.ops.push_back(connect(id, source, destination, label));
        // A new send follows its return's switch.
        if (returnPre(sends, destination))
            edit.ops.push_back(track_edits_detail::op("routing.setPreFader", {{"id", id}, {"pre", true}}, label));
    }
    edit.ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", written(db)}}, label));
    return edit;
}
// Live's Enable Send, and its undoing: a row switched on or off; a new row, fully down, when there
// is none to switch on. Nothing to do when the send is already as asked.
inline TrackEdit enableSend(const SnapshotSends &sends, std::int64_t source, std::int64_t destination, bool on) {
    using namespace send_edits_detail;
    const char *label = on ? "Enable Send" : "Disable Send";
    TrackEdit edit;
    const auto *s = findSend(sends, source, destination);
    if (s != nullptr ? s->enabled == on : !on) {
        edit.refusal = on ? "The send is on already." : "The send is off already.";
        return edit;
    }
    if (s != nullptr) {
        edit.ops.push_back(track_edits_detail::op("routing.setEnabled", {{"id", s->id}, {"enabled", on}}, label));
        return edit;
    }
    const auto id = newId(sends);
    edit.ops.push_back(connect(id, source, destination, label));
    if (returnPre(sends, destination))
        edit.ops.push_back(track_edits_detail::op("routing.setPreFader", {{"id", id}, {"pre", true}}, label));
    edit.ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", kSendSilenceDb}}, label));
    return edit;
}
// Live's Enable All Sends (§18.4: "right-click on a Return track's Send knob and select Enable Send or
// Enable All Sends"): every send of the return `source` switched on, to each return, itself included,
// as one batch; a new row for each that has none, fully down, each with its own id.
inline TrackEdit enableAllSends(const SnapshotSends &sends, std::int64_t source, const std::vector<std::int64_t> &returns) {
    using namespace send_edits_detail;
    constexpr const char *label = "Enable All Sends";
    TrackEdit edit;
    auto id = newId(sends);
    for (const auto destination : returns) {
        const auto *s = findSend(sends, source, destination);
        if (s != nullptr) {
            if (!s->enabled)
                edit.ops.push_back(track_edits_detail::op("routing.setEnabled", {{"id", s->id}, {"enabled", true}}, label));
            continue;
        }
        edit.ops.push_back(connect(id, source, destination, label));
        if (returnPre(sends, destination))
            edit.ops.push_back(track_edits_detail::op("routing.setPreFader", {{"id", id}, {"pre", true}}, label));
        edit.ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", kSendSilenceDb}}, label));
        ++id;
    }
    if (edit.ops.empty())
        edit.refusal = "Every send of this return is on already.";
    return edit;
}
} // namespace adi::ui
