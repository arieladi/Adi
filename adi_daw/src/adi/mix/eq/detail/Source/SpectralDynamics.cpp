#include "SpectralDynamics.h"
#include "SpectralMagnitude.h"
#include "../ThirdParty/ZLEqualizer/zldsp_fft/src/zldsp_fft_rfft.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>

namespace
{
static_assert(std::atomic<float>::is_always_lock_free);
using adi_eq_ext::spectral_internal::BiquadMagnitude;

template <typename Value>
using AlignedVector = std::vector<Value, hwy::AlignedAllocator<Value>>;

float bounded(float value, float low, float high, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

float gain(float decibelValue) noexcept
{
    return std::exp(std::clamp(decibelValue, -120.0f, 36.0f) * 0.1151292546497023f);
}

ADISharedEqSpectralDynamics::Band sanitize(ADISharedEqSpectralDynamics::Band band) noexcept
{
    band.frequencyHz = bounded(band.frequencyHz, 10.0f, 160000.0f, 1000.0f);
    band.q = bounded(band.q, 0.025f, 40.0f, 1.0f);
    band.staticGainDb = bounded(band.staticGainDb, -30.0f, 30.0f, 0.0f);
    band.rangeDb = bounded(band.rangeDb, -60.0f, 60.0f, 0.0f);
    band.thresholdDb = bounded(band.thresholdDb, -120.0f, 12.0f, -36.0f);
    band.density = bounded(band.density, 0.0f, 1.0f, 0.5f);
    band.tiltDbPerOctave = bounded(band.tiltDbPerOctave, -12.0f, 12.0f, 3.0f);
    band.attackMs = bounded(band.attackMs, 0.0f, 2000.0f, 10.0f);
    band.releaseMs = bounded(band.releaseMs, 0.0f, 10000.0f, 100.0f);
    band.routing = static_cast<ADISharedEqSpectralDynamics::Routing>(std::clamp(static_cast<int>(band.routing), 0, 4));
    band.shape = static_cast<ADISharedEqSpectralDynamics::Shape>(std::clamp(static_cast<int>(band.shape), 0, 2));
    return band;
}

}

struct ADISharedEqSpectralDynamics::Implementation
{
    struct Plan
    {
        std::unique_ptr<adi_eq_dsp::fft::RFFT<float>> transform;
        AlignedVector<float> window;
        AlignedVector<float> octave;
        AlignedVector<float> spreadStep;
        AlignedVector<double> cosine;
        AlignedVector<double> doubleCosine;
        std::array<int, displayBinCount> displayIndices {};
        std::array<float, displayBinCount> displayFractions {};
        int size = 0;
        float synthesisScale = 0.0f;
    };

    struct BandState
    {
        AlignedVector<float> weight;
        AlignedVector<float> staticDb;
        AlignedVector<float> rawStaticDb;
        AlignedVector<float> envelope;
        float attackCoefficient = 0.0f;
        float releaseCoefficient = 0.0f;
        int firstBin = 0;
        int lastBin = 0;
        bool curveDirty = true;
        bool staticDirty = true;
    };

    struct Detector
    {
        std::array<AlignedVector<float>, 5> levelDb;
        std::array<AlignedVector<float>, 5> peakDb;
        std::array<AlignedVector<float>, 5> backgroundDb;
        std::array<AlignedVector<float>, 5> spreadBackgroundDb;
        std::array<bool, 5> needed {};
    };

    std::array<Band, bandCount> bands {};
    std::array<BandState, bandCount> states;
    std::array<Plan, 3> plans;
    std::array<AlignedVector<float>, 2> input;
    std::array<AlignedVector<float>, 2> keyInput;
    std::array<AlignedVector<float>, 2> output;
    std::array<AlignedVector<double>, 2> dry;
    std::array<AlignedVector<std::complex<float>>, 2> spectrum;
    std::array<AlignedVector<std::complex<float>>, 2> keySpectrum;
    std::array<AlignedVector<float>, 4> gainsDb;
    AlignedVector<float> fftScratch;
    AlignedVector<float> spreadActivity;
    AlignedVector<double> prefix;
    std::array<Detector, 2> detectors;
    std::array<bool, 4> gainNeeded {};
    std::array<std::atomic<float>, displayBinCount> displayedDynamic {};
    std::array<std::atomic<float>, bandCount> displayedPeakChange {};
    std::atomic<bool> displayEnabled { false };
    double sampleRate = 48000.0;
    Resolution resolution = Resolution::Medium;
    int position = 0;
    int hopPosition = 0;
    int size = 0;
    int bins = 0;
    bool enabled = false;
    bool needsKey = false;
    bool anyMask = false;

