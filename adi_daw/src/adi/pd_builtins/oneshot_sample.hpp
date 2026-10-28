// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/oneshot.hpp"
#include "juce/pd_samples.hpp"

namespace adi::device {
// Prepared off-thread before publishing through PdSampleSlots. The callback
// borrows prepared.get(); it never copies or destroys this shared ownership.
// The original audio/hash remain available to the host's persistence path.
struct PdOneShotSample final : PdSampleBuffer {
    std::shared_ptr<const OneShot::PreparedSample> prepared;
};

[[nodiscard]] inline std::unique_ptr<PdSampleBuffer> prepareOneShotSample(
    std::unique_ptr<PdSampleBuffer> source, std::span<const std::size_t> slices = {}) {
    if (!source || source->id <= 0 || source->channels < 1 || source->channels > 2 ||
        source->frames <= 0 ||
        source->interleaved.size() % static_cast<std::size_t>(source->channels) != 0 ||
        source->interleaved.size() / static_cast<std::size_t>(source->channels) !=
            static_cast<std::uint64_t>(source->frames))
        return nullptr;
    auto prepared = OneShot::prepareSample(source->interleaved, source->channels,
                                           source->sampleRate, slices);
    if (!prepared) return nullptr;
    auto result = std::make_unique<PdOneShotSample>();
    result->prepared = std::move(prepared);
    result->id = source->id;
    result->channels = source->channels;
    result->frames = source->frames;
    result->sampleRate = source->sampleRate;
    result->blake3 = std::move(source->blake3);
    result->interleaved = std::move(source->interleaved);
    return result;
}

// Publish this empty snapshot to clear a slot. Slots reject nullptr; a real
// empty publication receives a new generation and retires the previous sample.
[[nodiscard]] inline std::unique_ptr<PdSampleBuffer> clearOneShotSample(std::int32_t id) {
    if (id <= 0) return nullptr;
    auto result = std::make_unique<PdOneShotSample>();
    result->id = id;
    return result;
}
} // namespace adi::device
