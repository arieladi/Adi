// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
namespace adi::notes {
struct QuantizeSettings {
    std::int64_t gridTicks = 0, originTicks = 0;
    // Fixed-point percent: 10000 means 100.00%. Swing delays odd divisions by
    // up to half a division. Integers make the tick arithmetic ABI independent.
    int strength = 10000, swing = 0;
    bool starts = true, ends = false;
};
OpRequest quantizeRequest(std::int64_t clip, std::span<const std::uint64_t> ids, const QuantizeSettings &);
OpRequest transposeRequest(std::int64_t clip, std::span<const std::uint64_t> ids, int semitones);
std::span<const OpDescriptor> transformOps();
} // namespace adi::notes