    Plan& plan() noexcept { return plans[static_cast<std::size_t>(resolution)]; }

    void reset() noexcept
    {
        position = 0;
        hopPosition = 0;
        for (auto* bank : { &input, &keyInput, &output })
            for (auto& values : *bank)
                std::fill(values.begin(), values.end(), 0.0f);
        for (auto& values : dry)
            std::fill(values.begin(), values.end(), 0.0);
        for (auto& state : states)
            std::fill(state.envelope.begin(), state.envelope.end(), 0.0f);
        for (auto& value : displayedDynamic)
            value.store(0.0f, std::memory_order_relaxed);
        for (auto& value : displayedPeakChange)
            value.store(0.0f, std::memory_order_relaxed);
    }

    void updateTiming(std::size_t bandIndex) noexcept
    {
        if (size == 0)
            return;
        const auto& band = bands[bandIndex];
        auto& state = states[bandIndex];
        const auto frameSeconds = static_cast<double>(size / 4) / sampleRate;
        state.attackCoefficient = band.attackMs > 0.0f
            ? static_cast<float>(std::exp(-frameSeconds / (0.001 * band.attackMs))) : 0.0f;
        state.releaseCoefficient = band.releaseMs > 0.0f
            ? static_cast<float>(std::exp(-frameSeconds / (0.001 * band.releaseMs))) : 0.0f;
    }

    void updateBand(std::size_t bandIndex) noexcept
    {
        if (size == 0)
            return;
        updateTiming(bandIndex);
        const auto& band = bands[bandIndex];
        auto& state = states[bandIndex];
        state.firstBin = bins;
        state.lastBin = 0;
        if (state.staticDirty)
        {
            if (band.staticGainDb == 0.0f)
                std::fill_n(state.rawStaticDb.begin(), bins, 0.0f);
            else
                BiquadMagnitude(band, sampleRate).rawResponses(plan().cosine.data(), plan().doubleCosine.data(),
                    state.rawStaticDb.data(), bins);
            for (int bin = 0; bin < bins; ++bin)
                state.staticDb[static_cast<std::size_t>(bin)] = std::clamp(state.rawStaticDb[static_cast<std::size_t>(bin)], -36.0f, 36.0f);
        }
        auto targetBand = band;
        targetBand.staticGainDb += band.rangeDb;
        const auto activeRange = std::abs(band.rangeDb) >= 1.0e-6f;
        if (!activeRange || targetBand.staticGainDb == 0.0f)
            std::fill_n(state.weight.begin(), bins, 0.0f);
        else
            BiquadMagnitude(targetBand, sampleRate).rawResponses(plan().cosine.data(), plan().doubleCosine.data(),
                state.weight.data(), bins);
        for (int bin = 0; bin < bins; ++bin)
        {
            const auto index = static_cast<std::size_t>(bin);
            state.weight[index] = activeRange ? (state.weight[index] - state.rawStaticDb[index]) / band.rangeDb : 0.0f;
            if (std::abs(state.weight[index]) > 0.0001f || std::abs(state.staticDb[index]) > 0.0001f)
            {
                state.firstBin = std::min(state.firstBin, bin);
                state.lastBin = bin + 1;
            }
        }
        state.curveDirty = false;
        state.staticDirty = false;
    }

