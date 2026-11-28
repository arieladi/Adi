// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/session_view_commands.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace adi::ui::session_view {
namespace {

OpRequest op(const char* type, Payload payload, const char* label) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(payload);
    r.label = label;
    return r;
}

bool holdsSlots(const Model& m, std::size_t c) noexcept { return m.column(c).kind != ColumnKind::Return; }

/// The slot-holding columns in display order.
std::vector<std::size_t> slotColumns(const Model& m) {
    std::vector<std::size_t> out;
    for (std::size_t c = 0; c < m.columnCount(); ++c)
        if (holdsSlots(m, c))
            out.push_back(c);
    return out;
}

Selection slotAt(const Model& m, std::size_t c, std::size_t r) {
    Selection s;
    s.focus = Focus::Slot;
    s.track = s.anchorTrack = m.column(c).track;
    s.scene = s.anchorScene = m.scene(r).id;
    return s;
}

Selection sceneAt(const Model& m, std::size_t r) {
    Selection s;
    s.focus = Focus::Scene;
    s.scene = s.anchorScene = m.scene(r).id;
    return s;
}

Selection trackAt(const Model& m, std::size_t c) {
    Selection s;
    s.focus = Focus::Track;
    s.track = s.anchorTrack = m.column(c).track;
    return s;
}

bool needsIncarnation(const Context& ctx, Outcome& out) {
    if (ctx.incarnation != 0)
        return true;
    out.refusal = "Nothing is ready to launch yet.";
    return false;
}

Payload stamped(Payload payload, const Context& ctx) {
    payload["incarnation"] = ctx.incarnation;
    return payload;
}

void launchClip(Outcome& out, const Model& m, std::size_t c, std::size_t r, const Context& ctx, bool release) {
    auto payload = stamped({{"track", m.column(c).track}, {"scene", m.scene(r).id}, {"clip", m.slot(c, r).clip}}, ctx);
    if (release)
        payload["release"] = true;
    else
        payload["velocity"] = static_cast<double>(ctx.velocity);
    out.launches.push_back(op("session.launchClip", std::move(payload), release ? "Release Clip" : "Launch Clip"));
}

void stopTrack(Outcome& out, const Model& m, std::size_t c, const Context& ctx) {
    out.launches.push_back(op("session.stopTrack", stamped({{"track", m.column(c).track}}, ctx), "Stop Track"));
}

/// The clip columns a column stands for: itself, or what a group holds.
std::vector<std::size_t> clipColumnsOf(const Model& m, std::size_t c) {
    if (m.column(c).kind == ColumnKind::Clips)
        return {c};
    const auto held = m.heldColumns(c);
    return {held.begin(), held.end()};
}

/// The clips inside the slot selection, group slots standing for the clips they refer to (7.1).
std::vector<std::pair<std::size_t, std::size_t>> selectedClipSlots(const Model& m) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    const auto range = m.selection().focus == Focus::Slot ? m.selectedRange() : std::nullopt;
    if (!range)
        return out;
    std::set<std::pair<std::size_t, std::size_t>> seen;
    for (std::size_t c = range->c0; c <= range->c1; ++c)
        for (std::size_t r = range->r0; r <= range->r1; ++r)
            for (const auto k : clipColumnsOf(m, c))
                if (m.slot(k, r).clip != 0 && seen.insert({k, r}).second)
                    out.emplace_back(k, r);
    return out;
}

std::optional<std::pair<std::size_t, std::size_t>> focusCell(const Model& m) {
    const auto& s = m.selection();
    const auto c = m.columnOf(s.track);
    const auto r = m.rowOf(s.scene);
    if (s.focus != Focus::Slot || !c || !r)
        return std::nullopt;
    return std::pair{*c, *r};
}

