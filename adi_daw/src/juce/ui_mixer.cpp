// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixer.hpp"
#include "adi/engine/midi_input_route.hpp"
#include "ui_controls.hpp"
#include "ui_shell.hpp"
#include "ui_value_entry.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/mixer.hpp"
#include "adi/ui/automation_mode.hpp"
#include "adi/ui/automation_override.hpp"
#include "adi/ui/cue_edits.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/send_edits.hpp"
#include "adi/ui/track_edits.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <span>
#include <utility>
namespace adi::ui {
namespace {
// The meter's ballistics, the window's (UI-ARCHITECTURE §9: the engine writes {peak, rms} per
// callback and holds nothing between reads, so the fall is what keeps a value on screen between
// frames). The level rises at once and falls kFallDbPerSecond; the peak line rises at once, holds
// kPeakHoldSeconds and then falls at the same rate. win's placement until Live 12's are read
// (Readings M1-02 to M1-04).
constexpr double kFallDbPerSecond = 24, kPeakHoldSeconds = 1;
// A note shorter than a window frame still needs a visible flash.
constexpr double kMidiPulseSeconds = .10;
// `shown` after `seconds` of falling at the rate, never under `now`, the engine's value; silence
// once under the meter's floor.
float fallen(float shown, float now, double seconds) noexcept {
    if (now >= shown)
        return now;
    const double db = 20 * std::log10(static_cast<double>(shown)) - kFallDbPerSecond * seconds;
    const auto level = static_cast<float>(std::pow(10., db / 20));
    return level <= 1.e-6f ? now : std::max(now, level);
}
// The Solo's half-coloured state (look::LiveLook draws it), named once.
const juce::Identifier kHalfLit{"live.half"};
const juce::Identifier kButtonLit{"adi.lit"};
const juce::var kAccentButtonLit{juce::String("accent")}, kValueButtonLit{juce::String("value")};
// Live's fader runs from -inf to +6 dB; ADI's bottom stop is -inf. What the bottom stop writes is
// silence as the format defines it, at or below -150 dB (SPEC §6.9), never the -70 dB it stands
// on: -70 dB is quiet, not silent.
constexpr double kFloorDb = -70, kTopDb = 6, kSilenceDb = -150;
double writtenVolume(double db) noexcept { return db <= kFloorDb ? kSilenceDb : db; }
// R13 will settle the reset gestures for Live's fader and sends.
constexpr bool kMixerResetOnDoubleClick = true;
constexpr bool kMixerResetOnDelete = true;

std::optional<double> levelValue(juce::String text, double floor) {
    text = text.trim().toLowerCase();
    if (text.endsWith("db"))
        text = text.dropLastCharacters(2).trimEnd();
    if (text == "-inf" || text == "-infinity" || text == juce::String::fromUTF8("−∞"))
        return floor;
    return valueNumber(text);
}

std::optional<double> panValue(juce::String text) {
    text = text.trim().toUpperCase();
    if (text == "C")
        return 0;
    const bool left = text.endsWithChar('L'), right = text.endsWithChar('R');
    const auto parsed = valueNumber(left || right ? text.dropLastCharacters(1) : text);
    if (!parsed)
        return std::nullopt;
    return (left ? -std::abs(*parsed) : right ? std::abs(*parsed) : *parsed) / 50;
}
// Live 11's meter, measured: +6 dB at the top, then 0, -12, -24, -36, -48 and -60 dB
// at these depths; the scale is wider near 0 dB, as Live's.
constexpr std::array<std::pair<double, double>, 7> kScale{
    {{6, 0}, {0, .06}, {-12, .28}, {-24, .5}, {-36, .71}, {-48, .87}, {-60, 1}}};
// One of Live's In/Out choosers: the shared chooser (language.png part D), its label and its arrow.
// A dimmed one keeps its label dim; an empty one has no arrow: the button's box alone.
void drawChooser(juce::Graphics &g, juce::Rectangle<float> r, const juce::String &label, bool enabled) {
    if (label.isEmpty()) {
        controls::button(g, r, controls::ButtonState::Rest);
        return;
    }
    controls::chooser(g, r, label, look::fontBody, enabled);
}
constexpr int kCuePairCount = engine::OutputChannelMap::maximumChannels / 2;
const juce::String &cuePairLabel(int first) {
    // The compact button uses channel numbers. Build the labels before its first frame.
    static const auto labels = [] {
        std::array<juce::String, kCuePairCount> pairs;
        for (int pair = 0; pair < kCuePairCount; ++pair) {
            const auto channel = pair * 2 + 1;
            pairs[static_cast<std::size_t>(pair)] = juce::String(channel) + "/" + juce::String(channel + 1);
        }
        return pairs;
    }();
    return labels[static_cast<std::size_t>(std::clamp(first / 2, 0, kCuePairCount - 1))];
}
class InputMenuReading {
  public:
    explicit InputMenuReading(std::shared_ptr<ExternalInputMeter> meter) : meter_(std::move(meter)) {
        meter_->beginReading();
    }
    ~InputMenuReading() { meter_->endReading(); }
    InputMenuReading(const InputMenuReading &) = delete;
    InputMenuReading &operator=(const InputMenuReading &) = delete;

  private:
    std::shared_ptr<ExternalInputMeter> meter_;
};

class InputMenuRow final : public juce::PopupMenu::CustomComponent, private juce::Timer {
  public:
    InputMenuRow(const ExternalAudioInput &input, std::shared_ptr<ExternalInputMeter> meter, bool selected)
        : CustomComponent(true), name_(juce::String::fromUTF8(input.label.c_str())),
          channels_(input.channels), meter_(std::move(meter)), selected_(selected) {
        if (meter_)
            startTimerHz(20);
    }
    void getIdealSize(int &width, int &height) override {
        width = 224;
        height = 22;
    }
    void paint(juce::Graphics &g) override {
        if (isItemHighlighted())
            g.fillAll(look::field.brighter(0.15f));
        auto bounds = getLocalBounds().toFloat().reduced(6.f, 4.f);
        const auto mark = bounds.removeFromLeft(14.f);
        if (selected_) {
            g.setColour(look::accent);
            g.fillEllipse(juce::Rectangle<float>(5.f, 5.f).withCentre(mark.getCentre()));
        }
        if (meter_ && !channels_.empty()) {
            const auto meters = bounds.removeFromRight(54.f).reduced(3.f, 1.f);
            const auto band = meters.getHeight() / static_cast<float>(channels_.size());
            for (std::size_t index = 0; index < channels_.size(); ++index) {
                const auto lane = meters.withY(meters.getY() + static_cast<float>(index) * band)
                                          .withHeight(band - 1.f);
                const auto peak = meter_->peak(channels_[index]);
                const auto db = 20. * std::log10(static_cast<double>(std::max(1.e-6f, peak)));
                const auto fill = static_cast<float>(std::clamp(1. - meterDepth(db), 0., 1.));
                g.setColour(look::display);
                g.fillRect(lane);
                g.setColour(peak >= 1.f ? look::record : look::meterLevel);
                g.fillRect(lane.withWidth(lane.getWidth() * fill));
            }
        }
        g.setColour(look::text);
        g.setFont(look::font(look::fontBody));
        g.drawText(name_, bounds, juce::Justification::centredLeft);
    }

  private:
    void timerCallback() override {
        if (isShowing())
            repaint();
    }
    juce::String name_;
    std::vector<std::uint32_t> channels_;
    std::shared_ptr<ExternalInputMeter> meter_;
    bool selected_;
};

// Live's monitoring switch: In, Auto and Off, one of three (language.png part D) across the span its
// three boxes took (16, 23 and 19 wide, `gap` apart), the track's choice lit (MX-09).
const juce::StringArray &monitorLabels() {
    static const juce::StringArray labels{"In", "Auto", "Off"};
    return labels;
}
juce::Rectangle<int> monitorBox(int x, int y, int gap) noexcept {
    return {x, y, 16 + 23 + 19 + 2 * gap, 13};
}
int monitorIndex(Monitor m) noexcept { return m == Monitor::In ? 0 : m == Monitor::Auto ? 1 : 2; }
// The choice whose cell holds `at`, if any.
std::optional<Monitor> monitorCellAt(juce::Rectangle<float> box, juce::Point<int> at) {
    if (!box.contains(at.toFloat()))
        return std::nullopt;
    for (int i = 0; i < 3; ++i)
        if (controls::oneOfCell(box, monitorLabels(), i).contains(at.toFloat()))
            return i == 0 ? Monitor::In : i == 1 ? Monitor::Auto : Monitor::Off;
    return std::nullopt;
}
} // namespace
PrimaryButtonSlider::PrimaryButtonSlider() = default;
PrimaryButtonSlider::~PrimaryButtonSlider() = default;

void PrimaryButtonSlider::enableValueEdits(double defaultValue, double step) {
    default_ = defaultValue;
    step_ = step;
    setWantsKeyboardFocus(true);
    // While clicked, the strip takes Esc for a live drag. Give the control
    // the keyboard on release so the same click also enables typed entry.
    setMouseClickGrabsKeyboardFocus(false);
    setTextBoxIsEditable(false);
    setVelocityModeParameters(1.0, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
}

void PrimaryButtonSlider::mouseUp(const juce::MouseEvent &event) {
    if (!std::exchange(primary_, false))
        return;
    const SafePointer<PrimaryButtonSlider> alive(this);
    juce::Slider::mouseUp(event);
    if (alive && alive->default_)
        focusIfShowing(*alive);
}

void PrimaryButtonSlider::mouseDoubleClick(const juce::MouseEvent &event) {
    if (event.mods.isPopupMenu() || !isEnabled())
        return;
    if (default_ && kMixerResetOnDoubleClick)
        setValue(*default_, juce::sendNotificationSync);
    else
        juce::Slider::mouseDoubleClick(event);
}

bool PrimaryButtonSlider::keyPressed(const juce::KeyPress &key) {
    if (!isEnabled())
        return false;
    if (!default_ || key.getModifiers().isAnyModifierKeyDown())
        return juce::Slider::keyPressed(key);
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        if (kMixerResetOnDelete)
            setValue(*default_, juce::sendNotificationSync);
        return true;
    }
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
        setValue(getValue() + (key == juce::KeyPress::upKey ? step_ : -step_), juce::sendNotificationSync);
        return true;
    }
    const auto first = valueEntryStart(key);
    if (key != juce::KeyPress::returnKey && first == 0)
        return juce::Slider::keyPressed(key);
    if (!valueEntry_)
        valueEntry_ = std::make_unique<ValueEntry>(getTitle(), [this](const juce::String &text) {
            if (isEnabled())
                setValue(getValueFromText(text), juce::sendNotificationSync);
        });
    const auto bounds = getLocalBounds().withHeight(std::min(getHeight(), 20)).withCentre(getLocalBounds().getCentre());
    valueEntry_->open(*this, bounds, getTextFromValue(getValue()), first);
    return true;
}

