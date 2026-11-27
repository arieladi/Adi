// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/dsp/biquad.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace adi::dsp {
inline constexpr std::size_t kColourWaveformColumns = 512;
inline constexpr double kColourWaveformSeconds = 2.;
inline constexpr double kColourWaveformColumnsPerSecond =
    static_cast<double>(kColourWaveformColumns) / kColourWaveformSeconds;

struct ColourWaveformReading {
    struct Column {
        float envelope = 0.f;
        std::array<float, 3> bands{};
        bool operator==(const Column &) const = default;
    };
    std::array<Column, kColourWaveformColumns> columns{}; // oldest to newest, first size entries
    std::size_t size = 0;
    std::uint64_t serial = 0;
    std::int64_t firstSequence = 0, endSequence = 0; // interval origin and newest completed native boundary
    bool operator==(const ColourWaveformReading &) const = default;
};

class ColourWaveformCapture {
  public:
    bool prepare(double sampleRate) noexcept {
        constexpr double lowHz = 200., highHz = 4000., q = .5;
        if (!std::isfinite(sampleRate) || sampleRate <= 2. * highHz) return false;
        sampleRate_ = sampleRate;
        lowCoefficients_ = lowPass(sampleRate, lowHz, q);
        highCoefficients_ = highPass(sampleRate, highHz, q);
        reset();
        return true;
    }

    void reset() noexcept {
        head_ = size_ = 0;
        serial_ = 0;
        first_ = end_ = nextInput_ = 0;
        haveInput_ = false;
        columnTime_ = 0.;
        pending_ = {};
        lowState_ = highState_ = {};
    }

    std::uint64_t serial() const noexcept { return serial_; }

    // A hole resets the presentation interval. The finite suffix still starts
    // a fresh capture, independent of the host's audio callback partition.
    bool feed(std::span<const float> left, std::span<const float> right, std::int64_t firstSequence) noexcept {
        if (!(sampleRate_ > 0.) || left.size() != right.size() || firstSequence < 0 ||
            left.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - firstSequence)) return false;
        if (left.empty()) return true;
        bool contiguous = !haveInput_ || firstSequence == nextInput_;
        if (!contiguous) reset();
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
                reset();
                contiguous = false;
                continue;
            }
            if (!haveInput_) {
                first_ = firstSequence + static_cast<std::int64_t>(i);
                haveInput_ = true;
            }
            // Keep finite processing headroom for a corrupt-but-finite sample.
            const auto mono = std::clamp(.5 * (static_cast<double>(left[i]) + right[i]), -1.e12, 1.e12);
            const auto low = lowState_.step(lowCoefficients_, mono);
            const auto high = highState_.step(highCoefficients_, mono);
            const auto middle = mono - low - high;
            const auto accumulate = [](float &peak, double sample) { peak = std::max(peak, static_cast<float>(std::abs(sample))); };
            accumulate(pending_.envelope, mono);
            accumulate(pending_.bands[0], low);
            accumulate(pending_.bands[1], middle);
            accumulate(pending_.bands[2], high);
            columnTime_ += kColourWaveformColumnsPerSecond;
            if (columnTime_ >= sampleRate_) {
                columnTime_ -= sampleRate_;
                columns_[head_] = pending_;
                head_ = (head_ + 1) % columns_.size();
                size_ = std::min(size_ + 1, columns_.size());
                ++serial_;
                end_ = firstSequence + static_cast<std::int64_t>(i) + 1;
                pending_ = {};
            }
        }
        nextInput_ = firstSequence + static_cast<std::int64_t>(left.size());
        return contiguous;
    }

    ColourWaveformReading reading() const noexcept {
        ColourWaveformReading out;
        out.size = size_;
        out.serial = serial_;
        out.firstSequence = first_;
        out.endSequence = end_;
        const auto oldest = (head_ + columns_.size() - size_) % columns_.size();
        for (std::size_t i = 0; i < size_; ++i) out.columns[i] = columns_[(oldest + i) % columns_.size()];
        return out;
    }

  private:
    struct FilterState {
        double first = 0., second = 0.;
        double step(const Biquad &coefficients, double input) noexcept {
            const auto output = coefficients.b0 * input + first;
            first = coefficients.b1 * input - coefficients.a1 * output + second;
            second = coefficients.b2 * input - coefficients.a2 * output;
            if (input == 0. && std::abs(first) < 1.e-24 && std::abs(second) < 1.e-24) first = second = 0.;
            return output;
        }
    };
    std::array<ColourWaveformReading::Column, kColourWaveformColumns> columns_{};
    ColourWaveformReading::Column pending_;
    Biquad lowCoefficients_, highCoefficients_;
    FilterState lowState_, highState_;
    double sampleRate_ = 0., columnTime_ = 0.;
    std::size_t head_ = 0, size_ = 0;
    std::uint64_t serial_ = 0;
    std::int64_t first_ = 0, end_ = 0, nextInput_ = 0;
    bool haveInput_ = false;
};
} // namespace adi::dsp
