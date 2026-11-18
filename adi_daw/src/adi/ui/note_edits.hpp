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
#include "adi/ui/scales.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
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

// Notes drawn in Draw Mode (manual 10.4.1): one a grid step long at each of `starts`, on `key`,
// skipping a step where a note on that key already sounds. One batch, however long the stroke.
inline NoteEdit drawNotes(const engine::ClipDetails &clip, const std::vector<std::int64_t> &starts, std::int64_t step,
                          int key, int velocity = 100) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    if (key < 0 || key >= kNoteKeys) {
        edit.refusal = "There is no key there.";
        return edit;
    }
    const auto length = std::max(kShortestNote, step);
    auto id = freeNoteId(clip);
    const char *label = starts.size() == 1 ? "Draw Note" : "Draw Notes";
    for (const auto start : starts) {
        const bool taken = std::any_of(clip.notes.begin(), clip.notes.end(), [&](const engine::ClipNote &n) {
            return n.key == key && n.startTicks < start + length && n.startTicks + n.durationTicks > start;
        });
        if (taken)
            continue;
        edit.ops.push_back(op("note.insert",
                              {{"clip", clip.id}, {"note", id}, {"start", start}, {"dur", length}, {"key", key},
                               {"vel", std::clamp(velocity, 1, 127)}},
                              label));
        edit.select.push_back(id++);
    }
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

// Copies of the notes, moved by `ticks` and `keys` from their originals (manual 10.5.4: Ctrl/Option
// and a drag; 10.5.3: Duplicate): each copy keeps its original's length, velocities, channel,
// probability and tuning, and is deactivated if its original is. The copies become the selection.
inline NoteEdit copyNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t ticks,
                          int keys, const char *label) {
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
    auto id = freeNoteId(clip);
    for (const auto *n : notes) {
        edit.ops.push_back(op("note.insert",
                              {{"clip", clip.id},
                               {"note", id},
                               {"start", n->startTicks + ticks},
                               {"dur", n->durationTicks},
                               {"key", n->key + by},
                               {"vel", n->velocity},
                               {"velOff", n->releaseVelocity},
                               {"chan", n->channel},
                               {"prob", n->probability},
                               {"cents", static_cast<double>(n->tuningCents)}},
                              label));
        if (!n->active)
            edit.ops.push_back(op("note.setMute", {{"clip", clip.id}, {"note", id}, {"muted", true}}, label));
        edit.select.push_back(id++);
    }
    return edit;
}
// Notes pasted into a clip (manual 10.5.4): `notes` hold their times relative to the first of them
// (the earliest starts at 0); each is inserted at `at` plus its own time, with a new id and all it
// carried. The pasted notes become the selection.
inline NoteEdit pasteNotes(const engine::ClipDetails &clip, const std::vector<engine::ClipNote> &notes, std::int64_t at) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    auto id = freeNoteId(clip);
    const char *label = notes.size() == 1 ? "Paste Note" : "Paste Notes";
    for (const auto &n : notes) {
        edit.ops.push_back(op("note.insert",
                              {{"clip", clip.id},
                               {"note", id},
                               {"start", at + n.startTicks},
                               {"dur", std::max(kShortestNote, n.durationTicks)},
                               {"key", n.key},
                               {"vel", std::clamp<int>(n.velocity, 1, 127)},
                               {"velOff", n.releaseVelocity},
                               {"chan", n.channel},
                               {"prob", n.probability},
                               {"cents", static_cast<double>(n.tuningCents)}},
                              label));
        if (!n.active)
            edit.ops.push_back(op("note.setMute", {{"clip", clip.id}, {"note", id}, {"muted", true}}, label));
        edit.select.push_back(id++);
    }
    return edit;
}
// The span of time the notes cover: from the first start to the last end; nothing for none.
inline std::int64_t spanOf(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    const auto notes = note_edits_detail::pick(clip, ids);
    if (notes.empty())
        return 0;
    std::int64_t from = notes.front()->startTicks, to = from;
    for (const auto *n : notes) {
        from = std::min(from, n->startTicks);
        to = std::max(to, n->startTicks + n->durationTicks);
    }
    return to - from;
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

// The trim tool (ADR-0241 d2; Cubase's Trim tool): each note named ends at its new length, never
// shorter than the shortest; one batch for the stroke.
inline NoteEdit trimNotesTo(const engine::ClipDetails &clip, const std::vector<std::pair<std::uint64_t, std::int64_t>> &lengths) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const char *label = lengths.size() == 1 ? "Trim Note" : "Trim Notes";
    for (const auto &[id, length] : lengths) {
        const auto *n = find(clip, id);
        const auto duration = std::max(kShortestNote, length);
        if (n != nullptr && duration != n->durationTicks)
            edit.ops.push_back(op("note.resize", {{"clip", clip.id}, {"note", id}, {"dur", duration}}, label));
    }
    return edit;
}

