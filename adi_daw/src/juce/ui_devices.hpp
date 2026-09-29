// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/param_edits.hpp"
#include "adi/ui/parameter_feed.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace adi::ui {
class AdiRootComponent;
class DeviceChainStrip;
// One parameter of the DAW-drawn panel (ADR-0150, ADR-0198): its name, a control
// shaped as the plug-in declares it, and the plug-in's own text for the value.
class ParameterControl final : public juce::Component, public juce::SettableTooltipClient {
  public:
    ParameterControl(DeviceChainStrip &, std::int64_t, const panel::Record &);
    void present(const panel::Record &);
    void resized() override;
    void paint(juce::Graphics &) override;
    static constexpr int kWidth = 112, kHeight = 46;
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
// One device in the chain, as Live draws it: a title bar with the activator,
// the name and Configure; the controls in columns that fit the strip's height.
class DevicePanelView final : public juce::Component {
  public:
    DevicePanelView(DeviceChainStrip &, const DevicePanelData &);
    void present(const DevicePanelData &);
    void resized() override;
    void paint(juce::Graphics &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    // The width this device takes at a given strip height.
    int widthFor(int height) const noexcept;
    void configure();
    void contextMenu(juce::Point<int>);
    static constexpr int kTitleHeight = 22, kFoldedWidth = 26;
    std::int64_t id;
    juce::TextButton title, configureButton{"Configure"};
    juce::ToggleButton enabled;
    juce::TextEditor search;
    std::vector<std::unique_ptr<ParameterControl>> controls;
    bool configuring = false;

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
// The strip's top edge: drag it up to grow the strip (ADR-0184), never below its floor.
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
