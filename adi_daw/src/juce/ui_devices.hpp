// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <optional>
#include "adi/engine/param_edits.hpp"
#include "adi/mix_functions.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "ui_arrangement.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <string_view>
#include <utility>
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
    std::function<bool(const std::string &, engine::ParamEventKind, double)> parameterGesture;

  private:
    bool gesture(engine::ParamEventKind, double);
    DeviceChainStrip &strip;
    const panel::Record *record = nullptr;
    double defaultValue = 0;
    juce::Label name, value;
    std::int32_t menuSteps_ = 0;
    std::vector<std::string> menuLabels_;
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
// Color Cab's Impulse slot (MR 9.8; D5, N28, #414): a tile a sample file drops on, from the desktop
// or from the browser's Samples; Clear empties it. It names the slot and shows whether it is empty,
// holds a sample, or holds one whose file is missing, from the snapshot's sample slots.
class SampleTile final : public juce::Component,
                         public juce::SettableTooltipClient,
                         public juce::FileDragAndDropTarget,
                         public juce::DragAndDropTarget {
  public:
    SampleTile(DeviceChainStrip &, std::int64_t device, std::int32_t slot, juce::String slotName);
    void paint(juce::Graphics &) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray &files) override { return !files.isEmpty(); }
    void filesDropped(const juce::StringArray &files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    // The slot as the snapshot holds it; none when it was never assigned (empty).
    void show(const engine::SampleSlotNode *);
    // A file dropped: assigns the slot (one undo step), or says why not.
    bool drop(const juce::String &path);
    enum class State { Empty, Loaded, Missing };
    State state() const noexcept { return state_; }
    juce::TextButton clear{"Clear"};
    std::function<bool()> targetValid;
    const std::int64_t device;
    const std::int32_t slot;
    const juce::String slotName;

  private:
    DeviceChainStrip &strip_;
    State state_ = State::Empty;
    std::string problem_;
};
// One device in the chain, as Live draws it: a title bar with the activator,
// the name and Configure; the controls in columns that fit the strip's height.
class DevicePanelView final : public juce::Component {
  public:
    DevicePanelView(DeviceChainStrip &, const DevicePanelData &);
    ~DevicePanelView() override;
    void present(const DevicePanelData &);
    void resized() override;
    void paint(juce::Graphics &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void visibilityChanged() override;
    const mix::FunctionInfo *nativeFunction() const noexcept;
    std::uint64_t presentedInstance() const noexcept;
    // Capture the displayed authored identity separately from native drag presentation.
    bool beginFunctionDrag();
    const DevicePanelData *functionDragTarget() const noexcept;
    // The device the keyboard chose, itself or one of its controls: where a load goes (D4).
    void focusGained(FocusChangeType) override;
    void focusOfChildComponentChanged(FocusChangeType) override;
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
    struct ParameterPageControl {
        std::string id, name, enableParameter;
        std::string unavailableReason{};
        std::unique_ptr<juce::TextButton> selector;
        std::unique_ptr<ActivatorButton> enabled;
    };
    // Processing controls require backend groups. EQ's deferred Carve/Focus pages
    // explicitly show their missing dependencies and never expose processing controls.
    std::vector<ParameterPageControl> parameterPages;
    const std::string &selectedParameterPage() const noexcept { return selectedPage_; }
    bool chooseParameterPage(const std::string &);
    bool setParameterPageEnabled(const std::string &, bool);
    bool parameterShown(const std::string &) const;
    bool controlsMatch(const DevicePanelData &) const;
    juce::ComboBox functionBand, functionSection;
    std::int32_t selectedFunctionBand() const noexcept { return selectedBand_; }
    const std::string &selectedFunctionSection() const noexcept { return selectedSection_; }
    bool chooseFunctionBand(std::int32_t);
    bool chooseFunctionSection(const std::string &);
    std::string_view parameterPageUnavailableReason() const;
    // Color Cab's Impulse tile, beside its controls (D5); none on any other device.
    std::unique_ptr<SampleTile> sample;
    static constexpr int kTileWidth = 96;
    bool configuring = false;
    // Optional owner hooks for retained views (Mix View); the ordinary device strip uses its
    // existing authority. Hooks must never substitute a second parameter/undo implementation.
    std::function<bool(const std::string &, engine::ParamEventKind, double)> parameterGesture;
    enum class Interaction { Text, Menu };
    std::function<bool(Interaction)> interactionBegin;
    std::function<void(Interaction)> interactionEnd;
    std::function<bool()> targetValid;
    std::function<bool(bool forward)> navigate;
    // Retention is separate from native menu presentation so an embedded owner can cancel it.
    bool beginInteraction(Interaction);
    void endInteraction(Interaction);
    void cancelInteractions();

  private:
    void endRename(bool commit);
    bool editFunctionParameter(const std::string &, engine::ParamEventKind, double);
    void endFunctionParameter();
    bool validTarget() const { return !targetValid || targetValid(); }
    bool textInteraction_ = false, menuInteraction_ = false;
    std::uint64_t menuEpoch_ = 0;
    DeviceChainStrip &strip;
    const DevicePanelData *data = nullptr;
    std::optional<DevicePanelData> dragTarget_;
    std::string editingParameter_;
    double editingValue_ = 0;
    bool editingBegan_ = false;
    std::string selectedPage_;
    std::uint64_t pageEpoch_ = 0;
    std::uint64_t controlsEpoch_ = 0;
    std::int32_t selectedBand_ = 0;
    std::string selectedSection_;
    std::vector<std::int32_t> functionBands_;
    std::vector<std::pair<std::string, std::string>> functionSections_;
    static bool eqPresentation(const DevicePanelData &) noexcept;
    bool parameterShown(const DevicePanelData &, const std::string &) const;
    bool eqPresentation() const noexcept { return data && eqPresentation(*data); }
    void presentFunctionSelection(const DevicePanelData &);
    bool presentParameterPages(const DevicePanelData &);
    const panel::Record *pageEnableRecord(const ParameterPageControl &) const;
    static int pageSelectorWidth(const ParameterPageControl &) noexcept;
    static constexpr int kPageHeight = 22;
    friend class DeviceChainStrip;
};
class DeviceChainStrip final : public juce::Component, public juce::DragAndDropTarget {
  public:
    explicit DeviceChainStrip(AdiRootComponent &);
    ~DeviceChainStrip() override;
    void suspendParameters(bool);
    bool parametersSuspended() const noexcept { return parametersSuspended_; }
    void frame(std::int64_t track);
    void resized() override;
    void paint(juce::Graphics &) override;
    void fold(std::int64_t);
    bool folded(std::int64_t) const;
    bool edit(std::int64_t, const std::string &, engine::ParamEventKind, double);
    bool editExpected(const DevicePanelData &, const std::string &, engine::ParamEventKind, double);
    bool validExpected(const DevicePanelData &) const;
    bool action(const std::string &, Payload);
    juce::PopupMenu functionMenu();
    bool hasFunction(mix::Function) const;
    bool dropFunction(DevicePanelView &, std::optional<std::int64_t> before);
    void cancelFunctionInteractions();
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    juce::TextButton addTab{"+"};
    // Moves the focus from a device to its neighbour; at either end it stays.
    bool focusNext(std::int64_t from, bool forward);
    // Live's device clipboard (manual 23.2.1; N18): Cut, Copy, Paste and Duplicate on `device`, or
    // on the device with the focus. Paste goes in front of that device, or at the chain's end after
    // a click in the strip's empty space; Duplicate goes in front of the next device in its chain
    // and leaves the clipboard as it was. False when there is nothing to act on.
    enum class Clip { Cut, Copy, Paste, Duplicate };
    bool clipboard(Clip, std::optional<std::int64_t> device = std::nullopt);
    bool canClipboard(Clip) const;
    std::optional<std::int64_t> focusedDevice() const;
    // Whether the chain's end is chosen: the strip itself has the focus, after a click past the
    // last device.
    bool endChosen() const { return hasKeyboardFocus(false); }
    // Where a device loaded from the browser goes (manual 4.10): in front of the device chosen
    // last in the strip, though the browser now has the focus; at the chain's end after a click
    // past the last device, or when none is chosen.
    std::optional<std::int64_t> insertPoint() const;
    void chose(std::int64_t device) { chosen_ = device; }
    // A device dragged from the browser drops into the chain, in front of the device it lands on.
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    // The device a drop at `x` (the strip's coordinates) goes in front of; none past the last.
    std::optional<std::int64_t> dropBefore(int x) const;
    void mouseDown(const juce::MouseEvent &) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    AdiRootComponent &root;
    std::function<std::shared_ptr<const ParameterPublication>()> publication;
    std::function<void(const void *, const std::vector<std::int64_t> &)> parameterDemand;
    std::function<bool(std::int64_t, const std::string &, engine::ParamEventKind, double)> gesture;
    std::function<bool(std::int64_t, const std::string &, engine::ParamEventKind, double,
                       const engine::DeviceRuntimeToken &, std::uint64_t)> guardedGesture;
    std::function<bool(std::int64_t, const engine::DeviceRuntimeToken &, std::uint64_t)> targetCurrent;
    std::function<bool(const std::string &, Payload)> submit;
    std::function<mix::Availability(mix::Function)> functionAvailability;
    std::function<bool(mix::Function, std::int64_t track)> addFunction;
    std::function<bool(const DevicePanelData &, std::int64_t track,
                       std::optional<std::int64_t> before)> moveFunction;
    // The app's clipboard: `copy` keeps a device's copy (and removes it, for Cut); `paste` puts the
    // kept copy on `track`, in front of `before` or at the chain's end; `duplicate` copies and
    // pastes at once, the clipboard untouched; `holdsCopy` says whether there is one to paste.
    std::function<bool(std::int64_t device, bool cut)> copy;
    std::function<bool(std::int64_t track, std::optional<std::int64_t> before)> paste;
    std::function<bool(std::int64_t device, std::int64_t track, std::optional<std::int64_t> before)> duplicate;
    std::function<bool()> holdsCopy;
    // The app's sample slot edits (D5): a file assigned to a device's slot, or the slot cleared;
    // each one undo step, its refusal said in the Status Bar.
    std::function<bool(std::int64_t device, std::int32_t slot, const std::string &path)> assignSample;
    std::function<bool(std::int64_t device, std::int32_t slot)> clearSample;
    std::vector<std::unique_ptr<DevicePanelView>> panels;

  private:
    const DevicePanelData *dataOf(std::int64_t device) const;
    void showSamples();
    void updateParameterDemand(std::vector<std::int64_t>);
    std::vector<std::int64_t> demandedDevices_;
    bool parametersSuspended_ = false;
    std::int64_t selectedTrack = -1;
    std::optional<std::int64_t> chosen_;
    int end_ = 0; // where the chain ends, in the content's pixels
    juce::Viewport scroll;
    juce::Component content;
    std::shared_ptr<const ParameterPublication> presented;
    std::uint64_t addMenuEpoch_ = 0;
    std::uint64_t frameEpoch_ = 0;
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
