// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace adi::audio {

enum class SampleKind : std::uint8_t { Unknown, OneShot, Loop };
enum class SampleKindSource : std::uint8_t { Detection, Acid, FileTag, User };

struct SampleKindHint {
    SampleKind kind = SampleKind::Unknown;
    std::optional<double> bpm;
};

struct SampleKindHints {
    SampleKindHint user;
    SampleKindHint fileTag;
    SampleKindHint acid;
};

struct SampleClassification {
    SampleKind kind = SampleKind::Unknown;
    SampleKindSource source = SampleKindSource::Detection;
    std::optional<double> bpm;
    std::optional<unsigned int> bars; // 4/4, present only for compatible loop timing
};

// Provisional short/long boundary from ADR-0214 pending the Live 12.4 reading.
inline constexpr unsigned int kShortSampleSeconds = 60;
inline constexpr std::array kShortLoopBarCounts{1u, 2u, 4u, 8u, 16u};
inline constexpr unsigned int kShortLoopBeatsPerBar = 4;

// Worker-only classification of a full source. Feed the ordered source-relative
// onset frames already computed for the file's analysis. Kind and BPM each use
// user, file tag, ACID, then detection precedence. The returned source is the
// kind's provenance. Unknown covers silence, unproven rhythmic structure and
// invalid input. No allocation or I/O. The callback returns an empty result.
[[nodiscard]] SampleClassification classifySample(std::span<const std::uint64_t> onsets,
    std::uint64_t frames, std::uint32_t rate, const SampleKindHints& hints = {}) noexcept;

} // namespace adi::audio
