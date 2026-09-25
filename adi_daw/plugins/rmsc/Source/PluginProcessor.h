// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI RMSC: ring-modulation sidechain ducking as a plug-in (ADR-0166).
//
// Signal flow, one sample at a time:
//
//   sidechain L+R -> mono (x 0.5) -> |x| -> threshold scale, clamped to 1
//       -> release (instant attack, exponential fall) -> gain = 1 - depth * env
//   main L/R x gain -> out;  [Merge AUX] out += raw sidechain L/R
//
// The DSP is adi::dsp::RingModSidechain (src/adi/dsp/rmsc.*), the same class
// the engine's reference and the Pd module are checked against, so the plug-in
// and the DAW cannot drift apart. The gain it applies is handed to the editor
// through a lock-free FIFO for the envelope scope.

#pragma once

#include "adi/dsp/rmsc.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <vector>

class RmscProcessor final : public juce::AudioProcessor {
public:
    RmscProcessor();
    ~RmscProcessor() override = default;

    // --- the plug-in contract ------------------------------------------------
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "ADI RMSC"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // --- for the editor ------------------------------------------------------
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState state;

    /// The envelope scope's feed: the lowest gain of every kScopeDecimation
    /// samples, so a one-sample duck still shows. Audio thread writes, the
    /// editor's timer reads; lock-free, single producer, single consumer.
    static constexpr int kScopeDecimation = 32;
    int readScope(float* dest, int maxPoints) noexcept;
    [[nodiscard]] double scopeRate() const noexcept { return sampleRate_ / kScopeDecimation; }

private:
    adi::dsp::RingModSidechain rmsc_;
    std::vector<float> key_, gain_;   // sized in prepareToPlay, never on the audio thread
    double sampleRate_ = 48000.0;

    std::atomic<float>* threshold_ = nullptr;
    std::atomic<float>* release_ = nullptr;
    std::atomic<float>* depth_ = nullptr;
    std::atomic<float>* mergeAux_ = nullptr;

    juce::AbstractFifo scopeFifo_{8192};
    std::array<float, 8192> scopeData_{};
    float chunkMin_ = 1.0f;
    int chunkCount_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RmscProcessor)
};
