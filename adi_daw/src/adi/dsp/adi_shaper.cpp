// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

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

float finiteOrZero(float sample) noexcept {
    return std::isfinite(sample) ? sample : 0.0f;
}

void sanitizeStereo(const float* inL, const float* inR, float* outL, float* outR,
                    std::size_t frames) noexcept {
    if (outL == inR || outR == inL) {
        for (std::size_t i = 0; i < frames; ++i) {
            const float l = inL[i], r = inR[i];
            outL[i] = finiteOrZero(l);
            outR[i] = finiteOrZero(r);
        }
    } else {
        std::transform(inL, inL + frames, outL, finiteOrZero);
        std::transform(inR, inR + frames, outR, finiteOrZero);
    }
}
void applyWidth(float width, float& left, float& right) noexcept {
    if (width == 1.0f) return;
    const float mid = 0.5f * left + 0.5f * right;
    const float side = (0.5f * left - 0.5f * right) * width;
    left = mid + side;
    right = mid - side;
}
} // namespace

AdiShaper::AdiShaper() noexcept {
    for (const Stage kind : activeStages) {
        const auto i = static_cast<std::size_t>(kind);
        ranks_[i] = static_cast<std::uint8_t>(8 + 10 * i);
        stages_[i].enabled = kind < Stage::Filter;
        for (auto& band : stages_[i].bands) {
            const bool startsAtZero = kind == Stage::Pan || kind == Stage::Drive;
            band.primary.points.fill(startsAtZero ? 0.0f : kind == Stage::Sweep ? 0.5f : 1.0f);
            band.secondary.points.fill(kind == Stage::Crush || kind == Stage::Reverb ? 1.0f :
                                       (kind == Stage::Sweep || kind == Stage::Noise) ? 0.5f : 0.0f);
            if (kind == Stage::Reverb) band.mix = .25f;
            if (kind == Stage::Sweep) band.type = static_cast<int>(AdiShaperSweep::Mode::Flange);
        }
        updateStage(kind);
    }
    for (auto& sweep : sweeps_) sweep.setMode(AdiShaperSweep::Mode::Flange);
    prepareStages(sampleRate_);
}

AdiShaper::StageOrder AdiShaper::orderFor(const StageRanks& ranks) noexcept {
    StageOrder result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto stage = activeStages[i];
        const auto rank = ranks[static_cast<std::size_t>(stage)];
        std::size_t slot = i;
        while (slot != 0 && ranks[static_cast<std::size_t>(result[slot - 1])] > rank) {
            result[slot] = result[slot - 1];
            --slot;
        }
        result[slot] = stage;
    }
    return result;
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
    for (auto& sweep : sweeps_) (void)sweep.prepare(sampleRate);
    for (auto& drive : drives_) drive.prepare(sampleRate);
    for (auto& pan : panHistory_) pan.prepare(sampleRate);
    for (auto& crusher : crush_) (void)crusher.prepare(sampleRate);
    constexpr std::array<std::uint32_t, 3> seeds{0xa28d45b3U, 0x41f9c67dU, 0x9db703e1U};
    for (std::size_t i = 0; i < noise_.size(); ++i) noise_[i].prepare(sampleRate, seeds[i]);
    for (auto& room : reverb_) (void)room.prepare(sampleRate);
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
    poles = {};
    pairs = {};
    compensation = {};
}

double AdiShaper::Split::pole(double input, double& state) const noexcept {
    const double increment = (input - state) * coefficient;
    const double low = state + increment;
    state = low + increment;
    return low;
}

