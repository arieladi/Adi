// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <optional>
#include "adi/engine/mpe_output.hpp"
#include "adi/engine/param_ops.hpp"
#include "adi/mix/limiter/ReductionMailbox.h"
#include "adi/mix_functions.hpp"
#include "adi/ui/parameter_feed.hpp"
#include "ui_arrangement.hpp"
#include "ui_controls.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
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
class SharedAnalysis;
// One parameter of the DAW-drawn panel (ADR-0150, ADR-0198): its name, a control
// shaped as the plug-in declares it, and the plug-in's own text for the value.
class ValueEntry;
class ParameterControl final : public juce::Component, public juce::SettableTooltipClient {
  public:
    // `defaultNormalized` is the plug-in's default for the parameter, where Delete returns it
    // (Live 12 manual 41.6, Return to Default).
    ParameterControl(DeviceChainStrip &, std::int64_t, const panel::Record &,
                     double defaultNormalized);
    ~ParameterControl() override;
    void present(const panel::Record &, double defaultNormalized);
    void resized() override;
    void paint(juce::Graphics &) override;
    // Delete and Backspace remove a hosted Configure control or reset its value outside Configure.
    // Up/Down nudge a knob; Enter or a numeric key edits its normalized value.
    // Left and right still move between devices.
    bool keyPressed(const juce::KeyPress &) override;
    bool returnToDefault();
    // The parameter's automation commands, from the control or its labels.
    void mouseDown(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    juce::PopupMenu menuItems();
    void showMenu();
    // The device's plug-in format, which names its parameters' lanes (`engine::automationParamRef`).
    std::string format;
    // Live's device column at 100 %: the name, a knob, the value under it.
    static constexpr int kWidth = 58, kHeight = 54;
    std::int64_t device;
    std::string parameter;
    controls::PrimaryMouseControl<juce::Slider> slider;
    controls::PrimaryMouseControl<juce::ToggleButton> toggle;
    juce::ComboBox menu;
    std::function<bool(const std::string &, engine::ParamEventKind, double)> parameterGesture;

  private:
    bool gesture(engine::ParamEventKind, double);
    bool setOnce(double);
    DeviceChainStrip &strip;
    const panel::Record *record = nullptr;
    double defaultValue = 0;
    juce::Label name, value;
    std::unique_ptr<ValueEntry> valueEntry_;
    int presentedMenuId_ = 0;
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
class HotSwapButton final : public juce::Button {
  public:
    HotSwapButton() : juce::Button("Hot-Swap") {}
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
class RevealSampleButton final : public juce::Button {
  public:
    RevealSampleButton() : juce::Button("Show Sample in Folder") {}
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// A built-in's sample slot (MR 9.8; D5, N28, #414): a tile a sample file drops on, from the desktop
// or from the browser's Samples; Clear empties it. It names the slot and shows whether it is empty,
// holds a sample, or holds one whose file is missing, from the snapshot's sample slots.
class SampleTile final : public juce::Component,
                         public juce::SettableTooltipClient,
                         public juce::FileDragAndDropTarget,
                         public juce::DragAndDropTarget {
  public:
    struct DeclaredSlot { std::int32_t id; juce::String name; };
    SampleTile(DeviceChainStrip &, std::int64_t device, std::vector<DeclaredSlot>);
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    bool isInterestedInFileDrag(const juce::StringArray &files) override { return !files.isEmpty(); }
    void fileDragEnter(const juce::StringArray &, int, int) override;
    void fileDragExit(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &files, int x, int y) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDragEnter(const SourceDetails &) override;
    void itemDragExit(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    // Refresh the selected slot from the project snapshot.
    void refresh();
    // A file dropped: assigns the slot (one undo step), or says why not.
    bool drop(const juce::String &path);
    enum class State { Empty, Loaded, Missing };
    State state() const noexcept { return state_; }
    const juce::String &fileName() const noexcept { return fileName_; }
    juce::Rectangle<int> fileNameBounds() const noexcept;
    juce::Rectangle<int> waveformBounds() const noexcept;
    juce::TextButton clear{"Clear"};
    juce::ComboBox slotChooser;
    HotSwapButton hotSwapButton;
    RevealSampleButton reveal;
    std::function<bool()> targetValid;
    const std::int64_t device;
    std::int32_t slot;
    juce::String slotName;

  private:
    void choose(const DeclaredSlot &);
    void setDropPreview(bool);
    DeviceChainStrip &strip_;
    std::vector<DeclaredSlot> slots_;
    State state_ = State::Empty;
    bool dropPreview_ = false;
    std::shared_ptr<const engine::SampleSlotNode> presented_;
    juce::String fileName_;
    WaveformCache *waveformsSeen_ = nullptr;
    std::uint64_t peaksSeen_ = 0;
    std::shared_ptr<const media::MediaPeaks> peaks_;
};
// One device in the chain, as Live draws it: a title bar with the activator,
// the name and Configure where available; the controls in columns that fit the strip's height.
class DevicePanelView final : public juce::Component,
                              public juce::FileDragAndDropTarget,
                              public juce::DragAndDropTarget,
                              public juce::TooltipClient {
  public:
    DevicePanelView(DeviceChainStrip &, const DevicePanelData &, bool mixCard = false);
    ~DevicePanelView() override;
    // A sample dropped on the tile, controls or title goes to the tile's selected slot.
    bool isInterestedInFileDrag(const juce::StringArray &files) override { return sample && !files.isEmpty(); }
    void fileDragEnter(const juce::StringArray &files, int x, int y) override {
        if (sample) sample->fileDragEnter(files, x, y);
    }
    void fileDragExit(const juce::StringArray &files) override { if (sample) sample->fileDragExit(files); }
    void filesDropped(const juce::StringArray &files, int x, int y) override {
        if (sample) sample->filesDropped(files, x, y);
    }
    bool isInterestedInDragSource(const SourceDetails &details) override {
        auto *tile = samplerTile();
        return tile && tile->isInterestedInDragSource(details);
    }
    void itemDragEnter(const SourceDetails &details) override {
        if (auto *tile = samplerTile()) tile->itemDragEnter(details);
    }
    void itemDragExit(const SourceDetails &details) override {
        if (sample) sample->itemDragExit(details);
    }
    void itemDropped(const SourceDetails &details) override {
        if (auto *tile = samplerTile()) tile->itemDropped(details);
    }
    void present(const DevicePanelData &);
    [[nodiscard]] bool hasPlaybackProblem() const noexcept;
    juce::String getTooltip() override;
    bool frameEqSpectrum(const std::shared_ptr<SharedAnalysis> &);
    void releaseEqSpectrum();
    void frameLimiterReduction(
        const std::function<mix::limiter::ReductionMailbox::Lease(const DevicePanelData &)>* observer);
    void releaseLimiterReduction();
    void resized() override;
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseMove(const juce::MouseEvent &) override;
    void mouseExit(const juce::MouseEvent &) override;
    bool pressEqGraph(juce::Point<float>);
    bool dragEqHandle(juce::Point<float>);
    void releaseEqHandle();
    void visibilityChanged() override;
    const mix::FunctionInfo *nativeFunction() const noexcept;
    std::uint64_t presentedInstance() const noexcept;
    // Capture the displayed authored identity separately from native drag presentation.
    bool beginFunctionDrag();
    const DevicePanelData *functionDragTarget() const noexcept;
    static constexpr const char *kDeviceMoveDrag = "adi-device-move";
    bool beginDeviceDrag();
    // The device the keyboard chose, itself or one of its controls: where a load goes (D4).
    void focusGained(FocusChangeType) override;
    void focusOfChildComponentChanged(FocusChangeType) override;
    // The width this device takes at a given strip height.
    int widthFor(int height) const noexcept;
    // Live's round fold switch in the title bar, beside the activator.
    juce::Rectangle<int> foldSwitch() const noexcept;
    juce::Rectangle<float> eqPreviewBounds() const;
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
    juce::TextButton parameterList{"Parameter List"};
    // Optional presenters for Configure's parameter list and its removal confirmation.
    std::function<void(juce::PopupMenu, std::function<void(int)>)> onParameterListMenu;
    std::function<void(juce::MessageBoxOptions, std::function<void(bool)>)> onPanelRemovalWarning;
    ActivatorButton enabled;
    WrenchButton wrench;
    HotSwapButton hotSwapButton;
    juce::TextEditor search;
    std::vector<std::unique_ptr<ParameterControl>> controls;
    struct ParameterPageControl {
        std::string id, name, enableParameter;
        std::string unavailableReason{};
        std::unique_ptr<juce::TextButton> selector;
        std::unique_ptr<ActivatorButton> enabled;
    };
    // Processing controls and selectors require real backend groups. Missing
    // EQ-family groups do not invent an unavailable page or processing controls.
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
    // A built-in's [adi.sample] slot tile beside its controls, initially on the first slot declared.
    std::unique_ptr<SampleTile> sample;
    bool sampleSlotsRead_ = false;
    static constexpr int kTileWidth = 96;
    bool configuring = false;
    // Optional owner hooks for retained views (Mix View); the ordinary device strip uses its
    // existing authority. Hooks must never substitute a second parameter/undo implementation.
    std::function<bool(const std::string &, engine::ParamEventKind, double)> parameterGesture;
    std::function<bool(const engine::ParameterPair &, engine::ParameterPairEvent)> parameterPairGesture;
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
    friend class ParameterControl;
    SampleTile *samplerTile() const noexcept { return data && data->instrument ? sample.get() : nullptr; }
    bool allowsConfigure() const noexcept;
    void endRename(bool commit);
    void showParameterList();
    bool deleteConfigureParameter(std::string parameter);
    void changeParameterPanel(const DevicePanelData &, std::uint64_t menuEpoch,
                              std::optional<std::vector<std::string>> params);
    void closeConfigure();
    void queueConfigureTouch(const DevicePanelData &);
    void applyConfigureTouches();
    bool beginParameterReorder(const std::string &parameter);
    void endParameterReorder(const std::string &parameter, juce::Point<int> position, int distance, bool labelClick);
    bool panelTargetCurrent(const DevicePanelData &);
    void chooseLastTouch(const DevicePanelData &, bool showAutomation);
    bool canHotSwap() const;
    bool requestHotSwap(DevicePanelData expected, std::optional<std::int32_t> slot, std::uint64_t epoch);
    bool editFunctionParameter(const std::string &, engine::ParamEventKind, double);
    void endFunctionParameter();
    void finishEqEdit(bool cancel);
    bool validTarget() const { return !targetValid || targetValid(); }
    const bool mixCard_;
    struct EqPreview;
    std::unique_ptr<EqPreview> eqPreview_;
    void updateEqPreview(const DevicePanelData &);
    int eqBandAt(juce::Point<float>) const;
    void drawEqNodes(juce::Graphics &, juce::Rectangle<float>) const;
    struct LimiterPreview;
    [[nodiscard]] juce::Rectangle<int> limiterPreviewBounds() const;
    [[nodiscard]] bool previewVisible(juce::Rectangle<int>) const;
    void paintLimiterReduction(juce::Graphics &, juce::Rectangle<int>) const;
    void layoutCompactControls(int top);
    void layoutTitle(bool folded);
    void updateTitleTooltip();
    bool refreshPlaybackProblem();
    bool playbackProblemPresented_ = false;
    bool textInteraction_ = false, menuInteraction_ = false;
    std::uint64_t menuEpoch_ = 0;
    juce::ScopedMessageBox panelRemovalWarning_;
    DeviceChainStrip &strip;
    const DevicePanelData *data = nullptr;
    struct ConfigureSession {
        DevicePanelData expected;
        std::uint64_t observed = 0;
        std::vector<std::string> pending;
        bool openedEditor = false;
    };
    std::optional<ConfigureSession> configureSession_;
    struct ParameterReorder {
        std::string parameter;
        std::vector<std::string> order;
        bool configured = false;
        std::uint64_t menuEpoch = 0, controlsEpoch = 0;
    };
    std::optional<ParameterReorder> parameterReorder_;
    std::optional<DevicePanelData> dragTarget_;
    std::vector<DevicePanelData> draggedDevices_;
    bool selectOnRelease_ = false;
    bool startedDeviceDrag_ = false;
    std::uint64_t dragEpoch_ = 0;
    std::string editingParameter_;
    double editingValue_ = 0;
    bool editingBegan_ = false;
    std::string selectedPage_;
    std::uint64_t pageEpoch_ = 0;
    std::uint64_t controlsEpoch_ = 0;
    std::int32_t selectedBand_ = 0;
    std::string selectedSection_;
    struct FunctionSelection { std::int32_t band = 0; std::string section; };
    std::map<std::string, FunctionSelection> functionSelectionByPage_;
    std::string functionSelectionPage_;
    std::vector<std::int32_t> functionBands_;
    std::vector<std::pair<std::string, std::string>> functionSections_;
    std::vector<std::string> functionBandSections_;
    static bool eqPresentation(const DevicePanelData &) noexcept;
    bool parameterShown(const DevicePanelData &, const std::string &) const;
    bool eqPresentation() const noexcept { return data && eqPresentation(*data); }
    void presentFunctionSelection(const DevicePanelData &);
    bool presentParameterPages(const DevicePanelData &);
    const panel::Record *pageEnableRecord(const ParameterPageControl &) const;
    static int pageSelectorWidth(const ParameterPageControl &) noexcept;
    static constexpr int kPageHeight = 22;
    static constexpr int kCompactCardHeight = 144;
    std::unique_ptr<LimiterPreview> limiterPreview_;
    friend class DeviceChainStrip;
};
class DeviceChainStrip final : public juce::Component,
                               public juce::DragAndDropTarget,
                               public juce::FileDragAndDropTarget,
                               private juce::AsyncUpdater {
  public:
    explicit DeviceChainStrip(AdiRootComponent &);
    ~DeviceChainStrip() override;
    void suspendParameters(bool);
    bool parametersSuspended() const noexcept { return parametersSuspended_; }
    void frame(std::int64_t track);
    void settleParameterTouches() { handleUpdateNowIfNeeded(); }
    // The engine projection supplies sorted, unique device IDs.
    void setPlaybackProblemIds(std::vector<std::int64_t> ids);
    [[nodiscard]] bool hasPlaybackProblem(std::int64_t id) const noexcept;
    void resized() override;
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    [[nodiscard]] const char *dropInstruction() const;
    [[nodiscard]] juce::Rectangle<int> dropInstructionArea() const noexcept;
    void fold(std::int64_t);
    bool folded(std::int64_t) const;
    bool edit(std::int64_t, const std::string &, engine::ParamEventKind, double);
    bool editExpected(const DevicePanelData &, const std::string &, engine::ParamEventKind, double);
    bool editPairExpected(const DevicePanelData &, const engine::ParameterPair &, engine::ParameterPairEvent,
                          std::shared_ptr<const gesture::OwnerToken> &);
    bool validExpected(const DevicePanelData &) const;
    bool addLastTouchedParameter(const DevicePanelData &expected);
    bool action(const std::string &, Payload);
    // Native functions remove with Undo; ordinary plug-ins retain bypass.
    bool setEnabledExpected(const DevicePanelData &, bool);
    juce::PopupMenu functionMenu();
    // Explicit target and lifetime for another visible surface, without selecting its channel.
    juce::PopupMenu functionMenuFor(std::int64_t, std::function<bool()> ownerCurrent);
    bool hasFunction(mix::Function) const;
    bool dropFunction(DevicePanelView &, std::optional<std::int64_t> before);
    void cancelFunctionInteractions();
    void visibilityChanged() override;
    void parentHierarchyChanged() override;
    juce::TextButton addTab{"+"};
    // Moves focus to a neighbour or past the last device to the insertion point.
    bool focusNext(std::int64_t from, bool forward);
    // Live's device clipboard (manual 23.2.1; N18): Cut, Copy, Paste and Duplicate on `device`, or
    // on the device with the focus. Paste goes in front of that device, or at the chain's end after
    // choosing the strip's empty space; Duplicate goes in front of the next device in its chain
    // and leaves the clipboard as it was. False when there is nothing to act on.
    enum class Clip { Cut, Copy, Paste, Duplicate };
    bool clipboard(Clip, std::optional<std::int64_t> device = std::nullopt, bool includeSelection = false);
    bool canClipboard(Clip) const;
    // Paste into an explicit destination, even when the strip itself lacks focus.
    bool pasteAt(std::int64_t track, std::optional<std::int64_t> before);
    std::optional<std::int64_t> focusedDevice() const;
    bool hasHotSwapContext() const;
    bool hotSwapFocused();
    bool deleteDevices(std::optional<std::int64_t> device = {});
    // The strip itself has focus when the insertion point after the last device is chosen.
    bool endChosen() const { return hasKeyboardFocus(false); }
    // Where a device loaded from the browser goes (manual 4.10): in front of the device chosen
    // last in the strip, though the browser now has the focus; at the chain's end when no device
    // is chosen.
    std::optional<std::int64_t> insertPoint() const;
    void chose(std::int64_t device) { chosen_ = device; }
    // A browser device loads here. An existing device moves to this position in the chain.
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDragEnter(const SourceDetails &) override;
    void itemDragMove(const SourceDetails &) override;
    void itemDragExit(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    bool isMovingDevice(const SourceDetails &) const;
    bool moveDroppedDevice(const SourceDetails &, std::int64_t track, std::optional<std::int64_t> before = {});
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void fileDragEnter(const juce::StringArray &, int, int) override;
    void fileDragExit(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    // The device a drop at `x` (the strip's coordinates) goes in front of; none past the last.
    std::optional<std::int64_t> dropBefore(int x) const;
    void mouseDown(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    AdiRootComponent &root;
    // Optional presenter for the empty-space context menu.
    std::function<void(juce::PopupMenu)> onEndMenu;
    std::function<std::shared_ptr<const ParameterPublication>()> publication;
    // Publish native gestures already waiting when a Configure session opens.
    std::function<void()> pollParameterEdits;
    std::function<void(const void *, const std::vector<std::int64_t> &)> parameterDemand;
    std::function<bool(std::int64_t, const std::string &, engine::ParamEventKind, double)> gesture;
    std::function<bool(std::int64_t, const std::string &, engine::ParamEventKind, double,
                       const engine::DeviceRuntimeToken &, std::uint64_t)> guardedGesture;
    std::function<bool(std::int64_t, const engine::ParameterPair &, engine::ParameterPairEvent,
                       const engine::DeviceRuntimeToken &, std::uint64_t,
                       std::shared_ptr<const gesture::OwnerToken> &)> guardedPairGesture;
    std::function<bool(std::int64_t, const engine::DeviceRuntimeToken &, std::uint64_t)> targetCurrent;
    std::function<bool(const std::string &, Payload)> submit;
    // The application names and saves a preset for this exact loaded device.
    std::function<void(const DevicePanelData &)> savePreset;
    std::function<void(const DevicePanelData &)> saveDefaultPreset;
    // A slot is present when the Browser should offer samples for this device slot.
    std::function<void(const DevicePanelData &, std::optional<std::int32_t> slot)> hotSwap;
    std::function<std::vector<DeviceKeyChoice>(const DevicePanelData&)> keyChoices;
    std::function<bool(const DevicePanelData&,const DeviceKeyChoice&)> setKey;
    // A plug-in's expression route chosen from its menu, after the project took it (SC-325): the app
    // remembers it for that plug-in, the default of its next instance.
    std::function<void(const DevicePanelData&, engine::RouteChoice)> rememberRoute;
    std::function<mix::Availability(mix::Function)> functionAvailability;
    std::function<bool(mix::Function, std::int64_t track)> addFunction;
    std::function<bool(const DevicePanelData &, std::int64_t track,
                       std::optional<std::int64_t> before)> moveFunction;
    std::function<bool(const DevicePanelData &, std::int64_t track,
                       std::optional<std::int64_t> before)> moveDevice;
    // The app's clipboard: `copy` keeps a device's copy (and removes it, for Cut); `paste` puts the
    // kept copy on `track`, in front of `before` or at the chain's end; `duplicate` copies and
    // pastes at once, the clipboard untouched; `holdsCopy` says whether there is one to paste.
    std::function<bool(std::int64_t device, bool cut)> copy;
    std::function<bool(std::int64_t track, std::optional<std::int64_t> before)> paste;
    std::function<bool(std::int64_t device, std::int64_t track, std::optional<std::int64_t> before)> duplicate;
    std::function<bool()> holdsCopy;
    // A title range acts as one edit. Each item carries the identity observed by the UI.
    std::function<bool(std::span<const DevicePanelData>)> removeSelection;
    std::function<bool(std::span<const DevicePanelData>, bool cut)> copySelection;
    std::function<bool(std::span<const DevicePanelData>, std::int64_t track, std::optional<std::int64_t> before)>
        duplicateSelection, moveSelection;
    // The app's sample slot edits (D5): a file assigned to a device's slot, or the slot cleared;
    // each one undo step, its refusal said in the Status Bar.
    std::function<bool(std::int64_t device, std::int32_t slot, const std::string &path)> assignSample;
    std::function<bool(std::int64_t device, std::int32_t slot)> clearSample;
    std::function<void(const std::filesystem::path &)> revealSample;
    std::vector<std::unique_ptr<DevicePanelView>> panels;

  private:
    struct OutputMeter;
    juce::Rectangle<int> outputMeterBounds(std::size_t gap) const noexcept;
    void frameOutputMeters();
    void releaseOutputMeters();
    std::vector<OutputMeter> outputMeters_;
    std::weak_ptr<SharedAnalysis> outputMeterBroker_;
    void setDropPreview(std::optional<int>);
    void handleAsyncUpdate() override;
    void cancelConfigureTouches();
    std::shared_ptr<const ParameterPublication> latestPublication() const;
    struct PickedDevice {
        std::int64_t id, track, chain;
        std::uint64_t instance;
        bool matches(const DevicePanelData &) const;
    };
    bool isSelected(std::int64_t) const;
    bool selectionActsOn(std::int64_t) const;
    void selectTitle(const DevicePanelView &, bool extend);
    void clearSelection();
    void chooseEnd();
    std::vector<DevicePanelData> selectedTargets() const;
    bool clipboardSelection(Clip);
    DevicePanelView *focusedHotSwapPanel() const;
    DevicePanelView *moveSource(const SourceDetails &) const;
    bool acceptsSample() const;
    const DevicePanelData *dataOf(std::int64_t device) const;
    void showSamples();
    void updateParameterDemand(std::vector<std::int64_t>);
    std::vector<std::int64_t> demandedDevices_;
    std::vector<std::int64_t> playbackProblemIds_;
    bool parametersSuspended_ = false;
    std::int64_t selectedTrack = -1;
    std::optional<std::int64_t> chosen_;
    std::vector<PickedDevice> selected_;
    std::optional<PickedDevice> selectionAnchor_;
    std::uint64_t selectionEpoch_ = 0;
    int end_ = 0; // where the chain ends, in the content's pixels
    std::optional<int> dropPreviewX_; // the cursor, in the strip's pixels
    juce::Viewport scroll;
    juce::Component content;
    std::shared_ptr<const ParameterPublication> presented;
    std::uint64_t addMenuEpoch_ = 0;
    std::uint64_t frameEpoch_ = 0;
    friend class DevicePanelView;
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
