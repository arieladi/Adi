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

bool validPosition(double qn) noexcept {
    return std::isfinite(qn) && std::abs(qn) <= 0x1p52;
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
        stages_[i].enabled = kind < Stage::Filter;
        for (auto& band : stages_[i].bands) {
            const bool startsAtZero = kind == Stage::Pan || kind == Stage::Drive;
            band.primary.points.fill(startsAtZero ? 0.0f : kind == Stage::Liquid ? 0.5f : 1.0f);
            band.secondary.points.fill(kind == Stage::Crush ? 1.0f : kind == Stage::Liquid ? 0.5f : 0.0f);
            if (kind == Stage::Liquid) band.type = static_cast<int>(AdiShaperLiquid::Mode::Flange);
        }
        updateStage(kind);
    }
    for (auto& liquid : liquids_) liquid.setMode(AdiShaperLiquid::Mode::Flange);
    prepareStages(sampleRate_);
}

void AdiShaper::prepare(double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 1.0) return;
    for (auto& time : times_) (void)time.prepare();
    prepareStages(sampleRate);
}

void AdiShaper::prepareStages(double sampleRate) noexcept {
    sampleRate_ = sampleRate;
    quarterNotes_ = 0.0;
    historyStampSet_ = false;
    sampleStampSet_ = false;
    historyQuarterNotes_ = 0.0;
    playing_ = true;
    for (auto& filter : filters_) filter.prepare(sampleRate);
    for (auto& liquid : liquids_) (void)liquid.prepare(sampleRate);
    for (auto& drive : drives_) drive.prepare(sampleRate);
    for (auto& crusher : crush_) (void)crusher.prepare(sampleRate);
    for (auto& time : times_) time.reset();
    updateTimeRanges();
    driveDry_ = {};
    driveSource_ = {};
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
    updateTimeRanges();
    for (auto& stage : stages_)
        for (auto& band : stage.bands)
            if (!band.hertz) updateStep(band);
}

void AdiShaper::sync(double quarterNotes, bool playing, unsigned historyToleranceFrames) noexcept {
    if (!validPosition(quarterNotes)) return;
    const bool changed = playing != playing_;
    // Legacy Pd may receive the same block-start stamp for several segments.
    // Keep an independent prediction across duplicate packets. Its audio clock
    // also quantizes to a 64-frame tick, unlike the exact native segment clock.
    if (stages_.back().enabled && (!historyStampSet_ || changed || quarterNotes != lastHistoryStamp_ || historyToleranceFrames == 0)) {
        const double tolerance = (historyToleranceFrames + 0.5) * bpm_ / (60.0 * sampleRate_) + 1.0e-7;
        if (changed || (historyStampSet_ && std::abs(quarterNotes - historyQuarterNotes_) > tolerance))
            resetKernels(Stage::Time);
        historyStampSet_ = true;
        sampleStampSet_ = false;
        lastHistoryStamp_ = historyQuarterNotes_ = quarterNotes;
    }
    syncClock(quarterNotes, playing);
}

void AdiShaper::syncAtSample(double quarterNotes, bool playing, std::int64_t sample, std::uint64_t revision) noexcept {
    if (!validPosition(quarterNotes)) return;
    if (stages_.back().enabled) {
        const auto position = static_cast<std::uint64_t>(sample);
        if (playing != playing_ || (sampleStampSet_ && (position != expectedSample_ || revision != transportRevision_)))
            resetKernels(Stage::Time);
        expectedSample_ = position;
        transportRevision_ = revision;
        sampleStampSet_ = true;
        historyStampSet_ = false;
    }
    syncClock(quarterNotes, playing);
}

void AdiShaper::syncClock(double quarterNotes, bool playing) noexcept {
    const bool changed = playing != playing_;
    playing_ = playing;
    quarterNotes_ = quarterNotes;
    for (auto& stage : stages_) {
        if (!stage.enabled) continue;
        for (auto& band : stage.bands) {
            if (band.hertz) continue;
            band.phase = cyclePhase(quarterNotes / band.beats);
            if (changed) updateStep(band);
        }
    }
}

