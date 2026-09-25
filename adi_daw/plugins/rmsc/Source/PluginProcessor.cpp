// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>

namespace {
const juce::String kThreshold = "threshold";
const juce::String kRelease = "release";
const juce::String kDepth = "depth";
const juce::String kMergeAux = "mergeAux";
}  // namespace

RmscProcessor::RmscProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "ADI_RMSC", createLayout()) {
    threshold_ = state.getRawParameterValue(kThreshold);
    release_ = state.getRawParameterValue(kRelease);
    depth_ = state.getRawParameterValue(kDepth);
    mergeAux_ = state.getRawParameterValue(kMergeAux);
}

juce::AudioProcessorValueTreeState::ParameterLayout RmscProcessor::createLayout() {
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // Where the duck is complete: a kick peaking at the threshold mutes the bass.
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID{kThreshold, 1}, "Threshold", NormalisableRange<float>(-48.0f, 0.0f, 0.1f), -12.0f,
        AudioParameterFloatAttributes().withLabel("dB")));
    // The release: instant down on the kick, back up with this time constant.
    // 0 is the classic, raw RMSC with its sidebands.
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID{kRelease, 1}, "Release", NormalisableRange<float>(0.0f, 500.0f, 0.1f, 0.4f), 40.0f,
        AudioParameterFloatAttributes().withLabel("ms")));
    layout.add(std::make_unique<AudioParameterFloat>(
        ParameterID{kDepth, 1}, "Depth", NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withLabel("%")));
    // Monitoring only: the raw sidechain summed into the output, off by default.
    layout.add(std::make_unique<AudioParameterBool>(ParameterID{kMergeAux, 1}, "Merge AUX", false));
    return layout;
}

bool RmscProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    // Main: stereo in, stereo out. Sidechain: stereo, mono, or not connected
    // (no key, no duck).
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    const auto sc = layouts.getChannelSet(true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void RmscProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    rmsc_.prepare(sampleRate_);
    const auto n = static_cast<std::size_t>(std::max(1, maximumExpectedSamplesPerBlock));
    key_.assign(n, 0.0f);
    gain_.assign(n, 1.0f);
    chunkMin_ = 1.0f;
    chunkCount_ = 0;
    scopeFifo_.reset();
}

void RmscProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (n <= 0 || key_.empty()) return;
    // A host that sends more than it prepared for is processed in pieces of
    // what was prepared, rather than allocated for here (the loop below).

    auto main = getBusBuffer(buffer, true, 0);
    const bool haveKey = getBus(true, 1) != nullptr && getBus(true, 1)->isEnabled();
    auto key = haveKey ? getBusBuffer(buffer, true, 1) : juce::AudioBuffer<float>();

    rmsc_.setThresholdDb(static_cast<double>(threshold_->load()));
    rmsc_.setReleaseMs(static_cast<double>(release_->load()));
    rmsc_.setDepth(static_cast<double>(depth_->load()) / 100.0);
    const bool merge = mergeAux_->load() >= 0.5f;

    const int chunk = static_cast<int>(key_.size());
    for (int start = 0; start < n; start += chunk) {
        const int len = std::min(chunk, n - start);

        // MONO SUM of the key: (L + R) / 2, so a kick panned or recorded in
        // stereo ducks the same as a mono one.
        const int kc = key.getNumChannels();
        for (int i = 0; i < len; ++i) {
            float s = 0.0f;
            for (int c = 0; c < kc; ++c) s += key.getSample(c, start + i);
            key_[static_cast<std::size_t>(i)] = kc > 0 ? s / static_cast<float>(kc) : 0.0f;
        }

        float* io[2] = {main.getWritePointer(0, start), main.getWritePointer(1, start)};
        rmsc_.process(io, io, 2, key_.data(), len, gain_.data());

        // MERGE AUX: the raw key, stereo as it came, summed into the output for
        // monitoring. It does not go through the duck.
        if (merge && kc > 0)
            for (int c = 0; c < 2; ++c)
                main.addFrom(c, start, key, std::min(c, kc - 1), start, len);

        // The scope's feed: the lowest gain of each 32 samples.
        for (int i = 0; i < len; ++i) {
            chunkMin_ = std::min(chunkMin_, gain_[static_cast<std::size_t>(i)]);
            if (++chunkCount_ == kScopeDecimation) {
                const auto w = scopeFifo_.write(1);
                if (w.blockSize1 > 0) scopeData_[static_cast<std::size_t>(w.startIndex1)] = chunkMin_;
                chunkMin_ = 1.0f;
                chunkCount_ = 0;
            }
        }
    }
}

int RmscProcessor::readScope(float* dest, int maxPoints) noexcept {
    const auto r = scopeFifo_.read(std::min(maxPoints, scopeFifo_.getNumReady()));
    for (int i = 0; i < r.blockSize1; ++i) dest[i] = scopeData_[static_cast<std::size_t>(r.startIndex1 + i)];
    for (int i = 0; i < r.blockSize2; ++i)
        dest[r.blockSize1 + i] = scopeData_[static_cast<std::size_t>(r.startIndex2 + i)];
    return r.blockSize1 + r.blockSize2;
}

void RmscProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (const auto xml = state.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void RmscProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (const auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(state.state.getType())) state.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* RmscProcessor::createEditor() { return new RmscEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RmscProcessor(); }