// Velocities drawn by a line tool (ADR-0241 d2): each note named takes its value, within 1 to 127;
// one batch for the stroke. A note the clip does not hold, or one already at its value, is left out.
inline NoteEdit drawVelocities(const engine::ClipDetails &clip, const std::vector<std::pair<std::uint64_t, int>> &values,
                               const char *label = "Draw Velocities") {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    for (const auto &[id, value] : values) {
        const auto *n = find(clip, id);
        const int velocity = std::clamp(value, 1, 127);
        if (n == nullptr)
            continue;
        edit.select.push_back(id);
        if (velocity != n->velocity)
            edit.ops.push_back(
                op("note.setVelocity", {{"clip", clip.id}, {"note", id}, {"vel", velocity}}, label));
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
// ---- The Pitch and Time utilities (manual 8.5, 10.5.9; STEP-7-PLAN 7.7 step 4) ---------------------
// Each acts on the selected notes, or on every note of the clip when none is selected, as Live's
// do, and is one batch of the same note ops, so one undo step.
namespace note_edits_detail {
// The notes a utility acts on: those named, or all of them when none is.
inline std::vector<const engine::ClipNote *> targets(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    if (!ids.empty())
        return pick(clip, ids);
    std::vector<const engine::ClipNote *> out;
    for (const auto &n : clip.notes)
        out.push_back(&n);
    return out;
}
inline std::vector<std::uint64_t> idsOf(const std::vector<const engine::ClipNote *> &notes) {
    std::vector<std::uint64_t> out;
    for (const auto *n : notes)
        out.push_back(n->id);
    return out;
}
// A note placed at `start` on `key` for `duration`: the ops that differ from where it is.
inline void place(NoteEdit &edit, std::int64_t clip, const engine::ClipNote &n, std::int64_t start, int key,
                  std::int64_t duration, const char *label) {
    if (start != n.startTicks || key != n.key)
        edit.ops.push_back(op("note.move", {{"clip", clip}, {"note", n.id}, {"start", start}, {"key", key}}, label));
    if (duration != n.durationTicks)
        edit.ops.push_back(op("note.resize", {{"clip", clip}, {"note", n.id}, {"dur", duration}}, label));
}
} // namespace note_edits_detail

// Transpose (manual 8.5.1): by semitones; the notes stop together at the ruler's ends.
inline NoteEdit transposeNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int semitones) {
    using namespace note_edits_detail;
    auto edit = moveNotes(clip, idsOf(targets(clip, ids)), 0, semitones);
    for (auto &o : edit.ops)
        o.label = "Transpose Notes";
    edit.select = ids;
    return edit;
}

// Fit to Scale (manual 8.5.1): a note outside the scale moves to the nearest key in it, the lower
// one when two are as near. `root` is 0 to 11, `mask` the scale's pitches above the root.
inline NoteEdit fitNotesToScale(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int root,
                                std::int64_t mask) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    for (const auto *n : targets(clip, ids))
        place(edit, clip.id, *n, n->startTicks, nearestInScale(n->key, root, mask), n->durationTicks, "Fit to Scale");
    return edit;
}

// Invert (manual 8.5.1): the notes mirrored in pitch between their lowest and highest key.
inline NoteEdit invertNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto notes = targets(clip, ids);
    int low = kNoteKeys - 1, high = 0;
    for (const auto *n : notes) {
        low = std::min(low, static_cast<int>(n->key));
        high = std::max(high, static_cast<int>(n->key));
    }
    for (const auto *n : notes)
        place(edit, clip.id, *n, n->startTicks, low + high - n->key, n->durationTicks, "Invert Notes");
    return edit;
}

// Stretch by `numerator`/`denominator` (manual 8.5.2: x2 and /2): starts and lengths scaled together
// from the first note's start.
inline NoteEdit stretchNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids,
                             std::int64_t numerator, std::int64_t denominator) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto notes = targets(clip, ids);
    if (notes.empty() || numerator <= 0 || denominator <= 0)
        return edit;
    std::int64_t from = notes.front()->startTicks;
    for (const auto *n : notes)
        from = std::min(from, n->startTicks);
    for (const auto *n : notes)
        place(edit, clip.id, *n, from + (n->startTicks - from) * numerator / denominator, n->key,
              std::max(kShortestNote, n->durationTicks * numerator / denominator), "Stretch Notes");
    return edit;
}

// Reverse (manual 8.5.2): the notes mirrored in time between the first start and the last end.
inline NoteEdit reverseNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto notes = targets(clip, ids);
    if (notes.empty())
        return edit;
    std::int64_t from = notes.front()->startTicks, to = from;
    for (const auto *n : notes) {
        from = std::min(from, n->startTicks);
        to = std::max(to, n->startTicks + n->durationTicks);
    }
    for (const auto *n : notes)
        place(edit, clip.id, *n, from + (to - (n->startTicks + n->durationTicks)), n->key, n->durationTicks,
              "Reverse Notes");
    return edit;
}

// Legato (manual 8.5.2): each note lasts until the next later start among them; the last keep
// their length.
inline NoteEdit legatoNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto notes = targets(clip, ids);
    for (const auto *n : notes) {
        std::optional<std::int64_t> next;
        for (const auto *other : notes)
            if (other->startTicks > n->startTicks && (!next || other->startTicks < *next))
                next = other->startTicks;
        if (next)
            place(edit, clip.id, *n, n->startTicks, n->key, *next - n->startTicks, "Legato");
    }
    return edit;
}
// ---- Chord editing and the length functions (ADR-0241 d2; Cubase Pro 15 manual, the Key Editor's
// Chord Editing section and MIDI functions). ADI's additions to Live's editor: each is a composition
// of the same note ops, one undo step, on the selected notes.

