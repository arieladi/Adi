// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/ui/session_view_commands.hpp"

#include <algorithm>
#include <set>
#include <tuple>
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
    std::set<std::pair<std::size_t, std::size_t>> seen;
    for (const auto& [c, r] : m.selectedSlots())
        for (const auto k : clipColumnsOf(m, c))
            if (m.slot(k, r).clip != 0 && seen.insert({k, r}).second)
                out.emplace_back(k, r);
    std::sort(out.begin(), out.end());
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
        next.picks.clear(); // an arrow leaves the picks for the rectangle
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
        next.picks.clear(); // an arrow leaves the picks for the rectangle
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
        next.picks.clear(); // an arrow leaves the picks for the rectangle
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

Outcome deleteSelection(const Model& m);

/// The last row of the selection, where a scene goes "below the current selection" (7.4.3).
std::optional<std::size_t> belowSelection(const Model& m) {
    const auto range = m.selectedRange();
    const auto& s = m.selection();
    return range && (s.focus == Focus::Slot || s.focus == Focus::Scene) ? std::optional<std::size_t>(range->r1)
                                                                        : std::nullopt;
}

Payload followPayload(const char* key, std::int64_t id, const engine::FollowSettings& f, bool clip) {
    Payload p{{key, id},
              {"enabled", f.enabled},
              {"duration", f.durationTicks},
              {"actionA", static_cast<std::int64_t>(f.first.action)},
              {"actionB", static_cast<std::int64_t>(f.second.action)},
              {"chanceB", f.secondChancePercent}};
    if (f.first.jumpSceneId)
        p["jumpA"] = *f.first.jumpSceneId;
    if (f.second.jumpSceneId)
        p["jumpB"] = *f.second.jumpSceneId;
    if (clip) {
        p["linked"] = f.linked;
        p["loops"] = f.loopCount;
    }
    return p;
}

/// A copied scene's own settings, where they differ from a new scene's.
void sceneSettings(Outcome& out, const SceneFacts& s, std::int64_t id, const char* label) {
    const engine::launch::Scene dormant{};
    if (s.tempoBpm != dormant.tempoBpm)
        out.edits.push_back(op("scene.setTempo", {{"id", id}, {"bpm", s.tempoBpm}}, label));
    if (s.signatureNumerator != dormant.signatureNumerator || s.signatureDenominator != dormant.signatureDenominator)
        out.edits.push_back(op("scene.setSignature",
                               {{"id", id}, {"numerator", s.signatureNumerator}, {"denominator", s.signatureDenominator}},
                               label));
    if (s.tempoEnabled || s.signatureEnabled)
        out.edits.push_back(op("scene.setEnabled", {{"id", id}, {"tempo", s.tempoEnabled}, {"signature", s.signatureEnabled}},
                               label));
    if (s.followSettings != dormant.follow)
        out.edits.push_back(op("session.setFollowAction", followPayload("scene", id, s.followSettings, false), label));
}

/// New scenes below `below` (none: at the end), one for each of `like`: an empty one for null, a
/// copy of the scene's name, colour, tempo, signature and Follow Action otherwise. The new
/// identities follow the largest, as Insert Scene's do.
std::vector<std::int64_t> createScenes(Outcome& out, const Model& m, std::optional<std::size_t> below,
                                       const std::vector<const SceneFacts*>& like, const char* label) {
    std::int64_t id = 1, ord = 0;
    for (std::size_t r = 0; r < m.sceneCount(); ++r) {
        id = std::max(id, m.scene(r).id + 1);
        ord = std::max(ord, m.scene(r).order + 1);
    }
    std::vector<std::int64_t> ids;
    for (const auto* s : like) {
        Payload create{{"id", id}, {"ord", ord++}};
        if (s != nullptr) {
            if constexpr (sceneCopyKeepsName)
                create["name"] = s->name;
            if (s->color)
                create["color"] = *s->color;
        }
        out.edits.push_back(op("scene.create", std::move(create), label));
        if (s != nullptr)
            sceneSettings(out, *s, id, label);
        ids.push_back(id++);
    }
    auto order = sceneOrder(m);
    const auto at = below ? *below + 1 : order.size();
    if (at < order.size()) {
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(at), ids.begin(), ids.end());
        out.edits.push_back(op("scene.reorder", {{"scenes", order}}, label));
    }
    return ids;
}

