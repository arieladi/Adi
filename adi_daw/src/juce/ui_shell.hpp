// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/midi_ports.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/settings/store.hpp"
#include "adi/ui/arm_edits.hpp"
#include "adi/ui/computer_midi_keyboard.hpp"
#include "adi/gesture_producers.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/export_choices.hpp"
#include "adi/ui/external_input_meter.hpp"
#include "adi/ui/automation_mode.hpp"
#include "adi/ui/import_choices.hpp"
#include "adi/ui/live_midi_take.hpp"
#include "adi/ui/locator_edits.hpp"
#include "adi/ui/loop_edits.hpp"
#include "adi/ui/op_submitter.hpp"
#include "adi/ui/momentary_latch.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/ui/tap_tempo.hpp"
#include "ui_input_clock.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/window_state.hpp"
#include "ui_arrangement.hpp"
#include "ui_history.hpp"
#include "ui_browser.hpp"
#include "ui_controlbar.hpp"
#include "ui_look.hpp"
#include "ui_place.hpp"
#include "adi/clip_commands.hpp"
#include "adi/automation_clipboard.hpp"
#include "ui_devices.hpp"
#include "ui_clipview.hpp"
#include "ui_analysis.hpp"
#include "ui_mixer.hpp"
#include "ui_mixview.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h> // the Shortcuts page's key editor (WIN-04)
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>
namespace adi::automation {
struct NativeSpliceKnot;
}
namespace adi::controllers {
struct Address;
}
namespace adi::ui {
class AdiRootComponent;
class FloatingHost;

struct AdditionalAutomationLane {
    AutomationTarget target;
    int height = 0;
};
class ProjectDocument;
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
        LargeTracks,
        GroupTracks,
        UngroupTracks,
        ToggleMixView,
        DetachMixView,
        ToggleReferenceTrack,
        PlayReference,
        SelectAll,
        InsertReturnTrack,
        ToggleReturnTracks,
        ToggleSends,
        ToggleMixerInOut,
        SoloSelected,
        DeactivateSelected,
        ActivateTrack1,
        ActivateTrack8 = ActivateTrack1 + 7,
        ToggleMixerSends,
        InsertTimeSignatureChange,
        SoloInPlace,
        ToggleTrackOptions,
        ToggleMixerTrackOptions,
        Metronome,
        LoopSelection,
        SelectLoop,
        AddLocator,
        DeleteLocator,
        EditInfoText,
        UndoHistory,
        SaveCopy,
        CollectAllAndSave,
        CollectAndExport,
        Cut,
        Copy,
        Paste,
        Duplicate,
        TogglePluginWindows,
        Split,
        ToggleClipView,
        InsertMidiClip,
        CutTime,
        CopyTime,
        PasteTime,
        DuplicateTime,
        DeleteTime,
        InsertSilence,
        CreateFade,
        DeleteFades,
        // The Note Editor's own (manual 10.5 and on): in the Edit, View and Options menus.
        InvertSelection,
        Quantize,
        QuantizeSettings,
        CropClip,
        ExpandClipView,
        FoldToNotes,
        FoldToScale,
        HighlightScales,
        DrawMode,
        // The Clip View's panels (manual 8.1.2): the View menu's three.
        ArrangePanelsAutomatically,
        ArrangePanelsHorizontally,
        ArrangePanelsVertically,
        FocusClipView, // Live's Navigate menu: Clip View, Alt/Option+3
        FitToTimeRange, // the Note Editor's (manual 10.5.5), Ctrl/Cmd+Alt+J
        ArrangementRecord, // the Control Bar's Arrangement Record (manual 21.1), F9
        ExportAudio,       // the File menu's Export Audio/Video (manual 5.2.1), Ctrl/Cmd+Shift+R
        DelayCompensation, // Live's Options > Delay Compensation (manual 18.7; MX-04)
        SoloSwitches,      // Live's Options menu (manual 18.6; MX-08): the Solo buttons solo
        CueSwitches,       // ... or cue, sending the track to Cue Out
        HelpManual,        // the Help menu's manual (WIN-09)
        KeyboardReference, // the Help menu's keyboard reference, written from the keys in force (WIN-09)
        AutomationMode,    // the View menu's Automation Mode, A (Live 12 manual 25.2, 41.7)
        DeleteAutomation,
        ArmSelected,
        ToggleTrackMixer,
        SwitchDetailView,
        ArrangementRecordAlternate, // Shift+F9: the other Start Playback with Record behavior
        ComputerMidiKeyboard, // the Control Bar and Options menu's Computer MIDI Keyboard, M
        ImportMidiFile,
        ExportMidiClip,
        About,
        ImportAudioFile,
        ToggleBrowserPreview,
        PreviewSelected,
        HotSwap,
        ManageProjectFiles,
        ToggleFavorite,
        ToggleCollectionRed,
        ToggleCollectionOrange,
        ToggleCollectionYellow,
        ToggleCollectionGreen,
        ToggleCollectionBlue,
        ToggleCollectionPurple,
        ToggleCollectionGray,
        ReverseAudio,      // Reverse Clip(s) in the Arrangement, R (manual 6.9)
        LockEnvelopes,
        OpenBackup,
        ConsolidateMidi,
        LastId = ConsolidateMidi
    };
    explicit AppCommands(settings::AppSettings &);
    void reload();
    // WIN-04: each command's stable text id ("view.browser", "edit.redo"), never renumbered, the list
    // only growing; its category, the Live menu that holds it (Transport, Arrangement and Mixer for the
    // commands no menu holds: keys and controls only); the command a stable id names (0: none).
    static const char *stableId(int id) noexcept;
    static const char *category(int id) noexcept;
    static int commandFor(const std::string &stableId) noexcept;
    // A chord as canonical text, the same on every platform ("Cmd+Alt+B", "F4", "Space"): Cmd is the
    // command key (Ctrl off the Mac), Ctrl the Mac's own; then Alt, Shift, the key. And back.
    static std::string chordText(const juce::KeyPress &);
    static std::optional<juce::KeyPress> parseChord(const std::string &);
    // The keys that differ from the key map's, as shortcuts.overrides holds them; written only when
    // they differ from what it holds (false with why when the write fails).
    settings::Value userKeys() const;
    bool storeUserKeys(std::string &why);
    bool key(const juce::KeyPress &, AdiRootComponent &, bool allowComputerMidi = true);
    void activate(AdiRootComponent &);
    void detach(AdiRootComponent &);
    juce::ApplicationCommandTarget *getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID> &) override;
    void getCommandInfo(juce::CommandID, juce::ApplicationCommandInfo &) override;
    bool perform(const InvocationInfo &) override;
    // App-scope view settings every window reads (ADR-0131, ADR-0203 d12).
    bool infoView() const noexcept { return infoView_; }
    bool setInfoView(bool on, std::string &why);
    bool browserPreviewEnabled() const noexcept { return browserPreviewEnabled_; }
    bool setBrowserPreviewEnabled(bool on, std::string &why);
    bool tooltips() const noexcept { return tooltips_; }
    int tooltipDelayMs() const noexcept { return tooltipDelayMs_; }
    // Live's Use Tab Key to Move Focus (§41.1.1), off by default.
    bool tabMovesFocus() const noexcept { return tabMovesFocus_; }
    // How Follow keeps the playhead on screen: by page (Live's default), or by scrolling.
    bool followByPage() const noexcept { return followByPage_; }
    bool setTabMovesFocus(bool on, std::string &why);
    // Live's Options > Delay Compensation (manual 18.7; MX-04; `audio.delayCompensation`, on by
    // default): every track lined up with the slowest chain. Off, nothing is compensated and Track
    // Delay is unavailable, its stored values kept. An application setting, not an undo step.
    bool delayCompensation() const noexcept { return delayCompensation_; }
    bool setDelayCompensation(bool on, std::string &why);
    // Lock Envelopes inverts the app's Automation follows clips setting.
    bool envelopesLocked() const noexcept { return envelopesLocked_; }
    bool setEnvelopesLocked(bool locked, std::string &why);
    std::function<void()> onEnvelopeLockChanged;
    // Live's Wrap Tab Navigation (§41.1.1), on by default: Tab from an area's last control
    // returns to its first; off, it stops there.
    bool wrapTabNavigation() const noexcept { return wrapTab_; }
    bool setWrapTabNavigation(bool on, std::string &why);
    // Live's Move Clips with Arrow Keys (§41.1.1), on by default; off, left and right
    // collapse a selection to its start or its end.
    bool arrowKeysMoveClips() const noexcept { return arrowKeysMoveClips_; }
    // Ctrl/Cmd+wheel zooms around the selection, as Live; off, around the cursor.
    bool zoomOnSelection() const noexcept { return zoomOnSelection_; }
    // The Plug-Ins page's window settings (Live 12 manual 23.3.1.1): Multiple Plug-In Windows,
    // Auto-Hide Plug-In Windows and Plug-in windows always on top.
    bool multiplePluginWindows() const noexcept { return multiplePluginWindows_; }
    bool autoHidePluginWindows() const noexcept { return autoHidePluginWindows_; }
    // Live's Auto-Open Plug-In Windows (manual 23.3.1.1; plugins.autoOpenWindows, on by default).
    bool autoOpenPluginWindows() const noexcept { return autoOpenPluginWindows_; }
    bool pluginWindowsOnTop() const noexcept { return pluginWindowsOnTop_; }
    // Live's Exclusive Solo (Record, Warp & Launch Settings), on by default: soloing a track unsolos
    // the others unless Ctrl/Cmd is held.
    bool exclusiveSolo() const noexcept { return exclusiveSolo_; }
    // Live's Exclusive Arm (the same page), on by default: arming a track disarms the others unless
    // Ctrl/Cmd is held.
    bool exclusiveArm() const noexcept { return exclusiveArm_; }
    bool startPlaybackWithRecord() const noexcept { return startPlaybackWithRecord_; }
    enum class SelectionArm { None, Midi, All };
    SelectionArm selectionArm() const noexcept { return selectionArm_; }
    // The registry's recording defaults a new track takes (`record.quantize`,
    // `audio.keepMonitoringLatency`; N31).
    const RecordingDefaults &recordingDefaults() const noexcept { return recordingDefaults_; }
    // `editing.linkLoopToSelection` (REAPER's), off by default: the brace follows a time selection.
    bool linkLoopToSelection() const noexcept { return linkLoop_; }
    // editing.splitAllWithNoSelection: Split with no clip chosen splits every track at the marker.
    bool splitAllWithNoSelection() const noexcept { return splitAll_; }
    // The settings, read where an engine edit captures the editing policy (`edit_plan::capturePolicy`).
    const settings::AppSettings &settings() const noexcept { return settings_; }
    // For what the window writes back: the import prompt's last choices (E5).
    settings::AppSettings &writableSettings() noexcept { return settings_; }
    const juce::RecentlyOpenedFilesList &recentProjects() const noexcept { return recentProjects_; }
    void rememberProject(const juce::File &);
    void clearRecentProjects(bool onlyUnavailable);
    struct TrackCopy {
        std::string sourceFile, kind, name;
        std::int64_t parent = 0, index = 0;
        Payload contents;
    };
    // The Arrangement's clipboard holds captured material and outlives a window.
    struct ClipClipboard {
        // A Time copy (E3: Cut and Copy Time) shares it: the selected time across the whole Set.
        struct EnvelopeCopy {
            edit::AutomationRangeCopy range;
            std::string sourceFile;
        };
        std::variant<std::monostate, clip_edit::ClipCopy, clip_edit::RangeCopy, clip_edit::TimeCopy, TrackCopy, EnvelopeCopy> held;
        bool empty() const noexcept { return std::holds_alternative<std::monostate>(held); }
        // The project file a copy names; a Time copy names it by its material.
        static std::string *fileOf(clip_edit::ClipCopy &c) { return &c.sourceFile; }
        static std::string *fileOf(clip_edit::RangeCopy &c) { return &c.sourceFile; }
        static std::string *fileOf(clip_edit::TimeCopy &c) { return &c.material.sourceFile; }
        static std::string *fileOf(TrackCopy &c) { return &c.sourceFile; }
        static std::string *fileOf(EnvelopeCopy &c) { return &c.sourceFile; }
        static std::string *fileOf(std::monostate &) { return nullptr; }
        std::string sourceFile() {
            auto *file = std::visit([](auto &copy) { return fileOf(copy); }, held);
            return file != nullptr ? *file : std::string{};
        }
        // After Save As, a copy held from the project names the new file, as the device copy does.
        void rebase(const std::string &from, const std::string &to) {
            if (auto *file = std::visit([](auto &copy) { return fileOf(copy); }, held); file != nullptr && *file == from)
                *file = to;
        }
    };
    ClipClipboard &clipClipboard() noexcept { return clipClipboard_; }
    // Each window's interface zoom, kept per window and per display in `window.zoom` (N4;
    // ADR-0129 d3, ADR-0203 d11): what was kept for that window on that display, if anything;
    // and keeping a window's zoom there, saved at once.
    std::optional<float> keptZoom(const std::string &window, const std::string &display) const;
    bool keepZoom(const std::string &window, const std::string &display, float zoom, std::string &why);
    juce::ApplicationCommandManager manager;

  private:
    settings::AppSettings &settings_;
    juce::File recentProjectsFile_;
    juce::RecentlyOpenedFilesList recentProjects_;
    void saveRecentProjects();
    AdiRootComponent *active_ = nullptr;
    bool live_ = true;
    bool infoView_ = true;
    bool browserPreviewEnabled_ = false;
    bool tooltips_ = true;
    int tooltipDelayMs_ = 600;
    bool zoomOnSelection_ = true;
    bool tabMovesFocus_ = false;
    bool delayCompensation_ = true;
    bool envelopesLocked_ = false;
    bool multiplePluginWindows_ = false, autoHidePluginWindows_ = true, pluginWindowsOnTop_ = true;
    bool autoOpenPluginWindows_ = true;
    bool wrapTab_ = true, arrowKeysMoveClips_ = true;
    bool followByPage_ = true;
    bool exclusiveSolo_ = true;
    bool exclusiveArm_ = true;
    bool startPlaybackWithRecord_ = true;
    SelectionArm selectionArm_ = SelectionArm::None;
    RecordingDefaults recordingDefaults_;
    bool linkLoop_ = false;
    bool splitAll_ = true;
    ClipClipboard clipClipboard_;
};
// WIN-04: the Settings window's Shortcuts page, in place of the shortcuts.overrides row: JUCE's key
// mapping editor on the application's commands, under Live's menus, in the theme's colours. Each change
// is written to the record at once, and only when the user's keys differ from what it holds; when the
// write fails, why shows under the editor.
class ShortcutsPage final : public juce::Component, private juce::ChangeListener {
  public:
    explicit ShortcutsPage(AppCommands &);
    ~ShortcutsPage() override;
    void paint(juce::Graphics &) override;
    void resized() override;
    juce::KeyMappingEditorComponent &editor() noexcept { return editor_; }
    const std::string &error() const noexcept { return error_; }
    // Writes the user's keys now (the change message comes later, on the message loop).
    void store();
    static constexpr int kHeight = 440, kErrorHeight = 20;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster *) override;
    AppCommands &commands_;
    juce::KeyMappingEditorComponent editor_;
    std::string error_;
};
// Live's Status Bar (manual 3.2): messages along the window's foot, with the
// Info View's show/hide toggle in its bottom-left corner (manual 2.2.2).
class StatusBar final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit StatusBar(AdiRootComponent &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // JUCE passes the activating gesture's modifiers to clicked().
    class DetailButton final : public juce::TextButton {
      public:
        DetailButton(StatusBar &bar, bool clip) : bar_(bar), clip_(clip) {}
        void clicked(const juce::ModifierKeys &) override;

      private:
        StatusBar &bar_;
        bool clip_;
    };
    juce::TextButton infoToggle{""}, mixerToggle{"Mixer"};
    DetailButton detailToggle{*this, false};
    juce::TextButton audioSettings{"Audio Settings"};
    void setAudioDeviceProblem(juce::String problem);
    const juce::String &audioDeviceProblem() const noexcept { return audioDeviceProblem_; }
    juce::TextButton playbackIssues{""};
    bool setPlaybackProblems(const std::vector<std::string> &problems);
    std::function<void(const std::vector<std::string> &)> onPlaybackProblems;
    void setMissingCueOutput(std::optional<int> firstChannel);
    juce::String messageText() const;
    juce::Rectangle<int> messageField() const noexcept { return {35, 8, std::max(0, getWidth() - 419), 22}; }
    // Live 12's Clip View Selector, in the second field: the shown clip in miniature; a click shows
    // or hides the Clip View (manual 8).
    DetailButton clipToggle{*this, true};
    // A drag in the selector scrolls the Clip View's editor sideways, the whole field standing for
    // the whole clip, and zooms it up and down (manual 8.7); a press let go where it began is the
    // click that shows or hides the view. The part of the clip the editor shows is framed in it.
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void setExportProgress(std::optional<int> percent);
    const juce::String &exportProgressText() const noexcept { return exportProgressText_; }
    juce::Rectangle<float> clipViewFrame() const;
    juce::Rectangle<int> secondField() const noexcept { return {getWidth() - 376, 8, 206, 22}; }
    // Live 12's arrow beside the Mixer View Control: its options menu (manual 18.1).
    juce::TextButton mixerOptions{""};

  private:
    AdiRootComponent &root_;
    juce::String audioDeviceProblem_;
    std::vector<std::string> playbackProblems_;
    friend class AdiRootComponent;
    juce::String playbackIssueLabel_;
    std::optional<int> missingCueOutput_;
    juce::String cueOutputProblem_;
    juce::Point<int> selectorLast_;
    bool selectorDragged_ = false;
    std::optional<int> exportPercent_;
    juce::String exportProgressText_;
};
// The browser's inner edge follows it from one side of the window to the other.
class BrowserResizer final : public juce::Component {
  public:
    explicit BrowserResizer(AdiRootComponent &);
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

