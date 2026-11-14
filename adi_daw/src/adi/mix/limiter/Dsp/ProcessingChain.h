// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "FiniteSample.h"

#include "StyleDispatch.h"
#include "Multirate.h"
#include "TruePeakDetector.h"
#include "OvershootCorrection.h"
#include "DcBlocker.h"
#include "Dither.h"
#include "FloatTarget.h"
#include "../DisplayTelemetry.h"

namespace adi::mix::limiter
{
struct ChainSettings
{
    Style style = Style::modern;
    adi::mix::limiter::programs::Controls controls{-0.001f, 0.25f, 0.5f, 0.04f};
    int oversampling = 1;
    bool truePeak = false, dcRemoval = false, unity = false, audition = false, bypass = false, externalSidechain = false;
    float inputGainDb = 0.0f, outputGainDb = 0.0f;
    float transientLink = 0.5f, releaseLink = 0.5f;
    int ditherBits = 0;
    NoiseShape noiseShape = NoiseShape::none;
};

class ProcessingChain
{
public:
    void prepare(double sampleRate, int channels, int maximumFrames, ChainSettings configuration)
    {
        if (maximumFrames < 1 || maximumFrames > 2048)
            throw std::invalid_argument("Processing chunk outside supported capacity");
        settings = configuration;
        settings.controls.threshold_db = -std::max(settings.inputGainDb, 0.001f);
        rate = sampleRate;
        channelCount = channels;
        maximumBlock = maximumFrames;
        factor = settings.oversampling;
        latency = processingLatency(settings.style, rate, factor, settings.truePeak);
        if (latency < 0) throw std::invalid_argument("Invalid processing configuration");
        const auto internalRate = rate * factor;
        engine.prepare(internalRate, static_cast<std::size_t>(channels));
        engine.setLinkControls(static_cast<int>(settings.style), settings.transientLink, settings.releaseLink);
        const auto silent = Frame::channels(static_cast<std::size_t>(channels));
        processStyle(settings.style, engine, settings.controls, silent, silent);
        engine.reset();
        if (channels == 2) stereoBlockEngine.prepare(2);
        else blockEngine.prepare(static_cast<std::size_t>(channels));
        blockAudio.resize(BlockEngine::maximumFrames * static_cast<std::size_t>(channels));
        blockMain = blockAudio;
        blockKey = blockAudio;
        converter.prepare(rate, channels, maximumFrames, factor);
        mainConverter.prepare(rate, channels, maximumFrames, factor);
        scratch.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(maximumFrames)));
        mainScratch = scratch;
        frameControls.resize(static_cast<std::size_t>(maximumFrames));
        peaks.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(maximumFrames * factor)));
        sourcePointers.resize(static_cast<std::size_t>(channels));
        destinationPointers.resize(static_cast<std::size_t>(channels));
        peakPointers.resize(static_cast<std::size_t>(channels));
        mainSourcePointers.resize(static_cast<std::size_t>(channels));
        mainDestinationPointers.resize(static_cast<std::size_t>(channels));
        for (int channel = 0; channel < channels; ++channel)
        {
            sourcePointers[static_cast<std::size_t>(channel)] = scratch[static_cast<std::size_t>(channel)].data();
            destinationPointers[static_cast<std::size_t>(channel)] = scratch[static_cast<std::size_t>(channel)].data();
            peakPointers[static_cast<std::size_t>(channel)] = peaks[static_cast<std::size_t>(channel)].data();
            mainSourcePointers[static_cast<std::size_t>(channel)] = mainScratch[static_cast<std::size_t>(channel)].data();
            mainDestinationPointers[static_cast<std::size_t>(channel)] = mainScratch[static_cast<std::size_t>(channel)].data();
        }
        inputPeakDelay = outputPeakDelay = 0;
        if (settings.truePeak)
        {
            inputDetector.prepare(internalRate, channels, maximumFrames * factor);
            outputDetector.prepare(rate, channels, maximumFrames);
            inputPeakDelay = inputDetector.getLatencySamples();
            outputPeakDelay = outputDetector.getLatencySamples();
        }
        compensationLatency = processingLatency(settings.style, internalRate, 1, false);
        compensationDelay.prepare(static_cast<std::size_t>(compensationLatency));
        const auto coreLatency = compensationLatency + inputPeakDelay;
        internalPadding = (factor - coreLatency % factor) % factor;
        dcFilters.resize(static_cast<std::size_t>(channels));
        mainDcFilters.resize(static_cast<std::size_t>(channels));
        inputDelays.resize(static_cast<std::size_t>(channels));
        mainInputDelays.resize(static_cast<std::size_t>(channels));
        roundingDelays.resize(static_cast<std::size_t>(channels));
        mainRoundingDelays.resize(static_cast<std::size_t>(channels));
        outputDelays.resize(static_cast<std::size_t>(channels));
        dryDelays.resize(static_cast<std::size_t>(channels));
        corrections.resize(static_cast<std::size_t>(channels));
        for (int channel = 0; channel < channels; ++channel)
        {
            dcFilters[static_cast<std::size_t>(channel)].prepare(rate);
            mainDcFilters[static_cast<std::size_t>(channel)].prepare(rate);
            inputDelays[static_cast<std::size_t>(channel)].prepare(static_cast<std::size_t>(inputPeakDelay));
            mainInputDelays[static_cast<std::size_t>(channel)].prepare(static_cast<std::size_t>(inputPeakDelay));
            roundingDelays[static_cast<std::size_t>(channel)].prepare(static_cast<std::size_t>(internalPadding));
            mainRoundingDelays[static_cast<std::size_t>(channel)].prepare(static_cast<std::size_t>(internalPadding));
            corrections[static_cast<std::size_t>(channel)].prepare(rate);
            outputDelays[static_cast<std::size_t>(channel)].prepare(static_cast<std::size_t>(outputPeakDelay) + corrections[static_cast<std::size_t>(channel)].getLatencySamples());
            dryDelays[static_cast<std::size_t>(channel)].prepare(static_cast<std::size_t>(latency));
        }
        displayInternalReduction.resize(static_cast<std::size_t>(maximumFrames * factor));
        displayPadding.prepare(static_cast<std::size_t>(internalPadding));
        displayReductionLatency = static_cast<int>(static_cast<std::size_t>(converter.getLatencySamples() / 2)
            + ((factor > 1 || settings.truePeak) ? static_cast<std::size_t>(outputPeakDelay) + corrections.front().getLatencySamples() : 0));
        displayReductionDelay.prepare(static_cast<std::size_t>(displayReductionLatency));
        inputGain = static_cast<float>(std::pow(10.0, std::max(0.0f, settings.inputGainDb - 0.001f) / 20.0));
        outputGain = static_cast<float>(std::pow(10.0, settings.outputGainDb / 20.0));
        unityGain = static_cast<float>(std::pow(10.0, -(settings.inputGainDb + settings.outputGainDb) / 20.0));
        const auto ditherCeiling = settings.unity ? static_cast<float>(std::pow(10.0, -settings.inputGainDb / 20.0)) : outputGain;
        dither.prepare(static_cast<std::size_t>(channels), settings.ditherBits, settings.noiseShape, ditherCeiling);
        const std::array initial{settings.inputGainDb, settings.outputGainDb - 0.001f, settings.controls.threshold_db,
            settings.controls.attack_seconds, settings.controls.release_seconds, settings.controls.lookahead_fraction,
            settings.transientLink, settings.releaseLink, float(settings.dcRemoval), float(settings.externalSidechain),
            float(settings.unity), float(settings.audition), float(settings.bypass)};
        constexpr std::array lower{0.0f, -30.001f, -60.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        constexpr std::array upper{30.0f, 0.0f, 60.0f, 10.0f, 10.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        for (std::size_t index = 0; index < ramps.size(); ++index)
        {
            const auto controlRate = index >= 1 && index <= 7 ? internalRate : rate;
            const auto adaptiveRange = index == 0 || index == 2 ? 120.0f : index == 1 ? 30.0f : index == 3 || index == 4 ? 1000.0f : 0.0f;
            ramps[index].prepare(static_cast<float>(controlRate), lower[index], upper[index], adaptiveRange);
            ramps[index].reset(initial[index]);
        }
        meterReduction = 0.0f;
        shadowSynchronizationBytes = converter.synchronizationBytes() + engine.shadowSynchronizationBytes();
        for (int channel = 0; channel < channelCount; ++channel)
            shadowSynchronizationBytes += inputDelays[static_cast<std::size_t>(channel)].synchronizationBytes() + roundingDelays[static_cast<std::size_t>(channel)].synchronizationBytes();
        shadowSynchronizationBudget = shadowCopyByteBudget(rate, maximumBlock);
        shadowAliased = !settings.externalSidechain && isShadowAliasEligible();
        cachedDrive = cachedCeiling = cachedCompensationDb = std::numeric_limits<float>::quiet_NaN();
    }

    void update(ChainSettings configuration) noexcept
    {
        configuration.style = settings.style;
        configuration.oversampling = settings.oversampling;
        configuration.truePeak = settings.truePeak;
        if (configuration.ditherBits != settings.ditherBits || configuration.noiseShape != settings.noiseShape)
            dither.configure(configuration.ditherBits, configuration.noiseShape, outputGain);
        settings = configuration;
        settings.controls.threshold_db = -std::max(settings.inputGainDb, 0.001f);
        const std::array targets{settings.inputGainDb, settings.outputGainDb - 0.001f, settings.controls.threshold_db,
            settings.controls.attack_seconds, settings.controls.release_seconds, settings.controls.lookahead_fraction,
            settings.transientLink, settings.releaseLink, float(settings.dcRemoval), float(settings.externalSidechain),
            float(settings.unity), float(settings.audition), float(settings.bypass)};
        for (std::size_t index = 0; index < ramps.size(); ++index)
            ramps[index].setTarget(targets[index], true, index >= 8 ? 20.0f : -1.0f);
    }

    void serviceKernelUpdates() noexcept { engine.serviceKernelUpdates(); }
    bool needsKernelService() const noexcept { return engine.needsKernelService(); }
    void setDisplayBuffer(DisplaySample* buffer) noexcept
    {
        if (buffer && !displayBuffer)
        {
            displayPadding.reset();
            displayReductionDelay.reset();
        }
        displayBuffer = buffer;
    }
    void setChannelGroups(std::span<const EngineBank::ChannelGroup> groups) { engine.setChannelGroups(groups); }

    void reset() noexcept
    {
        engine.reset();
        compensationDelay.reset();
        converter.reset();
        mainConverter.reset();
        if (settings.truePeak) { inputDetector.reset(); outputDetector.reset(); }
        for (auto& filter : dcFilters) filter.reset();
        for (auto& filter : mainDcFilters) filter.reset();
        for (auto* collection : {&inputDelays, &mainInputDelays, &roundingDelays, &mainRoundingDelays, &outputDelays, &dryDelays})
            for (auto& delay : *collection) delay.reset();
        for (auto& correction : corrections) correction.reset();
        dither.reset();
        for (auto& ramp : ramps) ramp.reset(ramp.destination());
        meterReduction = 0.0f;
        displayPadding.reset();
        displayReductionDelay.reset();
        shadowAliased = !settings.externalSidechain && isShadowAliasEligible();
    }

    bool process(const float* const* input, float* const* output, int frames, const float* const* sidechain = nullptr)
    {
        if (input == nullptr || output == nullptr || frames < 0 || frames > maximumBlock)
            return false;
        if (frames > 0 && shadowAliased && settings.externalSidechain)
        {
            if (!mainConverter.synchronizeStateFrom(converter) || !engine.synchronizeShadowAudio()) return false;
            for (int channel = 0; channel < channelCount; ++channel)
                if (!mainInputDelays[static_cast<std::size_t>(channel)].synchronizeStateFrom(inputDelays[static_cast<std::size_t>(channel)])
                    || !mainRoundingDelays[static_cast<std::size_t>(channel)].synchronizeStateFrom(roundingDelays[static_cast<std::size_t>(channel)])) return false;
            shadowAliased = false;
            ++forkCount;
        }
        return shadowAliased ? processRoute<true>(input, output, frames, sidechain)
                             : processRoute<false>(input, output, frames, sidechain);
    }

    template<bool Aliased>
    bool processRoute(const float* const* input, float* const* output, int frames, const float* const* sidechain)
    {
        if (channelCount == 2)
            return displayBuffer ? processRouteBlocks<Aliased, true>(input, output, frames, sidechain, stereoBlockEngine)
                                 : processRouteBlocks<Aliased, false>(input, output, frames, sidechain, stereoBlockEngine);
        return displayBuffer ? processRouteBlocks<Aliased, true>(input, output, frames, sidechain, blockEngine)
                             : processRouteBlocks<Aliased, false>(input, output, frames, sidechain, blockEngine);
    }

    template<bool Aliased, bool Capture, typename Blocks>
    bool processRouteBlocks(const float* const* input, float* const* output, int frames,
                            const float* const* sidechain, Blocks& blockEngine)
    {
        const auto processingChannels = Blocks::fixedChannelCount != 0
            ? static_cast<int>(Blocks::fixedChannelCount) : channelCount;
        blockEngine.setShadowAliased(Aliased);
        for (auto& ramp : ramps) ramp.beginBlock();
        meterReduction = 0.0f;
        if constexpr (Capture)
        {
            std::fill_n(displayBuffer, frames, DisplaySample{});
            std::fill_n(displayInternalReduction.begin(), frames * factor, 0.0f);
        }
        const bool fixedFrameControls = ramps[0].remaining() == 0 && ramps[8].remaining() == 0
            && ramps[9].remaining() == 0 && ramps[10].remaining() == 0
            && ramps[11].remaining() == 0 && ramps[12].remaining() == 0;
        for (int frame = 0; frame < (fixedFrameControls ? std::min(1, frames) : frames); ++frame)
        {
            auto& control = frameControls[static_cast<std::size_t>(frame)];
            const auto drive = ramps[0].process();
            const auto ceiling = settings.outputGainDb;
            if (drive != cachedDrive || ceiling != cachedCeiling)
            {
                cachedDrive = drive;
                cachedCeiling = ceiling;
                cachedInputGain = static_cast<float>(std::pow(10.0, std::max(0.0f, drive - 0.001f) / 20.0));
                cachedOutputGain = static_cast<float>(std::pow(10.0, ceiling / 20.0));
                cachedUnityGain = static_cast<float>(std::pow(10.0, -(drive + ceiling) / 20.0));
                cachedUnityCeiling = static_cast<float>(std::pow(10.0, -drive / 20.0));
            }
            control.inputGain = cachedInputGain;
            control.outputGain = cachedOutputGain;
            control.unityGain = cachedUnityGain;
            control.dcMix = ramps[8].process();
            control.sidechainMix = ramps[9].process();
            control.unityMix = ramps[10].process();
            control.auditionMix = ramps[11].process();
            control.bypassMix = ramps[12].process();
            control.ditherCeiling = blend(control.outputGain, cachedUnityCeiling, control.unityMix);
        }
        if (fixedFrameControls && frames > 1)
            std::fill_n(frameControls.begin() + 1, frames - 1, frameControls.front());
        for (int channel = 0; channel < processingChannels; ++channel)
        {
            if (!input[channel] || !output[channel]) return false;
            for (int frame = 0; frame < frames; ++frame)
            {
                const auto& control = frameControls[static_cast<std::size_t>(frame)];
                const auto raw = finiteSample(input[channel][frame]);
                const auto external = sidechain && sidechain[channel] ? finiteSample(sidechain[channel][frame]) : 0.0f;
                const auto mainSample = blend(raw, mainDcFilters[static_cast<std::size_t>(channel)].process(raw), control.dcMix);
                const auto keySample = blend(external, dcFilters[static_cast<std::size_t>(channel)].process(external), control.dcMix);
                scratch[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] = blend(mainSample, keySample, control.sidechainMix);
                if constexpr (!Aliased) mainScratch[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] = mainSample;
            }
        }
        const auto internal = converter.upsample(sourcePointers.data(), frames);
        const auto mainInternal = [&]
        {
            if constexpr (Aliased) return internal;
            else return mainConverter.upsample(mainSourcePointers.data(), frames);
        }();
        if (!internal || !mainInternal) return false;
        if (settings.truePeak && !inputDetector.process(internal, peakPointers.data(), frames * factor, 0.004f))
            return false;
        const auto count = static_cast<std::size_t>(processingChannels);
        for (int beginning = 0; beginning < frames * factor; beginning += static_cast<int>(BlockEngine::maximumFrames))
        {
            const auto chunk = std::min(BlockEngine::maximumFrames, static_cast<std::size_t>(frames * factor - beginning));
            std::array<bool, 6> fixedControls{};
            for (std::size_t control = 0; control < blockControls.size(); ++control)
            {
                auto& ramp = ramps[control + 2];
                fixedControls[control] = ramp.remaining() == 0;
                if (fixedControls[control])
                    // Attack, release, and lookahead become scalar Frames below;
                    // threshold compensation and link controls remain sample-indexed.
                    std::fill_n(blockControls[control].data(), control > 0 && control < 4 ? 1 : chunk, ramp.destination());
                else
                    for (std::size_t sample = 0; sample < chunk; ++sample)
                        blockControls[control][sample] = ramp.process();
            }
            for (std::size_t sample = 0; sample < chunk; ++sample)
            {
                for (std::size_t channel = 0; channel < count; ++channel)
                {
                    const auto frame = beginning + static_cast<int>(sample);
                    blockKey[sample * count + channel] = settings.truePeak ? peaks[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] : internal[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)];
                    blockAudio[sample * count + channel] = inputPeakDelay == 0 ? internal[channel][frame] : inputDelays[channel].process(internal[channel][frame], static_cast<std::size_t>(inputPeakDelay));
                    if constexpr (!Aliased)
                        blockMain[sample * count + channel] = inputPeakDelay == 0 ? mainInternal[channel][frame] : mainInputDelays[channel].process(mainInternal[channel][frame], static_cast<std::size_t>(inputPeakDelay));
                }
            }
            // These histories are prepared with exactly inputPeakDelay;
            // when zero, their one slot and cursor need only the last sample.
            // Commit before downstream processing can reuse blockAudio.
            if (inputPeakDelay == 0 && chunk != 0)
                for (std::size_t channel = 0; channel < count; ++channel)
                {
                    static_cast<void>(inputDelays[channel].process(blockAudio[(chunk - 1) * count + channel], 0));
                    if constexpr (!Aliased)
                        static_cast<void>(mainInputDelays[channel].process(blockMain[(chunk - 1) * count + channel], 0));
                }
            blocked::Controls controls;
            controls.threshold_db.values = blockControls[0].data();
            controls.attack_seconds.values = blockControls[1].data();
            controls.release_seconds.values = blockControls[2].data();
            controls.lookahead_fraction.values = blockControls[3].data();
            std::size_t controlIndex = 0;
            for (auto* control : {&controls.threshold_db, &controls.attack_seconds, &controls.release_seconds, &controls.lookahead_fraction})
            {
                if (fixedControls[controlIndex++] || std::all_of(control->values, control->values + chunk, [&](float value) { return value == control->values[0]; }))
                {
                    control->scalar = control->values[0];
                    control->values = nullptr;
                }
            }
            blocked::Frame audio, key;
            audio.values = blockAudio.data();
            audio.shadowValues = Aliased ? blockAudio.data() : blockMain.data();
            audio.count = count;
            audio.hasShadow = true;
            key.values = blockKey.data();
            key.count = count;
            blockEngine.begin(chunk, blockControls[4].data(), blockControls[5].data());
            const auto processed = processStyleBlock(settings.style, blockEngine, controls, audio, key);
            for (std::size_t sample = 0; sample < chunk; ++sample)
            {
                const auto outputSource = ramps[1].process();
                const auto compensationDb = outputSource - blockControls[0][sample];
                if (compensationDb != cachedCompensationDb)
                {
                    cachedCompensationDb = compensationDb;
                    cachedCompensationAmplitude = static_cast<float>(std::exp(static_cast<double>(compensationDb) * 0.1151292546497023));
                }
                const auto compensation = compensationDelay.process(cachedCompensationAmplitude, static_cast<std::size_t>(compensationLatency));
                for (std::size_t channel = 0; channel < count; ++channel)
                {
                    const auto frame = beginning + static_cast<int>(sample);
                    internal[channel][frame] = internalPadding == 0 ? processed.at(sample, channel) * compensation : roundingDelays[channel].process(processed.at(sample, channel) * compensation, static_cast<std::size_t>(internalPadding));
                    if constexpr (!Aliased)
                        mainInternal[channel][frame] = internalPadding == 0 ? processed.shadow(sample, channel) * compensation : mainRoundingDelays[channel].process(processed.shadow(sample, channel) * compensation, static_cast<std::size_t>(internalPadding));
                    meterReduction = std::max(meterReduction, blockEngine.reduction(sample, channel));
                    if constexpr (Capture)
                        displayInternalReduction[static_cast<std::size_t>(frame)] = std::max(displayInternalReduction[static_cast<std::size_t>(frame)], blockEngine.reduction(sample, channel));
                }
            }
            // internalPadding is immutable between prepare calls. Preserve
            // exactly the original one-slot state at every chunk boundary.
            if (internalPadding == 0 && chunk != 0)
                for (std::size_t channel = 0; channel < count; ++channel)
                {
                    const auto last = beginning + static_cast<int>(chunk) - 1;
                    static_cast<void>(roundingDelays[channel].process(internal[channel][last], 0));
                    if constexpr (!Aliased)
                        static_cast<void>(mainRoundingDelays[channel].process(mainInternal[channel][last], 0));
                }
        }
        engine.finishBlock();
        if constexpr (Capture)
            for (int frame = 0; frame < frames; ++frame)
            {
                float reduction = 0;
                for (int phase = 0; phase < factor; ++phase)
                    reduction = std::max(reduction, displayPadding.process(displayInternalReduction[static_cast<std::size_t>(frame * factor + phase)], static_cast<std::size_t>(internalPadding)));
                displayBuffer[frame].reduction = displayReductionDelay.process(reduction, static_cast<std::size_t>(displayReductionLatency));
            }
        if (!converter.downsample(destinationPointers.data(), frames)) return false;
        if constexpr (!Aliased)
            if (!mainConverter.downsample(mainDestinationPointers.data(), frames)) return false;
        if (settings.truePeak && !outputDetector.process(sourcePointers.data(), peakPointers.data(), frames))
            return false;
        const auto correctionEnabled = factor > 1 || settings.truePeak;
        bool outputAlreadyClipped = false;
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
        if constexpr (Blocks::fixedChannelCount == 2)
            if (!correctionEnabled)
            {
                clipOutputStereo<Aliased, Capture>(frames);
                outputAlreadyClipped = true;
            }
#endif
        for (int frame = 0; frame < frames; ++frame)
        {
            const auto& control = frameControls[static_cast<std::size_t>(frame)];
            dither.setCeiling(control.ditherCeiling);
            for (int channel = 0; channel < processingChannels; ++channel)
            {
                auto sample = Aliased ? scratch[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] : mainScratch[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)];
                if (correctionEnabled)
                {
                    const auto peak = settings.truePeak ? peaks[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] : std::abs(scratch[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
                    const auto gain = corrections[static_cast<std::size_t>(channel)].process(peak, control.outputGain);
                    meterReduction = std::max(meterReduction, 1.0f - gain);
                    if constexpr (Capture) displayBuffer[frame].reduction = std::max(displayBuffer[frame].reduction, 1.0f - gain);
                    sample = outputDelays[static_cast<std::size_t>(channel)].process(sample, static_cast<std::size_t>(outputPeakDelay) + corrections[static_cast<std::size_t>(channel)].getLatencySamples()) * gain;
                }
                else if (!outputAlreadyClipped)
                {
                    const auto magnitude = std::abs(scratch[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
                    const auto gain = magnitude > control.outputGain ? control.outputGain / magnitude : 1.0f;
                    meterReduction = std::max(meterReduction, 1.0f - gain);
                    if constexpr (Capture) displayBuffer[frame].reduction = std::max(displayBuffer[frame].reduction, 1.0f - gain);
                    sample *= gain;
                }
                const auto raw = finiteSample(input[channel][frame]);
                const auto dry = dryDelays[static_cast<std::size_t>(channel)].process(raw, static_cast<std::size_t>(latency));
                if constexpr (Capture)
                    displayBuffer[frame].inputPeak = std::max(displayBuffer[frame].inputPeak, std::abs(dry * control.inputGain));
                const auto delta = dry - sample / (control.inputGain * control.outputGain);
                sample = blend(sample, sample * control.unityGain, control.unityMix);
                sample = blend(sample, delta, control.auditionMix);
                if (control.bypassMix < 1.0f) sample = dither.process(static_cast<std::size_t>(channel), sample);
                output[channel][frame] = blend(sample, dry, control.bypassMix);
            }
            if constexpr (Capture) displayBuffer[frame].reduction *= 1.0f - control.bypassMix;
        }
        return true;
    }

    int getLatencySamples() const noexcept { return latency; }
    float getGainReductionDb() const noexcept { return -20.0f * std::log10(std::max(1.0e-8f, 1.0f - meterReduction)); }
    int getForkCount() const noexcept { return forkCount; }
    std::size_t getShadowSynchronizationBytes() const noexcept { return shadowSynchronizationBytes; }
    std::size_t getShadowSynchronizationBudget() const noexcept { return shadowSynchronizationBudget; }
    bool isShadowAliasEligible() const noexcept { return shadowSynchronizationBytes <= shadowSynchronizationBudget; }
    static std::size_t shadowCopyByteBudget(double sampleRate, int maximumFrames) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maximumFrames < 1) return 0;
        const auto nominalBudget = nominalShadowCopyBytesPerSecond * static_cast<double>(maximumFrames) / sampleRate;
        return static_cast<std::size_t>(std::min(static_cast<double>(maximumShadowSynchronizationBytes), nominalBudget));
    }

private:
#if !defined(ADI_DSP_FORCE_SCALAR) && (defined(__SSE2__) || defined(_M_X64))
    template<bool Aliased, bool Capture>
    void clipOutputStereo(int frames) noexcept
    {
        const auto ones = _mm_set1_ps(1.0f);
        const auto signless = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        auto maximum = _mm_setzero_ps();
        for (int channel = 0; channel < 2; ++channel)
        {
            auto* key = scratch[static_cast<std::size_t>(channel)].data();
            auto* samples = Aliased ? key : mainScratch[static_cast<std::size_t>(channel)].data();
            int frame = 0;
            for (; frame + 4 <= frames; frame += 4)
            {
                const auto magnitude = _mm_and_ps(_mm_loadu_ps(key + frame), signless);
                const auto ceiling = _mm_set_ps(frameControls[static_cast<std::size_t>(frame + 3)].outputGain,
                    frameControls[static_cast<std::size_t>(frame + 2)].outputGain, frameControls[static_cast<std::size_t>(frame + 1)].outputGain,
                    frameControls[static_cast<std::size_t>(frame)].outputGain);
                const auto over = _mm_cmpgt_ps(magnitude, ceiling);
                const auto denominator = _mm_or_ps(_mm_and_ps(over, magnitude), _mm_andnot_ps(over, ones));
                const auto ratio = _mm_div_ps(ceiling, denominator);
                const auto gain = _mm_or_ps(_mm_and_ps(over, ratio), _mm_andnot_ps(over, ones));
                _mm_storeu_ps(samples + frame, _mm_mul_ps(_mm_loadu_ps(samples + frame), gain));
                // Keep std::max's existing-value preference for a NaN candidate.
                maximum = _mm_max_ps(_mm_sub_ps(ones, gain), maximum);
                if constexpr (Capture)
                {
                    alignas(16) float values[4];
                    _mm_store_ps(values, _mm_sub_ps(ones, gain));
                    for (int lane = 0; lane < 4; ++lane)
                        displayBuffer[frame + lane].reduction = std::max(displayBuffer[frame + lane].reduction, values[lane]);
                }
            }
            for (; frame < frames; ++frame)
            {
                const auto magnitude = std::abs(key[frame]);
                const auto ceiling = frameControls[static_cast<std::size_t>(frame)].outputGain;
                const auto gain = magnitude > ceiling ? ceiling / magnitude : 1.0f;
                samples[frame] *= gain;
                meterReduction = std::max(meterReduction, 1.0f - gain);
                if constexpr (Capture) displayBuffer[frame].reduction = std::max(displayBuffer[frame].reduction, 1.0f - gain);
            }
        }
        alignas(16) float reductions[4];
        _mm_store_ps(reductions, maximum);
        for (const auto reduction : reductions)
            meterReduction = std::max(meterReduction, reduction);
    }
#endif
    static float blend(float first, float second, float mix) noexcept
    {
        if (mix <= 0.0f) return first;
        if (mix >= 1.0f) return second;
        return first + mix * (second - first);
    }
    struct FrameControl
    {
        float inputGain, outputGain, unityGain, dcMix, sidechainMix, unityMix, auditionMix, bypassMix, ditherCeiling;
    };
    std::array<FloatTarget, 13> ramps;
    std::vector<FrameControl> frameControls;
    ChainSettings settings;
    EngineBank engine;
    BlockEngine blockEngine{engine};
    StereoBlockEngine stereoBlockEngine{engine};
    std::array<std::array<float, BlockEngine::maximumFrames>, 6> blockControls;
    std::vector<float> blockAudio, blockMain, blockKey;
    SampleHistory compensationDelay;
    int compensationLatency = 0;
    Multirate converter, mainConverter;
    TruePeakDetector inputDetector, outputDetector;
    Dither dither;
    std::vector<DcBlocker> dcFilters, mainDcFilters;
    std::vector<SampleHistory> inputDelays, mainInputDelays, roundingDelays, mainRoundingDelays, outputDelays, dryDelays;
    std::vector<OvershootCorrection> corrections;
    std::vector<std::vector<float>> scratch, mainScratch, peaks;
    std::vector<const float*> sourcePointers, mainSourcePointers;
    std::vector<float*> destinationPointers, mainDestinationPointers, peakPointers;
    double rate = 48000.0;
    int channelCount = 2, maximumBlock = 0, factor = 1, latency = 0;
    int inputPeakDelay = 0, outputPeakDelay = 0, internalPadding = 0;
    float inputGain = 1.0f, outputGain = 1.0f, unityGain = 1.0f;
    float cachedDrive = 0.0f, cachedCeiling = 0.0f, cachedCompensationDb = 0.0f;
    float cachedInputGain = 1.0f, cachedOutputGain = 1.0f, cachedUnityGain = 1.0f, cachedUnityCeiling = 1.0f;
    float cachedCompensationAmplitude = 1.0f;
    float meterReduction = 0.0f;
    DisplaySample* displayBuffer = nullptr;
    std::vector<float> displayInternalReduction;
    SampleHistory displayPadding, displayReductionDelay;
    int displayReductionLatency = 0;
    bool shadowAliased = false;
    int forkCount = 0;
    std::size_t shadowSynchronizationBytes = 0;
    std::size_t shadowSynchronizationBudget = 0;
    static constexpr std::size_t maximumShadowSynchronizationBytes = 2 * 1024 * 1024;
    static constexpr double nominalShadowCopyBytesPerSecond = 64.0 * 1024.0 * 1024.0;
};
}
