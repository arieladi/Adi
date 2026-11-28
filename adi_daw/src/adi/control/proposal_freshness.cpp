// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/proposal_freshness.hpp"

#include <algorithm>

namespace adi::control {

std::string_view toString(Freshness freshness) {
    switch (freshness) {
        case Freshness::Current: return "current";
        case Freshness::Behind: return "behind";
        case Freshness::Ahead: return "ahead";
        case Freshness::Elsewhere: return "elsewhere";
        case Freshness::Unknown: return "unknown";
    }
    return "unknown";
}

ProposalFreshness proposalFreshness(const History::Timeline& timeline, std::optional<std::int64_t> baseHead) {
    if (!timeline.error.empty()) return {Freshness::Unknown, 0};
    if (timeline.headSeq == baseHead) return {Freshness::Current, 0};
    const auto& steps = timeline.steps;

    // On the undo path: some applied step was made on top of the base. The
    // applied steps form one line, and sequence numbers grow along a line, so
    // the steps after the base are the applied ones that start past it.
    const bool onUndoPath = std::any_of(steps.begin(), steps.end(), [&](const History::TimelineStep& s) {
        return s.applied && s.parentSeq == baseHead;
    });
    if (onUndoPath) {
        const auto after = std::count_if(steps.begin(), steps.end(), [&](const History::TimelineStep& s) {
            return s.applied && (!baseHead || s.step.firstSeq > *baseHead);
        });
        return {Freshness::Behind, static_cast<std::size_t>(after)};
    }

    // On the redo line Ctrl-Y follows: the user undid past the base.
    if (baseHead) {
        const auto redo = [](const History::TimelineStep& s) { return s.onCurrentLine && !s.applied; };
        const bool onRedoLine = std::any_of(steps.begin(), steps.end(), [&](const History::TimelineStep& s) {
            return redo(s) && s.step.lastSeq == *baseHead;
        });
        if (onRedoLine) {
            const auto upTo = std::count_if(steps.begin(), steps.end(), [&](const History::TimelineStep& s) {
                return redo(s) && s.step.lastSeq <= *baseHead;
            });
            return {Freshness::Ahead, static_cast<std::size_t>(upTo)};
        }
    }
    return {Freshness::Elsewhere, 0};
}

}  // namespace adi::control
