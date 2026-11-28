// SPDX-License-Identifier: GPL-2.0-or-later
// The MTDM tone ladder and ambiguity resolution follow Fons Adriaensen's
// Ardour MTDM, copyright 2003–2012 Fons Adriaensen, 2009–2012 David Robillard,
// 2009–2015 Paul Davis and 2016 Robin Gareus. Reference at 0f45032f6dbe636b8cfa21fd274d4be2c48d.
// https://github.com/Ardour/ardour/blob/0f45032f6dbe636b8cfa21fd274d4be2c48d/libs/ardour/mtdm.cc
#include "adi/measure/loopback.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <optional>

namespace adi::measure {
namespace {
constexpr std::array<std::size_t, 13> bins{4096, 2048, 3072, 2560, 2304, 2176, 1088,
                                        1312, 1552, 1800, 3332, 3586, 3841};
constexpr auto mask = LoopbackProbe::period - 1;
constexpr double fullPeriod = static_cast<double>(LoopbackProbe::period);
constexpr double tau = 2.0 * std::numbers::pi;
constexpr double pilotAmplitude = 0.20, ladderAmplitude = 0.01;

struct Candidate {
    double delay, worstError;
    bool inverted;
};

std::optional<Candidate> unwrap(const std::array<double, bins.size()>& phases, bool inverted) noexcept {
    const double polarity = inverted ? 0.5 : 0.0;
    const double pilotBin = static_cast<double>(bins.front());
    double delay = std::remainder(phases.front() + polarity, 1.0);
    double stride = 1.0, worstError = 0.0;
    for (std::size_t i = 1; i < bins.size(); ++i) {
        // Each tone resolves the next ambiguity bit of the pilot's period.
        const double expected = delay * static_cast<double>(bins[i]) / pilotBin;
        const double parity = std::remainder(2.0 * (phases[i] + polarity - expected), 2.0);
        const double nearest = std::round(parity);
        worstError = std::max(worstError, std::abs(parity - nearest));
        if (worstError > 0.35) return std::nullopt;
        if (nearest != 0) delay += stride;
        stride *= 2.0;
    }
    delay *= fullPeriod / pilotBin;
    if (std::abs(delay) < 1e-5) delay = 0;
    if (delay < 0) delay += fullPeriod;
    return Candidate{delay, worstError, inverted};
}
}

LoopbackProbe::LoopbackProbe() : sine_(period), output_(period) {
    for (std::size_t frame = 0; frame < period; ++frame) {
        sine_[frame] = std::sin(tau * static_cast<double>(frame) / fullPeriod);
    }
    for (std::size_t frame = 0; frame < period; ++frame) {
        double sample = pilotAmplitude * sine_[(frame * bins.front()) & mask];
        for (std::size_t tone = 1; tone < bins.size(); ++tone) {
            sample += ladderAmplitude * sine_[(frame * bins[tone]) & mask];
        }
        output_[frame] = static_cast<float>(sample);
    }
}

void LoopbackProbe::render(std::uint64_t firstFrame, std::span<float> output) const noexcept {
    auto phase = static_cast<std::size_t>(firstFrame % period);
    for (auto& sample : output) {
        sample = output_[phase];
        phase = (phase + 1) & mask;
    }
}

LoopbackResult LoopbackProbe::resolve(std::span<const float> input, std::uint64_t firstFrame,
                                     double maximumDelay) const noexcept {
    LoopbackResult result;
    if (input.size() != period || !(maximumDelay >= 0.0 && maximumDelay < fullPeriod)) return result;
    double sum = 0, energy = 0;
    for (const double sample : input) {
        if (!std::isfinite(sample)) return result;
        sum += sample;
        energy += sample * sample;
    }
    const double mean = sum / fullPeriod;
    const double signalPower = energy / fullPeriod - mean * mean;
    std::array<double, bins.size()> phases{};
    double tonePower = 0;
    bool complete = true;
    for (std::size_t tone = 0; tone < bins.size(); ++tone) {
        auto phase = (static_cast<std::size_t>(firstFrame % period) * bins[tone]) & mask;
        double inPhase = 0, quadrature = 0;
        for (const double value : input) {
            const double sample = value - mean;
            inPhase += sample * sine_[phase];
            quadrature -= sample * sine_[(phase + period / 4) & mask];
            phase = (phase + bins[tone]) & mask;
        }
        const double amplitude = (2.0 / fullPeriod) * std::hypot(inPhase, quadrature);
        if (tone == 0) {
            result.pilotGain = amplitude / pilotAmplitude;
            if (amplitude < 1e-5) {
                result.status = LoopbackStatus::SignalTooLow;
                return result;
            }
        }
        complete = complete && amplitude >= 1e-6;
        phases[tone] = std::atan2(quadrature, inPhase) / tau;
        tonePower += 0.5 * amplitude * amplitude;
    }
    result.coherence = signalPower > 0 ? std::clamp(tonePower / signalPower, 0.0, 1.0) : 0;
    result.status = LoopbackStatus::Incoherent;
    if (!complete || result.coherence < 0.5) return result;

    auto candidate = unwrap(phases, false);
    const auto reversed = unwrap(phases, true);
    if (reversed && (!candidate || reversed->worstError < candidate->worstError)) candidate = reversed;
    if (!candidate) return result;
    if (candidate->delay > maximumDelay + 1e-5) {
        result.status = LoopbackStatus::OutsideRange;
        return result;
    }
    result.status = LoopbackStatus::Measured;
    result.samples = std::min(candidate->delay, maximumDelay);
    result.inverted = candidate->inverted;
    return result;
}

}