    void refreshUsage() noexcept
    {
        enabled = false;
        needsKey = false;
        anyMask = false;
        gainNeeded.fill(false);
        for (auto& detector : detectors)
            detector.needed.fill(false);
        for (const auto& band : bands)
        {
            enabled = enabled || band.enabled;
            if (!band.enabled || band.bypassed || (band.rangeDb == 0.0f && band.staticGainDb == 0.0f))
                continue;
            anyMask = true;
            const auto route = static_cast<std::size_t>(band.routing);
            if (band.rangeDb != 0.0f)
            {
                detectors[band.externalSidechain ? 1 : 0].needed[route] = true;
                needsKey = needsKey || band.externalSidechain;
            }
            if (route == 0)
            {
                gainNeeded[0] = true;
                gainNeeded[1] = true;
            }
            else
                gainNeeded[route - 1] = true;
        }
    }

    void calculateDetector(const std::array<AlignedVector<std::complex<float>>, 2>& source,
                           Detector& detector) noexcept
    {
        const auto scale = 16.0f / static_cast<float>(size * size);
        const auto radius = std::max(8, size / 128);
        for (std::size_t route = 0; route < detector.needed.size(); ++route)
        {
            if (!detector.needed[route])
                continue;
            auto& levels = detector.levelDb[route];
            auto& peaks = detector.peakDb[route];
            auto& backgrounds = detector.backgroundDb[route];
            auto& spreadBackgrounds = detector.spreadBackgroundDb[route];
            prefix[0] = 0.0;
            for (int bin = 0; bin < bins; ++bin)
            {
                const auto index = static_cast<std::size_t>(bin);
                const auto left = source[0][index];
                const auto right = source[1][index];
                float power = 0.0f;
                switch (route)
                {
                    case 0: power = 0.5f * (std::norm(left) + std::norm(right)); break;
                    case 1: power = std::norm(left); break;
                    case 2: power = std::norm(right); break;
                    case 3: power = std::norm(0.5f * (left + right)); break;
                    default: power = std::norm(0.5f * (left - right)); break;
                }
                power *= bin == 0 || bin == bins - 1 ? scale * 0.25f : scale;
                levels[index] = power;
                prefix[index + 1] = prefix[index] + static_cast<double>(power);
            }
            adi_eq_ext::spectral_internal::powerToDecibels(levels.data(), bins);
            for (int bin = 0; bin < bins; ++bin)
            {
                const auto first = std::max(0, bin - radius);
                const auto last = std::min(bins, bin + radius + 1);
                const auto mean = (prefix[static_cast<std::size_t>(last)] - prefix[static_cast<std::size_t>(first)])
                    / static_cast<double>(last - first);
                const auto index = static_cast<std::size_t>(bin);
                backgrounds[index] = static_cast<float>(std::max(0.0, mean));
                if (radius != 16)
                {
                    const auto spreadFirst = std::max(0, bin - 16);
                    const auto spreadLast = std::min(bins, bin + 17);
                    const auto spreadMean = (prefix[static_cast<std::size_t>(spreadLast)]
                        - prefix[static_cast<std::size_t>(spreadFirst)]) / static_cast<double>(spreadLast - spreadFirst);
                    spreadBackgrounds[index] = static_cast<float>(std::max(0.0, spreadMean));
                }
                peaks[index] = std::max({ levels[static_cast<std::size_t>(std::max(0, bin - 1))],
                    levels[index], levels[static_cast<std::size_t>(std::min(bins - 1, bin + 1))] });
            }
            adi_eq_ext::spectral_internal::powerToDecibels(backgrounds.data(), bins);
            if (radius == 16)
                std::copy_n(backgrounds.begin(), bins, spreadBackgrounds.begin());
            else
                adi_eq_ext::spectral_internal::powerToDecibels(spreadBackgrounds.data(), bins);
        }
    }

