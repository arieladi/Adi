// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DspEngine.h"
#include "BackgroundOwner.h"
#include "Dsp/ChannelGroups.h"
#include "Dsp/ProcessingChain.h"
#include "LoudnessMeter.h"
#include <atomic>
#include <cstdint>

namespace adi::mix::limiter
{
namespace
{
constexpr int channelCapacity = MeterSnapshot::maximumChannels;
constexpr std::size_t settingCount = 20;

float finite(float value, float fallback, float minimum, float maximum) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

EngineSettings validated(EngineSettings value) noexcept
{
    value.styleIndex = std::clamp(value.styleIndex, 0, 7);
    value.inputGainDb = finite(value.inputGainDb, 0.0f, 0.0f, 30.0f);
    value.outputGainDb = finite(value.outputGainDb, 0.0f, -30.0f, 0.0f);
    value.lookaheadMs = finite(value.lookaheadMs, 0.2f, 0.0f, 5.0f);
    value.attackMs = finite(value.attackMs, 250.0f, 0.0f, 10000.0f);
    value.releaseMs = finite(value.releaseMs, 500.0f, 0.0f, 10000.0f);
    value.transientLink = finite(value.transientLink, 0.5f, 0.0f, 1.0f);
    value.releaseLink = finite(value.releaseLink, 0.5f, 0.0f, 1.0f);
    if (value.oversampling < 1 || value.oversampling > 32
        || !std::has_single_bit(static_cast<unsigned>(value.oversampling))) value.oversampling = 1;
    if (value.ditherBits != 0 && value.ditherBits != 16 && value.ditherBits != 18
        && value.ditherBits != 20 && value.ditherBits != 22 && value.ditherBits != 24) value.ditherBits = 0;
    value.noiseShape = std::clamp(value.noiseShape, 0, 3);
    return value;
}

ChainSettings chainSettings(const EngineSettings& value)
{
    ChainSettings result;
    result.style = static_cast<Style>(value.styleIndex);
    result.controls = {-0.001f, value.attackMs * 0.001f, value.releaseMs * 0.001f, value.lookaheadMs / 5.0f};
    result.inputGainDb = value.inputGainDb;
    result.outputGainDb = value.outputGainDb;
    result.transientLink = value.transientLink;
    result.releaseLink = value.releaseLink;
    result.oversampling = value.oversampling;
    result.truePeak = value.truePeak;
    result.dcRemoval = value.dcRemoval;
    result.externalSidechain = value.externalSidechain;
    result.unity = value.unity;
    result.audition = value.audition;
    result.bypass = value.bypass;
    result.ditherBits = value.ditherBits;
    result.noiseShape = static_cast<NoiseShape>(value.noiseShape);
    return result;
}

std::uint64_t structure(const EngineSettings& value) noexcept
{
    return static_cast<std::uint64_t>(value.styleIndex)
        | (static_cast<std::uint64_t>(value.oversampling) << 4)
        | (static_cast<std::uint64_t>(value.truePeak) << 10);
}

float decibels(float value) noexcept
{
    return value > 1.0e-8f ? std::max(-160.0f, 20.0f * std::log10(value)) : -160.0f;
}

struct SettingsMailbox
{
    std::atomic<std::uint64_t> sequence{0};
    std::array<std::atomic<float>, settingCount> values{};

    void write(const EngineSettings& value) noexcept
    {
        const std::array<float, settingCount> data{static_cast<float>(value.styleIndex), value.inputGainDb,
            value.outputGainDb, value.lookaheadMs, value.attackMs, value.releaseMs, value.transientLink,
            value.releaseLink, static_cast<float>(value.oversampling), static_cast<float>(value.truePeak),
            static_cast<float>(value.dcRemoval), static_cast<float>(value.externalSidechain), static_cast<float>(value.unity),
            static_cast<float>(value.audition), static_cast<float>(value.bypass), static_cast<float>(value.ditherBits),
            static_cast<float>(value.noiseShape), static_cast<float>(value.centerLink), static_cast<float>(value.lfeLink), 0.0f};
        sequence.fetch_add(1, std::memory_order_seq_cst);
        for (std::size_t index = 0; index < data.size(); ++index) values[index].store(data[index], std::memory_order_seq_cst);
        sequence.fetch_add(1, std::memory_order_seq_cst);
    }