  private:
    AdiRootComponent &root_;
    struct Drag {
        int visibleWidth, storedWidth, direction;
    };
    std::optional<Drag> drag_;
};

// The window is JUCE's drag container, so a browser row can be dragged onto the lanes (D1).
class AdiRootComponent final : public juce::Component,
                               public juce::DragAndDropContainer,
                               private juce::FocusChangeListener {
  public:
    AdiRootComponent(engine::ProjectView &, OpSubmitter &, TransportMailbox &, AppCommands &,
                     ViewStateStore &, SessionState &, std::string window,
                     DesktopDefaults defaults = {});
    ~AdiRootComponent() override;
    void frame() noexcept;
    void paint(juce::Graphics &) override;
    void paintOverChildren(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    // The ★ Reference's key comes up: a hold ends (ADR-0209 d9). A latching key comes up: a hold
    // of 500 ms or more goes back (Live 42.25).
    bool keyStateChanged(bool isKeyDown) override;
    bool command(int);
    // Live's Insert Return Track (§18.4; Ctrl/Cmd+Alt+T): a return after the others, named by its
    // letter, selected; the returns shown again if they were hidden. With `group`, Insert Return Track
    // in Group (ADR-0247 d2): inside it, after what it holds, lettered with its initial ("DA").
    bool insertReturnTrack(std::int64_t group = 0);
    void attachTrackRendering(ProjectDocument &document) noexcept { trackRenderDocument_ = &document; }
    void addTrackRenderItems(juce::PopupMenu &, std::int64_t track);
    void mark(unsigned bits) noexcept { dirty_.mark(bits); }
    const SnapshotReader &reader() const noexcept { return *reader_; }
    bool playing() const noexcept { return playing_; }
    std::int64_t renderedSamplePosition() const noexcept { return mailbox_.position(); }
    [[nodiscard]] auto settledLocateRevision() const noexcept { return mailbox_.settledLocateRevision(); }
    // The Control Bar's MIDI Indicators (Live 12 manual 2.x, the Control Bar's right end): lit while MIDI
    // has come in from an open port within the last moment (SRF-05). The application sets it.
    void setMidiIn(bool on) noexcept {
        if (on != midiIn_) {
            midiIn_ = on;
            mark(DirtySet::Transport); // the Control Bar redraws while the transport is stopped too
        }
    }
    bool midiIn() const noexcept { return midiIn_; }
    void setMidiOut(bool on) noexcept {
        if (on != midiOut_) {
            midiOut_ = on;
            mark(DirtySet::Transport);
        }
    }
    bool midiOut() const noexcept { return midiOut_; }
    bool computerMidiEnabled() const noexcept { return computerMidiEnabled_; }
    bool playComputerMidiKey(const juce::KeyPress &);
    juce::KeyPress computerMidiShortcut(const juce::KeyPress &) const;
    bool pressComputerMidiToggle(int key);
    void releaseComputerMidiKeys();
    std::function<void(const engine::Event &)> sendComputerMidi;
    std::function<bool(std::int64_t, int, int, bool)> previewMidiNote;
    std::function<void()> stopMidiPreview;
    std::function<std::array<std::uint64_t, 2>(std::int64_t)> midiKeysSounding;
    double bpm() const noexcept;
    std::uint64_t generation() const noexcept { return generation_; }
    std::uint64_t projectViewGeneration() const noexcept { return view_.generation(); }
    std::uint64_t committedGeneration() const noexcept { return view_.committedGeneration(); }
    unsigned lastDrain() const noexcept { return lastDrain_; }
    const std::string &error() const noexcept { return error_; }
    WindowState &state() noexcept { return state_; }
    bool persist();
    ViewStateStore &viewStateStore() noexcept { return persistence_; }
    // A message for the Status Bar from outside the root (the application's commands).
    void say(std::string message);
    std::function<void(const std::string &)> onStatusMessage;
    void sayStaleAddMenu();
    // What the Status Bar says when there is no message: Live's line for the insert
    // marker or the time selection.
    juce::String selectionText() const;
    // Undo can act with a knob edit still held (DF-W29): it commits it first, then takes it back, so
    // a project's first edit is undoable at once. Redo cannot: that commit clears what Redo would do.
    bool recordingHoldsHistory() const;
    bool canUndo(bool redo) const {
        if (recordingHoldsHistory())
            return false;
        const bool held = editsHeld && editsHeld();
        return redo ? submitter_.canRedo() && !held : submitter_.canUndo() || held;
    }
    // What Live's Edit menu shows: "Undo" and the step's own label, or "Nothing to Undo" with no
    // step (Live 12's menu, the director's LK-02 capture; Readings M1-16a); the same for Redo.
    std::string undoTitle(bool redo) const;
    // The application's commands (New, Open, Take Snapshot, Save As, Settings) and
    // the engine refresh after an edit belong to whoever owns the document.
    std::function<bool(int)> applicationCommand;
    std::function<void()> openAudioSettings;
    bool needsFirstSave = false;
    std::function<void(const juce::File &)> openProjectFile;
    std::function<void()> afterEdit;
    // The app stops audio and reloads the document for cross-checkpoint navigation.
    std::function<bool(HistoryRestoreTarget)> restoreStoredHistory;
    std::function<std::int64_t()> retainedDeviceId;
    std::function<bool(std::string &)> beforeDeleteTrack;
    // Commits the parameter edits the document still holds before Undo, Redo or a restored step acts
    // (DF-W29); false, with the document's reason said, when it could not.
    std::function<bool()> settleEdits;
    // Whether the document holds parameter edits Undo would commit first (DF-W29).
    std::function<bool()> editsHeld;
    // Opens the application's chooser and supplies the chosen file on the message thread.
    using ChosenAudioFile = std::function<void(const juce::File &)>;
    std::function<bool(ChosenAudioFile)> chooseAudioFile;
    // Reads a dropped file and captures its media row, writing nothing (the document's
    // `prepareImport`, #308); false with `why` when it can't. Unset, a drop says it needs one.
    std::function<bool(const std::string &utf8Path, AudioImport &, std::string &why)> importAudio;
    // The explicit sample-defaults write belongs to the active Library profile.
    std::function<bool(std::int64_t clip, std::string &why)> saveSampleDefaults;
    // The drop with Warp, edge fades and copy is prepared off the message thread and committed as
    // one undo step. `done` gives the first clip and track, or 0 and the reason for refusal.
    using MaterialDone = std::function<void(std::int64_t clip, std::int64_t track, const std::string &why)>;
    std::function<bool(const std::vector<std::pair<std::string, std::string>> &files,
                       std::int64_t track, std::int64_t pos, bool separate, const ImportChoices &,
                       MaterialDone done)>
        importMaterial;
    // ADI's import prompt (E5): open while a drop waits for its choices; none otherwise.
    class ImportPrompt;
    ImportPrompt *importPrompt() noexcept { return importPrompt_.get(); }
    // Whether a drop with choices is still being prepared.
    bool importing() const noexcept { return importing_.has_value(); }
    // Live's Automation Arm and Re-Enable Automation (manual 25.1, 25.4; MR 6.1; N26): the
    // session's state, which the document owns, so the application sets these three. Re-Enable's
    // lit state is the snapshot's (`SnapshotReader::automationOverridden`); a click runs
    // `reenableAutomation`, which says why in the Status Bar when it refuses. Automation Arm reads
    // `automationArmed` each frame (unset: on, as Live's new Set) and a click sets `armAutomation`.
    // Unset, a click says the switch has no session.
    // Live's Export Audio/Video (EX-11): the application shows the Export page with these choices, the
    // render range the selection, else the loop, else the whole song. Unset outside the application.
    std::function<void(const ExportChoices &)> exportAudio;
    // Ask for bars, beats and sixteenths when Insert Silence starts at a point.
    using InsertSilenceReply = std::function<void(std::optional<std::string>)>;
    std::function<void(InsertSilenceReply)> promptInsertSilence;
    // Opens a Help target (WIN-09): the manual's address, or the keyboard reference's file. Empty, the
    // system's browser opens it; a test puts its own here.
    std::function<bool(const juce::URL &)> openHelp;
    // The manual the Help menu opens: Live 12's, which ADI's behaviour is written against (ADR-0203
    // d1), until ADI has its own (the director's question in the window log).
    static constexpr const char *kManualAddress = "https://www.ableton.com/en/live-manual/12/";
    // The keyboard reference: every command with the keys it has now, as a page; its file.
    juce::File writeKeyboardReference() const;
    // The Export page's starting choices, from the frame's snapshot and selection.
    ExportChoices exportDefaults() const;
    // A mixer drag heard while it moves (MX-23, SRF-02): the document's live gesture. The strip begins it
    // on the control's drag start with its kind and its track or send, and the selected tracks go with
    // it when the dragged one is among them; each value goes to `mixerDrag`; the release commits one undo
    // step and Esc cancels. Unset outside the application: the strip commits on release, as before.
    std::function<bool(const gesture::RelativeScalarSelection &, std::optional<double>, const std::string &)> beginMixerDrag;
    std::function<bool(double)> mixerDrag;
    std::function<bool()> endMixerDrag;
    std::function<void()> cancelMixerDrag;
    std::function<bool()> reenableAutomation;
    // The session follows Options > Delay Compensation (MX-04): set by the application, called when
    // the command toggles the setting. Unset, the setting changes and the session hears it at the next
    // project open.
    std::function<bool(bool)> applyDelayCompensation; // false: refused, and the reason said
    // The current audio device's granted physical output channels, read on the message thread.
    // Empty when no device is running, and Cue Out offers the default 1/2.
    std::function<std::optional<engine::OutputChannelMap>()> physicalOutputChannels;
    // Physical stereo pairs actually available in the audio device, with their configured names.
    std::function<std::span<const std::pair<int, std::string>>()> externalAudioOutputs;
    std::function<void()> configureMidiRouting;
    std::function<std::vector<MidiPortListing>()> midiTrackOutputs;
    std::vector<MidiPortListing> midiInputPorts;
    std::function<bool(const rows::Model &, std::string &)> validateAudioRouting;
    std::function<std::span<const ExternalAudioInput>()> externalAudioInputs;
    std::shared_ptr<ExternalInputMeter> externalInputMeter;
    std::function<void()> configureExternalInputs;
    std::span<const ExternalAudioInput> audioInputs() const {
        return externalAudioInputs ? externalAudioInputs() : std::span<const ExternalAudioInput>{};
    }
    // Live's per-parameter Re-Enable Automation (manual 25.4; AU-04): one overridden lane follows its
    // envelope again; false with the reason said. Session state, no undo step, as the global switch.
    std::function<bool(std::int64_t)> reenableLane;
    // The item in a control's context menu while the control has a lane (`lane`, null for none):
    // available while that lane is overridden, as Live's.
    void addReenableAutomation(juce::PopupMenu &menu, const engine::AutomationLaneNode *lane);
    void addAutomationDeleteItem(juce::PopupMenu &, const AutomationTarget &);
    void addTrackAutomationDeleteItem(juce::PopupMenu &, std::int64_t track);
    void addDeviceAutomationDeleteItem(juce::PopupMenu &, std::int64_t device);
    std::function<bool()> automationArmed;
    std::function<void(bool)> armAutomation;
    // Arm, monitoring and Arrangement Record (MX-09). The session's rows hold a track's arm and its
    // monitoring, which the snapshot does not carry yet, so the application hands them over: once a
    // frame through `setRecordStates` (kept only when they changed, so a steady frame allocates
    // nothing), and fresh for an action through `readRecordStates`. `startRecording(insertTick,
    // playing)` prepares the recorder and, stopped, starts the transport as a recording start;
    // `stopRecording` stops the recorder (the transport goes on, as Live's Record button);
    // `recording` reads whether it runs. A refusal comes back as false and the application has said
    // why. Unset outside the application.
    std::function<RecordStates()> readRecordStates;
    std::function<bool(std::int64_t)> setSelectionArmedTrack;
    std::function<bool(std::int64_t, std::int64_t)> setTakeLaneAudition;
    void refreshSelectionArming();
    void setRecordStates(RecordStates states) {
        if (states != recordStates_)
            recordStates_ = std::move(states);
    }
    std::function<bool(std::int64_t insertTick, bool playing, bool overdub)> startRecording;
    std::function<void()> stopRecording;
    std::function<bool()> recording, capturingAudioTake;
    std::function<bool()> landingRecording;
    std::function<std::span<const LiveMidiTake>()> liveMidiTakes;
    std::function<void(std::int64_t positionTick, bool playing)> captureRecentMidi;
    // Live's MIDI Arrangement Overdub (manual 21.2): the Control Bar's switch, the window's own state.
    // It decides what a MIDI recording does to the notes already there once MIDI records (SRF-05).
    bool midiOverdub() const noexcept { return midiOverdub_; }
    void setMidiOverdub(bool on) noexcept { midiOverdub_ = on; }
    // The arm and monitoring of a track as the application last handed them (Off and unarmed when
    // not known).
    TrackRecordState recordState(std::int64_t track) const;
    bool isRecording() const { return recording && recording(); }
    bool arrangementRecordEnabled() const { return recordPending_ || isRecording(); }
    bool clipMediaOffline(const engine::ClipNode &clip) const {
        return clip.audio && (!clip.audio->mediaInfo || clip.audio->mediaInfo->missing ||
                              (unavailableClipMedia && unavailableClipMedia(clip.id)));
    }
    std::function<bool(std::int64_t)> unavailableClipMedia;
    std::function<bool(std::int64_t)> clipPlaybackUnavailable;
    std::function<void(std::int64_t)> locateClipMedia;
    // Live's file drop (manual 6.1): an audio file onto the audio track `track` at `pos`, or onto
    // the drop area below the tracks (`track` 0), which makes a new audio track. One undo step;
    // the new clip and its track are selected. A refusal goes to the Status Bar.
    bool dropAudio(const std::string &utf8Path, std::int64_t track, std::int64_t pos, const std::string &name);
    bool dropAudioPaths(const juce::StringArray &, std::int64_t track, std::int64_t pos, bool separate = false);
    bool dropMidiFiles(const std::vector<std::pair<std::string, std::string>> &files,
                       std::int64_t track, std::int64_t position, bool separate);
    std::int64_t midiClipForExport() const noexcept;
    // Several files dropped at once, each a (path, clip name): one after another on that track, or
    // with `separate` (Ctrl/Cmd held) the first there and each other on a new audio track of its own
    // (`adi::ui::dropAudio`). Each file is read and captured first; one undo step for them all.
    // The drop once the choices are known: the plain import when all are off, else the queue.
    bool dropWithChoices(const std::vector<std::pair<std::string, std::string>> &files, std::int64_t track,
                         std::int64_t pos, bool separate, const ImportChoices &);
    bool dropAudioFiles(const std::vector<std::pair<std::string, std::string>> &files, std::int64_t track,
                        std::int64_t pos, bool separate);
    bool sampleInstrumentTarget(std::int64_t track) const;
    bool loadSampleAsInstrument(std::int64_t track, const std::filesystem::path &);
    // The window's own interface zoom (ADR-0203 d6, A5-01): each window keeps its
    // own, and the ZoomHost around the root applies it as a transform.
    float interfaceZoom() const noexcept { return interfaceZoom_; }
    // On opening, and after a move onto another display: the zoom kept for this window on the
    // display it now stands on, if one was kept (N4). Ctrl/Cmd + and - keep it there.
    void followDisplay();
    // Whether this view's own window fills the screen (Live's Full Screen, §42.1).
    bool fullScreen() const;
    // What the Info View shows: the element under the mouse, read in the frame.
    const juce::String &infoText() const noexcept { return infoText_; }
    void hover(juce::Component *);
    // Laid-out rectangles, in the root's logical units, for tests and children.
    juce::Rectangle<int> deviceArea() const noexcept { return deviceArea_; }
    juce::Rectangle<int> clipArea() const noexcept { return clipArea_; }
    juce::Rectangle<int> infoArea() const noexcept { return infoArea_; }
    juce::Rectangle<int> browserArea() const noexcept { return browserArea_; }
    AppCommands &commands() noexcept { return commands_; }
    // The project as it is now: what an action lands on, never this window's last frame.
    std::shared_ptr<const engine::Snapshot> current() const { return view_.current(); }
    // The playhead in ticks, from the driver's feedback at the device's rate.
    std::int64_t timelineTick() const noexcept;
    // Asks the driver to move the playhead; reads the current cell on arrival.
    bool locate(std::int64_t tick, bool fromInsertMarker = false);
    // Plays from a point: starts the transport there, or jumps there while it plays.
    bool playFrom(std::int64_t tick);
    bool playOrStopFrom(std::int64_t tick);
    TransportBar transport;
    StatusBar status;
    BrowserPanel browser{*this};
    std::function<void(std::filesystem::path)> selectSamplePreview;
    std::function<void(std::filesystem::path, double fraction)> playSamplePreview;
    std::function<void()> stopSamplePreview;
    BrowserResizer browserResizer{*this};
    void resizeBrowser(int width);
    // The bar over the track names and their controls (the Arrangement sheet; DF-W25): made before the
    // arrangement, which keeps its Show in step with the view chosen.
    NamesBar namesBar;
    ArrangementView arrangement;
    // Live's Undo History (manual 5.4.2; A5): View › Undo History and Ctrl/Cmd+Alt+Z. Its own window
    // in the app (ui_main.cpp); not a child of this one.
    UndoHistoryPanel undoHistory;
    // Puts the project back to just after the history's step `txnId` (`OpSubmitter::restore`): one
    // refresh, Follow paused as by an undo, the session re-reading the project after it.
    bool restoreStep(std::int64_t txnId);
    // A plug-in's own window (Live 12 manual 23.3; N8, #391). The wrench in a plug-in device's
    // title bar opens it in a floating window at the plug-in's size, never scaled; a second click
    // closes it. Opening one closes the others unless Multiple Plug-In Windows is on or Ctrl/Cmd is
    // held. The app wires the opener (`createEditorComponent`) and the host; `frame` runs on the
    // window's clock and returns the editor's error, which goes to the Status Bar.
    struct PluginEditorView {
        std::unique_ptr<juce::Component> view;
        std::function<std::string()> frame;
        // Whether the plug-in has dropped its editor: its window then closes.
        std::function<bool()> gone;
        // Whether the plug-in says its editor can be resized: its window then is, from its edges and
        // corner, and the plug-in is asked for each size (SC-328).
        bool resizable = false;
    };
    std::function<PluginEditorView(std::int64_t device, std::string &error)> pluginEditor;
    FloatingHost *pluginHost = nullptr;
    bool openPluginWindow(std::int64_t device);
    bool togglePluginWindow(std::int64_t device, bool several);
    bool pluginWindowOpen(std::int64_t device) const;
    static std::string pluginWindowKey(std::int64_t device);
    // Each frame: Auto-Hide and Show/Hide applied, Always on Top followed.
    void followPluginWindows();
    // The window's vblank clock runs the frame from inside JUCE's VBlankThread::handleAsyncUpdate, which
    // walks its listener list by reference; a window opened there (Auto-Open) adds its peer's listener
    // to that list and crashed ADI (walk-A A28). So a vblank frame leaves the plug-in windows to a
    // message of their own, right after it; `frame()` called directly follows them at once.
    void vblankFrame() noexcept;
    void settlePluginWindows() { windowFollower_.handleUpdateNowIfNeeded(); }
    // WIN-03, Live's Auto-Open Plug-In Windows: a plug-in just added opens its window once the device
    // feed shows it (within a second of frames), when the setting is on. The app calls it after an
    // insert lands.
    void pluginAdded(std::int64_t device);
    // Live's Show/Hide Plug-In Windows (Ctrl/Cmd+Alt+P): every plug-in window at once.
    bool pluginWindowsShown() const noexcept { return pluginWindowsShown_; }
    // A built-in device loaded from the browser (D4): the app wires the load (`insertBuiltin`, one
    // undo step); `insertDevice` checks there is a track and says why it cannot.
    std::function<bool(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before)> loadBuiltin;
    std::function<bool(std::int64_t track, const std::filesystem::path &, std::string &error)> loadSampleInstrument;
    // A VST3 or CLAP plug-in from the Browser onto `track`, in front of `before` or where Live places its
    // kind (N57, `ProjectDocument::insertPlugin`): one undo step, and the Status Bar says how it went in.
    // `kindGuessed`: the reference's subtype is the Browser's guess (a VST3 before N58), which a kind the
    // project already holds for the plug-in overrides. Unset outside the application.
    std::function<bool(rows::PluginRef, std::int64_t track, std::optional<std::int64_t> before, bool kindGuessed)> loadPlugin;
    bool insertDevice(const std::string &uid, std::int64_t track, std::optional<std::int64_t> before);
    DeviceChainStrip devices;
    DockResizer dockResizer;
    // Live's Clip View, above the Device View in the detail row (STEP-7-PLAN 7.7).
    ClipViewPanel clipView{*this};
    ClipViewResizer clipResizer{*this};
    static constexpr int kClipViewFloor = 150;
    MixerPanel mixer;
    MixerPanel mixerView{*this, true};
    // The Mix View (ADR-0209): docked in place of the arrangement and its track controls.
    MixViewPanel mixViewPanel{*this};
    SharedSpectrumView infoSpectrum{true};
    std::function<void(const juce::PopupMenu &)> onInfoMenu;
    std::shared_ptr<SharedAnalysis> sharedAnalysis;
    std::int64_t analysisTrack(bool selected = false) const noexcept;
    std::int64_t analysisTrack(const SnapshotReader &, bool selected = false) const noexcept;
    // Application wires the project-owned following analyzer window. Analysis never adds a
    // device to a chain or changes an enabled processor's sound.
    std::function<bool(std::int64_t)> openAnalyzer;
    juce::Rectangle<int> mixViewArea() const noexcept { return mixViewArea_; }
    // Where the Mix View stands docked, open or not: the arrangement's place and its track controls'.
    // Its first window opens over it, on the screen (detachMixView).
    juce::Rectangle<int> mixViewDock() const noexcept { return mixViewDock_; }
    // The Mix View's tab, this window's view state; false only on a write error.
    bool chooseMixViewTab(int tab);
    // Opens or closes the Mix View and says where it goes; the application moves the panel into
    // its floating window or back (ui_floating.hpp, detachMixView). False only on a write error.
    bool setMixView(bool open, bool detached);
    // Reference/audition metadata supplied to the Mix View and arrangement.
    // Device rows and native functions come from their real authored chains.
    const MixViewProvider &mixProvider() const noexcept { return *mixProvider_; }
    void setMixProvider(const MixViewProvider &);
    // Live's momentary latching (§42.25) for S, F1…F8, Z and B: the key goes down and its command runs,
    // once, however long it is held; it comes up after 500 ms or more and the window goes back: the
    // solo or the activator's step is undone while it is still the latest, and zoom or Draw Mode goes back.
    static bool latches(int command) noexcept;
    bool latchKey(int command, int keyCode, bool down, std::int64_t nowMs);
    const MomentaryLatch &latch() const noexcept { return latch_; }
    void letGoUnseen() noexcept { latch_.letGo(); }
    // Where the window reads the time and the keys held (ADR-0231 d2; DF-W18): the system's unless a
    // test gives its own.
    InputClock input;
    // The ★ Reference track, or 0 when no track is it.
    std::int64_t referenceTrack() const noexcept;
    void addReferenceCommand(juce::PopupMenu &, int command);
    static constexpr auto kNoReferenceTrack = "No Reference track is assigned.";
    static constexpr auto kReferencePlaybackUnavailable = "Reference playback needs an audio engine connection in this window.";
    MixerViewResizer mixerViewResizer{*this};
    // Live's strip down the arrangement's outer edge: the view switches and the track height.
    SideStrip strip;
    juce::Rectangle<int> stripArea() const noexcept { return stripArea_; }
    juce::Rectangle<int> mixerViewArea() const noexcept { return mixerViewArea_; }
    // Drag the Mixer View's top edge: taller strips, never past one track row.
    void resizeMixerView(int height);
    // Live 12's Mixer View Control options (manual 18.1, "show or hide mixer components"): In/Out,
    // Sends, Volume, Track Options, Crossfader, Performance Impact, then Return Tracks. The View
    // menu's Mixer Controls submenu is the same list with Live's separator after Volume (manual 6.1).
    juce::PopupMenu mixerOptionsMenu(bool viewMenu = false);
    // Live 12's Arrangement Track Controls submenu of the View menu: ADI's Hide/Show Track Controls
    // (the side panel), then the track controls' In/Out and Sends (Ctrl/Cmd+Alt+I, Ctrl/Cmd+Alt+S),
    // Volume and Track Options. The View menu's Mixer Controls submenu is mixerOptionsMenu().
    juce::PopupMenu trackControlsMenu();
    juce::Rectangle<int> mixerArea() const noexcept { return mixerArea_; }
    // One edit from a control, as one named undo step; a refusal goes to the Status Bar.
    bool submitEdit(const std::string &op, Payload payload, const char *label);
    // Several ops as one undo step (a batch lands whole or not at all); a refusal goes to the
    // Status Bar.
    bool submitEdits(std::vector<OpRequest> ops);
    // An engine edit prepared against the store (`clip_edit::prepareSplit`, `prepareDeleteRange`;
    // N17, #393) committed as one batch, with at most one op of the window's on its end: one undo
    // step. The engine's refusal goes to the Status Bar.
    bool commitEdit(clip_edit::PreparedEdit prepared, std::optional<OpRequest> appended = std::nullopt);
    // Live's Split (Ctrl/Cmd+E, the Edit menu, a clip's menu; manual 6.9, 41.5): a time selection
    // cuts the clips on its tracks at its edges (inside one clip, at both); else the chosen clip at
    // the insert marker; else the clips under the marker on the selected tracks, or on every track
    // when editing.splitAllWithNoSelection is on. Each clip is one undo step.
    bool splitAtMarker();
    bool canReverseAudio() const;
    bool reverseAudio();
    // Live's Create Fade In/Out and Create Crossfade (manual 6.8; Ctrl/Cmd+Alt+F, the Create menu):
    // over the time selection's audio clip starts, ends and boundaries, one undo step. And Delete
    // Fades/Crossfades (Ctrl/Cmd+Alt+Backspace) on the chosen clips, or the audio clips the time
    // selection holds. E4.
    bool createFades();
    bool deleteFades();
    // Whether the selection crosses two clips' boundary: Create Fade reads Create Crossfade.
    bool selectionCrossfades() const;
    // Live's Cut, Copy, Paste and Duplicate in the Arrangement (manual 6.9, 41.5; E2): on the chosen
    // clip or the time selection; Paste at the insert marker, a range onto as many shown tracks as
    // it came from, in order, from the selected one. Each is one undo step; the engine's refusals go
    // to the Status Bar. `canClipClipboard` says whether there is something to act on.
    enum class ClipCommand { Cut, Copy, Paste, Duplicate };
    bool clipClipboard(ClipCommand);
    bool trackClipboard(ClipCommand);
    bool trackClipboardContext() const;
    bool trackGroupingContext() const;
    bool deleteAutomationRange();
    bool canClipClipboard(ClipCommand) const;
    bool canConsolidateMidi() const;
    // Whether Cut, Copy, Paste or Duplicate (an AppCommands id) has something to act on, on a device
    // or in the Arrangement; and whether the device chain takes it first.
    bool canClipboard(int commandId) const;
    bool deviceClipboardFirst(DeviceChainStrip::Clip) const;
    bool automationClipboard(ClipCommand);
    // Where Paste starts: the top selected track the lanes show that can hold clips; 0 if none.
    std::int64_t pasteOrigin() const;
    // Live's …Time commands and Insert Silence (manual 6.11, 41.5; E3): across every track, from the
    // time selection (Paste Time at the insert marker). Locators, time signature markers, the loop
    // and global automation move as the engine moves them. One undo step each. `canTimeCommand`
    // says whether there is something to act on.
    bool timeCommand(int commandId);
    bool canTimeCommand(int commandId) const;
    bool insertSilence(std::int64_t at, std::int64_t length);
    // The rate an edit's seconds and ticks convert at: the device's, else the project's.
    double editRate() const noexcept;
    // The store an engine edit is prepared against, and the settings its policy is captured from.
    Store &editStore() noexcept { return submitter_.store(); }
    bool writeControllerPoint(const controllers::Address &, const controllers::Point &,
                              const std::optional<std::int64_t> &expectedHead);
    bool deleteControllerPoints(const controllers::Lane &, std::span<const std::uint64_t>);
    const settings::AppSettings &commandSettings() const noexcept { return commands_.settings(); }
    // Launch the configured executable or application bundle with a verified source from the pool.
    std::function<bool(const std::filesystem::path &editor, std::int64_t mediaId, bool copy)> launchSampleEditor;
    // Live's Solo on track `id` (a switch, or S on the selection), with Exclusive Solo, Ctrl/Cmd for
    // several (`add`), and the selected tracks together; one undo step.
    bool soloTrack(std::int64_t id, bool add);
    // Live's Arm on track `id` (MX-09; manual 21.1), with Exclusive Arm, Ctrl/Cmd for several (`add`),
    // and the selected tracks together; and its monitoring switch In, Auto, Off (manual 21.4). Each
    // one batch.
    bool armTrack(std::int64_t id, bool add);
    // An empty mode restores each affected track's own default.
    bool setMonitor(std::int64_t id, std::optional<Monitor> mode);
    std::vector<std::int64_t> audioOutputChoices(std::int64_t track) const;
    bool setTrackAudioOutput(std::int64_t track, std::int64_t destination);
    bool setTrackMidiOutput(std::int64_t track, std::optional<rows::MidiOutput>);
    bool setAudioInput(std::int64_t id, std::string_view ref);
    bool setMidiInput(std::int64_t id, std::string_view port, int channel);
    // Arrangement Record (F9, the Control Bar's Record): records every armed audio track; again,
    // stops recording and the transport plays on.
    bool toggleRecord(bool alternate = false);
    // Selects clip `clip` as a click on it would (a recorded take, once it lands).
    void selectClip(std::int64_t clip);
    // Clips moved, each to a track and a position (DF-W28, ED-03; Live 12 manual 6.7): one placement by the
    // engine's planner (`edit_plan::prepareMove`), which clears what each clip lands on in its track as
    // Live does (the covered part goes, a clip crossing the edge is trimmed or split) under the user's
    // edit policy, one undo step named `label`. A refusal is said.
    struct ClipPlace {
        std::int64_t clip = 0, track = 0, position = 0;
    };
    bool placeClips(const std::vector<ClipPlace> &, const char *label);
    // Copies of clips, each to a track and a position (a Ctrl+drag; Option on the Mac): the originals
    // stay, each copy clears what it lands on (`clip_edit::prepareDuplicates`), one undo step named
    // `label`. The copies' ids, in the order given; none when refused, which is said.
    std::vector<std::int64_t> copyClips(const std::vector<ClipPlace> &, const char *label);
    // Live's Automation Mode (AU-01; manual 25.2): each track's Volume envelope drawn on its lane, and its
    // breakpoints edited there, each edit one undo step. Off in a new window, as Live's.
    bool automationMode() const noexcept { return automationMode_; }
    bool drawMode() const noexcept { return clipView.notes.drawMode(); }
    void setAutomationMode(bool on);
    bool addAutomationPoint(std::int64_t track, std::int64_t tick, double value, bool exactValue = false);
    bool addAutomationPoint(std::int64_t track, const AutomationTarget &, std::int64_t tick, double value,
                            bool exactValue = false);
    const std::vector<AdditionalAutomationLane> &additionalAutomationLanes(std::int64_t track) const;
    bool automationLanesExpanded(std::int64_t track) const;
    bool automationOverlayShown(std::int64_t track) const;
    bool addAutomationLane(std::int64_t track, const AutomationTarget &);
    bool addAutomatedLanes(std::int64_t track);
    bool removeAutomationLane(std::int64_t track, const AutomationTarget &);
    void toggleAutomationLanes(std::int64_t track);
    void hideAutomationOverlay(std::int64_t track);
    bool automationTargetShown(std::int64_t track, const AutomationTarget &) const;
    // Live's lane chooser (DF-W37; manual 25.2): what a track's lane shows in Automation Mode, its
    // Volume until another is chosen; a choice whose device or send is gone reads as Volume again.
    // Window state, kept while the window lives.
    AutomationTarget automationTarget(std::int64_t track) const;
    std::vector<AutomationTarget> automationChoices(std::int64_t track) const;
    // The chooser reads its track's declarations on demand, independently
    // of Device View's subscriptions for live control values.
    std::function<std::shared_ptr<const ParameterPublication>(std::int64_t track)> automationParameters;
    // Shows `target` on its track's lane and turns Automation Mode on (Live's Show Automation).
    void showAutomation(std::int64_t track, const AutomationTarget &target);
    // A device parameter's Show Automation, from its control's menu: false when it has no lane to show.
    bool showDeviceAutomation(std::int64_t device, const std::string &parameter);
    // The track and target a device parameter's Show Automation would show; none for one the engine
    // would not play.
    std::optional<std::pair<std::int64_t, AutomationTarget>> deviceAutomationTarget(std::int64_t device,
                                                                                   const std::string &parameter) const;
    // The target's value now, where its control stands: the line drawn while its lane has no point.
    double automationCurrent(const AutomationTarget &target) const;
    // For a typed value, tick is the point's expected, unchanged native time.
    bool moveAutomationPoint(std::int64_t track, std::uint64_t point, std::int64_t tick, double value, bool exactValue = false);
    bool moveAutomationPoint(std::int64_t track, const AutomationTarget &, std::uint64_t point, std::int64_t tick,
                             double value, bool exactValue = false);
    bool deleteAutomationPoint(std::int64_t track, std::uint64_t point);
    bool deleteAutomationPoint(std::int64_t track, const AutomationTarget &, std::uint64_t point);
    bool submitAutomationMove(std::vector<OpRequest> requests, std::span<const TrackEnvelope> envelopes);
    bool replaceAutomationRange(std::int64_t lane, std::int64_t begin, std::int64_t end,
                                std::span<const automation::NativeSpliceKnot>, const char *label);
    bool simplifyAutomationRange(std::int64_t lane, std::int64_t begin, std::int64_t end, double tolerance);
    bool transformAutomationRange(std::int64_t lane, const edit::AutomationTransform &, const char *label);
    bool drawAutomationStroke(std::int64_t track, const AutomationTarget &target,
                              std::int64_t begin, std::int64_t end,
                              const std::map<std::int64_t, double> &points, bool stepped,
                              std::uint64_t generation);
    bool canDeleteShownAutomation() const;
    // Live's Solo in Place (manual 18.6): soloing leaves the returns audible. The project's one
    // field, which the Solo button's menu and the Options menu's entry share (`mixer.setSoloInPlace`,
    // #412); outside undo, saved with the project; the session re-reads the project after it.
    bool setSoloInPlace(bool on);
    // Live's cueing (manual 18.6; MX-08), each one transport op outside undo, saved with the project:
    // Solo Switches or Cue Switches; a track's Cue button; Main's Cue Volume; Cue Out's stereo pair.
    bool setCueMode(bool cue);
    bool cueTrack(std::int64_t track);
    bool setCueVolume(double db);
    bool setMainOutput(int firstChannel);
    bool setCueOutput(int firstChannel);
    bool submitCue(TrackEdit edit, const char *needs);
    // A track's Rec-Q or Play-Q ("off" or a grid) and its Keep Monitoring Latency in Recorded Audio
    // (#397, N31; MR V.2 §9; Live 12 manual 18.8), each one undo step. Nothing follows them yet.
    bool setQuantize(std::int64_t track, bool record, const std::string &choice);
    bool setKeepMonitoringLatency(std::int64_t track, bool on);
    // The toggle both entries pick: the field's opposite as the CURRENT cell holds it at pick time,
    // so a menu left open while the other entry threw the switch still toggles, never a no-op.
    bool toggleSoloInPlace();
    // Live's Track Activator on track `id`: with `withSelection`, the selected tracks together when
    // `id` is among them (a switch, or 0 on the selection); without, that track alone (F1…F8).
    bool activateTrack(std::int64_t id, bool withSelection = true);
    // Live's time signature markers (manual 6.5), from the CURRENT cell (ADR-0201), each one undo
    // step: Insert Time Signature Change at `pos` with the value typed, Edit Time Signature on the
    // marker there, Delete. The marker inserted or edited is selected; a refusal is said.
    bool insertMeter(std::int64_t pos, const std::string &text);
    bool setTempoChange(std::int64_t pos, const std::string &text);
    bool editMeter(std::int64_t pos, const std::string &text);
    bool deleteMeter(std::int64_t pos);
    // The marker at `from` moved to `to`, its value kept, one step; it stays selected.
    bool moveMeter(std::int64_t from, std::int64_t to);
    // The control bar's Time Signature field (manual 6.5): the signature in force at the play
    // location takes the value typed, one step.
    bool setMeterAtPlay(const std::string &text);
    // The control bar's Tempo field (manual 9.1.1): the tempo in force at the play location takes the
    // value typed, 20 to 999 BPM, one step.
    bool setTempoAtPlay(const std::string &text, const char *label = "Change Tempo");
    // Live's Tap Tempo (manual 9.1.2): a tap at a time in milliseconds; from a run's second tap the
    // tempo follows the taps. A run is one undo step ("Tap Tempo"): each tap replaces the run's step
    // while it is still the latest.
    bool tapTempo(std::int64_t nowMs);
    // Live's Arrangement Position fields (manual 6.1): "Adjusting the Arrangement Position fields
    // automatically moves the insert marker". A position typed, bar.beat.sixteenth in the project's
    // meters, puts the insert marker and the playhead there; up and down step a bar from the play
    // position (the insert marker while stopped). Neither is an edit, so neither is an undo step.
    bool setPositionAt(const std::string &text);
    const TapTempo &taps() const noexcept { return tap_; }
    // Live's metronome (manual 19.4.1): O and the control bar's switch, `transport.setMetronome`
    // outside undo and saved with the project (#383, N19); the switch lights from the snapshot.
    bool setMetronome(bool on);
    bool toggleMetronome();
    bool toggleMetronomeWhileRecording();
    bool setMetronomeEighthNotes(bool eighthNotes);
    bool setMetronomeSound(MetronomeSound sound);
    bool setCountIn(int bars);
    bool togglePunch(bool start);
    // Live's Arrangement Loop (manual 6.6), `transport.setLoop` outside undo and saved with the
    // project (#383, N19), from the CURRENT cell (ADR-0201). The control bar's switch; the brace over
    // `range`, the switch as it is; Loop Selection (Ctrl/Cmd+L): the brace to the time selection or
    // the selected clips' span and the loop on, or the loop toggled where the brace is already there
    // or nothing is selected; the brace's keys while it is selected (manual 6.6, §42.8); the Loop
    // Start and Loop Length fields typed, its length or its start kept.
    bool toggleLoop();
    bool setLoopRange(LoopRange range);
    bool loopSelection();
    bool braceKey(BraceKey key, bool command);
    bool setLoopStartAt(const std::string &text);
    bool setLoopLengthAt(const std::string &text);
    // Live's locators (manual 6.4; N25, #385), from the CURRENT cell (ADR-0201), each edit one undo
    // step. While playing, Set adds at the playhead and retains the selection. While stopped, it
    // adds at the selection start or insert marker, or deletes the selected locator at the current
    // position. Previous and Next
    // jump to the locator before or after the play position, past the first or the last to the
    // arrangement's start or end. A jump while playing moves playback there; while stopped, the
    // insert marker. Add, delete, move, rename and Edit Info Text (the marker's note) act on one
    // locator; Loop to Next Locator loops from a locator to the next, or to the arrangement's end.
    bool setLocator();
    bool setLocatorDeletes() const noexcept;
    bool jumpLocator(bool forward);
    bool jumpTo(std::int64_t tick);
    bool addLocatorAt(std::int64_t pos, bool select = true);
    bool deleteLocator(std::int64_t id);
    bool moveLocator(std::int64_t id, std::int64_t to);
    bool renameLocator(std::int64_t id, const std::string &name);
    bool setLocatorInfo(std::int64_t id, const std::string &text);
    bool loopToNextLocator(std::int64_t id);
    // Where the arrangement ends: the latest clip's end or locator, whichever is later.
    std::int64_t arrangementEnd() const;
    // `editing.linkLoopToSelection` (REAPER's, off by default): with it on, a time selection made
    // in the lanes moves the brace to it, the loop's switch as it is. Nothing when it is off.
    bool linkLoopToSelection();
    // An edit of the start's tempo or signature, where the snapshot cannot tell a stored value from
    // its own: `tried` (an insert) first; refused, it changed nothing, and `otherwise` (a replace)
    // follows. One step either way.
    bool submitStartEdit(std::vector<OpRequest> tried, std::vector<OpRequest> otherwise);
    // Live's track drag (the Arrangement View; manual 18.3): the selected tracks when `dragged` is
    // among them, else that track alone, to `to`; one undo step, "Move Track" or "Move Tracks".
    bool moveTracks(std::int64_t dragged, TrackDrop to);
    bool nudgeSelectedTracks(bool down);
    // Group the selected tracks, or ungroup an explicit group from its title menu.
    bool groupTracks(bool ungroup, std::int64_t group = 0);
    // Live: "Double-clicking a track's title bar brings up the Device View" (Live 12 manual, Live
    // Concepts): shown if it is hidden, the selected track's devices in it.
    bool showDeviceView();
    // Shows the Clip View if it is hidden: a double-click on a clip (manual 8).
    bool showClipView();
    bool setDetailViews(bool showClip, bool showDevices);
    // Live's Insert Empty MIDI Clip(s) (Ctrl/Cmd+Shift+M): an empty MIDI clip from `start` to `end`
    // on each MIDI track of `tracks`, one undo step; the first is selected.
    bool insertMidiClips(const std::vector<std::int64_t> &tracks, std::int64_t start, std::int64_t end);
    // Hands this window's selection to the session, which every window presents.
    enum class SelectionOrigin { Other, Arrangement, Mixer };
    void publishSelection(SelectionOrigin origin = SelectionOrigin::Other);
    SessionState &session() noexcept { return session_; }
    // Drag the strip's top edge: never below the floor, never past the ceiling that
    // leaves the transport, the ruler and one track row (ADR-0184, ADR-0203 d7).
    void resizeDevices(int height);
    void resizeClipView(int height);
    // Ctrl/Cmd+Alt+E in the Clip View (manual 10.2): its full height, as far as the layout gives it,
    // and back to the height it had.
    void toggleClipViewFull();
    // Whether the Clip View's Note Editor is the view in use, so that the Edit menu's commands are its own.
    bool noteEditorHasContext() const;
    // Whether the Clip View shows a MIDI clip's notes: its folding and scale switches can then be
    // thrown from the menus, with the keyboard anywhere.
    bool noteEditorShown() const;
    bool clipViewFull() const noexcept { return clipViewRestore_ > 0; }

  private:
    std::int64_t stoppedSetLocatorTick() const noexcept;
    bool restoreHistoryHead(std::optional<std::int64_t> head);
    bool restoreHistory(const std::function<History::Result()> &, const std::function<bool()> &reload);
    clip_edit::MidiConsolidation prepareMidiConsolidation() const;
    std::int64_t referenceTrack(const SnapshotReader &) const noexcept;
    std::vector<std::int64_t> trackAutomationLanes(std::int64_t track);
    bool deleteAutomationLanes(const std::vector<std::int64_t> &, const char *label);
    enum class TrackRenderAction { Freeze, Unfreeze, Flatten, BounceToNewTrack, BounceInPlace };
    bool canRenderTrack(TrackRenderAction, const engine::TrackNode &) const;
    bool renderTrack(TrackRenderAction, std::int64_t);
    ProjectDocument *trackRenderDocument_ = nullptr;
    bool beginArrangementRecording(std::int64_t takeStart, bool rolling);
    void setRecordPending(bool pending);
    bool insertTrack(bool midi);
    bool deleteTrack();
    // A transport edit (the metronome, the loop): outside undo, so no step and no Follow paused; the
    // session re-reads the project after it (afterEdit), so the engine clicks and loops as it says.
    bool submitTransport(LoopEdit edit);
    // After a submit: a refusal to the Status Bar; an edit pauses Follow and repaints.
    bool edited(const SubmitResult &);
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
    std::int64_t revealInLanes_ = 0, revealInMixer_ = 0;
    std::string window_;
    WindowState state_;
    DirtySet dirty_;
    int clipViewRestore_ = 0;
    std::optional<SnapshotReader> reader_;
    std::uint64_t generation_ = 0;
    bool pluginWindowsShown_ = true;
    std::int64_t autoOpen_ = 0; // a plug-in added, its window to open (WIN-03)
    int autoOpenFrames_ = 0;
    bool vblankFrame_ = false; // the frame running is the window's vblank clock's (A28)
    struct WindowFollower final : juce::AsyncUpdater {
        explicit WindowFollower(AdiRootComponent &r) : root(r) {}
        ~WindowFollower() override { cancelPendingUpdate(); }
        void handleAsyncUpdate() override { root.followPluginWindows(); }
        AdiRootComponent &root;
    } windowFollower_{*this};
    unsigned lastDrain_ = 0;
    bool playing_ = false;
    bool recordPending_ = false;
    bool midiOverdub_ = false; // Live's MIDI Arrangement Overdub, off in a new Set
    RecordStates recordStates_; // the arm and monitoring the application last handed over (MX-09)
    std::int64_t selectionArmedTrack_ = 0;
    bool changeSelectionArming(std::int64_t track);
    RecordStates freshRecordStates() const { return readRecordStates ? readRecordStates() : recordStates_; }
    bool toggleArm(std::optional<std::int64_t> clicked, bool add);
    bool midiIn_ = false, midiOut_ = false;
    bool computerMidiEnabled_ = false;
    ComputerMidiKeyboard computerMidi_;
    std::optional<int> computerMidiToggleKey_;
    void globalFocusChanged(juce::Component *) override;
    bool automationMode_ = false;
    // Descriptor metadata for a selection whose device panel is hidden.
    mutable std::map<std::int64_t, AutomationTarget> automationTargets_;
    std::map<std::int64_t, std::vector<AdditionalAutomationLane>> additionalAutomationLanes_;
    std::shared_ptr<const engine::Snapshot> automationLaneSnapshot_;
    std::shared_ptr<const ParameterPublication> automationLanePublication_;
    void refreshAdditionalAutomationLanes();
    void layoutAdditionalAutomationLanes();
    bool infoShown_ = true;
    bool infoPressed_ = false;
    DensityId appliedDensity_ = DensityId::Compact;
    void applyInfoAnalyserMode();
    std::int64_t position_ = 0;
    double sampleRate_ = 0; // the device's granted rate, once a driver runs
    float interfaceZoom_ = 1;
    std::optional<std::string> zoomDisplay_; // the display the zoom was last followed on
    juce::Component *hovered_ = nullptr;
    juce::String infoText_;
    juce::Rectangle<int> clipArea_, deviceArea_, infoArea_, browserArea_, mixerArea_,
        mixerViewArea_, stripArea_, mixViewArea_, mixViewDock_;
    int leftWidth_ = 0, rightWidth_ = 0;
    int shownPanelWidth(const PanelGeometry &) const noexcept;
    std::string error_;
    std::unique_ptr<juce::DialogWindow> playbackProblemsWindow_;
    void refreshPlaybackProblemList();
    void showPlaybackProblems();
    friend class StatusBar;
    // E5: the prompt while open and the optional warning for the drop in progress.
    std::unique_ptr<ImportPrompt> importPrompt_;
    std::optional<std::string> importing_;
    bool dropPlain(const std::vector<std::pair<std::string, std::string>> &files, std::int64_t track, std::int64_t pos,
                   bool separate);
    void finishImport(std::int64_t first, std::int64_t onTrack, const std::string &why);
    MixViewProvider defaultMixProvider_;
    const MixViewProvider *mixProvider_ = &defaultMixProvider_;
    MomentaryLatch latch_;
    bool drawBeforeLatch_ = false, trimBeforeDrawLatch_ = false;
    TapTempo tap_;
    std::int64_t tapStep_ = 0; // the undo step the tap run made
    double tapBpm_ = 0;
};
// The interface zoom's steps, 50 % to 200 % (ADR-0129 d3): the next one in or out of
// `now`, or nothing at either end.
std::optional<float> nextZoom(float now, bool in) noexcept;
// A display's name in the App settings: its geometry and DPI, as the floating windows name the
// monitors (JUCE exposes no stable EDID identifier); the one nearest a point on the screen.
std::string displayId(const juce::Displays::Display &);
std::string displayAt(juce::Point<int> screen);
// A launch's arguments, unquoted, in order. What a launcher puts around them is no argument: a
// trailing space (PowerShell's Start-Process leaves one) gave an empty token, which opened the working
// folder as the project (walk-B B11).
juce::StringArray launchArguments(const juce::String &commandLine);
// WTS-01: a test launch (--smoke-output, --control-test): an instance of its own, never the running
// copy's, which keeps its window out of the foreground and reports to stderr.
bool isTestLaunch(const juce::String &commandLine);
// SC-100, ADR-0258 d7: with files.projectsFolder unset, projects live where the user can see them,
// <documents>/ADI/Projects, beside the User Library (Windows). With no documents folder (an ADI_HOME
// launch passes none), <config>/projects. Not made here.
juce::File defaultProjectsFolder(const settings::AppSettings &settings, const juce::File &documents);
// The projects folder (File & Folder's files.projectsFolder, else the default above), made when missing.
juce::File projectsFolder(const settings::AppSettings &settings);
// Live's New Live Set (manual 5.4.1): a new project is untitled and asks nothing; Save As names and
// places it. Reserve an exclusive Untitled, Untitled(2), ... directory for launch or New.
juce::File nextUntitledProject(const settings::AppSettings &settings);
// Keep an explicitly selected existing file. New songs get a same-named folder.
juce::File songProjectFile(const juce::File &file);
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
juce::String windowTooltipLabel(const juce::Component &);
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
    using juce::DocumentWindow::keyStateChanged;
    std::function<void()> onClose;
    void closeButtonPressed() override {
        if (onClose)
            onClose();
        else
            setVisible(false);
    }
    juce::MenuBarModel *menu() noexcept;
    // Placed, or moved onto another display: the root takes the zoom kept for it there (N4).
    void moved() override;
    void resized() override;
    // The displays changed (a monitor gone, another resolution or scale): the title bar kept on one.
    void parentSizeChanged() override;
    void minimisationStateChanged(bool) override;
    void activeWindowStatusChanged() override;
    void broughtToFront() override;
    // Where the window stands, saved after each move or size once it stands still (WIN-02).
    PlaceWatch place;
    // WTS-01: a test launch's window shows without taking the foreground (JUCE brings a window to the
    // front as it shows); set before it is first shown.
    bool quiet = false;
    void visibilityChanged() override;
    // What the window keeps of the App settings, brought up to date: each frame does it.
    void followSettings();
    AdiRootComponent &root() noexcept { return *root_; }
    const juce::TooltipWindow *tooltips() const noexcept { return tips_.get(); }
    int tooltipDelayMs() const noexcept { return tipDelayMs_; }

  private:
    bool keyPressed(const juce::KeyPress &k, juce::Component *) override;
    bool keyStateChanged(bool isKeyDown, juce::Component *) override;
    AdiRootComponent *root_;
    AppCommands &commands_;
    std::unique_ptr<juce::TooltipWindow> tips_;
    int tipDelayMs_ = 0;
    std::unique_ptr<ShellMenu> menu_;
    juce::VBlankAttachment clock_;
};
// ADI's import prompt (E5; Live 12 manual 9.2.1, 9.2.2, 5.7; ADR-0132 d6): what dropped audio is
// imported with. Its own choices, all off by default; Import keeps them as the last ones (the
// settings), Cancel imports nothing. It is ADI's dialog: Live imports without asking.
class AdiRootComponent::ImportPrompt final : public juce::Component {
  public:
    using Reply = std::function<void(std::optional<ImportChoices>, bool stopAsking)>;
    ImportPrompt(std::size_t files, const ImportChoices &, Reply answer);
    void paint(juce::Graphics &) override;
    void resized() override;
    // What the toggles say now.
    ImportChoices choices() const;
    // Import or Cancel, as the buttons do.
    void answer(bool import);
    // Return imports and Escape cancels; Tab moves among its controls; every other key stays here,
    // so the arrangement's keys do not act behind an open prompt.
    bool keyPressed(const juce::KeyPress &) override;
    juce::ToggleButton warp{"Warp"}, fades{"Create Fades on Clip Edges"}, copy{"Copy into the project's folder"};
    juce::ToggleButton stopAsking{"Do not show this again"};
    juce::ComboBox mode;
    juce::TextButton import{"Import"}, cancel{"Cancel"};

  private:
    juce::String title_;
    Reply answer_;
};
} // namespace adi::ui