    void frame() noexcept
    {
        auto& currentPlan = plan();
        const auto mask = size - 1;
        const auto publishDisplay = displayEnabled.load(std::memory_order_relaxed);
        if (!anyMask)
        {
            if (publishDisplay)
            {
                for (auto& value : displayedDynamic)
                    value.store(0.0f, std::memory_order_relaxed);
                for (auto& value : displayedPeakChange)
                    value.store(0.0f, std::memory_order_relaxed);
            }
            for (std::size_t channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < size; ++sample)
                {
                    const auto sourceIndex = static_cast<std::size_t>((position + sample) & mask);
                    const auto window = currentPlan.window[static_cast<std::size_t>(sample)];
                    output[channel][sourceIndex] += input[channel][sourceIndex] * window * window * (2.0f / 3.0f);
                }
            return;
        }
        for (std::size_t bandIndex = 0; bandIndex < bands.size(); ++bandIndex)
            if (bands[bandIndex].enabled && !bands[bandIndex].bypassed && states[bandIndex].curveDirty)
                updateBand(bandIndex);
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            for (int sample = 0; sample < size; ++sample)
                fftScratch[static_cast<std::size_t>(sample)] = input[channel][static_cast<std::size_t>((position + sample) & mask)]
                    * currentPlan.window[static_cast<std::size_t>(sample)];
            currentPlan.transform->forward(fftScratch.data(), spectrum[channel].data());
            if (needsKey)
            {
                for (int sample = 0; sample < size; ++sample)
                    fftScratch[static_cast<std::size_t>(sample)] = keyInput[channel][static_cast<std::size_t>((position + sample) & mask)]
                        * currentPlan.window[static_cast<std::size_t>(sample)];
                currentPlan.transform->forward(fftScratch.data(), keySpectrum[channel].data());
            }
        }
        calculateDetector(spectrum, detectors[0]);
        if (needsKey)
            calculateDetector(keySpectrum, detectors[1]);
        for (auto& values : gainsDb)
            std::fill_n(values.begin(), bins, 0.0f);
        std::array<float, displayBinCount> displayCurve {};
        const Band* spreadBand = nullptr;
        auto spreadThreshold = 0.0f;
        for (std::size_t bandIndex = 0; bandIndex < bands.size(); ++bandIndex)
        {
            const auto& band = bands[bandIndex];
            if (!band.enabled || band.bypassed)
            {
                if (publishDisplay)
                    displayedPeakChange[bandIndex].store(0.0f, std::memory_order_relaxed);
                continue;
            }
            auto& state = states[bandIndex];
            const auto route = static_cast<std::size_t>(band.routing);
            const auto& detector = detectors[band.externalSidechain ? 1 : 0];
            const auto maximumChange = std::abs(band.rangeDb);
            const auto sign = band.rangeDb < 0.0f ? -1.0f : 1.0f;
            const auto densityMargin = -12.0f + 11.0f * band.density * band.density * band.density;
            const auto spreadMargin = -12.0f + 11.0f * band.density;
            const auto levelThreshold = band.automaticThreshold ? -72.0f : band.thresholdDb;
            const auto canReuseSpread = spreadBand != nullptr && spreadBand->routing == band.routing
                && spreadBand->externalSidechain == band.externalSidechain && spreadBand->density == band.density
                && spreadBand->tiltDbPerOctave == band.tiltDbPerOctave && spreadThreshold == levelThreshold;
            if (maximumChange > 0.0f && !canReuseSpread)
            {
                for (int bin = 0; bin < bins; ++bin)
                {
                    const auto index = static_cast<std::size_t>(bin);
                    const auto tilt = band.tiltDbPerOctave * currentPlan.octave[index];
                    const auto contrast = detector.levelDb[route][index]
                        - detector.spreadBackgroundDb[route][index] - spreadMargin + tilt;
                    const auto absolute = detector.peakDb[route][index] - levelThreshold + tilt;
                    const auto activity = std::min(contrast, absolute);
                    spreadActivity[index] = bin == 0 ? activity
                        : std::max(activity, spreadActivity[index - 1] - currentPlan.spreadStep[index]);
                }
                for (int bin = bins - 2; bin >= 0; --bin)
                {
                    const auto index = static_cast<std::size_t>(bin);
                    spreadActivity[index] = std::max(spreadActivity[index],
                        spreadActivity[index + 1] - currentPlan.spreadStep[index + 1]);
                }
                spreadBand = &band;
                spreadThreshold = levelThreshold;
            }
            auto peakChange = 0.0f;
            for (int bin = state.firstBin; bin < state.lastBin; ++bin)
            {
                const auto index = static_cast<std::size_t>(bin);
                auto dynamicChange = 0.0f;
                if (maximumChange > 0.0f)
                {
                    const auto contrastThreshold = detector.backgroundDb[route][index] + densityMargin;
                    const auto tilt = band.tiltDbPerOctave * currentPlan.octave[index];
                    const auto contrastLimit = std::max(0.0f, detector.levelDb[route][index] - contrastThreshold + tilt);
                    const auto absoluteLimit = std::max(0.0f, detector.peakDb[route][index] - levelThreshold + tilt);
                    const auto localActivity = std::min(contrastLimit, absoluteLimit);
                    const auto desired = std::min(maximumChange, std::max(localActivity, spreadActivity[index]))
                        * std::abs(state.weight[index]);
                    auto& envelope = state.envelope[index];
                    const auto coefficient = desired > envelope ? state.attackCoefficient : state.releaseCoefficient;
                    envelope = desired + coefficient * (envelope - desired);
                    if (envelope < 1.0e-12f)
                        envelope = 0.0f;
                    if (publishDisplay)
                        peakChange = std::max(peakChange, envelope);
                    dynamicChange = std::copysign(envelope, band.rangeDb * state.weight[index]);
                }
                const auto decibelChange = state.staticDb[index] + dynamicChange;
                if (route == 0)
                {
                    gainsDb[0][index] += decibelChange;
                    gainsDb[1][index] += decibelChange;
                }
                else
                    gainsDb[route - 1][index] += decibelChange;
            }
            if (publishDisplay)
                displayedPeakChange[bandIndex].store(sign * peakChange, std::memory_order_relaxed);
            if (publishDisplay && maximumChange > 0.0f)
                for (std::size_t displayIndex = 0; displayIndex < displayBinCount; ++displayIndex)
                {
                    const auto binIndex = static_cast<std::size_t>(currentPlan.displayIndices[displayIndex]);
                    const auto fraction = currentPlan.displayFractions[displayIndex];
                    const auto first = static_cast<std::size_t>(state.firstBin);
                    const auto last = static_cast<std::size_t>(state.lastBin);
                    const auto lower = binIndex >= first && binIndex < last
                        ? std::copysign(state.envelope[binIndex], state.weight[binIndex]) : 0.0f;
                    const auto upper = binIndex + 1 >= first && binIndex + 1 < last
                        ? std::copysign(state.envelope[binIndex + 1], state.weight[binIndex + 1]) : 0.0f;
                    const auto envelope = lower * (1.0f - fraction) + upper * fraction;
                    displayCurve[displayIndex] += sign * envelope * (route == 0 ? 1.0f : 0.5f);
                }
        }
        if (publishDisplay)
            for (std::size_t displayIndex = 0; displayIndex < displayBinCount; ++displayIndex)
                displayedDynamic[displayIndex].store(displayCurve[displayIndex], std::memory_order_relaxed);
        for (int bin = 0; bin < bins; ++bin)
        {
            const auto index = static_cast<std::size_t>(bin);
            auto left = spectrum[0][index];
            auto right = spectrum[1][index];
            if (gainNeeded[0])
                left *= gain(gainsDb[0][index]);
            if (gainNeeded[1])
                right *= gain(gainsDb[1][index]);
            if (gainNeeded[2] || gainNeeded[3])
            {
                auto mid = 0.5f * (left + right);
                auto side = 0.5f * (left - right);
                if (gainNeeded[2])
                    mid *= gain(gainsDb[2][index]);
                if (gainNeeded[3])
                    side *= gain(gainsDb[3][index]);
                left = mid + side;
                right = mid - side;
            }
            spectrum[0][index] = left;
            spectrum[1][index] = right;
        }
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            currentPlan.transform->backward(spectrum[channel].data(), fftScratch.data());
            for (int sample = 0; sample < size; ++sample)
                output[channel][static_cast<std::size_t>((position + sample) & mask)]
                    += fftScratch[static_cast<std::size_t>(sample)] * currentPlan.window[static_cast<std::size_t>(sample)]
                        * currentPlan.synthesisScale;
        }
    }
};

