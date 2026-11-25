// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "juce/adi_shaper_panel_session.hpp"
#include "juce/adi_shaper_spectrum.hpp"
namespace adi::ui {
// Owned by the ordinary plug-in editor window. Its frame callback runs on the
// same clock as other editors and closes when this device instance disappears.
class AdiShaperPanelView final : public juce::Component {
  public:
    AdiShaperPanelView(ProjectDocument &, std::int64_t device);
    ~AdiShaperPanelView() override;
    void resized() override { panel_.setBounds(getLocalBounds()); }
    void frame();
    void visibilityChanged() override;
    bool valid() const;

  private:
    void choosePreset();
    void suspend();
    ProjectDocument &doc_;
    engine::DeviceRuntimeToken token_;
    AdiShaperPanelSession model_;
    AdiShaperPanel panel_;
    AdiShaperSpectrum spectrum_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::uint64_t revision_ = 0;
    juce::Label status_;
};
} // namespace adi::ui