// A chord built on a root: its name and the semitones above the root of its other notes.
struct ChordType {
    const char *id;
    const char *name;
    std::array<int, 3> above; // 0 where the chord has no further note
};
inline constexpr std::array<ChordType, 14> kChordTypes{{
    {"major", "Major", {4, 7, 0}},
    {"minor", "Minor", {3, 7, 0}},
    {"diminished", "Diminished", {3, 6, 0}},
    {"augmented", "Augmented", {4, 8, 0}},
    {"sus2", "Sus 2", {2, 7, 0}},
    {"sus4", "Sus 4", {5, 7, 0}},
    {"major7", "Major 7", {4, 7, 11}},
    {"dominant7", "7", {4, 7, 10}},
    {"minor7", "Minor 7", {3, 7, 10}},
    {"minor-major7", "Minor Major 7", {3, 7, 11}},
    {"half-diminished", "Minor 7 b5", {3, 6, 10}},
    {"diminished7", "Diminished 7", {3, 6, 9}},
    {"major6", "6", {4, 7, 9}},
    {"minor6", "Minor 6", {3, 7, 9}},
}};

namespace note_edits_detail {
// The selected notes as chords: those that start together, each chord's notes from the highest down.
inline std::vector<std::vector<const engine::ClipNote *>> chords(const engine::ClipDetails &clip,
                                                                 const std::vector<std::uint64_t> &ids) {
    auto notes = pick(clip, ids);
    std::stable_sort(notes.begin(), notes.end(), [](const auto *a, const auto *b) {
        return a->startTicks != b->startTicks ? a->startTicks < b->startTicks : a->key > b->key;
    });
    std::vector<std::vector<const engine::ClipNote *>> out;
    for (const auto *n : notes) {
        if (out.empty() || out.back().front()->startTicks != n->startTicks)
            out.emplace_back();
        out.back().push_back(n);
    }
    return out;
}
} // namespace note_edits_detail

// A chord made on each selected note, taken as its root: the chord's other notes are added with the
// root's start, length and velocity. A note that is already there, or would lie past the top key, is
// left out. The roots and the new notes are selected.
namespace note_edits_detail {
// Defined with Split, below: a new note that carries everything `n` carries.
inline void piece(NoteEdit &edit, std::int64_t clip, const engine::ClipNote &n, std::uint64_t id, std::int64_t start,
                  std::int64_t duration, const char *label);
} // namespace note_edits_detail
inline NoteEdit makeChords(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, const ChordType &type) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    auto id = freeNoteId(clip);
    std::vector<std::pair<std::int64_t, int>> made; // where this batch has put a note already
    for (const auto *root : pick(clip, ids))
        for (const int above : type.above) {
            const int key = root->key + above;
            if (above == 0 || key >= kNoteKeys)
                continue;
            const bool there = std::any_of(clip.notes.begin(), clip.notes.end(), [&](const engine::ClipNote &n) {
                return n.key == key && n.startTicks == root->startTicks;
            }) || std::find(made.begin(), made.end(), std::pair<std::int64_t, int>{root->startTicks, key}) != made.end();
            if (there)
                continue;
            // The chord's note carries what its root carries: release velocity, channel, chance,
            // tuning and whether it is deactivated.
            auto added = *root;
            added.key = static_cast<std::uint8_t>(key);
            piece(edit, clip.id, added, id++, root->startTicks, root->durationTicks, "Insert Chord");
            made.emplace_back(root->startTicks, key);
        }
    return edit;
}

// Inversions: in each selected chord (the notes that start together) the lowest note goes up an
// octave, or the highest down one. A chord that would leave the keys is left as it is.
inline NoteEdit invertChords(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, bool up) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const char *label = up ? "Invert Chord Up" : "Invert Chord Down";
    for (const auto &chord : chords(clip, ids)) {
        if (chord.size() < 2)
            continue;
        const auto *n = up ? chord.back() : chord.front();
        const int key = n->key + (up ? 12 : -12);
        if (key < 0 || key >= kNoteKeys)
            continue;
        edit.ops.push_back(op("note.move", {{"clip", clip.id}, {"note", n->id}, {"start", n->startTicks}, {"key", key}}, label));
    }
    return edit;
}

// Drop voicings: in each selected chord the second-highest note (Drop 2), the third-highest (Drop 3),
// or the second and the fourth (Drop 2+4) go down an octave. `which` lists the places from the top,
// from 1.
inline NoteEdit dropVoicing(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids,
                            std::initializer_list<int> which, const char *label) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    for (const auto &chord : chords(clip, ids))
        for (const int place : which) {
            if (place < 1 || static_cast<std::size_t>(place) > chord.size())
                continue;
            const auto *n = chord[static_cast<std::size_t>(place - 1)];
            if (n->key < 12)
                continue;
            edit.ops.push_back(
                op("note.move", {{"clip", clip.id}, {"note", n->id}, {"start", n->startTicks}, {"key", n->key - 12}}, label));
        }
    return edit;
}

// Fixed Lengths: every selected note as long as `length` (the editing grid's step).
inline NoteEdit fixedLengths(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t length) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto duration = std::max(kShortestNote, length);
    for (const auto *n : targets(clip, ids))
        place(edit, clip.id, *n, n->startTicks, n->key, duration, "Fixed Lengths");
    return edit;
}