    bool read(EngineSettings& value) const noexcept
    {
        const auto before = sequence.load(std::memory_order_seq_cst);
        if ((before & 1) != 0) return false;
        std::array<float, settingCount> data;
        for (std::size_t index = 0; index < data.size(); ++index) data[index] = values[index].load(std::memory_order_seq_cst);
        if (sequence.load(std::memory_order_seq_cst) != before) return false;
        value.styleIndex = static_cast<int>(data[0]);
        value.inputGainDb = data[1]; value.outputGainDb = data[2]; value.lookaheadMs = data[3];
        value.attackMs = data[4]; value.releaseMs = data[5]; value.transientLink = data[6]; value.releaseLink = data[7];
        value.oversampling = static_cast<int>(data[8]); value.truePeak = data[9] != 0;
        value.dcRemoval = data[10] != 0; value.externalSidechain = data[11] != 0; value.unity = data[12] != 0;
        value.audition = data[13] != 0; value.bypass = data[14] != 0; value.ditherBits = static_cast<int>(data[15]);
        value.noiseShape = static_cast<int>(data[16]); value.centerLink = data[17] != 0; value.lfeLink = data[18] != 0;
        return true;
    }
};

struct AtomicMeters
{
    std::array<std::atomic<float>, channelCapacity> input{}, output{}, peak{}, rms{};
    std::atomic<float> momentary{-160}, shortTerm{-160}, integrated{-160}, range{0}, reduction{0};
    std::atomic<int> channels{0};
    std::atomic<bool> clipping{false};

    void publish(const MeterSnapshot& snapshot) noexcept
    {
        for (int channel = 0; channel < channelCapacity; ++channel)
        {
            input[static_cast<std::size_t>(channel)].store(snapshot.inputPeak[static_cast<std::size_t>(channel)], std::memory_order_relaxed);
            output[static_cast<std::size_t>(channel)].store(snapshot.outputPeak[static_cast<std::size_t>(channel)], std::memory_order_relaxed);
            peak[static_cast<std::size_t>(channel)].store(snapshot.truePeak[static_cast<std::size_t>(channel)], std::memory_order_relaxed);
            rms[static_cast<std::size_t>(channel)].store(snapshot.rms[static_cast<std::size_t>(channel)], std::memory_order_relaxed);
        }
        momentary.store(snapshot.momentaryLUFS, std::memory_order_relaxed);
        shortTerm.store(snapshot.shortTermLUFS, std::memory_order_relaxed);
        integrated.store(snapshot.integratedLUFS, std::memory_order_relaxed);
        range.store(snapshot.loudnessRange, std::memory_order_relaxed);
        reduction.store(snapshot.gainReduction, std::memory_order_relaxed);
        channels.store(snapshot.channels, std::memory_order_relaxed);
        clipping.store(snapshot.clipping, std::memory_order_relaxed);
    }