void AdiShaper::updateTimeRanges() noexcept {
    const auto& stage = stages_[static_cast<std::size_t>(Stage::Time)];
    const double quarterFrames = sampleRate_ * 60.0 / bpm_;
    // Fixed bars use the measured 4/4 scale in all meters. Follow uses the
    // actual contour period, in either quarter notes or seconds.
    constexpr std::array<double, 7> lengths{4, 8, 16, 32, 0.005, 0.020, 0.040};
    for (std::size_t i = 0; i < timeSpans_.size(); ++i) {
        const auto& band = stage.bands[i];
        const auto range = static_cast<DelayRange>(band.type);
        if (range == DelayRange::Follow) {
            timeSpans_[i] = band.hertz ? sampleRate_ / band.hz : quarterFrames * band.beats;
        } else {
            const double length = lengths[static_cast<std::size_t>(band.type - 1)];
            timeSpans_[i] = length * (range < DelayRange::Short2_5ms ? quarterFrames : sampleRate_);
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
    band.primary.update();
    band.blend = masterMix_ * band.mix;
    if (kind >= Stage::Filter) {
        band.secondary.update();
        band.active = kind != Stage::Liquid || band.type != static_cast<int>(AdiShaperLiquid::Mode::Off);
        return;
    }
    const auto neutral = kind == Stage::Pan ? 0.0f : 1.0f;
    for (std::size_t i = 0; i < band.primary.points.size(); ++i)
        band.heldGains[i] = gain(kind, band.primary.points[i], band.blend);
    band.active = band.blend != 0.0f && (!band.primary.constant || band.primary.points[0] != neutral);
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

void AdiShaper::layout(StageData& stage, int value) noexcept {
    if (value == stage.layout) return;
    stage.low.reset();
    stage.high.reset();
    stage.layout = value;
    updateSplits(stage);
    updateActivity(stage);
}

void AdiShaper::slope(Split& split, float value) noexcept {
    const auto selected = static_cast<int>(std::round(std::clamp(value, 0.0f, 2.0f)));
    if (selected != split.slope) {
        split.slope = selected;
        split.reset();
    }
}

void AdiShaper::clock(CurveBand& band, BandControl control, float value) const noexcept {
    if (control == Clock) band.hertz = value >= 0.5f;
    else if (control == Cycle) band.beats = std::clamp(value, 0.03125f, 128.0f);
    else band.hz = std::clamp(value, 0.02f, 5240.0f);
    if (control != Rate && !band.hertz) band.phase = cyclePhase(quarterNotes_ / band.beats);
    updateStep(band);
}

void AdiShaper::restart(Stage kind) noexcept {
    auto& stage = stages_[static_cast<std::size_t>(kind)];
    stage.low.reset();
    stage.high.reset();
    for (auto& band : stage.bands) {
        band.phase = band.hertz ? 0 : cyclePhase(quarterNotes_ / band.beats);
        updateStep(band);
    }
    resetKernels(kind);
    if (kind == Stage::Time) sampleStampSet_ = historyStampSet_ = false;
}

void AdiShaper::resetKernels(Stage kind) noexcept {
    if (kind == Stage::Filter) for (auto& filter : filters_) filter.reset();
    if (kind == Stage::Liquid) for (auto& liquid : liquids_) liquid.reset();
    if (kind == Stage::Crush) for (auto& crusher : crush_) crusher.reset();
    if (kind == Stage::Time) for (auto& time : times_) time.reset();
    if (kind == Stage::Drive) {
        for (auto& drive : drives_) drive.reset();
        driveDry_ = {};
        driveSource_ = {};
    }
}

void AdiShaper::setMod(Param id, float value) noexcept {
    if (id >= LiquidLowPolarity && id < DriveOn) {
        liquids_[static_cast<std::size_t>(id - LiquidLowPolarity)].setPolarity(value >= 0.5f);
        return;
    }
    const auto start = id >= TimeOn ? TimeOn : id >= CrushOn ? CrushOn : id >= DriveOn ? DriveOn : id >= LiquidOn ? LiquidOn : FilterOn;
    const auto kind = start == TimeOn ? Stage::Time : start == CrushOn ? Stage::Crush : start == DriveOn ? Stage::Drive
                    : start == LiquidOn ? Stage::Liquid : Stage::Filter;
    const int local = id - start;
    auto& stage = stages_[static_cast<std::size_t>(kind)];
    if (local < modHeaderSize) {
        switch (local) {
        case 0:
            if (const bool on = value >= 0.5f; on != stage.enabled) {
                stage.enabled = on;
                if (on) restart(kind);
            }
            break;
        case 1: {
            const auto selected = static_cast<int>(std::round(std::clamp(value, 0.0f, 3.0f)));
            if (selected != stage.layout) {
                layout(stage, selected);
                resetKernels(kind);
            }
            break;
        }
        case 2: stage.lowHz = std::clamp(value, 20.0f, 20000.0f); updateSplits(stage); break;
        case 3: stage.highHz = std::clamp(value, 20.0f, 20000.0f); updateSplits(stage); break;
        case 4: slope(stage.low, value); break;
        case 5: slope(stage.high, value); break;
        }
        return;
    }
    const int stride = kind == Stage::Time ? timeBandParameterCount
                     : kind == Stage::Drive ? driveBandParameterCount : modBandParameterCount;
    const auto index = static_cast<std::size_t>((local - modHeaderSize) / stride);
    auto& band = stage.bands[index];
    int control = (local - modHeaderSize) % stride;
    if (kind == Stage::Drive || kind == Stage::Time) {
        if (kind == Stage::Drive && (control == InputTrim || control == OutputTrim)) {
            auto& trims = driveTrims_[index];
            trims[control == InputTrim ? 0 : 1] = std::clamp(value, 0.0f, 100.0f) * 0.01f;
            drives_[index].setTrims(trims[0], trims[1]);
            return;
        }
        // Single-contour stages omit the second curve selector.
        if (control >= Drive1) control += Primary1 - static_cast<int>(Drive1);
    }
    switch (control) {
    case Type:
        if (kind == Stage::Filter) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 4.0f)));
            filters_[index].setMode(static_cast<AdiShaperFilter::Mode>(band.type));
        } else if (kind == Stage::Liquid) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 5.0f)));
            liquids_[index].setMode(static_cast<AdiShaperLiquid::Mode>(band.type));
        } else if (kind == Stage::Drive) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 2.0f)));
            drives_[index].setMode(static_cast<AdiShaperDrive::Mode>(band.type));
        } else if (kind == Stage::Time) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 7.0f)));
            updateTimeRanges();
        } else crush_[index].setDither(value >= 0.5f);
        break;
    case ModMix: band.mix = std::clamp(value, 0.0f, 100.0f) * 0.01f; break;
    case ModClock: case ModCycle: case ModRate:
        clock(band, static_cast<BandControl>(Clock + control - static_cast<int>(ModClock)), value);
        if (kind == Stage::Time) updateTimeRanges();
        return;
    case PrimaryCurve: band.primary.linear = value >= 0.5f; return;
    case SecondaryCurve: band.secondary.linear = value >= 0.5f; return;
    default:
        if (control < Secondary1)
            band.primary.points[static_cast<std::size_t>(control - Primary1)] = std::clamp(value, 0.0f, 100.0f) * 0.01f;
        else
            band.secondary.points[static_cast<std::size_t>(control - Secondary1)] = std::clamp(value, 0.0f, 100.0f) * 0.01f;
    }
    updateBand(kind, band);
    updateActivity(stage);
}

