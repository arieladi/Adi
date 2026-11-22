// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace adi::measure {

struct ResidualReport {
    std::uint64_t sampleCount = 0;
    double referenceRms = 0, actualRms = 0, residualRms = 0, peakResidual = 0;
    // Empty when the reference or residual has zero power.
    std::optional<double> errorSignalDb;
    bool bitExact = true;
    bool operator==(const ResidualReport&) const = default;
};

// Streams an already aligned sample pair in interleaved order. Refusal for an
// invalid block leaves the completed prefix intact. RMS is by sample count.
class ResidualAccumulator {
public:
    [[nodiscard]] bool add(std::span<const float> reference, std::span<const float> actual) noexcept;
    [[nodiscard]] bool add(std::span<const double> reference, std::span<const double> actual) noexcept;
    [[nodiscard]] std::optional<ResidualReport> finish() const noexcept;
private:
    struct Energy {
        double maxAbs = 0, normalizedPower = 0;
        void add(double value) noexcept;
        double rms(std::uint64_t count) const noexcept;
    };
    template<class Sample> bool addBlock(std::span<const Sample>, std::span<const Sample>) noexcept;

    Energy reference_, actual_, difference_;
    std::uint64_t count_ = 0;
    bool bitExact_ = true;
};

}