double meterDepth(double db) noexcept {
    if (db >= kScale.front().first)
        return 0;
    for (std::size_t i = 1; i < kScale.size(); ++i)
        if (db >= kScale[i].first) {
            const auto [db0, d0] = kScale[i - 1];
            const auto [db1, d1] = kScale[i];
            return d0 + (db - db0) / (db1 - db0) * (d1 - d0);
        }
    return 1;
}
double meterLevel(double depth) noexcept {
    if (depth <= 0)
        return kScale.front().first;
    for (std::size_t i = 1; i < kScale.size(); ++i)
        if (depth <= kScale[i].second) {
            const auto [db0, d0] = kScale[i - 1];
            const auto [db1, d1] = kScale[i];
            return db0 + (depth - d0) / (d1 - d0) * (db1 - db0);
        }
    return kFloorDb;
}
PeakHold::PeakHold() : juce::Button("Peak Level") {
    setTitle("Peak Level");
    setTooltip("The highest peak since you last clicked here. Click to reset it.");
    onClick = [this] {
        held = 0;
        repaint();
    };
}
void PeakHold::paintButton(juce::Graphics &g, bool over, bool down) {
    // The shared button (language.png part B): lit in the record's red once the track has clipped.
    controls::button(g, getLocalBounds().toFloat(),
                     controls::stateOf(held >= 1, over, down, isEnabled(), controls::Lit::Record),
                     held <= 1.e-6f ? juce::String("-Inf") : juce::String(20 * std::log10(held), 1));
}
MixerStrip::MixerStrip(MixerPanel &panel) : cueOut(cuePairLabel(0)), panel_(panel) {
    // Focus is explicit for the channel's title menu and for Esc during a live drag (MX-23).
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(false);
    addChildComponent(monitor_);
    if (panel.vertical()) {
        // Live's Mixer View: the fader is the ◁ beside the meter, on the meter's scale.
        volume.setSliderStyle(juce::Slider::LinearVertical);
        volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volume.getProperties().set("live.meterFader", true);
        volume.getProperties().set("live.faderX", 15);
        volume.getProperties().set("live.faderReach", kFaderReach);
        volume.setColour(juce::Slider::textBoxTextColourId, look::text);
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(peakHold);
        // A click resets the track's hold, not only this strip's.
        peakHold.onClick = [this] {
            peakHold.held = 0;
            if (const auto at = panel_.peakLevels.find(track_); at != panel_.peakLevels.end())
                at->second = 0;
            peakHold.repaint();
        };
        active.getProperties().set("live.font", static_cast<double>(look::fontTitle)); // Live's big number in its mixer
    } else {
        // Live's value boxes: the number in the box, the volume's filled in blue. Dragged up or down, as
        // Live's value fields are (manual 6.3, 7.1): JUCE's vertical bar, filled across all the same (LiveLook).
        for (auto *s : {&volume, &pan}) {
            s->setSliderStyle(juce::Slider::LinearBarVertical);
            s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 0, 0);
            s->setSliderSnapsToMousePosition(false);
            s->setColour(juce::Slider::textBoxTextColourId, look::text);
        }
    }
    pan.getProperties().set("live.bipolar", true);
    // A fader drag moves from where it is, not to where it was clicked: a click alone changes nothing.
    volume.setSliderSnapsToMousePosition(false);
    if (panel.vertical())
        volume.setNormalisableRange(juce::NormalisableRange<double>(
            kFloorDb, kTopDb,
            // The foot is the floor itself, so the fader reaches silence and -70 maps back to the foot.
            [](double, double, double proportion) { return proportion <= 0 ? kFloorDb : meterLevel(1 - proportion); },
            [](double, double, double db) { return 1 - meterDepth(db); }));
    else {
        volume.setRange(kFloorDb, kTopDb, .01);
        // Live's 0 dB sits at 83 % of the box, not at the linear 92 %.
        volume.setSkewFactor(2.27);
    }
    volume.textFromValueFunction = [](double v) {
        if (v <= kFloorDb)
            return juce::String("-inf");
        return std::abs(v - std::round(v)) < .005 ? juce::String(static_cast<int>(std::lround(v)))
                                                  : juce::String(v, 1);
    };
    volume.updateText();
    volume.setTitle("Track Volume");
    volume.setTooltip("Adjusts the track's output level.");
    volume.valueFromTextFunction = [this](const juce::String &text) {
        return levelValue(text, kFloorDb).value_or(volume.getValue());
    };
    volume.enableValueEdits(0, .1);
    pan.setRange(-1, 1, .02);
    // Live's pan reads 50L to 50R, C in the middle.
    pan.textFromValueFunction = [](double v) {
        const auto n = static_cast<int>(std::lround(std::abs(v) * 50));
        return n == 0 ? juce::String("C") : juce::String(n) + (v < 0 ? "L" : "R");
    };
    pan.updateText();
    // Double-clicking the Pan control resets it to its default (manual 18.1).
    pan.setDoubleClickReturnValue(true, 0);
    pan.setTitle("Track Panning");
    pan.setTooltip("Places the track in the stereo field. Right-click for the pan mode.");
    pan.valueFromTextFunction = [this](const juce::String &text) { return panValue(text).value_or(pan.getValue()); };
    pan.enableValueEdits(0, .02);
    for (auto *s : {&volume, &pan}) {
        addAndMakeVisible(*s);
        // The wheel only scrolls, as Live (lookfeel.wheelChangesParameters).
        s->setScrollWheelEnabled(false);
    }
    // A drag is heard while it moves and is one undo step when the mouse lets go (MX-23, SRF-02): the
    // document's live gesture, the selected tracks together when this one is among them. Typed and
    // double-click changes commit at once. Without the live gesture (outside the application) the drag
    // commits on release, as before.
    volume.onDragStart = [this] {
        dragging_ = true;
        beginLive(gesture::ScalarKind::TrackVolume, track_, {kFloorDb, kTopDb, kSilenceDb}, "Change Track Volume");
    };
    pan.onDragStart = [this] {
        dragging_ = true;
        beginLive(gesture::ScalarKind::TrackPan, track_, {-1, 1, std::nullopt}, "Change Track Panning");
    };
    volume.onDragEnd = [this] {
        dragging_ = false;
        if (!endLive())
            panel_.submit("mixer.setVolume", {{"id", track_}, {"db", writtenVolume(volume.getValue())}},
                          "Change Track Volume");
    };
    pan.onDragEnd = [this] {
        dragging_ = false;
        if (!endLive())
            panel_.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}}, "Change Track Panning");
    };
    volume.onValueChange = [this] {
        if (live_)
            panel_.root.mixerDrag(volume.getValue());
        else if (!dragging_)
            panel_.submit("mixer.setVolume", {{"id", track_}, {"db", writtenVolume(volume.getValue())}},
                          "Change Track Volume");
    };
    pan.onValueChange = [this] {
        if (live_)
            panel_.root.mixerDrag(pan.getValue());
        else if (!dragging_)
            panel_.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}}, "Change Track Panning");
    };
    // Live's Track Delay (manual 18.7): a value box reading the delay in ms to two places, Live's
    // "ms" box beside it (paintDelayUnit); a negative value pre-delays. Hidden until Live's Track
    // Options shows it (resized). Dragged up or down, as Live's value fields are (manual 6.3, the
    // Arrangement Position fields; 7.1, the scene fields): JUCE's vertical bar, drawn as the number
    // alone (LiveLook, `live.valueField`), a pixel 0.1 ms, PROVISIONAL until Live's step is
    // measured (M1-06). Its travel is the engine's playable second either way (ADR-0172,
    // `StripNode::kMaxDelaySeconds`); Live's own range is to be measured.
    delay.setSliderStyle(juce::Slider::LinearBarVertical);
    delay.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 0, 0);
    delay.setSliderSnapsToMousePosition(false);
    delay.setColour(juce::Slider::textBoxTextColourId, look::text);
    delay.getProperties().set("live.valueField", true);
    delay.setRange(-1000, 1000, .01);
    delay.setMouseDragSensitivity(kDelayDragPixels);
    delay.textFromValueFunction = [](double v) { return juce::String(v, 2); };
    // Typed with or without its unit: "10", "10 ms", "-2.5ms".
    delay.valueFromTextFunction = [this](const juce::String &text) {
        auto number = text.trim();
        if (number.endsWithIgnoreCase("ms"))
            number = number.dropLastCharacters(2);
        return valueNumber(number).value_or(delay.getValue());
    };
    delay.updateText();
    keepLatency.setTitle("Keep Latency");
    keepLatency.setTooltip("Keep the monitored timing of audio takes outside loop recording when Monitor is In or Auto. "
                           "Turn off to apply the driver's latency correction.");
    keepLatency.setClickingTogglesState(false);
    keepLatency.onClick = [this] { panel_.root.setKeepMonitoringLatency(track_, !keepLatency.getToggleState()); };
    addChildComponent(keepLatency);
    delay.setTitle("Track Delay");
    setDelayTooltip("Delays the track's output by this many milliseconds; a negative value pre-delays it.");
    delay.setScrollWheelEnabled(false);
    delay.enableValueEdits(0, .1);
    delay.onDragStart = [this] { dragging_ = true; };
    delay.onDragEnd = [this] {
        dragging_ = false;
        commitDelay();
    };
    delay.onValueChange = [this] {
        if (!dragging_)
            commitDelay();
    };
    addChildComponent(delay);
    cueVolume.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    cueVolume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    cueVolume.setRange(kCueFloorDb, kCueTopDb, .01);
    cueVolume.setSkewFactor(2.27); // as the volume's: the top of the travel finer than the foot
    cueVolume.textFromValueFunction = [](double v) {
        return v <= kCueFloorDb ? juce::String("-inf") : juce::String(v, 1) + " dB";
    };
    cueVolume.setValue(0, juce::dontSendNotification);
    cueVolume.setTitle("Cue Volume");
    cueVolume.setTooltip("The level of cued tracks and the metronome at Cue Out.");
    cueVolume.setScrollWheelEnabled(false);
    cueVolume.valueFromTextFunction = [this](const juce::String &text) {
        return levelValue(text, kCueFloorDb).value_or(cueVolume.getValue());
    };
    cueVolume.enableValueEdits(0, .1);
    cueVolume.onDragStart = [this] { cueDragging_ = true; };
    cueVolume.onDragEnd = [this] {
        cueDragging_ = false;
        panel_.root.setCueVolume(cueVolume.getValue());
    };
    cueVolume.onValueChange = [this] {
        if (!cueDragging_)
            panel_.root.setCueVolume(cueVolume.getValue());
    };
    addChildComponent(cueVolume);
    cueOut.setTitle("Cue Out");
    cueOut.setTooltip("Chooses the stereo output pair for cued tracks and the metronome.");
    cueOut.getProperties().set("live.small", true);
    cueOut.onClick = [this] { cueOutMenu(); };
    addChildComponent(cueOut);
    active.setTitle("Track Activator");
    active.setTooltip("Turns the track's output on or off.");
    active.getProperties().set(kButtonLit, kAccentButtonLit);
    solo.setTitle("Solo");
    solo.setTooltip("Solos the track.");
    arm.setTitle("Arm Recording");
    arm.setTooltip("Arms the track for recording.");
    for (juce::TextButton *b : {static_cast<juce::TextButton *>(&active), static_cast<juce::TextButton *>(&solo),
                                static_cast<juce::TextButton *>(&arm)}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, look::button);
        // Only the keyboard focuses a mixer switch, so a focused one is ringed (Live 12).
        b->setMouseClickGrabsKeyboardFocus(false);
        b->getProperties().set("live.focusRing", true);
    }
    // Live's switches when on: the activator orange, Solo blue, Arm red.
    solo.setColour(juce::TextButton::buttonOnColourId, look::value);
    solo.getProperties().set("adi.lit", "value");
    solo.getProperties().set(kHalfLit, false);
    arm.setColour(juce::TextButton::buttonOnColourId, look::record);
    arm.getProperties().set("adi.lit", "record");
    pan.addMouseListener(this, true);
    volume.addMouseListener(this, true); // the inline bar's value field is a child Label
    active.setClickingTogglesState(false);
    // Live's Track Activator (manual 18.1): the selected tracks together when this one is among them.
    active.onClick = [this] {
        auto &root = panel_.root;
        if (root.automationMode() && kind_ != "master")
            root.showAutomation(track_, activatorTarget(track_));
        root.activateTrack(track_);
    };
    active.onMenu = [this] {
        auto m = activatorMenuItems();
        if (m.getNumItems() > 0)
            m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&active));
    };
    // Live's Solo (manual 18.1): exclusive unless Ctrl/Cmd is held; the selected tracks together.
    // With Options > Cue Switches it is Live's Cue button (manual 18.6; MX-08): the track to Cue Out.
    solo.onClick = [this] {
        if (const auto current = panel_.root.current(); current && current->cue.cueMode)
            panel_.root.cueTrack(track_);
        else
            panel_.root.soloTrack(track_, juce::ModifierKeys::currentModifiers.isCommandDown());
    };
    // Its right-click menu: Solo in Place (manual 18.6).
    solo.onMenu = [this] { soloMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&solo)); };
    // Live's Arm (manual 21.1): Exclusive Arm unless Ctrl/Cmd is held; the selected tracks together.
    arm.onClick = [this] { panel_.root.armTrack(track_, juce::ModifierKeys::currentModifiers.isCommandDown()); };
    // A return's Pre/Post (ADR-0203 d3), where a track has Arm: the return's own switch
    // (`TrackNode::returnSendTap`, #405), which every send into it follows, now and as each new one
    // connects. Shown on a return's row only.
    prePost.setTitle("Pre/Post");
    prePost.setTooltip("Whether this return's sends tap before or after their tracks' faders.");
    prePost.setClickingTogglesState(false);
    prePost.setColour(juce::TextButton::buttonColourId, look::button);
    prePost.setColour(juce::TextButton::buttonOnColourId, look::accent);
    prePost.setMouseClickGrabsKeyboardFocus(false);
    prePost.getProperties().set("live.focusRing", true);
    prePost.getProperties().set("live.font", static_cast<double>(look::fontLabel));
    prePost.onClick = [this] { throwPrePost(track_); };
    addChildComponent(prePost);
}
void ArmButton::paintButton(juce::Graphics &g, bool over, bool down) {
    getLookAndFeel().drawButtonBackground(g, *this, findColour(buttonColourId), over, down);
    // Live's Arm switch: a dark dot, light on red when armed. The ink, not the band, when unarmed: the
    // shared button's pressed fill is the band's grey.
    g.setColour(getToggleState() ? look::text : look::ink);
    g.fillEllipse(getLocalBounds().toFloat().withSizeKeepingCentre(8, 8));
}
void MixerStrip::bind(std::int64_t id) {
    if (track_ != id && monitor_.hasKeyboardFocus(false))
        monitor_.giveAwayKeyboardFocus();
    track_ = id;
    position_ = number_ = 0;
    levels_ = {};
    lastFrame_ = -1;
    midiOnsets_ = 0;
    midiPulseUntil_ = 0;
    midiLit_ = false;
    // The Peak Level is the track's: what it held on the strip that showed it before, else -Inf.
    const auto held = panel_.peakLevels.find(id);
    peakHold.held = held != panel_.peakLevels.end() ? held->second : 0.f;
    peakHold.repaint();
    automation_.clear(); // the LEDs are the new track's
    frame(true);
}
void MixerStrip::resolveMeter() { meter_ = panel_.meter && track_ != 0 ? panel_.meter(track_) : nullptr; }
void MixerStrip::setDelayTooltip(const juce::String &tip) {
    // The box's value label covers the slider and keeps the tooltip it had when it was made: both get it.
    delay.setTooltip(tip);
    for (auto *c : delay.getChildren())
        if (auto *label = dynamic_cast<juce::Label *>(c))
            label->setTooltip(tip);
}
void MixerStrip::frame(bool changed) noexcept {
    const auto *t = panel_.root.reader().findTrack(track_);
    // Live makes Track Delay unavailable while Options > Delay Compensation is off (manual 18.7;
    // MX-04); its value stays as stored, and comes back with the switch.
    if (const bool compensated = panel_.root.commands().delayCompensation(); delay.isEnabled() != compensated) {
        delay.setEnabled(compensated);
        setDelayTooltip(compensated ? "Delays the track's output by this many milliseconds; a negative value pre-delays it."
                                    : "Track Delay is unavailable while Options > Delay Compensation is off.");
        repaint(); // its "ms" box and title follow
    }
    // A device put on a MIDI track or taken off it may give it an audio mixer or take it away.
    const auto devices = panel_.root.devices.publication ? panel_.root.devices.publication() : nullptr;
    if (changed || devices.get() != devices_) {
        devices_ = devices.get();
        syncSends(devices.get());
    }
    if (changed) {
        // The Track Activator shows the track's number, its place in the arrangement's order,
        // as Live's (and as a # in its name does); the text changes only when it moves.
        const auto rows = panel_.root.arrangement.geometry.rows(panel_.root.reader());
        if (const auto at = rows.find(track_); at < rows.size()) {
            position_ = at + 1;
            number_ = rows[at].number();
            // A return's activator shows its letter, as Live's ("A", "B"); a track's its number. The
            // text changes only when the label does, so a steady frame allocates nothing.
            // A group's return is lettered with the group's initial first ("DA"; ADR-0247 d2), coded
            // here as a negative number so a steady frame compares and allocates nothing.
            std::array<char, 2> letter{};
            const auto letters = kind_ == "return" ? returnLetter(panel_.root.reader().tracks(), track_, letter) : 0;
            const int label = letters > 0 ? -1 - (letter[0] * 128 + (letters > 1 ? letter[1] : 0))
                                          : static_cast<int>(number_);
            if (label != label_) {
                label_ = label;
                active.setButtonText(letters > 0 ? juce::String(letter.data(), letters) : juce::String(label));
            }
            const auto parent = rows[at].place.parent;
            const auto group = parent == 0 ? rows.size() : rows.find(parent);
            if (const auto place = group < rows.size() ? group + 1 : 0; place != group_) {
                group_ = place;
                repaint();
            }
        }
    }
    // The track's arm and monitoring as the session's rows hold them (MX-09): lit when they change.
    if (const auto record = panel_.root.recordState(track_); record != record_) {
        if ((record.monitor == Monitor::In) != (record_.monitor == Monitor::In)) {
            active.getProperties().set(kButtonLit, record.monitor == Monitor::In ? kValueButtonLit : kAccentButtonLit);
            active.repaint();
        }
        record_ = record;
        arm.setToggleState(record.armed, juce::dontSendNotification);
        monitor_.repaint();
    }
    // The selected tracks' controls are lighter, as Live's, a selection across tracks lighting each.
    const bool selected = track_ != 0 && panel_.root.arrangement.geometry.inSelection(panel_.root.reader(), track_);
    if (selected != selected_) {
        selected_ = selected;
        repaint();
    }
    if (changed && kind_ == "master")
        if (const auto *snapshot = panel_.root.reader().get()) {
            if (!cueDragging_)
                cueVolume.setValue(std::max(kCueFloorDb, snapshot->cue.volumeDb), juce::dontSendNotification);
            if (!panel_.vertical() && cueFirstChannel_ != snapshot->cue.firstOutputChannel) {
                cueFirstChannel_ = snapshot->cue.firstOutputChannel;
                cueOut.setButtonText(cuePairLabel(cueFirstChannel_));
            }
        }
    if (changed && t && !dragging_) {
        volume.setValue(std::max(kFloorDb, static_cast<double>(t->volumeDb)),
                        juce::dontSendNotification);
        pan.setValue(t->pan, juce::dontSendNotification);
        // The delay as the sample grid keeps it, in ms at the project's rate (manual 18.7).
        delay.setValue(trackDelayMs(t->delaySamples, panel_.root.reader().sampleRate()), juce::dontSendNotification);
        keepLatency.setToggleState(t->recording.keepMonitoringLatency, juce::dontSendNotification);
        active.setToggleState(!t->muted, juce::dontSendNotification);
        // Solo, or with Cue Switches the track's Cue (manual 18.6; MX-08): "C" until the window lane
        // draws Live's headphones.
        const bool cueMode = panel_.root.reader().get() && panel_.root.reader().get()->cue.cueMode;
        if (cueMode_ != cueMode) {
            cueMode_ = cueMode;
            solo.setButtonText(cueMode ? "C" : "S");
            solo.setTitle(cueMode ? "Cue" : "Solo");
            solo.setTooltip(cueMode ? "Cues the track: it plays to Cue Out, for headphones, without soloing it."
                                    : "Solos the track.");
        }
        solo.setToggleState(cueMode ? panel_.root.reader().get() && cued(*panel_.root.reader().get(), track_)
                                    : t->soloed,
                            juce::dontSendNotification);
        // A group holding a soloed track shows its Solo half coloured (Live 12 manual 18.3). The
        // property is set once at construction, so a frame only changes its value.
        // In Cue mode the button is the Cue button, and solos are not heard: no half colour from them.
        const bool half = !cueMode && kind_ == "group" && !t->soloed && holdsSoloed(panel_.root.reader().tracks(), track_);
        if (static_cast<bool>(solo.getProperties()[kHalfLit]) != half) {
            solo.getProperties().set(kHalfLit, half);
            solo.repaint();
        }
        // Each send at its row's level, fully down without one; an off one dimmed, and its clicks
        // go to the strip, which offers Enable Send.
        const auto &rows = panel_.root.reader().sends();
        for (std::size_t i = 0; i < sends_.size(); ++i) {
            const auto *route = findSend(rows, track_, returns_[i]);
            sends_[i]->setValue(std::clamp(sendLevel(route), kSendFloorDb, kSendTopDb), juce::dontSendNotification);
            const bool on = adi::ui::sendOn(*t, route);
            sendOn_[i] = static_cast<char>(on ? 1 : 0);
            sends_[i]->setAlpha(on ? 1.f : .45f);
            sends_[i]->setInterceptsMouseClicks(on, false);
        }
        // Pre/Post, lit while the return's sends tap after the fader: the return's own switch
        // (`TrackNode::returnSendTap`, #405), on its row and on Main's strip for each return in the
        // Mixer View.
        // Mixed, a send's own tap differing (ADR-0247 d2): a dash in an accent edge, unlit.
        const auto &reader = panel_.root.reader();
        const auto current = panel_.root.current();
        auto showPost = [&reader, &current](juce::TextButton &b, std::int64_t ret) {
            const bool mixed = current && returnTapMixed(*current, ret);
            const bool post = !mixed && !returnPre(reader.findTrack(ret));
            const char *text = mixed ? "\xe2\x80\x93" : post ? "Post" : "Pre";
            if (post != b.getToggleState() || b.getButtonText() != juce::CharPointer_UTF8(text) ||
                mixed != static_cast<bool>(b.getProperties()["live.mixed"])) {
                b.setButtonText(juce::CharPointer_UTF8(text));
                b.setToggleState(post, juce::dontSendNotification);
                b.getProperties().set("live.mixed", mixed);
                b.repaint();
            }
        };
        if (kind_ == "return")
            showPost(prePost, track_);
        for (std::size_t i = 0; i < returnSwitches_.size(); ++i)
            showPost(*returnSwitches_[i], returns_[i]);
        repaint();
    }
    // An automated control follows its lane at the playhead (Live 12 manual 25.4; walk-A A13); a drag
    // overrides the lane, so the control dragged shows the hand's value.
    if (t && !dragging_)
        followAutomation(t->volumeDb, t->pan);
    // Read each channel with relaxed loads. A strip bound before the session
    // had a strip for its track finds the meter on a refresh.
    if (changed)
        resolveMeter();
    const double now = panel_.clock ? panel_.clock() : juce::Time::getMillisecondCounterHiRes() / 1000.;
    const bool firstFrame = lastFrame_ < 0;
    const double seconds = firstFrame ? 0 : std::max(0., now - lastFrame_);
    lastFrame_ = now;
    if (!audio_) {
        const auto notes = panel_.midiActivity ? panel_.midiActivity(track_) : engine::MidiNoteActivity{};
        if (!firstFrame && notes.onsets != midiOnsets_)
            midiPulseUntil_ = now + kMidiPulseSeconds;
        midiOnsets_ = notes.onsets;
        const bool lit = notes.held || now < midiPulseUntil_;
        if (lit != midiLit_) {
            midiLit_ = lit;
            repaint(meterArea().expanded(1));
        }
        return;
    }
    const float stripPeak = meter_ != nullptr ? meter_->peak.load(std::memory_order_relaxed) : 0.f;
    // Live's peak level holds the highest peak it is shown (manual 18.1); the engine holds nothing
    // between reads, so a callback no frame read is not in it (the held peak is N9's open half).
    if (panel_.vertical() && stripPeak > peakHold.held) {
        peakHold.held = stripPeak;
        panel_.peakLevels[track_] = stripPeak; // a track's entry is made at its first rise only
        peakHold.repaint();
    }
    // The window's ballistics: the level falls from where it was drawn; the peak line holds
    // kPeakHoldSeconds from its last rise, then falls for the part of this frame past the hold.
    bool meterChanged = false;
    for (std::size_t channel = 0; channel < levels_.size(); ++channel) {
        auto &level = levels_[channel];
        const auto *readout = meter_ != nullptr ? &meter_->stereo[channel] : nullptr;
        const float peakNow = readout != nullptr ? readout->peak.load(std::memory_order_relaxed) : 0.f;
        const float rmsNow = readout != nullptr ? readout->rms.load(std::memory_order_relaxed) : 0.f;
        const float rms = fallen(level.rms, rmsNow, seconds);
        float peak = level.peak;
        if (peakNow >= level.peak) {
            peak = peakNow;
            level.peakSince = now;
        } else if (const double falling = std::min(seconds, now - level.peakSince - kPeakHoldSeconds); falling > 0)
            peak = fallen(level.peak, peakNow, falling);
        meterChanged = meterChanged || peak != level.peak || rms != level.rms;
        level.peak = peak;
        level.rms = rms;
    }
    if (meterChanged) {
        repaint(panel_.vertical() ? meterArea().expanded(1)
                                  : juce::Rectangle<int>((showsInOut() ? MixerPanel::kInOutWidth : 0) + 105, 0,
                                                         10, getHeight()));
    }
}
void MixerStrip::followAutomation(double volumeDb, double panValue) {
    const auto *snapshot = panel_.root.reader().get();
    if (snapshot == nullptr)
        return;
    if (automation_.size() != 2 + sends_.size())
        automation_.assign(2 + sends_.size(), {});
    // The playhead the engine evaluates its lanes at, playing or parked (ADR-0164).
    const auto tick = panel_.root.timelineTick();
    bool relit = false;
    const auto follow = [&](juce::Slider &control, AutomationShown &shown, std::string_view kind, std::int64_t owner,
                            std::string_view param, double low, double high, double own) {
        const auto lane = followedLane(*snapshot, kind, owner, param, tick);
        const auto state = !lane.automated ? Automation::None
                           : lane.overridden ? Automation::Overridden
                                             : Automation::Followed;
        relit = relit || state != shown.state;
        shown.state = state;
        if (lane.value)
            control.setValue(std::clamp(*lane.value, low, high), juce::dontSendNotification);
        else if (shown.showing) // overridden or gone: the control's own value again
            control.setValue(own, juce::dontSendNotification);
        shown.showing = lane.value.has_value();
    };
    follow(volume, automation_[0], "track", track_, "volume", kFloorDb, kTopDb, std::max(kFloorDb, volumeDb));
    follow(pan, automation_[1], "track", track_, "pan", -1, 1, panValue);
    // A send's lane is its routing row's (`sendTarget`).
    const auto &rows = panel_.root.reader().sends();
    for (std::size_t i = 0; i < sends_.size(); ++i) {
        const auto *route = findSend(rows, track_, returns_[i]);
        follow(*sends_[i], automation_[2 + i], "routing", route != nullptr ? route->id : 0, "gain_db", kSendFloorDb,
               kSendTopDb, std::clamp(sendLevel(route), kSendFloorDb, kSendTopDb));
    }
    if (relit)
        repaint();
}
const juce::Component *MixerStrip::automatedControl(std::size_t index) const noexcept {
    if (index == 0)
        return &volume;
    if (index == 1)
        return &pan;
    return index - 2 < sends_.size() ? sends_[index - 2].get() : nullptr;
}
MixerStrip::Automation MixerStrip::automation(const juce::Component &control) const noexcept {
    for (std::size_t i = 0; i < automation_.size(); ++i)
        if (automatedControl(i) == &control)
            return automation_[i].state;
    return Automation::None;
}
void MixerStrip::paintOverChildren(juce::Graphics &g) {
    // The LED at the control's top right, as a device knob's (`ParameterControl::paint`): red while it
    // follows its lane, dim once overridden (ADI's colours until measured, parity-7.4 row 11).
    for (std::size_t i = 0; i < automation_.size(); ++i) {
        const auto *control = automatedControl(i);
        if (automation_[i].state == Automation::None || control == nullptr || !control->isVisible())
            continue;
        g.setColour(automation_[i].state == Automation::Overridden ? look::textDim : juce::Colours::red);
        const auto b = control->getBounds();
        g.fillEllipse(static_cast<float>(b.getRight() - 6), static_cast<float>(b.getY() + 1), 5.f, 5.f);
    }
    if (dropPreview_) {
        const auto band = panel_.rowsBand() - getY();
        const auto area = getLocalBounds().getIntersection({0, band.getStart(), getWidth(), band.getLength()});
        g.setColour(look::dropTarget);
        g.drawRect(area, 2);
    }
}
void MixerStrip::resized() {
    if (panel_.vertical()) {
        // Live 11's strip, measured: the name, then
        // the mixer: the peak level at its head; pan, the activator, Solo and Arm down
        // to its foot; the meter with the fader beside them.
        const auto sections = panel_.columnSections(getHeight());
        io_ = sections.inOut;
        delay_ = sections.trackDelay;
        shown_ = sections.sends;
        layoutMonitor();
        // Live's figure: the box across the strip, the "ms" box at its right edge.
        delay.setVisible(delay_ > 0);
        delay.setBounds(4, mixerBottom() + 19, getWidth() - 32, 13);
        // Under it, on a track that records (Live's figure: none on a return or Main).
        keepLatency.setVisible(delay_ > 0 && kind_ != "master" && kind_ != "return" && kind_ != "group");
        keepLatency.setBounds(4, mixerBottom() + 36, getWidth() - 8, 14);
        layoutSends();
        // Live's Mixer: a return has its letter and Solo but no Arm; Main none of the three.
        active.setVisible(kind_ != "master");
        solo.setVisible(kind_ != "master");
        arm.setVisible(kind_ != "master" && kind_ != "return" && kind_ != "group");
        prePost.setVisible(false);
        // A MIDI track with no instrument has no mixer but its switches and its MIDI meter.
        volume.setVisible(audio_);
        pan.setVisible(audio_);
        peakHold.setVisible(audio_);
        const int top = mixerTop(), bottom = mixerBottom();
        peakHold.setBounds(11, top + 5, 35, 12);
        pan.setBounds(15, bottom - 91, 27, 27);
        active.setBounds(18, bottom - 59, 21, 21);
        cueVolume.setVisible(kind_ == "master");
        cueVolume.setBounds(15, bottom - 62, 27, 27);
        solo.setBounds(17, bottom - 33, 23, 12);
        arm.setBounds(17, bottom - 16, 23, 12);
        const bool tall = getHeight() >= MixerPanel::kTallHeight;
        if (tall != (volume.getTextBoxPosition() == juce::Slider::TextBoxBelow))
            volume.setTextBoxStyle(tall ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                                   false, tall ? 40 : 0, tall ? 15 : 0);
        // The fader's travel is the meter's height, on its scale; the control reaches past both ends by
        // kFaderReach so the ◁ is pressed wherever it is drawn, at +6 dB too (LiveLook::getSliderLayout).
        const auto meter = meterArea();
        volume.setBounds(meter.getX() - 2, meter.getY() - kFaderReach, tall ? 40 : 26,
                         meter.getHeight() + kFaderReach + (tall ? 15 : kFaderReach));
        return;
    }
    // Live's rows: 13 pt boxes on a 17 pt pitch, 3 pt from the track's top. A row
    // shows only when the track is tall enough for it.
    layoutMonitor();
    const int x = showsInOut() ? MixerPanel::kInOutWidth : 0;
    const int rows = std::max(1, (getHeight() - 1) / 17);
    const bool levels = audio_ && panel_.root.state().trackMixer;
    auto place = [rows](juce::Component &c, int row, juce::Rectangle<int> r, bool shown = true) {
        c.setVisible(shown && row < rows);
        c.setBounds(r);
    };
    if (kind_ == "master") {
        // Live's Main row: no activator, Solo or Arm; its volume where a track has its activator.
        for (juce::Component *c : std::initializer_list<juce::Component *>{&active, &solo, &arm, &prePost})
            c->setVisible(false);
        place(volume, 0, {x + 4, 3, 45, 13}, levels);
        place(pan, 1, {x + 54, 20, 45, 13}, levels);
        // The 24-pixel Main row has room beside its volume and before the meter.
        const bool room = getWidth() >= x + MixerPanel::kControlsWidth;
        place(cueOut, 0, {x + 53, 3, 32, 13}, room);
        place(cueVolume, 0, {x + 85, 1, 19, 19}, room && levels);
    } else {
        cueOut.setVisible(false);
        cueVolume.setVisible(false);
        place(active, 0, {x + 4, 3, 45, 13});
        place(solo, 0, {x + 54, 3, 20, 13});
        // A return's Pre/Post where a track has Arm, as Live's ("A", "S", "Post").
        if (kind_ == "return") {
            arm.setVisible(false);
            place(prePost, 0, {x + 76, 3, 25, 13});
        } else if (kind_ == "group") {
            // A group records nothing, so it has no Arm (Live 12 manual 18.3, the group's strip).
            arm.setVisible(false);
            prePost.setVisible(false);
        } else {
            prePost.setVisible(false);
            place(arm, 0, {x + 78, 3, 21, 13});
        }
        place(volume, 1, {x + 4, 20, 35, 13}, levels);
        place(pan, 1, {x + 54, 20, 45, 13}, levels);
    }
    // Live's sends follow the mixer controls, three per row.
    const bool sends = showsSends();
    const int sendsStart = levels ? 2 : 1;
    for (std::size_t i = 0; i < sends_.size(); ++i) {
        const int row = sendsStart + static_cast<int>(i / 3), column = static_cast<int>(i % 3);
        place(*sends_[i], row, {x + 4 + 33 * column, 3 + 17 * row, 29, 13}, sends);
    }
    // Live's Track Delay on the row after the sends, with its "ms" box (paintDelayUnit), while the
    // track controls' Track Options shows it (the D selector).
    const int after = sendsStart + (sends ? sendRows() : 0);
    place(delay, after, {x + 4, 3 + 17 * after, 45, 13}, panel_.root.state().trackOptions);
}
juce::Rectangle<int> MixerStrip::sendBounds(std::size_t index) const noexcept {
    // Live's Mixer View sends: A on the left, B to its right and lower, C under A, each 22 pt under
    // the one before; the knobs a little larger than pan's.
    const int i = static_cast<int>(index);
    return {(i % 2 == 0) ? 10 : 51, sendsTop() + 6 + kSendPitch * i, 32, 32};
}
void MixerStrip::layoutSends() {
    const auto count = std::min(shown_, panel_.sendReturns_.size());
    const auto returns = std::span<const std::int64_t>(panel_.sendReturns_).subspan(panel_.firstSend(count), count);
    // Look up each control by return ID. A track outside the return's group leaves an empty slot,
    // and Main's Pre/Post switch lines up with that return on the strips that do have its knob.
    const auto place = [&](juce::Component &control, std::size_t index, bool prePostSwitch) {
        const auto at = std::find(returns.begin(), returns.end(), returns_[index]);
        if (at != returns.end()) {
            const auto b = sendBounds(static_cast<std::size_t>(at - returns.begin()));
            control.setBounds(prePostSwitch ? juce::Rectangle<int>(b.getX(), b.getY() + 9, 34, 14) : b);
        }
        control.setVisible(at != returns.end());
    };
    for (std::size_t i = 0; i < sends_.size(); ++i)
        place(*sends_[i], i, false);
    for (std::size_t i = 0; i < returnSwitches_.size(); ++i)
        place(*returnSwitches_[i], i, true);
}
juce::Rectangle<int> MixerStrip::sendArrowBounds(bool down) const noexcept {
    // Keep the arrows in the gutter past the right-hand knob and Main's slightly wider switch.
    return {getWidth() - 10, down ? mixerTop() - 16 : sendsTop() + 5, 8, 12};
}
juce::Rectangle<int> MixerStrip::meterArea() const noexcept {
    if (!panel_.vertical())
        return {(showsInOut() ? MixerPanel::kInOutWidth : 0) + 105, 1, 8, std::max(0, getHeight() - 4)};
    // Under a tall strip's numeric field, the meter stops short of the foot.
    const bool tall = getHeight() >= MixerPanel::kTallHeight;
    return {53, mixerTop() + 6, 11, std::max(0, mixerBottom() - mixerTop() - 10 - (tall ? 15 : 0))};
}
bool MixerStrip::showsSends() const noexcept {
    // The Mixer View's own Sends (Live 12's Mixer Controls), apart from the track controls'.
    return panel_.vertical() ? panel_.root.state().mixerSends : panel_.root.state().sends;
}
bool MixerStrip::showsInOut() const noexcept {
    // Shown when the View menu's In/Out is on (§42.1) and the panel is wide enough.
    return !panel_.vertical() && panel_.root.state().inOut &&
           getWidth() >= MixerPanel::kInOutWidth + MixerPanel::kControlsWidth;
}
void MixerStrip::paint(juce::Graphics &g) {
    const auto *track = panel_.root.reader().findTrack(track_);
    if (panel_.vertical()) {
        // Live's strip: grey, lighter when its track is selected; the name in the track's
        // colour; dark rules between the parts and down the right-hand edge.
        const int w = getWidth() - 2;
        g.fillAll(selected_ ? look::laneSelectedAlt : look::panel);
        g.setColour(look::separator);
        g.fillRect(w, 0, 2, getHeight());
        g.fillRect(0, kMixerTop - 2, w, 2);
        if (io_ > 0) {
            paintMixerInOut(g);
            g.setColour(look::separator);
            g.fillRect(0, sendsTop() - 2, w, 2);
        }
        if (shown_ > 0) {
            // Live's Sends section: its title at the right, each knob lettered as its return in the
            // ring's open foot; a rule under it. A strip with neither knobs nor switches keeps the
            // section's room, untitled, so the mixers line up.
            g.fillRect(0, mixerTop() - 2, w, 2);
            g.setColour(look::text);
            g.setFont(look::font(look::fontBody));
            if (!sends_.empty() || !returnSwitches_.empty()) {
                const auto first = panel_.firstSend(shown_);
                const bool canScrollUp = first > 0, canScrollDown = first + shown_ < panel_.sendReturns_.size();
                const int gutter = canScrollUp || canScrollDown ? 8 : 0;
                g.drawText("Sends", 46, sendsTop() + 6, w - 50 - gutter, 15, juce::Justification::centredRight);
                const auto drawArrow = [&](bool down) {
                    const auto b = sendArrowBounds(down).reduced(1, 4).toFloat();
                    const float wing = down ? b.getY() : b.getBottom();
                    const float point = down ? b.getBottom() : b.getY();
                    g.drawLine(b.getX(), wing, b.getCentreX(), point, 1.f);
                    g.drawLine(b.getCentreX(), point, b.getRight(), wing, 1.f);
                };
                if (canScrollUp)
                    drawArrow(false);
                if (canScrollDown)
                    drawArrow(true);
            }
            for (std::size_t i = 0; i < sends_.size(); ++i) {
                if (!sends_[i]->isVisible())
                    continue;
                const auto b = sends_[i]->getBounds();
                g.setColour(look::text.withAlpha(sendOn_[i] != 0 ? 1.f : .45f));
                g.drawText(i < letters_.size() ? letters_[i] : juce::String(), b.getX(), b.getBottom() - 9,
                           b.getWidth(), 12, juce::Justification::centred);
            }
        }
        if (delay_ > 0) {
            // Live's Track Delay section under the mixer (manual 18.7, its figure): a band darker
            // than the mixer above it, across every strip, a rule over it; its title, the value box
            // across the strip with the lit "ms" box at its right. The band's shade is the figure's,
            // not measured (LK-03).
            const int top = mixerBottom();
            g.setColour(look::frame);
            g.fillRect(0, top, w, delay_);
            g.setColour(look::separator);
            g.fillRect(0, top, w, 2);
            g.setColour(look::text);
            g.setFont(look::font(look::fontBody, true));
            g.drawText("Track Delay", 5, top + 4, w - 10, 13, juce::Justification::centredLeft);
            paintDelayUnit(g);
        }
        if (position_ > 0) {
            g.setColour(panel_.root.arrangement.colourFor(position_ - 1));
            g.fillRect(0, 0, w, kTitleHeight);
            // A track in a group: a band in the group's colour along its top, as Live's Session
            // View draws what a group holds (manual 18.3).
            if (group_ > 0) {
                g.setColour(panel_.root.arrangement.colourFor(group_ - 1));
                g.fillRect(0, 0, w, 3);
            }
            if (track) {
                g.setColour(look::ink);
                g.setFont(look::font(look::fontBody, true));
                const int badgeSpace = track->frozen ? 14 : 0;
                g.drawText(juce::String::fromUTF8(displayName(track->name, number_).c_str()), 4, 0,
                           w - 8 - badgeSpace, 17, juce::Justification::centredLeft);
                if (track->frozen)
                    controls::frozenTrackBadge(g, {static_cast<float>(w - 14), 3, 11, 11});
            }
        }
        if (audio_)
            paintMeter(g, meterArea());
        else
            paintMidiMeter(g, meterArea());
        return;
    }
    // Live's columns: grey, lighter for the selected track, a dark rule between them
    // and under the track.
    g.fillAll(selected_ ? look::laneSelectedAlt : look::panel);
    g.setColour(look::separator);
    g.fillRect(0, getHeight() - 2, getWidth(), 2);
    const int x = showsInOut() ? MixerPanel::kInOutWidth : 0;
    if (x > 0) {
        g.fillRect(0, 0, 2, getHeight());
        g.fillRect(x - 1, 0, 1, getHeight());
        paintInOut(g, 2);
    }
    if (delay.isVisible())
        paintDelayUnit(g);
    if (!panel_.root.state().trackMixer)
        return;
    const auto meter = meterArea();
    if (audio_)
        paintMeter(g, meter);
    else
        paintMidiMeter(g, meter);
    if (track && track->frozen) {
        g.setColour(look::value);
        g.fillRect(meter.getRight(), meter.getY(), 2, meter.getHeight());
    }
}
void MixerStrip::paintDelayUnit(juce::Graphics &g) const {
    // Live's "ms" box right of the Track Delay's value box, lit as its figure shows it (manual 18.7).
    const auto b = delay.getBounds();
    const juce::Rectangle<float> r(static_cast<float>(b.getRight() + 2), static_cast<float>(b.getY()), 22.f, 13.f);
    controls::button(g, r, delay.isEnabled() ? controls::ButtonState::On : controls::ButtonState::DisabledOn, "ms");
}
void MixerStrip::commitDelay() {
    // From the CURRENT cell, as every action is (ADR-0201): ms into samples at the project's rate,
    // one op, one undo step. The box then shows what the sample grid kept; a refusal puts it back
    // to the track's delay and the Status Bar says why.
    const auto current = panel_.root.current();
    if (!current)
        return;
    auto edit = setTrackDelay(current->tracks, current->sampleRate, track_, delay.getValue());
    if (!edit.refusal.empty() || edit.ops.empty()) {
        if (const auto *t = current->findTrack(track_))
            delay.setValue(trackDelayMs(t->delaySamples, current->sampleRate), juce::dontSendNotification);
        if (!edit.refusal.empty())
            panel_.root.say(edit.refusal);
        return;
    }
    panel_.root.submitEdits(std::move(edit.ops));
}
void MixerStrip::paintMidiMeter(juce::Graphics &g, juce::Rectangle<int> area) const {
    // Live's MIDI meter: a column of dots down the meter's place that light while notes play.
    const float d = std::min(5.5f, static_cast<float>(area.getWidth())), pitch = d + 2.5f;
    const float x = static_cast<float>(area.getCentreX()) - d / 2;
    g.setColour(midiLit_ ? look::accent : look::display);
    for (float y = static_cast<float>(area.getY()); y + d <= static_cast<float>(area.getBottom()); y += pitch)
        g.fillEllipse(x, y, d, d);
}
void MixerStrip::paintInOut(juce::Graphics &g, int x) const {
    // The track's input and monitoring go above its output.
    const int rows = std::max(1, (getHeight() - 1) / 17);
    auto chooser = [&](int row, const juce::String &label, bool enabled) {
        if (row < rows)
            drawChooser(g, {static_cast<float>(x + 3), static_cast<float>(3 + 17 * row), 70, 13}, label, enabled);
    };
    // Main plays to the outputs, a return and a group to Main or their group; none records, so none
    // has an input or the monitoring switch, as Live's (manual 18.3: a group "cannot contain clips").
    if (kind_ == "master" || kind_ == "return" || kind_ == "group") {
        chooser(0, outputLabel(), true);
        return;
    }
    const bool resampling = audioInputIsResampling();
    chooser(0, midi_ ? midiInputLabel(RoutingChooser::MidiFrom) : juce::String(resampling ? "Resampling" : "Ext. In"), true);
    chooser(1, midi_ ? midiInputLabel(RoutingChooser::MidiChannel) : juce::String(resampling ? "Stereo" : audioInputLabel()),
            midi_ || (!resampling && static_cast<bool>(panel_.root.externalAudioInputs)));
    chooser(3, outputLabel(), true);
    chooser(4, "", false);
}
const char *MixerStrip::audioInputLabel() const {
    if (!panel_.root.externalAudioInputs)
        return "1/2";
    const auto *track = panel_.root.reader().findTrack(track_);
    if (track)
        if (const auto *input = resolveExternalAudioInput(panel_.root.audioInputs(), track->inputRef))
            return input->label.c_str();
    return track && track->inputRef ? "Unavailable" : "No Input";
}
bool MixerStrip::audioInputIsResampling() const {
    const auto *track = panel_.root.reader().findTrack(track_);
    return track && track->inputRef == kResamplingInput;
}
bool MixerStrip::onAudioInput(juce::Point<int> at) const noexcept {
    if (kind_ != "audio")
        return false;
    const bool hasChannelChooser = panel_.root.externalAudioInputs && !audioInputIsResampling();
    if (panel_.vertical()) {
        const auto width = getWidth() - 10;
        return io_ > 0 && (juce::Rectangle<int>(4, kMixerTop + 15, width, 13).contains(at) ||
                           (hasChannelChooser && juce::Rectangle<int>(4, kMixerTop + 31, width, 13).contains(at)));
    }
    return showsInOut() && (juce::Rectangle<int>(5, 3, 70, 13).contains(at) ||
                            (hasChannelChooser && (getHeight() - 1) / 17 > 1 &&
                             juce::Rectangle<int>(5, 20, 70, 13).contains(at)));
}
juce::PopupMenu MixerStrip::audioInputMenu() {
    juce::PopupMenu menu;
    const auto *track = panel_.root.reader().findTrack(track_);
    if (!track || track->kind != "audio")
        return menu;
    const auto choices = panel_.root.audioInputs();
    const auto *selected = resolveExternalAudioInput(choices, track->inputRef);
    const auto safe = SafePointer<MixerStrip>(this);
    for (const auto &input : choices) {
        const bool inputSelected = &input == selected;
        juce::PopupMenu::Item item(juce::String::fromUTF8(input.label.c_str()));
        item.setID(-1).setTicked(inputSelected).setAction([safe, id = track_, ref = input.ref] {
            if (safe)
                safe->panel_.root.setAudioInput(id, ref);
        });
        if (panel_.root.externalInputMeter)
            item.setCustomComponent(new InputMenuRow(input, panel_.root.externalInputMeter, inputSelected));
        menu.addItem(std::move(item));
    }
    if (choices.empty())
        menu.addItem("No enabled inputs", false, false, [] {});
    menu.addItem("Resampling", true, track->inputRef == kResamplingInput, [safe, id = track_] {
        if (safe)
            safe->panel_.root.setAudioInput(id, kResamplingInput);
    });
    menu.addSeparator();
    menu.addItem("Configure...", static_cast<bool>(panel_.root.configureExternalInputs), false, [safe] {
        if (safe && safe->panel_.root.configureExternalInputs)
            safe->panel_.root.configureExternalInputs();
    });
    return menu;
}
void MixerStrip::showAudioInputMenu() {
    auto menu = audioInputMenu();
    if (onAudioInputMenu) {
        onAudioInputMenu(std::move(menu));
        return;
    }
    std::shared_ptr<InputMenuReading> reading;
    if (panel_.root.externalInputMeter)
        reading = std::make_shared<InputMenuReading>(panel_.root.externalInputMeter);
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition(),
                       [reading = std::move(reading)](int) mutable { reading.reset(); });
}
juce::Rectangle<int> MixerStrip::monitorBounds() const noexcept {
    if (kind_ != "audio" && kind_ != "midi" && kind_ != "instrument")
        return {};
    if (panel_.vertical())
        return io_ > 0 ? monitorBox(4, kMixerTop + 59, 3) : juce::Rectangle<int>{};
    if (!showsInOut() || (getHeight() - 1) / 17 <= 2)
        return {};
    return monitorBox(5, 37, 6);
}
void MixerStrip::layoutMonitor() {
    const auto bounds = monitorBounds();
    monitor_.setBounds(bounds);
    monitor_.setVisible(!bounds.isEmpty());
}
std::optional<Monitor> MixerStrip::monitorAt(juce::Point<int> at) const noexcept {
    return monitorCellAt(monitorBounds().toFloat(), at);
}
MixerStrip::MonitorSwitch::MonitorSwitch(MixerStrip &strip) : strip_(strip) {
    setTitle("Monitor");
    setTooltip("Choose In, Auto or Off. Delete restores the track's default.");
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(false);
}
bool MixerStrip::MonitorSwitch::hitTest(int x, int y) {
    // A short Arrangement row can draw the lowest pixels of the switch on the track's resize band.
    return (strip_.panel_.vertical() || getY() + y < strip_.getHeight() - TrackHeaderList::kSplitLine) &&
           juce::Component::hitTest(x, y);
}
void MixerStrip::MonitorSwitch::paint(juce::Graphics &g) {
    const auto bounds = getLocalBounds().toFloat();
    controls::oneOf(g, bounds, monitorLabels(), monitorIndex(strip_.record_.monitor));
    if (hasKeyboardFocus(false)) {
        g.setColour(look::accent);
        g.drawRoundedRectangle(bounds.reduced(look::hairline / 2), look::radiusControl, look::hairline);
    }
}
void MixerStrip::MonitorSwitch::mouseDown(const juce::MouseEvent &event) {
    // Use the strip's coordinates and event component for its existing hit and popup handling.
    strip_.mouseDown(event.getEventRelativeTo(&strip_));
}
bool MixerStrip::MonitorSwitch::keyPressed(const juce::KeyPress &key) {
    if (!isVisible() || (key != juce::KeyPress::deleteKey && key != juce::KeyPress::backspaceKey))
        return false;
    strip_.panel_.root.setMonitor(strip_.track_, std::nullopt);
    return true;
}
std::optional<MixerStrip::RoutingChooser> MixerStrip::routingChooserAt(juce::Point<int> at) const noexcept {
    const bool vertical = panel_.vertical();
    if (vertical ? io_ == 0 : !showsInOut())
        return std::nullopt;
    const auto on = [&](int mixerY, int row) {
        if (vertical)
            return juce::Rectangle<int>(4, kMixerTop + mixerY, getWidth() - 10, 13).contains(at);
        const int rows = std::max(1, (getHeight() - 1) / 17);
        return row < rows && juce::Rectangle<int>(5, 3 + 17 * row, 70, 13).contains(at);
    };
    if (kind_ == "master") {
        if (vertical && on(73, 0))
            return RoutingChooser::CueOut;
        if (on(103, 0))
            return RoutingChooser::MainOut;
        return std::nullopt;
    }
    if (kind_ == "group" || kind_ == "return")
        return on(87, 0) ? std::optional{RoutingChooser::AudioTo} : std::nullopt;
    if (midi_) {
        if (on(15, 0))
            return RoutingChooser::MidiFrom;
        if (on(31, 1))
            return RoutingChooser::MidiChannel;
    }
    if (on(87, 3))
        return audio_ ? RoutingChooser::AudioTo : RoutingChooser::MidiTo;
    return std::nullopt;
}

