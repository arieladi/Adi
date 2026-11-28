// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/library/sample_analysis.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace adi::library {

struct SampleQueryRange {
    double minimum = -std::numeric_limits<double>::infinity();
    double maximum = std::numeric_limits<double>::infinity();
};

enum class SampleQueryRank { LibraryOrder, Similar, Fits };

struct SampleQuery {
    std::optional<audio::SampleKind> kind;
    std::optional<SampleQueryRange> lengthSeconds;
    std::optional<SampleKey> key;
    std::optional<SampleQueryRange> tempoBpm;
    std::optional<SampleQueryRange> brightnessHz;
    std::optional<SampleQueryRange> hitsPerSecond;
    SampleQueryRank rank = SampleQueryRank::LibraryOrder;
    const SampleAnalysis *reference = nullptr;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
};

struct SampleQueryMatch {
    std::size_t row = 0;
    double score = 0.0;
};

// Refine the ordered sample candidates emitted by the shared library word query.
// The span has one analysis pointer per candidate, with null for an unmeasured
// file. Measured filters are inclusive, intersect, and require known values.
// In ranked queries the reference must be measured and is excluded from results;
// equal scores preserve candidate order. Returned row offsets index this span.
// Off the audio callback. A callback call refuses without changing results.
void querySamples(std::span<const SampleAnalysis *const> wordMatches, const SampleQuery &,
                  std::vector<SampleQueryMatch> &results);
void querySamples(std::span<const std::shared_ptr<const SampleAnalysis>> wordMatches, const SampleQuery &,
                  std::vector<SampleQueryMatch> &results);

} // namespace adi::library