// Fit to Time Range (Live 12 manual 10.5.5): every chosen note begins at the range's start and ends
// at its end. In the clip's own time; the notes are named, never "all of them". One undo step.
inline NoteEdit fitNotesToRange(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t from,
                                std::int64_t to) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty() || ids.empty() || to <= from)
        return edit;
    edit.select = ids;
    for (const auto *n : pick(clip, ids))
        place(edit, clip.id, *n, from, n->key, std::max(kShortestNote, to - from), "Fit to Time Range");
    return edit;
}

// Delete Overlaps: a note that still sounds when a later one begins is cut to end there. Mono: only
// against later notes on the same key. Poly: against any later note, so no two notes sound together.
// Notes that start together are left as they are (a chord is not an overlap).
inline NoteEdit deleteOverlaps(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, bool poly) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto notes = targets(clip, ids);
    const char *label = poly ? "Delete Overlaps (Poly)" : "Delete Overlaps (Mono)";
    for (const auto *n : notes) {
        std::optional<std::int64_t> next;
        for (const auto *other : notes)
            if (other->startTicks > n->startTicks && (poly || other->key == n->key) && (!next || other->startTicks < *next))
                next = other->startTicks;
        if (next && *next < n->startTicks + n->durationTicks)
            place(edit, clip.id, *n, n->startTicks, n->key,
                  std::min(n->durationTicks, std::max(kShortestNote, *next - n->startTicks)), label);
    }
    return edit;
}
// Quantize (manual 10.5.11; Ctrl/Cmd+U): the notes' starts to the grid, in full, as Live's default
// settings do. `grid` is the step and `origin` a grid line, both in the clip's own ticks. The engine's
// one op carries every setting (OPS 9.5), and its inverse restores the notes exactly.
// The Quantize settings (manual 10.5.11, the Quantize tool): how far towards the grid, in hundredths
// of a per cent; whether starts and ends move; and swing, the engine's, which delays every other
// division by up to half of one (ADI's addition, ADR-0242 d1).
struct QuantizeSettings {
    int strength = 10000, swing = 0;
    bool starts = true, ends = false;
};
inline NoteEdit quantizeNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t grid,
                              std::int64_t origin, QuantizeSettings settings = {}) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    const auto notes = targets(clip, ids);
    const int strength = std::clamp(settings.strength, 0, 10000), swing = std::clamp(settings.swing, 0, 10000);
    if (notes.empty() || grid <= 0 || strength == 0 || (!settings.starts && !settings.ends))
        return edit;
    // Nothing to write when every start and end that would move is on the grid already; with
    // swing the grid's own lines move, so the engine is asked.
    const bool off = swing != 0 || std::any_of(notes.begin(), notes.end(), [&](const auto *n) {
        return (settings.starts && ((n->startTicks - origin) % grid) != 0) ||
               (settings.ends && ((n->startTicks + n->durationTicks - origin) % grid) != 0);
    });
    if (!off)
        return edit;
    edit.ops.push_back(op("note.quantize",
                          {{"clip", clip.id},
                           {"note_ids", idsOf(notes)},
                           {"grid_ticks", grid},
                           {"origin_ticks", origin},
                           {"strength", strength},
                           {"swing", swing},
                           {"starts", settings.starts},
                           {"ends", settings.ends}},
                          "Quantize Notes"));
    return edit;
}

namespace note_edits_detail {
// A piece of `n` from `start` for `duration`, as a new note carrying what `n` carries.
inline void piece(NoteEdit &edit, std::int64_t clip, const engine::ClipNote &n, std::uint64_t id, std::int64_t start,
                  std::int64_t duration, const char *label) {
    edit.ops.push_back(op("note.insert",
                          {{"clip", clip},
                           {"note", id},
                           {"start", start},
                           {"dur", duration},
                           {"key", n.key},
                           {"vel", std::clamp<int>(n.velocity, 1, 127)},
                           {"velOff", n.releaseVelocity},
                           {"chan", n.channel},
                           {"prob", n.probability},
                           {"cents", static_cast<double>(n.tuningCents)}},
                          label));
    if (!n.active)
        edit.ops.push_back(op("note.setMute", {{"clip", clip}, {"note", id}, {"muted", true}}, label));
    edit.select.push_back(id);
}
// The most ops one Chop may hold.
inline constexpr std::size_t kMostChopOps = 4096;
} // namespace note_edits_detail

// Split (manual 10.5.8.1; Ctrl/Cmd+E with no note selected): every note sounding at `at` is cut
// there; the part after it is a new note.
inline NoteEdit splitNotesAt(const engine::ClipDetails &clip, std::int64_t at) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    auto id = freeNoteId(clip);
    for (const auto &n : clip.notes) {
        if (at <= n.startTicks || at >= n.startTicks + n.durationTicks)
            continue;
        edit.ops.push_back(op("note.resize", {{"clip", clip.id}, {"note", n.id}, {"dur", at - n.startTicks}}, "Split Notes"));
        piece(edit, clip.id, n, id++, at, n.startTicks + n.durationTicks - at, "Split Notes");
    }
    return edit;
}

