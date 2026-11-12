// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "DisplayTelemetry.h"
#include "ChannelLayout.h"
#include <array>
#include <cstdint>
#include <memory>

namespace adi::mix::limiter
{
struct EngineSettings
{
    int styleIndex = 5;
    float inputGainDb = 0.0f;
    float outputGainDb = 0.0f;
    float lookaheadMs = 0.2f;
    float attackMs = 250.0f;
    float releaseMs = 500.0f;
    float transientLink = 0.5f;
    float releaseLink = 0.5f;
    int oversampling = 1;
    bool truePeak = false;
    bool dcRemoval = false;
    bool externalSidechain = false;
    bool unity = false;
    bool audition = false;
    bool bypass = false;
    int ditherBits = 0;
    int noiseShape = 0;
    bool centerLink = false;
    bool lfeLink = false;
};

struct MeterSnapshot
{
    static constexpr int maximumChannels = 64;
    std::array<float, maximumChannels> inputPeak, outputPeak, truePeak, rms;
    float momentaryLUFS = -160.0f;
    float shortTermLUFS = -160.0f;
    float integratedLUFS = -160.0f;
    float loudnessRange = 0.0f;
    float gainReduction = 0.0f;
    int channels = 0;
    bool clipping = false;
    bool processingError = false;

    MeterSnapshot() noexcept
    {
        inputPeak.fill(-160.0f);
        outputPeak.fill(-160.0f);
        truePeak.fill(-160.0f);
        rms.fill(-160.0f);
    }
};

class DspEngine
{
public:
    struct LatencyChange
    {
        std::uint64_t token = 0;
        int samples = 0;
    };

    // Share one snapshot across every segment of a host audio block.
    struct BlockContext
    {
        std::uint64_t latencyAcknowledged = 0;
    };

    DspEngine();
    ~DspEngine();
    DspEngine(const DspEngine&) = delete;
    DspEngine& operator=(const DspEngine&) = delete;
    void prepare(double sampleRate, int maximumBlock, const ChannelLayout& channels, const EngineSettings& settings);
    void reset() noexcept;
    void setBackgroundActive(bool active) noexcept;
    // Single audio-side publisher, serialized with process. Prepares requested
    // structures on the existing worker while a containing stage is disabled;
    // does not process, warm, reset, fade or advance any audio/meter state.
    void requestConfiguration(const EngineSettings& settings) noexcept;
    BlockContext beginBlock() const noexcept;
    void process(float* const* main, const float* const* sidechain, int frames, const EngineSettings& settings) noexcept;
    void process(float* const* main, const float* const* sidechain, int frames, const EngineSettings& settings,
                 BlockContext blockContext) noexcept;
    int latencySamples() const noexcept;
    LatencyChange pendingLatencyChange() const noexcept;
    void acknowledgeLatencyChange(std::uint64_t token) noexcept;
    void setMeteringOptions(bool truePeak, bool paused) noexcept;
    void setLoudnessRecording(bool recording) noexcept;
    void resetLoudness() noexcept;
    void resetMeters() noexcept;
    MeterSnapshot getMeterSnapshot() const noexcept;
    void enableDisplayHistory(bool enabled) noexcept;
    bool popDisplayFrame(DisplayFrame& frame) noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation;
};
}
