// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/library/sample_descriptors.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/dsp/masking.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <numeric>
#include <vector>

namespace {
int failures = 0;

void check(bool result, const char* name) {
    if (!result) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    }
}

std::vector<float> sine(std::uint32_t rate, std::size_t frames, double hz) {
    std::vector<float> pcm(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto angle = 2 * std::numbers::pi * hz * static_cast<double>(frame) / rate;
        pcm[frame] = static_cast<float>(0.5 * std::sin(angle));
    }
    return pcm;
}

void spectralAndLevel() {
    constexpr std::uint32_t rate = 48000;
    constexpr double kickHz = 375.0;
    constexpr double hatHz = 6000.0;
    auto low = sine(rate, rate, kickHz);
    auto high = sine(rate, rate, hatHz);
    adi::library::SampleDescriptorAnalyzer kick(rate, 1), hat(rate, 1);
    check(kick.append(low), "read bass tone");
    check(hat.append(high), "read high tone");
    constexpr std::array<std::uint64_t, 3> onsets{0, rate / 8, 7 * rate / 8};
    const auto bass = kick.finish(onsets);
    const auto treble = hat.finish({});
    check(bass.has_value() && treble.has_value(), "produce tone measurements");
    if (!bass || !treble) return;
    check(std::abs(bass->brightnessHz - kickHz) < 1.0 &&
          std::abs(treble->brightnessHz - hatHz) < 1.0, "centroids within one Hz");
    check(bass->spectralEnergy[2] > .99f && treble->spectralEnergy[6] > .99f, "known spectral bands");
    check(std::abs(bass->rmsDbfs - 10.0 * std::log10(0.125)) < 0.001, "known tone RMS in dBFS");
    check(bass->hitDensityHz == 3.0f && treble->hitDensityHz == 0.0f, "known hits per second");
    check(bass->onsetDensity[0] == 1.0f / 3.0f && bass->onsetDensity[1] == 1.0f / 3.0f &&
          bass->onsetDensity[7] == 1.0f / 3.0f && bass->onsetDensity[2] == 0.0f, "relative hit grid");
    check(std::all_of(bass->envelopeEnergy.begin(), bass->envelopeEnergy.end(),
          [](float share) { return std::abs(share - 0.25f) < 0.001f; }), "steady envelope shares");

    const adi::dsp::MaskingBands bands(1025, 44100);
    check(bass->maskingEnergy.size() == static_cast<std::size_t>(bands.count()), "existing ERB layout");
    check(std::abs(std::accumulate(bass->maskingEnergy.begin(), bass->maskingEnergy.end(), 0.0f) - 1) < .001f,
          "conserved masking energy");
    const auto strongest = std::max_element(bass->maskingEnergy.begin(), bass->maskingEnergy.end());
    const auto band = static_cast<std::int32_t>(strongest - bass->maskingEnergy.begin());
    check(std::abs(bands.centreHz(band) - kickHz) < 40, "energy falls in the matching ERB band");
    std::printf("Measured brightness %.2f Hz and %.2f Hz, level %.4f dBFS, density %.2f hits/s\n",
                static_cast<double>(bass->brightnessHz), static_cast<double>(treble->brightnessHz),
                static_cast<double>(bass->rmsDbfs), static_cast<double>(bass->hitDensityHz));
}

void streamingAndSilence() {
    constexpr std::uint32_t rate = 44100;
    constexpr std::size_t frames = 8007;
    auto mono = sine(rate, frames, 900.0);
    std::fill(mono.begin() + 1700, mono.end(), 0.0f);
    std::vector<float> stereo(mono.size() * 2);
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        stereo[frame * 2] = mono[frame];
        stereo[frame * 2 + 1] = -mono[frame];
    }
    adi::library::SampleDescriptorAnalyzer whole(rate, 1), chunked(rate, 2);
    check(whole.append(mono), "read one large block");
    for (std::size_t sample = 0; sample < stereo.size();) {
        const auto count = std::min<std::size_t>(258, stereo.size() - sample);
        check(chunked.append(std::span(stereo).subspan(sample, count)), "read arbitrary stereo chunks");
        sample += count;
    }
    const auto a = whole.finish({});
    check(!chunked.finish(std::array<std::uint64_t, 1>{frames}), "out of range onset rejected");
    const auto b = chunked.finish({});
    check(a && b && std::abs(a->brightnessHz - b->brightnessHz) < 0.001f &&
          a->brightnessHz > 800.0f && a->rmsDbfs == b->rmsDbfs,
          "stereo phase and decoder chunk boundaries preserve spectrum and level");
    check(a && b && a->envelopeEnergy == b->envelopeEnergy && a->envelopeEnergy[0] > .999f,
          "energy localized in first time quarter");
    check(!chunked.append(stereo) && !chunked.finish({}), "finished analyzer rejects updates");

    adi::library::SampleDescriptorAnalyzer quiet(rate, 1);
    const std::array<float, 3> silence{0.0f, std::numeric_limits<float>::infinity(),
                                          std::numeric_limits<float>::quiet_NaN()};
    check(quiet.append(silence), "read source with nonfinite samples");
    const auto measured = quiet.finish({});
    check(measured && measured->brightnessHz == 0.0f && measured->rmsDbfs == -200.0f &&
          measured->hitDensityHz == 0.0f, "silence has finite defined results");
}

void edgeImpulses() {
    // A hit at either end of a file must reach a spectrum window, including
    // across a silent hop and the final padded window of an odd-length file.
    std::array<float, 8193> pcm{};
    for (const auto position : {std::size_t{0}, pcm.size() - 1}) {
        pcm[position] = 1.0f;
        adi::library::SampleDescriptorAnalyzer analyzer(48000, 1);
        check(analyzer.append(pcm), "read file edge impulse");
        const auto result = analyzer.finish({});
        check(result && result->brightnessHz > 9000.0f && result->brightnessHz < 11000.0f,
              "broadband spectrum survives either file edge");
        pcm[position] = 0.0f;
    }

    adi::library::SampleDescriptorAnalyzer lowRate(16, 1);
    check(lowRate.append(std::array<float, 2>{1.0f, -1.0f}), "read low sample rate below audible range");
    const auto result = lowRate.finish({});
    check(result && result->brightnessHz == 0.0f && result->rmsDbfs == 0.0f, "no audible bands, valid level");
}

void callbackRefusal() {
    const std::array<float, 1> pcm{1.0f};
    adi::library::SampleDescriptorAnalyzer worker(44100, 1);
    {
        const adi::audio::CallbackScope callback;
        adi::library::SampleDescriptorAnalyzer refused(44100, 1);
        check(!refused.append(pcm) && !refused.finish({}), "callback constructor refuses work");
        check(!worker.append(pcm) && !worker.finish({}), "callback refuses existing analyzer work");
    }
    check(worker.append(pcm) && worker.finish({}).has_value(), "callback refusal preserves worker state");
}
} // namespace

int main() {
    spectralAndLevel();
    streamingAndSilence();
    edgeImpulses();
    callbackRefusal();
    return failures != 0;
}
