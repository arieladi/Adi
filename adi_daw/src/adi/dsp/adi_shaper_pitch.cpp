// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_pitch.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <numbers>

namespace adi::dsp {
namespace {

constexpr std::size_t taps = 16, phases = 256, filters = 64, steps = 1024;
constexpr double pi = std::numbers::pi_v<double>;

// The interpolation window is slightly asymmetric about the desired sample.
// The input impulse constrains both its centre and its sidelobes.
double window(double x) noexcept
{
    const double angle = 2 * pi * (x + 0.5) / 13.0;
    if (std::abs(angle) > pi) return 0;
    return 0.4235 + 0.5080 * std::cos(angle) + 0.0919 * std::cos(2 * angle)
                  + 0.0024 * std::cos(3 * angle);
}

float normalize(float x) noexcept
{
    return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.5f;
}

} // namespace

struct AdiShaperPitch::Tables {
    using Kernel = std::array<float, taps>;
    std::array<std::array<Kernel, phases + 1>, filters + 1> kernel{};
    std::array<double, steps + 1> ratio{};

    Tables() noexcept
    {
        for (std::size_t i = 0; i <= steps; ++i) {
            const double position = static_cast<double>(i) / steps;
            ratio[i] = std::exp2(4 * position - 2);
        }
        for (std::size_t phase = 0; phase <= phases; ++phase) {
            for (std::size_t tap = 0; tap < taps; ++tap) {
                const double age = static_cast<double>(taps - 1 - tap);
                const double x = age - 7.0 - static_cast<double>(phase) / phases;
                const double weight = window(x);
                for (std::size_t row = 0; row <= filters; ++row) {
                    const double cutoff = 0.9 * std::exp2(-2.0 * row / filters);
                    const double sinc = x == 0 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
                    kernel[row][phase][tap] = static_cast<float>(sinc * weight);
                }
            }
        }
    }
};

bool AdiShaperPitch::prepare(double sampleRate) noexcept
{
    tables_ = nullptr;
    if (!std::isfinite(sampleRate) || sampleRate < 1000 || sampleRate > 768000) return false;
    const auto frames = static_cast<std::size_t>(std::ceil(0.1 * sampleRate)) + taps + 1;
    try {
        history_.resize(frames + taps);
    } catch (const std::bad_alloc&) {
        return false;
    }
    static const Tables sharedTables;
    tables_ = &sharedTables;
    rate_ = sampleRate;
    capacity_ = frames;
    reset();
    return true;
}

void AdiShaperPitch::reset() noexcept
{
    write_ = valid_ = 0;
    lastHeight_ = lastGrain_ = -1;
    readOffset_ = 0;
}

AdiShaperPitch::Stereo AdiShaperPitch::readHead(double offset) const noexcept
{
    const auto age = static_cast<std::size_t>(offset);
    if (age >= valid_) return {};
    const auto count = std::min(taps, valid_ - age);
    const auto oldest = age + count - 1;
    const auto first = write_ >= oldest ? write_ - oldest : write_ + capacity_ - oldest;
    const Stereo* input = history_.data() + first;
    const auto phase = static_cast<std::size_t>((offset - age) * phases + 0.5);
    const float* coeff = tables_->kernel[filterRow_][phase].data() + taps - count;
    Stereo result{};
    if (filterBlend_ == 0) {
        for (std::size_t i = 0; i < count; ++i) {
            result[0] += coeff[i] * input[i][0];
            result[1] += coeff[i] * input[i][1];
        }
    } else {
        const float* next = tables_->kernel[filterRow_ + 1][phase].data() + taps - count;
        for (std::size_t i = 0; i < count; ++i) {
            const float c = coeff[i] + filterBlend_ * (next[i] - coeff[i]);
            result[0] += c * input[i][0];
            result[1] += c * input[i][1];
        }
    }
    return result;
}

void AdiShaperPitch::processStereo(float& left, float& right, float height, float grain) noexcept
{
    if (!tables_) return;
    height = normalize(height);
    grain = normalize(grain);
    if (height != lastHeight_) {
        const double position = static_cast<double>(height) * steps;
        const auto i = static_cast<std::size_t>(position);
        const double delta = (position - i) * (4.0 * std::numbers::ln2 / steps);
        const double ratio = tables_->ratio[i] * (1 + delta * (1 + 0.5 * delta));
        step_ = static_cast<float>(1 - ratio);
        const float row = std::max(0.0f, (2 * height - 1) * filters);
        filterRow_ = static_cast<std::size_t>(row);
        filterBlend_ = row - static_cast<float>(filterRow_);
        lastHeight_ = height;
    }
    if (grain != lastGrain_) {
        const double phase = readOffset_ * inverseWindow_;
        window_ = rate_ * (0.001 + 0.099 * static_cast<double>(grain) * grain);
        inverseWindow_ = 1 / window_;
        readOffset_ = static_cast<float>(phase * window_);
        lastGrain_ = grain;
    }

    const Stereo frame{std::isfinite(left) ? left : 0.0f, std::isfinite(right) ? right : 0.0f};
    history_[write_] = frame;
    if (write_ < taps) history_[write_ + capacity_] = frame;
    valid_ = std::min(valid_ + 1, capacity_);
    const double half = 0.5 * window_;
    const double opposite = readOffset_ < half ? readOffset_ + half : readOffset_ - half;
    Stereo out = readHead(opposite);
    if (readOffset_ != 0) {
        const double nearerEnd = std::min(static_cast<double>(readOffset_), window_ - readOffset_);
        const float t = static_cast<float>(2 * nearerEnd * inverseWindow_);
        const float blend = t * t * (3 - 2 * t);
        const Stereo other = readHead(readOffset_);
        out[0] += blend * (other[0] - out[0]);
        out[1] += blend * (other[1] - out[1]);
    }
    left = out[0];
    right = out[1];
    if (++write_ == capacity_) write_ = 0;
    readOffset_ += step_;
    if (readOffset_ < 0 || readOffset_ >= window_) {
        readOffset_ = static_cast<float>(readOffset_ - window_ * std::floor(readOffset_ * inverseWindow_));
    }
}
} // namespace adi::dsp
