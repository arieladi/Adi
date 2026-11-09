// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/snapshot.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace adi::ui {
// Live's locators (Live 12 manual 6.4; MR 3.3; N25, #385): position markers in ticks, kept in the
// project's `markers` (kind "position", time base 0) through `marker.create`, `.delete`, `.move`,
// `.rename`, `.setNote` and `.setPrompt`. Each edit is one batch, so one undo step, from the
// snapshot the command lands on (ADR-0201). A marker's prompt (ADI's, MR 3.3) is stored literally and
// never run; the window does not read it.
struct Locator {
    std::int64_t id = 0, pos = 0;
    std::string name, note;
};
// The snapshot's locators, in time order (by id where two share a place). Cycle markers, chapter and
// sync markers, and markers in seconds are not locators and are left out.
inline std::vector<Locator> locatorsOf(const engine::Snapshot &snapshot) {
    std::vector<Locator> out;
    for (const auto &m : snapshot.markers)
        if (m && m->kind == "position" && m->timeBase == 0 && m->posTicks)
            out.push_back({m->id, *m->posTicks, m->name, m->note});
    std::sort(out.begin(), out.end(), [](const Locator &a, const Locator &b) { return a.pos != b.pos ? a.pos < b.pos : a.id < b.id; });
    return out;
}
inline const Locator *locatorById(const std::vector<Locator> &locators, std::int64_t id) noexcept {
    for (const auto &l : locators)
        if (l.id == id)
            return &l;
    return nullptr;
}
inline const Locator *locatorAt(const std::vector<Locator> &locators, std::int64_t pos) noexcept {
    for (const auto &l : locators)
        if (l.pos == pos)
            return &l;
    return nullptr;
}
// The next locator after `from`, or the one before it: its place, or none past the last or the first.
inline std::optional<std::int64_t> nextLocator(const std::vector<Locator> &locators, std::int64_t from, bool forward) {
    if (forward) {
        for (const auto &l : locators)
            if (l.pos > from)
                return l.pos;
    } else {
        for (auto it = locators.rbegin(); it != locators.rend(); ++it)
            if (it->pos < from)
                return it->pos;
    }
    return std::nullopt;
}
struct LocatorEdit {
    std::vector<OpRequest> ops;
    std::string refusal;
    std::int64_t id = 0; // the locator made or edited
};
// Add Locator at `pos` (manual 6.4): a new marker, one past the highest marker id of any kind, named
// by its number among the locators, as Live numbers new ones. Where a locator already stands, none.
inline LocatorEdit addLocator(const engine::Snapshot &snapshot, std::int64_t pos) {
    LocatorEdit edit;
    const auto locators = locatorsOf(snapshot);
    if (pos < 0) {
        edit.refusal = "A locator goes at or after the arrangement's start.";
        return edit;
    }
    if (const auto *there = locatorAt(locators, pos)) {
        edit.refusal = "A locator already stands there: " + (there->name.empty() ? std::string("unnamed") : there->name) + ".";
        return edit;
    }
    std::int64_t id = 1;
    for (const auto &m : snapshot.markers)
        if (m)
            id = std::max(id, m->id + 1);
    edit.id = id;
    edit.ops.push_back(track_edits_detail::op(
        "marker.create",
        {{"id", id}, {"pos", pos}, {"kind", "position"}, {"name", std::to_string(locators.size() + 1)}}, "Add Locator"));
    return edit;
}
namespace locator_edits_detail {
inline LocatorEdit selected(const std::vector<Locator> &locators, std::int64_t id, const char *what) {
    LocatorEdit edit;
    if (locatorById(locators, id) == nullptr)
        edit.refusal = std::string("Select a locator to ") + what + ".";
    edit.id = id;
    return edit;
}
} // namespace locator_edits_detail
inline LocatorEdit deleteLocator(const engine::Snapshot &snapshot, std::int64_t id) {
    auto edit = locator_edits_detail::selected(locatorsOf(snapshot), id, "delete");
    if (edit.refusal.empty())
        edit.ops.push_back(track_edits_detail::op("marker.delete", {{"id", id}}, "Delete Locator"));
    return edit;
}
// A locator moved, by a drag or the arrows; never before the start nor onto another. In place, none.
inline LocatorEdit moveLocator(const engine::Snapshot &snapshot, std::int64_t id, std::int64_t to) {
    const auto locators = locatorsOf(snapshot);
    auto edit = locator_edits_detail::selected(locators, id, "move");
    if (!edit.refusal.empty())
        return edit;
    const auto *l = locatorById(locators, id);
    if (to == l->pos)
        return edit;
    if (to < 0) {
        edit.refusal = "A locator stays at or after the arrangement's start.";
        return edit;
    }
    if (const auto *there = locatorAt(locators, to); there != nullptr && there->id != id) {
        edit.refusal = "Another locator stands there.";
        return edit;
    }
    edit.ops.push_back(track_edits_detail::op("marker.move", {{"id", id}, {"pos", to}}, "Move Locator"));
    return edit;
}
inline LocatorEdit renameLocator(const engine::Snapshot &snapshot, std::int64_t id, std::string_view name) {
    const auto locators = locatorsOf(snapshot);
    auto edit = locator_edits_detail::selected(locators, id, "rename");
    if (edit.refusal.empty() && locatorById(locators, id)->name != name)
        edit.ops.push_back(track_edits_detail::op("marker.rename", {{"id", id}, {"name", std::string(name)}}, "Rename Locator"));
    return edit;
}
// Edit Info Text (manual 6.4): the locator's info text, kept as the marker's note.
inline LocatorEdit setLocatorInfo(const engine::Snapshot &snapshot, std::int64_t id, std::string_view text) {
    const auto locators = locatorsOf(snapshot);
    auto edit = locator_edits_detail::selected(locators, id, "edit its info text");
    if (edit.refusal.empty() && locatorById(locators, id)->note != text)
        edit.ops.push_back(track_edits_detail::op("marker.setNote", {{"id", id}, {"text", std::string(text)}}, "Edit Info Text"));
    return edit;
}
} // namespace adi::ui
