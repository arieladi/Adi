// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "adi/ui/shared_analysis.hpp"
#include <juce_gui_basics/juce_gui_basics.h>

namespace adi::ui {
struct ClassicScopeControls {
    dsp::TriggeredScope::Trigger trigger = dsp::TriggeredScope::Trigger::Free;
    float level = .5f, amplitude = .5f, time = .75f, holdoff = .5f, speed = .5f;
    bool right = false, removeDc = false, sync = false;
    bool operator==(const ClassicScopeControls &) const = default;
    dsp::TriggeredScope::Settings settings() const noexcept;
};

// The Scope page fits the fixed original panel to the available area.
class ClassicScopePanel final : public juce::Component {
  public:
    ClassicScopePanel();
    ~ClassicScopePanel() override;
    void present(const ClassicScopeControls &, bool frozen,
                 const std::shared_ptr<const SharedAnalysis::CaptureResult> &, std::int64_t minimumSequence);
    void resized() override;
    std::function<void(const ClassicScopeControls &)> onChange;
    std::function<void(bool)> onFreeze;
  private:
    struct Face;
    std::unique_ptr<Face> face_;
};
} // namespace adi::ui
