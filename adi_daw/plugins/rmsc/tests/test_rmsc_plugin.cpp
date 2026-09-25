// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI RMSC, tested as the plug-in a host sees (ADR-0166): its buses, its
// processBlock with a main and a sidechain input, its parameters and its state.
// The DSP inside is adi::dsp::RingModSidechain, tested on its own in
// tests/test_dsp.cpp; this proves the wrapping.

#include "../Source/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

constexpr int kBlock = 512;
constexpr double kRate = 48000.0;

/// Main L/R in channels 0-1, the sidechain in 2-3, as JUCE lays out the buses.
juce::AudioBuffer<float> block(float main, float keyL, float keyR, int keyFrom, int keyTo) {
    juce::AudioBuffer<float> b(4, kBlock);
    for (int i = 0; i < kBlock; ++i) {
        const bool k = i >= keyFrom && i < keyTo;
        b.setSample(0, i, main);
        b.setSample(1, i, main);
        b.setSample(2, i, k ? keyL : 0.0f);
        b.setSample(3, i, k ? keyR : 0.0f);
    }
    return b;
}

void set(RmscProcessor& p, const char* id, float value) {
    auto* param = p.state.getParameter(id);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    std::printf("adi_rmsc_tests -- the ADI RMSC plug-in (ADR-0166)\n\n");

    {
        RmscProcessor p;
        check(p.getBusCount(true) == 2 && p.getBus(true, 1)->getName() == "Sidechain",
              "two inputs: the main signal and a sidechain");
        auto layout = p.getBusesLayout();
        layout.getChannelSet(true, 1) = juce::AudioChannelSet::mono();
        check(p.checkBusesLayoutSupported(layout), "a mono sidechain is accepted");
        layout.getChannelSet(true, 1) = juce::AudioChannelSet::disabled();
        check(p.checkBusesLayoutSupported(layout), "and so is none: no key, no duck");
        layout.getChannelSet(true, 0) = juce::AudioChannelSet::mono();
        check(!p.checkBusesLayoutSupported(layout), "the main input is stereo only");
    }
    {
        RmscProcessor p;
        p.prepareToPlay(kRate, kBlock);
        set(p, "threshold", -12.0f);
        set(p, "release", 20.0f);
        set(p, "depth", 100.0f);
        const float kick = static_cast<float>(std::pow(10.0, -12.0 / 20.0));
        auto b = block(0.5f, kick, kick, 100, 200);
        juce::MidiBuffer midi;
        p.processBlock(b, midi);
        check(b.getSample(0, 99) == 0.5f && b.getSample(1, 99) == 0.5f, "before the kick, the bass is untouched");
        check(std::fabs(b.getSample(0, 100)) < 1e-5f && std::fabs(b.getSample(1, 150)) < 1e-5f,
              "a kick at the threshold mutes the bass from its first sample, both channels");
        check(b.getSample(0, 201) < 0.1f && b.getSample(0, 511) > b.getSample(0, 201),
              "after the kick, the release brings it back, not a jump");
    }
    {
        // THE MONO SUM, and its trap: a key whose channels are in opposite
        // polarity sums to nothing, and ducks nothing.
        RmscProcessor p;
        p.prepareToPlay(kRate, kBlock);
        set(p, "threshold", -12.0f);
        auto b = block(0.5f, 0.3f, -0.3f, 100, 200);
        juce::MidiBuffer midi;
        p.processBlock(b, midi);
        check(b.getSample(0, 150) == 0.5f,
              "the key is summed to mono first: L and R in opposite polarity cancel, and nothing ducks");
        auto one = block(0.5f, 0.3f, 0.0f, 100, 200);
        p.prepareToPlay(kRate, kBlock);
        p.processBlock(one, midi);
        const float half = 0.5f * (1.0f - static_cast<float>(std::min(1.0, 0.15 / std::pow(10.0, -12.0 / 20.0))));
        check(std::fabs(one.getSample(0, 150) - half) < 1e-4f, "a key on one channel counts half");
    }
    {
        RmscProcessor p;
        p.prepareToPlay(kRate, kBlock);
        set(p, "depth", 0.0f);
        set(p, "mergeAux", 1.0f);
        auto b = block(0.25f, 0.5f, 0.125f, 0, kBlock);
        juce::MidiBuffer midi;
        p.processBlock(b, midi);
        check(b.getSample(0, 10) == 0.75f && b.getSample(1, 10) == 0.375f,
              "Merge AUX adds the raw key to the output, channel for channel");
        set(p, "mergeAux", 0.0f);
        auto c = block(0.25f, 0.5f, 0.125f, 0, kBlock);
        p.processBlock(c, midi);
        check(c.getSample(0, 10) == 0.25f, "and off, it adds nothing");
    }
    {
        RmscProcessor a;
        set(a, "threshold", -30.0f);
        set(a, "release", 120.0f);
        set(a, "mergeAux", 1.0f);
        juce::MemoryBlock saved;
        a.getStateInformation(saved);
        RmscProcessor b;
        b.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));
        auto get = [](RmscProcessor& p, const char* id) {
            return p.state.getRawParameterValue(id)->load();
        };
        check(std::fabs(get(b, "threshold") + 30.0f) < 0.05f && std::fabs(get(b, "release") - 120.0f) < 0.05f &&
                  get(b, "mergeAux") >= 0.5f,
              "the state round-trips: threshold, release, Merge AUX");
    }
    {
        // The scope's feed: the lowest gain of every 32 samples.
        RmscProcessor p;
        p.prepareToPlay(kRate, kBlock);
        set(p, "threshold", -12.0f);
        auto b = block(0.5f, 1.0f, 1.0f, 64, 96);
        juce::MidiBuffer midi;
        p.processBlock(b, midi);
        float points[64] = {};
        const int n = p.readScope(points, 64);
        check(n == kBlock / RmscProcessor::kScopeDecimation, "one scope point per 32 samples: " + std::to_string(n));
        check(n >= 3 && points[0] == 1.0f && points[2] < 1e-5f, "and each is the lowest gain of its 32");
    }

    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