Selection sceneRun(std::int64_t first, std::int64_t last) {
    Selection s;
    s.focus = Focus::Scene;
    s.anchorScene = first;
    s.scene = last;
    return s;
}

Outcome insertScene(const Model& m) {
    Outcome out;
    // Below the current selection (7.4.3); with no scene in it, at the end.
    const auto ids = createScenes(out, m, belowSelection(m), {nullptr}, "Insert Scene");
    out.selection = sceneRun(ids.front(), ids.front());
    return out;
}

/// A scene as the clipboard keeps it: its facts and the clip tracks' slots that are not plain.
Clipboard::Scene sceneEntry(const Model& m, std::size_t r) {
    Clipboard::Scene entry{m.scene(r), {}};
    for (std::size_t c = 0; c < m.columnCount(); ++c) {
        const auto& f = m.slot(c, r);
        if (m.column(c).kind == ColumnKind::Clips && (f.clip != 0 || !f.hasStop))
            entry.slots.push_back({m.column(c).track, f.clip, f.midiClip, f.hasStop});
    }
    return entry;
}

/// Inserts copies of `scenes` below `below`, their clips copied into them, the copies selected.
void copyScenes(Outcome& out, const Model& m, std::optional<std::size_t> below,
                const std::vector<Clipboard::Scene>& scenes, const char* label) {
    if (scenes.empty())
        return;
    std::vector<const SceneFacts*> like;
    for (const auto& s : scenes)
        like.push_back(&s.facts);
    const auto ids = createScenes(out, m, below, like, label);
    for (std::size_t i = 0; i < scenes.size(); ++i)
        for (const auto& slot : scenes[i].slots) {
            const auto c = m.columnOf(slot.track);
            if (!c || m.column(*c).kind != ColumnKind::Clips)
                continue; // the track went since the copy
            if (slot.clip != 0) {
                if (m.column(*c).midi == slot.midi)
                    out.copies.push_back({slot.clip, slot.track, ids[i],
                                          slot.hasStop ? std::nullopt : std::optional<bool>(false), out.edits.size()});
            } else if (!slot.hasStop) {
                out.edits.push_back(op("session.setStopButton", {{"track", slot.track}, {"scene", ids[i]}, {"on", false}}, label));
            }
        }
    out.selection = sceneRun(ids.front(), ids.back());
    out.label = label;
}

/// The slot a selection's corner lands on when the grid moves by (dc, dr); none off the grid.
std::optional<std::pair<std::int64_t, std::int64_t>> shifted(const Model& m, std::int64_t track, std::int64_t scene,
                                                              std::ptrdiff_t dc, std::ptrdiff_t dr) {
    const auto c = m.columnOf(track);
    const auto r = m.rowOf(scene);
    if (!c || !r)
        return std::nullopt;
    const auto tc = static_cast<std::ptrdiff_t>(*c) + dc;
    const auto tr = static_cast<std::ptrdiff_t>(*r) + dr;
    if (tc < 0 || tr < 0 || tc >= static_cast<std::ptrdiff_t>(m.columnCount()) ||
        tr >= static_cast<std::ptrdiff_t>(m.sceneCount()) || !holdsSlots(m, static_cast<std::size_t>(tc)))
        return std::nullopt;
    return std::pair{m.column(static_cast<std::size_t>(tc)).track, m.scene(static_cast<std::size_t>(tr)).id};
}

/// The lead clip of a toggle: the focus where it is a selected clip, else the first.
const SlotFacts& leadOf(const Model& m, const std::vector<std::pair<std::size_t, std::size_t>>& clips) {
    const auto focus = focusCell(m);
    if (focus && m.slotSelected(focus->first, focus->second) && m.slot(focus->first, focus->second).clip != 0)
        return m.slot(focus->first, focus->second);
    return m.slot(clips.front().first, clips.front().second);
}

