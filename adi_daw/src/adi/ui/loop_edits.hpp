// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/transport_settings.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
// Live's metronome and Arrangement Loop (Live 12 manual 6.6, 19.4.1): the control bar's switches and
// fields, O, Loop Selection (Ctrl/Cmd+L) and the loop brace. Both are the transport's saved state
// (#383, N19): `transport.setMetronome {on}` and `transport.setLoop {on,start,end}`, in ticks,
// outside undo and saved with the project. The window reads them from `Snapshot::transport`. Each
// edit is at most one op; the same state again is none.
struct LoopRange {
    std::int64_t start = 0, end = 0;
    std::int64_t length() const noexcept { return end - start; }
    bool operator==(const LoopRange &) const = default;
};
inline LoopRange loopOf(const TransportSettings &t) noexcept { return {t.loopStartTicks, t.loopEndTicks}; }
struct LoopEdit {
    std::vector<OpRequest> ops;
    std::string refusal;
};
inline LoopEdit setMetronome(const TransportSettings &t, bool on) {
    LoopEdit edit;
    if (t.metronome != on)
        edit.ops.push_back(track_edits_detail::op("transport.setMetronome", {{"on", on}}, "Metronome"));
    return edit;
}
// The loop on or off over `range`. A brace never starts before the start nor ends at or before its
// own start.
inline LoopEdit setLoop(const TransportSettings &t, bool on, LoopRange range) {
    LoopEdit edit;
    if (range.start < 0 || range.end <= range.start) {
        edit.refusal = "The loop needs a start at or after the arrangement's start and an end after it.";
        return edit;
    }
    if (t.loopEnabled == on && loopOf(t) == range)
        return edit;
    edit.ops.push_back(track_edits_detail::op("transport.setLoop",
                                              {{"on", on}, {"start", range.start}, {"end", range.end}}, "Loop"));
    return edit;
}
// Loop Selection (manual 6.6): "turn on the Arrangement loop and set the loop brace to the current
// time selection ... which can also be used [to] toggle the Arrangement loop on or off when a clip or
// time is selected". With `selected` (a time selection, or the selected clips' span) the brace
// takes it and the loop goes on; where the brace is already there, the loop toggles. With nothing
// selected, the loop toggles where it is.
inline LoopEdit loopSelection(const TransportSettings &t, std::optional<LoopRange> selected) {
    if (!selected || *selected == loopOf(t))
        return setLoop(t, !t.loopEnabled, loopOf(t));
    return setLoop(t, true, *selected);
}
// The brace's keys while it is selected (manual 6.6, §42.8). `next(tick, forward)` is the grid line
// after or before `tick` (the arrangement's `nextGridLine`). Left and right nudge the brace by the
// grid, its length kept; up and down move it by its length; with Ctrl/Cmd, left and right shorten or
// lengthen it by the grid and up and down double or halve it. The brace stays after the start and at
// least one grid step long; none when it would not move.
enum class BraceKey { Left, Right, Up, Down };
template <class Next>
std::optional<LoopRange> braceKey(LoopRange r, BraceKey key, bool command, Next &&next) {
    const auto length = r.length();
    if (length <= 0)
        return std::nullopt;
    LoopRange to = r;
    if (!command) {
        switch (key) {
        case BraceKey::Left:
        case BraceKey::Right:
            to.start = next(r.start, key == BraceKey::Right);
            break;
        case BraceKey::Up:
            to.start = r.start + length;
            break;
        case BraceKey::Down:
            to.start = r.start - length;
            break;
        }
        if (to.start < 0)
            return std::nullopt;
        to.end = to.start + length;
    } else {
        switch (key) {
        case BraceKey::Left:
        case BraceKey::Right:
            to.end = next(r.end, key == BraceKey::Right);
            break;
        case BraceKey::Up:
            to.end = r.start + 2 * length;
            break;
        case BraceKey::Down:
            to.end = r.start + length / 2;
            break;
        }
        if (to.end <= r.start)
            return std::nullopt;
    }
    if (to == r)
        return std::nullopt;
    return to;
}
// A length typed in the Loop Length field (manual 6.6): bars, beats and sixteenths, each from 0, the
// later ones 0 when left out, separated as a position is. All three 0 is no length.
inline std::optional<BarPosition> parseLength(std::string_view text) {
    BarPosition out{0, 0, 0};
    std::int64_t *parts[] = {&out.bar, &out.beat, &out.sixteenth};
    std::size_t i = 0, n = 0;
    const auto space = [&] { return i < text.size() && std::isspace(static_cast<unsigned char>(text[i])); };
    while (space())
        ++i;
    while (i < text.size()) {
        if (n == 3)
            return std::nullopt;
        std::int64_t value = 0;
        const auto from = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) && i - from < 5)
            value = value * 10 + (text[i++] - '0');
        if (i == from || (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))))
            return std::nullopt;
        *parts[n++] = value;
        const auto beforeGap = i;
        while (space())
            ++i;
        if (i < text.size() && (text[i] == '.' || text[i] == ',' || text[i] == ':')) {
            ++i;
            while (space())
                ++i;
            if (i == text.size())
                return std::nullopt;
        } else if (i < text.size() && i == beforeGap) {
            return std::nullopt;
        }
    }
    if (n == 0 || (out.bar == 0 && out.beat == 0 && out.sixteenth == 0))
        return std::nullopt;
    return out;
}
// A length's ticks from `start`, in the meter in force there, as `barSpan` shows one.
inline std::int64_t lengthTicks(std::int64_t start, const BarPosition &length, const std::vector<textproj::Meter> &meters) {
    const auto &m = meterAt(start, meters);
    const auto beatTicks = m.denominator > 0 ? textproj::kWhole / static_cast<std::int64_t>(m.denominator) : 0;
    return length.bar * m.barTicks() + length.beat * beatTicks + length.sixteenth * (textproj::kWhole / 16);
}
// The Loop Start field typed: the brace starts there, its length kept.
inline LoopEdit typeLoopStart(const TransportSettings &t, std::string_view text, const std::vector<textproj::Meter> &meters) {
    LoopEdit edit;
    const auto at = parsePosition(text);
    const auto tick = at ? tickOf(*at, meters) : std::nullopt;
    if (!tick) {
        edit.refusal = "Type the loop's start such as 5.1.1: bar, beat and sixteenth.";
        return edit;
    }
    const auto r = loopOf(t);
    return setLoop(t, t.loopEnabled, {*tick, *tick + r.length()});
}
// The Loop Length field typed: the brace ends that far after its start.
inline LoopEdit typeLoopLength(const TransportSettings &t, std::string_view text, const std::vector<textproj::Meter> &meters) {
    LoopEdit edit;
    const auto length = parseLength(text);
    if (!length) {
        edit.refusal = "Type the loop's length such as 4.0.0: bars, beats and sixteenths.";
        return edit;
    }
    const auto start = t.loopStartTicks;
    return setLoop(t, t.loopEnabled, {start, start + lengthTicks(start, *length, meters)});
}
} // namespace adi::ui
