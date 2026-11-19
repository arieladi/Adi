// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Find and Select Notes (Live 12 manual 10.5.3): the note filters, without a window.
//
// Eight filters, each with its own settings and an Invert switch. A filter that is applied narrows
// the selection; several applied together select the notes that pass all of them. The window
// keeps one NoteFilters for the editor and selects what `findNotes` returns.
#pragma once
#include "adi/engine/clip_details.hpp"
#include "adi/textproj.hpp"
#include "adi/ui/scales.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace adi::ui {

enum class NoteFilter { Pitch, Time, Chance, Condition, Count, Duration, Scale, Velocity };
inline constexpr std::size_t kNoteFilterCount = 8;
inline constexpr std::array<const char *, kNoteFilterCount> kNoteFilterNames = {"Pitch", "Time",     "Chance", "Condition",
                                                                             "Count", "Duration", "Scale",  "Velocity"};

struct NoteFilters {
    // Which filters are applied, and which of them select what they do not find.
    std::array<bool, kNoteFilterCount> applied{}, invert{};
    // Pitch: the pitches wanted, in every octave; bit 0 is C.
    int pitches = 1;
    // Time: a range from `timeStart`, `timeLength` long, in the clip's own ticks; with `timeRepeat`
    // over 0 the range comes again every `timeRepeat`.
    std::int64_t timeStart = 0, timeLength = textproj::kPPQ, timeRepeat = 0;
    // Chance: a probability range, in per cent.
    int chanceMin = 0, chanceMax = 100;
    // Condition: any of these that is on.
    bool conditionActive = true, conditionChance = false, conditionDeviation = false;
    // Count: every `every`th note or chord in time, the first being number `offset` from 0. With
    // `quantized`, the notes of one grid step count as one, and the steps are counted instead.
    int every = 2, offset = 0;
    bool quantized = false;
    // Duration: a range of lengths, in ticks.
    std::int64_t durationMin = 0, durationMax = textproj::kPPQ;
    // Scale: the clip's own, or this one.
    bool useClipScale = true;
    int scaleRoot = 0;
    std::int64_t scaleMask = 2741;
    // Velocity: a range.
    int velocityMin = 1, velocityMax = 127;

    bool any() const noexcept {
        return std::any_of(applied.begin(), applied.end(), [](bool on) { return on; });
    }
    void clear() noexcept { applied.fill(false); }
};

namespace note_filters_detail {
// Each note's place in the count: notes that begin together are one chord and share a number; with
// a grid, the notes of one grid step share a number and empty steps are counted too.
inline std::vector<std::int64_t> countOf(const engine::ClipDetails &clip, bool quantized, std::int64_t grid, std::int64_t origin) {
    std::vector<std::int64_t> number(clip.notes.size(), 0);
    if (quantized && grid > 0) {
        for (std::size_t i = 0; i < clip.notes.size(); ++i) {
            const auto rel = clip.notes[i].startTicks - origin;
            number[i] = rel >= 0 ? rel / grid : -((-rel + grid - 1) / grid);
        }
        // Counted from the first step that holds a note.
        if (!number.empty()) {
            const auto first = *std::min_element(number.begin(), number.end());
            for (auto &n : number)
                n -= first;
        }
        return number;
    }
    std::vector<std::int64_t> starts;
    for (const auto &n : clip.notes)
        starts.push_back(n.startTicks);
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    for (std::size_t i = 0; i < clip.notes.size(); ++i)
        number[i] = std::lower_bound(starts.begin(), starts.end(), clip.notes[i].startTicks) - starts.begin();
    return number;
}
} // namespace note_filters_detail

// Whether one filter finds a note, before its Invert switch. `count` is the note's number from
// `countOf`.
inline bool noteFilterFinds(NoteFilter filter, const NoteFilters &f, const engine::ClipDetails &clip, const engine::ClipNote &n,
                            std::int64_t count) noexcept {
    switch (filter) {
    case NoteFilter::Pitch:
        return ((f.pitches >> (n.key % 12)) & 1) != 0;
    case NoteFilter::Time: {
        if (f.timeLength <= 0 || n.startTicks < f.timeStart)
            return false;
        const auto after = n.startTicks - f.timeStart;
        return (f.timeRepeat > 0 ? after % f.timeRepeat : after) < f.timeLength;
    }
    case NoteFilter::Chance: {
        const int percent = (static_cast<int>(n.probability) + 50) / 100;
        return percent >= std::min(f.chanceMin, f.chanceMax) && percent <= std::max(f.chanceMin, f.chanceMax);
    }
    case NoteFilter::Condition:
        // Velocity deviation is not a note's field yet: no note has any.
        return (f.conditionActive && n.active) || (f.conditionChance && n.probability < 10000);
    case NoteFilter::Count: {
        const auto every = static_cast<std::int64_t>(std::max(1, f.every));
        const auto from = count - f.offset;
        return from >= 0 && from % every == 0;
    }
    case NoteFilter::Duration:
        return n.durationTicks >= std::min(f.durationMin, f.durationMax) && n.durationTicks <= std::max(f.durationMin, f.durationMax);
    case NoteFilter::Scale:
        return f.useClipScale ? inScale(n.key, static_cast<int>(clip.scale.root), clip.scale.mask) : inScale(n.key, f.scaleRoot, f.scaleMask);
    case NoteFilter::Velocity:
        return n.velocity >= std::min(f.velocityMin, f.velocityMax) && n.velocity <= std::max(f.velocityMin, f.velocityMax);
    }
    return false;
}

// The notes every applied filter finds (or, inverted, does not find), in the clip's order. None
// applied: none. `grid` and `origin` are the editing grid's step and one of its lines, in the
// clip's own ticks, for the Count filter's Quantized switch.
inline std::vector<std::uint64_t> findNotes(const engine::ClipDetails &clip, const NoteFilters &f, std::int64_t grid = 0,
                                            std::int64_t origin = 0) {
    std::vector<std::uint64_t> found;
    if (!f.any())
        return found;
    const auto counts = note_filters_detail::countOf(clip, f.quantized, grid, origin);
    for (std::size_t i = 0; i < clip.notes.size(); ++i) {
        bool keep = true;
        for (std::size_t k = 0; k < kNoteFilterCount && keep; ++k)
            if (f.applied[k])
                keep = noteFilterFinds(static_cast<NoteFilter>(k), f, clip, clip.notes[i], counts[i]) != f.invert[k];
        if (keep)
            found.push_back(clip.notes[i].id);
    }
    return found;
}

} // namespace adi::ui
