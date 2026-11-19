// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {

std::uint64_t cyclePhase(double cycles) noexcept {
    const double part = cycles - std::floor(cycles);
    return part < 1.0 ? static_cast<std::uint64_t>(part * 0x1p64) : 0;
}

// The measured balance curve is cubic: zero at the bottom, unity at the
// centre, and square root two with a horizontal tangent at the top.
float panGain(double position) noexcept {
    constexpr double a = 8.0 - 6.0 * std::numbers::sqrt2;
    constexpr double b = 11.0 * std::numbers::sqrt2 - 16.0;
    constexpr double c = 8.0 - 4.0 * std::numbers::sqrt2;
    return static_cast<float>(position * (c + position * (b + a * position)));
}

void copyStereo(const float* inL, const float* inR, float* outL, float* outR,
                std::size_t frames) noexcept {
    if (outL == inR || outR == inL) {
        for (std::size_t i = 0; i < frames; ++i) {
            const float l = inL[i], r = inR[i];
            outL[i] = l;
            outR[i] = r;
        }
    } else {
        if (outL != inL) std::copy_n(inL, frames, outL);
        if (outR != inR) std::copy_n(inR, frames, outR);
    }
}
} // namespace

AdiShaper::AdiShaper() noexcept {
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        const auto kind = static_cast<Stage>(i);
        for (auto& band : stages_[i].bands)
            band.points.fill(kind == Stage::Pan ? 0.0f : 1.0f);
        updateStage(kind);
    }
    prepare(sampleRate_);
}

void AdiShaper::prepare(double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 1.0) return;
    sampleRate_ = sampleRate;
    quarterNotes_ = 0.0;
    playing_ = true;
    for (auto& stage : stages_) {
        stage.low.reset();
        stage.high.reset();
        updateSplits(stage);
        for (auto& band : stage.bands) {
            band.phase = 0;
            updateStep(band);
        }
    }
}

void AdiShaper::Split::prepare(double hz, double sampleRate) noexcept {
    const auto warped = std::tan(std::numbers::pi * std::min(hz / sampleRate, 0.49));
    coefficient = warped / (1.0 + warped);
    a1 = 1.0 / (1.0 + warped * (std::numbers::sqrt2 + warped));
    a2 = warped * a1;
    a3 = warped * a2;
}

void AdiShaper::Split::reset() noexcept {
    channels = {};
    compensation = {};
}

double AdiShaper::Split::pole(double input, double& state) const noexcept {
    const double increment = (input - state) * coefficient;
    const double low = state + increment;
    state = low + increment;
    return low;
}

void AdiShaper::Split::butterworth(double x, Integrator& state, double& low,
                                   double& high, double& all) const noexcept {
    const double v3 = x - state.low;
    const double band = a1 * state.band + a2 * v3;
    low = state.low + a2 * state.band + a3 * v3;
    state.band = 2.0 * band - state.band;
    state.low = 2.0 * low - state.low;
    high = x - std::numbers::sqrt2 * band - low;
    all = x - 2.0 * std::numbers::sqrt2 * band;
}

AdiShaper::Split::Bands AdiShaper::Split::split(float input, std::size_t channel) noexcept {
    auto& state = channels[channel];
    if (slope == 0) {
        const auto low = static_cast<float>(pole(input, state.poles[0]));
        return {low, input - low};
    }
    if (slope == 1) {
        // Both LR2 branches pass through one additional first-order allpass.
        // The aligned high branch has inverted polarity, so the sum is AP^2.
        const double phase = 2.0 * pole(input, state.poles[0]) - input;
        const double low1 = pole(phase, state.poles[1]);
        const double high1 = phase - low1;
        const double low = pole(low1, state.poles[2]);
        const double high = pole(high1, state.poles[3]) - high1;
        return {static_cast<float>(low), static_cast<float>(high)};
    }
    double low1, high1, low, high, unused1, unused2;
    butterworth(input, state.pairs[0], low1, high1, unused1);
    butterworth(low1, state.pairs[1], low, unused1, unused2);
    butterworth(high1, state.pairs[2], unused1, high, unused2);
    return {static_cast<float>(low), static_cast<float>(high)};
}