std::span<const std::pair<int, std::string>> MixerStrip::stereoOutputs() const {
    if (panel_.root.externalAudioOutputs)
        return panel_.root.externalAudioOutputs();
    static const std::array<std::pair<int, std::string>, 1> defaultOutputs{{{0, "1/2"}}};
    return defaultOutputs;
}

int MixerStrip::selectedStereoOutput(RoutingChooser chooser) const {
    const auto *project = panel_.root.reader().get();
    if (!project)
        return 0;
    return chooser == RoutingChooser::MainOut ? project->cue.firstMainOutputChannel : project->cue.firstOutputChannel;
}

juce::String MixerStrip::stereoOutputLabel(RoutingChooser chooser) const {
    const int first = selectedStereoOutput(chooser);
    if (first == 0 && !panel_.root.externalAudioOutputs)
        return "1/2";
    const auto pairs = stereoOutputs();
    const auto selected = std::ranges::find(pairs, first, &std::pair<int, std::string>::first);
    if (selected != pairs.end())
        return juce::String::fromUTF8(selected->second.c_str());
    const auto pairName = juce::String(first + 1) + "/" + juce::String(first + 2);
    return chooser == RoutingChooser::CueOut ? pairName : pairName + " (Unavailable)";
}

juce::String MixerStrip::midiInputLabel(RoutingChooser chooser) const {
    const auto *track = panel_.root.reader().findTrack(track_);
    const auto choice = engine::midiInputChoice(track && track->inputRef ? std::string_view(*track->inputRef) : std::string_view{});
    if (chooser == RoutingChooser::MidiChannel)
        return choice.channel == 0 ? juce::String("All Channels") : juce::String(choice.channel);
    if (choice.port.empty())
        return "All Ins";
    const auto &ports = panel_.root.midiInputPorts;
    const auto selected = std::ranges::find(ports, choice.port, &MidiPortListing::id);
    return selected == ports.end() ? juce::String("Unavailable") : juce::String::fromUTF8(selected->name.c_str());
}
juce::PopupMenu MixerStrip::midiInputMenu(RoutingChooser chooser) {
    juce::PopupMenu menu;
    const auto *track = panel_.root.reader().findTrack(track_);
    const auto choice = engine::midiInputChoice(track && track->inputRef ? std::string_view(*track->inputRef) : std::string_view{});
    const auto safe = SafePointer<MixerStrip>(this);
    const auto add = [&](const juce::String &name, std::string_view endpoint, int channel) {
        menu.addItem(name, true, choice.port == endpoint && choice.channel == channel,
                     [safe, id = track_, port = std::string(endpoint), channel] {
                         if (safe)
                             safe->panel_.root.setMidiInput(id, port, channel);
                     });
    };
    if (chooser == RoutingChooser::MidiFrom) {
        add("All Ins", {}, choice.channel);
        for (const auto &port : panel_.root.midiInputPorts)
            add(juce::String::fromUTF8(port.name.c_str()), port.id, choice.channel);
    } else {
        add("All Channels", choice.port, 0);
        for (int channel = 1; channel <= 16; ++channel)
            add(juce::String(channel), choice.port, channel);
    }
    return menu;
}

