// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/gesture_producers.hpp"
#include "adi/ops.hpp"
#include "adi/return_send_tap.hpp"
#include "adi/ui/arm_edits.hpp"
#include "adi/ui/track_edits.hpp"
#include "ui_controls.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace adi::engine {
struct StripMeter; // adi/engine/mixer.hpp: the engine's post-fader peak and RMS
struct MidiNoteActivity;
}
namespace adi::ui {
class AdiRootComponent;
class MixerPanel;
struct DevicePanelData; // adi/ui/parameter_feed.hpp
// Live's Arm switch: a box with its dot.
class ArmButton final : public juce::TextButton {
  public:
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// A right-click opens a mixer switch's menu without switching it.
class MixerMenuButton final : public controls::PrimaryMouseControl<juce::TextButton> {
  public:
    using PrimaryMouseControl::PrimaryMouseControl;
    std::function<void()> onMenu;
    void mouseDown(const juce::MouseEvent &) override;
};
class ValueEntry;
// A mixer control the left button moves; the right one opens the strip's menu for it and moves nothing (AU-04):
// JUCE's Slider starts a drag on any button when its own menu is off.
class PrimaryButtonSlider final : public juce::Slider {
  public:
    PrimaryButtonSlider();
    ~PrimaryButtonSlider() override;
    void enableValueEdits(double defaultValue, double step);
    bool keyPressed(const juce::KeyPress &) override;
    // A gesture belongs to the press that began it: a primary press's drag and release reach the Slider whatever
    // keys or buttons join it (a Ctrl held at the release is a popup click on macOS), and a popup press's never do.
    void mouseDown(const juce::MouseEvent &e) override {
        primary_ = !e.mods.isPopupMenu();
        if (primary_)
            juce::Slider::mouseDown(e);
    }
    void mouseDrag(const juce::MouseEvent &e) override {
        if (primary_)
            juce::Slider::mouseDrag(e);
    }
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;

