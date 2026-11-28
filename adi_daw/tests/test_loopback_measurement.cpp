// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/measure/loopback.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

namespace {
using namespace adi::measure;
int checks = 0, failures = 0;
void check(bool okay, const char* what) {
    ++checks;
    if (!okay) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

// Analytic delayed waveform, independent of the probe lookup and phase unwrapping.
std::vector<float> returned(std::uint64_t first, double delay, double gain, double noise = 0,
                            double unrelated = 0, bool pilotOnly = false) {
    constexpr std::array bins{4096, 2048, 3072, 2560, 2304, 2176, 1088, 1312, 1552, 1800, 3332, 3586, 3841};
    constexpr double turns = 2.0 * std::numbers::pi / static_cast<double>(LoopbackProbe::period);
    std::vector<float> input(LoopbackProbe::period);
    std::minstd_rand random(92741);
    std::uniform_real_distribution<double> disturbance(-noise, noise);
    const auto origin = static_cast<double>(first % LoopbackProbe::period);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double frame = origin + static_cast<double>(i);
        double sample = 0;
        const auto binsUsed = pilotOnly ? std::size_t{1} : bins.size();
        for (std::size_t bin = 0; bin < binsUsed; ++bin) {
            sample += (bin == 0 ? 0.20 : 0.01) * std::sin(turns * bins[bin] * (frame - delay));
        }
        input[i] = static_cast<float>(0.012 + gain * sample + disturbance(random) + unrelated * std::sin(turns * 1723.0 * frame));
    }
    return input;
}

void probeSignal(const LoopbackProbe& probe) {
    const std::uint64_t first = std::numeric_limits<std::uint64_t>::max() - 200;
    std::vector<float> one(LoopbackProbe::period + 3), sliced(one.size());
    probe.render(first, one);
    for (const std::size_t block : {1U, 64U, 511U, 4096U}) {
        for (std::size_t begin = 0; begin < sliced.size(); begin += block) {
            probe.render(first + begin, std::span(sliced).subspan(begin, std::min(block, sliced.size() - begin)));
        }
        check(one == sliced, "arbitrary driver slice partitions produce the exact same probe samples");
    }
    const auto peak = std::max_element(one.begin(), one.end());
    check(*peak > 0.2f && *peak < 0.32f && one[0] == one[LoopbackProbe::period],
          "the fixed-period output stays at the intended headroom");
    const auto self = probe.resolve(std::span(one).first(LoopbackProbe::period), first, 8192);
    check(self && self.samples == 0 && !self.inverted && self.coherence > 0.9999,
          "an exact digital wire resolves as zero, including at an arbitrary starting phase");
}

void latency(const LoopbackProbe& probe) {
    for (const double delay : {0.0, 0.25, 8.0, 255.5, 256.0, 256.5, 1234.25, 4096.125, 32766.75, 65530.5}) {
        const std::uint64_t first = 91119;
        for (const double gain : {0.6, -0.25}) {
            auto capture = returned(first, delay, gain);
            const auto observed = probe.resolve(capture, first, 65535);
            check(observed && std::abs(observed.samples - delay) < 0.002 && observed.inverted == (gain < 0.0),
                  "integer and sub-sample latencies resolve for normal and inverted loopback");
            check(observed && observed.coherence > 0.9999 && std::abs(observed.pilotGain - std::abs(gain)) < 0.0001,
                  "phase confidence and gain reflect the actual coherent tone energy");
        }
    }
    constexpr std::uint64_t first = 321077;
    const auto noisy = returned(first, 256.5, -0.65, 0.01);
    const auto observed = probe.resolve(noisy, first, 8192);
    check(observed && observed.inverted && std::abs(observed.samples - 256.5) < 0.03 && observed.coherence < 1.0,
          "the solver measures a noisy inverted fractional loopback");
    const auto boundary = probe.resolve(returned(first, 256, 0.4), first, 256);
    check(boundary && std::abs(boundary.samples - 256) < 0.002, "the stated search ceiling admits an exact match");
    check(probe.resolve(returned(first, 256, 0.4), first, 220).status == LoopbackStatus::OutsideRange,
          "a coherent latency beyond the stated physical range is refused");
}

void refusals(const LoopbackProbe& probe) {
    std::vector<float> input(LoopbackProbe::period, 0.04f);
    check(probe.resolve(input, 0, 8192).status == LoopbackStatus::SignalTooLow,
          "DC with no returned probe produces an explicit low signal");
    check(probe.resolve(std::span(input).first(27), 0, 8192).status == LoopbackStatus::InvalidInput &&
              probe.resolve(input, 0, LoopbackProbe::period).status == LoopbackStatus::InvalidInput,
          "the solver requires a full period and an unambiguous physical search range");
    input[150] = std::numeric_limits<float>::infinity();
    check(probe.resolve(input, 0, 8192).status == LoopbackStatus::InvalidInput,
          "non-finite captured samples cannot produce a result");
    const auto unrelated = returned(32768, 256, 0.7, 0, 0.35);
    check(probe.resolve(unrelated, 32768, 8192).status == LoopbackStatus::Incoherent,
          "an uncorrelated louder tone cannot produce a confident result");
    const auto pilotOnly = returned(48000, 256, 0.7, 0, 0, true);
    check(probe.resolve(pilotOnly, 48000, 8192).status == LoopbackStatus::Incoherent,
          "an otherwise strong pilot without the range-disambiguating tones is refused");
}
}

int main() {
    const LoopbackProbe probe;
    probeSignal(probe);
    latency(probe);
    refusals(probe);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