void AdiShaper::set(Param id, float value) noexcept {
    if (id < MasterMix || id >= Count || !std::isfinite(value)) return;
    if (id == MasterMix) {
        masterMix_ = std::clamp(value, 0.0f, 100.0f) * 0.01f;
        for (std::size_t i = 0; i < stages_.size(); ++i) updateStage(static_cast<Stage>(i));
        return;
    }
    if (id >= FilterOn) {
        setMod(id, value);
        return;
    }
    if (id >= VolumeLowSlope) {
        const auto index = static_cast<std::size_t>(id - VolumeLowSlope);
        auto& stage = stages_[index % 3];
        slope(index < 3 ? stage.low : stage.high, value);
        return;
    }
    const auto index = static_cast<std::size_t>(static_cast<int>(id) - 1);
    const auto kind = static_cast<Stage>(index / stageParameterCount);
    auto& stage = stages_[index / stageParameterCount];
    const auto local = index % stageParameterCount;
    if (local < stageHeaderSize) {
        if (local == 0) {
            layout(stage, static_cast<int>(std::round(std::clamp(value, 0.0f, 3.0f))));
            return;
        }
        else if (local == 1) stage.lowHz = std::clamp(value, 20.0f, 20000.0f);
        else stage.highHz = std::clamp(value, 20.0f, 20000.0f);
        updateSplits(stage);
    } else {
        auto& band = stage.bands[(local - stageHeaderSize) / bandParameterCount];
        const auto control = (local - stageHeaderSize) % bandParameterCount;
        switch (control) {
        case Mix: band.mix = std::clamp(value, 0.0f, 100.0f) * 0.01f; break;
        case Clock: case Cycle: case Rate: clock(band, static_cast<BandControl>(control), value); return;
        case Curve: band.primary.linear = value >= 0.5f; return;
        default: {
            const float lo = kind == Stage::Pan ? -100.0f : 0.0f;
            const float hi = kind == Stage::Width ? 200.0f : 100.0f;
            band.primary.points[control - Point1] = std::clamp(value, lo, hi) * 0.01f;
            break;
        }
        }
        updateBand(kind, band);
    }
    updateActivity(stage);
}

void AdiShaper::Contour::update() noexcept {
    constant = true;
    for (std::size_t i = 0; i < points.size(); ++i) {
        differences[i] = points[(i + 1) % points.size()] - points[i];
        if (differences[i] != 0.0f) constant = false;
    }
}

float AdiShaper::Contour::at(std::uint64_t position) const noexcept {
    if (constant) return points[0];
    const auto index = static_cast<std::size_t>(position >> 61);
    if (!linear) return points[index];
    const float fraction = static_cast<float>((position >> 37) & 0xffffffU) * 0x1p-24f;
    return points[index] + fraction * differences[index];
}

