// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/measure/residual.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace adi::measure {

void ResidualAccumulator::Energy::add(double value) noexcept {
    const double absolute = std::abs(value);
    if (absolute == 0) return;
    if (absolute > maxAbs) {
        const double ratio = maxAbs / absolute;
        normalizedPower = 1.0 + normalizedPower * ratio * ratio;
        maxAbs = absolute;
    } else {
        const double ratio = absolute / maxAbs;
        normalizedPower += ratio * ratio;
    }
}

double ResidualAccumulator::Energy::rms(std::uint64_t count) const noexcept {
    const auto power = normalizedPower / static_cast<double>(count);
    return maxAbs * std::sqrt(std::min(1.0, power));
}

template<class Sample> bool ResidualAccumulator::addBlock(std::span<const Sample> reference,
                                                         std::span<const Sample> actual) noexcept {
    if (reference.size() != actual.size() || reference.size() > std::numeric_limits<std::uint64_t>::max() - count_) {
        return false;
    }
    if (reference.empty()) return true;
    auto referenceEnergy = reference_;
    auto actualEnergy = actual_;
    auto differenceEnergy = difference_;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const double a = reference[i], b = actual[i], error = a - b;
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(error)) return false;
        referenceEnergy.add(a);
        actualEnergy.add(b);
        differenceEnergy.add(error);
    }
    reference_ = referenceEnergy;
    actual_ = actualEnergy;
    difference_ = differenceEnergy;
    count_ += reference.size();
    bitExact_ = bitExact_ && std::memcmp(reference.data(), actual.data(), reference.size_bytes()) == 0;
    return true;
}

bool ResidualAccumulator::add(std::span<const float> reference, std::span<const float> actual) noexcept {
    return addBlock(reference, actual);
}

bool ResidualAccumulator::add(std::span<const double> reference, std::span<const double> actual) noexcept {
    return addBlock(reference, actual);
}

std::optional<ResidualReport> ResidualAccumulator::finish() const noexcept {
    if (count_ == 0) return std::nullopt;
    ResidualReport report{count_, reference_.rms(count_), actual_.rms(count_), difference_.rms(count_),
                          difference_.maxAbs, {}, bitExact_};
    if (reference_.maxAbs > 0 && difference_.maxAbs > 0) {
        // Counts cancel in the ratio. Keeping each scale in log space retains
        // an error that is tiny compared with the reference, or vice versa.
        report.errorSignalDb = 20.0 * (std::log10(difference_.maxAbs) - std::log10(reference_.maxAbs)) +
            10.0 * (std::log10(difference_.normalizedPower) - std::log10(reference_.normalizedPower));
    }
    return report;
}

}
