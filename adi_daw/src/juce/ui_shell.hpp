// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/project_view.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/window_state.hpp"
#include "ui_arrangement.hpp"
#include "ui_browser.hpp"
#include "ui_controlbar.hpp"
#include "ui_look.hpp"
#include "ui_devices.hpp"
#include "ui_mixer.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <optional>
namespace adi::ui {
class AdiRootComponent;
// The app-scoped command layer (ADR-0180 d6): one manager, installed on every window.
// The keys are Live 12's (manual §41, ADR-0203 d6) unless a preset says otherwise.
class AppCommands final : public juce::ApplicationCommandTarget {
  public:
    enum Id {
        PlayStop = 0x7100,
        Stop,
        Undo,
        Redo,
        SwapPanels,
        ToggleBrowser,
        ToggleMixer,
        ToggleDeviceView,
        ToggleInfoView,
        ZoomIn,
        ZoomOut,
        NewProject,
        OpenProject,
        TakeSnapshot,
        SaveAs,
        Settings,
        TimeZoomIn,
        TimeZoomOut,
        ZoomToSelection,
        ZoomBack,
        OptimizeWidth,
        OptimizeHeight,
        GoStart,
        ContinuePlay,
        InsertAudioTrack,
        InsertMidiTrack,
        DeleteTrack,
        RenameTrack,
        ToggleMixerView,
        ShowChannelEq,
        Quit,
        FocusControlBar,
        FocusArrangement,
        FocusDeviceView,
        FocusBrowser,
        SearchBrowser,
        TabMovesFocus,
        Follow,
        InsertMarkerToPlayhead,
        GrowTracks,
        ShrinkTracks,
        FocusMixer,
        NextNeighbor,
        PreviousNeighbor,
        FullScreen,
        ToggleOverview,
        ToggleInOut,
        NarrowGrid,
        WidenGrid,
        TripletGrid,
        SnapToGrid,
        FixedGrid,
        WrapTabNavigation,
        LastId = WrapTabNavigation
    };
    explicit AppCommands(settings::AppSettings &);
    void reload();
    bool key(const juce::KeyPress &, AdiRootComponent &);
    void activate(AdiRootComponent &);
    void detach(AdiRootComponent &);
    juce::ApplicationCommandTarget *getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID> &) override;
    void getCommandInfo(juce::CommandID, juce::ApplicationCommandInfo &) override;
    bool perform(const InvocationInfo &) override;
    // App-scope view settings every window reads (ADR-0131, ADR-0203 d12).
    bool infoView() const noexcept { return infoView_; }
    bool setInfoView(bool on, std::string &why);
    bool tooltips() const noexcept { return tooltips_; }
    int tooltipDelayMs() const noexcept { return tooltipDelayMs_; }
    // Live's Use Tab Key to Move Focus (§41.1.1), off by default.
    bool tabMovesFocus() const noexcept { return tabMovesFocus_; }
    // How Follow keeps the playhead on screen: by page (Live's default), or by scrolling.
    bool followByPage() const noexcept { return followByPage_; }
    bool setTabMovesFocus(bool on, std::string &why);
    // Live's Wrap Tab Navigation (§41.1.1), on by default: Tab from an area's last control
    // returns to its first; off, it stops there.
    bool wrapTabNavigation() const noexcept { return wrapTab_; }
    bool setWrapTabNavigation(bool on, std::string &why);
    // Live's Move Clips with Arrow Keys (§41.1.1), on by default; off, left and right
    // collapse a selection to its start or its end.
    bool arrowKeysMoveClips() const noexcept { return arrowKeysMoveClips_; }
    // Ctrl/Cmd+wheel zooms around the selection, as Live; off, around the cursor.
    bool zoomOnSelection() const noexcept { return zoomOnSelection_; }
    juce::ApplicationCommandManager manager;

