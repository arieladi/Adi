// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace adi::ui {
class ProjectDocument;
struct DevicePanelData;
}

namespace adi::library {

struct SimilarSampleCandidate {
    std::filesystem::path file;
    std::string contentHash;
};

enum class SimilarSampleStep { Next, Previous };
enum class SimilarSampleSwap { Applied, WrongThread, EmptySlot, NoCandidate, StaleCandidate, AssignmentFailed };

// The owner supplies alternatives ranked against the retained reference. Use one
// row per content hash. The current slot's hash selects the position in that
// order, including after a refreshed listing. With no current match, start at
// the first or last alternative. One call makes one ordinary slot assignment.
[[nodiscard]] SimilarSampleSwap swapSimilarSample(ui::ProjectDocument &, const ui::DevicePanelData &,
                                                   std::int32_t slot, std::span<const SimilarSampleCandidate>,
                                                   SimilarSampleStep, std::int64_t nowUtc);

} // namespace adi::library
