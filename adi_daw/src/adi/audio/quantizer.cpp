// SPDX-License-Identifier: AGPL-3.0-or-later
// FIR error-feedback adaptation of SoX 14.4.2 src/dither.h and src/dither.c.
// Copyright (c) 2008-9 robs@users.sourceforge.net
// Upstream: LGPL-2.1-or-later. See third_party_notices/sox_dither/NOTICE.md.
// ADI changes, 2026-11-20: container-independent double input, SplitMix64,
// interleaved channel state, strict headroom bound, nonfinite handling and counts.
#include "quantizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace adi::audio {
namespace {

#include "sox_dither_coefficients.inc"

struct Filter {
    WavDither mode;
    std::uint32_t nominalRate;
    std::span<const double> coefficients;
};

constexpr std::array filters{
    Filter{WavDither::Shaped1, 44100, lip44},
    Filter{WavDither::Shaped2, 46000, iew44},
    Filter{WavDither::Shaped3, 48000, shi48},
    Filter{WavDither::Shaped3, 44100, shi44},
    Filter{WavDither::Shaped3, 37800, shi38},
    Filter{WavDither::Shaped3, 32000, shi32},
    Filter{WavDither::Shaped3, 22050, shi22},
    Filter{WavDither::Shaped3, 16000, shi16},
    Filter{WavDither::Shaped3, 11025, shi11},
    Filter{WavDither::Shaped3, 8000, shi08}
};

bool shaped(WavDither mode) noexcept {
    return mode == WavDither::Shaped1 || mode == WavDither::Shaped2 || mode == WavDither::Shaped3;
}

const Filter* filterFor(WavDither mode, std::uint32_t sampleRate) noexcept {
    for (const auto& filter : filters) {
        const auto difference = sampleRate > filter.nominalRate
            ? sampleRate - filter.nominalRate : filter.nominalRate - sampleRate;
        // Integer comparison keeps the exact five-percent boundary reproducible.
        if (filter.mode == mode && static_cast<std::uint64_t>(difference) * 20 <= filter.nominalRate) {
            return &filter;
        }
    }
    return nullptr;
}

} // namespace

bool validDither(WavDither mode) noexcept {
    switch (mode) {
    case WavDither::None:
    case WavDither::Triangular:
    case WavDither::Rectangular:
    case WavDither::Shaped1:
    case WavDither::Shaped2:
    case WavDither::Shaped3:
        return true;
    }
    return false;
}

const char* ditherName(WavDither mode) noexcept {
    switch (mode) {
    case WavDither::None: return "none";
    case WavDither::Triangular: return "triangular";
    case WavDither::Rectangular: return "rectangular";
    case WavDither::Shaped1: return "shaped1";
    case WavDither::Shaped2: return "shaped2";
    case WavDither::Shaped3: return "shaped3";
    }
    return "unknown";
}

WavDither effectiveDither(WavDither mode, std::uint32_t sampleRate) noexcept {
    return shaped(mode) && filterFor(mode, sampleRate) == nullptr ? WavDither::Triangular : mode;
}

PcmQuantizer::PcmQuantizer(std::uint32_t sampleRate, std::uint16_t channels, unsigned bits,
                         WavDither mode, std::uint64_t seed)
    : mode_(mode), channels_(channels), randomState_(seed) {
    if (sampleRate == 0 || channels == 0 || (bits != 16 && bits != 24) || !validDither(mode)) {
        throw std::invalid_argument("Invalid PCM quantizer format or dither");
    }
    scale_ = bits == 16 ? 32768.0 : 8388608.0;
    mode_ = effectiveDither(mode, sampleRate);
    if (const auto* filter = filterFor(mode_, sampleRate)) {
        coefficients_ = filter->coefficients;
        double coefficientSum = 0;
        for (const double coefficient : coefficients_) {
            coefficientSum += std::abs(coefficient);
        }
        // In destination LSBs, u = x - sum(a[k] * e[k]), q = round(u + t),
        // e = q - u. |t| <= 1 and rounding error <= 0.5 imply |e| <= 1.5.
        // Thus |q| <= |x| + 1.5 * (1 + sum(abs(a))). Reserving ceil(bound)+2
        // also covers the positive endpoint (scale-1) and numerical error.
        // Unlike SoX's empirical gain_cB values, this is a worst-case FIR bound.
        const double reserve = std::ceil(1.5 * (1 + coefficientSum)) + 2;
        gain_ = (scale_ - reserve) / scale_;
        histories_.resize(channels);
    }
}

double PcmQuantizer::uniform() noexcept {
    auto z = (randomState_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<double>((z ^ (z >> 31)) >> 11) * 0x1p-53;
}

double PcmQuantizer::noise() noexcept {
    if (mode_ == WavDither::None) {
        return 0;
    }
    const double a = uniform();
    return mode_ == WavDither::Rectangular ? a - 0.5 : a - uniform();
}

void PcmQuantizer::nextChannel() noexcept {
    ++channel_;
    if (channel_ == channels_) {
        channel_ = 0;
    }
}

std::int32_t PcmQuantizer::quantize(double sample) noexcept {
    // Legacy WavWriter draws first, even when the input is nonfinite.
    const double dither = noise();
    if (!std::isfinite(sample)) {
        if (!histories_.empty()) {
            histories_[channel_] = {};
        }
        nextChannel();
        return 0;
    }

    double signal = std::clamp(sample, -2.0, 2.0) * scale_;
    if (!histories_.empty()) {
        signal *= gain_;
        const auto& history = histories_[channel_];
        for (std::size_t tap = 0; tap < coefficients_.size(); ++tap) {
            const auto index = (history.position + tap) % coefficients_.size();
            signal -= coefficients_[tap] * history.errors[index];
        }
    }
    const double rounded = std::round(signal + dither);
    if (!histories_.empty()) {
        auto& history = histories_[channel_];
        history.position = history.position == 0 ? coefficients_.size() - 1 : history.position - 1;
        // Keep the pre-clamp error bounded even after out-of-range source input.
        history.errors[history.position] = rounded - signal;
    }
    if ((rounded < -scale_ || rounded > scale_ - 1) && saturations_ != std::numeric_limits<std::uint64_t>::max()) {
        ++saturations_;
    }
    nextChannel();
    return static_cast<std::int32_t>(std::clamp(rounded, -scale_, scale_ - 1));
}

} // namespace adi::audio