// Split at a time selection's edges (manual 10.5.8.1): every note is cut at each of `cuts` that
// falls inside it, in one batch; a note crossing two of them becomes three.
inline NoteEdit splitNotesAtEach(const engine::ClipDetails &clip, std::vector<std::int64_t> cuts) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    auto id = freeNoteId(clip);
    for (const auto &n : clip.notes) {
        const auto end = n.startTicks + n.durationTicks;
        std::vector<std::int64_t> inside;
        for (const auto at : cuts)
            if (at > n.startTicks && at < end)
                inside.push_back(at);
        if (inside.empty())
            continue;
        edit.ops.push_back(op("note.resize", {{"clip", clip.id}, {"note", n.id}, {"dur", inside.front() - n.startTicks}}, "Split Notes"));
        for (std::size_t i = 0; i < inside.size(); ++i)
            piece(edit, clip.id, n, id++, inside[i], (i + 1 < inside.size() ? inside[i + 1] : end) - inside[i], "Split Notes");
    }
    return edit;
}

// Chop (manual 10.5.8.2; Ctrl/Cmd+E with notes selected): each selected note is cut at every grid
// line that crosses it. `grid` is the step and `origin` a grid line, in the clip's own ticks.
inline NoteEdit chopNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, std::int64_t grid,
                          std::int64_t origin) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    if (grid <= 0)
        return edit;
    // The first grid line after a note's start.
    const auto firstLine = [&](const engine::ClipNote &n) {
        const auto rel = n.startTicks - origin;
        auto line = origin + (rel >= 0 ? rel / grid + 1 : -((-rel) / grid)) * grid;
        if (line <= n.startTicks)
            line += grid;
        return line;
    };
    const auto notes = pick(clip, ids);
    // Counted first, and refused whole past the cap: a chop that stopped part way would leave a
    // note cut to its first piece with the rest of it gone.
    std::size_t count = 0;
    for (const auto *n : notes) {
        const auto end = n->startTicks + n->durationTicks;
        const auto line = firstLine(*n);
        if (line >= end)
            continue;
        const auto pieces = static_cast<std::size_t>((end - line + grid - 1) / grid);
        if (pieces > kMostChopOps || (count += 1 + pieces * (n->active ? 1u : 2u)) > kMostChopOps) {
            edit.refusal = "That is too many pieces to chop into. Choose a wider grid or fewer notes.";
            return edit;
        }
    }
    auto id = freeNoteId(clip);
    for (const auto *n : notes) {
        const auto end = n->startTicks + n->durationTicks;
        auto line = firstLine(*n);
        if (line >= end) {
            edit.select.push_back(n->id);
            continue;
        }
        edit.ops.push_back(op("note.resize", {{"clip", clip.id}, {"note", n->id}, {"dur", line - n->startTicks}}, "Chop Notes"));
        edit.select.push_back(n->id);
        for (; line < end; line += grid)
            piece(edit, clip.id, *n, id++, line, std::min(grid, end - line), "Chop Notes");
    }
    return edit;
}

// Join (manual 10.5.8.3; Ctrl/Cmd+J): the selected notes of each key become one note, from the first
// one's start to the last one's end; the first survives and the others go.
inline NoteEdit joinNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    auto notes = pick(clip, ids);
    std::stable_sort(notes.begin(), notes.end(), [](const auto *a, const auto *b) {
        return a->key != b->key ? a->key < b->key : a->startTicks < b->startTicks;
    });
    for (std::size_t i = 0; i < notes.size();) {
        std::size_t j = i + 1;
        auto end = notes[i]->startTicks + notes[i]->durationTicks;
        while (j < notes.size() && notes[j]->key == notes[i]->key) {
            end = std::max(end, notes[j]->startTicks + notes[j]->durationTicks);
            ++j;
        }
        edit.select.push_back(notes[i]->id);
        if (j - i > 1) {
            for (std::size_t k = i + 1; k < j; ++k)
                edit.ops.push_back(op("note.delete", {{"clip", clip.id}, {"note", notes[k]->id}}, "Join Notes"));
            if (end - notes[i]->startTicks != notes[i]->durationTicks)
                edit.ops.push_back(op("note.resize", {{"clip", clip.id}, {"note", notes[i]->id}, {"dur", end - notes[i]->startTicks}}, "Join Notes"));
        }
        i = j;
    }
    return edit;
}

// ---- Live's Velocity Controls (manual 10.5.12) --------------------------------------------------
// Randomize: each velocity raised or lowered by a value between zero and `amount`. `offset(amount)`
// gives one such value, from -amount to +amount; the caller owns the randomness.
template <class Offset>
NoteEdit randomizeVelocities(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int amount, Offset &&offset) {
    using namespace note_edits_detail;
    const int range = std::clamp(amount, 0, 127);
    std::vector<std::pair<std::uint64_t, int>> values;
    for (const auto *n : targets(clip, ids))
        values.emplace_back(n->id, static_cast<int>(n->velocity) + std::clamp(static_cast<int>(offset(range)), -range, range));
    auto edit = drawVelocities(clip, values, "Randomize Velocities");
    edit.select = ids;
    return edit;
}
// The Ramp: the first note in time takes `start`, the last takes `end`, and those between are
// spread evenly, in the order they begin (of notes that begin together, the lower first).
inline NoteEdit rampVelocities(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int start, int end) {
    using namespace note_edits_detail;
    auto notes = targets(clip, ids);
    std::stable_sort(notes.begin(), notes.end(), [](const auto *a, const auto *b) {
        return a->startTicks != b->startTicks ? a->startTicks < b->startTicks : a->key < b->key;
    });
    std::vector<std::pair<std::uint64_t, int>> values;
    const auto last = static_cast<std::int64_t>(notes.size()) - 1;
    for (std::int64_t i = 0; i <= last; ++i) {
        const auto span = static_cast<std::int64_t>(end) - start;
        const auto value = last == 0 ? start : start + (span * i * 2 + (span >= 0 ? last : -last)) / (2 * last);
        values.emplace_back(notes[static_cast<std::size_t>(i)]->id, static_cast<int>(value));
    }
    auto edit = drawVelocities(clip, values, "Ramp Velocities");
    edit.select = ids;
    return edit;
}

