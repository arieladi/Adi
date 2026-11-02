// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/textproj.hpp"
#include <cstdint>
#include <utility>
#include <vector>
namespace adi::ui {
// Bars, beats and sixteenths in the project's meters (Live 12 manual 6.1, 6.2): the numbers the
// ruler, the control bar and the Status Bar show. The snapshot's meters (#299) are sorted, with
// an implicit 4/4 at zero; an empty list is 4/4 throughout.
//
// Bars accumulate across signature changes as the text projection counts them
// (textproj::renderPosition): a change part-way through a bar starts a new bar, and the short bar
// before it still counts. A beat is the signature's denominator (an eighth in 6/8); a sixteenth is
// a sixteenth note, so a beat of an eighth holds two.
struct BarPosition {
    std::int64_t bar = 1, beat = 1, sixteenth = 1; // each from 1
    bool operator==(const BarPosition &) const = default;
};
namespace meters_detail {
inline const textproj::Meter &defaultMeter() noexcept {
    static const textproj::Meter m{};
    return m;
}
// The meter in force at `tick`, the tick its segment starts at, and the bars before that.
struct Segment {
    const textproj::Meter *meter;
    std::int64_t start, barsBefore;
};
inline Segment segmentAt(std::int64_t tick, const std::vector<textproj::Meter> &meters) noexcept {
    Segment s{&defaultMeter(), 0, 0};
    for (std::size_t i = 0; i < meters.size(); ++i) {
        const auto &m = meters[i];
        if (m.start_ticks > tick)
            break;
        if (i > 0) {
            const auto bt = s.meter->barTicks();
            if (bt > 0) {
                const auto span = m.start_ticks - s.start;
                s.barsBefore += span / bt + (span % bt != 0 ? 1 : 0);
            }
        }
        s.meter = &m;
        s.start = m.start_ticks;
    }
    return s;
}
} // namespace meters_detail
// Where `tick` falls: bar, beat and sixteenth, each from 1. A tick before zero counts back.
inline BarPosition barPosition(std::int64_t tick, const std::vector<textproj::Meter> &meters) noexcept {
    const auto s = meters_detail::segmentAt(tick, meters);
    const auto barTicks = s.meter->barTicks();
    if (barTicks <= 0 || s.meter->denominator == 0)
        return {};
    const auto into = tick - s.start;
    auto bar = s.barsBefore + into / barTicks;
    auto rem = into % barTicks;
    if (rem < 0) {
        --bar;
        rem += barTicks;
    }
    const auto beatTicks = textproj::kWhole / static_cast<std::int64_t>(s.meter->denominator);
    const auto sixteenthTicks = textproj::kWhole / 16;
    const auto beat = beatTicks > 0 ? rem / beatTicks : 0;
    const auto within = beatTicks > 0 ? rem % beatTicks : rem;
    return {bar + 1, beat + 1, sixteenthTicks > 0 ? within / sixteenthTicks + 1 : 1};
}
// A length from `start`, as Live's Status Bar shows one: whole bars, beats and sixteenths of the
// meter in force at `start`, each from 0.
inline BarPosition barSpan(std::int64_t start, std::int64_t length, const std::vector<textproj::Meter> &meters) noexcept {
    const auto &m = *meters_detail::segmentAt(start, meters).meter;
    const auto barTicks = m.barTicks();
    if (barTicks <= 0 || m.denominator == 0 || length < 0)
        return {0, 0, 0};
    const auto beatTicks = textproj::kWhole / static_cast<std::int64_t>(m.denominator);
    const auto sixteenthTicks = textproj::kWhole / 16;
    const auto rem = length % barTicks;
    return {length / barTicks, beatTicks > 0 ? rem / beatTicks : 0,
            beatTicks > 0 && sixteenthTicks > 0 ? rem % beatTicks / sixteenthTicks : 0};
}
// The meter in force at `tick`.
inline const textproj::Meter &meterAt(std::int64_t tick, const std::vector<textproj::Meter> &meters) noexcept {
    return *meters_detail::segmentAt(tick, meters).meter;
}
// Every bar line from `from` up to `to`, with its number from 1: `visit(tick, bar)`. Bars that
// start before `from` are skipped; a signature change starts a new bar where it falls.
template <class Visit>
void forEachBar(std::int64_t from, std::int64_t to, const std::vector<textproj::Meter> &meters, Visit &&visit) {
    const auto first = meters_detail::segmentAt(std::max<std::int64_t>(0, from), meters);
    std::int64_t start = first.start, bars = first.barsBefore;
    const textproj::Meter *meter = first.meter;
    std::size_t next = 0;
    while (next < meters.size() && meters[next].start_ticks <= start)
        ++next;
    while (start <= to) {
        const auto barTicks = meter->barTicks();
        if (barTicks <= 0)
            return;
        const auto end = next < meters.size() ? meters[next].start_ticks : to + barTicks;
        for (auto t = start; t < end && t <= to; t += barTicks, ++bars)
            if (t >= from)
                visit(t, bars + 1);
        if (next >= meters.size())
            return;
        // The change starts a new bar; the short bar before it was counted by the loop.
        start = meters[next].start_ticks;
        meter = &meters[next];
        ++next;
    }
}
// Live's fragmentary bars (Live 12 manual 6.5): a change placed before the previous bar is complete
// cuts that bar short, "represented in the scrub area by a crosshatched region". `visit(start, end)`
// for each, from the bar line before the change to the change, in order.
template <class Visit>
void forEachFragmentaryBar(const std::vector<textproj::Meter> &meters, Visit &&visit) {
    for (std::size_t i = 1; i < meters.size(); ++i) {
        const auto &before = meters[i - 1];
        const auto barTicks = before.barTicks();
        if (barTicks <= 0 || meters[i].start_ticks <= before.start_ticks)
            continue;
        if (const auto into = (meters[i].start_ticks - before.start_ticks) % barTicks; into != 0)
            visit(meters[i].start_ticks - into, meters[i].start_ticks);
    }
}
// The fragmentary bar holding `tick`, as {start, end}; {-1, -1} when it falls in none.
inline std::pair<std::int64_t, std::int64_t> fragmentAt(std::int64_t tick, const std::vector<textproj::Meter> &meters) {
    std::pair<std::int64_t, std::int64_t> found{-1, -1};
    forEachFragmentaryBar(meters, [&](std::int64_t start, std::int64_t end) {
        if (tick >= start && tick < end)
            found = {start, end};
    });
    return found;
}
} // namespace adi::ui
