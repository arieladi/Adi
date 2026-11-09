// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <array>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <string>
#include <vector>
namespace adi::engine {
struct StripMeter; // adi/engine/mixer.hpp: the engine's {peak, rms} per strip (#381)
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
// Live's Solo switch: a right-click opens its menu (Solo in Place, manual 18.6) and solos nothing.
class SoloButton final : public juce::TextButton {
  public:
    using juce::TextButton::TextButton;
    std::function<void()> onMenu;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
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
// return on the rows under them (ADR-0203 d3); ADI's channel slots for ADI-Q and ADI-L
// (ADR-0204 d1) on the row after the sends; the meter.
class MixerStrip final : public juce::Component, public juce::SettableTooltipClient {
  public:
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
    // A double-click on a Mixer View strip's title selects its track and brings up the Device View.
    void mouseDoubleClick(const juce::MouseEvent &) override;
    // A row takes clicks only level with the lanes (MixerPanel::rowsBand).
    bool hitTest(int x, int y) override;
    std::int64_t track() const noexcept { return track_; }
    bool dragging() const noexcept { return dragging_; }
    // What the meter draws, linear: the level and the peak line, after the window's ballistics.
    float peak() const noexcept { return peak_; }
    float rms() const noexcept { return rms_; }
    // Whether the strip has found the engine's meter for its track.
    bool hasMeter() const noexcept { return meter_ != nullptr; }
    // Finds the engine's meter for the track again (MixerPanel::meter): on binding, on every
    // refresh, and when the panel is asked to (MixerPanel::resolveMeters).
    void resolveMeter();
    juce::Slider volume, pan;
    // Live's Track Delay (manual 18.7): a value box in milliseconds, typed or dragged up or down, a
    // negative value pre-delaying; its "ms" unit box beside it. Shown by Live's Track Options: on
    // the row under ADI's slots in the track controls, and as the section under the mixer in the
    // Mixer View.
    juce::Slider delay;
    // The drag's travel: the box's 2000 ms over this many pixels, 0.1 ms a pixel (PROVISIONAL,
    // Live's step is M1-06).
    static constexpr int kDelayDragPixels = 20000;
    juce::TextButton active, eq{"Q"}, limiter{"L"};
    SoloButton solo{"S"};
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
    juce::PopupMenu monitorMenu();
    // Whether the sends show, in the row and in the Mixer View: the View menu's Sends (§42.1).
    bool showsSends() const noexcept;
    // The switches the keyboard reaches, in the row's order: the Track Activator, Solo, Arm,
    // then ADI-Q and ADI-L. A click on one does not take the focus.
    std::array<juce::Button *, 5> switches() noexcept {
        return {&active, &solo, kind_ == "return" ? static_cast<juce::Button *>(&prePost) : &arm, &eq, &limiter};
    }
    // The track's kind, as the snapshot gives it: Live draws a return's row and Main's their own way.
    const std::string &kind() const noexcept { return kind_; }
    // A MIDI track (its In/Out column shows All Ins and All Channels), and whether the track has an
    // audio mixer: a MIDI track holding no instrument has no volume, pan or sends, as Live's.
    bool isMidi() const noexcept { return midi_; }
    bool hasAudio() const noexcept { return audio_; }
    // Where the track plays to, as its output chooser shows it: Main's outputs (1/2), Master, its
    // group ("Group"), or No Output.
    const char *outputLabel() const noexcept;
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
    // and the strip has room for it and the mixer. It stands under the slots, over the sends.
    bool showsMixerInOut() const noexcept { return io_ > 0; }
    static constexpr int kInOutHeight = 120;
    // Whether the Mixer View strip shows Live's Track Delay section under its mixer: when the Mixer
    // Controls' Track Options shows it and the strip has room for it and the mixer.
    bool showsTrackDelay() const noexcept { return delay_ > 0; }
    static constexpr int kDelayHeight = 36;
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
    void paintInOut(juce::Graphics &, int x) const;