/// Moves the focus by (dc, dr) over the slot columns, the scene column past the last of them.
Outcome move(const Model& m, int dc, std::ptrdiff_t dr, bool extend) {
    Outcome out;
    const auto& s = m.selection();
    const auto columns = slotColumns(m);
    const auto scenes = m.sceneCount();
    const auto clampRow = [&](std::size_t r) {
        const auto next = static_cast<std::ptrdiff_t>(r) + dr;
        return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(next, 0, static_cast<std::ptrdiff_t>(scenes) - 1));
    };
    switch (s.focus) {
    case Focus::None:
        if (!columns.empty() && scenes != 0)
            out.selection = slotAt(m, columns.front(), 0);
        else if (scenes != 0)
            out.selection = sceneAt(m, 0);
        return out;
    case Focus::Slot: {
        const auto [c, r] = *focusCell(m);
        const auto place = static_cast<std::ptrdiff_t>(std::find(columns.begin(), columns.end(), c) - columns.begin());
        auto next = s;
        if (dc != 0) {
            const auto to = place + dc;
            if (to < 0)
                return out;
            if (to >= static_cast<std::ptrdiff_t>(columns.size())) {
                if (extend)
                    return out; // a slot range does not reach into the scene column
                out.selection = sceneAt(m, r);
                return out;
            }
            next.track = m.column(columns[static_cast<std::size_t>(to)]).track;
        }
        next.scene = m.scene(clampRow(r)).id;
        if (!extend) {
            next.anchorTrack = next.track;
            next.anchorScene = next.scene;
        }
        out.selection = next;
        return out;
    }
    case Focus::Scene: {
        const auto r = *m.rowOf(s.scene);
        if (dc < 0 && !extend && !columns.empty()) {
            out.selection = slotAt(m, columns.back(), r);
            return out;
        }
        if (dc != 0)
            return out; // past the scenes: Live's tempo and signature fields, not modelled here
        auto next = s;
        next.scene = m.scene(clampRow(r)).id;
        if (!extend)
            next.anchorScene = next.scene;
        out.selection = next;
        return out;
    }
    case Focus::Track: {
        const auto c = *m.columnOf(s.track);
        if (dr > 0 && !extend && holdsSlots(m, c) && scenes != 0) {
            out.selection = slotAt(m, c, 0);
            return out;
        }
        if (dc == 0)
            return out;
        const auto to = static_cast<std::ptrdiff_t>(c) + dc;
        if (to < 0 || to >= static_cast<std::ptrdiff_t>(m.columnCount()))
            return out;
        auto next = s;
        next.track = m.column(static_cast<std::size_t>(to)).track;
        if (!extend)
            next.anchorTrack = next.track;
        out.selection = next;
        return out;
    }
    }
    return out;
}

Outcome edge(const Model& m, bool last) {
    Outcome out;
    const auto columns = slotColumns(m);
    const auto& s = m.selection();
    std::optional<std::size_t> row;
    if (s.focus == Focus::Slot || s.focus == Focus::Scene)
        row = m.rowOf(s.scene);
    if (!row || columns.empty())
        return out;
    if (last && s.focus == Focus::Slot && *m.columnOf(s.track) == columns.back()) {
        out.selection = sceneAt(m, *row); // End from the last track: the current scene (41.15)
        return out;
    }
    out.selection = slotAt(m, last ? columns.back() : columns.front(), *row);
    return out;
}

Outcome selectAll(const Model& m) {
    Outcome out;
    const auto& s = m.selection();
    const auto columns = slotColumns(m);
    if (s.focus == Focus::Scene && m.sceneCount() != 0) {
        Selection all = sceneAt(m, m.sceneCount() - 1);
        all.anchorScene = m.scene(0).id;
        out.selection = all;
    } else if (s.focus == Focus::Track && m.columnCount() != 0) {
        Selection all = trackAt(m, m.columnCount() - 1);
        all.anchorTrack = m.column(0).track;
        out.selection = all;
    } else if (!columns.empty() && m.sceneCount() != 0) {
        Selection all = slotAt(m, columns.back(), m.sceneCount() - 1);
        all.anchorTrack = m.column(columns.front()).track;
        all.anchorScene = m.scene(0).id;
        out.selection = all;
    }
    return out;
}

/// The scene identities in display order.
std::vector<std::int64_t> sceneOrder(const Model& m) {
    std::vector<std::int64_t> ids;
    for (std::size_t r = 0; r < m.sceneCount(); ++r)
        ids.push_back(m.scene(r).id);
    return ids;
}