void AdiShaper::DriveDryDelay::advance(float& l, float& r) noexcept {
    std::swap(l, samples[index][0]);
    std::swap(r, samples[index][1]);
    index = (index + 1) % samples.size();
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
    auto applyBand = [&](std::size_t index, std::size_t i, float& l, float& r, [[maybe_unused]] float& dryL, [[maybe_unused]] float& dryR) {
        const auto& band = stage.bands[index];
        const auto p = band.phase + band.step * static_cast<std::uint64_t>(i);
        if constexpr (kind >= Stage::Filter) {
            const float primary = band.primary.at(p);
            if constexpr (kind == Stage::Drive) {
                driveDry_[index].advance(dryL, dryR);
                drives_[index].process(l, r, primary);
            } else if constexpr (kind == Stage::Filter) {
                filters_[index].process(l, r, primary, band.secondary.at(p));
            } else if constexpr (kind == Stage::Liquid) {
                liquids_[index].processStereo(l, r, primary, band.secondary.at(p));
            } else if constexpr (kind == Stage::Time) {
                double distance = (1.0 - static_cast<double>(primary)) * timeSpans_[index];
                if (band.type >= static_cast<int>(DelayRange::Short2_5ms))
                    distance = std::max(0.0, distance - 1.0);
                times_[index].processStereo(l, r, distance);
            } else {
                crush_[index].processStereo(l, r, primary, band.secondary.at(p));
            }
            if (band.blend == 0.0f) { l = dryL; r = dryR; }
            else if (band.blend != 1.0f) {
                l = dryL + band.blend * (l - dryL);
                r = dryR + band.blend * (r - dryR);
            }
        } else {
            Gain g;
            if (band.primary.constant) g = band.heldGains[0];
            else if (!band.primary.linear) g = band.heldGains[static_cast<std::size_t>(p >> 61)];
            else g = gain(kind, band.primary.at(p), band.blend);
            applyGain(g, l, r);
        }
    };
    if (stage.layout == 0) {
        if constexpr (kind == Stage::Volume || kind == Stage::Pan) {
            const auto& band = stage.bands[1];
            if (band.primary.constant) {
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
            float dryL = l, dryR = r;
            applyBand(1, i, l, r, dryL, dryR);
            outL[i] = l;
            outR[i] = r;
        }
        return;
    }
    const bool hasLow = (stage.layout & 1) != 0;
    const bool hasHigh = (stage.layout & 2) != 0;
    const bool flatPhase = (!hasLow || stage.low.slope == 0) && (!hasHigh || stage.high.slope == 0);
    for (std::size_t i = 0; i < frames; ++i) {
        float dryL = inL[i], dryR = inR[i];
        float midL = dryL, midR = dryR;
        // First-order splits reconstruct the source exactly. Its aligned dry
        // copy keeps a zero-wet drive bit transparent, even at a crossover.
        if constexpr (kind == Stage::Drive) driveSource_.advance(dryL, dryR);
        float sumL = 0.0f, sumR = 0.0f;
        auto accumulate = [&](std::size_t index, float l, float r) {
            float wetL = l, wetR = r;
            if (stage.bands[index].active) applyBand(index, i, wetL, wetR, l, r);
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
            accumulate(0, l.low, r.low);
        }
        if (hasHigh) {
            const auto l = stage.high.split(midL, 0);
            const auto r = stage.high.split(midR, 1);
            accumulate(2, l.high, r.high);
            midL = l.low;
            midR = r.low;
        }
        accumulate(1, midL, midR);
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
        if constexpr (kind >= Stage::Filter) {
            if (!stage.enabled) return;
        }
        if (!stage.active && stage.layout == 0) return;
        render<kind>(stage, inL, inR, outL, outR, frames);
        inL = outL;
        inR = outR;
    };
    run.template operator()<Stage::Volume>();
    run.template operator()<Stage::Width>();
    run.template operator()<Stage::Pan>();
    run.template operator()<Stage::Filter>();
    run.template operator()<Stage::Liquid>();
    run.template operator()<Stage::Drive>();
    run.template operator()<Stage::Crush>();
    run.template operator()<Stage::Time>();
    copyStereo(inL, inR, outL, outR, frames);
    for (auto& stage : stages_) {
        if (!stage.enabled) continue;
        for (auto& band : stage.bands)
            band.phase += band.step * static_cast<std::uint64_t>(frames);
    }
    if (playing_) {
        const double advance = (bpm_ / (60.0 * sampleRate_)) * static_cast<double>(frames);
        quarterNotes_ += advance;
        if (sampleStampSet_) expectedSample_ += static_cast<std::uint64_t>(frames);
        if (historyStampSet_) historyQuarterNotes_ += advance;
    }
}

} // namespace adi::dsp
