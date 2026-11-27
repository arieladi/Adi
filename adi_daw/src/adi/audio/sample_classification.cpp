// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/sample_classification.hpp"

#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>

namespace adi::audio {
namespace {

constexpr double kMinDetectedBpm = 60.0;
constexpr double kMaxDetectedBpm = 200.0;
constexpr double kGridToleranceBeats = 0.06;
constexpr double kMinAlignedOnsets = 0.8;
constexpr unsigned int kMinLoopOnsets = 3;

bool validOnsets(std::span<const std::uint64_t> onsets, std::uint64_t frames) {
    return onsets.empty() || (onsets.back() < frames &&
        std::adjacent_find(onsets.begin(), onsets.end(), std::greater_equal<>()) == onsets.end());
}

std::optional<unsigned int> compatibleBars(double seconds, double bpm) {
    const auto durationBeats = seconds * bpm / 60.0;
    for (auto bars : kShortLoopBarCounts) {
        const auto beats = bars * kShortLoopBeatsPerBar;
        if (std::abs(durationBeats - beats) <= std::max(0.08, beats * 0.005))
            return bars;
    }
    return std::nullopt;
}

struct LoopTiming {
    unsigned int bars = 0;
    double bpm = 0.0;
};

LoopTiming measureLoop(std::span<const std::uint64_t> onsets, double frames, double seconds,
                       std::optional<unsigned int> taggedBars) {
    if (onsets.size() < kMinLoopOnsets)
        return {};
    LoopTiming best;
    double bestScore = -1.0;
    for (const auto bars : kShortLoopBarCounts) {
        const auto beats = bars * kShortLoopBeatsPerBar;
        const auto bpm = 60.0 * beats / seconds;
        if (bpm < kMinDetectedBpm || bpm > kMaxDetectedBpm)
            continue;

        const auto period = frames / beats;
        const auto begin = static_cast<double>(onsets.front());
        const auto end = static_cast<double>(onsets.back());
        if (begin > period || frames - end > 2.0 * period || end - begin < 2.0 * period - kGridToleranceBeats * period)
            continue;

        std::size_t aligned = 0;
        double gapScore = 0.0;
        double last = begin;
        for (const auto frame : onsets) {
            const auto position = static_cast<double>(frame);
            const auto quarter = position / period;
            const auto nearestSixteenth = std::round(quarter * 4.0) / 4.0;
            if (std::abs(quarter - nearestSixteenth) <= kGridToleranceBeats)
                ++aligned;

            // Compare evidence at the quarter-note pulse when more than one
            // octave fits the sixteenth-note grid equally well.
            const auto gap = (position - last) / period;
            gapScore += std::min(gap, 1.0 / std::max(1.0, gap));
            last = position;
        }
        const auto alignment = static_cast<double>(aligned) / static_cast<double>(onsets.size());
        if (alignment < kMinAlignedOnsets)
            continue;
        if (taggedBars == bars)
            return {bars, bpm};

        const auto score = alignment + gapScore / static_cast<double>(onsets.size() - 1);
        if (score > bestScore) {
            best = {bars, bpm};
            bestScore = score;
        }
    }
    return best;
}

} // namespace

SampleClassification classifySample(std::span<const std::uint64_t> onsets, std::uint64_t frames,
                                    std::uint32_t rate, const SampleKindHints& hints) noexcept {
    if (onAudioThread || frames == 0 || rate == 0 || rate > 768000 || !validOnsets(onsets, frames))
        return {};

    SampleClassification result;
    const std::array sources{
        std::pair{&hints.user, SampleKindSource::User},
        std::pair{&hints.fileTag, SampleKindSource::FileTag},
        std::pair{&hints.acid, SampleKindSource::Acid}
    };
    for (const auto& [hint, source] : sources) {
        if (result.kind == SampleKind::Unknown &&
            (hint->kind == SampleKind::OneShot || hint->kind == SampleKind::Loop)) {
            result.kind = hint->kind;
            result.source = source;
        }
        if (!result.bpm && hint->bpm && std::isfinite(*hint->bpm) && *hint->bpm > 0.0 && *hint->bpm < 1000.0)
            result.bpm = hint->bpm;
    }
    if (result.kind == SampleKind::OneShot)
        return result;

    const auto frameCount = static_cast<double>(frames);
    const auto seconds = frameCount / rate;
    const auto taggedBars = result.bpm ? compatibleBars(seconds, *result.bpm) : std::nullopt;
    if (result.kind == SampleKind::Loop && result.bpm) {
        result.bars = taggedBars;
        return result;
    }
    if (frames > static_cast<std::uint64_t>(kShortSampleSeconds) * rate)
        return result;

    if (onsets.size() == 1 && result.kind == SampleKind::Unknown) {
        result.kind = SampleKind::OneShot;
        return result;
    }
    const auto timing = measureLoop(onsets, frameCount, seconds, taggedBars);
    if (timing.bars == 0)
        return result;

    result.kind = SampleKind::Loop;
    if (!result.bpm) {
        result.bpm = timing.bpm;
        result.bars = timing.bars;
    } else if (taggedBars == timing.bars) {
        result.bars = timing.bars;
    }
    return result;
}

} // namespace adi::audio
