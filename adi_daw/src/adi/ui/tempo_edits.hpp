// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/track_edits.hpp"
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
// The control bar's Tempo field (Live 12 manual 9.1.1): "lets you adjust the playback tempo of your
// Live Set"; it "displays values for both the coarse tempo in BPM and the fine tempo in hundredths of
// a BPM". Typed, it sets the tempo in force at the play location, one undo step, through
// `project.insertTempoEvent` and `project.removeTempoEvent`, from the snapshot the command lands on
// (ADR-0201). Where a tempo change is in force there, that change takes the value (its curve kept):
// ADI's reading until the tempo's automation (manual 26.9.1) is built. Else the start's tempo: the
// snapshot shows an implicit 120 BPM there when the project stores none, so a 120 there may be
// either, and `startStored` says what the caller knows or tries (`tempoSurelyStored`).
inline constexpr double kMinTempo = 20, kMaxTempo = 999;
// A tempo typed: digits, with hundredths at most after a point, from 20 to 999 BPM.
inline std::optional<double> parseTempo(std::string_view text) {
    std::size_t from = 0, to = text.size();
    while (from < to && std::isspace(static_cast<unsigned char>(text[from])))
        ++from;
    while (to > from && std::isspace(static_cast<unsigned char>(text[to - 1])))
        --to;
    double whole = 0, fraction = 0, scale = 1;
    std::size_t i = from, digits = 0, decimals = 0;
    for (; i < to && std::isdigit(static_cast<unsigned char>(text[i])); ++i, ++digits)
        whole = whole * 10 + (text[i] - '0');
    if (i < to && text[i] == '.')
        for (++i; i < to && std::isdigit(static_cast<unsigned char>(text[i])); ++i, ++decimals) {
            scale /= 10;
            fraction += (text[i] - '0') * scale;
        }
    if (digits == 0 || digits > 3 || decimals > 2 || i != to)
        return std::nullopt;
    const double bpm = whole + fraction;
    if (bpm < kMinTempo || bpm > kMaxTempo)
        return std::nullopt;
    return std::round(bpm * 100) / 100;
}
struct TempoEdit {
    std::vector<OpRequest> ops;
    std::string refusal;
};
inline TempoEdit setTempoAt(const std::vector<engine::TempoEvent> &events, std::int64_t at, std::string_view text,
                            bool startStored, const char *label = "Change Tempo") {
    TempoEdit edit;
    const auto bpm = parseTempo(text);
    if (!bpm) {
        edit.refusal = "Type a tempo from 20 to 999 BPM, such as 128 or 97.50.";
        return edit;
    }
    std::size_t in = 0;
    for (std::size_t i = 1; i < events.size(); ++i)
        if (events[i].posTicks <= at)
            in = i;
    const engine::TempoEvent now = events.empty() ? engine::TempoEvent{} : events[in];
    if (std::abs(now.bpm - *bpm) < .005)
        return edit;
    if (in != 0 || startStored)
        edit.ops.push_back(track_edits_detail::op("project.removeTempoEvent", {{"pos", now.posTicks}}, label));
    edit.ops.push_back(track_edits_detail::op("project.insertTempoEvent",
                                              {{"pos", now.posTicks}, {"bpm", *bpm}, {"curve", now.curve}}, label));
    return edit;
}
// The start's tempo is surely stored when it is not the implicit 120 BPM jump.
inline bool tempoSurelyStored(const std::vector<engine::TempoEvent> &events) noexcept {
    return !events.empty() && events[0].posTicks == 0 && (std::abs(events[0].bpm - 120.) > 1e-9 || events[0].curve != 0);
}
} // namespace adi::ui