    MeterSnapshot read() const noexcept
    {
        MeterSnapshot result;
        for (int channel = 0; channel < channelCapacity; ++channel)
        {
            result.inputPeak[static_cast<std::size_t>(channel)] = input[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
            result.outputPeak[static_cast<std::size_t>(channel)] = output[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
            result.truePeak[static_cast<std::size_t>(channel)] = peak[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
            result.rms[static_cast<std::size_t>(channel)] = rms[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
        }
        result.momentaryLUFS = momentary.load(std::memory_order_relaxed);
        result.shortTermLUFS = shortTerm.load(std::memory_order_relaxed);
        result.integratedLUFS = integrated.load(std::memory_order_relaxed);
        result.loudnessRange = range.load(std::memory_order_relaxed);
        result.gainReduction = reduction.load(std::memory_order_relaxed);
        result.channels = channels.load(std::memory_order_relaxed);
        result.clipping = clipping.load(std::memory_order_relaxed);
        return result;
    }
};
}

struct DspEngine::Implementation
{
    struct Instance
    {
        ProcessingChain chain;
        EngineSettings initial;
        std::uint64_t key = 0;
        std::vector<std::vector<float>> output;
        std::array<float*, channelCapacity> pointers{};
        std::array<DisplaySample, 2048> display{};
        Instance* retiredNext = nullptr;
    };

    static_assert(std::atomic<Instance*>::is_always_lock_free);
    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    SettingsMailbox mailbox;
    std::atomic<std::uint64_t> requested{0}, built{0};
    std::atomic<bool> resetRequested{false};
    std::atomic<Instance*> pending{nullptr}, retired{nullptr};
    std::atomic<bool> stopping{false}, pauseMeters{false}, measureTruePeak{true}, clearMeters{false};
    std::atomic<bool> loudnessRecording{true}, clearLoudness{false};
    std::atomic<bool> workerFailed{false};
    std::atomic<bool> backgroundActive{true};
    std::atomic<int> latency{0};
    std::atomic<std::uint64_t> latencyRequest{0}, latencyAcknowledged{0};
    std::uint32_t latencyGeneration = 0;
    std::uint64_t replacementLatencyToken = 0;
    std::vector<std::unique_ptr<Instance>> owned;
    std::unique_ptr<BackgroundOwner::Client> background;
    Instance* active = nullptr;
    Instance* replacement = nullptr;
    std::array<std::vector<EngineBank::ChannelGroup>, 4> groups;
    double rate = 48000.0;
    int channels = 0, capacity = 0, warmRemaining = 0, fadePosition = 0, fadeLength = 1;
    LoudnessMeter loudness;
    TruePeakDetector truePeak;
    std::vector<std::vector<float>> peakBuffer;
    std::array<float*, channelCapacity> peakPointers{};
    std::array<float, channelCapacity> inputHold{}, outputHold{}, peakHold{}, rmsEnergy{};
    MeterSnapshot meters;
    AtomicMeters published;
    bool peakWasEnabled = false;
    DisplayTelemetry displayHistory;

    Implementation() { published.publish(meters); }
    ~Implementation() { stop(); }

    void stop()
    {
        stopping.store(true, std::memory_order_release);
        background.reset();
        active = replacement = nullptr;
        pending.store(nullptr, std::memory_order_relaxed);
        retired.store(nullptr, std::memory_order_relaxed);
        latencyRequest.store(0, std::memory_order_release);
        latencyAcknowledged.store(0, std::memory_order_relaxed);
        replacementLatencyToken = 0;
        owned.clear();
        channels = 0;
    }

    std::unique_ptr<Instance> makeInstance(const EngineSettings& settings, std::uint64_t key)
    {
        auto instance = std::make_unique<Instance>();
        instance->initial = settings;
        instance->key = key;
        instance->chain.prepare(rate, channels, capacity, chainSettings(settings));
        instance->chain.setChannelGroups(groups[static_cast<std::size_t>(static_cast<int>(settings.centerLink) + 2 * static_cast<int>(settings.lfeLink))]);
        instance->output.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(capacity)));
        for (int channel = 0; channel < channels; ++channel) instance->pointers[static_cast<std::size_t>(channel)] = instance->output[static_cast<std::size_t>(channel)].data();
        return instance;
    }

    void removeOwned(Instance* instance)
    {
        const auto found = std::find_if(owned.begin(), owned.end(), [instance](const auto& candidate) { return candidate.get() == instance; });
        if (found != owned.end()) owned.erase(found);
    }

    void retire(Instance* instance) noexcept
    {
        if (instance == nullptr) return;
        auto* prior = retired.load(std::memory_order_relaxed);
        do { instance->retiredNext = prior; }
        while (!retired.compare_exchange_weak(prior, instance, std::memory_order_release, std::memory_order_relaxed));
        notifyBackground();
    }

    void notifyBackground() noexcept
    {
        if (background && backgroundActive.load(std::memory_order_acquire)) background->notify();
    }

    void serviceBackground() noexcept
    {
        if (stopping.load(std::memory_order_acquire) || !backgroundActive.load(std::memory_order_acquire)) return;
        auto* obsolete = retired.exchange(nullptr, std::memory_order_acquire);
        while (obsolete != nullptr)
        {
            auto* next = obsolete->retiredNext;
            removeOwned(obsolete);
            obsolete = next;
        }
        const auto wanted = requested.load(std::memory_order_acquire);
        EngineSettings settings;
        if (wanted != built.load(std::memory_order_acquire) && mailbox.read(settings)
            && structure(settings) == wanted)
        {
            try
            {
                auto instance = makeInstance(settings, wanted);
                if (requested.load(std::memory_order_acquire) == wanted)
                {
                    auto* pointer = instance.get();
                    owned.push_back(std::move(instance));
                    auto* obsoletePending = pending.exchange(pointer, std::memory_order_acq_rel);
                    if (obsoletePending != nullptr) removeOwned(obsoletePending);
                    workerFailed.store(false, std::memory_order_release);
                }
            }
            catch (...) { workerFailed.store(true, std::memory_order_release); }
            built.store(wanted, std::memory_order_release);
        }
        for (const auto& instance : owned) instance->chain.serviceKernelUpdates();
    }

    void prepare(double sampleRate, int maximumBlock, const ChannelLayout& layout, const EngineSettings& configuration)
    {
        stop();
        if (!std::isfinite(sampleRate) || sampleRate < 8000.0 || sampleRate > 192000.0
            || maximumBlock < 1 || layout.size() < 1 || layout.size() > channelCapacity)
            throw std::invalid_argument("Unsupported ADI-L audio format");
        rate = sampleRate;
        channels = layout.size();
        capacity = std::min(maximumBlock, 2048);
        for (int mode = 0; mode < 4; ++mode)
        {
            const auto classified = classifyChannelGroups(layout, (mode & 1) != 0, (mode & 2) != 0);
            if (!classified) throw std::invalid_argument("Unsupported ADI-L speaker layout");
            groups[static_cast<std::size_t>(mode)].clear();
            for (const auto& group : *classified) groups[static_cast<std::size_t>(mode)].push_back({group.pair, group.shared, group.stereoPair});
        }
        const auto settings = validated(configuration);
        const auto key = structure(settings);
        resetRequested.store(false, std::memory_order_relaxed);
        mailbox.write(settings);
        requested.store(key, std::memory_order_relaxed);
        built.store(key, std::memory_order_relaxed);
        auto initial = makeInstance(settings, key);
        active = initial.get();
        latency.store(active->chain.getLatencySamples(), std::memory_order_release);
        owned.push_back(std::move(initial));
        workerFailed.store(false, std::memory_order_release);
        fadeLength = std::max(1, static_cast<int>(rate * 0.02));
        warmRemaining = fadePosition = 0;
        loudness.prepare(rate, channels, layout.getChannelIndexForType(ChannelLayout::LFE));
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto type = layout.getTypeOfChannel(channel);
            if (type == ChannelLayout::leftSurround || type == ChannelLayout::rightSurround
                || type == ChannelLayout::leftSurroundSide || type == ChannelLayout::rightSurroundSide
                || type == ChannelLayout::leftSurroundRear || type == ChannelLayout::rightSurroundRear
                || type == ChannelLayout::centreSurround) loudness.setChannelWeight(channel, 1.41);
        }
        truePeak.prepare(rate, channels, capacity);
        displayHistory.prepare(rate);
        peakBuffer.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(capacity)));
        for (int channel = 0; channel < channels; ++channel) peakPointers[static_cast<std::size_t>(channel)] = peakBuffer[static_cast<std::size_t>(channel)].data();
        resetMeterState();
        backgroundActive.store(true, std::memory_order_release);
        stopping.store(false, std::memory_order_release);
        background = std::make_unique<BackgroundOwner::Client>(this, [](void* context) noexcept {
            static_cast<Implementation*>(context)->serviceBackground();
        });
    }