  private:
    settings::AppSettings &settings_;
    AdiRootComponent *active_ = nullptr;
    bool live_ = true;
    bool infoView_ = true;
    bool tooltips_ = true;
    int tooltipDelayMs_ = 600;
    bool zoomOnSelection_ = true;
    bool tabMovesFocus_ = false;
    bool wrapTab_ = true, arrowKeysMoveClips_ = true;
    bool followByPage_ = true;
};
// Live's Status Bar (manual 3.2): messages along the window's foot, with the
// Info View's show/hide toggle in its bottom-left corner (manual 2.2.2).
class StatusBar final : public juce::Component {
  public:
    explicit StatusBar(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    juce::TextButton infoToggle{""}, detailToggle{""}, mixerToggle{"Mixer"};

  private:
    AdiRootComponent &root_;
};
class AdiRootComponent final : public juce::Component {
  public:
    AdiRootComponent(engine::ProjectView &, OpSubmitter &, TransportMailbox &, AppCommands &,
                     ViewStateStore &, SessionState &, std::string window,
                     DesktopDefaults defaults = {});
    ~AdiRootComponent() override;
    void frame() noexcept;
    void paint(juce::Graphics &) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress &) override;
    bool command(int);
    void mark(unsigned bits) noexcept { dirty_.mark(bits); }
    const SnapshotReader &reader() const noexcept { return *reader_; }
    bool playing() const noexcept { return playing_; }
    double bpm() const noexcept;
    std::uint64_t generation() const noexcept { return generation_; }
    unsigned lastDrain() const noexcept { return lastDrain_; }
    const std::string &error() const noexcept { return error_; }
    WindowState &state() noexcept { return state_; }
    bool persist();
    // A message for the Status Bar from outside the root (the application's commands).
    void say(std::string message);
    // What the Status Bar says when there is no message: Live's line for the insert
    // marker or the time selection.
    juce::String selectionText() const;
    bool canUndo(bool redo) const { return redo ? submitter_.canRedo() : submitter_.canUndo(); }
    // What Live's Edit menu shows: "Undo" and the step's own label.
    std::string undoTitle(bool redo) const;
    // The application's commands (New, Open, Take Snapshot, Save As, Settings) and
    // the engine refresh after an edit belong to whoever owns the document.
    std::function<bool(int)> applicationCommand;
    std::function<void()> afterEdit;
    // The window's own interface zoom (ADR-0203 d6, A5-01): each window keeps its
    // own, and the ZoomHost around the root applies it as a transform.
    float interfaceZoom() const noexcept { return interfaceZoom_; }
    // Whether this view's own window fills the screen (Live's Full Screen, §42.1).
    bool fullScreen() const;
    // What the Info View shows: the element under the mouse, read in the frame.
    const juce::String &infoText() const noexcept { return infoText_; }
    void hover(juce::Component *);
    // Laid-out rectangles, in the root's logical units, for tests and children.
    juce::Rectangle<int> deviceArea() const noexcept { return deviceArea_; }
    juce::Rectangle<int> infoArea() const noexcept { return infoArea_; }
    juce::Rectangle<int> browserArea() const noexcept { return browserArea_; }
    juce::Rectangle<int> chatArea() const noexcept { return chatArea_; }
    AppCommands &commands() noexcept { return commands_; }
    // The project as it is now: what an action lands on, never this window's last frame.
    std::shared_ptr<const engine::Snapshot> current() const { return view_.current(); }
    // The playhead in ticks, from the driver's feedback at the device's rate.
    std::int64_t timelineTick() const noexcept;
    // Asks the driver to move the playhead; reads the current cell on arrival.
    bool locate(std::int64_t tick);
    // Plays from a point: starts the transport there, or jumps there while it plays.
    bool playFrom(std::int64_t tick);
    TransportBar transport;
    StatusBar status;
    juce::TextButton aiTab{"AI"};
    BrowserPanel browser{*this};
    ArrangementView arrangement;
    DeviceChainStrip devices;
    DockResizer dockResizer;
    MixerPanel mixer;
    MixerPanel mixerView{*this, true};
    MixerViewResizer mixerViewResizer{*this};
    // Live's strip down the arrangement's outer edge, with the flip arrows.
    SideStrip strip;
    juce::Rectangle<int> stripArea() const noexcept { return stripArea_; }
    juce::Rectangle<int> mixerViewArea() const noexcept { return mixerViewArea_; }
    // Drag the Mixer View's top edge: taller strips, never past one track row.
    void resizeMixerView(int height);
    juce::Rectangle<int> mixerArea() const noexcept { return mixerArea_; }
    // One edit from a control, as one named undo step; a refusal goes to the Status Bar.
    bool submitEdit(const std::string &op, Payload payload, const char *label);
    // Hands this window's selection to the session, which every window presents.
    void publishSelection();
    SessionState &session() noexcept { return session_; }
    // Drag the strip's top edge: never below the floor, never past the ceiling that
    // leaves the transport, the ruler and one track row (ADR-0184, ADR-0203 d7).
    void resizeDevices(int height);