Outcome insertMidiClip(const Model& m, const Context& ctx) {
    Outcome out;
    if (ctx.nextClipId <= 0 || ctx.barTicks <= 0) {
        out.refusal = "The project is not ready for a new clip yet.";
        return out;
    }
    auto id = ctx.nextClipId;
    const auto length = ctx.barTicks * newSlotClipBars;
    constexpr auto label = "Insert MIDI Clip";
    for (const auto& [c, r] : m.selectedSlots()) {
        const auto& col = m.column(c);
        if (col.kind != ColumnKind::Clips || !col.midi || m.slot(c, r).clip != 0)
            continue;
        out.edits.push_back(op("clip.create", {{"id", id}, {"track", col.track}, {"kind", "midi"}, {"pos", 0}, {"length", length}}, label));
        out.edits.push_back(op("clip.setLoop", {{"id", id}, {"on", true}, {"start", 0}, {"length", length}}, label));
        out.edits.push_back(op("session.setSlotClip", {{"track", col.track}, {"scene", m.scene(r).id}, {"clip", id}}, label));
        if (!out.selection)
            out.selection = slotAt(m, c, r); // the new clip, for the Clip View
        ++id;
    }
    if (out.edits.empty())
        out.refusal = "Insert MIDI Clip needs an empty slot on a MIDI track.";
    return out;
}

Outcome toggleFollowActions(const Model& m) {
    Outcome out;
    const auto clips = selectedClipSlots(m);
    if (clips.empty())
        return out;
    const bool on = !leadOf(m, clips).follow;
    for (const auto& [c, r] : clips) {
        const auto& f = m.slot(c, r);
        if (f.follow == on)
            continue;
        auto settings = f.followSettings;
        settings.enabled = on;
        out.edits.push_back(op("session.setFollowAction", followPayload("clip", f.clip, settings, true),
                               on ? "Activate Follow Actions" : "Deactivate Follow Actions"));
    }
    return out;
}

Outcome followActionChain(const Model& m) {
    Outcome out;
    const auto clips = selectedClipSlots(m); // by column, then row
    for (std::size_t i = 0; i < clips.size();) {
        auto j = i;
        while (j < clips.size() && clips[j].first == clips[i].first)
            ++j;
        for (auto k = i; k < j; ++k) {
            const auto [c, r] = clips[k];
            const auto target = clips[k + 1 < j ? k + 1 : i].second;
            auto settings = m.slot(c, r).followSettings;
            settings.enabled = true;
            settings.first = {engine::FollowAction::Jump, m.scene(target).id};
            settings.second = {engine::FollowAction::None, std::nullopt};
            settings.secondChancePercent = 0.0;
            if constexpr (!chainKeepsFollowTime) {
                settings.linked = true;
                settings.loopCount = 1;
            }
            out.edits.push_back(op("session.setFollowAction", followPayload("clip", m.slot(c, r).clip, settings, true),
                                   "Create Follow Action Chain"));
        }
        i = j;
    }
    return out;
}

Outcome duplicateSlots(const Model& m) {
    Outcome out;
    const auto clips = selectedClipSlots(m);
    if (clips.empty())
        return out;
    const auto box = *m.selectedRange();
    const auto height = box.r1 - box.r0 + 1;
    bool fits = true;
    for (const auto& [c, r] : clips)
        fits = fits && r + height < m.sceneCount() && m.slot(c, r + height).clip == 0;
    std::vector<std::int64_t> added;
    if (!fits) {
        if constexpr (!duplicateInsertsScenes) {
            out.refusal = "There is no room below for the copies.";
            return out;
        }
        added = createScenes(out, m, box.r1, std::vector<const SceneFacts*>(height, nullptr), "Duplicate");
    }
    const auto copyScene = [&](std::size_t r) { return fits ? m.scene(r + height).id : added[r - box.r0]; };
    for (const auto& [c, r] : clips)
        out.copies.push_back({m.slot(c, r).clip, m.column(c).track, copyScene(r), std::nullopt, out.edits.size()});
    out.label = "Duplicate";
    // The copies are selected, as the selection stood, one height lower.
    auto next = m.selection();
    const auto lower = [&](std::int64_t scene) {
        const auto r = m.rowOf(scene);
        return r && *r >= box.r0 && *r <= box.r1 ? copyScene(*r) : scene;
    };
    next.scene = lower(next.scene);
    next.anchorScene = lower(next.anchorScene);
    for (auto& p : next.picks)
        p.scene = lower(p.scene);
    out.selection = next;
    return out;
}

