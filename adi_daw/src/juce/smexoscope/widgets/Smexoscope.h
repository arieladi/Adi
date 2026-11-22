// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Defines.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

// Message-thread projection for the upstream waveform renderer. Neither array is
// shared with the worker. Validity distinguishes a missing segment from silence.
struct Smexoscope {
    enum { kTriggerType, kTriggerLevel, kSyncDraw, kTimeWindow, kAmpWindow, kNumParams };
    enum { kTriggerFree, kTriggerRising, kTriggerFalling, kTriggerInternal, kNumTriggerTypes };
    using Trace = std::array<juce::Point<int>, OSC_WIDTH * 2>;
    const Trace& getPeaks() const noexcept { return current; }
    const Trace& getCopy() const noexcept { return previous; }
    float getParameter(int i) const noexcept { return parameters[static_cast<size_t>(i)]; }
    double getSampleRate() const noexcept { return sampleRate; }
    bool visible(size_t column) const noexcept {
        return column < OSC_WIDTH && (parameters[kSyncDraw] > .5f ? previousValid : currentValid)[column];
    }
    Trace current{}, previous{};
    std::array<bool, OSC_WIDTH> currentValid{}, previousValid{};
    std::array<float, kNumParams> parameters{};
    double sampleRate = 0;
};
