// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/note_timing_randomize.hpp"

#include <limits>
#include <unordered_set>
#include <utility>

namespace adi::notes {
namespace {
std::uint64_t splitMix(std::uint64_t &state) noexcept {
    auto value = (state += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::int64_t drawOffset(std::uint64_t state, std::uint64_t bound) noexcept {
    const auto span = bound * 2 + 1;
    const auto rejected = (std::uint64_t{0} - span) % span;
    auto value = splitMix(state);
    while (value < rejected)
        value = splitMix(state);
    value %= span;
    if (value >= bound)
        return static_cast<std::int64_t>(value - bound);
    return -static_cast<std::int64_t>(bound - value);
}
} // namespace

std::optional<Diff> seededTimingDiff(const Snapshot &source, std::span<const std::uint64_t> selected,
                                    std::int64_t maximumOffset, std::uint64_t seed, std::string &error) {
    error.clear();
    if (!source.ok || source.clip() <= 0) {
        error = source.error.empty() ? "invalid notes snapshot" : source.error;
        return {};
    }
    if (maximumOffset < 0) {
        error = "note timing offset bound must be nonnegative";
        return {};
    }
    std::unordered_set<std::uint64_t> remaining(selected.begin(), selected.end());
    if (remaining.contains(0) || remaining.size() != selected.size()) {
        error = "note timing selection needs distinct nonzero note IDs";
        return {};
    }
    Diff diff;
    for (const auto &record : source.records()) {
        if (remaining.empty())
            break;
        auto note = values(record, error);
        if (!note)
            return {};
        if (remaining.erase(note->id) == 0 || maximumOffset == 0)
            continue;
        const auto offset = drawOffset(seed ^ note->id, static_cast<std::uint64_t>(maximumOffset));
        if (offset == 0)
            continue;
        constexpr auto first = std::numeric_limits<std::int64_t>::min();
        constexpr auto last = std::numeric_limits<std::int64_t>::max();
        if ((offset > 0 && note->start > last - offset) ||
            (offset < 0 && note->start < first - offset)) {
            error = "note timing move would overflow a musical tick";
            return {};
        }
        note->start += offset;
        auto moved = withValues(record, *note, error);
        if (!moved)
            return {};
        diff.changed.push_back(Change{record, std::move(*moved)});
    }
    if (!remaining.empty()) {
        error = "note timing selection contains a missing note ID";
        return {};
    }
    return diff;
}

} // namespace adi::notes