Outcome duplicate(const Model& m) {
    const auto& s = m.selection();
    if (s.focus == Focus::Slot)
        return duplicateSlots(m);
    Outcome out;
    if (s.focus == Focus::Track) {
        out.passToShell = true; // Duplicate Track is the track commands'
        return out;
    }
    if (s.focus == Focus::Scene) {
        const auto rows = m.selectedScenes();
        std::vector<Clipboard::Scene> scenes;
        for (const auto r : rows)
            scenes.push_back(sceneEntry(m, r));
        copyScenes(out, m, rows.back(), scenes, rows.size() == 1 ? "Duplicate Scene" : "Duplicate Scenes");
    }
    return out;
}

std::optional<Clipboard> clipboardOf(const Model& m) {
    const auto& s = m.selection();
    Clipboard board;
    if (s.focus == Focus::Slot) {
        const auto box = *m.selectedRange();
        board.kind = Clipboard::Kind::Clips;
        for (const auto& [c, r] : selectedClipSlots(m))
            board.clips.push_back({c - std::min(c, box.c0), r - box.r0, m.slot(c, r).clip, m.slot(c, r).midiClip});
        if (board.clips.empty())
            return std::nullopt;
    } else if (s.focus == Focus::Scene) {
        board.kind = Clipboard::Kind::Scenes;
        for (const auto r : m.selectedScenes())
            board.scenes.push_back(sceneEntry(m, r));
    } else {
        return std::nullopt;
    }
    return board;
}

Outcome copySelection(const Model& m) {
    Outcome out;
    out.clipboard = clipboardOf(m);
    out.passToShell = m.selection().focus == Focus::Track;
    return out;
}

Outcome cutSelection(const Model& m) {
    auto out = deleteSelection(m);
    if (!out.refusal.empty() || out.passToShell)
        return out;
    out.clipboard = clipboardOf(m);
    for (auto& edit : out.edits)
        edit.label = "Cut";
    return out;
}

Outcome paste(const Model& m, const Context& ctx) {
    Outcome out;
    const auto* board = ctx.clipboard;
    if (board == nullptr || board->kind == Clipboard::Kind::None)
        return out;
    if (board->kind == Clipboard::Kind::Scenes) {
        copyScenes(out, m, belowSelection(m), board->scenes, "Paste");
        return out;
    }
    const auto& s = m.selection();
    if (s.focus != Focus::Slot) {
        out.refusal = "Select a slot to paste the clips into.";
        return out;
    }
    const auto box = *m.selectedRange();
    std::size_t rows = 0;
    for (const auto& clip : board->clips)
        rows = std::max(rows, box.r0 + clip.row + 1);
    std::vector<std::int64_t> added;
    if constexpr (pasteAddsScenes)
        if (rows > m.sceneCount())
            added = createScenes(out, m, std::nullopt, std::vector<const SceneFacts*>(rows - m.sceneCount(), nullptr), "Paste");
    const auto sceneOfRow = [&](std::size_t r) -> std::optional<std::int64_t> {
        if (r < m.sceneCount())
            return m.scene(r).id;
        if (r - m.sceneCount() < added.size())
            return added[r - m.sceneCount()];
        return std::nullopt;
    };
    auto corner = std::pair{box.c0, box.r0};
    for (const auto& clip : board->clips) {
        const auto c = box.c0 + clip.column;
        const auto r = box.r0 + clip.row;
        const auto scene = sceneOfRow(r);
        if (c >= m.columnCount() || m.column(c).kind != ColumnKind::Clips || m.column(c).midi != clip.midi || !scene)
            continue; // a track that does not take the clip's kind is passed by
        out.copies.push_back({clip.clip, m.column(c).track, *scene, std::nullopt, out.edits.size()});
        corner = {std::max(corner.first, c), std::max(corner.second, r)};
    }
    if (out.copies.empty()) {
        out.edits.clear();
        out.refusal = "No track here takes those clips.";
        return out;
    }
    out.label = "Paste";
    Selection pasted;
    pasted.focus = Focus::Slot;
    pasted.anchorTrack = m.column(box.c0).track;
    pasted.anchorScene = *sceneOfRow(box.r0);
    pasted.track = m.column(corner.first).track;
    pasted.scene = *sceneOfRow(corner.second);
    out.selection = pasted;
    return out;
}

