// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
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
// The returns `parent` holds (0: the top level), in the arrangement's order (TrackOrder's: by
// index_in_parent, then by place in the snapshot), appended to `out` without allocating once it has
// room. An insertion sort: stable, so returns that share an index keep the snapshot's order, and
// unlike std::stable_sort it takes no buffer. A project has a handful of returns.
inline void appendReturnsOf(const SnapshotTracks &tracks, std::int64_t parent, std::vector<std::int64_t> &out) {
    const auto from = out.size();
    for (const auto &t : tracks)
        if (t && t->kind == "return" && t->parent == parent)
            out.push_back(t->id);
    auto placeOf = [&tracks](std::int64_t id) { return track_edits_detail::find(tracks, id)->indexInParent; };
    for (std::size_t i = from + 1; i < out.size(); ++i)
        for (std::size_t j = i; j > from && placeOf(out[j]) < placeOf(out[j - 1]); --j)
            std::swap(out[j], out[j - 1]);
}
// Every return: the top-level ones in the arrangement's order, then each group's, the groups in the
// snapshot's order (Main's Pre/Post switches). Refilled in place, so the same returns allocate nothing.
inline void returnsInOrder(const SnapshotTracks &tracks, std::vector<std::int64_t> &out) {
    out.clear();
    appendReturnsOf(tracks, 0, out);
    for (const auto &g : tracks)
        if (g && g->kind == "group")
            appendReturnsOf(tracks, g->id, out);
}
// The returns a track's send knobs go to (DF-W33; ADR-0247 d2; ADR-0245 d9): the top-level returns,
// then the returns of every group that holds it, at any depth, the outermost first. A group's return
// takes sends only from inside its group, so a track outside it has no knob for it; nor has a group's
// return a knob for itself, as the agreed sheet draws it (a top-level return keeps Live's, greyed).
inline void returnsFor(const SnapshotTracks &tracks, std::int64_t track, std::vector<std::int64_t> &out) {
    out.clear();
    appendReturnsOf(tracks, 0, out);
    // The groups that hold `track`, innermost first, then walked outermost first. A loop of parents,
    // which a project never holds, stops at the snapshot's size.
    std::array<std::int64_t, 32> holders{};
    std::size_t held = 0;
    const auto *t = track_edits_detail::find(tracks, track);
    for (std::size_t steps = 0; t != nullptr && t->parent != 0 && held < holders.size() && steps < tracks.size(); ++steps) {
        holders[held++] = t->parent;
        t = track_edits_detail::find(tracks, t->parent);
    }
    while (held > 0)
        appendReturnsOf(tracks, holders[--held], out);
    if (const auto *self = track_edits_detail::find(tracks, track); self != nullptr && self->kind == "return" && self->parent != 0)
        out.erase(std::remove(out.begin(), out.end(), track), out.end());
}
// A return's letter, as its knobs, its activator and its column show it (ADR-0247 d2): a top-level
// return's by its place among the top-level ones ("A", "B"); a group's return's is the group's
// initial, then its place among that group's returns ("DA" for the Drums group's first), so A and B
// keep their letters everywhere. Written into `out`, nothing allocated; its length is returned.
inline std::size_t returnLetter(const SnapshotTracks &tracks, std::int64_t ret, std::array<char, 2> &out) {
    // Its place as appendReturnsOf orders them: by index_in_parent, then by place in the snapshot.
    std::size_t at = tracks.size();
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i] && tracks[i]->id == ret)
            at = i;
    const auto *r = at < tracks.size() ? tracks[at].get() : nullptr;
    const auto parent = r != nullptr ? r->parent : 0;
    std::size_t place = 0;
    for (std::size_t i = 0; r != nullptr && i < tracks.size(); ++i)
        if (const auto &t = tracks[i]; t && i != at && t->kind == "return" && t->parent == parent &&
                                       (t->indexInParent < r->indexInParent || (t->indexInParent == r->indexInParent && i < at)))
            ++place;
    const auto letter = static_cast<char>('A' + place % 26);
    if (parent == 0) {
        out[0] = letter;
        return 1;
    }
    char initial = 'G';
    if (const auto *g = track_edits_detail::find(tracks, parent))
        for (const char c : g->name)
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                initial = static_cast<char>(c >= 'a' ? c - 'a' + 'A' : c);
                break;
            }
    out[0] = initial;
    out[1] = letter;
    return 2;
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
// Why a new send's batch is refused when no routing id is left (window log N39): the op carries its id
// (ADR-0021), and the snapshot's highest is already the largest the project can hold. "The project" is
// what every other Status Bar line of the shell calls the open document.
inline constexpr const char *kSendNoIdLeft =
    "No routing id is left: the project's highest routing id is already the largest it can hold.";
