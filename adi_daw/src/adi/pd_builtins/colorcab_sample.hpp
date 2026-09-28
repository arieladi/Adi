// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "juce/pd_samples.hpp"
#include "adi/dsp/colorcab.hpp"

namespace adi::device {
// A prepared sample is published through PdSampleSlots itself. Sequenced has
// a virtual destructor, so retirement through PdSampleBuffer is safe.
struct PdColorCabSample final : PdSampleBuffer {
    dsp::ColorCabDesign design;
    dsp::ColorCabOptions options;
};
// Message/worker thread only. Input must already be decoded at the engine rate.
// Keep both the source hash and derived profile, for persistence without media.
inline std::unique_ptr<PdSampleBuffer> prepareColorCabSample(
    std::unique_ptr<PdSampleBuffer> sample, dsp::ColorCabOptions options = {}) {
    if (!sample || sample->channels <= 0 || sample->frames <= 0 ||
        sample->interleaved.size() / static_cast<std::size_t>(sample->channels)
            != static_cast<std::size_t>(sample->frames)) return nullptr;
    std::vector<float> mono(static_cast<std::size_t>(sample->frames));
    for (std::size_t f = 0; f < mono.size(); ++f) {
        double sum = 0;
        for (std::int32_t c = 0; c < sample->channels; ++c)
            sum += sample->interleaved[f * static_cast<std::size_t>(sample->channels)
                                      + static_cast<std::size_t>(c)];
        mono[f] = static_cast<float>(sum / sample->channels);
    }
    auto result = std::make_unique<PdColorCabSample>();
    result->design = dsp::buildColorCab(mono, sample->sampleRate, options);
    result->options = options;
    result->id = sample->id; result->channels = sample->channels;
    result->frames = sample->frames; result->sampleRate = sample->sampleRate;
    result->blake3 = std::move(sample->blake3);
    result->interleaved = std::move(sample->interleaved);
    return result;
}
} // namespace adi::device