ADISharedEqSpectralDynamics::ADISharedEqSpectralDynamics() : implementation(std::make_unique<Implementation>()) {}
ADISharedEqSpectralDynamics::~ADISharedEqSpectralDynamics() = default;

void ADISharedEqSpectralDynamics::prepare(double sampleRate, int maximumBlockSize)
{
    static_cast<void>(maximumBlockSize);
    auto& engine = *implementation;
    engine.sampleRate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 48000.0;
    auto baseOrder = 10;
    for (auto rate = engine.sampleRate; rate > 48000.0 && baseOrder < 13; rate *= 0.5)
        ++baseOrder;
    const auto maximumSize = 1 << (baseOrder + 2);
    const auto maximumBins = maximumSize / 2 + 1;
    for (auto* bank : { &engine.input, &engine.keyInput, &engine.output })
        for (auto& values : *bank)
            values.resize(static_cast<std::size_t>(maximumSize));
    for (auto& values : engine.dry)
        values.resize(static_cast<std::size_t>(maximumSize));
    for (auto* bank : { &engine.spectrum, &engine.keySpectrum })
        for (auto& values : *bank)
            values.resize(static_cast<std::size_t>(maximumBins));
    for (auto& values : engine.gainsDb)
        values.resize(static_cast<std::size_t>(maximumBins));
    for (auto& detector : engine.detectors)
        for (auto* bank : { &detector.levelDb, &detector.peakDb, &detector.backgroundDb, &detector.spreadBackgroundDb })
            for (auto& values : *bank)
                values.resize(static_cast<std::size_t>(maximumBins));
    for (auto& state : engine.states)
    {
        state.weight.resize(static_cast<std::size_t>(maximumBins));
        state.staticDb.resize(static_cast<std::size_t>(maximumBins));
        state.rawStaticDb.resize(static_cast<std::size_t>(maximumBins));
        state.envelope.resize(static_cast<std::size_t>(maximumBins));
        state.curveDirty = true;
        state.staticDirty = true;
    }
    engine.fftScratch.resize(static_cast<std::size_t>(maximumSize));
    engine.spreadActivity.resize(static_cast<std::size_t>(maximumBins));
    engine.prefix.resize(static_cast<std::size_t>(maximumBins + 1));
    for (std::size_t resolutionIndex = 0; resolutionIndex < engine.plans.size(); ++resolutionIndex)
    {
        auto& plan = engine.plans[resolutionIndex];
        const auto order = baseOrder + static_cast<int>(resolutionIndex);
        plan.size = 1 << order;
        plan.transform = std::make_unique<adi_eq_dsp::fft::RFFT<float>>(static_cast<std::size_t>(order));
        plan.window.resize(static_cast<std::size_t>(plan.size));
        const auto binCount = static_cast<std::size_t>(plan.size / 2 + 1);
        plan.octave.resize(binCount);
        plan.spreadStep.resize(binCount);
        plan.cosine.resize(binCount);
        plan.doubleCosine.resize(binCount);
        for (int sample = 0; sample < plan.size; ++sample)
            plan.window[static_cast<std::size_t>(sample)] = static_cast<float>(0.5 - 0.5
                * std::cos(2.0 * std::numbers::pi * static_cast<double>(sample) / plan.size));
        for (std::size_t bin = 0; bin < binCount; ++bin)
        {
            const auto frequency = std::max(10.0, static_cast<double>(bin) * engine.sampleRate / plan.size);
            plan.octave[bin] = static_cast<float>(std::log2(frequency / 1000.0));
            const auto binFrequency = static_cast<double>(bin) * engine.sampleRate / plan.size;
            const auto previousFrequency = static_cast<double>(bin == 0 ? 0 : bin - 1) * engine.sampleRate / plan.size;
            plan.spreadStep[bin] = static_cast<float>(440.0
                * (std::sqrt(binFrequency / 1000.0) - std::sqrt(previousFrequency / 1000.0)));
            const auto omega = 2.0 * std::numbers::pi * static_cast<double>(bin) / plan.size;
            plan.cosine[bin] = std::cos(omega);
            plan.doubleCosine[bin] = std::cos(2.0 * omega);
        }
        for (std::size_t displayIndex = 0; displayIndex < displayBinCount; ++displayIndex)
        {
            const auto binPosition = static_cast<double>(displayFrequency(displayIndex, engine.sampleRate))
                * plan.size / engine.sampleRate;
            const auto binIndex = std::clamp(static_cast<int>(binPosition), 0, static_cast<int>(binCount) - 2);
            plan.displayIndices[displayIndex] = binIndex;
            plan.displayFractions[displayIndex] = static_cast<float>(std::clamp(binPosition - binIndex, 0.0, 1.0));
        }
        std::fill(engine.fftScratch.begin(), engine.fftScratch.end(), 0.0f);
        engine.fftScratch[0] = 1.0f;
        plan.transform->forward(engine.fftScratch.data(), engine.spectrum[0].data());
        plan.transform->backward(engine.spectrum[0].data(), engine.fftScratch.data());
        plan.synthesisScale = (2.0f / 3.0f) / engine.fftScratch[0];
    }
    engine.size = engine.plan().size;
    engine.bins = engine.size / 2 + 1;
    for (std::size_t bandIndex = 0; bandIndex < bandCount; ++bandIndex)
        engine.updateBand(bandIndex);
    engine.refreshUsage();
    engine.reset();
}