float AdiShaper::Split::align(float input, std::size_t channel) noexcept {
    if (slope == 0) return input;
    auto& state = compensation[channel];
    if (slope == 1) {
        const double first = 2.0 * pole(input, state.band) - input;
        return static_cast<float>(2.0 * pole(first, state.low) - first);
    }
    double low, high, all;
    butterworth(input, state, low, high, all);
    return static_cast<float>(all);
}

void AdiShaper::Split::quiet() noexcept {
    const auto flush = [](double& x) { if (std::abs(x) < 1.0e-280) x = 0; };
    for (auto& state : channels) {
        for (auto& poleState : state.poles) flush(poleState);
        for (auto& pair : state.pairs) {
            flush(pair.band);
            flush(pair.low);
        }
    }
    for (auto& pair : compensation) {
        flush(pair.band);
        flush(pair.low);
    }
}

void AdiShaper::updateSplits(StageData& stage) const noexcept {
    stage.low.prepare(stage.lowHz, sampleRate_);
    const double upper = stage.layout == 3 ? std::max(stage.lowHz, stage.highHz) : stage.highHz;
    stage.high.prepare(upper, sampleRate_);
}

void AdiShaper::updateStep(CurveBand& band) const noexcept {
    const double cyclesPerSecond = band.hertz ? band.hz : (playing_ ? bpm_ / (60.0 * band.beats) : 0.0);
    band.step = cyclePhase(cyclesPerSecond / sampleRate_);
}

void AdiShaper::tempo(double bpm) noexcept {
    if (!std::isfinite(bpm) || bpm <= 0.0 || bpm > 10000.0 || bpm == bpm_) return;
    bpm_ = bpm;
    for (auto& stage : stages_)
        for (auto& band : stage.bands)
            if (!band.hertz) updateStep(band);
}

void AdiShaper::sync(double quarterNotes, bool playing) noexcept {
    if (!std::isfinite(quarterNotes) || std::abs(quarterNotes) > 0x1p52) return;
    const bool changed = playing != playing_;
    playing_ = playing;
    quarterNotes_ = quarterNotes;
    for (auto& stage : stages_) {
        for (auto& band : stage.bands) {
            if (band.hertz) continue;
            band.phase = cyclePhase(quarterNotes / band.beats);
            if (changed) updateStep(band);
        }
    }
}

AdiShaper::Gain AdiShaper::gain(Stage kind, float amount, float mix) noexcept {
    const float dry = 1.0f - mix;
    if (kind != Stage::Pan) {
        const float wet = dry + mix * amount;
        return {wet, wet};
    }
    // With graph height on the vertical axis, the top pans left.
    const double height = std::clamp(0.5 + 0.5 * amount, 0.0, 1.0);
    return {dry + mix * panGain(height), dry + mix * panGain(1.0 - height)};
}

void AdiShaper::updateBand(Stage kind, CurveBand& band) const noexcept {
    const auto neutral = kind == Stage::Pan ? 0.0f : 1.0f;
    band.constant = true;
    band.blend = masterMix_ * band.mix;
    for (std::size_t i = 0; i < band.points.size(); ++i) {
        const float difference = band.points[(i + 1) % band.points.size()] - band.points[i];
        band.differences[i] = difference;
        if (difference != 0.0f) band.constant = false;
        band.heldGains[i] = gain(kind, band.points[i], band.blend);
    }
    band.active = band.blend != 0.0f && (!band.constant || band.points[0] != neutral);
}

void AdiShaper::updateActivity(StageData& stage) noexcept {
    stage.active = stage.bands[1].active
                   || ((stage.layout & 1) && stage.bands[0].active)
                   || ((stage.layout & 2) && stage.bands[2].active);
}