// ---- Velocity and note functions (ADR-0242 d1: N4, N5; Cubase Pro 15 manual, MIDI functions) ------
// ADI's additions. Each acts on the selected notes, or on every note when none is selected.

// Compress or Expand: every velocity multiplied by `percent` (under 100 compresses, over 100 expands),
// within 1 to 127.
inline NoteEdit scaleVelocities(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int percent) {
    using namespace note_edits_detail;
    std::vector<std::pair<std::uint64_t, int>> values;
    for (const auto *n : targets(clip, ids))
        values.emplace_back(n->id, static_cast<int>((static_cast<std::int64_t>(n->velocity) * std::max(0, percent) + 50) / 100));
    auto edit = drawVelocities(clip, values, percent < 100 ? "Compress Velocities" : "Expand Velocities");
    edit.select = ids;
    return edit;
}
// Limit: every velocity brought inside `low` to `high`.
inline NoteEdit limitVelocities(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int low, int high) {
    using namespace note_edits_detail;
    const int floor = std::clamp(std::min(low, high), 1, 127), ceiling = std::clamp(std::max(low, high), 1, 127);
    std::vector<std::pair<std::uint64_t, int>> values;
    for (const auto *n : targets(clip, ids))
        values.emplace_back(n->id, std::clamp<int>(n->velocity, floor, ceiling));
    auto edit = drawVelocities(clip, values, "Limit Velocities");
    edit.select = ids;
    return edit;
}
// Add or Subtract: `amount` added to every velocity, within 1 to 127.
inline NoteEdit addToVelocities(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int amount) {
    using namespace note_edits_detail;
    std::vector<std::pair<std::uint64_t, int>> values;
    for (const auto *n : targets(clip, ids))
        values.emplace_back(n->id, static_cast<int>(n->velocity) + amount);
    auto edit = drawVelocities(clip, values, amount < 0 ? "Subtract Velocity" : "Add Velocity");
    edit.select = ids;
    return edit;
}
// Delete Doubles: of notes on the same key that start together, the first stays and the others go.
inline NoteEdit deleteDoubles(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    const auto notes = targets(clip, ids);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        bool doubled = false;
        for (std::size_t j = 0; j < i && !doubled; ++j)
            doubled = notes[j]->key == notes[i]->key && notes[j]->startTicks == notes[i]->startTicks;
        if (doubled)
            edit.ops.push_back(op("note.delete", {{"clip", clip.id}, {"note", notes[i]->id}}, "Delete Doubles"));
        else if (!ids.empty())
            edit.select.push_back(notes[i]->id);
    }
    return edit;
}
// Restrict Polyphony: no more than `voices` notes sound at once. Where a note begins while that many
// already sound, the ones that began earliest are cut to end there.
inline NoteEdit restrictPolyphony(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int voices) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    struct Sounding {
        const engine::ClipNote *note;
        std::int64_t end;
    };
    auto notes = targets(clip, ids);
    std::stable_sort(notes.begin(), notes.end(), [](const auto *a, const auto *b) { return a->startTicks < b->startTicks; });
    std::vector<Sounding> sounding, all;
    const std::size_t limit = static_cast<std::size_t>(std::max(1, voices));
    for (const auto *n : notes) {
        std::erase_if(sounding, [&](const Sounding &s) { return s.end <= n->startTicks; });
        // Notes that start together are one chord: only earlier notes are cut for it.
        while (sounding.size() >= limit) {
            auto earliest = std::min_element(sounding.begin(), sounding.end(), [](const Sounding &a, const Sounding &b) {
                return a.note->startTicks < b.note->startTicks;
            });
            if (earliest->note->startTicks >= n->startTicks)
                break;
            for (auto &kept : all)
                if (kept.note == earliest->note)
                    kept.end = n->startTicks;
            sounding.erase(earliest);
        }
        sounding.push_back({n, n->startTicks + n->durationTicks});
        all.push_back({n, n->startTicks + n->durationTicks});
    }
    for (const auto &kept : all) {
        // Cut, never lengthened: a note already shorter than the shortest stays as it is.
        const auto duration = std::min(kept.note->durationTicks, std::max(kShortestNote, kept.end - kept.note->startTicks));
        if (duration != kept.note->durationTicks)
            edit.ops.push_back(
                op("note.resize", {{"clip", clip.id}, {"note", kept.note->id}, {"dur", duration}}, "Restrict Polyphony"));
    }
    return edit;
}

