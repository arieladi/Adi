// SPDX-License-Identifier: AGPL-3.0-or-later
#include "GainEnvelope.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace adi::mix::opto {
namespace {
bool unit(float value) noexcept { return std::isfinite(value) && value >= 0 && value <= 1; }
bool valid(const Settings &s) noexcept {
    return std::isfinite(s.thresholdDb) && s.thresholdDb >= -60 && s.thresholdDb <= 0 &&
           unit(s.attack.value) && unit(s.release.value);
}
double coefficient(float samples) noexcept {
    return samples <= std::numeric_limits<float>::epsilon()
               ? 1
               : 1 - std::exp(static_cast<double>(-2.3025851249694824f) / samples);
}
float logAmplitude(float x) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(std::max(x, std::numeric_limits<float>::min()));
    float exponent = static_cast<float>(static_cast<int>(bits >> 23U) - 126);
    const float mantissa = std::bit_cast<float>((bits & 0x807fffffU) | 0x3f000000U);
    float r = mantissa - 1;
    if (mantissa < .7071067690849304f) {
        exponent -= 1;
        r += mantissa;
    }
    float y = .07037683576345444f;
    for (const float c :
         {-.11514610052108765f, .11676998436450958f, -.12420140951871872f, .14249323308467865f,
          -.16668057441711426f, .2000071406364441f, -.24999994039535522f, .3333333134651184f})
        y = y * r + c;
    y = ((y * r) * r) * r;
    y += exponent * -.00021219444170128554f;
    y -= (r * r) * .5f;
    y += r;
    return y + exponent * .693359375f;
}
float amplitudeFromLog(float x) noexcept {
    x = std::clamp(x, -88.37625885009766f, 88.37625885009766f);
    const int n = static_cast<int>(std::floor(x * 1.4426950216293335f + .5f));
    const float nf = static_cast<float>(n);
    float r = x - nf * .693359375f;
    r -= nf * -.00021219444170128554f;
    float y = .00019875691214110702f;
    for (const float c :
         {.001398199936375022f, .008333452045917511f, .04166579619050026f, .1666666567325592f, .5f})
        y = y * r + c;
    y = y * (r * r) + r;
    y += 1;
    return y * std::bit_cast<float>(static_cast<std::uint32_t>(n + 127) << 23U);
}
struct ExpTable {
    std::array<double, 8193> values{};
    ExpTable() noexcept {
        double position = -25;
        for (std::size_t i = 0; i < 8191; ++i) {
            values[i] = std::exp(position);
            position += 25. / 8191.;
        }
        values[8191] = values[8192] = 1;
    }
    double lookup(double value) const noexcept {
        const double position = std::max(-25., value) * (8191. / 25.) + 8191.;
        const auto j = static_cast<std::uint32_t>(position) & 8191U;
        return values[j] + (values[j + 1] - values[j]) * (position - static_cast<double>(j));
    }
};
const ExpTable &sharedTable() noexcept {
    // Stopped RNE preparation only; callbacks retain the resulting pointer and
    // never encounter this static initialization guard.
    static const ExpTable table;
    return table;
}
bool buffers(std::span<const float> key, std::span<float> gain, int maximumFrames) noexcept {
    if (key.size() != gain.size() || key.size() > static_cast<std::size_t>(maximumFrames))
        return false;
    if (key.empty())
        return true;
    if (!key.data() || !gain.data())
        return false;
    const auto source = reinterpret_cast<std::uintptr_t>(key.data());
    const auto target = reinterpret_cast<std::uintptr_t>(gain.data());
    const auto bytes = static_cast<std::uintptr_t>(key.size()) * sizeof(float);
    const auto last = std::numeric_limits<std::uintptr_t>::max() - bytes;
    if (source % alignof(float) || target % alignof(float) || source > last || target > last)
        return false;
    return source == target || !(source < target + bytes && target < source + bytes);
}
} // namespace
struct GainEnvelope::Impl {
    Settings accepted;
    int maximumFrames;
    float rate;
    const ExpTable *table;
    double attack = 0, releaseCoefficient = 1, followerCoefficient = 1;
    double y = 0, u = 0, attackCoefficient = 1;
    float attackSamples = -1;
    Impl(const Preparation &p, const Settings &s, const ExpTable &t) noexcept
        : accepted(s), maximumFrames(p.maximumFrames), rate(static_cast<float>(p.sampleRate)),
          table(&t) {
        const double an = s.attack.value;
        const double a = static_cast<float>(an * an * an);
        const float squaredRelease = s.release.value * s.release.value;
        const double r = static_cast<float>(std::tan(static_cast<double>(squaredRelease) * 1.2) *
                                            .38877956936820496);
        const double seconds = .001 + .249 * a;
        const double coupled =
            (.9 * std::sqrt((std::clamp(seconds, .004, .25) - .004) / (.25 - .004))) * .02;
        const double releaseSeconds = std::max(.0005, .005 + 1.195 * r - coupled);
        attack = seconds * rate;
        releaseCoefficient = coefficient(static_cast<float>(releaseSeconds * rate));
        followerCoefficient = coefficient(static_cast<float>(.004 * rate));
    }
    void reset() noexcept {
        y = u = 0;
        attackSamples = -1;
        attackCoefficient = 1;
    }
    double attackValue(double samples) noexcept {
        const auto rounded = static_cast<float>(samples);
        if (rounded != attackSamples) {
            attackCoefficient = coefficient(rounded);
            attackSamples = rounded;
        }
        return attackCoefficient;
    }
    float step(float key) noexcept {
        const float db =
            std::max(-180.f, logAmplitude(std::max(std::abs(key), 1e-10f)) * 8.68588924407959f);
        const double target = std::max(
            0.f, std::min(60.f, static_cast<float>(
                                    (static_cast<double>(db) - accepted.thresholdDb) * .75)));
        if (target > y) {
            const double c =
                attackValue(attack + (.05 * rate - .2 * attack) * table->lookup(y - target));
            y += c * (target - y);
            u = y;
        } else {
            double delta = target - u;
            if (delta < -1)
                delta = -std::sqrt(-delta);
            u += releaseCoefficient * delta;
            y += followerCoefficient * (u - y);
        }
        return amplitudeFromLog(-static_cast<float>(y) * .115129254758358f);
    }
    void render(std::span<const float> key, std::span<float> gain) noexcept {
        for (std::size_t i = 0; i < key.size(); ++i)
            gain[i] = step(key[i]);
    }
};
GainEnvelope::GainEnvelope() noexcept = default;
GainEnvelope::~GainEnvelope() = default;
Status GainEnvelope::prepare(const Preparation &p, const Settings &s) noexcept {
    if (!std::isfinite(p.sampleRate) || p.sampleRate < 44100 || p.sampleRate > 96000 ||
        p.maximumFrames <= 0 || p.maximumFrames > 4096)
        return Status::InvalidSpec;
    if (!valid(s))
        return Status::InvalidSettings;
    if (std::fegetround() != FE_TONEAREST)
        return Status::UnsupportedEnvironment;
    try {
        const auto &table = sharedTable();
        auto candidate = std::make_unique<Impl>(p, s, table);
        impl_.swap(candidate);
        return Status::Ok;
    } catch (...) {
        return Status::AllocationFailed;
    }
}
void GainEnvelope::reset() noexcept {
    if (impl_)
        impl_->reset();
}
bool GainEnvelope::isPrepared() const noexcept { return static_cast<bool>(impl_); }
const Settings *GainEnvelope::settings() const noexcept {
    return impl_ ? &impl_->accepted : nullptr;
}
Status GainEnvelope::processChecked(std::span<const float> key, std::span<float> gain) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (!buffers(key, gain, impl_->maximumFrames))
        return Status::InvalidBuffers;
    for (const float sample : key)
        if (!std::isfinite(sample))
            return Status::InvalidBuffers;
    impl_->render(key, gain);
    return Status::Ok;
}
Status GainEnvelope::processTrustedFinite(std::span<const float> key,
                                          std::span<float> gain) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (!buffers(key, gain, impl_->maximumFrames))
        return Status::InvalidBuffers;
    impl_->render(key, gain);
    return Status::Ok;
}
} // namespace adi::mix::opto