void AdiShaper::updateStage(Stage kind) noexcept {
    auto& stage = stages_[static_cast<std::size_t>(kind)];
    for (auto& band : stage.bands) updateBand(kind, band);
    updateActivity(stage);
}

void AdiShaper::set(Param id, float value) noexcept {
    if (id < MasterMix || id >= Count || !std::isfinite(value)) return;
    if (id == MasterMix) {
        masterMix_ = std::clamp(value, 0.0f, 100.0f) * 0.01f;
        for (std::size_t i = 0; i < stages_.size(); ++i) updateStage(static_cast<Stage>(i));
        return;
    }
    if (id >= VolumeLowSlope) {
        const auto index = static_cast<std::size_t>(id - VolumeLowSlope);
        auto& stage = stages_[index % stages_.size()];
        auto& split = index < stages_.size() ? stage.low : stage.high;
        const int slope = static_cast<int>(std::round(std::clamp(value, 0.0f, 2.0f)));
        if (slope != split.slope) {
            split.slope = slope;
            split.reset();
        }
        return;
    }
    const auto index = static_cast<std::size_t>(static_cast<int>(id) - 1);
    const auto kind = static_cast<Stage>(index / stageParameterCount);
    auto& stage = stages_[index / stageParameterCount];
    const auto local = index % stageParameterCount;
    if (local < stageHeaderSize) {
        if (local == 0) {
            const int layout = static_cast<int>(std::round(std::clamp(value, 0.0f, 3.0f)));
            if (layout != stage.layout) {
                stage.low.reset();
                stage.high.reset();
                stage.layout = layout;
            }
        }
        else if (local == 1) stage.lowHz = std::clamp(value, 20.0f, 20000.0f);
        else stage.highHz = std::clamp(value, 20.0f, 20000.0f);
        updateSplits(stage);
    } else {
        auto& band = stage.bands[(local - stageHeaderSize) / bandParameterCount];
        const auto control = (local - stageHeaderSize) % bandParameterCount;
        switch (control) {
        case Mix: band.mix = std::clamp(value, 0.0f, 100.0f) * 0.01f; break;
        case Clock:
            band.hertz = value >= 0.5f;
            if (!band.hertz) band.phase = cyclePhase(quarterNotes_ / band.beats);
            updateStep(band);
            return;
        case Cycle:
            band.beats = std::clamp(value, 0.03125f, 128.0f);
            if (!band.hertz) band.phase = cyclePhase(quarterNotes_ / band.beats);
            updateStep(band);
            return;
        case Rate:
            band.hz = std::clamp(value, 0.02f, 5240.0f);
            updateStep(band);
            return;
        case Curve: band.linear = value >= 0.5f; return;
        default: {
            const float lo = kind == Stage::Pan ? -100.0f : 0.0f;
            const float hi = kind == Stage::Width ? 200.0f : 100.0f;
            band.points[control - Point1] = std::clamp(value, lo, hi) * 0.01f;
            break;
        }
        }
        updateBand(kind, band);
    }
    updateActivity(stage);
}

float AdiShaper::CurveBand::valueAt(std::size_t sample) const noexcept {
    const auto p = phase + step * static_cast<std::uint64_t>(sample);
    const auto index = static_cast<std::size_t>(p >> 61);
    const float fraction = static_cast<float>((p >> 37) & 0xffffffU) * 0x1p-24f;
    return points[index] + fraction * differences[index];
}

