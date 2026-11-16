// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui {
// Live's audio clip fades (Live 12 manual 6.8; E4, N17; PLAN ED-10). A fade is a property of its
// clip: a fade in from the clip's start and a fade out to its end, each a length in ticks and a
// curve, written by `clip.setFades` (a plain op: one batch, one undo step). The two fades of a clip
// never overlap: their lengths together are at most the clip's. Only audio clips have fades.
// The curve a fade bent by its handle takes: the engine's Bezier, which tension bends either way.
inline constexpr std::int64_t kBentFadeCurve = 5;
namespace fade_edits_detail {
inline bool isAudio(const engine::ClipNode &c) { return c.audio.has_value(); }
// One clip's fades set, either side or both; a side left out keeps what it has.
inline OpRequest setFades(std::int64_t clip, std::optional<std::int64_t> in, std::optional<std::int64_t> out,
                          const char *label, std::optional<double> inTension = std::nullopt,
                          std::optional<double> outTension = std::nullopt) {
    Payload payload = {{"id", clip}};
    if (in || inTension) {
        Payload side = Payload::object();
        if (in)
            side["ticks"] = *in;
        if (inTension) {
            // A bent fade is the engine's Bezier curve (`curves.hpp`): the linear curve ignores tension.
            side["tension"] = *inTension;
            side["curve"] = kBentFadeCurve;
        }
        payload["in"] = side;
    }
    if (out || outTension) {
        Payload side = Payload::object();
        if (out)
            side["ticks"] = *out;
        if (outTension) {
            side["tension"] = *outTension;
            side["curve"] = kBentFadeCurve;
        }
        payload["out"] = side;
    }
    auto r = track_edits_detail::op("clip.setFades", std::move(payload), label);
    r.targetKind = "clip";
    r.targetId = clip;
    return r;
}
} // namespace fade_edits_detail
// A fade's length kept inside its clip: never negative, never past the other fade.
inline std::int64_t fadeLimit(const engine::ClipNode &c, bool in, std::int64_t ticks) {
    const auto other = in ? c.fadeOutTicks : c.fadeInTicks;
    return std::clamp<std::int64_t>(ticks, 0, std::max<std::int64_t>(0, c.lengthTicks - other));
}
struct FadeEdit {
    std::vector<OpRequest> ops;
    std::string refusal;
    bool crossfade = false; // a boundary between two clips was in the selection
};
// Create Fade In/Out, or Create Crossfade (manual 6.8; Ctrl/Cmd+Alt+F): on each selected track, a
// selection that holds an audio clip's start makes its fade in end at the selection's end; one that
// holds its end makes its fade out start at the selection's start. Over the boundary of two adjacent
// clips, both: the left clip's fade out and the right one's fade in meet at the boundary, a crossfade.
// One batch, so one undo step.
inline FadeEdit createFades(const SnapshotTracks &tracks, const std::vector<std::int64_t> &selected,
                            std::int64_t start, std::int64_t end) {
    FadeEdit edit;
    const auto from = std::min(start, end), to = std::max(start, end);
    if (from == to) {
        edit.refusal = "Select the time to fade first: over an audio clip's start or end, or two clips' boundary.";
        return edit;
    }
    std::vector<OpRequest> ins, outs;
    for (const auto id : selected) {
        const auto *t = track_edits_detail::find(tracks, id);
        if (t == nullptr)
            continue;
        for (const auto &c : t->clips) {
            if (!c || !fade_edits_detail::isAudio(*c))
                continue;
            const auto a = c->posTicks, b = c->posTicks + c->lengthTicks;
            const bool holdsStart = a >= from && a < to && to < b;
            const bool holdsEnd = b > from && b <= to && from > a;
            std::optional<std::int64_t> in, out;
            if (holdsStart)
                in = fadeLimit(*c, true, to - a);
            if (holdsEnd)
                out = fadeLimit(*c, false, b - from);
            if (!in && !out)
                continue;
            // A clip that ends where another begins, both inside the selection: a crossfade.
            if (holdsEnd)
                for (const auto &n : t->clips)
                    if (n && n.get() != c.get() && fade_edits_detail::isAudio(*n) && n->posTicks == b && b < to)
                        edit.crossfade = true;
            edit.ops.push_back(fade_edits_detail::setFades(c->id, in, out, "Create Fade"));
        }
    }
    if (edit.ops.empty()) {
        edit.refusal = "The selection holds no audio clip's start or end: select from before a clip's start, or "
                       "past its end.";
        return edit;
    }
    if (edit.crossfade)
        for (auto &op : edit.ops)
            op.label = "Create Crossfade";
    return edit;
}
// Delete Fades/Crossfades (Ctrl/Cmd+Alt+Backspace): both fades of each given audio clip back to none.
// Create Fades on Clip Edges stays off (ADR-0132 d6), so a fade goes to nothing, not to 4 ms.
inline FadeEdit deleteFades(const SnapshotTracks &tracks, const std::vector<std::int64_t> &clips) {
    FadeEdit edit;
    for (const auto &t : tracks)
        if (t)
            for (const auto &c : t->clips)
                if (c && fade_edits_detail::isAudio(*c) &&
                    std::find(clips.begin(), clips.end(), c->id) != clips.end() &&
                    (c->fadeInTicks != 0 || c->fadeOutTicks != 0))
                    edit.ops.push_back(fade_edits_detail::setFades(c->id, 0, 0, "Delete Fades"));
    if (edit.ops.empty())
        edit.refusal = "The selected clips have no fades to delete.";
    return edit;
}
// One fade dragged by its handle (manual 6.8): the same length, or the same curve, on the dragged
// clip and every other selected audio clip, each kept inside its clip. One batch.
inline FadeEdit dragFades(const SnapshotTracks &tracks, const std::vector<std::int64_t> &clips, bool in,
                          std::optional<std::int64_t> ticks, std::optional<double> tension) {
    FadeEdit edit;
    for (const auto &t : tracks)
        if (t)
            for (const auto &c : t->clips)
                if (c && fade_edits_detail::isAudio(*c) && std::find(clips.begin(), clips.end(), c->id) != clips.end()) {
                    const auto length = ticks ? std::optional<std::int64_t>(fadeLimit(*c, in, *ticks)) : std::nullopt;
                    const char *label = tension ? "Change Fade Curve" : "Change Fade";
                    edit.ops.push_back(in ? fade_edits_detail::setFades(c->id, length, std::nullopt, label, tension)
                                          : fade_edits_detail::setFades(c->id, std::nullopt, length, label, std::nullopt,
                                                                        tension));
                }
    return edit;
}
} // namespace adi::ui
