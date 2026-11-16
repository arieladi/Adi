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

// Velocities drawn by a line tool (ADR-0241 d2): each note named takes its value, within 1 to 127;
// one batch for the stroke. A note the clip does not hold, or one already at its value, is left out.
inline NoteEdit drawVelocities(const engine::ClipDetails &clip, const std::vector<std::pair<std::uint64_t, int>> &values) {
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
                op("note.setVelocity", {{"clip", clip.id}, {"note", id}, {"vel", velocity}}, "Draw Velocities"));
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
inline NoteEdit makeChords(const engine::ClipDetails &clip, const std::vector<std::uint64_t> &ids, const ChordType &type) {
    using namespace note_edits_detail;
    NoteEdit edit;
    edit.refusal = closed(clip);
    if (!edit.refusal.empty())
        return edit;
    edit.select = ids;
    auto id = freeNoteId(clip);
    for (const auto *root : pick(clip, ids))
        for (const int above : type.above) {
            const int key = root->key + above;
            if (above == 0 || key >= kNoteKeys)
                continue;
            const bool there = std::any_of(clip.notes.begin(), clip.notes.end(), [&](const engine::ClipNote &n) {
                return n.key == key && n.startTicks == root->startTicks;
            });
            if (there)
                continue;
            edit.ops.push_back(op("note.insert",
                                  {{"clip", clip.id},
                                   {"note", id},
                                   {"start", root->startTicks},
                                   {"dur", root->durationTicks},
                                   {"key", key},
                                   {"vel", root->velocity}},
                                  "Insert Chord"));
            edit.select.push_back(id++);
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
            place(edit, clip.id, *n, n->startTicks, n->key, std::max(kShortestNote, *next - n->startTicks), label);
    }
    return edit;
}
} // namespace adi::ui