juce::String MixerStrip::midiOutputLabel() const {
    const auto* trackNode = panel_.root.reader().findTrack(track_);
    if (!trackNode || !trackNode->midiOutput) return "No Output";
    const auto ports = panel_.root.midiTrackOutputs ? panel_.root.midiTrackOutputs() : std::vector<MidiPortListing>{};
    const auto port = std::ranges::find(ports, trackNode->midiOutput->port, &MidiPortListing::id);
    const auto name = port == ports.end() ? juce::String("Unavailable") : juce::String::fromUTF8(port->name.c_str());
    return name + " / " + juce::String(trackNode->midiOutput->channel);
}

juce::PopupMenu MixerStrip::midiOutputMenu() {
    const auto safe = SafePointer<MixerStrip>(this);
    const auto* trackNode = panel_.root.reader().findTrack(track_);
    const auto current = trackNode ? trackNode->midiOutput : std::optional<rows::MidiOutput>{};
    const auto ports = panel_.root.midiTrackOutputs ? panel_.root.midiTrackOutputs() : std::vector<MidiPortListing>{};
    juce::PopupMenu menu;
    menu.addItem("No Output", current.has_value(), !current, [safe] {
        if (safe)
            safe->panel_.root.setTrackMidiOutput(safe->track_, {});
    });
    for (const auto& port : ports) {
        juce::PopupMenu channels;
        const bool selectedPort = current && current->port == port.id;
        for (int channel = 1; channel <= 16; ++channel) {
            channels.addItem("Channel " + juce::String(channel), true, selectedPort && current->channel == channel,
                [safe, route = rows::MidiOutput{port.id, channel}] {
                    if (safe)
                        safe->panel_.root.setTrackMidiOutput(safe->track_, route);
                });
        }
        menu.addSubMenu(juce::String::fromUTF8(port.name.c_str()), channels, true, nullptr, selectedPort);
    }
    return menu;
}