void ADISharedEqSpectralDynamics::reset() noexcept { implementation->reset(); }

void ADISharedEqSpectralDynamics::setBands(const std::array<Band, bandCount>& bands) noexcept
{
    auto& engine = *implementation;
    const auto wasEnabled = engine.enabled;
    for (std::size_t bandIndex = 0; bandIndex < bandCount; ++bandIndex)
    {
        const auto next = sanitize(bands[bandIndex]);
        if (next == engine.bands[bandIndex])
            continue;
        const auto& previous = engine.bands[bandIndex];
        const auto clearEnvelope = previous.enabled != next.enabled || previous.bypassed != next.bypassed
            || previous.routing != next.routing || previous.externalSidechain != next.externalSidechain;
        const auto updateStatic = previous.frequencyHz != next.frequencyHz || previous.q != next.q
            || previous.shape != next.shape || previous.staticGainDb != next.staticGainDb
            || (!previous.enabled && next.enabled);
        const auto updateShape = updateStatic || previous.rangeDb != next.rangeDb;
        const auto updateTiming = previous.attackMs != next.attackMs || previous.releaseMs != next.releaseMs;
        engine.bands[bandIndex] = next;
        if (clearEnvelope)
            std::fill(engine.states[bandIndex].envelope.begin(), engine.states[bandIndex].envelope.end(), 0.0f);
        if (updateShape)
        {
            engine.states[bandIndex].curveDirty = true;
            engine.states[bandIndex].staticDirty = engine.states[bandIndex].staticDirty || updateStatic;
        }
        else if (updateTiming)
            engine.updateTiming(bandIndex);
    }
    engine.refreshUsage();
    if (wasEnabled != engine.enabled)
        engine.reset();
}

