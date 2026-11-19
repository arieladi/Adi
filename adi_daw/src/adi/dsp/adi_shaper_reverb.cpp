// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_reverb.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace adi::dsp {
namespace {
float normalized(float u, float fallback) noexcept {
    return std::isfinite(u) ? std::clamp(u, 0.f, 1.f) : fallback;
}
// Reproducible positions and signed pressure coefficients for an original
// stereo room. This sequence is evaluated only when rebuilding tap geometry.
double unit(std::uint32_t& state) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<double>(state) * 0x1p-32;
}
double decayRate(double u) noexcept {
    // Reciprocal measured half-amplitude times. At full duration the
    // original room determines the complete decay.
    constexpr std::array<double, 5> halfRate{1/.014, 1/.146, 1/.837, 1/2.046, 0};
    const double pos = u * 4.;
    const auto index = std::min(static_cast<std::size_t>(pos), halfRate.size() - 2);
    const double f = pos - static_cast<double>(index);
    if (index == 3) return halfRate[3] * (1. - f);
    return halfRate[index] * std::pow(halfRate[index + 1] / halfRate[index], f);
}
}

AdiShaperReverb::Frame AdiShaperReverb::Diffuser::advance(Frame input) noexcept {
    Frame sum = input;
    const auto mask = ring.size() - 1;
    ring[write] = input;
    valid = std::min(valid + 1, ring.size());
    for (const auto& t : taps) {
        if (t.lag >= valid) continue;
        const auto& past = ring[(write - t.lag) & mask];
        sum[0] += t.left * past[0];
        sum[1] += t.right * past[1];
    }
    write = (write + 1) & mask;
    return sum;
}

bool AdiShaperReverb::prepare(double rate) noexcept {
    if (!std::isfinite(rate) || rate < 1000 || rate > 768000) {
        rate_ = 0;
        return false;
    }
    try {
        // Max three seconds of predelay plus the maximum size of this room.
        history_.resize(std::bit_ceil(static_cast<std::size_t>(std::ceil(5.5 * rate)) + 1));
        const auto shortSize = std::bit_ceil(static_cast<std::size_t>(std::ceil(.021 * rate)) + 2);
        for (auto& d : diffusion_) d.ring.resize(shortSize);
    } catch (...) {
        rate_ = 0;
        return false;
    }
    rate_ = rate;
    shape();
    setPredelay(delay_, bpm_);
    reset();
    return true;
}

void AdiShaperReverb::reset() noexcept {
    write_ = valid_ = remaining_ = 0;
    for (auto& d : diffusion_) d.write = d.valid = 0;
    previousReturn_ = 1.f;
}

void AdiShaperReverb::setShape(float size, float decay, bool gate) noexcept {
    size = normalized(size, .5f);
    decay = normalized(decay, 1.f);
    if (size_ == size && decay_ == decay && gate_ == gate) return;
    size_ = size;
    decay_ = decay;
    gate_ = gate;
    shape();
}

void AdiShaperReverb::shape() noexcept {
    if (rate_ == 0) return;
    const double scale = std::exp2(2. * size_ - 1.);
    const double damping = decayRate(decay_);
    const auto envelope = [&](double seconds) {
        if (!gate_) return std::exp2(-damping * seconds);
        const double phase = std::clamp((seconds - 1.8 * decay_) / .2, 0., 1.);
        return .5 + .5 * std::cos(std::numbers::pi * phase);
    };
    std::uint32_t rng = 0xada18573U;
    tailLength_ = 0;
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        const double t = (static_cast<double>(i) + .7 * unit(rng)) / taps_.size();
        const double base = i == 0 ? 1./48000. : .001 + 1.2 * t * t;
        const double seconds = base * scale;
        const double gain = .055 * std::exp(-4.8 * base) * envelope(seconds);
        auto& tap = taps_[i];
        tap.lag = static_cast<std::size_t>(std::llround(seconds * rate_));
        const auto reflection = [&]() {
            const double magnitude = gain * (.65 + .35 * unit(rng));
            return static_cast<float>(unit(rng) > .5 ? magnitude : -magnitude);
        };
        tap.left = reflection();
        tap.right = reflection();
        tailLength_ = std::max(tailLength_, tap.lag);
    }
    for (std::size_t stage = 0; stage < diffusion_.size(); ++stage) {
        const double period = stage == 0 ? .0023 : .0098;
        for (std::size_t i = 0; i < diffusion_[stage].taps.size(); ++i) {
            auto& t = diffusion_[stage].taps[i];
            const double seconds = period * ((static_cast<double>(i) + .75 + .25 * unit(rng)) / 5.) * scale;
            t.lag = static_cast<std::size_t>(std::llround(rate_ * seconds));
            const double gain = .31 * envelope(seconds);
            t.left = static_cast<float>(gain * (unit(rng) > .5 ? 1. : -1.));
            t.right = static_cast<float>(gain * (unit(rng) > .5 ? 1. : -1.));
        }
        tailLength_ += diffusion_[stage].taps.back().lag;
    }
    // A longer room can expose older signal after the control changes.
    if (valid_ > 0) remaining_ = tailLength_ + predelayFrames_ + 1;
}

void AdiShaperReverb::setPredelay(float u, double bpm) noexcept {
    delay_ = normalized(u, 0.f);
    bpm_ = std::isfinite(bpm) ? std::clamp(bpm, 20., 10000.) : 120.;
    const double beats = std::clamp(std::ceil((delay_ - .5) * 8.), 1., 4.) * .25;
    const double seconds = delay_ <= .5f ? .6 * delay_ * delay_ : 60. * beats / bpm_;
    predelayFrames_ = static_cast<std::size_t>(std::llround(seconds * rate_));
    if (valid_ > 0) remaining_ = tailLength_ + predelayFrames_ + 1;
}

void AdiShaperReverb::setWidth(float width) noexcept {
    side_ = 2 * normalized(width, .5f);
}

void AdiShaperReverb::processStereo(float& l, float& r, float send, float ret) noexcept {
    if (rate_ == 0) { l = r = 0; return; }
    send = normalized(send, 0.f);
    ret = normalized(ret, 0.f);
    if (clearTails_ && previousReturn_ > 0 && ret == 0) reset();
    previousReturn_ = ret;
    const Frame source{l*send,r*send};
    const bool excitation = source[0] != 0 || source[1] != 0;
    if (excitation) remaining_ = tailLength_ + predelayFrames_ + 1;
    // Keep recording when the current return is silent. Increasing the room
    // size or predelay can make earlier signal audible again.
    const auto mask = history_.size() - 1;
    Frame diffused = diffusion_[0].advance(source);
    diffused = diffusion_[1].advance(diffused);
    history_[write_] = diffused;
    valid_ = std::min(valid_ + 1, history_.size());
    Frame wet{};
    if (remaining_ && ret != 0) {
        for (const auto& t : taps_) {
            const auto lag = t.lag + predelayFrames_;
            if (lag >= valid_) break;
            const auto& past = history_[(write_ - lag) & mask];
            wet[0] += past[0] * t.left;
            wet[1] += past[1] * t.right;
        }
    }
    if (remaining_) --remaining_;
    write_ = (write_ + 1) & mask;
    const float mid = (wet[0] + wet[1]) * .5f;
    const float side = (wet[0] - wet[1]) * .5f * side_;
    l = (mid + side) * ret;
    r = (mid - side) * ret;
}

} // namespace adi::dsp