/// The selection's items as picks, so a Ctrl+click adds to or takes from what is selected.
std::vector<Pick> picksOf(const Model& m) {
    if (!m.selection().picks.empty())
        return m.selection().picks;
    std::vector<Pick> out;
    switch (m.selection().focus) {
    case Focus::Slot:
        for (const auto& [c, r] : m.selectedSlots())
            out.push_back({m.column(c).track, m.scene(r).id});
        break;
    case Focus::Scene:
        for (const auto r : m.selectedScenes())
            out.push_back({0, m.scene(r).id});
        break;
    case Focus::Track:
        for (const auto c : m.selectedTracks())
            out.push_back({m.column(c).track, 0});
        break;
    case Focus::None:
        break;
    }
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
    const auto rows = m.selectedScenes();
    if (keepOneScene && rows.size() >= m.sceneCount()) {
        out.refusal = "A Session keeps at least one scene.";
        return out;
    }
    for (const auto r : rows)
        out.edits.push_back(op("scene.delete", {{"id", m.scene(r).id}}, rows.size() == 1 ? "Delete Scene" : "Delete Scenes"));
    // The selection stays at the place the scenes left: the next scene that stays, or the new last one.
    const auto kept = [&](std::size_t r) { return !std::binary_search(rows.begin(), rows.end(), r); };
    std::optional<std::size_t> next;
    for (auto r = rows.back() + 1; r < m.sceneCount() && !next; ++r)
        if (kept(r))
            next = r;
    for (auto r = rows.back(); r-- > 0 && !next;)
        if (kept(r))
            next = r;
    out.selection = next ? sceneAt(m, *next) : Selection{};
    return out;
}

Outcome stopButtons(const Model& m) {
    Outcome out;
    const auto focus = focusCell(m);
    std::optional<bool> on;
    if (focus && m.slotSelected(focus->first, focus->second) && m.column(focus->first).kind == ColumnKind::Clips)
        on = !m.slot(focus->first, focus->second).hasStop;
    for (const auto& [c, r] : m.selectedSlots()) {
        if (m.column(c).kind != ColumnKind::Clips)
            continue;
        if (!on)
            on = !m.slot(c, r).hasStop;
        if (m.slot(c, r).hasStop == *on)
            continue;
        out.edits.push_back(op("session.setStopButton",
                               {{"track", m.column(c).track}, {"scene", m.scene(r).id}, {"on", *on}},
                               *on ? "Add Stop Button" : "Remove Stop Button"));
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
    const auto& first = focus && m.slotSelected(focus->first, focus->second) && m.slot(focus->first, focus->second).clip != 0
                            ? m.slot(focus->first, focus->second)
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
    const auto rows = m.selectedScenes();
    if (rows.empty() || (up && rows.front() == 0) || (!up && rows.back() + 1 >= m.sceneCount()))
        return out;
    // Each selected scene trades places with its neighbour, so nonadjacent scenes keep their gaps
    // (41.15: Move Nonadjacent Scenes Without Collapsing).
    auto order = sceneOrder(m);
    if (up)
        for (const auto r : rows)
            std::swap(order[r - 1], order[r]);
    else
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            std::swap(order[*it], order[*it + 1]);
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
        if (onlyShift)
            return Command::ToggleFollowActions;
        if (k.command && k.shift && !k.alt)
            return Command::CreateFollowActionChain;
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
            case U'd': return Command::Duplicate;
            case U'c': return Command::Copy;
            case U'x': return Command::Cut;
            case U'v': return Command::Paste;
            default: return std::nullopt;
            }
        }
        if (k.command && k.shift && !k.alt && k.character == U'i')
            return Command::CaptureAndInsertScene;
        if (k.command && k.shift && !k.alt && k.character == U'm')
            return Command::InsertMidiClip;
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
        const auto cells = m.selectedSlots();
        if (cells.empty() || !needsIncarnation(ctx, out))
            return out;
        std::set<std::size_t> stopped;
        for (const auto& cell : cells)
            for (const auto k : clipColumnsOf(m, cell.first))
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
    case Command::InsertMidiClip: return insertMidiClip(m, ctx);
    case Command::ToggleFollowActions: return toggleFollowActions(m);
    case Command::CreateFollowActionChain: return followActionChain(m);
    case Command::Duplicate: return duplicate(m);
    case Command::Copy: return copySelection(m);
    case Command::Cut: return cutSelection(m);
    case Command::Paste: return paste(m, ctx);
    }
    return {};
}

