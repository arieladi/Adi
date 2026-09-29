// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/param_edits.hpp"
#include "adi/ui/parameter_feed.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
namespace adi::ui {
class AdiRootComponent;
class DeviceChainStrip;
class ParameterControl final : public juce::Component {
  public:
    ParameterControl(DeviceChainStrip &, std::int64_t, const panel::Record &);
    void present(const panel::Record &);
    void resized() override;
    void paint(juce::Graphics &) override;
    std::int64_t device;
    std::string parameter;
    juce::Slider slider;
    juce::ToggleButton toggle;
    juce::ComboBox menu;

  private:
    void gesture(engine::ParamEventKind, double);
    DeviceChainStrip &strip;
    const panel::Record *record = nullptr;
    juce::Label name, value;
};
class DevicePanelView final : public juce::Component {
  public:
    DevicePanelView(DeviceChainStrip &, const DevicePanelData &);
    void present(const DevicePanelData &);
    void resized() override;
    void paint(juce::Graphics &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void configure();
    std::int64_t id;
    juce::TextButton title, configureButton{"Configure"};
    juce::ToggleButton enabled;
    juce::TextEditor search;
    std::vector<std::unique_ptr<ParameterControl>> controls;

  private:
    DeviceChainStrip &strip;
    const DevicePanelData *data = nullptr;
};
class DeviceChainStrip final : public juce::Component {
  public:
    explicit DeviceChainStrip(AdiRootComponent &);
    void frame(std::int64_t track);
    void resized() override;
    void paint(juce::Graphics &) override;
    void fold(std::int64_t);
    bool folded(std::int64_t) const;
    bool edit(std::int64_t, const std::string &, engine::ParamEventKind, double);
    bool action(const std::string &, Payload);
    AdiRootComponent &root;
    std::function<std::shared_ptr<const ParameterPublication>()> publication;
    std::function<bool(std::int64_t, const std::string &, engine::ParamEventKind, double)> gesture;
    std::function<bool(const std::string &, Payload)> submit;
    std::vector<std::unique_ptr<DevicePanelView>> panels;

  private:
    std::int64_t selectedTrack = -1;
    juce::Viewport scroll;
    juce::Component content;
    std::shared_ptr<const ParameterPublication> presented;
};
class DockResizer final : public juce::Component {
  public:
    explicit DockResizer(AdiRootComponent &);
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

  private:
    AdiRootComponent &root;
    int start = 0;
};
} // namespace adi::ui
