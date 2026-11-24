// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "juce/adi_shaper_panel_session.hpp"
#include "juce/adi_shaper_spectrum.hpp"
#include <vector>
namespace adi::ui {
// Owned by the ordinary plug-in editor window. Its frame callback runs on the
// same clock as other editors. It follows its device across an audio restart
// and closes when the device is deleted or replaced.
class AdiShaperPanelView final : public juce::Component {
  public:
    AdiShaperPanelView(ProjectDocument &, std::int64_t device);
    ~AdiShaperPanelView() override;
    void resized() override { panel_.setBounds(getLocalBounds()); }
    void frame();
    void visibilityChanged() override;
    bool valid();
    // Why the last edit was refused, for the Status Bar; empty when it was not.
    const std::string &error() const noexcept { return model_.error(); }
    // What the panel shows for a one-based parameter.
    double value(int id) const { return model_.value(id); }

  private:
    void choosePreset();
    void suspend();
    device::DeviceInstance *liveDevice();
    ProjectDocument &doc_;
    std::int64_t id_;
    engine::DeviceRuntimeToken token_;
    AdiShaperPanelSession model_;
    AdiShaperPanel panel_;
    AdiShaperSpectrum spectrum_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::uint64_t revision_ = 0, committed_ = 0, stamp_ = 0;
    std::vector<double> playing_;
    juce::Label status_;
};
} // namespace adi::ui