// Add Interval (manual 10.5.9.4): a new note beside each note, `interval` semitones above it (below,
// when negative), or that many degrees of the scale when `mask` is given (the clip's scale is on).
// The new notes carry what theirs carry and are the selection. One that would leave the ruler is
// left out.
inline NoteEdit addInterval(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, int interval, std::int64_t mask = 0,
                            int tonic = 0) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty() || interval == 0)
        return edit;
    auto id = freeNoteId(clip);
    for (const auto *n : targets(clip, ids)) {
        int key = n->key;
        if (mask == 0) {
            key += interval;
        } else {
            // By the scale's degrees, from the note's own place in it (the nearest, when it is outside).
            key = nearestInScale(key, tonic, mask);
            for (int step = 0; step < std::abs(interval) && key >= 0 && key < kNoteKeys; ++step)
                do
                    key += interval > 0 ? 1 : -1;
                while (key >= 0 && key < kNoteKeys && !inScale(key, tonic, mask));
        }
        if (key < 0 || key >= kNoteKeys)
            continue;
        auto added = *n;
        added.key = static_cast<std::uint8_t>(key);
        piece(edit, clip.id, added, id++, n->startTicks, n->durationTicks, "Add Interval");
    }
    return edit;
}

// ---- MIDI Note Stretch (manual 10.5.6) ------------------------------------------------------------
// A place between `a` and `b` carried to the same share of the way between `a2` and `b2`; with
// `a2` after `b2` the order is turned round.
inline std::int64_t stretchPoint(std::int64_t t, std::int64_t a, std::int64_t b, std::int64_t a2, std::int64_t b2) noexcept {
    if (b == a)
        return a2;
    return a2 + static_cast<std::int64_t>(std::llround(static_cast<double>(t - a) * static_cast<double>(b2 - a2) / static_cast<double>(b - a)));
}
// The pseudo marker's map: what lies between `a` and `p` goes between `a` and `p2`, what lies
// between `p` and `b` goes between `p2` and `b`; outside `a` to `b` nothing moves.
inline std::int64_t stretchPointAround(std::int64_t t, std::int64_t a, std::int64_t p, std::int64_t b, std::int64_t p2) noexcept {
    if (t <= a || t >= b)
        return t;
    return t <= p ? stretchPoint(t, a, p, a, p2) : stretchPoint(t, p, b, p2, b);
}
// The named notes with their starts and ends carried through `map`, a place in the clip's ticks
// to its new place: stretched in proportion; a map that turns the order round mirrors them.
template <class Map>
NoteEdit remapNotes(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, Map &&map, const char *label = "Stretch Notes") {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    for (const auto *n : pick(clip, ids)) {
        const std::int64_t a = map(n->startTicks), b = map(n->startTicks + n->durationTicks);
        place(edit, clip.id, *n, std::min(a, b), n->key, std::max(kShortestNote, std::abs(b - a)), label);
    }
    return edit;
}

// ---- Time inside the clip (manual 10.7) -----------------------------------------------------------
// Crop Clip (10.7.1; Ctrl/Cmd+Shift+J): the notes outside `from` to `to` go. With `cut`, a note that
// crosses an edge is cut there: the loop on, or a time selection (as measured on Live). Without
// it a crossing note is kept whole: the loop off, between the Start and End markers.
inline NoteEdit cropNotes(const engine::ClipDetails &clip, std::int64_t from, std::int64_t to, bool cut) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty() || to <= from)
        return edit;
    for (const auto &n : clip.notes) {
        const auto end = n.startTicks + n.durationTicks;
        if (end <= from || n.startTicks >= to) {
            edit.ops.push_back(op("note.delete", {{"clip", clip.id}, {"note", n.id}}, "Crop Clip"));
            continue;
        }
        if (!cut)
            continue;
        const auto start = std::max(n.startTicks, from), stop = std::min(end, to);
        place(edit, clip.id, n, start, n.key, std::max(kShortestNote, stop - start), "Crop Clip");
    }
    return edit;
}
// Duplicate Time (10.7.2): a copy of the notes that begin in `from` to `to` is placed after it, and
// the notes that begin at or after `to` move on by its length. Duplicate Loop (10.7.3) is the same
// over the loop, under its own name; the loop's new length is the caller's op.
inline NoteEdit duplicateTime(const engine::ClipDetails &clip, std::int64_t from, std::int64_t to, const char *label = "Duplicate Time") {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    const auto length = to - from;
    if (!edit.refusal.empty() || length <= 0)
        return edit;
    auto id = freeNoteId(clip);
    for (const auto &n : clip.notes) {
        if (n.startTicks >= to)
            place(edit, clip.id, n, n.startTicks + length, n.key, n.durationTicks, label);
        else if (n.startTicks >= from)
            piece(edit, clip.id, n, id++, n.startTicks + length, n.durationTicks, label);
    }
    edit.select.clear();
    return edit;
}
// Delete Time (10.7.2): the notes that begin in `from` to `to` go; later notes move back by its
// length; a note from before that reaches into it ends at `from`, or loses the deleted time when
// it reaches past `to`.
inline NoteEdit deleteTime(const engine::ClipDetails &clip, std::int64_t from, std::int64_t to) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    const auto length = to - from;
    if (!edit.refusal.empty() || length <= 0)
        return edit;
    for (const auto &n : clip.notes) {
        const auto end = n.startTicks + n.durationTicks;
        if (n.startTicks >= to)
            place(edit, clip.id, n, n.startTicks - length, n.key, n.durationTicks, "Delete Time");
        else if (n.startTicks >= from)
            edit.ops.push_back(op("note.delete", {{"clip", clip.id}, {"note", n.id}}, "Delete Time"));
        else if (end > from)
            place(edit, clip.id, n, n.startTicks, n.key, std::max(kShortestNote, (end <= to ? from : end - length) - n.startTicks), "Delete Time");
    }
    return edit;
}
// Insert Time (10.7.2): as much empty time as `from` to `to` holds, before it: the notes that begin
// at or after `from` move on by its length.
inline NoteEdit insertTime(const engine::ClipDetails &clip, std::int64_t from, std::int64_t to) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    const auto length = to - from;
    if (!edit.refusal.empty() || length <= 0)
        return edit;
    for (const auto &n : clip.notes)
        if (n.startTicks >= from)
            place(edit, clip.id, n, n.startTicks + length, n.key, n.durationTicks, "Insert Time");
    return edit;
}