Outcome click(const Model& m, const Click& k) {
    Outcome out;
    Selection item;
    switch (k.what) {
    case Target::Slot:
        if (k.column >= m.columnCount() || k.row >= m.sceneCount() || !holdsSlots(m, k.column))
            return out;
        item = slotAt(m, k.column, k.row);
        break;
    case Target::Scene:
        if (k.row >= m.sceneCount())
            return out;
        item = sceneAt(m, k.row);
        break;
    case Target::Track:
        if (k.column >= m.columnCount())
            return out;
        item = trackAt(m, k.column);
        break;
    }
    const auto& s = m.selection();
    const bool sameKind = s.focus == item.focus;
    if (k.command && sameKind) {
        auto picks = picksOf(m);
        const Pick p{item.track, item.scene};
        if (const auto at = std::find(picks.begin(), picks.end(), p); at != picks.end())
            picks.erase(at);
        else
            picks.push_back(p);
        if (picks.empty()) {
            out.selection = Selection{};
            return out;
        }
        item.picks = std::move(picks);
        out.selection = std::move(item);
        return out;
    }
    if (k.shift && sameKind) {
        auto next = s;
        next.picks.clear();
        next.track = item.track;
        next.scene = item.scene;
        out.selection = std::move(next);
        return out;
    }
    out.selection = std::move(item);
    return out;
}

Outcome rubberBand(const Model& m, std::size_t fromColumn, std::size_t fromRow, std::size_t toColumn, std::size_t toRow) {
    Outcome out;
    const auto inside = [&](std::size_t c, std::size_t r) { return c < m.columnCount() && r < m.sceneCount() && holdsSlots(m, c); };
    if (!inside(fromColumn, fromRow) || !inside(toColumn, toRow))
        return out;
    auto band = slotAt(m, toColumn, toRow);
    band.anchorTrack = m.column(fromColumn).track;
    band.anchorScene = m.scene(fromRow).id;
    out.selection = std::move(band);
    return out;
}

