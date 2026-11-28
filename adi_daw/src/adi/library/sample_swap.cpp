// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/library/sample_swap.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/ui/project_document.hpp"

#include <algorithm>

namespace adi::library {

SimilarSampleSwap swapSimilarSample(ui::ProjectDocument &document, const ui::DevicePanelData &target,
                                    std::int32_t slot, std::span<const SimilarSampleCandidate> ranked,
                                    SimilarSampleStep step, std::int64_t nowUtc) {
    if (audio::onAudioThread)
        return SimilarSampleSwap::WrongThread;

    const auto view = document.view().committed();
    if (!view)
        return SimilarSampleSwap::EmptySlot;
    const auto current = std::ranges::find_if(view->sampleSlots, [&](const auto &sample) {
        return sample && sample->deviceId == target.id && sample->slot == slot;
    });
    if (current == view->sampleSlots.end() || (*current)->empty || (*current)->mediaHash.empty())
        return SimilarSampleSwap::EmptySlot;
    if (ranked.empty())
        return SimilarSampleSwap::NoCandidate;

    const auto &currentHash = (*current)->mediaHash;
    const auto position = std::ranges::find(ranked, currentHash, &SimilarSampleCandidate::contentHash);
    const bool forward = step == SimilarSampleStep::Next;
    auto index = forward ? ranked.size() - 1 : 0;
    if (position != ranked.end())
        index = static_cast<std::size_t>(position - ranked.begin());

    for (std::size_t visited = 0; visited < ranked.size(); ++visited) {
        index = forward ? (index + 1) % ranked.size() : (index == 0 ? ranked.size() - 1 : index - 1);
        const auto &candidate = ranked[index];
        if (candidate.file.empty() || candidate.contentHash.empty() || candidate.contentHash == currentHash)
            continue;

        const auto assignment = document.hotSwapIndexedSample(target, slot, candidate.file, nowUtc, candidate.contentHash);
        using Assignment = ui::ProjectDocument::SampleAssignmentResult;
        if (assignment == Assignment::SourceChanged)
            return SimilarSampleSwap::StaleCandidate;
        return assignment == Assignment::Applied ? SimilarSampleSwap::Applied : SimilarSampleSwap::AssignmentFailed;
    }
    return SimilarSampleSwap::NoCandidate;
}

} // namespace adi::library