void ADISharedEqSpectralDynamics::setResolution(Resolution resolution) noexcept
{
    auto& engine = *implementation;
    resolution = static_cast<Resolution>(std::clamp(static_cast<int>(resolution), 0, 2));
    if (resolution == engine.resolution)
        return;
    engine.resolution = resolution;
    engine.size = engine.plan().size;
    engine.bins = engine.size / 2 + 1;
    for (std::size_t bandIndex = 0; bandIndex < bandCount; ++bandIndex)
    {
        engine.states[bandIndex].staticDirty = true;
        engine.updateBand(bandIndex);
    }
    engine.reset();
}

void ADISharedEqSpectralDynamics::process(double* const main[2], const double* const sidechain[2],
                               int channels, int samples, bool bypass) noexcept
{
    auto& engine = *implementation;
    if (!engine.enabled || engine.size == 0 || samples <= 0 || channels < 1 || main == nullptr || main[0] == nullptr)
        return;
    channels = std::clamp(channels, 1, 2);
    if (channels == 2 && main[1] == nullptr)
        channels = 1;
    const auto mask = engine.size - 1;
    for (int sample = 0; sample < samples; ++sample)
    {
        const auto position = static_cast<std::size_t>(engine.position);
        const std::array<double, 2> inputValues { main[0][sample], main[channels == 2 ? 1 : 0][sample] };
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto channelIndex = static_cast<std::size_t>(channel);
            const auto inputValue = inputValues[channelIndex];
            const auto safeInput = std::isfinite(inputValue) ? std::clamp(inputValue, -1.0e10, 1.0e10) : 0.0;
            const auto delayed = engine.dry[channelIndex][position];
            engine.dry[channelIndex][position] = safeInput;
            engine.input[channelIndex][position] = static_cast<float>(safeInput);
            if (engine.needsKey)
            {
                auto keyValue = 0.0;
                if (sidechain != nullptr && sidechain[channel < channels ? channel : 0] != nullptr)
                    keyValue = sidechain[channel < channels ? channel : 0][sample];
                engine.keyInput[channelIndex][position] = static_cast<float>(std::isfinite(keyValue)
                    ? std::clamp(keyValue, -1.0e10, 1.0e10) : 0.0);
            }
            if (channel < channels)
                main[channel][sample] = bypass || !engine.anyMask ? delayed
                    : static_cast<double>(engine.output[channelIndex][position]);
            engine.output[channelIndex][position] = 0.0f;
        }
        engine.position = (engine.position + 1) & mask;
        if (++engine.hopPosition == engine.size / 4)
        {
            engine.hopPosition = 0;
            engine.frame();
        }
    }
}

