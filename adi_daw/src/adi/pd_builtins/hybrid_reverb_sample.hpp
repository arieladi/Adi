// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/hybrid_reverb.hpp"
#include "juce/pd_samples.hpp"
namespace adi::device {
struct PdHybridReverbSample final : PdSampleBuffer {
    dsp::HybridImpulse impulse;
};
inline std::unique_ptr<PdSampleBuffer>
prepareHybridReverbSample(std::unique_ptr<PdSampleBuffer> sample,
                          dsp::HybridImpulseOptions options = {}) {
    if (!sample || sample->channels < 1 || sample->channels > 2 || sample->frames <= 0 ||
        sample->interleaved.size() !=
            static_cast<std::size_t>(sample->frames) * static_cast<std::size_t>(sample->channels))
        return {};
    auto result = std::make_unique<PdHybridReverbSample>();
    result->impulse = dsp::prepareHybridImpulse(sample->interleaved, sample->channels,
                                                sample->sampleRate, options);
    result->id = sample->id;
    result->channels = sample->channels;
    result->frames = sample->frames;
    result->sampleRate = sample->sampleRate;
    result->blake3 = std::move(sample->blake3);
    result->interleaved = std::move(sample->interleaved);
    return result;
}
} // namespace adi::device
