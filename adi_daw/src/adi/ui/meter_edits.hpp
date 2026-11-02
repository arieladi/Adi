// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/textproj.hpp"
#include "adi/ui/track_edits.hpp"
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
// Live's time signature markers (Live 12 manual 6.5): "To add a marker at the current insert marker
// position, use the Insert Time Signature Change command"; "You can edit the time signature values
// for selected markers via the Edit Time Signature command ... CtrlR (Win) / CmdR (Mac). To delete a
// time signature marker, use the Delete key". Each is one batch, so one undo step, from the
// snapshot the command lands on (ADR-0201), through `project.insertTimeSignature` and
// `project.removeTimeSignature`.
//
// The markers are the snapshot's meters after the first: the first, at the start, is the control
// bar's, and the snapshot gives an implicit 4/4 there when the project stores none, so its row
// cannot be told from the snapshot. The start's signature is left to the control bar's fields,
// which wait.
struct MeterValue {
    int numerator = 4, denominator = 4;
    bool operator==(const MeterValue &) const = default;
};
// "Any time signature with a one- or two-digit numerator and a denominator of 1, 2, 4, 8 or 16 can
// be used as a value for a marker. The numbers must be separated by a delimiter such as a slash,
// comma, period, or any number of spaces."
inline std::optional<MeterValue> parseMeter(std::string_view text) {
    std::size_t i = 0;
    const auto skipSpaces = [&] {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
    };
    const auto number = [&](std::size_t most) -> std::optional<int> {
        const auto from = i;
        int value = 0;
        while (i < text.size() && i - from < most + 1 && std::isdigit(static_cast<unsigned char>(text[i])))
            value = value * 10 + (text[i++] - '0');
        if (i == from || i - from > most)
            return std::nullopt;
        return value;
    };
    skipSpaces();
    const auto numerator = number(2);
    if (!numerator || *numerator == 0)
        return std::nullopt;
    const auto beforeDelimiter = i;
    skipSpaces();
    if (i < text.size() && (text[i] == '/' || text[i] == ',' || text[i] == '.')) {
        ++i;
        skipSpaces();
    } else if (i == beforeDelimiter) {
        return std::nullopt; // no delimiter at all
    }
    const auto denominator = number(2);
    skipSpaces();
    if (!denominator || i != text.size())
        return std::nullopt;
    switch (*denominator) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
        return MeterValue{*numerator, *denominator};
    default:
        return std::nullopt;
    }
}
inline std::string meterText(const textproj::Meter &m) {
    return std::to_string(m.numerator) + "/" + std::to_string(m.denominator);
}
// The marker at `pos`: its index among the meters, or 0 when there is none there (the start's is
// the control bar's, never a marker).
inline std::size_t markerAt(const std::vector<textproj::Meter> &meters, std::int64_t pos) noexcept {
    for (std::size_t i = 1; i < meters.size(); ++i)
        if (meters[i].start_ticks == pos)
            return i;
    return 0;
}
struct MeterEdit {
    std::vector<OpRequest> ops;
    std::string refusal;
};
namespace meter_edits_detail {
inline constexpr const char *kTypeOne = "Type a time signature such as 3/4: one or two digits over 1, 2, 4, 8 or 16.";
inline OpRequest insert(std::int64_t pos, const MeterValue &v, const char *label) {
    return track_edits_detail::op("project.insertTimeSignature",
                                  {{"pos", pos}, {"num", v.numerator}, {"den", v.denominator}}, label);
}
inline OpRequest remove(std::int64_t pos, const char *label) {
    return track_edits_detail::op("project.removeTimeSignature", {{"pos", pos}}, label);
}
} // namespace meter_edits_detail
// Edit Time Signature: the marker at `pos` takes the value typed. The same value is no edit.
inline MeterEdit editMeter(const std::vector<textproj::Meter> &meters, std::int64_t pos, std::string_view text) {
    MeterEdit edit;
    const auto at = markerAt(meters, pos);
    if (at == 0) {
        edit.refusal = "Select a time signature marker to edit.";
        return edit;
    }
    const auto value = parseMeter(text);
    if (!value) {
        edit.refusal = meter_edits_detail::kTypeOne;
        return edit;
    }
    if (*value == MeterValue{meters[at].numerator, meters[at].denominator})
        return edit;
    edit.ops.push_back(meter_edits_detail::remove(pos, "Edit Time Signature"));
    edit.ops.push_back(meter_edits_detail::insert(pos, *value, "Edit Time Signature"));
    return edit;
}
// Insert Time Signature Change at `pos` (the insert marker), with the value typed. Where a marker
// already stands, it takes the value, as an edit.
inline MeterEdit insertMeter(const std::vector<textproj::Meter> &meters, std::int64_t pos, std::string_view text) {
    if (markerAt(meters, pos) != 0)
        return editMeter(meters, pos, text);
    MeterEdit edit;
    if (pos <= 0) {
        edit.refusal = "The time signature at the start is the control bar's; a change goes after it.";
        return edit;
    }
    const auto value = parseMeter(text);
    if (!value) {
        edit.refusal = meter_edits_detail::kTypeOne;
        return edit;
    }
    edit.ops.push_back(meter_edits_detail::insert(pos, *value, "Insert Time Signature Change"));
    return edit;
}
// A marker moved, its value kept: "Time signature markers can be moved with the mouse or left and
// right arrow keys" (manual 6.5). Never onto the start, which is the control bar's, nor onto another
// marker. In place, no edit.
inline MeterEdit moveMeter(const std::vector<textproj::Meter> &meters, std::int64_t from, std::int64_t to) {
    MeterEdit edit;
    const auto at = markerAt(meters, from);
    if (at == 0) {
        edit.refusal = "Select a time signature marker to move.";
        return edit;
    }
    if (to == from)
        return edit;
    if (to <= 0) {
        edit.refusal = "A time signature change stays after the start; the start's is the control bar's.";
        return edit;
    }
    if (markerAt(meters, to) != 0) {
        edit.refusal = "Another time signature change stands there.";
        return edit;
    }
    const MeterValue value{meters[at].numerator, meters[at].denominator};
    edit.ops.push_back(meter_edits_detail::remove(from, "Move Time Signature Change"));
    edit.ops.push_back(meter_edits_detail::insert(to, value, "Move Time Signature Change"));
    return edit;
}
// Delete on a selected marker.
inline MeterEdit deleteMeter(const std::vector<textproj::Meter> &meters, std::int64_t pos) {
    MeterEdit edit;
    if (markerAt(meters, pos) == 0)
        edit.refusal = "Select a time signature marker to delete.";
    else
        edit.ops.push_back(meter_edits_detail::remove(pos, "Delete Time Signature Change"));
    return edit;
}
} // namespace adi::ui