int ADISharedEqSpectralDynamics::latencySamples() const noexcept
{
    return implementation->enabled ? implementation->size : 0;
}

int ADISharedEqSpectralDynamics::fftSize() const noexcept { return implementation->size; }
int ADISharedEqSpectralDynamics::preparedFftSize(Resolution resolution) const noexcept {
    return implementation->plans[static_cast<std::size_t>(std::clamp(static_cast<int>(resolution),0,2))].size;
}
bool ADISharedEqSpectralDynamics::isEnabled() const noexcept { return implementation->enabled; }

void ADISharedEqSpectralDynamics::setDisplayEnabled(bool enabled) noexcept
{
    implementation->displayEnabled.store(enabled, std::memory_order_relaxed);
}

void ADISharedEqSpectralDynamics::copyDisplay(std::array<float, displayBinCount>& dynamicDb,
                                   std::array<float, bandCount>& bandPeakChangeDb) const noexcept
{
    for (std::size_t index = 0; index < displayBinCount; ++index)
        dynamicDb[index] = implementation->displayedDynamic[index].load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < bandCount; ++index)
        bandPeakChangeDb[index] = implementation->displayedPeakChange[index].load(std::memory_order_relaxed);
}

float ADISharedEqSpectralDynamics::displayFrequency(std::size_t index, double sampleRate) noexcept
{
    const auto rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 48000.0;
    const auto upper = std::min(30000.0, rate * 0.5);
    const auto position = static_cast<double>(std::min(index, displayBinCount - 1))
        / static_cast<double>(displayBinCount - 1);
    return static_cast<float>(10.0 * std::pow(upper / 10.0, position));
}

void ADISharedEqSpectralDynamics::calculateStaticResponse(const Band& band, double sampleRate,
                                               const float* frequencies, float* decibelValues, int count) noexcept
{
    if (frequencies == nullptr || decibelValues == nullptr || count <= 0)
        return;
    const auto cleanBand = sanitize(band);
    const auto rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 48000.0;
    const BiquadMagnitude response(cleanBand, rate);
    for (int index = 0; index < count; ++index)
    {
        const auto frequency = bounded(frequencies[index], 0.0f, static_cast<float>(rate * 0.5), 0.0f);
        const auto omega = 2.0 * std::numbers::pi * frequency / rate;
        decibelValues[index] = cleanBand.staticGainDb == 0.0f ? 0.0f
            : response.response(std::cos(omega), std::cos(2.0 * omega));
    }
}