void MixerStrip::showRoutingMenu(RoutingChooser chooser) {
    const auto safe = SafePointer<MixerStrip>(this);
    const auto configure = [&](juce::PopupMenu &menu, bool midi) {
        const auto &handler = midi ? panel_.root.configureMidiRouting : panel_.root.configureExternalInputs;
        menu.addSeparator();
        menu.addItem("Configure...", static_cast<bool>(handler), false, [safe, midi] {
            if (safe) {
                const auto &action = midi ? safe->panel_.root.configureMidiRouting : safe->panel_.root.configureExternalInputs;
                if (action)
                    action();
            }
        });
    };
    juce::PopupMenu menu;
    switch (chooser) {
    case RoutingChooser::MidiFrom:
    case RoutingChooser::MidiChannel:
        menu = midiInputMenu(chooser);
        configure(menu, true);
        break;
    case RoutingChooser::MidiTo:
        menu = midiOutputMenu();
        configure(menu, true);
        break;
    case RoutingChooser::MainOut:
    case RoutingChooser::CueOut: {
        const auto outputs = stereoOutputs();
        const auto &physicalChannels = panel_.root.physicalOutputChannels;
        const auto grant = physicalChannels ? physicalChannels() : std::nullopt;
        const int first = selectedStereoOutput(chooser);
        const auto addPair = [&](int pair, const juce::String &name) {
            menu.addItem(name, true, pair == first, [safe, pair, chooser, track = track_] {
                if (safe && safe->track_ == track && safe->kind_ == "master") {
                    if (chooser == RoutingChooser::MainOut)
                        safe->panel_.root.setMainOutput(pair);
                    else
                        safe->panel_.root.setCueOutput(pair);
                }
            });
        };
        if (chooser == RoutingChooser::CueOut && physicalChannels) {
            for (int pair = 0; pair < engine::OutputChannelMap::maximumChannels; pair += 2) {
                if (grant ? grant->packedPair(pair) < 0 : pair != 0)
                    continue;
                const auto named = std::ranges::find(outputs, pair, &std::pair<int, std::string>::first);
                addPair(pair, named == outputs.end() ? juce::String(pair + 1) + "/" + juce::String(pair + 2)
                                                     : juce::String::fromUTF8(named->second.c_str()));
            }
            if (menu.getNumItems() == 0 && !onRoutingMenu && !panel_.contextMenuPresenter) {
                panel_.root.say("Enable a stereo output pair in Settings > Audio.");
                return;
            }
        } else {
            for (const auto &output : outputs)
                if (!grant || grant->packedPair(output.first) >= 0)
                    addPair(output.first, juce::String::fromUTF8(output.second.c_str()));
        }
        if (menu.getNumItems() == 0)
            menu.addItem("No enabled stereo outputs", false, false, [] {});
        configure(menu, false);
        break;
    }
    case RoutingChooser::AudioTo: {
        const auto *track = panel_.root.reader().findTrack(track_);
        bool hasChoice = false;
        for (const auto id : panel_.root.audioOutputChoices(track_))
            if (const auto *destination = panel_.root.reader().findTrack(id)) {
                const auto label = audioDestinationLabel(*destination);
                const auto current = track && track->audioOutput ? *track->audioOutput : track ? track->parent : 0;
                const bool selected = current == id || (current == 0 && destination->kind == "master");
                menu.addItem(label, true, selected, [safe, source = track_, id] {
                    if (safe)
                        safe->panel_.root.setTrackAudioOutput(source, id);
                });
                hasChoice = true;
            }
        if (!hasChoice)
            menu.addItem("No available destinations", false, false, [] {});
        break;
    }
    }
    if (onRoutingMenu) {
        onRoutingMenu(chooser, std::move(menu));
        return;
    }
    if (chooser == RoutingChooser::CueOut) {
        if (panel_.contextMenuPresenter) {
            panel_.contextMenuPresenter(std::move(menu));
            return;
        }
        if (!panel_.vertical()) {
            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&cueOut));
            return;
        }
    }
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
}
void MixerStrip::cueOutMenu() {
    if (kind_ == "master")
        showRoutingMenu(RoutingChooser::CueOut);
}
bool MixerStrip::onMonitorInOrAuto(juce::Point<int> at) const noexcept {
    const auto box = monitorBounds().toFloat();
    if (box.isEmpty())
        return false;
    const auto in = controls::oneOfCell(box, monitorLabels(), 0), automatic = controls::oneOfCell(box, monitorLabels(), 1);
    return in.getUnion(automatic).contains(at.toFloat());
}
juce::PopupMenu MixerStrip::monitorMenu() {
    juce::PopupMenu menu;
    const auto *t = panel_.root.reader().findTrack(track_);
    if (t == nullptr)
        return menu;
    const bool on = t->recording.keepMonitoringLatency;
    menu.addItem("Keep Monitoring Latency in Recorded Audio", true, on,
                 [this, id = track_, on] { panel_.root.setKeepMonitoringLatency(id, !on); });
    return menu;
}
juce::String MixerStrip::audioDestinationLabel(const engine::TrackNode &destination) const {
    if (destination.kind == "master")
        return "Main";
    if (const auto *track = panel_.root.reader().findTrack(track_); track && track->parent == destination.id)
        return "Group";
    return juce::String::fromUTF8(destination.name.c_str());
}
juce::String MixerStrip::outputLabel() const {
    if (kind_ == "master")
        return stereoOutputLabel(RoutingChooser::MainOut);
    if (!audio_)
        return midiOutputLabel();
    if (const auto *track = panel_.root.reader().findTrack(track_); track && track->audioOutput)
        if (const auto *target = panel_.root.reader().findTrack(*track->audioOutput))
            return audioDestinationLabel(*target);
    return inGroup_ ? "Group" : "Main";
}
void MixerStrip::paintMixerInOut(juce::Graphics &g) const {
    // Live 12's In/Out section in the Mixer (manual, the Mixer in Arrangement View): where the track
    // records from, its monitoring, where it plays to, each under its label; a return's output only,
    // level with the tracks'; Main's Cue Out and Main Out.
    const int top = kMixerTop, w = getWidth() - 2;
    auto label = [&](int y, const char *text) {
        g.setColour(look::text);
        g.setFont(look::font(look::fontBody, true));
        g.drawText(text, 5, top + y, w - 10, 13, juce::Justification::centredLeft);
    };
    auto chooser = [&](int y, const juce::String &text, bool enabled) {
        drawChooser(g, {4.f, static_cast<float>(top + y), static_cast<float>(w - 8), 13.f}, text, enabled);
    };
    if (kind_ == "master") {
        // Live's Cue Out (manual 18.6; MX-08): the stereo pair the cued tracks play to, chosen here.
        label(60, "Cue Out");
        chooser(73, stereoOutputLabel(RoutingChooser::CueOut), true);
        label(90, "Main Out");
        chooser(103, stereoOutputLabel(RoutingChooser::MainOut), true);
        return;
    }
    if (kind_ == "return" || kind_ == "group") {
        label(74, "Audio To");
        chooser(87, outputLabel(), true);
        chooser(103, "", false);
        return;
    }
    label(2, midi_ ? "MIDI From" : "Audio From");
    const bool resampling = audioInputIsResampling();
    chooser(15, midi_ ? midiInputLabel(RoutingChooser::MidiFrom) : juce::String(resampling ? "Resampling" : "Ext. In"), true);
    chooser(31, midi_ ? midiInputLabel(RoutingChooser::MidiChannel) : juce::String(resampling ? "Stereo" : audioInputLabel()),
            midi_ || (!resampling && static_cast<bool>(panel_.root.externalAudioInputs)));
    label(46, "Monitor");
    label(74, audio_ ? "Audio To" : "MIDI To");
    chooser(87, outputLabel(), true);
    chooser(103, "", false);
}
void MixerStrip::paintMeter(juce::Graphics &g, juce::Rectangle<int> area) const {
    // Live's meter: two dark channels on Live's scale, the level in green, the peak a
    // line, red once it clips.
    const int height = area.getHeight();
    auto depth = [height](float v) {
        return static_cast<int>(static_cast<double>(height) *
                                meterDepth(20 * std::log10(std::max(v, 1.e-6f))));
    };
    const std::array columns{area.withWidth(area.getWidth() / 2 - 1),
                             area.withTrimmedLeft(area.getWidth() / 2 + 1)};
    for (std::size_t channel = 0; channel < columns.size(); ++channel) {
        const auto &column = columns[channel];
        const auto &level = levels_[channel];
        g.setColour(look::display);
        g.fillRect(column);
        g.setColour(look::meterLevel);
        g.fillRect(column.withTrimmedTop(depth(level.rms)));
        if (level.peak > 1.e-6f) {
            g.setColour(level.peak >= 1 ? look::record : look::meterPeak);
            g.fillRect(column.getX(), column.getY() + depth(level.peak), column.getWidth(), 1);
        }
    }
    if (!panel_.vertical())
        return;
    // Live's scale beside the fader: 0 to 60 dB every 12, every 6 on a tall strip.
    const bool tall = getHeight() >= MixerPanel::kTallHeight;
    g.setFont(look::font(look::fontBody));
    for (int db = 0; db >= -60; db -= tall ? 6 : 12) {
        const int y = area.getY() + static_cast<int>(height * meterDepth(db));
        g.setColour(look::text);
        g.fillRect(area.getRight() + 5, y, 3, 1);
        if (db % 12 == 0)
            g.drawText(juce::String(-db), area.getRight() + 11, y - 7, 24, 14,
                       juce::Justification::centredLeft);
    }
}
bool MixerStrip::hitTest(int x, int y) {
    // Above or below the lanes the panel's frame stands in front (MixerPanel::paintOverChildren),
    // so a click there is not this row's, nor any of its controls'.
    if (!panel_.rowsBand().contains(getY() + y))
        return false;
    return juce::Component::hitTest(x, y);
}
void MixerStrip::mouseDoubleClick(const juce::MouseEvent &e) {
    // Live: a track's title bar, double-clicked, brings up the Device View with its devices.
    if (!panel_.vertical() || e.eventComponent != this || e.y >= kTitleHeight || track_ == 0)
        return;
    panel_.root.arrangement.geometry.selection.track = track_;
    panel_.root.arrangement.geometry.selection.toTrack = 0;
    panel_.root.arrangement.geometry.selection.picked.clear();
    panel_.root.publishSelection(AdiRootComponent::SelectionOrigin::Mixer);
    panel_.root.showDeviceView();
}
bool MixerStrip::isInterestedInDragSource(const SourceDetails &details) {
    return track_ != 0 && (panel_.root.browser.acceptsDeviceDrop(details) ||
                           !BrowserPanel::droppedFiles(details.description).isEmpty());
}
void MixerStrip::setDropPreview(bool on) {
    if (dropPreview_ == on)
        return;
    dropPreview_ = on;
    repaint();
}
void MixerStrip::itemDragMove(const SourceDetails &details) {
    if (track_ != 0 && panel_.root.browser.acceptsDeviceDrop(details))
        setDropPreview(true);
    else
        fileDragEnter(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
void MixerStrip::itemDragExit(const SourceDetails &) { setDropPreview(false); }
void MixerStrip::itemDropped(const SourceDetails &details) {
    setDropPreview(false);
    if (track_ == 0)
        return;
    if (panel_.root.browser.acceptsDeviceDrop(details)) {
        panel_.root.browser.loadDroppedDevice(details, track_);
        return;
    }
    filesDropped(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
bool MixerStrip::isInterestedInFileDrag(const juce::StringArray &files) {
    return track_ != 0 && !files.isEmpty();
}
void MixerStrip::fileDragEnter(const juce::StringArray &files, int, int) {
    const bool instrument = files.size() == 1 && panel_.root.loadSampleInstrument &&
                            panel_.root.sampleInstrumentTarget(track_);
    setDropPreview(track_ != 0 && !files.isEmpty() && (kind_ == "audio" || instrument));
}
void MixerStrip::fileDragExit(const juce::StringArray &) { setDropPreview(false); }
void MixerStrip::filesDropped(const juce::StringArray &files, int, int) {
    setDropPreview(false);
    if (track_ != 0)
        panel_.root.dropAudioPaths(files, track_, panel_.root.arrangement.geometry.selection.insert);
}
std::optional<bool> MixerStrip::splitLine(const juce::MouseEvent &e) const noexcept {
    if (panel_.vertical() || track_ == 0 || e.eventComponent != this) return {};
    const auto rows = panel_.root.arrangement.geometry.rows(panel_.root.reader());
    const auto at = rows.find(track_);
    if (at >= rows.size()) return {};
    const bool anchored = rows[at].place.foot();
    if (!TrackHeaderList::onResizeEdge(e.y, getHeight(), anchored)) return {};
    return anchored;
}
void MixerStrip::mouseMove(const juce::MouseEvent &e) {
    setMouseCursor(splitLine(e) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}
void MixerStrip::mouseDrag(const juce::MouseEvent &e) {
    if (titleDrag_) {
        auto &drag = *titleDrag_;
        const auto at = e.getEventRelativeTo(&panel_).getPosition();
        if (drag.generation != panel_.root.committedGeneration()) {
            titleDrag_.reset();
            panel_.previewTrackDrop(std::nullopt);
        } else if (drag.moving || std::abs(at.x - drag.startX) > 4) {
            if (!drag.moving)
                focusIfShowing(panel_);
            drag.moving = true;
            if (panel_.getLocalBounds().contains(at))
                panel_.previewTrackDrop(panel_.trackDropAt(at.x, drag.movingReturn));
            else
                panel_.previewTrackDrop(std::nullopt);
        }
        return;
    }
    if (resizeFrom_ < 0)
        return;
    auto &view = panel_.root.arrangement;
    const int dy = e.getDistanceFromDragStartY();
    view.dragTrackHeight(track_, resizeFrom_ + (resizeFromTop_ ? -dy : dy), e.mods.isAltDown());
}
void MixerStrip::mouseUp(const juce::MouseEvent &e) {
    if (const auto drag = std::exchange(titleDrag_, std::nullopt)) {
        panel_.previewTrackDrop(std::nullopt);
        if (drag->generation != panel_.root.committedGeneration())
            return;
        if (drag->moving) {
            const auto at = e.getEventRelativeTo(&panel_).getPosition();
            if (panel_.getLocalBounds().contains(at))
                panel_.root.moveTracks(drag->source, panel_.trackDropAt(at.x, drag->movingReturn).target);
        } else if (drag->selectOnRelease) {
            selectTitle(drag->source);
            panel_.root.publishSelection(AdiRootComponent::SelectionOrigin::Mixer);
            panel_.root.arrangement.changed();
        }
        return;
    }
    if (resizeFrom_ < 0)
        return;
    resizeFrom_ = -1;
    dragging_ = false;
    panel_.root.arrangement.changed();
}
void MixerStrip::trackTitleMenu(juce::Point<int> where) {
    const auto track = track_;
    const auto current = panel_.root.current();
    if (!current || !current->findTrack(track))
        return;
    auto &view = panel_.root.arrangement;
    view.canvas.clearAutomationSelection();
    auto &selection = view.geometry.selection;
    const bool tracksSelected = current->findTrack(selection.track) && selection.clip == 0 &&
                                selection.pickedClips.empty() && selection.start == selection.end &&
                                view.selectedLocator == 0 && view.selectedMeter < 0 && !view.loopSelected;
    if (!tracksSelected || !view.geometry.inSelection(view.reader(), track)) {
        selection.track = track;
        selection.toTrack = 0;
        selection.picked.clear();
    }
    selection.clearMaterial();
    view.selectedLocator = 0;
    view.selectedMeter = -1;
    view.loopSelected = false;
    view.scrub.repaint();
    focusIfShowing(*this);
    panel_.root.publishSelection(AdiRootComponent::SelectionOrigin::Mixer);
    view.changed();
    panel_.showContextMenu(view.headerMenu(track), where);
}
void MixerStrip::selectTitle(std::int64_t track) {
    auto &selection = panel_.root.arrangement.geometry.selection;
    selection.track = track;
    selection.toTrack = 0;
    selection.picked.clear();
}
void MixerStrip::mouseDown(const juce::MouseEvent &e) {
    if (std::exchange(titleDrag_, std::nullopt))
        panel_.previewTrackDrop(std::nullopt);
    if (const auto edge = splitLine(e); edge && !e.mods.isPopupMenu()) {
        // Kept bound to its track while the drag resizes the rows (MixerPanel::place).
        resizeFrom_ = panel_.root.arrangement.geometry.heightFor(track_);
        resizeFromTop_ = *edge;
        dragging_ = true;
        return;
    }
    if (panel_.vertical() && shown_ > 0 && (!sends_.empty() || !returnSwitches_.empty()) &&
        e.eventComponent == this && e.mods.isLeftButtonDown() && !e.mods.isPopupMenu()) {
        const auto first = panel_.firstSend(shown_);
        if (first > 0 && sendArrowBounds(false).contains(e.getPosition())) {
            panel_.scrollSendsBy(-1, shown_);
            return;
        }
        if (first + shown_ < panel_.sendReturns_.size() && sendArrowBounds(true).contains(e.getPosition())) {
            panel_.scrollSendsBy(1, shown_);
            return;
        }
    }
    auto &root = panel_.root;
    const bool chooseEnvelope = root.automationMode() && e.mods.isLeftButtonDown() && !e.mods.isPopupMenu();
    const auto onControl = [&](const juce::Component &control) {
        return e.eventComponent == &control ||
               (dynamic_cast<const juce::Label *>(e.eventComponent) != nullptr &&
                e.eventComponent->getParentComponent() == &control);
    };
    // A send's own clicks, and an off send's, which reach the strip: its menu, or what it waits for.
    for (std::size_t i = 0; i < sends_.size(); ++i) {
        auto &knob = *sends_[i];
        const bool own = onControl(knob);
        if (!own && !(e.eventComponent == this && knob.isVisible() && knob.getBounds().contains(e.getPosition())))
            continue;
        if (e.mods.isPopupMenu())
            sendMenu(i);
        else if (!own)
            root.say("This send is off. Right-click it for Enable Send.");
        else if (chooseEnvelope && i < returns_.size())
            if (const auto snapshot = root.current())
                if (const auto *send = findSend(snapshot->sends, track_, returns_[i]))
                    root.showAutomation(track_, sendTarget(send->id, {}));
        return;
    }
    if (e.eventComponent == this) {
        if (onAudioInput(e.getPosition())) {
            showAudioInputMenu();
            return;
        }
        if (const auto chooser = routingChooserAt(e.getPosition())) {
            showRoutingMenu(*chooser);
            return;
        }
    }
    // A click on a Mixer View strip's title bar selects its track, as a click on its header does
    // (Live 12 manual 18.1): Ctrl/Cmd+click picks it apart or lets it go, Shift+click reaches across
    // the tracks between. So Ctrl+G after picking strips groups them (walk-B B10).
    if (panel_.vertical() && e.eventComponent == this && e.y < kTitleHeight && track_ != 0) {
        if (e.mods.isPopupMenu()) {
            trackTitleMenu(e.getScreenPosition());
            return;
        }
        focusIfShowing(*this);
        const auto &reader = panel_.root.reader();
        auto &arrangement = panel_.root.arrangement;
        arrangement.canvas.clearAutomationSelection();
        auto &geometry = arrangement.geometry;
        auto &selection = geometry.selection;
        selection.clearMaterial();
        if (e.mods.isLeftButtonDown() && !e.mods.isAnyModifierKeyDown() && kind_ != "master") {
            // The pool can rebind this strip as the Mixer scrolls. Keep the pressed track's ID.
            const bool deferSelection = geometry.inSelection(reader, track_) &&
                                        geometry.selectedTracks(reader).size() > 1;
            titleDrag_ = TitleDrag{track_, panel_.root.committedGeneration(), e.getEventRelativeTo(&panel_).x,
                                   kind_ == "return", deferSelection};
            if (deferSelection) {
                panel_.root.arrangement.changed();
                return;
            }
        }
        if (e.mods.isCommandDown() && selection.track != 0) {
            auto &picked = selection.picked;
            if (track_ != selection.track) {
                if (const auto at = std::find(picked.begin(), picked.end(), track_); at != picked.end())
                    picked.erase(at);
                else
                    picked.push_back(track_);
            }
        } else if (e.mods.isShiftDown() && selection.track != 0)
            selection.toTrack = track_ == selection.track ? 0 : track_;
        else
            selectTitle(track_);
        panel_.root.publishSelection(AdiRootComponent::SelectionOrigin::Mixer);
        panel_.root.arrangement.changed();
        return;
    }
    if (chooseEnvelope) {
        if (onControl(pan))
            root.showAutomation(track_, panTarget(track_));
        else if (onControl(volume))
            root.showAutomation(track_, volumeTarget(track_));
    }
    if (!e.mods.isPopupMenu()) {
        // A click on In, Auto or Off sets the track's monitoring (manual 21.4), the selected tracks
        // together when it is among them.
        if (e.eventComponent == this)
            if (const auto mode = monitorAt(e.getPosition())) {
                focusIfShowing(monitor_);
                panel_.root.setMonitor(track_, *mode);
            }
        return;
    }
    if (e.eventComponent == this && onMonitorInOrAuto(e.getPosition())) {
        monitorMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
        return;
    }
    if (onControl(pan))
        panMenu();
    else if (onControl(volume))
        volumeMenu();
    else
        panel_.contextMenu(e.getScreenPosition());
}
void MixerStrip::syncSends(const std::vector<DevicePanelData> *devices) {
    const auto &reader = panel_.root.reader();
    const auto *t = reader.findTrack(track_);
    // A track's knobs go to the top-level returns and its own groups' (DF-W33); Main's switches to
    // every return.
    if (t != nullptr && t->kind == "master")
        returnsInOrder(reader.tracks(), order_);
    else
        returnsFor(reader.tracks(), track_, order_);
    static const std::string none;
    const std::string &kind = t != nullptr ? t->kind : none;
    const bool midi = t != nullptr && isMidiTrack(*t);
    const bool audio = t == nullptr || hasAudioMixer(*t, devices);
    const bool grouped = t != nullptr && t->parent != 0;
    // A send knob per return on every track with an audio mixer but Main; on Main's Mixer View
    // strip, each return's Pre/Post switch instead (Live's Mixer).
    const auto knobs = t != nullptr && hasSends(*t) && audio ? order_.size() : 0;
    const auto switches = kind == "master" && panel_.vertical() ? order_.size() : 0;
    // A letter follows its group's name and the group it sits in, which the ids alone don't show.
    bool sameLetters = letterCodes_.size() == order_.size();
    for (std::size_t i = 0; sameLetters && i < order_.size(); ++i) {
        std::array<char, 2> letter{};
        const auto n = returnLetter(reader.tracks(), order_[i], letter);
        sameLetters = letterCodes_[i] == std::array<char, 2>{letter[0], n > 1 ? letter[1] : '\0'};
    }
    if (sameLetters && order_ == returns_ && kind == kind_ && knobs == sends_.size() && switches == returnSwitches_.size() &&
        midi == midi_ && audio == audio_ && grouped == inGroup_)
        return;
    kind_ = kind;
    midi_ = midi;
    audio_ = audio;
    inGroup_ = grouped;
    returns_ = order_;
    sends_.resize(std::min(sends_.size(), knobs));
    while (sends_.size() < knobs)
        sends_.push_back(makeSend(sends_.size()));
    // Each knob named for its return's letter, which a grouped return or a moved one changes.
    letters_.clear();
    letterCodes_.clear();
    for (std::size_t i = 0; i < returns_.size(); ++i) {
        std::array<char, 2> letter{};
        const auto n = returnLetter(reader.tracks(), returns_[i], letter);
        letters_.push_back(juce::String(letter.data(), n));
        letterCodes_.push_back({letter[0], n > 1 ? letter[1] : '\0'});
        if (i < sends_.size()) {
            sends_[i]->setTitle("Send " + letters_.back());
            sends_[i]->setTooltip("Sets how much of the track goes to return track " + letters_.back() + ".");
        }
    }
    returnSwitches_.resize(std::min(returnSwitches_.size(), switches));
    while (returnSwitches_.size() < switches) {
        auto button = std::make_unique<juce::TextButton>("Post");
        const auto index = returnSwitches_.size();
        button->setTooltip("Whether this return's sends tap before or after their tracks' faders.");
        button->setClickingTogglesState(false);
        button->setColour(juce::TextButton::buttonColourId, look::button);
        button->setColour(juce::TextButton::buttonOnColourId, look::accent);
        button->setMouseClickGrabsKeyboardFocus(false);
        button->getProperties().set("live.font", static_cast<double>(look::fontLabel));
        button->onClick = [this, index] {
            if (index < returns_.size())
                throwPrePost(returns_[index]);
        };
        addAndMakeVisible(*button);
        returnSwitches_.push_back(std::move(button));
    }
    // Each switch titled for its return's letter, which a rename or a move changes.
    for (std::size_t i = 0; i < returnSwitches_.size(); ++i)
        returnSwitches_[i]->setTitle("Pre/Post " + (i < letters_.size() ? letters_[i] : juce::String()));
    sendOn_.assign(knobs, 1);
    resized();
    repaint();
}
void MixerStrip::throwPrePost(std::int64_t ret) {
    // From the CURRENT cell (ADR-0201): the return's own switch thrown, one op and one undo step,
    // which the engine spreads over every send into the return; it holds with no send (#405). A
    // mixed switch goes to Post first, every send's own tap cleared (the engine's click, ADR-0247 d2).
    const auto current = panel_.root.current();
    if (!current)
        return;
    panel_.root.submitEdits({clickReturnSendTap(ret)});
}
std::unique_ptr<juce::Slider> MixerStrip::makeSend(std::size_t index) {
    auto knob = std::make_unique<PrimaryButtonSlider>();
    if (panel_.vertical()) {
        knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    } else {
        // Live's send box: the number in it, filled in blue and dragged up or down as the volume's.
        knob->setSliderStyle(juce::Slider::LinearBarVertical);
        knob->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 0, 0);
        knob->setSliderSnapsToMousePosition(false);
        knob->setColour(juce::Slider::textBoxTextColourId, look::text);
    }
    knob->setRange(kSendFloorDb, kSendTopDb, .01);
    // As the volume's: the top of the travel finer than the foot.
    knob->setSkewFactor(2.27);
    knob->textFromValueFunction = [](double v) {
        if (v <= kSendFloorDb)
            return juce::String("-inf");
        return std::abs(v - std::round(v)) < .005 ? juce::String(static_cast<int>(std::lround(v)))
                                                  : juce::String(v, 1);
    };
    knob->setValue(kSendFloorDb, juce::dontSendNotification);
    knob->updateText();
    const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + index % 26));
    knob->setTitle("Send " + letter);
    knob->setTooltip("Sets how much of the track goes to return track " + letter + ".");
    knob->valueFromTextFunction = [control = knob.get()](const juce::String &text) {
        return levelValue(text, kSendFloorDb).value_or(control->getValue());
    };
    knob->enableValueEdits(kSendFloorDb, .1);
    knob->setScrollWheelEnabled(false);
    // A drag is one undo step: it commits when the mouse lets go. A typed value commits at once.
    // A send that has its row is dragged live, as the fader is (MX-23); one with none yet is made when
    // the mouse lets go (its row and id come with the commit).
    knob->onDragStart = [this, index] {
        dragging_ = true;
        const auto current = panel_.root.current();
        if (current && index < returns_.size())
            if (const auto *send = findSend(current->sends, track_, returns_[index]))
                beginLive(gesture::ScalarKind::RoutingGain, send->id, {kSendFloorDb, kSendTopDb, kSendSilenceDb},
                          "Change Send Level");
    };
    knob->onDragEnd = [this, index] {
        dragging_ = false;
        if (!endLive())
            commitSend(index);
    };
    knob->onValueChange = [this, index] {
        if (live_)
            panel_.root.mixerDrag(sends_[index]->getValue());
        else if (!dragging_)
            commitSend(index);
    };
    knob->addMouseListener(this, true);
    addAndMakeVisible(*knob);
    return knob;
}
void MixerStrip::beginLive(gesture::ScalarKind kind, std::int64_t id, gesture::ScalarControlDomain domain,
                           const std::string &label) {
    live_ = false;
    cancelled_ = false;
    if (!panel_.root.beginMixerDrag)
        return;
    // Only a mouse drag: a typed value, a double-click's reset or an accessibility set changes the clicked
    // control alone, committed as before.
    if (!juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        return;
    // The selected tracks go together when the dragged one is among them (a send by its track's place).
    std::vector<std::int64_t> selected;
    const auto picked = panel_.root.arrangement.geometry.selectedTracks(panel_.root.reader());
    const bool together = std::find(picked.begin(), picked.end(), track_) != picked.end() && picked.size() > 1;
    if (together && kind != gesture::ScalarKind::RoutingGain)
        selected = picked;
    // Begun where the rows stand: heard from the first move, and a click that does not move writes nothing.
    live_ = panel_.root.beginMixerDrag({kind, id, selected, domain}, std::nullopt, label);
    if (live_)
        focusIfShowing(*this); // Esc during the drag reaches keyPressed
    else
        panel_.root.say(panel_.root.error().empty() ? "This control cannot be dragged live." : panel_.root.error());
}
bool MixerStrip::endLive() {
    if (cancelled_) {
        // Esc put the values back; the release commits nothing, and the controls show the rows again.
        cancelled_ = false;
        frame(true);
        return true;
    }
    if (!live_)
        return false;
    live_ = false;
    if (panel_.root.endMixerDrag)
        panel_.root.endMixerDrag();
    return true;
}
bool MixerStrip::cancelTitleDrag() {
    if (!std::exchange(titleDrag_, std::nullopt))
        return false;
    panel_.previewTrackDrop(std::nullopt);
    return true;
}
bool MixerStrip::keyPressed(const juce::KeyPress &key) {
    if (key == juce::KeyPress::escapeKey && cancelTitleDrag())
        return true;
    // Esc during a drag puts every value back (MX-23).
    if (key == juce::KeyPress::escapeKey && live_) {
        live_ = false;
        cancelled_ = true;
        if (panel_.root.cancelMixerDrag)
            panel_.root.cancelMixerDrag();
        return true;
    }
    return false;
}
void MixerStrip::commitSend(std::size_t index) {
    // From the CURRENT cell, as every action is (ADR-0201).
    const auto current = panel_.root.current();
    if (!current || index >= sends_.size())
        return;
    auto edit = setSend(*current, track_, returns_[index], sends_[index]->getValue());
    if (!edit.refusal.empty()) {
        // A new row with no routing id left (N39): nothing lands, so the knob goes back to its row's
        // level, fully down without one, and the Status Bar says why.
        sends_[index]->setValue(std::clamp(sendLevel(findSend(current->sends, track_, returns_[index])), kSendFloorDb, kSendTopDb),
                                juce::dontSendNotification);
        panel_.root.say(edit.refusal);
        return;
    }
    panel_.root.submitEdits(std::move(edit.ops));
}
void MixerStrip::enableSend(std::size_t index, bool on) {
    const auto current = panel_.root.current();
    if (!current || index >= sends_.size())
        return;
    auto edit = adi::ui::enableSend(*current, track_, returns_[index], on);
    if (!edit.refusal.empty())
        panel_.root.say(edit.refusal);
    else
        panel_.root.submitEdits(std::move(edit.ops));
}
void MixerStrip::enableAllSends() {
    const auto current = panel_.root.current();
    if (!current)
        return;
    auto edit = adi::ui::enableAllSends(*current, track_, returns_);
    if (!edit.refusal.empty())
        panel_.root.say(edit.refusal);
    else
        panel_.root.submitEdits(std::move(edit.ops));
}
juce::PopupMenu MixerStrip::sendMenuItems(std::size_t index) {
    // Live's Enable Send, ticked while the send is on. A track's sends are on from the start; a
    // return's are off until it is chosen (ADR-0203 d3).
    juce::PopupMenu m;
    if (index >= sends_.size() || index >= returns_.size())
        return m;
    const bool on = sendOn(index);
    m.addItem("Enable Send", true, on, [this, index, on] { enableSend(index, !on); });
    // A return's sends start off; Live's menu also switches them all on at once (§18.4).
    if (kind_ == "return")
        m.addItem("Enable All Sends", [this] { enableAllSends(); });
    // Where this send taps its track (ADR-0245 d10; DF-W33): Live's three, the one it uses ticked.
    if (const auto current = panel_.root.current()) {
        m.addSeparator();
        const auto tap = adi::ui::sendTap(*current, track_, returns_[index]);
        // A plain local for the lambda: Clang 14 rejects a lambda capturing a structured binding.
        for (const auto &place : {std::pair{"Pre FX", IndividualSendTap::PreFx},
                                  std::pair{"Post FX", IndividualSendTap::PostFx},
                                  std::pair{"Post Mixer", IndividualSendTap::PostMixer}}) {
            const IndividualSendTap which = place.second;
            m.addItem(place.first, true, tap == which, [safe = SafePointer<MixerStrip>(this), index, which] {
                if (safe)
                    safe->chooseSendTap(index, which);
            });
        }
    }
    // The envelope belongs to this send's routing row.
    if (const auto current = panel_.root.current())
        if (const auto *send = findSend(current->sends, track_, returns_[index])) {
            panel_.root.addReenableAutomation(m, laneOf(*current, "routing", send->id, "gain_db"));
            panel_.root.addAutomationDeleteItem(m, sendTarget(send->id, "Send Level"));
        }
    return m;
}
void MixerStrip::sendMenu(std::size_t index) {
    auto m = sendMenuItems(index);
    if (m.getNumItems() > 0)
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(sends_[index].get()));
}
void MixerStrip::chooseSendTap(std::size_t index, IndividualSendTap tap) {
    // From the CURRENT cell (ADR-0201), as every send edit.
    const auto current = panel_.root.current();
    if (!current || index >= returns_.size())
        return;
    auto edit = adi::ui::setSendTap(*current, track_, returns_[index], tap);
    if (!edit.refusal.empty()) {
        panel_.root.say(edit.refusal);
        return;
    }
    if (!edit.ops.empty())
        panel_.root.submitEdits(std::move(edit.ops));
}
juce::PopupMenu MixerStrip::soloMenu() {
    // Live's Solo button menu (manual 18.6): Solo in Place, ticked from the project's one field, on
    // by default (the engine's #412 default; Live's factory default is unmeasured, F-02 NOT RUN);
    // the Options menu's entry reads the same field, so the two agree. The pick is the root's one
    // toggle, read at pick time from the CURRENT cell: the other menu, or the Mix View's own
    // window, may have thrown the field while this menu was open, and the pick is never a no-op.
    juce::PopupMenu m;
    m.addItem("Solo in Place", true, panel_.root.reader().soloInPlace(), [safe = SafePointer<MixerStrip>(this)] {
        if (safe)
            safe->panel_.root.toggleSoloInPlace();
    });
    return m;
}
void MixerMenuButton::mouseDown(const juce::MouseEvent &e) {
    PrimaryMouseControl::mouseDown(e);
    if (e.mods.isPopupMenu() && onMenu)
        onMenu();
}
juce::PopupMenu MixerStrip::activatorMenuItems() {
    juce::PopupMenu m;
    const auto target = activatorTarget(track_);
    if (const auto current = panel_.root.current())
        panel_.root.addReenableAutomation(m, laneOf(*current, target.ownerKind, target.ownerId, target.paramRef));
    panel_.root.addAutomationDeleteItem(m, target);
    return m;
}
juce::PopupMenu MixerStrip::panMenuItems() {
    juce::PopupMenu m;
    m.addItem(1, "Stereo Pan", true, true);
    // Live's Split Stereo Pan Mode (ADR-0203 d9) needs a pan mode and two pans per
    // strip in the format and the mixer: the engine's (collab/window.md, N12).
    m.addItem(2, "Split Stereo Pan", false, false);
    // Live's Re-Enable Automation for the pan (AU-04).
    if (const auto current = panel_.root.current())
        panel_.root.addReenableAutomation(m, laneOf(*current, "track", track_, "pan"));
    panel_.root.addAutomationDeleteItem(m, panTarget(track_));
    return m;
}
void MixerStrip::panMenu() {
    panMenuItems().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&pan));
}
// The volume fader's menu: Live's Re-Enable Automation for the track's Volume lane (manual 25.4, AU-04). A
// track with no Volume lane has nothing to offer, and the fader shows no menu.
juce::PopupMenu MixerStrip::volumeMenuItems() {
    juce::PopupMenu m;
    if (const auto current = panel_.root.current())
        panel_.root.addReenableAutomation(m, laneOf(*current, "track", track_, "volume"));
    panel_.root.addAutomationDeleteItem(m, volumeTarget(track_));
    return m;
}
void MixerStrip::volumeMenu() {
    auto m = volumeMenuItems();
    if (m.getNumItems() > 0)
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&volume));
}
MixerPanel::MixerPanel(AdiRootComponent &r, bool vertical) : root(r), vertical_(vertical) {
    setWantsKeyboardFocus(vertical_);
    setMouseClickGrabsKeyboardFocus(false);
}
bool MixerPanel::keyPressed(const juce::KeyPress &key) {
    // The panel keeps focus while scrolling can recycle or hide the source strip.
    if (key != juce::KeyPress::escapeKey)
        return false;
    return std::any_of(strips_.begin(), strips_.end(), [](const auto &strip) { return strip->cancelTitleDrag(); });
}
bool MixerPanel::submit(const std::string &op, Payload payload, const char *label) {
    return root.submitEdit(op, std::move(payload), label);
}
void MixerPanel::resized() { place(); }
void MixerPanel::resolveMeters() {
    for (auto &s : strips_)
        s->resolveMeter();
}
bool MixerPanel::focusTrack(std::int64_t track) {
    if (const auto rows = root.arrangement.geometry.rows(root.reader()); rows.find(track) < rows.size())
        reveal(rows.find(track));
    MixerStrip *target = nullptr;
    for (auto &s : strips_)
        if (s->isVisible() && (target == nullptr || s->track() == track))
            target = s.get();
    if (target == nullptr)
        return false;
    focusIfShowing(target->active);
    return true;
}
void MixerPanel::reveal(std::size_t index) {
    auto &geometry = root.arrangement.geometry;
    const auto tracks = geometry.rows(root.reader());
    if (index >= tracks.size())
        return;
    if (vertical_) {
        // Its column: the tracks the project view shows before it.
        int column = 0;
        for (std::size_t i = 0; i < index; ++i)
            if (geometry.inView(tracks[i].track->id))
                ++column;
        const int left = column * kStripWidth;
        if (left < scrollX_)
            scrollX_ = left;
        else if (left + kStripWidth > scrollX_ + getWidth())
            scrollX_ = left + kStripWidth - getWidth();
        placeColumns();
        return;
    }
    // The returns and Main stand at the foot, always in sight.
    const auto foot = geometry.footStart(tracks);
    if (index >= foot)
        return;
    int top = 0;
    for (std::size_t i = 0; i < index; ++i)
        top += geometry.rowHeight(*tracks[i].track);
    const int bottom = top + geometry.heightFor(tracks[index].track->id);
    const int view = std::max(0, geometry.footTop(root.reader()));
    if (top >= geometry.scrollY && bottom <= geometry.scrollY + view)
        return;
    geometry.scrollY = top < geometry.scrollY ? top : std::min(top, bottom - view);
    root.arrangement.changed();
    place();
}
bool MixerPanel::focusNeighbour(bool next) {
    // Which strip and which of its switches has the focus.
    const auto *focused = getCurrentlyFocusedComponent();
    const MixerStrip *from = nullptr;
    std::size_t slot = 0;
    for (const auto &s : strips_) {
        const auto switches = s->switches();
        if (const auto at = std::find(switches.begin(), switches.end(), focused);
            s->isVisible() && at != switches.end()) {
            from = s.get();
            slot = static_cast<std::size_t>(at - switches.begin());
            break;
        }
    }
    if (from == nullptr)
        return false;
    // The same switch on the track before or after; out of view, it is brought into view
    // first, as Live's.
    const auto &geometry = root.arrangement.geometry;
    const auto tracks = geometry.rows(root.reader());
    // The next or previous track this panel shows (the Mixer View keeps what a folded group
    // holds); at either end the focus stays.
    // Main has no activator, Solo or Arm, as Live's: the focus passes it by.
    auto showing = [this, &geometry, slot](const engine::TrackNode *t) {
        return (vertical_ ? geometry.inView(t->id) : geometry.shown(t->id)) && t->kind != "master" &&
               !(t->kind == "group" && slot == 2);
    };
    const auto i = tracks.find(from->track());
    if (i == tracks.size())
        return true;
    std::size_t to = tracks.size();
    if (next) {
        for (std::size_t j = i + 1; j < tracks.size() && to == tracks.size(); ++j)
            if (showing(tracks[j].track))
                to = j;
    } else {
        for (std::size_t j = i; j-- > 0 && to == tracks.size();)
            if (showing(tracks[j].track))
                to = j;
    }
    if (to == tracks.size())
        return true;
    reveal(to);
    for (const auto &other : strips_)
        if (other->isVisible() && other->track() == tracks[to].track->id)
            focusIfShowing(*other->switches()[slot]);
    return true;
}
MixerPanel::ColumnSections MixerPanel::columnSections(int height) const noexcept {
    const auto &state = root.state();
    ColumnSections sections;
    // The strips share one geometry. Give In/Out and Track Delay their room first, then fit as
    // many send slots as possible while leaving the fader and meter their space.
    int room = height - MixerStrip::kMixerTop - MixerStrip::kMixerRoom;
    if (state.mixerInOut && room >= MixerStrip::kInOutHeight) {
        sections.inOut = MixerStrip::kInOutHeight;
        room -= sections.inOut;
    }
    if (state.mixerTrackOptions && room >= MixerStrip::kDelayHeight) {
        sections.trackDelay = MixerStrip::kDelayHeight;
        room -= sections.trackDelay;
    }
    if (state.mixerSends && room > MixerStrip::kSendPadding) {
        const auto capacity = static_cast<std::size_t>((room - MixerStrip::kSendPadding) / MixerStrip::kSendPitch);
        sections.sends = std::min(sendReturns_.size(), capacity);
    }
    return sections;
}
std::size_t MixerPanel::firstSend(std::size_t shown) const noexcept {
    return shown > 0 && shown < sendReturns_.size() ? std::min(sendOffset_, sendReturns_.size() - shown) : 0;
}
void MixerPanel::placeColumns() {
    // Strips left to right, realising only the columns on screen (UI-ARCHITECTURE §10), in
    // the lanes' order. Only the tracks the project view shows; a folded group's stay, as
    // folding is the timeline's (UI-ARCHITECTURE §4).
    const auto &geometry = root.arrangement.geometry;
    const auto tracks = geometry.rows(root.reader());
    std::size_t count = 0;
    for (const auto t : tracks)
        if (geometry.inView(t.track->id))
            ++count;
    const int total = static_cast<int>(count) * kStripWidth;
    scrollX_ = std::clamp(scrollX_, 0, std::max(0, total - getWidth()));
    const std::size_t first = static_cast<std::size_t>(scrollX_ / kStripWidth);
    const std::size_t wanted =
        std::min(count - std::min(first, count), static_cast<std::size_t>(getWidth() / kStripWidth + 2));
    while (strips_.size() < wanted) {
        auto strip = std::make_unique<MixerStrip>(*this);
        addChildComponent(*strip);
        strips_.push_back(std::move(strip));
    }
    // The n-th strip takes the (first + n)-th track shown.
    std::size_t n = 0, column = 0;
    for (std::size_t i = 0; i < tracks.size() && n < wanted; ++i) {
        const auto id = tracks[i].track->id;
        if (!geometry.inView(id) || column++ < first)
            continue;
        auto &s = *strips_[n++];
        if (s.track() != id && !s.dragging())
            s.bind(id);
        s.setBounds(static_cast<int>(column - 1) * kStripWidth - scrollX_, 0, kStripWidth, getHeight());
        if (!s.isVisible())
            s.resized();
        s.setVisible(true);
    }
    for (; n < strips_.size(); ++n)
        if (!strips_[n]->dragging())
            strips_[n]->setVisible(false);
}
MixerPanel::TitleDrop MixerPanel::trackDropAt(int x, bool movingReturn) const {
    const auto &geometry = root.arrangement.geometry;
    const auto rows = geometry.rows(root.reader());
    const auto foot = geometry.footStart(rows);
    auto at = rows.size();
    int left = -scrollX_, column = left, footX = left, mainX = left;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto &place = rows[i].place;
        if (i == foot)
            footX = column;
        if (place.main)
            mainX = column;
        if (!geometry.inView(place.id))
            continue;
        if (x >= column && x < column + kStripWidth) {
            at = i;
            left = column;
        }
        column += kStripWidth;
    }
    if (foot == rows.size())
        footX = mainX = column;

    if (movingReturn) {
        if (at == rows.size() || rows[at].place.main)
            return {{0, 0}, mainX};
        const auto &place = rows[at].place;
        if (place.ret) {
            if ((x - left) * 2 < kStripWidth)
                return {{place.parent, place.id}, left};
            const auto next = at + 1;
            const auto before = next < rows.size() && rows[next].place.ret && rows[next].place.parent == place.parent
                                    ? rows[next].place.id : 0;
            return {{place.parent, before}, left + kStripWidth};
        }
        if (const auto group = place.group ? place.id : place.parent)
            return {{group, 0}, left, place.group};
        const auto before = foot < rows.size() && rows[foot].place.ret ? rows[foot].place.id : 0;
        return {{0, before}, footX};
    }

    // Main and the top-level returns start after the last ordinary track.
    if (at >= foot)
        return {{0, 0}, footX};
    const auto &place = rows[at].place;
    const int across = x - left;
    if (place.ret)
        return {{place.parent, 0}, left};
    if (across * (place.group ? 4 : 2) < kStripWidth)
        return {{place.parent, place.id}, left};
    if (place.group) {
        const auto next = at + 1;
        const bool open = root.state().foldedGroups.count(place.id) == 0;
        const bool holds = next < foot && rows[next].place.parent == place.id && !rows[next].place.ret;
        if (open && holds)
            return {{place.id, rows[next].place.id}, left + kStripWidth};
        if (across * 4 < kStripWidth * 3)
            return {{place.id, 0}, left, true};
    }
    // Past this track and its children, before its next sibling if there is one.
    auto next = at + 1;
    int right = left + kStripWidth;
    while (next < foot && rows[next].place.depth > place.depth) {
        if (geometry.inView(rows[next].place.id))
            right += kStripWidth;
        ++next;
    }
    const auto before = next < foot && rows[next].place.parent == place.parent ? rows[next].place.id : 0;
    return {{place.parent, before}, right};
}
void MixerPanel::previewTrackDrop(std::optional<TitleDrop> drop) {
    if (!drop && !trackDrop_)
        return;
    trackDrop_ = drop;
    repaint();
}
void MixerPanel::place() {
    if (vertical_) {
        returnsInOrder(root.reader().tracks(), nextSendReturns_);
        const bool returnsChanged = nextSendReturns_ != sendReturns_;
        if (returnsChanged)
            sendReturns_.swap(nextSendReturns_);
        const auto previousOffset = sendOffset_;
        sendOffset_ = firstSend(columnSections(getHeight()).sends);
        if (returnsChanged || sendOffset_ != previousOffset)
            sendWheelFraction_ = 0.f;
        placeColumns();
        if (returnsChanged || sendOffset_ != previousOffset)
            for (auto &strip : strips_)
                if (strip->isVisible()) {
                    strip->resized();
                    strip->repaint();
                }
        return;
    }
    // Realise what is visible and a margin, never a strip per track (UI-ARCHITECTURE
    // §10); rows follow the arrangement's lanes, so the two scroll as one.
    const auto &geometry = root.arrangement.geometry;
    const auto tracks = geometry.rows(root.reader());
    const int bodyTop = root.arrangement.topHeight(); // level with the lanes
    // The returns and Main at the foot, whatever the scroll; a track scrolled under it is out of
    // sight.
    const auto foot = geometry.footStart(tracks);
    const int footY = bodyTop + geometry.footTop(root.reader());
    const int bandEnd = rowsBand().getEnd();
    auto inSight = [&](std::size_t i, int y, int h) {
        return h > 0 && y + h > bodyTop && y < (i < foot ? std::min(footY, bandEnd) : bandEnd);
    };
    std::size_t wanted = 0;
    int y = bodyTop - geometry.scrollY;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i == foot)
            y = footY;
        const int h = geometry.heightFor(tracks[i].track->id);
        if (inSight(i, y, h))
            ++wanted;
        y += geometry.rowHeight(*tracks[i].track);
    }
    const auto addStrip = [&] {
        auto strip = std::make_unique<MixerStrip>(*this);
        addChildComponent(*strip);
        strips_.push_back(std::move(strip));
    };
    while (strips_.size() < wanted)
        addStrip();
    const auto isDragging = [](const auto &strip) { return strip->dragging(); };
    const bool anyDragging = std::ranges::any_of(strips_, isDragging);
    y = bodyTop - geometry.scrollY;
    std::size_t used = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i == foot)
            y = footY;
        const auto id = tracks[i].track->id;
        const int h = geometry.heightFor(id);
        if (inSight(i, y, h) && used < wanted) {
            // A resize can move a pinned Return to an earlier pool slot. Keep the component
            // receiving the pointer with its track, even when different rows come into view.
            if (anyDragging) {
                const auto first = std::next(strips_.begin(), static_cast<std::ptrdiff_t>(used));
                const auto active = std::find_if(first, strips_.end(), [id](const auto &strip) {
                    return strip->dragging() && strip->track() == id;
                });
                if (active != strips_.end()) {
                    std::iter_swap(first, active);
                } else if ((*first)->dragging()) {
                    const auto available = std::find_if_not(first, strips_.end(), isDragging);
                    if (available == strips_.end()) {
                        addStrip();
                        std::swap(strips_[used], strips_.back());
                    } else {
                        std::iter_swap(first, available);
                    }
                }
            }
            auto &s = *strips_[used++];
            if (s.track() != id && !s.dragging())
                s.bind(id);
            s.setBounds(0, y, getWidth(), h);
            s.setVisible(true);
            // The foot's strips stand in front of a track's partly scrolled under them.
            if (i >= foot)
                s.toFront(false);
        }
        y += geometry.rowHeight(*tracks[i].track);
    }
    for (std::size_t i = used; i < strips_.size(); ++i)
        if (!strips_[i]->dragging())
            strips_[i]->setVisible(false);
}
void MixerPanel::frame(bool changed) noexcept {
    if (changed) {
        // A deleted track's Peak Level goes with it, so an id used again starts at -Inf.
        for (auto at = peakLevels.begin(); at != peakLevels.end();)
            at = root.reader().findTrack(at->first) != nullptr ? std::next(at) : peakLevels.erase(at);
        place();
    }
    for (auto &s : strips_)
        if (s->isVisible())
            s->frame(changed);
}
void MixerPanel::paint(juce::Graphics &g) {
    if (vertical_) {
        // Live's mixer: grey right of the last strip.
        g.fillAll(look::panel);
        return;
    }
    // Live's arrangement frame above the rows; the columns' grey under the last track. The
    // strip down the outer edge is the root's SideStrip.
    g.fillAll(look::band);
    const int top = root.arrangement.topHeight();
    const auto &geometry = root.arrangement.geometry;
    // Under the last track down to the foot, or to the bottom without one.
    const int footY = top + geometry.footTop(root.reader());
    int bottom = top - geometry.scrollY + geometry.tracksHeight(root.reader());
    bottom = std::clamp(bottom, top, std::min(footY, getHeight()));
    g.setColour(look::panel);
    g.fillRect(0, bottom, getWidth(), std::max(0, getHeight() - bottom - ArrangementView::kTimeRulerHeight));
    g.setColour(look::separator);
    g.fillRect(0, bottom, 2, getHeight() - bottom);
    paintTop(g);
}
void MixerPanel::paintTop(juce::Graphics &g) const {
    g.setColour(look::band);
    g.fillRect(0, 0, getWidth(), rowsBand().getStart());
}
juce::Range<int> MixerPanel::rowsBand() const noexcept {
    if (vertical_)
        return {0, getHeight()};
    const int top = root.arrangement.topHeight();
    const int lanes = root.arrangement.geometry.lanesHeight;
    return {top, lanes > 0 ? std::min(getHeight(), top + lanes) : getHeight()};
}
void MixerPanel::paintOverChildren(juce::Graphics &g) {
    // The rows scroll with the lanes; above and below them, Live's frame stays in front: a row
    // scrolled partly above the lanes shows no more than the lanes do (UI-ARCHITECTURE §4).
    if (vertical_) {
        if (trackDrop_) {
            g.setColour(look::accent);
            if (trackDrop_->intoGroup)
                g.drawRect(trackDrop_->x, 0, kStripWidth, MixerStrip::kTitleHeight, 2);
            else {
                const int x = std::clamp(trackDrop_->x - 1, 0, std::max(0, getWidth() - 2));
                g.fillRect(x, 0, 2, getHeight());
            }
        }
        return;
    }
    const int bottom = rowsBand().getEnd();
    paintTop(g);
    // Below the lanes, as paint() leaves it: the columns' grey, the frame's band at the foot.
    const int ruler = getHeight() - ArrangementView::kTimeRulerHeight;
    g.setColour(look::panel);
    g.fillRect(0, bottom, getWidth(), std::max(0, ruler - bottom));
    g.setColour(look::band);
    g.fillRect(0, std::max(bottom, ruler), getWidth(), getHeight() - std::max(bottom, ruler));
    g.setColour(look::separator);
    g.fillRect(0, bottom, 2, getHeight() - bottom);
}
TrackHeightButton::TrackHeightButton() : juce::Button("Use Large Track Height") {
    setTitle("Use Large Track Height");
    setTooltip("Use Large Track Height: every track at its own height; off, at half of it.");
    setClickingTogglesState(false);
    // A view command, reached from the keyboard through the View menu: it takes no focus.
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}
void TrackHeightButton::paintButton(juce::Graphics &g, bool over, bool down) {
    g.setColour(getToggleState() ? look::accent : over || down ? look::textBright : look::textOnBand);
    const auto r = getLocalBounds().toFloat().reduced(3.f, 3.f);
    const float w = (r.getWidth() - 2.f) / 2.f, pitch = r.getHeight() / 3.f;
    for (int column = 0; column < 2; ++column)
        for (int bar = 0; bar < 3; ++bar)
            g.fillRect(r.getX() + static_cast<float>(column) * (w + 2.f), r.getY() + static_cast<float>(bar) * pitch, w,
                       std::max(1.f, pitch - 1.5f));
}
SideStrip::SideStrip() { addAndMakeVisible(tracks); }
void SideStrip::setHead(int y) {
    if (y == head_)
        return;
    head_ = y;
    resized();
    repaint();
}
void SideStrip::resized() {
    // Keep the track height control at its position under the names bar.
    tracks.setBounds(4, head_ + 47, 19, 14);
}
void SideStrip::paint(juce::Graphics &g) {
    g.fillAll(look::band);
}
void MixerPanel::mouseDown(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu())
        contextMenu(e.getScreenPosition());
}
void MixerPanel::contextMenu(juce::Point<int> where) {
    juce::PopupMenu m;
    // ADI's addition, shown where a mix is made (ADR-0203 d2, ADR-0195 d4).
    m.addItem("Auto Gain Stage: gated RMS, -18 dBFS",
              [safe = SafePointer<MixerPanel>(this)] {
                  if (safe && safe->autoGain)
                      safe->autoGain(false);
              });
    m.addItem("Auto Gain Stage: integrated loudness, -18 LUFS",
              [safe = SafePointer<MixerPanel>(this)] {
                  if (safe && safe->autoGain)
                      safe->autoGain(true);
              });
    showContextMenu(std::move(m), where);
}
void MixerPanel::showContextMenu(juce::PopupMenu menu, juce::Point<int> where) {
    if (contextMenuPresenter) {
        contextMenuPresenter(std::move(menu));
        return;
    }
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}));
}
void MixerPanel::scrollSendsBy(int rows, std::size_t shown) {
    const auto first = firstSend(shown);
    const auto last = sendReturns_.size() - shown;
    if (rows < 0)
        sendOffset_ = first - std::min(first, static_cast<std::size_t>(-rows));
    else
        sendOffset_ = first + std::min(last - first, static_cast<std::size_t>(rows));
    if (sendOffset_ == first)
        return;
    for (auto &strip : strips_)
        if (strip->isVisible()) {
            strip->layoutSends();
            strip->repaint();
        }
}
bool MixerPanel::scrollSends(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    const auto sections = columnSections(getHeight());
    if (sections.sends == 0 || sections.sends == sendReturns_.size() || w.deltaY == 0.f ||
        std::abs(w.deltaX) > std::abs(w.deltaY) || e.mods.isShiftDown())
        return false;
    const auto at = e.getEventRelativeTo(this).getPosition();
    const int top = MixerStrip::kMixerTop + sections.inOut;
    if (at.y < top || at.y >= top + MixerStrip::sendsHeight(sections.sends) || !getLocalBounds().contains(at) ||
        std::none_of(strips_.begin(), strips_.end(), [&](const auto &strip) {
            return strip->isVisible() && strip->getBounds().contains(at);
        }))
        return false;

    int rows = w.deltaY < 0.f ? 1 : -1;
    if (w.isSmooth) {
        // A trackpad sends fractions of a wheel tick. Accumulate travel until a slot has passed.
        sendWheelFraction_ -= w.deltaY * (300.f / MixerStrip::kSendPitch);
        rows = static_cast<int>(sendWheelFraction_);
        sendWheelFraction_ -= static_cast<float>(rows);
    } else
        sendWheelFraction_ = 0.f;
    if (rows != 0)
        scrollSendsBy(rows, sections.sends);
    const auto first = firstSend(sections.sends);
    if ((w.deltaY > 0.f && first == 0) || (w.deltaY < 0.f && first + sections.sends == sendReturns_.size()))
        sendWheelFraction_ = 0.f;
    return true;
}
void MixerPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (vertical_) {
        if (scrollSends(e, w))
            return;
        sendWheelFraction_ = 0.f;
        // Elsewhere the Mixer View scrolls sideways, whichever way the wheel turns.
        const float delta = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
        scrollX_ -= static_cast<int>(delta * 300);
        previewTrackDrop(std::nullopt);
        placeColumns();
        return;
    }
    // The rows belong to the arrangement's lanes: the wheel scrolls both.
    root.arrangement.wheel(e.getEventRelativeTo(&root.arrangement.canvas), w);
}
MixerViewResizer::MixerViewResizer(AdiRootComponent &r) : root(r) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    setTitle("Mixer View height");
}
void MixerViewResizer::mouseDown(const juce::MouseEvent &) {
    start = root.state().mixerViewHeight;
}
void MixerViewResizer::mouseDrag(const juce::MouseEvent &e) {
    root.resizeMixerView(start - e.getDistanceFromDragStartY());
}
void MixerViewResizer::mouseUp(const juce::MouseEvent &) { root.persist(); }
} // namespace adi::ui