    void resetMeterState() noexcept
    {
        loudness.reset();
        truePeak.reset();
        inputHold.fill(0); outputHold.fill(0); peakHold.fill(0); rmsEnergy.fill(0);
        meters = MeterSnapshot{};
        meters.channels = channels;
        peakWasEnabled = false;
        published.publish(meters);
    }

    void publishConfiguration(const EngineSettings& settings) noexcept
    {
        mailbox.write(settings);
        const auto key = structure(settings);
        requested.store(key, std::memory_order_release);
        if (built.load(std::memory_order_acquire) != key) notifyBackground();
    }
    void requestConfiguration(const EngineSettings& configuration) noexcept
    {
        publishConfiguration(validated(configuration));
    }

    void updateInstance(Instance& instance, EngineSettings settings)
    {
        settings.styleIndex = instance.initial.styleIndex;
        settings.oversampling = instance.initial.oversampling;
        settings.truePeak = instance.initial.truePeak;
        instance.chain.update(chainSettings(settings));
        instance.chain.setChannelGroups(groups[static_cast<std::size_t>(static_cast<int>(settings.centerLink) + 2 * static_cast<int>(settings.lfeLink))]);
    }

    void process(float* const* main, const float* const* sidechain, int frames, const EngineSettings& configuration,
                 BlockContext blockContext) noexcept
    {
        if (main == nullptr || active == nullptr || frames <= 0) return;
        for (int channel = 0; channel < channels; ++channel) if (main[channel] == nullptr) return;
        const auto settings = validated(configuration);
        const auto key = structure(settings);
        publishConfiguration(settings);
        if (resetRequested.exchange(false, std::memory_order_acq_rel))
        {
            active->chain.reset();
            displayHistory.reset();
            if (replacement != nullptr)
            {
                replacement->chain.reset();
                warmRemaining = replacement->chain.getLatencySamples();
                fadePosition = 0;
            }
        }
        if (clearMeters.exchange(false, std::memory_order_acq_rel)) resetMeterState();
        if (clearLoudness.exchange(false, std::memory_order_acq_rel))
        {
            loudness.reset();
            meters.momentaryLUFS = meters.shortTermLUFS = meters.integratedLUFS = -160.0f;
            meters.loudnessRange = 0;
            published.publish(meters);
        }
        std::array<const float*, channelCapacity> input{}, detector{}, meteredOutput{};
        std::array<float*, channelCapacity> output{};
        try
        {
            for (int offset = 0; offset < frames; offset += capacity)
            {
                const auto count = std::min(capacity, frames - offset);
                if (replacement == nullptr)
                {
                    auto* candidate = pending.exchange(nullptr, std::memory_order_acquire);
                    if (candidate != nullptr)
                    {
                        if (candidate->key != key) retire(candidate);
                        else
                        {
                            replacement = candidate;
                            warmRemaining = replacement->chain.getLatencySamples();
                            fadePosition = 0;
                            replacementLatencyToken = 0;
                        }
                    }
                }
                for (int channel = 0; channel < channels; ++channel)
                {
                    input[static_cast<std::size_t>(channel)] = main[static_cast<std::size_t>(channel)] + offset;
                    output[static_cast<std::size_t>(channel)] = main[static_cast<std::size_t>(channel)] + offset;
                    meteredOutput[static_cast<std::size_t>(channel)] = output[static_cast<std::size_t>(channel)];
                    detector[static_cast<std::size_t>(channel)] = sidechain && sidechain[static_cast<std::size_t>(channel)] ? sidechain[static_cast<std::size_t>(channel)] + offset : nullptr;
                }
                const auto paused = pauseMeters.load(std::memory_order_relaxed);
                const auto captureDisplay = displayHistory.beginBlock(paused);
                const auto decay = static_cast<float>(std::exp(-count / (0.3 * rate)));
                if (!paused)
                    for (int channel = 0; channel < channels; ++channel)
                    {
                        float peak = 0;
                        for (int frame = 0; frame < count; ++frame)
                            peak = std::max(peak, std::abs(finiteSample(input[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)])));
                        inputHold[static_cast<std::size_t>(channel)] = std::max(peak, inputHold[static_cast<std::size_t>(channel)] * decay);
                        meters.inputPeak[static_cast<std::size_t>(channel)] = decibels(inputHold[static_cast<std::size_t>(channel)]);
                    }
                updateInstance(*active, settings);
                active->chain.setDisplayBuffer(captureDisplay ? active->display.data() : nullptr);
                if (!active->chain.process(input.data(), active->pointers.data(), count, sidechain ? detector.data() : nullptr))
                    for (int channel = 0; channel < channels; ++channel) std::fill_n(active->pointers[static_cast<std::size_t>(channel)], count, 0.0f);
                if (active->chain.needsKernelService()) notifyBackground();
                auto gainReduction = active->chain.getGainReductionDb();
                if (replacement != nullptr)
                {
                    updateInstance(*replacement, settings);
                    replacement->chain.setDisplayBuffer(captureDisplay ? replacement->display.data() : nullptr);
                    if (!replacement->chain.process(input.data(), replacement->pointers.data(), count, sidechain ? detector.data() : nullptr))
                        for (int channel = 0; channel < channels; ++channel) std::fill_n(replacement->pointers[static_cast<std::size_t>(channel)], count, 0.0f);
                    if (replacement->chain.needsKernelService()) notifyBackground();
                    gainReduction = std::max(gainReduction, replacement->chain.getGainReductionDb());
                }
                const auto changesLatency = replacement != nullptr
                    && replacement->chain.getLatencySamples() != active->chain.getLatencySamples();
                if (changesLatency && warmRemaining <= count && replacementLatencyToken == 0)
                {
                    if (++latencyGeneration == 0) ++latencyGeneration;
                    replacementLatencyToken = (static_cast<std::uint64_t>(latencyGeneration) << 32)
                        | static_cast<std::uint32_t>(replacement->chain.getLatencySamples());
                    latencyRequest.store(replacementLatencyToken, std::memory_order_release);
                }
                const auto canFade = !changesLatency || (replacementLatencyToken != 0
                    && blockContext.latencyAcknowledged == replacementLatencyToken);
                for (int frame = 0; frame < count; ++frame)
                {
                    float weight = 0;
                    if (replacement != nullptr)
                    {
                        if (warmRemaining > 0) --warmRemaining;
                        else if (canFade) weight = static_cast<float>(std::min(++fadePosition, fadeLength)) / static_cast<float>(fadeLength);
                    }
                    float displayOutput = 0;
                    double displayEnergy = 0;
                    for (int channel = 0; channel < channels; ++channel)
                    {
                        const auto original = active->pointers[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)];
                        output[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] = replacement ? original + weight * (replacement->pointers[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)] - original) : original;
                        if (captureDisplay)
                        {
                            const auto value = finiteSample(output[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
                            displayOutput = std::max(displayOutput, std::abs(value));
                            displayEnergy += static_cast<double>(value) * value;
                        }
                    }
                    if (captureDisplay)
                    {
                        const auto first = active->display[static_cast<std::size_t>(frame)];
                        const auto second = replacement ? replacement->display[static_cast<std::size_t>(frame)] : first;
                        const auto inputPeak = first.inputPeak + weight * (second.inputPeak - first.inputPeak);
                        const auto reduction = weight > 0 ? std::max(first.reduction, second.reduction) : first.reduction;
                        displayHistory.add(inputPeak, displayOutput, reduction, displayEnergy / channels,
                            meters.momentaryLUFS, meters.shortTermLUFS, meters.integratedLUFS);
                    }
                }
                if (replacement != nullptr && fadePosition >= fadeLength)
                {
                    retire(active);
                    active = replacement;
                    replacement = nullptr;
                    latency.store(active->chain.getLatencySamples(), std::memory_order_release);
                    if (replacementLatencyToken != 0)
                    {
                        auto expected = replacementLatencyToken;
                        latencyRequest.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
                    }
                    replacementLatencyToken = 0;
                }
                if (!paused) meters.gainReduction = gainReduction;
                meterOutput(meteredOutput.data(), count, decay, paused);
            }
        }
        catch (...)
        {
            for (int channel = 0; channel < channels; ++channel) std::fill_n(main[channel], frames, 0.0f);
        }
    }

    void meterOutput(const float* const* output, int count, float decay, bool paused) noexcept
    {
        const auto recording = loudnessRecording.load(std::memory_order_relaxed);
        loudness.process(output, count, !paused && recording);
        const auto peakEnabled = measureTruePeak.load(std::memory_order_relaxed);
        if (peakEnabled && !peakWasEnabled) truePeak.reset();
        if (peakEnabled) truePeak.process(output, peakPointers.data(), count);
        peakWasEnabled = peakEnabled;
        if (paused) return;
        for (int channel = 0; channel < channels; ++channel)
        {
            float peak = 0, interpolated = 0;
            double sum = 0;
            for (int frame = 0; frame < count; ++frame)
            {
                const auto sample = finiteSample(output[channel][frame]);
                peak = std::max(peak, std::abs(sample));
                sum += static_cast<double>(sample) * sample;
                if (peakEnabled) interpolated = std::max(interpolated, peakPointers[static_cast<std::size_t>(channel)][static_cast<std::size_t>(frame)]);
            }
            interpolated = std::max(interpolated, peak);
            outputHold[static_cast<std::size_t>(channel)] = std::max(peak, outputHold[static_cast<std::size_t>(channel)] * decay);
            peakHold[static_cast<std::size_t>(channel)] = std::max(interpolated, peakHold[static_cast<std::size_t>(channel)] * decay);
            rmsEnergy[static_cast<std::size_t>(channel)] = rmsEnergy[static_cast<std::size_t>(channel)] * decay + static_cast<float>(sum / count) * (1.0f - decay);
            meters.outputPeak[static_cast<std::size_t>(channel)] = decibels(outputHold[static_cast<std::size_t>(channel)]);
            meters.truePeak[static_cast<std::size_t>(channel)] = decibels(peakEnabled ? peakHold[static_cast<std::size_t>(channel)] : outputHold[static_cast<std::size_t>(channel)]);
            meters.rms[static_cast<std::size_t>(channel)] = decibels(std::sqrt(std::max(0.0f, rmsEnergy[static_cast<std::size_t>(channel)])));
            meters.clipping = meters.clipping || interpolated > 1.0f;
        }
        if (recording)
        {
            const auto result = loudness.snapshot();
            meters.momentaryLUFS = std::isfinite(result.momentary) ? result.momentary : -160.0f;
            meters.shortTermLUFS = std::isfinite(result.shortTerm) ? result.shortTerm : -160.0f;
            meters.integratedLUFS = std::isfinite(result.integrated) ? result.integrated : -160.0f;
            meters.loudnessRange = result.range;
        }
        published.publish(meters);
    }
};