  private:
    bool primary_ = false;
    std::optional<double> default_;
    double step_ = 0;
    std::unique_ptr<ValueEntry> valueEntry_;
};
// Live's peak level (manual 18.1): the highest peak since the last click, in dB, red once
// it has clipped. A click resets it.
class PeakHold final : public juce::Button {
  public:
    PeakHold();
    void paintButton(juce::Graphics &, bool over, bool down) override;
    float held = 0; // linear
};
// Live's meter scale, measured on Live 11's mixer: how far down the meter a level in dB
// sits, from +6 dB at the top (0) to -60 dB and below at the foot (1); and back.
double meterDepth(double db) noexcept;
double meterLevel(double depth) noexcept;
// One track's mixer controls in the track's own row, as Live's Arrangement Track
// Controls (manual 18.1), laid out as Live 11 draws them (measured 2026-10-27): the
// In/Out column, then the Track Activator, Solo and Arm; volume and pan; one send per
// return on the rows under them (ADR-0203 d3); the meter. A channel has no button of its own for
// any Mix View function: those are added in the Mix View like every other one (ADR-0243).
class MixerStrip final : public juce::Component,
                         public juce::SettableTooltipClient,
                         public juce::DragAndDropTarget,
                         public juce::FileDragAndDropTarget {
  public:
    // Esc during a live drag cancels it (MX-23).
    bool keyPressed(const juce::KeyPress &) override;
    explicit MixerStrip(MixerPanel &);
    ~MixerStrip() override { pan.setLookAndFeel(nullptr); }
    void bind(std::int64_t track);
    void frame(bool changed) noexcept;
    void paint(juce::Graphics &) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent &) override;
    // A track's header is one piece in Live: the split line at the foot of its track controls, as at
    // the foot of its name, shows the resize cursor and drags the track's height (Live 12 manual 6.9).
    void mouseMove(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    // A Mixer View strip's title bar: a click selects its track (Shift, Ctrl/Cmd as a header's), and a
    // double-click also brings up the Device View.
    static constexpr int kTitleHeight = 17;
    // A double-click on a Mixer View strip's title selects its track and brings up the Device View.
    void mouseDoubleClick(const juce::MouseEvent &) override;
    bool isInterestedInDragSource(const SourceDetails &) override;
    void itemDragEnter(const SourceDetails &details) override { itemDragMove(details); }
    void itemDragMove(const SourceDetails &) override;
    void itemDragExit(const SourceDetails &) override;
    void itemDropped(const SourceDetails &) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void fileDragEnter(const juce::StringArray &, int, int) override;
    void fileDragExit(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    // A row takes clicks only level with the lanes (MixerPanel::rowsBand).
    bool hitTest(int x, int y) override;
    std::int64_t track() const noexcept { return track_; }
    bool dragging() const noexcept { return dragging_; }
    // What each meter column draws after the window's ballistics, in linear units.
    float peak(std::size_t channel) const noexcept { return levels_[channel].peak; }
    float rms(std::size_t channel) const noexcept { return levels_[channel].rms; }
    // Whether the strip has found the engine's meter for its track.
    bool hasMeter() const noexcept { return meter_ != nullptr; }
    // Finds the engine's meter for the track again (MixerPanel::meter): on binding, on every
    // refresh, and when the panel is asked to (MixerPanel::resolveMeters).
    void resolveMeter();
    // Live's automation LED on Volume, Pan and each send (manual 25.4), as a device knob's: lit while the
    // control follows its lane, dim once a manual change overrides it, none without a lane.
    enum class Automation : char { None, Followed, Overridden };
    Automation automation(const juce::Component &control) const noexcept;
    void paintOverChildren(juce::Graphics &) override;
    PrimaryButtonSlider volume, pan;
    // Live's Track Delay (manual 18.7): a value box in milliseconds, typed or dragged up or down, a
    // negative value pre-delaying; its "ms" unit box beside it. Shown by Live's Track Options: on
    // the row after the sends in the track controls, and as the section under the mixer in the
    // Mixer View.
    PrimaryButtonSlider delay;
    void setDelayTooltip(const juce::String &tip);
    // Cue Volume on Main in both views. One transport op at release or when typed, outside undo.
    PrimaryButtonSlider cueVolume;
    enum class RoutingChooser { MidiFrom, MidiChannel, MidiTo, AudioTo, MainOut, CueOut };
    std::function<void(RoutingChooser, juce::PopupMenu)> onRoutingMenu;
    // The Arrangement's compact Cue Out chooser shares the Mixer View's routing menu.
    juce::TextButton cueOut;
    void cueOutMenu();
    // Live 12's Keep Latency toggle under Track Delay in the Mixer's Track Options (manual 18.8, its
    // figure): Keep Monitoring Latency in Recorded Audio, lit from the snapshot (#397, N31), on a
    // track that records.
    juce::TextButton keepLatency{"Keep Latency"};
    // The drag's travel: the box's 2000 ms over this many pixels, 0.1 ms a pixel (PROVISIONAL,
    // Live's step is M1-06).
    static constexpr int kDelayDragPixels = 20000;
    MixerMenuButton active, solo{"S"};
    bool cueMode_ = false; // the Solo button is the Cue button (Options > Cue Switches)
    bool cueDragging_ = false;
    ArmButton arm;
    // A return's Pre/Post switch, where a track has Arm (ADR-0203 d3: one switch on each return);
    // lit while its sends tap after the fader, as Live's.
    juce::TextButton prePost{"Post"};
    PeakHold peakHold; // the Mixer View's only
    // Whether the row shows its In/Out column: when the panel is wide enough.
    bool showsInOut() const noexcept;
    // Live 12 manual 18.8: a right-click on the In or Auto monitor button switches Keep Monitoring
    // Latency in Recorded Audio (#397, N31). Whether a point is on those two buttons in the track
    // controls' column, and the menu they open.
    bool onMonitorInOrAuto(juce::Point<int>) const noexcept;
    // Which of In, Auto and Off is under `at`, where this strip draws the monitoring switch (MX-09).
    std::optional<Monitor> monitorAt(juce::Point<int>) const noexcept;
    juce::PopupMenu monitorMenu();
    bool onAudioInput(juce::Point<int>) const noexcept;
    juce::PopupMenu audioInputMenu();
    std::function<void(juce::PopupMenu)> onAudioInputMenu;
    // Whether the sends show, in the row and in the Mixer View: the View menu's Sends (§42.1).
    bool showsSends() const noexcept;
    // The switches the keyboard reaches, in the row's order: the Track Activator, Solo, Arm.
    // A click on one does not take the focus.
    std::array<juce::Button *, 3> switches() noexcept {
        return {&active, &solo, kind_ == "return" ? static_cast<juce::Button *>(&prePost) : &arm};
    }
    // The track's kind, as the snapshot gives it: Live draws a return's row and Main's their own way.
    const std::string &kind() const noexcept { return kind_; }
    // A MIDI track (its In/Out column shows All Ins and All Channels), and whether the track has an
    // audio mixer: a MIDI track holding no instrument has no volume, pan or sends, as Live's.
    bool isMidi() const noexcept { return midi_; }
    bool hasAudio() const noexcept { return audio_; }
    // Where the track plays to, as its output chooser shows it: Main's outputs (1/2), Main, its
    // group ("Group"), or No Output.
    juce::String outputLabel() const;
    // Main's strip in the Mixer View: each return's Pre/Post switch in its Sends section, as Live's.
    const std::vector<std::unique_ptr<juce::TextButton>> &returnSwitches() const noexcept { return returnSwitches_; }
    // One send per return, the returns in the arrangement's order (ADR-0203 d3): Live's value
    // boxes, three to a row, in the track controls; Live's knobs under "Sends", each lettered as
    // its return, in the Mixer View. Main has none. A return's own are off until Enable Send.
    const std::vector<std::unique_ptr<juce::Slider>> &sendKnobs() const noexcept { return sends_; }
    // The return the send at `index` goes to.
    std::int64_t sendsTo(std::size_t index) const noexcept { return returns_[index]; }
    bool sendOn(std::size_t index) const noexcept { return sendOn_[index] != 0; }
    // How many rows of the Sends section the Mixer View strip has room for, one per return: on
    // Main's strip too, which has no knobs, so the mixers line up.
    std::size_t sendsShown() const noexcept { return shown_; }
    // Whether the Mixer View strip shows Live 12's In/Out section: when the Mixer Controls show it
    // and the strip has room for it and the mixer. It stands under the name, over the sends.
    bool showsMixerInOut() const noexcept { return io_ > 0; }
    static constexpr int kInOutHeight = 120;
    // Whether the Mixer View strip shows Live's Track Delay section under its mixer: when the Mixer
    // Controls' Track Options shows it and the strip has room for it and the mixer.
    bool showsTrackDelay() const noexcept { return delay_ > 0; }
    static constexpr int kDelayHeight = 54;
    // How far the Mixer View fader reaches past the meter's top (and its foot, without the numeric field):
    // half its ◁ and the outline, so the ◁ at +6 dB or -inf is pressed where it is drawn (walk-A A22).
    static constexpr int kFaderReach = 6;
    // Commits the track's delay at the box's value, in ms, as one undo step: what a drag's end and
    // a typed value do. The project's rate turns it into samples (`setTrackDelay`).
    void commitDelay();
    // Commits the send at `index` at its knob's value, as one undo step; a new send writes its
    // row first. What a knob's drag and a typed value do.
    void commitSend(std::size_t index);
    // Live's Enable Send on the send at `index`, or its undoing.
    void enableSend(std::size_t index, bool on);
    // Live's Enable All Sends, on a return's strip: every one of its sends on, one undo step.
    void enableAllSends();
    // The Solo button's menu (Live 12 manual 18.6): Solo in Place, ticked from the project's one
    // field, which the Options menu's entry shares (#412); picked, the root's toggle writes
    // `mixer.setSoloInPlace` outside undo. What a right-click on Solo shows.
    juce::PopupMenu soloMenu();

  private:
    friend class MixerPanel;
    void trackTitleMenu(juce::Point<int> where);
    void selectTitle(std::int64_t track);
    bool cancelTitleDrag();
    void setDropPreview(bool);
    std::optional<RoutingChooser> routingChooserAt(juce::Point<int>) const noexcept;
    void showRoutingMenu(RoutingChooser);
    juce::PopupMenu midiInputMenu(RoutingChooser);
    juce::String midiInputLabel(RoutingChooser) const;
    juce::PopupMenu midiOutputMenu();
    juce::String midiOutputLabel() const;
    juce::String audioDestinationLabel(const engine::TrackNode &) const;
    std::span<const std::pair<int, std::string>> stereoOutputs() const;
    juce::String stereoOutputLabel(RoutingChooser) const;
    int selectedStereoOutput(RoutingChooser) const;
    void paintInOut(juce::Graphics &, int x) const;
    const char *audioInputLabel() const;
    bool audioInputIsResampling() const;
    void showAudioInputMenu();
    juce::Rectangle<int> monitorBounds() const noexcept;
    void layoutMonitor();
    class MonitorSwitch final : public juce::Component, public juce::SettableTooltipClient {
      public:
        explicit MonitorSwitch(MixerStrip &);
        bool hitTest(int, int) override;
        void paint(juce::Graphics &) override;
        void mouseDown(const juce::MouseEvent &) override;
        bool keyPressed(const juce::KeyPress &) override;
        void focusGained(FocusChangeType) override { repaint(); }
        void focusLost(FocusChangeType) override { repaint(); }

      private:
        MixerStrip &strip_;
    };
    MonitorSwitch monitor_{*this};

    // The Mixer View strip's In/Out section, as Live 12's Mixer draws it.
    void paintMixerInOut(juce::Graphics &) const;
    // Live's "ms" unit box beside the Track Delay's value box, lit as Live's.
    void paintDelayUnit(juce::Graphics &) const;
    // The Mixer View strip's meter, under its fader.
    juce::Rectangle<int> meterArea() const noexcept;
    void panMenu();
    void volumeMenu();

  public:
    // Menus for the mixer controls, including controls with no automation yet.
    juce::PopupMenu volumeMenuItems();
    juce::PopupMenu panMenuItems();
    juce::PopupMenu sendMenuItems(std::size_t index);
    juce::PopupMenu activatorMenuItems();

  private:
    void sendMenu(std::size_t index);
    void followSendAutomation(std::int64_t returnTrack);
    // A send's tap chosen from its menu (DF-W33): one `routing.setSendTap`.
    void chooseSendTap(std::size_t index, IndividualSendTap tap);
    void paintMeter(juce::Graphics &, juce::Rectangle<int>) const;
    // Live's MIDI meter on a track with no instrument, at the head of the meter's column.
    void paintMidiMeter(juce::Graphics &, juce::Rectangle<int>) const;
    // One knob per return, made or dropped when the returns or the track's devices change; then
    // laid out. `devices` is the window's device publication, or null.
    void syncSends(const std::vector<DevicePanelData> *devices);
    std::unique_ptr<juce::Slider> makeSend(std::size_t index);
    // The sends' rows in the track controls: three to a row, from the row under volume and pan.
    int sendRows() const noexcept { return static_cast<int>((sends_.size() + 2) / 3); }
    // A Mixer View strip's sends: Live's two columns, each knob 22 pt under the one before.
    static constexpr int kSendPitch = 22, kSendPadding = 28;
    static int sendsHeight(std::size_t count) noexcept {
        return count == 0 ? 0 : kSendPadding + kSendPitch * static_cast<int>(count);
    }
    void layoutSends();
    juce::Rectangle<int> sendBounds(std::size_t index) const noexcept;
    juce::Rectangle<int> sendArrowBounds(bool down) const noexcept;
    MixerPanel &panel_;
    std::int64_t track_ = 0;
    // Where a Mixer View strip's sections begin, under the name and its rule.
    static constexpr int kMixerTop = 19;
    // What a Mixer View strip's mixer keeps under its sends: the peak level above pan, the
    // activator, Solo and Arm.
    static constexpr int kMixerRoom = 110;
    int sendsTop() const noexcept { return kMixerTop + io_; }
    int mixerTop() const noexcept { return sendsTop() + sendsHeight(shown_); }
    // Where the Mixer View strip's mixer ends: at the Track Delay section when it shows, else the foot.
    int mixerBottom() const noexcept { return getHeight() - delay_; }
    std::vector<std::unique_ptr<juce::Slider>> sends_;
    std::vector<std::unique_ptr<juce::TextButton>> returnSwitches_;
    std::string kind_;
    bool midi_ = false, audio_ = true, inGroup_ = false;
    const void *devices_ = nullptr; // the device publication the strip last read
    int label_ = 0; // what the activator shows: a track's number, or -1 - a return's place
    int cueFirstChannel_ = 0;
    // Throws a return's own Pre/Post switch (`TrackNode::returnSendTap`): one `return.setSendTap`,
    // one undo step, which the engine spreads over every send into the return.
    void throwPrePost(std::int64_t ret);
    // The return each send goes to; the snapshot's returns, refilled on each change; whether each
    // send is on.
    std::vector<std::int64_t> returns_, order_;
    // Each of `returns_`'s letters, as its knob and its column show it ("A", "DA").
    std::vector<juce::String> letters_;
    std::vector<std::array<char, 2>> letterCodes_; // the letters, compared each sync without allocating
    std::vector<char> sendOn_;
    // Each automated control's LED, and whether it shows its lane's value: Volume, Pan, then the sends.
    struct AutomationShown {
        Automation state = Automation::None;
        bool showing = false;
    };
    std::vector<AutomationShown> automation_;
    const juce::Component *automatedControl(std::size_t index) const noexcept;
    // While a lane is followed its control shows the envelope at the playhead, moving as the engine
    // plays it; shown only, no op and no gesture. `volumeDb` and `pan` are the track's own values.
    void followAutomation(double volumeDb, double pan);
    std::size_t shown_ = 0;
    int io_ = 0; // the In/Out section's height in the Mixer View, 0 while it is hidden
    int delay_ = 0; // the Track Delay section's height in the Mixer View, 0 while it is hidden
    std::size_t position_ = 0; // the track's place, 1-based: its colour by place
    std::size_t number_ = 0;   // Live's track number, which skips the returns: what the activator shows
    std::size_t group_ = 0;    // its group's place, 1-based; 0 at the top
    bool dragging_ = false, selected_ = false;
    bool dropPreview_ = false;
    TrackRecordState record_; // the arm and monitoring last shown (MX-09)
    // A drag heard while it moves (MX-23): begun on the control's drag start, ended on its release.
    bool live_ = false, cancelled_ = false; // cancelled: Esc came before the release, which commits nothing
    void beginLive(gesture::ScalarKind, std::int64_t id, gesture::ScalarControlDomain, const std::string &label);
    bool endLive();
    int resizeFrom_ = -1; // the track's height when its split line was pressed; -1 while not resizing
    bool resizeFromTop_ = false;
    struct TitleDrag {
        std::int64_t source;
        std::uint64_t generation;
        int startX;
        bool movingReturn;
        bool selectOnRelease;
        bool moving = false;
    };
    std::optional<TitleDrag> titleDrag_;
    // Empty away from an Arrangement split line, otherwise whether the line is at the top.
    std::optional<bool> splitLine(const juce::MouseEvent &) const noexcept;
    // The engine's post-fader meter for the track (UI-ARCHITECTURE §9; `StripNode::meter()`),
    // written per callback by the audio thread and read here with relaxed loads; null until the
    // session has a strip for the track. The strips live as long as the session (ADR-0122's
    // rule), and the session as long as the root that holds this panel.
    const engine::StripMeter *meter_ = nullptr;
    std::uint32_t midiOnsets_ = 0;
    double midiPulseUntil_ = 0;
    bool midiLit_ = false;
    // Each channel's level falls after the engine's value drops. Its peak line
    // holds for a moment before falling, independently of the other channel.
    struct MeterLevel {
        float peak = 0, rms = 0;
        double peakSince = 0;
    };
    std::array<MeterLevel, 2> levels_{};
    double lastFrame_ = -1;
};
// Two shapes over one strip: rows level with the arrangement's lanes (the track
// controls), or Live's Mixer View, vertical strips left to right (manual 18.1).
class MixerPanel final : public juce::Component {
  public:
    explicit MixerPanel(AdiRootComponent &, bool vertical = false);
    bool vertical() const noexcept { return vertical_; }
    // A vertical strip's width, Live 11's 93 and its rule, and the height from which it
    // shows ticks every 6 dB and a numeric volume field (Live: drag the mixer's top edge
    // up for them).
    static constexpr int kStripWidth = 95, kTallHeight = 260;
    // The track controls' columns, Live 11's at 100 %: In/Out with the dark rules
    // either side, the mixer with its meter, the strip down the right-hand edge.
    static constexpr int kInOutWidth = 78, kControlsWidth = 116, kRightStrip = 27;
    static constexpr int kFullWidth = kInOutWidth + kControlsWidth + kRightStrip;
    void frame(bool changed) noexcept;
    void resized() override;
    void paint(juce::Graphics &) override;
    // The frame above and below the lanes stands in front of a row scrolled past either edge.
    void paintOverChildren(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    void contextMenu(juce::Point<int>);
    void showContextMenu(juce::PopupMenu, juce::Point<int>);
    void showContextMenu(juce::PopupMenu, const juce::PopupMenu::Options &);
    // A presenter can inspect the requested menu without opening a native popup.
    std::function<void(juce::PopupMenu)> contextMenuPresenter;
    // Where the track controls' rows show, level with the arrangement's lanes, in this panel's
    // coordinates; the whole height for the Mixer View.
    juce::Range<int> rowsBand() const noexcept;
    // Find every bound strip's meter again: when the session has gained strips (the audio device
    // attached; a refresh does the same for each strip shown).
    void resolveMeters();
    // Move Focus to Mixer (§42.16): the Track Activator of the track's strip, else of the
    // first strip shown. False when no strip is shown.
    bool focusTrack(std::int64_t track);
    // Next or Previous Neighbor (§41.4.1.2): from the switch with the focus to the same
    // switch on the next or previous track; at either end it stays. False when the focus is
    // not on one of this panel's switches.
    bool focusNeighbour(bool next);
    // Brings the track at `index`, in the arrangement's order, into view, as Live does when the
    // focus moves to it: the Mixer View scrolls sideways; the track controls' rows are the
    // arrangement's lanes, so the arrangement scrolls, its top kept when a row is taller than
    // the view.
    void reveal(std::size_t index);
    bool submit(const std::string &op, Payload payload, const char *label);
    AdiRootComponent &root;
    // Where a strip's meter reads from: the engine's post-fader meter (UI-ARCHITECTURE §9;
    // `Session::strips().stripFor(track)->meter()`, #381), written by the audio thread and read
    // with relaxed loads, never a tap. Null for a track the session has no strip for yet.
    std::function<const engine::StripMeter *(std::int64_t)> meter;
    std::function<engine::MidiNoteActivity(std::int64_t)> midiActivity;
    // The ballistics' clock, in seconds; JUCE's while unset (the tests set their own).
    std::function<double()> clock;
    // Each track's Peak Level (manual 18.1), linear, by track id: the strips are reused for other
    // tracks as tracks are added, moved, folded or scrolled past, and the hold is the track's (A12).
    std::unordered_map<std::int64_t, float> peakLevels;
    // Auto Gain Stage, the one offline command (ADR-0195 d4, ADR-0203 d14): LUFS or RMS.
    std::function<bool(bool lufs)> autoGain;
    const std::vector<std::unique_ptr<MixerStrip>> &strips() const noexcept { return strips_; }

  private:
    friend class MixerStrip;
    struct TitleDrop {
        TrackDrop target;
        int x;
        bool intoGroup = false;
    };
    TitleDrop trackDropAt(int x, bool movingReturn) const;
    void previewTrackDrop(std::optional<TitleDrop> drop);
    void place();
    void placeColumns();
    struct ColumnSections {
        int inOut = 0, trackDelay = 0;
        std::size_t sends = 0;
    };
    ColumnSections columnSections(int height) const noexcept;
    std::size_t firstSend(std::size_t shown) const noexcept;
    void scrollSendsBy(int rows, std::size_t shown);
    bool scrollSends(const juce::MouseEvent &, const juce::MouseWheelDetails &);
    // Live's frame above the rows: the band, and H and W beside the Overview's end.
    void paintTop(juce::Graphics &) const;
    bool vertical_;
    int scrollX_ = 0;
    // The columns share one window into Main's return order. Each track has its own eligible returns.
    std::vector<std::int64_t> sendReturns_, nextSendReturns_;
    std::size_t sendOffset_ = 0;
    float sendWheelFraction_ = 0.f;
    std::optional<TitleDrop> trackDrop_;
    std::vector<std::unique_ptr<MixerStrip>> strips_;
};
// Bitwig's Use Large Track Height (Master Reference 10, Figure 33; the director, 28 October
// 2026): its icon of two columns of bars, lit while every track shows at its own height.
class TrackHeightButton final : public juce::Button {
  public:
    TrackHeightButton();
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// The strip down the Arrangement's outer edge holds the track height control. Flipped, it moves to
// the window's left edge so the track controls meet the lanes (the director, 28 October 2026).
class SideStrip final : public juce::Component {
  public:
    SideStrip();
    void paint(juce::Graphics &) override;
    void resized() override;
    // Where its head begins: under the bar over the track names.
    void setHead(int y);
    TrackHeightButton tracks;

  private:
    int head_ = 0;
};
// The Mixer View's top edge: drag it up for taller strips.
class MixerViewResizer final : public juce::Component {
  public:
    explicit MixerViewResizer(AdiRootComponent &);
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;

  private:
    AdiRootComponent &root;
    int start = 0;
};
} // namespace adi::ui