AdiShaper::StageData::Bands AdiShaper::StageData::firstOrderBands(float left, float right) noexcept {
    // Each outer band crosses its own edge first. Swapping the two filters
    // changes the response while either cutoff moves.
#if defined(_M_X64) || defined(__SSE2__)
    const auto input = _mm_set_pd(right, left);
    const auto subtract = [](__m128d a, __m128d b) { return _mm_sub_pd(a, b); };
    const auto pole = [](__m128d signal, Split& split, std::size_t index) {
        auto& history = split.poles[index];
        const auto state = _mm_loadu_pd(history.data());
        const auto delta = _mm_sub_pd(signal, state);
        const auto increment = _mm_mul_pd(delta, _mm_set1_pd(split.coefficient));
        const auto output = _mm_add_pd(state, increment);
        _mm_storeu_pd(history.data(), _mm_add_pd(output, increment));
        return output;
    };
    const auto asFloats = [](__m128d pair) -> std::array<float, 2> {
        std::array<float, 4> values;
        _mm_storeu_ps(values.data(), _mm_cvtpd_ps(pair));
        return {values[0], values[1]};
    };
#else
    using Pair = Split::StereoPole;
    const Pair input{left, right};
    const auto subtract = [](const Pair& a, const Pair& b) -> Pair {
        return {a[0] - b[0], a[1] - b[1]};
    };
    const auto pole = [](const Pair& signal, Split& split, std::size_t index) -> Pair {
        auto& history = split.poles[index];
        return {split.pole(signal[0], history[0]), split.pole(signal[1], history[1])};
    };
    const auto asFloats = [](const Pair& pair) -> std::array<float, 2> {
        return {static_cast<float>(pair[0]), static_cast<float>(pair[1])};
    };
#endif
    const auto bass = pole(pole(input, low, 0), high, 1);
    const auto upperHigh = subtract(input, pole(input, high, 0));
    const auto treble = subtract(upperHigh, pole(upperHigh, low, 1));
    return {asFloats(bass), asFloats(subtract(subtract(input, bass), treble)), asFloats(treble)};
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
    auto& channelPairs = pairs[channel];
    if (slope == 0) {
        const auto low = static_cast<float>(pole(input, poles[0][channel]));
        return {low, input - low};
    }
    if (slope == 1) {
        // Both LR2 branches pass through one additional first-order allpass.
        // The aligned high branch has inverted polarity, so the sum is AP^2.
        const double phase = 2.0 * pole(input, poles[0][channel]) - input;
        const double low1 = pole(phase, poles[1][channel]);
        const double high1 = phase - low1;
        const double low = pole(low1, poles[2][channel]);
        const double high = pole(high1, poles[3][channel]) - high1;
        return {static_cast<float>(low), static_cast<float>(high)};
    }
    double low1, high1, low, high, unused1, unused2;
    butterworth(input, channelPairs[0], low1, high1, unused1);
    butterworth(low1, channelPairs[1], low, unused1, unused2);
    butterworth(high1, channelPairs[2], unused1, high, unused2);
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

bool AdiShaper::Split::finishBlock() noexcept {
    bool repaired = false;
    for (std::size_t ch = 0; ch < pairs.size(); ++ch) {
        bool bad = false;
        const auto flush = [&bad](double& x) {
            bad |= !std::isfinite(x);
            if (std::abs(x) < 1.0e-280) x = 0;
        };
        for (auto& history : poles) flush(history[ch]);
        for (auto& pair : pairs[ch]) {
            flush(pair.band);
            flush(pair.low);
        }
        auto& pair = compensation[ch];
        flush(pair.band);
        flush(pair.low);
        if (bad) {
            for (auto& history : poles) history[ch] = 0.0;
            pairs[ch] = {};
            pair = {};
            repaired = true;
        }
    }
    return repaired;
}

void AdiShaper::updateSplits(StageData& stage) const noexcept {
    // The two cutoff controls span ten octaves, with half an octave
    // reserved above the lower edge. Keep the requested Hz for later edits.
    constexpr double bottomHz = 20.601722307054366;
    constexpr double lowestUpperHz = bottomHz * std::numbers::sqrt2;
    constexpr double highestLowerHz = 512.0 * lowestUpperHz;
    const double lower = std::clamp(stage.lowHz, bottomHz, highestLowerHz);
    const double upperFloor = stage.layout == 3 ? lower * std::numbers::sqrt2 : lowestUpperHz;
    stage.low.prepare(lower, sampleRate_);
    stage.high.prepare(std::max(stage.highHz, upperFloor), sampleRate_);
}

void AdiShaper::updateStep(CurveBand& band) const noexcept {
    const double cyclesPerSecond = band.hertz ? band.hz : (playing_ ? bpm_ / (60.0 * band.beats) : 0.0);
    band.step = cyclePhase(cyclesPerSecond / sampleRate_);
}

void AdiShaper::tempo(double bpm) noexcept {
    if (!std::isfinite(bpm) || bpm <= 0.0 || bpm > 10000.0 || bpm == bpm_) return;
    bpm_ = bpm;
    for (std::size_t i = 0; i < reverb_.size(); ++i)
        reverb_[i].setPredelay(reverbSettings_[i].predelay, bpm);
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
    if (stages_[static_cast<std::size_t>(Stage::Time)].enabled && (!historyStampSet_ || changed || quarterNotes != lastHistoryStamp_ || historyToleranceFrames == 0)) {
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
    if (stages_[static_cast<std::size_t>(Stage::Time)].enabled) {
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
    // The host and the running clock can round the same next beat differently.
    // Reanchoring a continuous Crush clock can move the transition into or out
    // of full-rate sampling by one frame and change the held samples that follow.
    const double beatRoundoff = 4.0 * std::numeric_limits<double>::epsilon()
                              * std::max(1.0, std::abs(quarterNotes));
    const double quarterSampleInBeats = bpm_ / (240.0 * sampleRate_);
    const bool continuous = !changed && std::abs(quarterNotes - quarterNotes_)
                                      <= std::min(beatRoundoff, quarterSampleInBeats);
    playing_ = playing;
    quarterNotes_ = quarterNotes;
    for (auto& stage : stages_) {
        if (!stage.enabled) continue;
        const bool retainHoldClock = continuous && &stage == &stages_[static_cast<std::size_t>(Stage::Crush)];
        for (auto& band : stage.bands) {
            if (band.hertz || (retainHoldClock && !band.secondary.constant)) continue;
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
    if (kind == Stage::Reverb) {
        band.returnDry = std::min(1.f, 2.f * (1.f - band.blend));
        band.returnWet = std::min(1.f, 2.f * band.blend);
    }
    if (kind >= Stage::Filter) {
        band.secondary.update();
        band.active = kind != Stage::Sweep || band.type != static_cast<int>(AdiShaperSweep::Mode::Off);
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
    if (kind == Stage::Pan) for (auto& pan : panHistory_) pan.valid = 0;
    if (kind == Stage::Filter) for (auto& filter : filters_) filter.reset();
    if (kind == Stage::Sweep) for (auto& sweep : sweeps_) sweep.reset();
    if (kind == Stage::Reverb) for (auto& room : reverb_) room.reset();
    if (kind == Stage::Crush) for (auto& crusher : crush_) crusher.reset();
    if (kind == Stage::Noise) for (auto& noise : noise_) noise.reset();
    if (kind == Stage::Time) for (auto& time : times_) time.reset();
    if (kind == Stage::Drive) {
        for (auto& drive : drives_) drive.reset();
        driveDry_ = {};
        driveSource_ = {};
    }
}

void AdiShaper::setNoiseOption(std::size_t band, NoiseOption option, float value) noexcept {
    auto& noise = noise_[band];
    const float unit = std::clamp(value, 0.f, 100.f) * .01f;
    switch (option) {
    case NoiseMono: noise.setMono(value >= .5f); break;
    case NoiseOnly: noise.setNoiseOnly(value >= .5f); break;
    case NoiseTrim: noise.setTrim(unit); break;
    case NoiseHighPass: case NoiseLowPass: {
        auto& filters = noiseFilters_[band];
        filters[option == NoiseHighPass ? 0 : 1] = unit;
        noise.setFilters(filters[0], filters[1]);
        break;
    }
    }
}

void AdiShaper::setReverbOption(std::size_t band, ReverbOption option, float value) noexcept {
    auto& reverb = reverb_[band];
    auto& settings = reverbSettings_[band];
    const float unit = std::clamp(value, 0.f, 100.f) * .01f;
    switch (option) {
    case RoomSize: settings.size = unit; break;
    case RoomDecay: settings.decay = unit; break;
    case RoomGate: settings.gate = value >= .5f; break;
    case RoomWidth: reverb.setWidth(unit); return;
    case RoomPredelay: settings.predelay = unit; reverb.setPredelay(unit, bpm_); return;
    case RoomClearTails: reverb.setClearTails(value >= .5f); return;
    }
    reverb.setShape(settings.size, settings.decay, settings.gate);
}

void AdiShaper::setMod(Param id, float value) noexcept {
    if (id >= SweepLowPolarity && id < DriveOn) {
        sweeps_[static_cast<std::size_t>(id - SweepLowPolarity)].setPolarity(value >= 0.5f);
        return;
    }
    struct Parameters { Param first; Stage kind; int bandSize; };
    // Descending by saved parameter ID. The caller has already rejected every hole.
    static constexpr std::array blocks{
        Parameters{ReverbOn, Stage::Reverb, reverbBandParameterCount},
        Parameters{NoiseOn, Stage::Noise, noiseBandParameterCount},
        Parameters{TimeOn, Stage::Time, timeBandParameterCount},
        Parameters{CrushOn, Stage::Crush, modBandParameterCount},
        Parameters{DriveOn, Stage::Drive, driveBandParameterCount},
        Parameters{SweepOn, Stage::Sweep, modBandParameterCount},
        Parameters{FilterOn, Stage::Filter, modBandParameterCount}
    };
    const auto block = std::find_if(blocks.begin(), blocks.end(),
                                    [id](const Parameters& row) { return id >= row.first; });
    const auto kind = block->kind;
    const int local = id - block->first;
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
    const int stride = block->bandSize;
    const auto index = static_cast<std::size_t>((local - modHeaderSize) / stride);
    auto& band = stage.bands[index];
    int control = (local - modHeaderSize) % stride;
    if (kind == Stage::Noise && control > Secondary8) {
        setNoiseOption(index, static_cast<NoiseOption>(control - Secondary8 - 1), value);
        return;
    }
    if (kind == Stage::Reverb) {
        ++control; // The band's first control is Mix.
        if (control > Secondary8) {
            setReverbOption(index, static_cast<ReverbOption>(control - Secondary8 - 1), value);
            return;
        }
    }
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
        } else if (kind == Stage::Sweep) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 5.0f)));
            sweeps_[index].setMode(static_cast<AdiShaperSweep::Mode>(band.type));
        } else if (kind == Stage::Drive) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 2.0f)));
            drives_[index].setMode(static_cast<AdiShaperDrive::Mode>(band.type));
        } else if (kind == Stage::Noise) {
            band.type = static_cast<int>(std::round(std::clamp(value, 0.0f, 2.0f)));
            noise_[index].setSource(static_cast<AdiShaperNoise::Source>(band.type));
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
            band.secondary.points[static_cast<std::size_t>(control - Secondary1)] = kind == Stage::Noise
                ? (std::clamp(value, -24.0f, 24.0f) + 24.0f) / 48.0f
                : std::clamp(value, 0.0f, 100.0f) * 0.01f;
    }
    updateBand(kind, band);
    updateActivity(stage);
}