DspEngine::DspEngine() : implementation(std::make_unique<Implementation>()) {}
DspEngine::~DspEngine() = default;

void DspEngine::setBackgroundActive(bool active) noexcept
{
    const auto previous = implementation->backgroundActive.exchange(active, std::memory_order_acq_rel);
    if (active && !previous) implementation->notifyBackground();
}

void DspEngine::prepare(double sampleRate, int maximumBlock, const ChannelLayout& channels, const EngineSettings& settings)
{
    implementation->prepare(sampleRate, maximumBlock, channels, settings);
}

void DspEngine::reset() noexcept { implementation->resetRequested.store(true, std::memory_order_release); }
void DspEngine::requestConfiguration(const EngineSettings& settings) noexcept
{
    implementation->requestConfiguration(settings);
}
DspEngine::BlockContext DspEngine::beginBlock() const noexcept
{
    return {implementation->latencyAcknowledged.load(std::memory_order_acquire)};
}
void DspEngine::process(float* const* main, const float* const* sidechain, int frames, const EngineSettings& settings) noexcept
{
    process(main, sidechain, frames, settings, beginBlock());
}
void DspEngine::process(float* const* main, const float* const* sidechain, int frames, const EngineSettings& settings,
                        BlockContext blockContext) noexcept
{
    implementation->process(main, sidechain, frames, settings, blockContext);
}
int DspEngine::latencySamples() const noexcept { return implementation->latency.load(std::memory_order_acquire); }
DspEngine::LatencyChange DspEngine::pendingLatencyChange() const noexcept
{
    const auto token = implementation->latencyRequest.load(std::memory_order_acquire);
    if (token == 0 || implementation->latencyAcknowledged.load(std::memory_order_acquire) == token) return {};
    return {token, static_cast<int>(static_cast<std::uint32_t>(token))};
}
void DspEngine::acknowledgeLatencyChange(std::uint64_t token) noexcept
{
    if (token != 0 && implementation->latencyRequest.load(std::memory_order_acquire) == token)
        implementation->latencyAcknowledged.store(token, std::memory_order_release);
}
void DspEngine::setMeteringOptions(bool truePeak, bool paused) noexcept
{
    implementation->measureTruePeak.store(truePeak, std::memory_order_relaxed);
    implementation->pauseMeters.store(paused, std::memory_order_relaxed);
}
void DspEngine::resetMeters() noexcept { implementation->clearMeters.store(true, std::memory_order_release); }
void DspEngine::setLoudnessRecording(bool recording) noexcept { implementation->loudnessRecording.store(recording, std::memory_order_relaxed); }
void DspEngine::resetLoudness() noexcept { implementation->clearLoudness.store(true, std::memory_order_release); }
MeterSnapshot DspEngine::getMeterSnapshot() const noexcept
{
    auto result = implementation->published.read();
    result.processingError = implementation->workerFailed.load(std::memory_order_acquire);
    return result;
}
void DspEngine::enableDisplayHistory(bool enabled) noexcept { implementation->displayHistory.enable(enabled); }
bool DspEngine::popDisplayFrame(DisplayFrame& frame) noexcept { return implementation->displayHistory.pop(frame); }
}
