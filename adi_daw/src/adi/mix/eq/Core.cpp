// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100 4127 4334 5030)
#endif
#include <Source/SpectralDynamics.h>
#include <ThirdParty/ZLEqualizer/source/zlp/controller.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace adi::mix::eq {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<std::uint64_t> nextGeneration{1};
bool in(float x, float lo, float hi) noexcept { return std::isfinite(x) && x >= lo && x <= hi; }
bool order(std::uint8_t n) noexcept {
    return n == 1 || n == 2 || n == 4 || n == 6 || n == 8 || n == 12 || n == 16;
}
bool spectral(const BandSettings &b) noexcept {
    return b.spectralOn && b.status != Status::Off &&
           (b.shape == Shape::Bell || b.shape == Shape::LowShelf || b.shape == Shape::HighShelf);
}
bool spectral(const Settings &s) noexcept {
    for (const auto &b : s.bands)
        if (spectral(b))
            return true;
    return false;
}
bool samePlan(const Settings &a, const Settings &b) noexcept {
    return a.structure == b.structure && a.lookaheadMs == b.lookaheadMs &&
           a.spectralResolution == b.spectralResolution && spectral(a) == spectral(b);
}
struct FlushDenormals {
#if defined(_M_X64) || defined(__SSE2__)
    unsigned previous = _mm_getcsr();
    FlushDenormals() { _mm_setcsr(previous | 0x8040U); }
    ~FlushDenormals() { _mm_setcsr(previous); }
#endif
};
} // namespace
bool valid(const Settings &s) noexcept {
    if (static_cast<unsigned>(s.structure) > 5 || static_cast<unsigned>(s.spectralResolution) > 2 ||
        !in(s.outputGainDb, -30, 30) || !in(s.gainScalePercent, -100, 200) || !in(s.lookaheadMs, 0, 20) ||
        s.soloBand < -1 || s.soloBand >= 24 || !in(s.soloGainDb, -120, 120))
        return false;
    for (const auto &b : s.bands) {
        if (static_cast<unsigned>(b.status) > 2 || static_cast<unsigned>(b.shape) > 10 ||
            static_cast<unsigned>(b.route) > 4 || static_cast<unsigned>(b.sideShape) > 3 || !order(b.order) ||
            !order(b.sideOrder) || !in(b.frequencyHz, 10, 160000) || !in(b.gainDb, -30, 30) ||
            !in(b.q, .025f, 40) || !in(b.sideFrequencyHz, 10, 160000) || !in(b.sideQ, .025f, 25) ||
            !in(b.targetGainDb, -30, 30) || !in(b.thresholdDb, -80, 0) || !in(b.kneeDb, 0, 32) ||
            !in(b.attackMs, 0, 1000) || !in(b.releaseMs, 0, 5000) || !in(b.rmsLengthMs, 0, 40) ||
            !in(b.rmsMixPercent, 0, 100) || !in(b.smoothPercent, 0, 100) || !in(b.fractionalSlope, -1, 96) ||
            !in(b.spectralDensityPercent, 0, 100))
            return false;
    }
    return true;
}
Settings resolveLinkedChanges(const Settings &previous, Settings next) noexcept {
    for (std::size_t i = 0; i < bandCount; ++i) {
        auto &b = next.bands[i];
        const auto &old = previous.bands[i];
        if (!b.sideLink)
            continue;
        const bool linked = !old.sideLink;
        const bool supports = b.shape == Shape::Bell || b.shape == Shape::LowShelf ||
                              b.shape == Shape::HighShelf || b.shape == Shape::FlatGain ||
                              b.shape == Shape::FlatTilt || b.shape == Shape::TiltShelf;
        if (linked || b.shape != old.shape) {
            if (b.shape == Shape::Bell)
                b.sideShape = SideShape::BandPass;
            else if (b.shape == Shape::LowShelf)
                b.sideShape = SideShape::LowPass;
            else if (b.shape == Shape::HighShelf)
                b.sideShape = SideShape::HighPass;
            else if (supports)
                b.sideShape = SideShape::All;
        }
        if (supports && (linked || b.frequencyHz != old.frequencyHz))
            b.sideFrequencyHz =
                std::clamp(10.f + .1f * std::round((b.frequencyHz - 10.f) / .1f), 10.f, 160000.f);
        if (b.shape == Shape::Bell && (linked || b.q != old.q))
            b.sideQ = std::clamp(.025f + .001f * std::round((b.q - .025f) / .001f), .025f, 25.f);
    }
    return next;
}
struct Core::Implementation {
    adi_eq_zlp::Controller controller;
    ADISharedEqSpectralDynamics spectralEngine;
    PrepareSpec spec{};
    bool prepared = false, applied = false, blockAdmitted = false;
    Settings last{}, active{}, offer{};
    std::array<std::vector<double>, 2> mainStorage, keyStorage, detectorDelay;
    std::array<double *, 2> mainPointers{}, keyPointers{};
    std::array<ADISharedEqSpectralDynamics::Band, bandCount> spectralBands{};
    int detectorPosition = 0, spectralLatency = 0;
    std::uint64_t generation = 0, block = 0, lastPhysical = 0, offerSerial = 0;
    std::atomic<std::uint64_t> pending{0};
    std::atomic<int> committedLatency{0}, reportedActual{0};
    static void latencyChanged(void *context, int samples) noexcept {
        static_cast<Implementation *>(context)->reportedActual.store(samples, std::memory_order_release);
    }
    Implementation() : controller({this, latencyChanged}) {}
    int planLatency(const Settings &s) const noexcept {
        return controller.preparedCorrectionLatency(static_cast<adi_eq_zlp::FilterStructure>(s.structure)) +
               controller.preparedDelayLatency(static_cast<double>(s.lookaheadMs) * .001) +
               (spectral(s) ? spectralEngine.preparedFftSize(
                                  static_cast<ADISharedEqSpectralDynamics::Resolution>(s.spectralResolution))
                            : 0);
    }
    void consider(const Settings &desired) noexcept {
        if (pending.load(std::memory_order_relaxed) || samePlan(active, desired))
            return;
        offer = desired;
        if (++offerSerial == 0)
            ++offerSerial;
        const auto samples = static_cast<std::uint32_t>(planLatency(desired));
        pending.store((offerSerial << 32) | samples, std::memory_order_release);
    }
    Settings pinned(Settings desired) const noexcept {
        desired.structure = active.structure;
        desired.lookaheadMs = active.lookaheadMs;
        desired.spectralResolution = active.spectralResolution;
        if (spectral(desired) != spectral(active))
            for (std::size_t i = 0; i < bandCount; ++i) {
                // Hold only topology that changes spectral latency. Unrelated
                // bands keep their ordinary, segment-accurate control edits.
                if (spectral(desired.bands[i]) != spectral(active.bands[i])) {
                    desired.bands[i].spectralOn = active.bands[i].spectralOn;
                    desired.bands[i].status = active.bands[i].status;
                    desired.bands[i].shape = active.bands[i].shape;
                }
            }
        return desired;
    }
    void apply(const Settings &s) noexcept {
        const bool all = !applied;
        if (all || s.structure != last.structure)
            controller.setFilterStructure(static_cast<adi_eq_zlp::FilterStructure>(s.structure));
        if (all || s.outputGainDb != last.outputGainDb)
            controller.setMakeupGain(s.outputGainDb);
        if (all || s.staticGain != last.staticGain)
            controller.setSGCON(s.staticGain);
        if (all || s.automaticGain != last.automaticGain)
            controller.setAGCON(s.automaticGain);
        if (all || s.polarity != last.polarity)
            controller.setPhaseFlipON(s.polarity);
        if (all || s.lookaheadMs != last.lookaheadMs)
            controller.setDelay(static_cast<double>(s.lookaheadMs) * .001);
        if (all || s.loudnessMatch != last.loudnessMatch)
            controller.setLoudnessMatchON(s.loudnessMatch);
        if (all || s.soloBand != last.soloBand || s.soloSide != last.soloSide)
            controller.setSoloWholeIdx(s.soloBand < 0 ? 2 * bandCount
                                                      : static_cast<std::size_t>(s.soloBand) +
                                                            (s.soloSide ? bandCount : 0));
        if (all || s.soloGainDb != last.soloGainDb)
            controller.setSoloGain(s.soloGainDb);
        const bool gainScale = all || s.gainScalePercent != last.gainScalePercent;
        std::array<ADISharedEqSpectralDynamics::Band, bandCount> nextBands{};
        for (std::size_t i = 0; i < bandCount; ++i) {
            const auto &b = s.bands[i];
            const auto &old = last.bands[i];
            // Remember actual applied topology, not the prepare-time snapshot,
            // for any later structural request. This is control metadata only.
            active.bands[i].status = b.status;
            active.bands[i].shape = b.shape;
            active.bands[i].spectralOn = b.spectralOn;
            if (all || b.status != old.status)
                controller.setFilterStatus(i, static_cast<adi_eq_zlp::FilterStatus>(b.status));
            if (all || b.route != old.route)
                controller.setLRMS(i, static_cast<adi_eq_zlp::FilterStereo>(b.route));
            auto &e = controller.getEmptyFilters()[i];
            bool changed = false;
            if (all || b.shape != old.shape) {
                e.setFilterType(static_cast<adi_eq_dsp::filter::FilterType>(b.shape));
                changed = true;
            }
            if (all || b.order != old.order) {
                e.setOrder(b.order);
                changed = true;
            }
            if (all || b.frequencyHz != old.frequencyHz) {
                e.setFreq(b.frequencyHz);
                changed = true;
            }
            if (gainScale || b.gainDb != old.gainDb) {
                e.setGain(std::clamp(b.gainDb * (s.gainScalePercent / 100.f), -30.f, 30.f));
                changed = true;
            }
            if (all || b.q != old.q) {
                e.setQ(b.q);
                changed = true;
            }
            if (changed) {
                controller.getEmptyUpdateFlags()[i].signal();
                controller.getUpdateFlag().signal();
            }
            if (all || b.dynamicOn != old.dynamicOn)
                controller.setDynamicON(i, b.dynamicOn);
            if (all || b.dynamicBypass != old.dynamicBypass)
                controller.setDynamicBypass(i, b.dynamicBypass);
            if (all || b.dynamicLearn != old.dynamicLearn)
                controller.setDynamicLearn(i, b.dynamicLearn);
            if (all || b.dynamicRelative != old.dynamicRelative)
                controller.setDynamicRelative(i, b.dynamicRelative);
            if (all || b.sideSwap != old.sideSwap)
                controller.setDynamicSwap(i, b.sideSwap);
            if (all || b.thresholdDb != old.thresholdDb)
                controller.setDynamicThreshold(i, b.thresholdDb);
            if (all || b.kneeDb != old.kneeDb)
                controller.setDynamicKnee(i, b.kneeDb);
            if (all || b.attackMs != old.attackMs)
                controller.setDynamicAttack(i, b.attackMs);
            if (all || b.releaseMs != old.releaseMs)
                controller.setDynamicRelease(i, b.releaseMs);
            if (all || b.rmsLengthMs != old.rmsLengthMs)
                controller.setDynamicRMSLength(i, b.rmsLengthMs * .001f);
            if (all || b.rmsMixPercent != old.rmsMixPercent)
                controller.setDynamicRMSMix(i, b.rmsMixPercent * .01f);
            if (all || b.smoothPercent != old.smoothPercent)
                controller.setDynamicSmooth(i, b.smoothPercent * .01f);
            auto &side = controller.getSideEmptyFilters()[i];
            changed = false;
            if (all || b.sideShape != old.sideShape) {
                constexpr std::array types{adi_eq_dsp::filter::kBandPass, adi_eq_dsp::filter::kLowPass,
                                           adi_eq_dsp::filter::kHighPass, adi_eq_dsp::filter::kFlatGain};
                side.setFilterType(types[static_cast<std::size_t>(b.sideShape)]);
                changed = true;
            }
            if (all || b.sideOrder != old.sideOrder) {
                side.setOrder(b.sideOrder);
                changed = true;
            }
            if (all || b.sideFrequencyHz != old.sideFrequencyHz) {
                side.setFreq(b.sideFrequencyHz);
                changed = true;
            }
            if (all || b.sideQ != old.sideQ) {
                side.setQ(b.sideQ);
                changed = true;
            }
            if (gainScale || b.targetGainDb != old.targetGainDb) {
                side.setGain(std::clamp(b.targetGainDb * (s.gainScalePercent / 100.f), -30.f, 30.f));
                changed = true;
            }
            if (changed) {
                controller.getSideEmptyUpdateFlags()[i].signal();
                controller.getUpdateFlag().signal();
            }
            if (all || b.fractionalSlope != old.fractionalSlope)
                controller.setFractionalSlope(i, b.fractionalSlope);
            auto &band = nextBands[i];
            band.enabled = spectral(b);
            if (all || band.enabled != spectralBands[i].enabled)
                controller.setSpectralBand(i, band.enabled);
            if (!band.enabled)
                continue;
            const auto scale = s.gainScalePercent * .01f;
            band.bypassed = b.status != Status::On;
            band.externalSidechain = s.externalSidechain;
            band.automaticThreshold = b.spectralAutomaticThreshold;
            band.frequencyHz = b.frequencyHz;
            band.q = b.q;
            band.staticGainDb = std::clamp(b.gainDb * scale, -30.f, 30.f);
            band.rangeDb = b.dynamicOn && !b.dynamicBypass
                               ? std::clamp(b.targetGainDb * scale, -30.f, 30.f) - band.staticGainDb
                               : 0.f;
            band.thresholdDb = b.thresholdDb;
            band.density = b.spectralDensityPercent * .01f;
            band.tiltDbPerOctave = b.spectralTilt ? 3.f : 0.f;
            band.attackMs = b.attackMs;
            band.releaseMs = b.releaseMs;
            band.routing = static_cast<ADISharedEqSpectralDynamics::Routing>(b.route);
            band.shape = b.shape == Shape::LowShelf    ? ADISharedEqSpectralDynamics::Shape::LowShelf
                         : b.shape == Shape::HighShelf ? ADISharedEqSpectralDynamics::Shape::HighShelf
                                                       : ADISharedEqSpectralDynamics::Shape::Bell;
        }
        spectralEngine.setResolution(
            static_cast<ADISharedEqSpectralDynamics::Resolution>(s.spectralResolution));
        if (all || nextBands != spectralBands) {
            spectralBands = nextBands;
            spectralEngine.setBands(nextBands);
        }
        const auto latency = spectralEngine.latencySamples();
        if (all || latency != spectralLatency) {
            spectralLatency = latency;
            detectorPosition = 0;
            for (auto &d : detectorDelay)
                std::fill(d.begin(), d.end(), 0.);
            controller.setAdditionalLatency(latency);
        }
        last = s;
        applied = true;
    }
    template <class Sample>
    bool run(Sample *const *main, const Sample *const *key, int keyChannels, int frames,
             const Settings &desired, BlockContext context, bool forced) noexcept {
        if (!prepared || frames < 0 || keyChannels < 0 || keyChannels > 2 || !valid(desired) ||
            context.generation != generation || context.block != block || block == 0)
            return false;
        if (frames == 0)
            return true;
        if (main == nullptr)
            return false;
        for (int ch = 0; ch < spec.channels; ++ch)
            if (main[ch] == nullptr)
                return false;
        if (keyChannels && key == nullptr)
            return false;
        for (int ch = 0; ch < keyChannels; ++ch)
            if (key[ch] == nullptr)
                return false;
        [[maybe_unused]] FlushDenormals flush;
        consider(desired);
        const auto s = pinned(desired);
        apply(s);
        const bool external = s.externalSidechain && keyChannels > 0, bypass = forced || s.bypass;
        for (int offset = 0; offset < frames;) {
            const int count = std::min(spec.maximumFrames, frames - offset);
            for (int ch = 0; ch < 2; ++ch) {
                const auto channel = static_cast<std::size_t>(ch);
                const auto *input = main[std::min(ch, spec.channels - 1)] + offset;
                const auto *detector = external ? key[std::min(ch, keyChannels - 1)] + offset : input;
                for (int i = 0; i < count; ++i) {
                    mainPointers[channel][i] = static_cast<double>(input[i]);
                    keyPointers[channel][i] = static_cast<double>(detector[i]);
                }
            }
            const std::array<const double *, 2> spectralKey{keyPointers[0], keyPointers[1]};
            spectralEngine.process(mainPointers.data(), spectralKey.data(), 2, count,
                                   bypass || controller.isSoloActive());
            if (spectralLatency)
                for (int i = 0; i < count; ++i) {
                    for (std::size_t ch = 0; ch < 2; ++ch)
                        std::swap(keyPointers[ch][i],
                                  detectorDelay[ch][static_cast<std::size_t>(detectorPosition)]);
                    if (++detectorPosition == spectralLatency)
                        detectorPosition = 0;
                }
            if (bypass)
                controller.process<true>(mainPointers, keyPointers, static_cast<std::size_t>(count));
            else
                controller.process<false>(mainPointers, keyPointers, static_cast<std::size_t>(count));
            for (int ch = 0; ch < spec.channels; ++ch)
                for (int i = 0; i < count; ++i)
                    main[ch][offset + i] = static_cast<Sample>(mainPointers[static_cast<std::size_t>(ch)][i]);
            offset += count;
        }
        return true;
    }
};
Core::Core() : implementation(std::make_unique<Implementation>()) {}
Core::~Core() = default;
void Core::prepare(PrepareSpec spec, const Settings &s) {
    if (!valid(s) || !std::isfinite(spec.sampleRate) || spec.sampleRate < 8000 || spec.sampleRate > 768000 ||
        spec.maximumFrames < 1 || spec.maximumFrames > 65536 || (spec.channels != 1 && spec.channels != 2))
        throw std::invalid_argument("ADI EQ: unsupported prepare specification or settings");
    auto &i = *implementation;
    i.prepared = false;
    i.spec = spec;
    i.spec.maximumFrames = std::clamp(spec.maximumFrames, 16, 65536);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        i.mainStorage[ch].assign(static_cast<std::size_t>(i.spec.maximumFrames), 0.);
        i.keyStorage[ch].assign(static_cast<std::size_t>(i.spec.maximumFrames), 0.);
        i.mainPointers[ch] = i.mainStorage[ch].data();
        i.keyPointers[ch] = i.keyStorage[ch].data();
    }
    i.spectralEngine.prepare(spec.sampleRate, i.spec.maximumFrames);
    i.spectralEngine.setResolution(ADISharedEqSpectralDynamics::Resolution::High);
    for (auto &d : i.detectorDelay)
        d.assign(static_cast<std::size_t>(i.spectralEngine.fftSize()), 0.);
    i.applied = false;
    i.spectralLatency = -1;
    i.apply(s);
    i.controller.setEditorON(false);
    i.spectralEngine.setDisplayEnabled(false);
    i.controller.prepare(spec.sampleRate, static_cast<std::size_t>(i.spec.maximumFrames));
    i.active = s;
    i.offer = s;
    i.pending.store(0, std::memory_order_release);
    i.committedLatency.store(i.controller.getBaseLatency() + i.spectralLatency, std::memory_order_release);
    i.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);
    i.block = 0;
    i.lastPhysical = 0;
    i.offerSerial = 0;
    i.blockAdmitted = false;
    i.prepared = true;
}
void Core::reset(const Settings &s) { prepare(implementation->spec, s); }
std::int32_t Core::latencySamples() const noexcept {
    return implementation->committedLatency.load(std::memory_order_acquire);
}
LatencyChange Core::pendingLatencyChange() const noexcept {
    const auto token = implementation->pending.load(std::memory_order_acquire);
    return {token, static_cast<std::int32_t>(token & 0xffffffffU)};
}
BlockContext Core::beginBlock(std::uint64_t physical, std::int32_t configured, const Settings &s) noexcept {
    auto &i = *implementation;
    if (!i.prepared || !valid(s) || configured < 0)
        return {};
    if (physical && physical == i.lastPhysical) {
        i.consider(s);
        return i.blockAdmitted ? BlockContext{i.generation, i.block} : BlockContext{};
    }
    i.lastPhysical = physical;
    ++i.block;
    i.blockAdmitted = false;
    const auto pending = i.pending.load(std::memory_order_acquire);
    if (pending && configured == static_cast<std::int32_t>(pending & 0xffffffffU)) {
        i.active = i.offer;
        i.committedLatency.store(configured, std::memory_order_release);
        i.pending.store(0, std::memory_order_release);
    }
    if (configured != i.committedLatency.load(std::memory_order_relaxed))
        return {};
    i.consider(s);
    i.blockAdmitted = true;
    return {i.generation, i.block};
}
bool Core::process(float *const *m, const float *const *k, std::int32_t kc, std::int32_t n, const Settings &s,
                   BlockContext b, bool bypass) noexcept {
    return implementation->run(m, k, kc, n, s, b, bypass);
}
bool Core::process(double *const *m, const double *const *k, std::int32_t kc, std::int32_t n,
                   const Settings &s, BlockContext b, bool bypass) noexcept {
    return implementation->run(m, k, kc, n, s, b, bypass);
}
double Core::learnedThreshold(std::size_t band) const noexcept {
    return band < bandCount ? implementation->controller.getLearnedThreshold(band) : 0.;
}
double Core::learnedKnee(std::size_t band) const noexcept {
    return band < bandCount ? implementation->controller.getLearnedKnee(band) : 0.;
}
} // namespace adi::mix::eq