template <AdiShaper::Stage kind>
void AdiShaper::render(StageData& stage, const float* inL, const float* inR,
                       float* outL, float* outR, std::size_t frames) noexcept {
    auto applyGain = [](Gain g, float& l, float& r) {
        if constexpr (kind == Stage::Width) {
            if (g.left == 1.0f) return;
            const float mid = 0.5f * l + 0.5f * r;
            const float side = (0.5f * l - 0.5f * r) * g.left;
            l = mid + side;
            r = mid - side;
        } else {
            l *= g.left;
            r *= g.right;
        }
    };
    auto applyBand = [&](const CurveBand& band, std::size_t i, float& l, float& r) {
        Gain g;
        if (band.constant) {
            g = band.heldGains[0];
        } else if (!band.linear) {
            const auto p = band.phase + band.step * static_cast<std::uint64_t>(i);
            g = band.heldGains[static_cast<std::size_t>(p >> 61)];
        } else {
            g = gain(kind, band.valueAt(i), band.blend);
        }
        applyGain(g, l, r);
    };
    if (stage.layout == 0) {
        const auto& band = stage.bands[1];
        if constexpr (kind != Stage::Width) {
            if (band.constant) {
                for (std::size_t i = 0; i < frames; ++i) {
                    const float l = inL[i], r = inR[i];
                    outL[i] = l * band.heldGains[0].left;
                    outR[i] = r * band.heldGains[0].right;
                }
                return;
            }
        }
        for (std::size_t i = 0; i < frames; ++i) {
            float l = inL[i], r = inR[i];
            applyBand(band, i, l, r);
            outL[i] = l;
            outR[i] = r;
        }
        return;
    }
    const bool hasLow = (stage.layout & 1) != 0;
    const bool hasHigh = (stage.layout & 2) != 0;
    const bool flatPhase = (!hasLow || stage.low.slope == 0) && (!hasHigh || stage.high.slope == 0);
    for (std::size_t i = 0; i < frames; ++i) {
        const float dryL = inL[i], dryR = inR[i];
        float midL = dryL, midR = dryR;
        float sumL = 0.0f, sumR = 0.0f;
        auto accumulate = [&](const CurveBand& band, float l, float r) {
            float wetL = l, wetR = r;
            if (band.active) applyBand(band, i, wetL, wetR);
            if (flatPhase) {
                sumL += wetL - l;
                sumR += wetR - r;
            } else {
                sumL += wetL;
                sumR += wetR;
            }
        };
        if (hasLow) {
            auto l = stage.low.split(midL, 0);
            auto r = stage.low.split(midR, 1);
            midL = l.high;
            midR = r.high;
            // The low band bypasses the upper split and needs its summed
            // allpass phase for coherent three-way reconstruction.
            if (hasHigh) {
                l.low = stage.high.align(l.low, 0);
                r.low = stage.high.align(r.low, 1);
            }
            accumulate(stage.bands[0], l.low, r.low);
        }
        if (hasHigh) {
            const auto l = stage.high.split(midL, 0);
            const auto r = stage.high.split(midR, 1);
            accumulate(stage.bands[2], l.high, r.high);
            midL = l.low;
            midR = r.low;
        }
        accumulate(stage.bands[1], midL, midR);
        outL[i] = !flatPhase ? sumL : (sumL == 0.0f ? dryL : dryL + sumL);
        outR[i] = !flatPhase ? sumR : (sumR == 0.0f ? dryR : dryR + sumR);
    }
    stage.low.quiet();
    stage.high.quiet();
}

void AdiShaper::process(const float* inL, const float* inR, float* outL, float* outR,
                        std::size_t frames) noexcept {
    if (!frames) return;
    // A full-range neutral rack takes only a stereo copy. Configured split
    // filters still follow the input while dry so automation reopens them warm.
    auto run = [&]<Stage kind>() {
        auto& stage = stages_[static_cast<std::size_t>(kind)];
        if (!stage.active && stage.layout == 0) return;
        render<kind>(stage, inL, inR, outL, outR, frames);
        inL = outL;
        inR = outR;
    };
    run.template operator()<Stage::Volume>();
    run.template operator()<Stage::Width>();
    run.template operator()<Stage::Pan>();
    copyStereo(inL, inR, outL, outR, frames);
    for (auto& stage : stages_)
        for (auto& band : stage.bands)
            band.phase += band.step * static_cast<std::uint64_t>(frames);
    if (playing_) quarterNotes_ += (bpm_ / (60.0 * sampleRate_)) * static_cast<double>(frames);
}

} // namespace adi::dsp
