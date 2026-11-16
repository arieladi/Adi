// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Clip edits the Clip View and the arrangement share, headless (STEP-7-PLAN 7.7): each builds one
// batch from the snapshot the action lands on (ADR-0201), for the window to commit as one undo step.
#pragma once
#include "adi/engine/clip_details.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/note_geometry.hpp"
#include "adi/ui/track_edits.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
// ---- The Clip View's Main panel (manual 8.2; N33's ops) -------------------------------------------
// Each builds one op from the clip as it was read. Times are source-relative musical ticks.

// Where the clip's End marker is: its own, else where the clip's length puts it.
inline std::int64_t endMarkerOf(const engine::ClipDetails &clip) noexcept {
    return clip.endMarkerTicks.value_or(clip.startMarkerTicks + clip.lengthTicks.value_or(0));
}
// The loop's window: its own, else from its start to the End marker.
inline std::int64_t loopLengthOf(const engine::ClipDetails &clip) noexcept {
    return clip.loopLengthTicks.value_or(std::max<std::int64_t>(0, endMarkerOf(clip) - clip.loopStartTicks));
}
inline TrackEdit setClipStart(const engine::ClipDetails &clip, std::int64_t ticks) {
    using track_edits_detail::op;
    TrackEdit edit;
    if (ticks < 0)
        edit.refusal = "A clip's Start cannot come before its beginning.";
    else if (ticks >= endMarkerOf(clip))
        edit.refusal = "A clip's Start comes before its End.";
    else if (ticks != clip.startMarkerTicks)
        edit.ops.push_back(op("clip.setStartMarker", {{"id", clip.id}, {"ticks", ticks}}, "Set Clip Start"));
    return edit;
}
inline TrackEdit setClipEnd(const engine::ClipDetails &clip, std::int64_t ticks) {
    using track_edits_detail::op;
    TrackEdit edit;
    if (ticks <= clip.startMarkerTicks)
        edit.refusal = "A clip's End comes after its Start.";
    else if (ticks != endMarkerOf(clip))
        edit.ops.push_back(op("clip.setEndMarker", {{"id", clip.id}, {"ticks", ticks}}, "Set Clip End"));
    return edit;
}
// The Loop switch: the window it had is kept while it is off (OPS: a switch-only edit keeps it);
// a clip that never had one loops from its Start marker to its End.
inline TrackEdit toggleClipLoop(const engine::ClipDetails &clip) {
    using track_edits_detail::op;
    TrackEdit edit;
    const bool on = !clip.loopEnabled;
    Payload payload = {{"id", clip.id}, {"on", on}};
    if (on && (!clip.loopLengthTicks || *clip.loopLengthTicks <= 0 || clip.loopStartTicks < 0)) {
        const auto start = std::max<std::int64_t>(0, clip.startMarkerTicks);
        const auto length = endMarkerOf(clip) - start;
        if (length <= 0) {
            edit.refusal = "This clip has nothing to loop.";
            return edit;
        }
        payload["start"] = start;
        payload["length"] = length;
    }
    edit.ops.push_back(op("clip.setLoop", std::move(payload), on ? "Activate Loop" : "Deactivate Loop"));
    return edit;
}
// The loop's Position and Length (manual 8.2.2): the switch stays as it is, unless `on` says
// otherwise (Live's Set buttons switch the loop on).
inline TrackEdit setClipLoop(const engine::ClipDetails &clip, std::int64_t start, std::int64_t length,
                             std::optional<bool> on = std::nullopt) {
    using track_edits_detail::op;
    TrackEdit edit;
    if (start < 0)
        edit.refusal = "A loop cannot start before the clip's beginning.";
    else if (length <= 0)
        edit.refusal = "A loop needs a length.";
    else if (start != clip.loopStartTicks || length != loopLengthOf(clip) || !clip.loopLengthTicks ||
             on.value_or(clip.loopEnabled) != clip.loopEnabled)
        edit.ops.push_back(op("clip.setLoop",
                              {{"id", clip.id}, {"on", on.value_or(clip.loopEnabled)}, {"start", start}, {"length", length}},
                              start != clip.loopStartTicks ? "Set Loop Position" : "Set Loop Length"));
    return edit;
}
// The clip's own time signature (manual 8.2.4): 1 to 99 over 1, 2, 4, 8 or 16, as Live's fields take.
inline TrackEdit setClipSignature(const engine::ClipDetails &clip, std::int64_t numerator, std::int64_t denominator) {
    using track_edits_detail::op;
    TrackEdit edit;
    const bool power = denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16;
    if (numerator < 1 || numerator > 99 || !power)
        edit.refusal = "A time signature is 1 to 99 over 1, 2, 4, 8 or 16.";
    else if (!clip.signature.local || numerator != clip.signature.numerator || denominator != clip.signature.denominator)
        edit.ops.push_back(op("clip.setTimeSignature", {{"id", clip.id}, {"num", numerator}, {"den", denominator}},
                              "Change Clip Time Signature"));
    return edit;
}
// The clip's own scale (manual 8.2.5): its root, 0 to 11, the scale's id and its pitches above the
// root as a 12-bit mask that holds the root.
inline TrackEdit setClipScale(const engine::ClipDetails &clip, std::int64_t root, const std::string &id,
                              std::int64_t mask) {
    using track_edits_detail::op;
    TrackEdit edit;
    if (root < 0 || root > 11 || id.empty() || (mask & 1) == 0 || mask < 0 || mask > 0xfff)
        edit.refusal = "A scale has a root and holds it.";
    else if (!clip.scale.local || clip.scale.root != root || clip.scale.id != id || clip.scale.mask != mask)
        edit.ops.push_back(op("clip.setScale", {{"id", clip.id}, {"root", root}, {"scale", id}, {"mask", mask}},
                              "Change Clip Scale"));
    return edit;
}