// Live's overlap rule, as measured on Live 12.4 (manual 10.5.4): where an edit puts a note over
// another on the same key, the note just placed survives whole. An older note that begins inside it
// is removed; an older note that began before it is cut to end where it begins, and what the older
// note had after it is gone. The edit's batch gains those ops, so the whole is still one undo step.
// Only this clip's ops in the batch are read; notes the edit itself placed never cut each other.
inline NoteEdit withOverlaps(const engine::ClipDetails &clip, NoteEdit edit) {
    using namespace note_edits_detail;
    if (!edit.refusal.empty() || edit.ops.empty())
        return edit;
    struct Placed {
        std::uint64_t id;
        std::int64_t start, duration;
        int key;
        bool touched, gone;
        bool placed = false; // put where it is by this batch: inserted, or moved
    };
    std::vector<Placed> notes;
    for (const auto &n : clip.notes)
        notes.push_back({n.id, n.startTicks, n.durationTicks, n.key, false, false});
    const auto find = [&](std::uint64_t id) -> Placed * {
        for (auto &n : notes)
            if (n.id == id)
                return &n;
        return nullptr;
    };
    for (const auto &o : edit.ops) {
        if (!o.payload.contains("clip") || o.payload.at("clip") != clip.id || !o.payload.contains("note"))
            continue;
        const auto id = o.payload.at("note").get<std::uint64_t>();
        if (o.opType == "note.insert") {
            notes.push_back({id, o.payload.at("start").get<std::int64_t>(), o.payload.at("dur").get<std::int64_t>(),
                             o.payload.at("key").get<int>(), true, false, true});
        } else if (auto *n = find(id); n != nullptr) {
            if (o.opType == "note.move") {
                n->start = o.payload.at("start").get<std::int64_t>();
                n->key = o.payload.at("key").get<int>();
                n->touched = true;
                n->placed = true;
            } else if (o.opType == "note.resize") {
                n->duration = o.payload.at("dur").get<std::int64_t>();
                n->touched = true;
            } else if (o.opType == "note.delete") {
                n->gone = true;
            }
        }
    }
    const char *label = edit.ops.front().label.c_str();
    std::vector<OpRequest> extra;
    for (auto &old : notes) {
        if (old.touched || old.gone)
            continue;
        for (const auto &placed : notes) {
            if (!placed.touched || placed.gone || placed.key != old.key)
                continue;
            const auto placedEnd = placed.start + placed.duration, oldEnd = old.start + old.duration;
            if (old.start >= placed.start && old.start < placedEnd) {
                old.gone = true;
                break;
            }
            if (old.start < placed.start && oldEnd > placed.start)
                old.duration = placed.start - old.start;
        }
        const auto *was = note_edits_detail::find(clip, old.id);
        if (was == nullptr)
            continue;
        if (old.gone)
            extra.push_back(op("note.delete", {{"clip", clip.id}, {"note", old.id}}, label));
        else if (old.duration != was->durationTicks)
            extra.push_back(op("note.resize", {{"clip", clip.id}, {"note", old.id}, {"dur", std::max<std::int64_t>(1, old.duration)}}, label));
    }
    // Two notes this batch puts on one key at one start are one note: the first stays. (Fit to Scale,
    // an inversion or a fold can carry two notes to one place.)
    std::vector<std::uint64_t> dropped, removed;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto &first = notes[i];
        if (!first.touched || first.gone)
            continue;
        for (std::size_t j = i + 1; j < notes.size(); ++j) {
            auto &second = notes[j];
            if (!second.touched || second.gone || second.key != first.key || second.start != first.start ||
                !(first.placed || second.placed))
                continue;
            second.gone = true;
            removed.push_back(second.id);
            if (note_edits_detail::find(clip, second.id) != nullptr)
                extra.push_back(op("note.delete", {{"clip", clip.id}, {"note", second.id}}, label));
            else
                dropped.push_back(second.id); // made by this batch: its ops go instead
        }
    }
    const auto among = [](const std::vector<std::uint64_t> &ids, std::uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    if (!dropped.empty())
        std::erase_if(edit.ops, [&](const OpRequest &o) {
            return o.payload.contains("clip") && o.payload.at("clip") == clip.id && o.payload.contains("note") &&
                   among(dropped, o.payload.at("note").get<std::uint64_t>());
        });
    if (!removed.empty())
        std::erase_if(edit.select, [&](std::uint64_t id) { return among(removed, id); });
    for (auto &o : extra)
        edit.ops.push_back(std::move(o));
    return edit;
}
} // namespace adi::ui