void AdiShaper::set(Param id, float value) noexcept {
    if (!hasParameter(id) || !std::isfinite(value)) return;
    if (id >= PanLowDelayBlend) {
        panHistory_[static_cast<std::size_t>(id - PanLowDelayBlend)].setBlend(value);
        return;
    }
    if (id >= VolumeOrder) {
        const auto rank = static_cast<std::uint8_t>(std::round(std::clamp(value, 0.f, 127.f)));
        auto& current = ranks_[static_cast<std::size_t>(id - VolumeOrder)];
        if (rank != current) {
            current = rank;
            orderDirty_ = true;
        }
        return;
    }
    if (id == MasterMix) {
        masterMix_ = std::clamp(value, 0.0f, 100.0f) * 0.01f;
        if (masterMix_ == 0.f) resetKernels(Stage::Pan);
        for (const Stage stage : activeStages) updateStage(stage);
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
            const auto selected = static_cast<int>(std::round(std::clamp(value, 0.0f, 3.0f)));
            if (kind == Stage::Pan && stage.layout != selected) resetKernels(kind);
            layout(stage, selected);
            return;
        }
        else if (local == 1) stage.lowHz = std::clamp(value, 20.0f, 20000.0f);
        else stage.highHz = std::clamp(value, 20.0f, 20000.0f);
        updateSplits(stage);
    } else {
        const auto bandIndex = (local - stageHeaderSize) / bandParameterCount;
        auto& band = stage.bands[bandIndex];
        const auto control = (local - stageHeaderSize) % bandParameterCount;
        switch (control) {
        case Mix: band.mix = std::clamp(value, 0.0f, 100.0f) * 0.01f; break;
        case Clock: case Cycle: case Rate: clock(band, static_cast<BandControl>(control), value); return;
        case Curve:
            band.primary.linear = value >= 0.5f;
            return;
        default: {
            const float lo = kind == Stage::Pan ? -100.0f : 0.0f;
            const float hi = kind == Stage::Width ? 200.0f : 100.0f;
            band.primary.points[control - Point1] = std::clamp(value, lo, hi) * 0.01f;
            break;
        }
        }
        updateBand(kind, band);
        if (kind == Stage::Pan && !band.active) panHistory_[bandIndex].valid = 0;
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

double AdiShaper::Contour::heldPanAt(std::uint64_t position) const noexcept {
    // A held segment occupies 1,024 of the 8,192 contour samples. Only the
    // samples next to a step need the four-point interpolation.
    // Round the sub-cell fraction before splitting it. A host beat restored
    // at a block boundary and one reached by phase increments can differ by
    // a few low bits at the very same knot.
    const auto rounded = position + (std::uint64_t{1} << 26);
    const auto cell = static_cast<unsigned>(rounded >> 51);
    const auto within = cell & 1023u;
    const double a = points[cell >> 10];
    if (within > 0 && within < 1022) return a;
    const auto point = [&](unsigned k) { return static_cast<double>(points[(k & 8191u) >> 10]); };
    const double l = point(cell - 1), b = point(cell + 1), r = point(cell + 2);
    const double t = static_cast<double>((rounded >> 27) & 0xffffffu) * 0x1p-24;
    return a + .5 * t * (b - l + t * (2*l - 5*a + 4*b - r + t * (3*(a-b) + r-l)));
}

void AdiShaper::PanHistory::prepare(double rate) noexcept {
    head = valid = 0;
    if (rate > 1536000.) { frames.clear(); return; }
    // The 2 ms far-ear delay reaches 2.87 ms while the contour overshoots.
    const auto length = static_cast<std::size_t>(std::ceil(.003 * rate)) + 2;
    try {
        frames.resize(std::bit_ceil(length));
    } catch (const std::bad_alloc&) {
        frames.clear();
    }
}

void AdiShaper::PanHistory::setBlend(float percent) noexcept {
    const float twice = std::clamp(percent, 0.f, 100.f) * .02f;
    const float direct = std::max(0.f, 1.f - twice);
    current = direct * direct * direct;
    unity = std::max(0.f, twice - 1.f);
}

AdiShaper::PanHistory::Tap AdiShaper::PanHistory::tap(double position, double rate, bool constant) const noexcept {
    const double height = std::clamp(.5 + .5 * position, 0., 1.);
    const float leftGain = panGain(height), rightGain = panGain(1. - height);
    Tap result;
    result.gain = {leftGain + unity * (1.f - leftGain), rightGain + unity * (1.f - rightGain)};
    if (current == 1.f || frames.empty()) return result;
    // At the two-millisecond range a stationary contour reads a whole frame.
    // A moving contour also interpolates between the two neighboring frames.
    const double far = std::abs(position);
    const double ms = far * (.710640 + far * (-.505011 + far * 1.794401));
    const double delay = ms * (.001 * rate);
    result.age = static_cast<std::size_t>(delay);
    result.fraction = constant ? 0. : delay - static_cast<double>(result.age);
    result.channel = position > 0. ? 1u : 0u;
    return result;
}

void AdiShaper::PanHistory::process(const Tap& tap, float& left, float& right) noexcept {
    left *= tap.gain.left;
    right *= tap.gain.right;
    if (frames.empty()) return;
    frames[head] = {finiteOrZero(left), finiteOrZero(right)};
    valid = std::min(valid + 1, frames.size());
    const auto mask = frames.size() - 1;
    if (tap.age != 0 || tap.fraction != 0.) {
        const auto read = [&](std::size_t age) { return age < valid ? frames[(head-age) & mask][tap.channel] : 0.f; };
        const float a = read(tap.age);
        const float historical = tap.fraction == 0. ? a :
            static_cast<float>(a + tap.fraction * (static_cast<double>(read(tap.age + 1)) - a));
        auto& far = tap.channel == 0 ? left : right;
        far = historical + current * (far - historical);
    }
    head = (head + 1) & mask;
}

float AdiShaper::Contour::returnAt(std::uint64_t position, std::uint64_t step) const noexcept {
    if (linear && !constant && step != 0) {
        // A linear return may touch zero between samples. Assign a crossed
        // node to its nearest frame so transport resync and callback size
        // cannot skip it. Inspect the authored height, including tiny
        // positive minima that must preserve the tail.
        constexpr auto segment = std::uint64_t{1} << 61;
        const auto begin = position - step / 2;
        const auto crossings = (step >> 61) + (((begin % segment) + (step % segment)) >> 61);
        auto node = static_cast<std::size_t>(begin >> 61);
        for (std::uint64_t i = 0; i < crossings; ++i) {
            node = (node + 1) % points.size();
            if (points[node] == 0.f) return 0.f;
        }
    }
    return at(position);
}

void AdiShaper::DriveDryDelay::advance(float& l, float& r) noexcept {
    const auto previous = samples[index];
    // A preceding stage can overflow even with finite rack input. Do not put
    // that sample in the parallel dry delay where it could reach a later block.
    samples[index] = {finiteOrZero(l), finiteOrZero(r)};
    l = previous[0];
    r = previous[1];
    index = (index + 1) % samples.size();
}

template <AdiShaper::Stage kind>
void AdiShaper::render(StageData& stage, const float* inL, const float* inR,
                       float* outL, float* outR, std::size_t frames) noexcept {
    if constexpr (kind == Stage::Pan) {
        for (std::size_t i = 0; i < stage.bands.size(); ++i) {
            const auto& band = stage.bands[i];
            if (band.active && band.primary.constant)
                panHistory_[i].constantTap = panHistory_[i].tap(band.primary.points[0], sampleRate_, true);
        }
    }
    auto applyGain = [](Gain g, float& l, float& r) {
        if constexpr (kind == Stage::Width) {
            applyWidth(g.left, l, r);
        } else {
            l *= g.left;
            r *= g.right;
        }
    };
    [[maybe_unused]] auto timeDistance = [&](std::size_t bandIndex, float height) {
        const double framesBack = (1.0 - static_cast<double>(height)) * timeSpans_[bandIndex];
        return stage.bands[bandIndex].type >= static_cast<int>(DelayRange::Short2_5ms)
                ? std::max(0.0, framesBack - 1.0) : framesBack;
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
            } else if constexpr (kind == Stage::Sweep) {
                sweeps_[index].processStereo(l, r, primary, band.secondary.at(p));
            } else if constexpr (kind == Stage::Noise) {
                noise_[index].processStereo(l, r, primary, band.secondary.at(p));
            } else if constexpr (kind == Stage::Reverb) {
                auto& room = reverb_[index];
                const float ret = room.clearsTails() ? band.secondary.returnAt(p, band.step) : band.secondary.at(p);
                room.processStereo(l, r, primary, ret, band.blend != 0.f);
            } else if constexpr (kind == Stage::Time) {
                times_[index].processStereo(l, r, timeDistance(index, primary), band.primary.linear && !band.primary.constant);
            } else {
                crush_[index].processStereo(l, r, primary, band.secondary.at(p), band.secondary.constant);
            }
            if (band.blend == 0.0f) { l = dryL; r = dryR; }
            else if constexpr (kind == Stage::Reverb) {
                l = finiteOrZero(band.returnDry * dryL + band.returnWet * l);
                r = finiteOrZero(band.returnDry * dryR + band.returnWet * r);
            } else if (band.blend != 1.0f) {
                l = dryL + band.blend * (l - dryL);
                r = dryR + band.blend * (r - dryR);
            }
        } else {
            if constexpr (kind == Stage::Pan) {
                auto& history = panHistory_[index];
                if (band.primary.constant) history.process(history.constantTap, l, r);
                else {
                    const double position = band.primary.linear ? band.primary.at(p) : band.primary.heldPanAt(p);
                    history.process(history.tap(position, sampleRate_, false), l, r);
                }
                if (band.blend != 1.f) {
                    l = dryL + band.blend * (l - dryL);
                    r = dryR + band.blend * (r - dryR);
                }
            } else {
                // An else, not a return: MSVC /WX rejects the gain path as unreachable code (C4702) for Pan.
                Gain g;
                if (band.primary.constant) g = band.heldGains[0];
                else if (!band.primary.linear) g = band.heldGains[static_cast<std::size_t>(p >> 61)];
                else g = gain(kind, band.primary.at(p), band.blend);
                applyGain(g, l, r);
            }
        }
    };
    if (stage.layout == 0) {
        if constexpr (kind == Stage::Noise) {
            const auto& band = stage.bands[1];
            if (band.primary.constant && band.blend == 1.f &&
                noise_[1].processWhiteBlock(inL, inR, outL, outR, frames, band.primary.points[0])) return;
        } else if constexpr (kind == Stage::Time) {
            const auto& band = stage.bands[1];
            if (band.primary.constant && band.blend == 1.0f
                && times_[1].processFixedBlock(inL, inR, outL, outR, frames, timeDistance(1, band.primary.points[0]))) return;
        }
        if constexpr (kind == Stage::Volume) {
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
        if (flatPhase && hasLow && hasHigh) {
            const auto split = stage.firstOrderBands(midL, midR);
            accumulate(0, split.low[0], split.low[1]);
            accumulate(2, split.high[0], split.high[1]);
            midL = split.mid[0];
            midR = split.mid[1];
        } else {
            if (hasLow) {
                auto l = stage.low.split(midL, 0);
                auto r = stage.low.split(midR, 1);
                midL = l.high;
                midR = r.high;
                // Steeper three-way splits align the low band to the upper allpass.
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
        }
        accumulate(1, midL, midR);
        outL[i] = !flatPhase ? sumL : (sumL == 0.0f ? dryL : dryL + sumL);
        outR[i] = !flatPhase ? sumR : (sumR == 0.0f ? dryR : dryR + sumR);
    }
}

bool AdiShaper::renderFixedWidth(StageData& stage, float* left, float* right, std::size_t frames) noexcept {
    if (stage.layout != 3 || stage.low.slope != 0 || stage.high.slope != 0) return false;
    std::array<float, 3> widths;
    for (std::size_t i = 0; i < widths.size(); ++i) {
        const auto& band = stage.bands[i];
        if (!band.primary.constant) return false;
        widths[i] = band.active ? band.heldGains[0].left : 1.0f;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        const float dryL = left[i], dryR = right[i];
        const auto bands = stage.firstOrderBands(dryL, dryR);
        float sumL = 0.0f, sumR = 0.0f;
        const auto addBand = [&](const std::array<float, 2>& dry, float width) {
            float l = dry[0], r = dry[1];
            applyWidth(width, l, r);
            sumL += l - dry[0];
            sumR += r - dry[1];
        };
        addBand(bands.low, widths[0]);
        addBand(bands.high, widths[2]);
        addBand(bands.mid, widths[1]);
        left[i] = sumL == 0.0f ? dryL : dryL + sumL;
        right[i] = sumR == 0.0f ? dryR : dryR + sumR;
    }
    return true;
}

void AdiShaper::process(const float* inL, const float* inR, float* outL, float* outR,
                        std::size_t frames) noexcept {
    if (!frames) return;
    // Replace a bad upstream sample before it reaches any recursive or delayed
    // stage. A finite sample, including signed zero, is passed through intact.
    sanitizeStereo(inL, inR, outL, outR, frames);
    auto run = [&]<Stage kind>() {
        auto& stage = stages_[static_cast<std::size_t>(kind)];
        if constexpr (kind >= Stage::Filter) {
            if (!stage.enabled) return;
        }
        if (!stage.active && stage.layout == 0) return;
        bool rendered = false;
        if constexpr (kind == Stage::Width) rendered = renderFixedWidth(stage, outL, outR, frames);
        if (!rendered) render<kind>(stage, outL, outR, outL, outR, frames);
        bool repaired = false;
        if (stage.layout != 0) {
            repaired |= stage.low.finishBlock();
            repaired |= stage.high.finishBlock();
        }
        if constexpr (kind >= Stage::Filter && kind <= Stage::Crush) {
            const auto finish = [&](std::size_t index) {
                if constexpr (kind == Stage::Filter) return filters_[index].finishBlock();
                if constexpr (kind == Stage::Sweep) return sweeps_[index].finishBlock();
                if constexpr (kind == Stage::Drive) return drives_[index].finishBlock();
                if constexpr (kind == Stage::Crush) return crush_[index].finishBlock();
            };
            if (stage.layout == 0 || stage.bands[1].active) repaired |= finish(1);
            if ((stage.layout & 1) && stage.bands[0].active) repaired |= finish(0);
            if ((stage.layout & 2) && stage.bands[2].active) repaired |= finish(2);
        }
        if (repaired) sanitizeStereo(outL, outR, outL, outR, frames);
    };
    if (orderDirty_) {
        order_ = orderFor(ranks_);
        defaultOrder_ = std::is_sorted(order_.begin(), order_.end());
        orderDirty_ = false;
    }
    if (defaultOrder_) {
        run.template operator()<Stage::Volume>();
        run.template operator()<Stage::Width>();
        run.template operator()<Stage::Pan>();
        run.template operator()<Stage::Filter>();
        run.template operator()<Stage::Sweep>();
        run.template operator()<Stage::Drive>();
        run.template operator()<Stage::Crush>();
        run.template operator()<Stage::Time>();
        run.template operator()<Stage::Noise>();
        run.template operator()<Stage::Reverb>();
    } else {
        // Route one host segment at a time. Each stage retains its kernels,
        // histories and free-running phase when it moves to a different slot.
        for (const auto stage : order_) {
            switch (stage) {
            case Stage::Volume: run.template operator()<Stage::Volume>(); break;
            case Stage::Width: run.template operator()<Stage::Width>(); break;
            case Stage::Pan: run.template operator()<Stage::Pan>(); break;
            case Stage::Filter: run.template operator()<Stage::Filter>(); break;
            case Stage::Sweep: run.template operator()<Stage::Sweep>(); break;
            case Stage::Drive: run.template operator()<Stage::Drive>(); break;
            case Stage::Crush: run.template operator()<Stage::Crush>(); break;
            case Stage::Time: run.template operator()<Stage::Time>(); break;
            case Stage::Noise: run.template operator()<Stage::Noise>(); break;
            case Stage::Reverb: run.template operator()<Stage::Reverb>(); break;
            }
        }
    }
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