  private:
    bool insertTrack(bool midi);
    bool deleteTrack();
    bool focusView(int);
    bool fail(std::string message);
    bool toggle(Panel);
    engine::ProjectView &view_;
    OpSubmitter &submitter_;
    TransportMailbox &mailbox_;
    AppCommands &commands_;
    ViewStateStore &persistence_;
    SessionState &session_;
    std::uint64_t selectionGeneration_ = 0;
    std::string window_;
    WindowState state_;
    DirtySet dirty_;
    std::optional<SnapshotReader> reader_;
    std::uint64_t generation_ = 0;
    unsigned lastDrain_ = 0;
    bool playing_ = false;
    bool infoShown_ = true;
    std::int64_t position_ = 0;
    double sampleRate_ = 0; // the device's granted rate, once a driver runs
    float interfaceZoom_ = 1;
    juce::Component *hovered_ = nullptr;
    juce::String infoText_;
    juce::Rectangle<int> deviceArea_, infoArea_, browserArea_, chatArea_, mixerArea_,
        mixerViewArea_, stripArea_;
    int leftWidth_ = 0, rightWidth_ = 0;
    std::string error_;
};
// The interface zoom's steps, 50 % to 200 % (ADR-0129 d3): the next one in or out of
// `now`, or nothing at either end.
std::optional<float> nextZoom(float now, bool in) noexcept;
// Holds a root at its window's interface zoom: the root lays out in logical
// units and the transform scales it, so nothing below knows the zoom exists.
class ZoomHost final : public juce::Component {
  public:
    explicit ZoomHost(std::unique_ptr<AdiRootComponent>);
    void resized() override;
    AdiRootComponent &root() noexcept { return *root_; }

  private:
    std::unique_ptr<AdiRootComponent> root_;
};
// The Mac's application menu, Live's "Live" menu: Settings, above the system's own items
// (Live 12 manual §2.3). Off the Mac, Settings is the Options menu's.
juce::PopupMenu applicationMenu(AppCommands &);
// Live on Windows (§42.28): Alt and a menu's first letter opens that menu. The menu's index
// in the bar, or -1 when the key opens none.
int menuForKey(juce::MenuBarModel &, const juce::KeyPress &);
// A native window's tooltips, kept to the App settings: made or removed as tooltips are
// switched on or off, and given a new delay, so a change in the Settings window reaches an
// open window at its next frame. `delayMs` is the delay last given.
void followTooltips(juce::Component &window, const AppCommands &,
                    std::unique_ptr<juce::TooltipWindow> &, int &delayMs);
// One attachment, key binding and tooltip window per native window, sharing
// the app command manager.
class ShellMenu;
class AdiWindow final : public juce::DocumentWindow, private juce::KeyListener {
  public:
    // `onDesktop` false builds the window without a native peer, for tests.
    AdiWindow(std::unique_ptr<AdiRootComponent>, AppCommands &, bool onDesktop = true);
    ~AdiWindow() override;
    using juce::DocumentWindow::keyPressed;
    std::function<void()> onClose;
    void closeButtonPressed() override {
        if (onClose)
            onClose();
        else
            setVisible(false);
    }
    juce::MenuBarModel *menu() noexcept;
    // What the window keeps of the App settings, brought up to date: each frame does it.
    void followSettings();
    AdiRootComponent &root() noexcept { return *root_; }
    const juce::TooltipWindow *tooltips() const noexcept { return tips_.get(); }
    int tooltipDelayMs() const noexcept { return tipDelayMs_; }

  private:
    bool keyPressed(const juce::KeyPress &k, juce::Component *) override;
    AdiRootComponent *root_;
    AppCommands &commands_;
    std::unique_ptr<juce::TooltipWindow> tips_;
    int tipDelayMs_ = 0;
    std::unique_ptr<ShellMenu> menu_;
    juce::VBlankAttachment clock_;
};
} // namespace adi::ui
