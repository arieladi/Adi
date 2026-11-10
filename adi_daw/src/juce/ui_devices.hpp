// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/param_edits.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "ui_arrangement.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace adi::ui {
// A view that draws on its window's frame clock and never drives its own repaint
// (ADR-0180 d1): the floating host calls frame() from the window's VBlank.
class ClockedView : public juce::Component {
  public:
    virtual void frame() noexcept = 0;
};
class AdiRootComponent;
class DeviceChainStrip;
// One parameter of the DAW-drawn panel (ADR-0150, ADR-0198): its name, a control
// shaped as the plug-in declares it, and the plug-in's own text for the value.
class ParameterControl final : public juce::Component, public juce::SettableTooltipClient {
  public:
    // `defaultNormalized` is the plug-in's default for the parameter, where Delete returns it
    // (Live 12 manual 41.6, Return to Default).
    ParameterControl(DeviceChainStrip &, std::int64_t, const panel::Record &,
                     double defaultNormalized);
    void present(const panel::Record &, double defaultNormalized);
    void resized() override;
    void paint(juce::Graphics &) override;
    // A clicked control takes the focus, so Live's value keys reach it (manual 41.6): Delete
    // or Backspace is Return to Default, one gesture to the plug-in's default, one undo step.
    // Other keys go on to the device, so the left and right arrows still move between devices.
    bool keyPressed(const juce::KeyPress &) override;
    bool returnToDefault();
    // Live's device column at 100 %: the name, a knob, the value under it.
    static constexpr int kWidth = 58, kHeight = 54;
    std::int64_t device;
    std::string parameter;
    juce::Slider slider;
    juce::ToggleButton toggle;
    juce::ComboBox menu;

  private:
    void gesture(engine::ParamEventKind, double);
    DeviceChainStrip &strip;
    const panel::Record *record = nullptr;
    double defaultValue = 0;
    juce::Label name, value;
};
// The Device Activator: Live's round switch at the left of a device's title bar,
// orange when the device is on.
class ActivatorButton final : public juce::ToggleButton {
  public:
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// Live's wrench in a plug-in device's title bar (manual 23.3): opens and closes the plug-in's own
// window; lit while it is open.
class WrenchButton final : public juce::Button {
  public:
    WrenchButton() : juce::Button("Plug-In Window") {}
    void paintButton(juce::Graphics &, bool over, bool down) override;
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
    // Live's round fold switch in the title bar, beside the activator.
    juce::Rectangle<int> foldSwitch() const noexcept;
    void configure();
    // The title bar's menu, in Live's order: the editing commands, then the device's own.
    juce::PopupMenu commands();
    void contextMenu(juce::Point<int>);
    // Rename (Ctrl/Cmd+R, the menu): edit the name in the title bar, as Live does.
    bool beginRename();
    RenameEditor rename;
    static constexpr int kTitleHeight = 17, kFoldedWidth = 22;
    std::int64_t id;
    juce::TextButton title, configureButton{"Configure"};
    ActivatorButton enabled;
    WrenchButton wrench;
    juce::TextEditor search;
    std::vector<std::unique_ptr<ParameterControl>> controls;
    bool configuring = false;

  private:
    void endRename(bool commit);
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
    // Moves the focus from a device to its neighbour; at either end it stays.
    bool focusNext(std::int64_t from, bool forward);
    AdiRootComponent &root;
    std::function<std::shared_ptr<const ParameterPublication>()> publication;
    std::function<bool(std::int64_t, const std::string &, engine::ParamEventKind, double)> gesture;
    std::function<bool(const std::string &, Payload)> submit;
    std::vector<std::unique_ptr<DevicePanelView>> panels;

  private:
    std::int64_t selectedTrack = -1;
    int end_ = 0; // where the chain ends, in the content's pixels
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