    // The Mixer View strip's In/Out section, as Live 12's Mixer draws it.
    void paintMixerInOut(juce::Graphics &) const;
    // Live's "ms" unit box beside the Track Delay's value box, lit as Live's.
    void paintDelayUnit(juce::Graphics &) const;
    // The Mixer View strip's meter, under its fader.
    juce::Rectangle<int> meterArea() const noexcept;
    void panMenu();
    void sendMenu(std::size_t index);
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
    int sendsHeight(std::size_t count) const noexcept {
        return count == 0 ? 0 : 28 + 22 * static_cast<int>(count);
    }
    juce::Rectangle<int> sendBounds(std::size_t index) const noexcept;
    MixerPanel &panel_;
    std::int64_t track_ = 0;
    // Where a Mixer View strip's mixer begins, under the name, the slots and the sends.
    static constexpr int kMixerTop = 40;
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
    // Throws a return's own Pre/Post switch (`TrackNode::returnSendTap`): one `return.setSendTap`,
    // one undo step, which the engine spreads over every send into the return.
    void throwPrePost(std::int64_t ret);
    // The return each send goes to; the snapshot's returns, refilled on each change; whether each
    // send is on.
    std::vector<std::int64_t> returns_, order_;
    std::vector<char> sendOn_;
    std::size_t shown_ = 0, returnCount_ = 0;
    int io_ = 0; // the In/Out section's height in the Mixer View, 0 while it is hidden
    int delay_ = 0; // the Track Delay section's height in the Mixer View, 0 while it is hidden
    std::size_t position_ = 0; // the track's place, 1-based: what the activator shows
    std::size_t group_ = 0;    // its group's place, 1-based; 0 at the top
    bool dragging_ = false, selected_ = false;
    int resizeFrom_ = -1; // the track's height when its split line was pressed; -1 while not resizing
    bool onSplitLine(const juce::MouseEvent &) const noexcept;
    // The engine's scalar meter for the track (UI-ARCHITECTURE §9; `StripNode::meter()`, #381),
    // written per callback by the audio thread and read here with relaxed loads; null until the
    // session has a strip for the track. The strips live as long as the session (ADR-0122's
    // rule), and the session as long as the root that holds this panel.
    const engine::StripMeter *meter_ = nullptr;
    // What the meter draws after the window's ballistics: the level, falling once the engine's
    // value drops, and the peak line, held and then falling; the clock readings they need.
    float peak_ = 0, rms_ = 0;
    double lastFrame_ = -1, peakSince_ = 0;
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
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    void contextMenu(juce::Point<int>);
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
    // Where a strip's meter reads from: the engine's {peak, rms} per strip (UI-ARCHITECTURE §9;
    // `Session::strips().stripFor(track)->meter()`, #381), written by the audio thread and read
    // with relaxed loads, never a tap. Null for a track the session has no strip for yet.
    std::function<const engine::StripMeter *(std::int64_t)> meter;
    // The ballistics' clock, in seconds; JUCE's while unset (the tests set their own).
    std::function<double()> clock;
    // Auto Gain Stage, the one offline command (ADR-0195 d4, ADR-0203 d14): LUFS or RMS.
    std::function<bool(bool lufs)> autoGain;
    const std::vector<std::unique_ptr<MixerStrip>> &strips() const noexcept { return strips_; }

  private:
    void place();
    void placeColumns();
    // Live's frame above the rows: the band, and H and W beside the Overview's end.
    void paintTop(juce::Graphics &) const;
    bool vertical_;
    int scrollX_ = 0;
    std::vector<std::unique_ptr<MixerStrip>> strips_;
};
// ADI's addition (ADR-0080; the director, 28 October 2026): small arrows that flip the
// browser and the track controls between the window's sides; "Flip sides" on hovering. Lit
// while the track controls are on the left.
class FlipButton final : public juce::Button {
  public:
    FlipButton();
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// Bitwig's Use Large Track Height (Master Reference 10, Figure 33; the director, 28 October
// 2026): its icon of two columns of bars, lit while every track shows at its own height.
class TrackHeightButton final : public juce::Button {
  public:
    TrackHeightButton();
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// One of Live's round selectors at the strip's foot (manual 18.1: "the Mixer Section selectors ...
// duplicate the View Menu commands"): its letters in a disc, lit while its part shows.
class SelectorButton final : public juce::Button {
  public:
    SelectorButton(const juce::String &letters, const juce::String &tooltip);
    void paintButton(juce::Graphics &, bool over, bool down) override;
};
// Live's strip down the arrangement's outer edge (Live 11, measured 2026-10-27): its switches
// between Arrangement and Session View at its head, the flip arrows and the track height
// under them, the In/Out, Returns, Mixer and Delay selectors at its foot. Flipped, it moves to
// the window's left edge, so the track controls meet the lanes (the director, 28 October 2026).
// In/Out, Returns and Delay switch their View menu toggles (Delay the track controls' Track Options,
// Live's Track Delay); Mixer stays lit, as the track controls' mixer has no toggle.
class SideStrip final : public juce::Component {
  public:
    SideStrip();
    void paint(juce::Graphics &) override;
    void resized() override;
    FlipButton flip;
    TrackHeightButton tracks;
    SelectorButton inOut{"I-O", "In/Out: show or hide the track controls' In/Out (Ctrl/Cmd+Alt+I)."},
        returns{"R", "Return Tracks: show or hide the return tracks (Ctrl/Cmd+Alt+R)."},
        mixer{"M", "Mixer: the track controls' mixer."},
        delay{"D", "Track Delay: show or hide the track controls' Track Delay (Live's Track Options)."};
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
