// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MIDI Note Editor's edits (STEP-7-PLAN 7.7 step 3; Live 12 manual 10.4, 10.5), headless: each
// builds the ops of one gesture from the clip as it was read (`engine::readClipDetails`), for the
// window to commit as one undo step. Nothing here touches the store.
//
// A note is addressed by its exact id (`ClipNote::id`); times are source-relative ticks. A clip
// whose notes could not be read, or whose ids are ambiguous, refuses every edit (ENGINE-CLIP-DETAILS).
#pragma once
#include "adi/engine/clip_details.hpp"
#include "adi/ops.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/note_geometry.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace adi::ui {
struct NoteEdit {
    std::vector<OpRequest> ops;
    std::vector<std::uint64_t> select; // the notes selected once the batch lands
    std::string refusal;               // why nothing is done; empty when the ops are ready
};
// The shortest note an edit leaves: a 128th note.
inline constexpr std::int64_t kShortestNote = textproj::kPPQ / 32;

namespace note_edits_detail {
inline OpRequest op(const char *type, Payload payload, const char *label) {
    OpRequest r;
    r.opType = type;
    r.payload = std::move(payload);
    r.label = label;
    return r;
}
inline const engine::ClipNote *find(const engine::ClipDetails &clip, std::uint64_t id) noexcept {
    for (const auto &n : clip.notes)
        if (n.id == id)
            return &n;
    return nullptr;
}
// Why this clip's notes cannot be edited at all; empty when they can.
inline std::string closed(const engine::ClipDetails &clip) {
    if (clip.kind != "midi")
        return "Only a MIDI clip has notes.";
    if (!clip.notesProblem.empty())
        return "This clip's notes cannot be edited: " + clip.notesProblem;
    return {};
}
// The notes named by `ids` that the clip holds, in the clip's order.
inline std::vector<const engine::ClipNote *> pick(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    std::vector<const engine::ClipNote *> out;
    for (const auto &n : clip.notes)
        if (std::find(ids.begin(), ids.end(), n.id) != ids.end())
            out.push_back(&n);
    return out;
}
} // namespace note_edits_detail

// An id no note of the clip has: one past the highest, never zero.
inline std::uint64_t freeNoteId(const engine::ClipDetails &clip) noexcept {
    std::uint64_t highest = 0;
    for (const auto &n : clip.notes)
        highest = std::max(highest, n.id);
    return highest + 1;
}

// A new note (manual 10.4): a double-click in the editor.
inline NoteEdit addNote(const engine::ClipDetails &clip, std::int64_t start, std::int64_t duration, int key,
                        int velocity = 100) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    if (key < 0 || key >= kNoteKeys) {
        edit.refusal = "There is no key there.";
        return edit;
    }
    const auto id = freeNoteId(clip);
    edit.ops.push_back(op("note.insert",
                          {{"clip", clip.id},
                           {"note", id},
                           {"start", start},
                           {"dur", std::max(kShortestNote, duration)},
                           {"key", key},
                           {"vel", std::clamp(velocity, 1, 127)}},
                          "Add Note"));
    edit.select = {id};
    return edit;
}

// The notes deleted (manual 10.4, 10.5): a double-click on one, or Delete on the selection.
inline NoteEdit deleteNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const auto notes = pick(clip, ids);
    const char *label = notes.size() == 1 ? "Delete Note" : "Delete Notes";
    for (const auto *n : notes)
        edit.ops.push_back(op("note.delete", {{"clip", clip.id}, {"note", n->id}}, label));
    return edit;
}

// The notes moved in time and pitch together (manual 10.5.4). A move that would take any of them
// past the lowest or highest key stops at it for all, so the chord keeps its shape.
inline NoteEdit moveNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t ticks,
                          int keys) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const auto notes = pick(clip, ids);
    if (notes.empty())
        return edit;
    int low = kNoteKeys - 1, high = 0;
    for (const auto *n : notes) {
        low = std::min(low, static_cast<int>(n->key));
        high = std::max(high, static_cast<int>(n->key));
    }
    const int by = std::clamp(keys, -low, kNoteKeys - 1 - high);
    edit.select = ids;
    if (ticks == 0 && by == 0)
        return edit;
    const char *label = notes.size() == 1 ? "Move Note" : "Move Notes";
    for (const auto *n : notes)
        edit.ops.push_back(op("note.move",
                              {{"clip", clip.id}, {"note", n->id}, {"start", n->startTicks + ticks}, {"key", n->key + by}},
                              label));
    return edit;
}

// The notes' ends moved (manual 10.5.5): each keeps at least the shortest length.
inline NoteEdit resizeNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t ticks) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const auto notes = pick(clip, ids);
    edit.select = ids;
    const char *label = notes.size() == 1 ? "Resize Note" : "Resize Notes";
    for (const auto *n : notes) {
        const auto duration = std::max(kShortestNote, n->durationTicks + ticks);
        if (duration != n->durationTicks)
            edit.ops.push_back(op("note.resize", {{"clip", clip.id}, {"note", n->id}, {"dur", duration}}, label));
    }
    return edit;
}

// The notes' starts moved, their ends staying where they are (manual 10.5.5).
inline NoteEdit trimNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t ticks) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const auto notes = pick(clip, ids);
    edit.select = ids;
    const char *label = notes.size() == 1 ? "Resize Note" : "Resize Notes";
    for (const auto *n : notes) {
        // The start stops a shortest note before the end.
        const auto by = std::min(ticks, std::max<std::int64_t>(0, n->durationTicks - kShortestNote));
        if (by == 0)
            continue;
        edit.ops.push_back(
            op("note.move", {{"clip", clip.id}, {"note", n->id}, {"start", n->startTicks + by}, {"key", n->key}}, label));
        edit.ops.push_back(
            op("note.resize", {{"clip", clip.id}, {"note", n->id}, {"dur", n->durationTicks - by}}, label));
    }
    return edit;
}

// The notes' velocities changed by `by` (manual 10.6): each stays within 1 to 127.
inline NoteEdit changeVelocities(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int by) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    for (const auto *n : pick(clip, ids)) {
        const int velocity = std::clamp(static_cast<int>(n->velocity) + by, 1, 127);
        if (velocity != n->velocity)
            edit.ops.push_back(
                op("note.setVelocity", {{"clip", clip.id}, {"note", n->id}, {"vel", velocity}}, "Change Velocity"));
    }
    return edit;
}
// The notes deactivated, or activated when every one of them already is off (manual 10.5.7): the
// 0 key. A deactivated note stays in the clip, grey and silent.
inline NoteEdit toggleNotesActive(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const auto notes = pick(clip, ids);
    edit.select = ids;
    const bool allOff = !notes.empty() && std::none_of(notes.begin(), notes.end(), [](const auto *n) { return n->active; });
    const bool plural = notes.size() != 1;
    const char *label = allOff ? (plural ? "Activate Notes" : "Activate Note") : (plural ? "Deactivate Notes" : "Deactivate Note");
    for (const auto *n : notes)
        if (n->active == !allOff)
            edit.ops.push_back(op("note.setMute", {{"clip", clip.id}, {"note", n->id}, {"muted", !allOff}}, label));
    return edit;
}
} // namespace adi::ui
