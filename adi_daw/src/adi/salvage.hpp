// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/check.hpp"
#include "adi/media/publish_file.hpp"
#include <cstdint>
#include <filesystem>
#include <string>

namespace adi {

enum class SalvageFailure { None, Refused, Recovery, Validation, Publication };
struct SalvageResult {
    bool published = false;
    SalvageFailure failure = SalvageFailure::Refused;
    std::string error;
    std::string warning;
    CheckReport report;
    std::string lostAndFoundTable;
    std::int64_t lostAndFoundRows = 0;
    std::int32_t applicationId = 0;
    std::int32_t userVersion = 0;
    bool durabilityConfirmed = false;
    // Completion is not a proof of completeness: damaged/unreachable rows may
    // have no recoverable record and therefore no entry in lost-and-found.
    [[nodiscard]] int exitCode() const noexcept {
        return !published ? 2 : (report.errors || report.warnings || lostAndFoundRows ? 1 : 0);
    }
};

// CLI/owner worker only. Closed, sidecar-free input of the supported major;
// never upgrades, repairs or copies external media. Damaged-but-readable ADI
// copies may be published with findings. Unreadable/invalid output is refused.
// Hooks are the existing publication failure seam, never an overwrite policy.
[[nodiscard]] SalvageResult salvageProject(const std::filesystem::path& input,
    const std::filesystem::path& output, media::PublishHooks hooks = {});

} // namespace adi