Outcome insertScene(const Model& m) {
    Outcome out;
    std::int64_t id = 1, ord = 0;
    for (std::size_t r = 0; r < m.sceneCount(); ++r) {
        id = std::max(id, m.scene(r).id + 1);
        ord = std::max(ord, m.scene(r).order + 1);
    }
    // Below the current selection (7.4.3); with no scene in it, at the end.
    const auto range = m.selectedRange();
    const auto& s = m.selection();
    const auto below = range && (s.focus == Focus::Slot || s.focus == Focus::Scene)
                           ? std::optional<std::size_t>(range->r1) : std::nullopt;
    out.edits.push_back(op("scene.create", {{"id", id}, {"ord", ord}}, "Insert Scene"));
    auto order = sceneOrder(m);
    const auto at = below ? *below + 1 : order.size();
    if (at < order.size()) {
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(at), id);
        out.edits.push_back(op("scene.reorder", {{"scenes", order}}, "Insert Scene"));
    }
    Selection made;
    made.focus = Focus::Scene;
    made.scene = made.anchorScene = id;
    out.selection = made;
    return out;
}

Outcome deleteSelection(const Model& m) {
    Outcome out;
    const auto& s = m.selection();
    if (s.focus == Focus::Track || s.focus == Focus::None) {
        out.passToShell = s.focus == Focus::Track;
        return out;
    }
    if (s.focus == Focus::Slot) {
        for (const auto& [c, r] : selectedClipSlots(m))
            out.edits.push_back(op("session.clearSlot", {{"track", m.column(c).track}, {"scene", m.scene(r).id}},
                                   "Delete Clips"));
        return out;
    }
    const auto range = *m.selectedRange();
    const auto removed = range.r1 - range.r0 + 1;
    if (keepOneScene && removed >= m.sceneCount()) {
        out.refusal = "A Session keeps at least one scene.";
        return out;
    }
    for (auto r = range.r0; r <= range.r1; ++r)
        out.edits.push_back(op("scene.delete", {{"id", m.scene(r).id}}, removed == 1 ? "Delete Scene" : "Delete Scenes"));
    // The selection stays at the place the scenes left: the next scene, or the new last one.
    if (range.r1 + 1 < m.sceneCount())
        out.selection = sceneAt(m, range.r1 + 1);
    else if (range.r0 > 0)
        out.selection = sceneAt(m, range.r0 - 1);
    else
        out.selection = Selection{};
    return out;
}

Outcome stopButtons(const Model& m) {
    Outcome out;
    const auto range = m.selection().focus == Focus::Slot ? m.selectedRange() : std::nullopt;
    if (!range)
        return out;
    const auto focus = focusCell(m);
    std::optional<bool> on;
    if (focus && m.column(focus->first).kind == ColumnKind::Clips)
        on = !m.slot(focus->first, focus->second).hasStop;
    for (std::size_t c = range->c0; c <= range->c1; ++c) {
        if (m.column(c).kind != ColumnKind::Clips)
            continue;
        for (std::size_t r = range->r0; r <= range->r1; ++r) {
            if (!on)
                on = !m.slot(c, r).hasStop;
            if (m.slot(c, r).hasStop == *on)
                continue;
            out.edits.push_back(op("session.setStopButton",
                                   {{"track", m.column(c).track}, {"scene", m.scene(r).id}, {"on", *on}},
                                   *on ? "Add Stop Button" : "Remove Stop Button"));
        }
    }
    return out;
}

Outcome deactivate(const Model& m) {
    Outcome out;
    if (m.selection().focus == Focus::Track) {
        out.passToShell = true; // Live's 0 on a track title: the Track Activator (7.2)
        return out;
    }
    const auto clips = selectedClipSlots(m);
    if (clips.empty())
        return out;
    const auto focus = focusCell(m);
    const auto& first = focus && m.slot(focus->first, focus->second).clip != 0 ? m.slot(focus->first, focus->second)
                                                                                : m.slot(clips.front().first, clips.front().second);
    const bool mute = !first.deactivated;
    for (const auto& [c, r] : clips)
        if (m.slot(c, r).deactivated != mute)
            out.edits.push_back(op("clip.setMute", {{"id", m.slot(c, r).clip}, {"muted", mute}},
                                   mute ? "Deactivate Clips" : "Activate Clips"));
    return out;
}