Outcome dropSlots(const Model& m, std::size_t grabColumn, std::size_t grabRow, std::size_t column, std::size_t row,
                  bool copy) {
    Outcome out;
    const auto clips = selectedClipSlots(m);
    if (clips.empty() || grabColumn >= m.columnCount() || column >= m.columnCount() || grabRow >= m.sceneCount() ||
        row >= m.sceneCount())
        return out;
    const auto dc = static_cast<std::ptrdiff_t>(column) - static_cast<std::ptrdiff_t>(grabColumn);
    const auto dr = static_cast<std::ptrdiff_t>(row) - static_cast<std::ptrdiff_t>(grabRow);
    if (dc == 0 && dr == 0)
        return out;
    struct Move {
        std::size_t c, r;
        std::int64_t track, scene;
    };
    std::vector<Move> moves;
    for (const auto& [c, r] : clips) {
        const auto to = shifted(m, m.column(c).track, m.scene(r).id, dc, dr);
        const auto tc = to ? m.columnOf(to->first) : std::nullopt;
        if (!tc || m.column(*tc).kind != ColumnKind::Clips || m.column(*tc).midi != m.slot(c, r).midiClip) {
            out.refusal = "Those clips do not fit there.";
            return out;
        }
        moves.push_back({c, r, to->first, to->second});
    }
    const auto label = copy ? "Copy Clips" : "Move Clips";
    out.label = label;
    if (copy) {
        for (const auto& mv : moves)
            out.copies.push_back({m.slot(mv.c, mv.r).clip, mv.track, mv.scene, std::nullopt, 0});
    } else if (dc == 0) {
        // Along its own track a clip moves itself: the one moving out of a slot first.
        std::sort(moves.begin(), moves.end(), [dr](const Move& a, const Move& b) { return dr > 0 ? a.r > b.r : a.r < b.r; });
        for (const auto& mv : moves)
            out.edits.push_back(op("session.setSlotClip", {{"track", mv.track}, {"scene", mv.scene}, {"clip", m.slot(mv.c, mv.r).clip}}, label));
    } else {
        // To another track a clip goes as its copy: the sources are cleared first, so a copy may
        // land where another clip of the drag left (the copies are taken before the batch runs).
        for (const auto& mv : moves)
            out.edits.push_back(op("session.clearSlot", {{"track", m.column(mv.c).track}, {"scene", m.scene(mv.r).id}}, label));
        for (const auto& mv : moves)
            out.copies.push_back({m.slot(mv.c, mv.r).clip, mv.track, mv.scene, std::nullopt, out.edits.size()});
    }
    // The selection goes with the clips.
    auto next = m.selection();
    const auto focus = shifted(m, next.track, next.scene, dc, dr);
    const auto anchor = shifted(m, next.anchorTrack, next.anchorScene, dc, dr);
    bool kept = focus && anchor;
    for (auto& p : next.picks) {
        const auto to = shifted(m, p.track, p.scene, dc, dr);
        kept = kept && to;
        if (to)
            p = {to->first, to->second};
    }
    if (kept) {
        std::tie(next.track, next.scene) = *focus;
        std::tie(next.anchorTrack, next.anchorScene) = *anchor;
        out.selection = std::move(next);
    } else {
        out.selection = slotAt(m, column, row);
    }
    return out;
}

Outcome dropScenes(const Model& m, std::size_t beforeRow) {
    Outcome out;
    const auto rows = m.selectedScenes();
    if (rows.empty() || beforeRow > m.sceneCount())
        return out;
    const auto order = sceneOrder(m);
    std::vector<std::int64_t> moved, rest;
    std::size_t place = 0;
    for (std::size_t r = 0; r < order.size(); ++r) {
        if (std::binary_search(rows.begin(), rows.end(), r)) {
            moved.push_back(order[r]);
        } else {
            if (r < beforeRow)
                ++place;
            rest.push_back(order[r]);
        }
    }
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(place), moved.begin(), moved.end());
    if (rest == order)
        return out;
    out.edits.push_back(op("scene.reorder", {{"scenes", rest}}, rows.size() == 1 ? "Move Scene" : "Move Scenes"));
    out.selection = sceneRun(moved.front(), moved.back()); // collapsed together (7.2)
    return out;
}

Outcome recolour(const Model& m, std::int64_t colour) {
    Outcome out;
    if (colour < 0) {
        out.refusal = "A colour is one of the palette's.";
        return out;
    }
    switch (m.selection().focus) {
    case Focus::Slot:
        for (const auto& [c, r] : selectedClipSlots(m))
            if (m.slot(c, r).color != colour)
                out.edits.push_back(op("clip.setColor", {{"id", m.slot(c, r).clip}, {"color", colour}}, "Change Clip Color"));
        break;
    case Focus::Track:
        for (const auto c : m.selectedTracks())
            if (m.column(c).color != colour)
                out.edits.push_back(op("track.setColor", {{"id", m.column(c).track}, {"color", colour}}, "Change Track Color"));
        break;
    case Focus::Scene:
        out.refusal = "A scene's colour cannot be changed yet.";
        break;
    case Focus::None:
        break;
    }
    return out;
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