// Scale Mode (manual 8.2.4): on, the clip holds the scale it shows as its own; off, it holds none
// and follows the project's again.
inline TrackEdit setClipScaleMode(const engine::ClipDetails &clip, bool on) {
    using track_edits_detail::op;
    TrackEdit edit;
    if (on == clip.scale.local)
        return edit;
    if (on)
        edit.ops.push_back(op("clip.setScale", {{"id", clip.id}, {"root", clip.scale.root}, {"scale", clip.scale.id}, {"mask", clip.scale.mask}},
                              "Activate Scale"));
    else
        edit.ops.push_back(op("clip.setScale", {{"id", clip.id}}, "Deactivate Scale"));
    return edit;
}

namespace clip_edits_detail {
inline bool digit(char c) noexcept { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
inline bool gap(char c) noexcept {
    return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '.' || c == ',' || c == ':' || c == '/';
}
// Up to `most` whole numbers separated by gaps; none when anything else is typed.
inline std::optional<std::vector<std::int64_t>> numbers(std::string_view text, std::size_t most) {
    std::vector<std::int64_t> out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (gap(text[i])) {
            ++i;
            continue;
        }
        if (!digit(text[i]) || out.size() == most)
            return std::nullopt;
        std::int64_t value = 0;
        const auto from = i;
        while (i < text.size() && digit(text[i]) && i - from < 6)
            value = value * 10 + (text[i++] - '0');
        if (i < text.size() && digit(text[i]))
            return std::nullopt;
        out.push_back(value);
    }
    if (out.empty())
        return std::nullopt;
    return out;
}
} // namespace clip_edits_detail

// A length typed as Live's Length fields read it: bars, beats and sixteenths from zero, the later
// ones 0 when left out, separated by a point, a comma, a colon or spaces.
inline std::optional<BarsBeats> parseBarsBeats(std::string_view text) {
    const auto parts = clip_edits_detail::numbers(text, 3);
    if (!parts)
        return std::nullopt;
    BarsBeats out;
    out.bars = (*parts)[0];
    out.beats = parts->size() > 1 ? (*parts)[1] : 0;
    out.sixteenths = parts->size() > 2 ? (*parts)[2] : 0;
    return out;
}
inline std::int64_t ticksOf(const BarsBeats &length, const textproj::Meter &meter) noexcept {
    const std::int64_t beat = meter.denominator == 0 ? 0 : textproj::kWhole / meter.denominator;
    return length.bars * meter.barTicks() + length.beats * beat + length.sixteenths * (textproj::kPPQ / 4);
}
// A time signature typed as "3/4" or "3 4".
inline std::optional<std::pair<std::int64_t, std::int64_t>> parseSignature(std::string_view text) {
    const auto parts = clip_edits_detail::numbers(text, 2);
    if (!parts || parts->size() != 2)
        return std::nullopt;
    return std::pair<std::int64_t, std::int64_t>{(*parts)[0], (*parts)[1]};
}
} // namespace adi::ui