Outcome moveScenes(const Model& m, bool up) {
    Outcome out;
    if (m.selection().focus != Focus::Scene)
        return out;
    const auto range = *m.selectedRange();
    if ((up && range.r0 == 0) || (!up && range.r1 + 1 >= m.sceneCount()))
        return out;
    auto order = sceneOrder(m);
    const auto first = order.begin() + static_cast<std::ptrdiff_t>(range.r0);
    const auto last = order.begin() + static_cast<std::ptrdiff_t>(range.r1) + 1;
    if (up)
        std::rotate(first - 1, first, last);
    else
        std::rotate(first, last, last + 1);
    out.edits.push_back(op("scene.reorder", {{"scenes", order}}, "Move Scenes"));
    return out;
}

Outcome renameSelection(const Model& m) {
    Outcome out;
    const auto& s = m.selection();
    if (s.focus == Focus::Scene)
        out.rename = RenameTarget{Focus::Scene, s.scene};
    else if (s.focus == Focus::Track)
        out.rename = RenameTarget{Focus::Track, s.track};
    else if (const auto cell = focusCell(m); cell && m.slot(cell->first, cell->second).clip != 0)
        out.rename = RenameTarget{Focus::Slot, m.slot(cell->first, cell->second).clip};
    return out;
}

Outcome cancelScene(const Model& m, const Context& ctx) {
    Outcome out;
    auto scene = m.pendingScene();
    if (!scene && m.selection().focus == Focus::Scene)
        scene = m.selection().scene;
    if (!scene || !needsIncarnation(ctx, out))
        return out;
    out.launches.push_back(op("session.cancelSceneLaunch", stamped({{"scene", *scene}}, ctx), "Cancel Scene Launch"));
    return out;
}

} // namespace

