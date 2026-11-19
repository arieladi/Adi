// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/main_audition.hpp"
#include "ui_analysis.hpp"

namespace adi::ui {
// Passive post-fader Main measurement, clocked by its visible owner.
class MixOutputMeter final : public juce::Component, private juce::ComponentListener {
  public:
    using Audition = engine::MainAudition;
    MixOutputMeter();
    ~MixOutputMeter() override;
    void frame(const std::shared_ptr<SharedAnalysis> &, std::int64_t mainTrack,
               const std::shared_ptr<const MainMeterSource> &loudness = {});
    void suspend(bool value);
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    void setAuditionSink(std::function<void(Audition)> sink);
    juce::ComboBox listenBox;
    void resized() override;
    void paint(juce::Graphics &) override;
    bool subscribed() const noexcept { return static_cast<bool>(lease_); }
    std::shared_ptr<const SharedAnalysis::StereoMeterResult> reading() const noexcept { return shown_; }
    std::optional<MainMeterSnapshot> loudness() const noexcept { return main_; }
    juce::ComboBox scaleBox, weightingBox, channelBox;
    juce::ToggleButton floatButton{"Float"};
    juce::TextButton resetButton{"Reset"};

  private:
    void chooseAudition(Audition mode);
    void stopAudition();
    void resetProfile();
    void release();
    bool eligible() const noexcept;
    void componentVisibilityChanged(juce::Component &) override;
    void componentBeingDeleted(juce::Component &) override;
    void stopWatching();
    void paintReadings(juce::Graphics &, juce::Rectangle<int>);
    double offsetDb() const noexcept;
    SharedAnalysis::StereoMeterProfile profile_;
    std::weak_ptr<SharedAnalysis> broker_;
    SharedAnalysis::Lease lease_;
    std::shared_ptr<const SharedAnalysis::StereoMeterResult> shown_;
    std::weak_ptr<const MainMeterSource> lastMainSource_;
    std::optional<MainMeterSnapshot> main_;
    std::vector<juce::Component::SafePointer<juce::Component>> ancestors_;
    std::function<void(Audition)> audition_;
    Audition listening_ = Audition::Stereo;
    std::int64_t track_ = 0;
    bool suspended_ = false, wasAvailable_ = false;
    juce::Rectangle<int> plot_;
};
} // namespace adi::ui