namespace send_edits_detail {
// A new row's id: one past the highest routing id of any kind, `Snapshot::maxRoutingId` (#403, window
// log N39), so it cannot collide with a main, sidechain, VCA or cue row, which the snapshot's `sends`
// leave out; a batch that makes several rows takes the next ids after it. The snapshot's maximum is an
// observation, not an allocator: the batch commits against the head it read, and another writer's row
// in between refuses it there. A frame never holds a send above its maximum (the engine's doc).
// The first of `count` new ids; 0 when `count` ids do not fit at or below INT64_MAX (the last may equal
// it), which refuses the edit with `kSendNoIdLeft`. 0 is never an id: `maxRoutingId` is at least 0.
inline std::int64_t newIds(const engine::Snapshot &snapshot, std::int64_t count) noexcept {
    const auto highest = snapshot.maxRoutingId;
    return count <= std::numeric_limits<std::int64_t>::max() - highest ? highest + 1 : 0;
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
// A return's Pre/Post (ADR-0203 d3: one switch on each return, not on each send) is the return's own
// (#405, window log N42): `TrackNode::returnSendTap` holds it, explicit once the switch was thrown and
// kept while no send reaches the return, else read from its sends, Pre only when every one taps before
// the fader (an older file's mixed sends read Post). Any other track, or none, reads Post.
inline bool returnPre(const engine::TrackNode *ret) noexcept {
    return ret != nullptr && ret->returnSendTap && ret->returnSendTap->preFader;
}
inline bool returnPre(const SnapshotTracks &tracks, std::int64_t ret) noexcept {
    return returnPre(track_edits_detail::find(tracks, ret));
}
namespace send_edits_detail {
// What a new send into `ret` carries to follow the return's switch: nothing once the return holds a
// choice, since `routing.connect` reads it (#405); `routing.setPreFader` for an older file whose sends
// are all Pre with no choice written, as the engine's own `prepareSendToReturn` adds it.
inline bool inheritsPre(const engine::TrackNode *ret) noexcept {
    return ret != nullptr && ret->returnSendTap && !ret->returnSendTap->explicitChoice && ret->returnSendTap->preFader;
}
inline void follow(TrackEdit &edit, const engine::TrackNode *ret, std::int64_t id, const char *label) {
    if (inheritsPre(ret))
        edit.ops.push_back(track_edits_detail::op("routing.setPreFader", {{"id", id}, {"pre", true}}, label));
}
} // namespace send_edits_detail
// A send's own tap (ADR-0245 d10; DF-W33): Pre FX, Post FX or Post Mixer, which the send knob's menu
// chooses; where the send has none of its own, the return's switch decides (Pre is Post FX, Post is
// Post Mixer). Nothing for a track with no send row to that return.
inline std::optional<IndividualSendTap> sendTap(const engine::Snapshot &snapshot, std::int64_t source,
                                                std::int64_t ret) {
    const auto *s = findSend(snapshot.sends, source, ret);
    if (s == nullptr)
        return std::nullopt;
    if (s->sendTap)
        return s->sendTap;
    return returnPre(snapshot.tracks, ret) || s->preFader ? IndividualSendTap::PostFx : IndividualSendTap::PostMixer;
}
// One `routing.setSendTap` on the send from `source` to `ret` (#677): one undo step. Refused, with the
// reason, while the send has no row (it is fully down: turn it up first); no ops when it already taps
// there, through its own tap or its return's switch; the switch's own tap clears the send's, so it
// follows the switch again rather than being pinned against it.
inline TrackEdit setSendTap(const engine::Snapshot &snapshot, std::int64_t source, std::int64_t ret,
                            IndividualSendTap tap) {
    TrackEdit edit;
    const auto *s = findSend(snapshot.sends, source, ret);
    if (s == nullptr) {
        edit.refusal = "This send is fully down: turn it up first, then choose where it taps.";
        return edit;
    }
    // Where it taps without a tap of its own: the return's switch (or an older file's own flag).
    const auto given = returnPre(snapshot.tracks, ret) || s->preFader ? IndividualSendTap::PostFx : IndividualSendTap::PostMixer;
    if ((s->sendTap ? *s->sendTap : given) == tap)
        return edit; // it taps there already: no step
    if (tap == given) // the switch's own tap: the send follows the switch again, its own tap cleared
        edit.ops.push_back(track_edits_detail::op("routing.setSendTap", {{"id", s->id}}, "Change Send Tap"));
    else
        edit.ops.push_back(track_edits_detail::op(
            "routing.setSendTap", {{"id", s->id}, {"tap", std::string(individualSendTapName(tap))}}, "Change Send Tap"));
    return edit;
}
// The return's switch thrown: one `return.setSendTap`, which the engine spreads over every send into
// the return, those it has and each one connected after. It holds with no send, so it never refuses.
inline TrackEdit setReturnPre(std::int64_t ret, bool pre) {
    TrackEdit edit;
    edit.ops.push_back(track_edits_detail::op("return.setSendTap", {{"id", ret}, {"pre", pre}}, "Change Pre/Post"));
    return edit;
}
// Whether a return's Pre/Post switch is mixed (ADR-0247 d2, the sheet's "the sends differ"): the taps
// its sends play from differ among themselves, each send's own or the switch's, as the engine reads
// them (`ReturnSendTap::legacyMixed`, any file, #677). One send with a tap of its own is not mixed: its
// knob carries no mark, its menu says it.
inline bool returnTapMixed(const engine::Snapshot &snapshot, std::int64_t ret) noexcept {
    const auto *r = track_edits_detail::find(snapshot.tracks, ret);
    return r != nullptr && r->returnSendTap && r->returnSendTap->legacyMixed;
}
// The send from `source` to `destination` set to `db`, as one batch: the row's level when the row
// exists; else the row, then its level (a new row starts at the format's 0 dB, and the batch lands
// whole or not at all). The batch reads the snapshot an action lands on: its sends for the rows, its
// highest routing id for the new one, its tracks for the return's switch. No ops when the level would
// not change, so a click that moves nothing leaves no undo step; nor for a send without a row left
// fully down. A new row with no id left is refused, with the reason.
inline TrackEdit setSend(const engine::Snapshot &snapshot, std::int64_t source, std::int64_t destination, double db) {
    using namespace send_edits_detail;
    constexpr const char *label = "Change Send Level";
    TrackEdit edit;
    const auto *s = findSend(snapshot.sends, source, destination);
    if (written(db) == (s != nullptr ? s->gainDb : kSendSilenceDb))
        return edit;
    const auto id = s != nullptr ? s->id : newIds(snapshot, 1);
    if (s == nullptr) {
        if (id == 0) {
            edit.refusal = kSendNoIdLeft;
            return edit;
        }
        edit.ops.push_back(connect(id, source, destination, label));
        follow(edit, snapshot.findTrack(destination), id, label); // a new send follows its return's switch
    }
    edit.ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", written(db)}}, label));
    return edit;
}
namespace send_edits_detail {
// Whether the send from `source` reads on: `sendOn`'s one rule, for Enable Send's tick and for what
// Enable Send writes (DF-W35 (a)). A source the snapshot does not hold reads a missing row as off.
inline bool readsOn(const engine::Snapshot &snapshot, std::int64_t source, const engine::SendRoute *s) noexcept {
    const auto *src = snapshot.findTrack(source);
    return src != nullptr ? sendOn(*src, s) : s != nullptr && s->enabled;
}
inline TrackEdit silentSend(const engine::Snapshot &snapshot, std::int64_t source, std::int64_t destination,
                            bool enabled, const char *label) {
    TrackEdit edit;
    const auto id = newIds(snapshot, 1);
    if (id == 0) {
        edit.refusal = kSendNoIdLeft;
        return edit;
    }
    edit.ops.push_back(connect(id, source, destination, label));
    follow(edit, snapshot.findTrack(destination), id, label);
    edit.ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", kSendSilenceDb}}, label));
    if (!enabled)
        edit.ops.push_back(track_edits_detail::op("routing.setEnabled", {{"id", id}, {"enabled", false}}, label));
    return edit;
}
} // namespace send_edits_detail

// The send starts at its current silent value and enabled state. Its routing row
// then gives the automation chooser a stable target without affecting the mix.
inline TrackEdit prepareSendAutomation(const engine::Snapshot &snapshot, std::int64_t source,
                                       std::int64_t destination) {
    if (findSend(snapshot.sends, source, destination))
        return {};
    using namespace send_edits_detail;
    return silentSend(snapshot, source, destination, readsOn(snapshot, source, nullptr), "Prepare Send Automation");
}
// Live's Enable Send, and its undoing: a row switched on or off. Without a row, the send reads as
// `sendOn` reads it, on for a track and off for a return, so the menu's tick and its action agree: a
// return's Enable Send makes the row, fully down; a track's Disable Send makes the row, fully down, as
// the knob shows it, and switched off. Refused, with the reason, when no id is left for that row;
// nothing to do when the send is already as asked.
inline TrackEdit enableSend(const engine::Snapshot &snapshot, std::int64_t source, std::int64_t destination, bool on) {
    using namespace send_edits_detail;
    const char *label = on ? "Enable Send" : "Disable Send";
    TrackEdit edit;
    const auto *s = findSend(snapshot.sends, source, destination);
    if (readsOn(snapshot, source, s) == on) {
        edit.refusal = on ? "The send is on already." : "The send is off already.";
        return edit;
    }
    if (s != nullptr) {
        edit.ops.push_back(track_edits_detail::op("routing.setEnabled", {{"id", s->id}, {"enabled", on}}, label));
        return edit;
    }
    return silentSend(snapshot, source, destination, on, label);
}
// Live's Enable All Sends (§18.4: "right-click on a Return track's Send knob and select Enable Send or
// Enable All Sends"): every send of the return `source` switched on, to each return, itself included,
// as one batch; a new row for each that has none, fully down, the rows' ids consecutive from the
// snapshot's highest. Refused, with the reason, when the new rows do not all fit below INT64_MAX.
inline TrackEdit enableAllSends(const engine::Snapshot &snapshot, std::int64_t source, const std::vector<std::int64_t> &returns) {
    using namespace send_edits_detail;
    constexpr const char *label = "Enable All Sends";
    TrackEdit edit;
    const auto &sends = snapshot.sends;
    // A send needs a new row only when it reads off without one, as a return's does; a track's reads on.
    const bool missingOn = readsOn(snapshot, source, nullptr);
    std::int64_t rows = 0;
    for (const auto destination : returns)
        if (!missingOn && findSend(sends, source, destination) == nullptr)
            ++rows;
    auto id = rows > 0 ? newIds(snapshot, rows) : 0;
    if (rows > 0 && id == 0) {
        edit.refusal = kSendNoIdLeft;
        return edit;
    }
    for (const auto destination : returns) {
        const auto *s = findSend(sends, source, destination);
        if (s != nullptr) {
            if (!s->enabled)
                edit.ops.push_back(track_edits_detail::op("routing.setEnabled", {{"id", s->id}, {"enabled", true}}, label));
            continue;
        }
        if (missingOn)
            continue;
        edit.ops.push_back(connect(id, source, destination, label));
        follow(edit, snapshot.findTrack(destination), id, label);
        edit.ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", kSendSilenceDb}}, label));
        // The next row's id; none past the last row, whose id may be INT64_MAX itself (DF-W35 (b)).
        if (--rows > 0)
            ++id;
    }
    if (edit.ops.empty())
        edit.refusal = "Every send of this return is on already.";
    return edit;
}
// Live's sends to a new return (manual 18.4; ADR-0203 d3: a knob per return on every track, fully down
// until turned up): a row from each track with a knob for `ret`, at the silence floor, for the batch that
// makes the return, as the default Set's A and B have theirs. A knob with a row is dragged live, heard
// while it moves; one without is written only at the release (walk-A A10). Not from Main, a VCA or a
// return (a return's sends are off until Enable Send); for a group's return (`group`), only from the
// tracks inside it. Ids consecutive from the snapshot's highest; none when they do not all fit, and a
// knob then makes its row when turned up, as before.
inline std::vector<OpRequest> newReturnSends(const engine::Snapshot &snapshot, std::int64_t ret, std::int64_t group,
                                             const char *label) {
    using namespace send_edits_detail;
    const auto &tracks = snapshot.tracks;
    const auto inside = [&tracks, group](const engine::TrackNode &t) {
        if (group == 0)
            return true;
        const auto *at = &t;
        for (std::size_t steps = 0; at != nullptr && at->parent != 0 && steps < tracks.size(); ++steps) {
            if (at->parent == group)
                return true;
            at = track_edits_detail::find(tracks, at->parent);
        }
        return false;
    };
    std::vector<std::int64_t> sources;
    for (const auto &t : tracks)
        if (t && t->id != ret && t->kind != "master" && t->kind != "vca" && t->kind != "return" && inside(*t))
            sources.push_back(t->id);
    std::vector<OpRequest> ops;
    auto id = sources.empty() ? 0 : newIds(snapshot, static_cast<std::int64_t>(sources.size()));
    if (id == 0)
        return ops;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        ops.push_back(connect(id, sources[i], ret, label));
        ops.push_back(track_edits_detail::op("routing.setGain", {{"id", id}, {"db", kSendSilenceDb}}, label));
        if (i + 1 < sources.size()) // none past the last, whose id may be INT64_MAX itself
            ++id;
    }
    return ops;
}
} // namespace adi::ui