std::optional<Command> commandForKey(const KeyChord& k, MainView view, const KeyOptions& options) noexcept {
    const bool plain = !k.command && !k.shift && !k.alt;
    if ((k.key == Key::Tab && plain && !options.useTabToMoveFocus) || (k.key == Key::F3 && plain && f3TogglesSessionView))
        return Command::ToggleView;
    if (view != MainView::Session)
        return std::nullopt;
    const bool onlyCommand = k.command && !k.shift && !k.alt;
    const bool onlyShift = k.shift && !k.command && !k.alt;
    switch (k.key) {
    case Key::Enter:
        if (plain)
            return Command::LaunchSelected;
        if (onlyCommand)
            return Command::StopSelectedTracks;
        return std::nullopt;
    case Key::Up:
    case Key::Down: {
        const bool up = k.key == Key::Up;
        if (plain)
            return up ? Command::SelectUp : Command::SelectDown;
        if (onlyShift)
            return up ? Command::ExtendUp : Command::ExtendDown;
        if (onlyCommand)
            return up ? Command::MoveScenesUp : Command::MoveScenesDown;
        return std::nullopt;
    }
    case Key::Left:
    case Key::Right: {
        const bool left = k.key == Key::Left;
        if (plain)
            return left ? Command::SelectLeft : Command::SelectRight;
        if (onlyShift)
            return left ? Command::ExtendLeft : Command::ExtendRight;
        return std::nullopt; // Ctrl+Left/Right moves the track: the track commands' (41.15)
    }
    case Key::PageUp: return plain ? std::optional{Command::PageUp} : std::nullopt;
    case Key::PageDown: return plain ? std::optional{Command::PageDown} : std::nullopt;
    case Key::Home: return plain ? std::optional{Command::FirstTrack} : std::nullopt;
    case Key::End: return plain ? std::optional{Command::LastTrack} : std::nullopt;
    case Key::Escape: return plain ? std::optional{Command::TrackTitle} : std::nullopt;
    case Key::Delete:
    case Key::Backspace: return plain ? std::optional{Command::Delete} : std::nullopt;
    case Key::Character:
        if (plain && k.character == U'0')
            return Command::Deactivate;
        if (onlyCommand) {
            switch (k.character) {
            case U'a': return Command::SelectAll;
            case U'i': return Command::InsertScene;
            case U'e': return Command::AddRemoveStopButton;
            case U'r': return Command::Rename;
            default: return std::nullopt;
            }
        }
        if (k.command && k.shift && !k.alt && k.character == U'i')
            return Command::CaptureAndInsertScene;
        return std::nullopt;
    case Key::Tab:
    case Key::F3: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Command> commandForKeyRelease(const KeyChord& k, MainView view) noexcept {
    if (view == MainView::Session && k.key == Key::Enter && !k.command && !k.shift && !k.alt)
        return Command::ReleaseSelected;
    return std::nullopt;
}

Outcome launchSlot(const Model& m, std::size_t c, std::size_t r, const Context& ctx) {
    Outcome out;
    if (c >= m.columnCount() || r >= m.sceneCount() || !holdsSlots(m, c))
        return out;
    const auto& col = m.column(c);
    const auto& slot = m.slot(c, r);
    if (col.kind == ColumnKind::Clips && slot.clip == 0 && col.armed) {
        out.recordInto = engine::launch::SlotKey{col.track, m.scene(r).id}; // Session Record (19)
        return out;
    }
    if (!needsIncarnation(ctx, out))
        return out;
    if (col.kind == ColumnKind::Clips) {
        if (slot.clip != 0)
            launchClip(out, m, c, r, ctx, false);
        else if (slot.hasStop)
            stopTrack(out, m, c, ctx); // an empty slot's square is its track's Clip Stop button
    } else {
        // A group slot launches every clip it refers to (7.1); without any it is a Stop button.
        for (const auto k : m.heldColumns(c)) {
            const auto& held = m.slot(k, r);
            if (held.clip != 0)
                launchClip(out, m, k, r, ctx, false);
            else if ((!slot.groupOccupied || groupLaunchStopsEmptyHeldTracks) && held.hasStop)
                stopTrack(out, m, k, ctx);
        }
    }
    if (ctx.selectOnLaunch && !out.launches.empty())
        out.selection = slotAt(m, c, r);
    return out;
}

Outcome releaseSlot(const Model& m, std::size_t c, std::size_t r, const Context& ctx) {
    Outcome out;
    if (c >= m.columnCount() || r >= m.sceneCount() || ctx.incarnation == 0)
        return out;
    for (const auto k : clipColumnsOf(m, c))
        if (m.slot(k, r).clip != 0)
            launchClip(out, m, k, r, ctx, true);
    return out;
}

Outcome launchScene(const Model& m, std::size_t r, const Context& ctx) {
    Outcome out;
    if (r >= m.sceneCount() || !needsIncarnation(ctx, out))
        return out;
    out.launches.push_back(op("session.launchScene",
                              stamped({{"scene", m.scene(r).id}, {"velocity", static_cast<double>(ctx.velocity)}}, ctx),
                              "Launch Scene"));
    if (ctx.selectNextSceneOnLaunch && r + 1 < m.sceneCount())
        out.selection = sceneAt(m, r + 1);
    else if (ctx.selectOnLaunch)
        out.selection = sceneAt(m, r);
    return out;
}

Outcome stopColumn(const Model& m, std::size_t c, const Context& ctx) {
    Outcome out;
    if (c >= m.columnCount() || !holdsSlots(m, c) || !needsIncarnation(ctx, out))
        return out;
    for (const auto k : clipColumnsOf(m, c))
        stopTrack(out, m, k, ctx);
    return out;
}

Outcome run(Command command, const Model& m, const Context& ctx) {
    if (command == Command::ToggleView) {
        Outcome out;
        out.view = toggled(ctx.view);
        return out;
    }
    if (ctx.view != MainView::Session)
        return {};
    const auto& s = m.selection();
    switch (command) {
    case Command::ToggleView: return {};
    case Command::LaunchSelected: {
        if (s.focus == Focus::Scene)
            return launchScene(m, *m.rowOf(s.scene), ctx);
        const auto cell = focusCell(m);
        return cell ? launchSlot(m, cell->first, cell->second, ctx) : Outcome{};
    }
    case Command::ReleaseSelected: {
        const auto cell = focusCell(m);
        return cell ? releaseSlot(m, cell->first, cell->second, ctx) : Outcome{};
    }
    case Command::StopSelectedTracks: {
        Outcome out;
        const auto range = s.focus == Focus::Slot ? m.selectedRange() : std::nullopt;
        if (!range || !needsIncarnation(ctx, out))
            return out;
        std::set<std::size_t> stopped;
        for (std::size_t c = range->c0; c <= range->c1; ++c)
            for (const auto k : clipColumnsOf(m, c))
                if (stopped.insert(k).second)
                    stopTrack(out, m, k, ctx);
        return out;
    }
    case Command::StopAll: {
        Outcome out;
        if (needsIncarnation(ctx, out))
            out.launches.push_back(op("session.stopAll", stamped(Payload::object(), ctx), "Stop All Clips"));
        return out;
    }
    case Command::BackToArrangement: {
        Outcome out;
        if (needsIncarnation(ctx, out))
            out.launches.push_back(op("session.backToArrangement", stamped(Payload::object(), ctx), "Back to Arrangement"));
        return out;
    }
    case Command::CancelSceneLaunch: return cancelScene(m, ctx);
    case Command::SelectUp: return move(m, 0, -1, false);
    case Command::SelectDown: return move(m, 0, 1, false);
    case Command::SelectLeft: return move(m, -1, 0, false);
    case Command::SelectRight: return move(m, 1, 0, false);
    case Command::ExtendUp: return move(m, 0, -1, true);
    case Command::ExtendDown: return move(m, 0, 1, true);
    case Command::ExtendLeft: return move(m, -1, 0, true);
    case Command::ExtendRight: return move(m, 1, 0, true);
    case Command::PageUp: return move(m, 0, -static_cast<std::ptrdiff_t>(scenesPerPage), false);
    case Command::PageDown: return move(m, 0, static_cast<std::ptrdiff_t>(scenesPerPage), false);
    case Command::FirstTrack: return edge(m, false);
    case Command::LastTrack: return edge(m, true);
    case Command::TrackTitle: {
        Outcome out;
        if (s.focus == Focus::Slot)
            out.selection = trackAt(m, *m.columnOf(s.track));
        return out;
    }
    case Command::SelectAll: return selectAll(m);
    case Command::InsertScene: return insertScene(m);
    case Command::CaptureAndInsertScene: {
        Outcome out;
        const auto range = m.selectedRange();
        CaptureRequest request;
        if (range && (s.focus == Focus::Slot || s.focus == Focus::Scene))
            request.belowScene = m.scene(range->r1).id;
        out.capture = request;
        return out;
    }
    case Command::AddRemoveStopButton: return stopButtons(m);
    case Command::Rename: return renameSelection(m);
    case Command::Delete: return deleteSelection(m);
    case Command::Deactivate: return deactivate(m);
    case Command::MoveScenesUp: return moveScenes(m, true);
    case Command::MoveScenesDown: return moveScenes(m, false);
    }
    return {};
}

Outcome renameTo(const Model& m, const RenameTarget& target, std::string_view name) {
    Outcome out;
    const std::string text(name);
    switch (target.what) {
    case Focus::Scene:
        if (const auto r = m.rowOf(target.id); r && m.scene(*r).name != text)
            out.edits.push_back(op("scene.rename", {{"id", target.id}, {"name", text}}, "Rename Scene"));
        break;
    case Focus::Track:
        if (const auto c = m.columnOf(target.id); c && m.column(*c).name != text)
            out.edits.push_back(op("track.rename", {{"id", target.id}, {"name", text}}, "Rename Track"));
        break;
    case Focus::Slot:
        for (std::size_t c = 0; c < m.columnCount(); ++c)
            for (std::size_t r = 0; r < m.sceneCount(); ++r)
                if (m.slot(c, r).clip == target.id && m.column(c).kind == ColumnKind::Clips && m.slot(c, r).name != text)
                    out.edits.push_back(op("clip.setName", {{"id", target.id}, {"name", text}}, "Rename Clip"));
        break;
    case Focus::None:
        break;
    }
    return out;
}

std::optional<RenameTarget> nextRenameTarget(const Model& m, const RenameTarget& target, bool backward) {
    const auto step = [backward](std::size_t at, std::size_t count) -> std::optional<std::size_t> {
        if (backward)
            return at == 0 ? std::nullopt : std::optional{at - 1};
        return at + 1 < count ? std::optional{at + 1} : std::nullopt;
    };
    if (target.what == Focus::Scene)
        if (const auto r = m.rowOf(target.id))
            if (const auto next = step(*r, m.sceneCount()))
                return RenameTarget{Focus::Scene, m.scene(*next).id};
    if (target.what == Focus::Track)
        if (const auto c = m.columnOf(target.id))
            if (const auto next = step(*c, m.columnCount()))
                return RenameTarget{Focus::Track, m.column(*next).track};
    return std::nullopt;
}

} // namespace adi::ui::session_view
