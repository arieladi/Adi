// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_controlbar.hpp"
#include "adi/ui/cue_edits.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include "adi/ui/tempo_edits.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
namespace adi::ui {
namespace {
using G = BarItem::Glyph;
class MetronomeVolumeRow final : public juce::PopupMenu::CustomComponent {
  public:
    MetronomeVolumeRow(AdiRootComponent &root, double level) : CustomComponent(false) {
        label_.setText("Metronome / Cue", juce::dontSendNotification);
        label_.setColour(juce::Label::textColourId, look::text);
        slider_.setTitle("Metronome and Cue Volume");
        slider_.setSliderStyle(juce::Slider::LinearHorizontal);
        slider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
        slider_.setRange(kCueFloorDb, kCueTopDb, .01);
        slider_.setSkewFactor(2.27);
        slider_.setChangeNotificationOnlyOnRelease(true);
        slider_.textFromValueFunction = [](double db) {
            return db <= kCueFloorDb ? juce::String("-inf") : juce::String(db, 1) + " dB";
        };
        slider_.setValue(std::max(kCueFloorDb, level), juce::dontSendNotification);
        slider_.onValueChange = [this, safe = juce::Component::SafePointer<AdiRootComponent>(&root)] {
            if (safe)
                safe->setCueVolume(slider_.getValue());
        };
        addAndMakeVisible(label_);
        addAndMakeVisible(slider_);
    }
    void getIdealSize(int &width, int &height) override {
        width = 284;
        height = 30;
    }
    void resized() override {
        auto bounds = getLocalBounds().reduced(8, 2);
        label_.setBounds(bounds.removeFromLeft(112));
        slider_.setBounds(bounds);
    }
  private:
    juce::Label label_;
    juce::Slider slider_;
};

void paintGlyph(juce::Graphics &g, G kind, juce::Rectangle<float> r, juce::Colour c) {
    g.setColour(c);
    const float cx = r.getCentreX(), cy = r.getCentreY();
    const float s = std::min(r.getWidth(), r.getHeight());
    juce::Path p;
    switch (kind) {
    case G::Play:
        p.addTriangle(cx - s * 0.32f, cy - s * 0.4f, cx + s * 0.4f, cy, cx - s * 0.32f,
                      cy + s * 0.4f);
        g.fillPath(p);
        break;
    case G::Stop:
        g.fillRect(juce::Rectangle<float>(s * 0.68f, s * 0.68f).withCentre({cx, cy}));
        break;
    case G::Record:
        g.fillEllipse(juce::Rectangle<float>(s * 0.72f, s * 0.72f).withCentre({cx, cy}));
        break;
    case G::Overdub:
        g.fillRect(cx - 5, cy - 0.75f, 10.f, 1.5f);
        g.fillRect(cx - 0.75f, cy - 5, 1.5f, 10.f);
        break;
    case G::Follow:
        g.fillRect(cx - 8, cy - 0.75f, 7.f, 1.5f);
        p.addTriangle(cx - 2, cy - 3.5f, cx + 2, cy, cx - 2, cy + 3.5f);
        g.fillPath(p);
        for (int i = 0; i < 2; ++i)
            g.fillRect(cx + 3.f + static_cast<float>(i) * 3.f, cy - 0.75f, 2.f, 1.5f);
        break;
    case G::Metronome:
        g.drawEllipse(juce::Rectangle<float>(7, 7).withCentre({cx - 10, cy}), 1.2f);
        g.fillEllipse(juce::Rectangle<float>(7, 7).withCentre({cx - 1, cy}));
        break;
    case G::NudgeDown:
    case G::NudgeUp:
        for (int i = 0; i < 4; ++i)
            g.fillRect(cx - 5.f + static_cast<float>(i) * 3.f, cy - 5, 1.2f, 10.f);
        if (kind == G::NudgeUp)
            g.fillRect(cx + 5.f, cy - 5, 1.2f, 10.f);
        break;
    case G::AutomationArm:
        g.drawEllipse(juce::Rectangle<float>(5, 5).withCentre({cx - 3, cy + 3}), 1.2f);
        g.drawEllipse(juce::Rectangle<float>(5, 5).withCentre({cx + 3, cy - 3}), 1.2f);
        g.drawLine(cx - 1.5f, cy + 1.5f, cx + 1.5f, cy - 1.5f, 1.2f);
        break;
    case G::ReEnable:
        g.fillRect(cx - 3, cy - 0.75f, 8.f, 1.5f);
        p.addTriangle(cx - 6, cy, cx - 2, cy - 3.5f, cx - 2, cy + 3.5f);
        g.fillPath(p);
        break;
    case G::Capture:
        for (float d : {-5.f, 3.f}) {
            g.fillRect(cx + d, cy - 5, 2.f, 1.2f);
            g.fillRect(cx + d, cy + 3.8f, 2.f, 1.2f);
        }
        g.fillRect(cx - 5, cy - 5, 1.2f, 3.f);
        g.fillRect(cx + 3.8f, cy - 5, 1.2f, 3.f);
        g.fillRect(cx - 5, cy + 2, 1.2f, 3.f);
        g.fillRect(cx + 3.8f, cy + 2, 1.2f, 3.f);
        break;
    case G::SessionRecord:
        g.drawEllipse(juce::Rectangle<float>(9, 9).withCentre({cx, cy}), 1.4f);
        break;
    case G::PunchIn:
    case G::PunchOut: {
        const float dir = kind == G::PunchIn ? 1.f : -1.f;
        p.startNewSubPath(cx - 6 * dir, cy + 4);
        p.lineTo(cx - 1 * dir, cy + 4);
        p.lineTo(cx + 2 * dir, cy - 4);
        p.lineTo(cx + 6 * dir, cy - 4);
        g.strokePath(p, juce::PathStrokeType(1.3f));
        break;
    }
    case G::Loop:
        p.addRoundedRectangle(cx - 6, cy - 4, 12, 7, 2.f);
        g.strokePath(p, juce::PathStrokeType(1.3f));
        g.fillRect(cx - 1, cy + 2, 2.f, 3.f);
        break;
    case G::Draw:
        p.startNewSubPath(cx - 5, cy + 5);
        p.lineTo(cx + 4, cy - 4);
        g.strokePath(p, juce::PathStrokeType(2.2f));
        g.fillRect(cx - 6, cy + 4.5f, 3.f, 1.2f);
        break;
    case G::Keyboard:
        g.drawRect(juce::Rectangle<float>(12, 10).withCentre({cx, cy}), 1.f);
        for (int i = 0; i < 3; ++i)
            g.fillRect(cx - 4.f + static_cast<float>(i) * 3.5f, cy - 5, 1.2f, 6.f);
        break;
    case G::None:
        break;
    }
}
juce::String formatPosition(BarPosition at, const char *separator) {
    return juce::String(at.bar) + separator + juce::String(at.beat) + separator + juce::String(at.sixteenth);
}
std::int64_t adjustedPosition(std::int64_t tick, const std::vector<textproj::Meter> &meters, int part, int steps) {
    if (part == 0) {
        auto at = barPosition(tick, meters);
        at.bar = std::max<std::int64_t>(1, at.bar + steps);
        if (const auto to = tickOf(at, meters))
            return *to;
        // A shorter or fragmentary bar might not contain the original beat.
        return tickOf({at.bar, 1, 1}, meters).value_or(tick);
    }
    if (part == 2)
        return std::max<std::int64_t>(0, tick + steps * (textproj::kWhole / 16));

    const bool forward = steps > 0;
    for (int i = 0; i < std::abs(steps); ++i) {
        const auto &meter = meterAt(forward ? tick : std::max<std::int64_t>(0, tick - 1), meters);
        const auto beat = textproj::kWhole / meter.denominator;
        if (forward) {
            const auto next = std::upper_bound(meters.begin(), meters.end(), tick,
                                               [](auto at, const auto &m) { return at < m.start_ticks; });
            tick = next == meters.end() ? tick + beat : std::min(tick + beat, next->start_ticks);
        } else {
            tick = std::max(meter.start_ticks, tick - beat);
        }
    }
    return tick;
}
} // namespace
BarItem::BarItem(const juce::String &t, Glyph gl, bool b, bool d)
    : juce::Button(t), text(t), glyph(gl), boxed(b), dropdown(d) {}
void BarItem::setText(const juce::String &t) {
    if (t != text) {
        text = t;
        repaint();
    }
}
void BarItem::setCountInPosition(engine::CountInPosition position) noexcept {
    if (position != countIn_) {
        countIn_ = position;
        repaint();
    }
}
void BarItem::clicked(const juce::ModifierKeys &modifiers) {
    if (onModifiedClick)
        onModifiedClick(modifiers);
}
void BarItem::mouseDown(const juce::MouseEvent &event) {
    dropdownPress_ = onDropdown && (event.mods.isPopupMenu() || event.x >= getWidth() - 10);
    if (dropdownPress_)
        onDropdown();
    else
        juce::Button::mouseDown(event);
}
void BarItem::mouseUp(const juce::MouseEvent &event) {
    if (dropdownPress_) {
        dropdownPress_ = false;
        return;
    }
    juce::Button::mouseUp(event);
}
void BarItem::paintButton(juce::Graphics &g, bool over, bool down) {
    auto r = getLocalBounds().toFloat();
    if (boxed) {
        auto fill = getToggleState() ? look::accent : look::field;
        if (down)
            fill = fill.darker(0.15f);
        else if (over)
            fill = fill.brighter(0.06f);
        g.setColour(fill);
        if (workspaceSegment)
            g.fillRoundedRectangle(r, look::radiusControl);
        else
            g.fillRect(r);
    }
    auto content = r;
    if (dropdown) {
        look::triangle(g, {r.getRight() - 9, r.getCentreY() - 2.f, 4, 6},
                       juce::MathConstants<float>::halfPi, look::text);
        content = r.withTrimmedRight(10);
    }
    if (glyph != Glyph::None)
        paintGlyph(g, glyph, content,
                   !isEnabled() ? look::text.withMultipliedAlpha(.4f) :
                   getToggleState() ? (boxed ? look::ink : look::accent) : look::text);
    if (countIn_.bar < 0) {
        char position[48];
        std::snprintf(position, sizeof(position), "%d.  %d.  %d", countIn_.bar, countIn_.beat, countIn_.sixteenth);
        g.setColour(look::value);
        g.setFont(look::font(look::fontBody));
        g.drawText(juce::String(position), content, juce::Justification::centred);
    } else if (const auto &value = displayText(); value.isNotEmpty()) {
        g.setColour(boxed && getToggleState() ? look::ink : look::text);
        g.setFont(look::font(look::fontBody));
        g.drawText(value, content, juce::Justification::centred);
    }
    if (automationLed || overloadLed) {
        // Live 12's figure: a red dot with a dark rim in the corner (its size to be measured).
        const float y = overloadLed ? r.getCentreY() - 2.5f : r.getY() + 2.f;
        const juce::Rectangle<float> led(r.getX() + 2.f, y, 5.f, 5.f);
        g.setColour(look::recordLed);
        g.fillEllipse(led);
        g.setColour(look::separator);
        g.drawEllipse(led, .8f);
    }
    if (ring_) {
        g.setColour(look::accent);
        g.drawRect(getLocalBounds(), 1);
    }
}
void BarItem::focusGained(FocusChangeType cause) {
    ring_ = cause != focusChangedByMouseClick;
    if (onFocus)
        onFocus();
    juce::Button::focusGained(cause);
}
void BarItem::focusLost(FocusChangeType cause) {
    ring_ = false;
    juce::Button::focusLost(cause);
}
BarValue::BarValue(const juce::String &value, char separator, int parts)
    : BarItem(value), separator_(separator), parts_(parts) {}
float BarValue::partBoundary(int part) const {
    if (part <= 0)
        return 0.f;
    if (part >= parts_)
        return static_cast<float>(getWidth());
    const auto &value = displayText();
    const auto delimiter = static_cast<juce::juce_wchar>(separator_);
    int split = value.indexOfChar(delimiter);
    if (part == 2 && split >= 0)
        split = value.indexOfChar(split + 1, delimiter);
    if (split < 0)
        return static_cast<float>(getWidth()) * static_cast<float>(part) / static_cast<float>(parts_);
    const auto font = look::font(look::fontBody);
    const auto width = [&font](const juce::String &s) { return juce::GlyphArrangement::getStringWidth(font, s); };
    return (static_cast<float>(getWidth()) - width(value) + width(value.substring(0, split)) +
            width(value.substring(0, split + 1))) * .5f;
}
int BarValue::partAt(float x) const {
    int part = 0;
    while (part + 1 < parts_ && x >= partBoundary(part + 1))
        ++part;
    return part;
}
void BarValue::paintButton(juce::Graphics &g, bool over, bool down) {
    BarItem::paintButton(g, over, down);
    if (focusRing()) {
        g.setColour(look::accent);
        const float left = partBoundary(part_) + 2.f, right = partBoundary(part_ + 1) - 2.f;
        g.fillRect(left, static_cast<float>(getHeight() - 3), std::max(0.f, right - left), 1.f);
    }
}
bool BarValue::stepKey(const juce::KeyPress &key) {
    if (!isEnabled() || !adjust || pressed_ || key.getModifiers().isAnyModifierKeyDown())
        return false;
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
        adjust(part_, key == juce::KeyPress::upKey ? 1 : -1, true);
        return true;
    }
    return false;
}
bool BarValue::keyPressed(const juce::KeyPress &key) {
    if (!key.getModifiers().isAnyModifierKeyDown() &&
        (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)) {
        part_ = std::clamp(part_ + (key == juce::KeyPress::rightKey ? 1 : -1), 0, parts_ - 1);
        repaint();
        return true;
    }
    return stepKey(key) || juce::Button::keyPressed(key);
}
void BarValue::mouseDown(const juce::MouseEvent &e) {
    if (!isEnabled() || e.mods.isPopupMenu())
        return;
    part_ = partAt(e.position.x);
    pressed_ = true;
    setState(buttonDown);
}
void BarValue::mouseDrag(const juce::MouseEvent &e) {
    if (!pressed_ || !adjust)
        return;
    const int steps = -e.getDistanceFromDragStartY() / 4;
    if (steps == dragSteps_)
        return;
    dragSteps_ = steps;
    dragged_ = true;
    preview_ = steps != 0 ? adjust(part_, steps, false) : juce::String{};
    repaint();
}
void BarValue::mouseUp(const juce::MouseEvent &e) {
    if (!std::exchange(pressed_, false))
        return;
    const bool inside = getLocalBounds().contains(e.getPosition());
    const bool dragged = std::exchange(dragged_, false);
    const int steps = std::exchange(dragSteps_, 0);
    preview_.clear();
    setState(inside ? buttonOver : buttonNormal);
    repaint();
    if (!isEnabled())
        return;
    if (dragged) {
        if (steps != 0 && adjust)
            adjust(part_, steps, true);
    } else if (inside) {
        internalClickCallback(e.mods);
    }
}
bool BarValue::wheelAt(float x, const juce::MouseWheelDetails &wheel) {
    if (!isEnabled() || !adjust || pressed_ || wheel.deltaY == 0 || wheel.isInertial)
        return false;
    part_ = partAt(x);
    adjust(part_, wheel.deltaY > 0 ? 1 : -1, true);
    repaint();
    return true;
}
void BarValue::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
    if (!wheelAt(e.position.x, wheel))
        juce::Component::mouseWheelMove(e, wheel);
}
void TransportBar::FieldEditor::bind(BarValue &field, const juce::String &value) {
    field_ = &field;
    initial_ = value;
    setText(value, false);
}
void TransportBar::FieldEditor::finishAdjustment() {
    setVisible(false);
    focusIfShowing(*field_);
}
bool TransportBar::FieldEditor::keyPressed(const juce::KeyPress &key) {
    if (field_ && getText() == initial_ && field_->stepKey(key)) {
        finishAdjustment();
        return true;
    }
    return juce::TextEditor::keyPressed(key);
}
void TransportBar::FieldEditor::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) {
    if (field_ && getText() == initial_ && field_->wheelAt(e.position.x, wheel))
        finishAdjustment();
    else
        juce::TextEditor::mouseWheelMove(e, wheel);
}
TransportBar::TransportBar(AdiRootComponent &root) : root_(root) {
    struct Info {
        BarItem *item;
        const char *title, *tip;
        bool available = true;
    };
    // The Info View's words for each item (ADR-0131), in Live's order.
    const Info infos[] = {
        {&link, "Link", "Unavailable until the app can join a Link session.", false},
        {&tap, "Tap Tempo", "Click once every beat: the tempo follows your clicking."},
        {&tempo, "Tempo", "The tempo at the play location, from 20 to 999 BPM. Drag, scroll or use Up and Down "
                          "to adjust whole BPM or hundredths. Click to type."},
        {&nudgeDown, "Nudge Down", "Unavailable until playback supports temporary tempo changes.", false},
        {&nudgeUp, "Nudge Up", "Unavailable until playback supports temporary tempo changes.", false},
        {&signature, "Time Signature", "Drag, scroll or use Up and Down to adjust the numerator or denominator "
                                       "at the play location. Click to type a signature such as 3/4."},
        {&metronome, "Metronome", "O toggles the metronome. Its menu sets the click, Count-In and shared Cue Volume."},
        {&quantize, "Launch Quantization", "Unavailable until Session View can launch clips.", false},
        {&follow, "Follow", "The arrangement follows the playhead while it plays. An edit, a "
                            "sideways scroll or a click on the ruler pauses it. Ctrl+Shift+F "
                            "(Option+Shift+F on the Mac)."},
        {&position, "Arrangement Position", "Drag, scroll or use Up and Down on a number to move the insert marker "
                                            "by bars, beats or sixteenths. Click to type a position such as 5.2.3."},
        {&play, "Play", "Starts playback from the insert marker, or stops it. Space."},
        {&stop, "Stop", "Stops playback. Press while stopped to return to the start."},
        {&record, "Arrangement Record", "Records into the arrangement. F9."},
        {&overdub, "MIDI Arrangement Overdub", "Adds notes to MIDI clips during Arrangement Record."},
        {&automationArm, "Automation Arm", "While on, moving a control during Arrangement Record writes automation."},
        {&reEnable, "Re-Enable Automation", "Lit while a control you moved overrides its automation. "
                                            "Click to follow every envelope again."},
        {&capture, "Capture MIDI", "Turns recently played MIDI into clips."},
        {&sessionRecord, "Session Record", "Unavailable until Session View can record clips.", false},
        {&loopStart, "Loop Start", "Drag, scroll or use Up and Down on a number to move the loop without changing "
                                   "its length. Click to type a start such as 5.1.1."},
        {&punchIn, "Punch-In", "Records from the loop brace's start."},
        {&loop, "Loop Switch", "Loops the arrangement between the loop brace's ends. Ctrl/Cmd+L loops the selection."},
        {&punchOut, "Punch-Out", "Ends the recording at the loop brace's end."},
        {&loopLength, "Loop Length", "Drag, scroll or use Up and Down on a number to move the loop's end. "
                                     "Click to type a length such as 8.0.0."},
        {&draw, "Draw Mode", "Draws notes and automation with the mouse. B."},
        {&keyboard, "Computer MIDI Keyboard", "Plays MIDI notes from the computer keyboard (M). Z and X change the octave. C and V change velocity."},
        {&key, "Key Map Mode", "Unavailable until controls can be mapped to computer keys.", false},
        {&midi, "MIDI Map Mode", "Unavailable until the app can act on MIDI controller mappings.", false},
        {&cpu, "CPU Load Meter", "How much of the audio thread's time is used."},
        {&indicators, "MIDI In", "Lights when MIDI arrives from a port."},
        {&midiOutIndicator, "MIDI Out", "Lights when MIDI is sent to a port."},
        {&arrangementPage, "Arrangement", "Return to the Arrangement workspace (F4)."},
        {&mix, "Mix View", "Open the channel chains and analysers (F4)."}};
    for (const auto &info : infos) {
        info.item->setTitle(info.title);
        info.item->setTooltip(juce::String(info.title) + ". " + info.tip);
        if (!info.available) {
            info.item->setEnabled(false);
            info.item->setAlpha(.45f);
        }
        addAndMakeVisible(*info.item);
        // The bar remembers the control that last had the focus.
        info.item->onFocus = [this, item = info.item] { lastFocused_ = item; };
    }
    for (auto *item : {&play, &stop, &record, &follow, &metronome, &loop})
        item->getProperties().set(controls::iconTooltip, true);
    play.onClick = [this] { root_.command(AppCommands::PlayStop); };
    stop.onClick = [this] { root_.command(AppCommands::Stop); };
    follow.onClick = [this] { root_.command(AppCommands::Follow); };
    arrangementPage.workspaceSegment = mix.workspaceSegment = true;
    arrangementPage.onClick = [this] { if (root_.state().mixView) root_.command(AppCommands::ToggleMixView); };
    mix.onClick = [this] { if (!root_.state().mixView) root_.command(AppCommands::ToggleMixView); };
    tempo.onClick = [this] { beginTempoEdit(); };
    tempo.adjust = [this](int part, int steps, bool commit) -> juce::String {
        const auto current = root_.current();
        if (!current || !current->tempo)
            return {};
        const auto bpm = current->tempo->bpmAt(root_.timelineTick());
        const auto value = std::clamp(bpm + steps * (part == 0 ? 1. : .01), kMinTempo, kMaxTempo);
        const juce::String text(value, 2);
        if (commit && value != bpm)
            root_.setTempoAtPlay(text.toStdString());
        return text;
    };
    // On the press, so each tap is timed when it lands.
    tap.setTriggeredOnMouseDown(true);
    tap.onClick = [this] { root_.tapTempo(root_.input.ms()); };
    signature.onClick = [this] { beginSignatureEdit(); };
    signature.adjust = [this](int part, int steps, bool commit) -> juce::String {
        const auto current = root_.current();
        if (!current)
            return {};
        const auto &meter = meterAt(root_.timelineTick(), current->meters);
        constexpr std::array<int, 5> denominators{1, 2, 4, 8, 16};
        const int at = static_cast<int>(std::distance(denominators.begin(),
                                                     std::find(denominators.begin(), denominators.end(), meter.denominator)));
        const int num = part == 0 ? std::clamp(static_cast<int>(meter.numerator) + steps, 1, 99) : meter.numerator;
        const int den = part == 1 ? denominators[static_cast<std::size_t>(std::clamp(at + steps, 0, 4))]
                                  : meter.denominator;
        if (commit && (num != meter.numerator || den != meter.denominator))
            root_.setMeterAtPlay((juce::String(num) + "/" + juce::String(den)).toStdString());
        return juce::String(num) + " / " + juce::String(den);
    };
    // The metronome and the Arrangement Loop (manual 19.4.1, 6.6; #383, N19): outside undo, saved
    // with the project, lit from the snapshot.
    metronome.onClick = [this] { root_.command(AppCommands::Metronome); };
    metronome.onDropdown = [this] {
        metronomeMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&metronome));
    };
    punchIn.onClick = [this] { root_.togglePunch(true); };
    loop.onClick = [this] { root_.toggleLoop(); };
    punchOut.onClick = [this] { root_.togglePunch(false); };
    loopStart.onClick = [this] { beginLoopStartEdit(); };
    loopLength.onClick = [this] { beginLoopLengthEdit(); };
    position.onClick = [this] { beginPositionEdit(); };
    position.adjust = [this](int part, int steps, bool commit) { return adjustTime(TimeField::Position, part, steps, commit); };
    loopStart.adjust = [this](int part, int steps, bool commit) { return adjustTime(TimeField::LoopStart, part, steps, commit); };
    loopLength.adjust = [this](int part, int steps, bool commit) { return adjustTime(TimeField::LoopLength, part, steps, commit); };
    addChildComponent(fieldEditor);
    fieldEditor.setJustification(juce::Justification::centred);
    fieldEditor.onReturnKey = [this] {
        if (commit_ && commit_(fieldEditor.getText().toStdString()))
            fieldEditor.setVisible(false);
        else
            fieldEditor.selectAll();
    };
    fieldEditor.onEscapeKey = [this] { fieldEditor.setVisible(false); };
    fieldEditor.onFocusLost = [this] { fieldEditor.setVisible(false); };
    // Live 12 manual 25.4: lit while any automated control's manual value stands in for its
    // envelope; a click follows every envelope again, "on tape". Not an undo step.
    reEnable.onClick = [this] {
        if (!root_.reenableAutomation) {
            root_.say("Re-Enable Automation: no session holds automation to re-enable.");
            return;
        }
        root_.reenableAutomation(); // a refusal says why; the snapshot unlights the switch
    };
    automationArm.onClick = [this] {
        if (!root_.automationArmed || !root_.armAutomation)
            return;
        root_.armAutomation(!root_.automationArmed());
        root_.mark(DirtySet::Transport);
    };
    automationArm.setEnabled(false);
    automationArm.setAlpha(.45f);
    // Live 12 manual 21.1: Arrangement Record records every armed track; F9 is the same command.
    record.onModifiedClick = [this](const juce::ModifierKeys &modifiers) {
        root_.command(modifiers.isShiftDown() ? AppCommands::ArrangementRecordAlternate
                                              : AppCommands::ArrangementRecord);
    };
    draw.onClick = [this] { root_.command(AppCommands::DrawMode); };
    capture.setEnabled(false);
    capture.onClick = [this] {
        const bool playing = root_.playing();
        const auto tick = playing ? root_.timelineTick() : root_.arrangement.geometry.selection.insert;
        if (root_.captureRecentMidi) root_.captureRecentMidi(tick, playing);
    };
    keyboard.onClick = [this] { root_.command(AppCommands::ComputerMidiKeyboard); };
    // Manual 21.2: MIDI Arrangement Overdub, the window's own switch; it decides what a MIDI recording
    // does to the notes already there.
    overdub.onClick = [this] {
        const bool on = !root_.midiOverdub();
        root_.setMidiOverdub(on);
        overdub.setToggleState(on, juce::dontSendNotification);
        root_.say(on ? "MIDI Arrangement Overdub on: a MIDI recording will add to the notes already there."
                     : "MIDI Arrangement Overdub off: a MIDI recording will replace the notes it records over.");
    };
    // The MIDI Indicators follow the app's input and output activity.
    indicators.onClick = [this] { indicators.setToggleState(root_.midiIn(), juce::dontSendNotification); };
    midiOutIndicator.onClick = [this] { midiOutIndicator.setToggleState(root_.midiOut(), juce::dontSendNotification); };
    onCpuMenu = [this](juce::PopupMenu menu) {
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&cpu));
    };
    cpu.onClick = [this] {
        if (onCpuMenu)
            onCpuMenu(cpuMenu());
    };
}
void TransportBar::resized() {
    // Live's widths, in points; the left group from the left, the right group from the
    // right, the transport and the loop centred.
    const int y = (getHeight() - look::fieldHeight) / 2, h = look::fieldHeight;
    int x = 8;
    auto place = [&](BarItem &item, int w, int after = 2) {
        item.setBounds(x, y, w, h);
        x += w + after;
    };
    place(link, 43);
    place(tap, 25);
    place(tempo, 41, 4);
    place(nudgeDown, 17);
    place(nudgeUp, 16, 2);
    place(signature, 40);
    place(metronome, 40);
    place(quantize, 45);
    // Live puts the middle group's left edge 297 points left of the window's centre.
    x = std::max(x + 20, getWidth() / 2 - 297);
    place(follow, 21);
    place(position, 82, 18);
    place(play, 18);
    place(stop, 18);
    place(record, 18, 18);
    place(overdub, 17, 1);
    place(automationArm, 17, 1);
    place(reEnable, 17, 1);
    place(capture, 17, 1);
    place(sessionRecord, 17, 12);
    place(loopStart, 85, 1);
    place(punchIn, 20, 1);
    place(loop, 21, 1);
    place(punchOut, 20, 1);
    place(loopLength, 85);
    // Preserve the positions of the right-hand global switches. The
    // workspace labels shorten when the controls to their left need room.
    const auto workspaceRight = getWidth() - 223;
    const auto full = workspaceRight - x >= 2 * look::workspaceSegmentWidth + look::gap;
    const int segmentWidth = full ? look::workspaceSegmentWidth : look::workspaceCompactSegmentWidth;
    x = workspaceRight - 2 * segmentWidth;
    arrangementPage.setText(full ? "Arrangement" : "Arr");
    mix.setText(full ? "Mix View" : "Mix");
    place(arrangementPage, segmentWidth, 0);
    place(mix, segmentWidth, look::gap);
    place(draw, 17, 2);
    place(keyboard, 17, 2);
    place(key, 26, 12);
    place(midi, 28, 2);
    place(cpu, 58, 2);
    place(indicators, 13, 1);
    place(midiOutIndicator, 18, 0);
}
juce::PopupMenu TransportBar::metronomeMenu() const {
    juce::PopupMenu menu;
    const auto current = root_.current();
    const auto safe = juce::Component::SafePointer<AdiRootComponent>(&root_);
    const auto options = current ? current->transport.metronomeOptions : MetronomeOptions{};
    const bool enabled = static_cast<bool>(current);
    menu.addItem("Enable Only While Recording", enabled, enabled && options.recordingOnly, [safe] {
        if (safe)
            safe->toggleMetronomeWhileRecording();
    });
    juce::PopupMenu sounds;
    constexpr std::array soundChoices{std::pair{MetronomeSound::Classic, "Classic"},
                                      std::pair{MetronomeSound::Wood, "Wood"},
                                      std::pair{MetronomeSound::Bell, "Bell"}};
    for (const auto &[sound, label] : soundChoices)
        sounds.addItem(label, enabled, enabled && options.sound == sound, [safe, value = sound] {
            if (safe)
                safe->setMetronomeSound(value);
        });
    menu.addSubMenu("Sound", sounds, enabled);
    const bool eighthNotesFit = current && metronomeEighthNotesFit(meterAt(root_.timelineTick(), current->meters).barTicks());
    const bool playsEighthNotes = options.eighthNotes && eighthNotesFit;
    juce::PopupMenu rhythms;
    rhythms.addItem("Auto", enabled, enabled && !playsEighthNotes, [safe] {
        if (safe)
            safe->setMetronomeEighthNotes(false);
    });
    rhythms.addItem("1/8", eighthNotesFit, playsEighthNotes, [safe] {
        if (safe)
            safe->setMetronomeEighthNotes(true);
    });
    menu.addSubMenu("Rhythm", rhythms, enabled);
    menu.addSeparator();
    menu.addSectionHeader("Count-In");
    constexpr std::array choices{std::pair{0, "None"}, std::pair{1, "1 Bar"},
                                 std::pair{2, "2 Bars"}, std::pair{4, "4 Bars"}};
    for (const auto &[bars, label] : choices) {
        menu.addItem(label, static_cast<bool>(current), current && current->transport.countInBars == bars,
                     [safe, value = bars] {
                         if (safe)
                             safe->setCountIn(value);
                     });
    }
    if (current) {
        menu.addSeparator();
        menu.addCustomItem(100, std::make_unique<MetronomeVolumeRow>(root_, current->cue.volumeDb), nullptr,
                           "Metronome and Cue Volume");
    }
    return menu;
}
void TransportBar::refresh() {
    if (const double bpm = root_.bpm(); bpm != shownBpm_) {
        shownBpm_ = bpm;
        tempo.setText(juce::String(bpm, 2));
    }
    // Bars, beats and sixteenths, as Live shows the position, and the signature in force there,
    // in the project's meters (#299).
    const auto &meters = root_.reader().meters();
    const auto tick = root_.timelineTick();
    if (const auto at = barPosition(tick, meters); at != shownPosition_) {
        shownPosition_ = at;
        position.setText(juce::String(at.bar) + ".  " + juce::String(at.beat) + ".  " + juce::String(at.sixteenth));
    }
    const auto &meter = meterAt(tick, meters);
    if (const auto shown = static_cast<std::uint32_t>(meter.numerator) << 16 | meter.denominator;
        shown != shownSignature_) {
        shownSignature_ = shown;
        signature.setText(juce::String(meter.numerator) + " / " + juce::String(meter.denominator));
    }
    // The automation LED while the arrangement holds a change of signature (manual 6.5).
    if (const bool changes = meters.size() > 1; changes != signature.automationLed) {
        signature.automationLed = changes;
        signature.repaint();
    }
    // The loop's fields (manual 6.6): its start, a position, and its length in bars, beats and
    // sixteenths of the meter at its start; the switches from the transport's saved state.
    const auto &transport = root_.reader().transport();
    if (const auto start = barPosition(transport.loopStartTicks, meters); start != shownLoopStart_) {
        shownLoopStart_ = start;
        loopStart.setText(juce::String(start.bar) + ".  " + juce::String(start.beat) + ".  " + juce::String(start.sixteenth));
    }
    if (const auto length = barSpan(transport.loopStartTicks, transport.loopEndTicks - transport.loopStartTicks, meters);
        length != shownLoopLength_) {
        shownLoopLength_ = length;
        loopLength.setText(juce::String(length.bar) + ".  " + juce::String(length.beat) + ".  " + juce::String(length.sixteenth));
    }
    metronome.setToggleState(transport.metronome, juce::dontSendNotification);
    punchIn.setToggleState(transport.punchIn, juce::dontSendNotification);
    loop.setToggleState(transport.loopEnabled, juce::dontSendNotification);
    punchOut.setToggleState(transport.punchOut, juce::dontSendNotification);
    play.setToggleState(root_.playing(), juce::dontSendNotification);
    record.setToggleState(root_.arrangementRecordEnabled(), juce::dontSendNotification);
    overdub.setToggleState(root_.midiOverdub(), juce::dontSendNotification);
    keyboard.setToggleState(root_.computerMidiEnabled(), juce::dontSendNotification);
    const bool canArmAutomation = root_.automationArmed && root_.armAutomation;
    automationArm.setEnabled(canArmAutomation);
    automationArm.setAlpha(canArmAutomation ? 1.f : .45f);
    automationArm.setToggleState(canArmAutomation && root_.automationArmed(), juce::dontSendNotification);
    indicators.setToggleState(root_.midiIn(), juce::dontSendNotification);
    midiOutIndicator.setToggleState(root_.midiOut(), juce::dontSendNotification);
    follow.setToggleState(root_.state().follow, juce::dontSendNotification);
    // Re-Enable Automation lights from the frame's snapshot (manual 25.4).
    reEnable.setToggleState(root_.reader().automationOverridden(), juce::dontSendNotification);
}
void TransportBar::setCpuLoad(std::optional<CpuLoad> load) {
    if (!load || load->deviceReset)
        setCpuOverload(false);
    if (load && load->missedDeadline)
        setCpuOverload(true);
    cpuLoad_ = load;
    refreshCpuMeter();
}
void TransportBar::refreshCpuMeter() {
    int percent = -1;
    if (cpuLoad_ && cpuMode_ != CpuMode::Off) {
        const double fraction = cpuMode_ == CpuMode::Current ? cpuLoad_->current : cpuLoad_->average;
        if (std::isfinite(fraction) && fraction >= 0)
            percent = static_cast<int>(std::min(std::round(100. * fraction),
                                                static_cast<double>((std::numeric_limits<int>::max)())));
    }
    if (percent != shownCpuPercent_) {
        shownCpuPercent_ = percent;
        cpu.setText(percent < 0 ? juce::String{} : juce::String(percent) + " %");
    }
}
void TransportBar::setCpuOverload(bool on) {
    if (cpu.overloadLed == on)
        return;
    cpu.overloadLed = on;
    cpu.setTooltip(on ? "CPU Load Meter. Audio processing missed a buffer deadline. Open the menu to clear the warning."
                      : "CPU Load Meter. How much of the audio thread's time is used.");
    cpu.repaint();
}
juce::PopupMenu TransportBar::cpuMenu() {
    juce::PopupMenu menu;
    const juce::Component::SafePointer<TransportBar> safe(this);
    auto addMode = [&](const char *label, CpuMode mode) {
        menu.addItem(label, true, cpuMode_ == mode, [safe, mode] {
            if (safe) {
                safe->cpuMode_ = mode;
                safe->refreshCpuMeter();
            }
        });
    };
    addMode("Average", CpuMode::Average);
    addMode("Current", CpuMode::Current);
    addMode("Off", CpuMode::Off);
    if (cpu.overloadLed) {
        menu.addSeparator();
        menu.addItem("Clear Overload Warning", [safe] {
            if (safe)
                safe->setCpuOverload(false);
        });
    }
    return menu;
}
void TransportBar::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    const auto group = arrangementPage.getBounds().getUnion(mix.getBounds()).toFloat();
    g.setColour(look::field);
    g.fillRoundedRectangle(group.expanded(look::hairline), look::radiusGroup);
}
void TransportBar::focus() {
    auto &item = lastFocused_ && lastFocused_->isEnabled() ? *lastFocused_ : link.isEnabled() ? link : tap;
    focusIfShowing(item);
}
juce::String TransportBar::adjustTime(TimeField field, int part, int steps, bool commit) {
    const auto current = root_.current();
    if (!current)
        return {};
    const auto &meters = current->meters;
    const auto range = loopOf(current->transport);
    if (field == TimeField::LoopLength) {
        const auto &meter = meterAt(range.start, meters);
        const auto sixteenth = textproj::kWhole / 16;
        const auto unit = part == 0 ? meter.barTicks() : part == 1 ? textproj::kWhole / meter.denominator : sixteenth;
        const auto length = std::max(std::min(range.length(), sixteenth), range.length() + steps * unit);
        if (commit && length != range.length())
            root_.setLoopRange({range.start, range.start + length});
        return formatPosition(barSpan(range.start, length, meters), ".  ");
    }
    const auto tick = field == TimeField::LoopStart ? range.start
                                                   : root_.playing() ? root_.timelineTick()
                                                                     : root_.arrangement.geometry.selection.insert;
    const auto value = adjustedPosition(tick, meters, part, steps);
    if (commit && value != tick) {
        if (field == TimeField::Position)
            root_.arrangement.placeInsertMarker(value);
        else
            root_.setLoopRange({value, value + range.length()});
    }
    return formatPosition(barPosition(value, meters), ".  ");
}
void TransportBar::beginFieldEdit(BarValue &item, const juce::String &text,
                                  std::function<bool(const std::string &)> commit) {
    editing_ = &item;
    commit_ = std::move(commit);
    fieldEditor.setTitle(item.getTitle());
    fieldEditor.setBounds(item.getBounds());
    fieldEditor.bind(item, text);
    fieldEditor.setVisible(true);
    fieldEditor.selectAll();
    if (fieldEditor.isShowing())
        focusIfShowing(fieldEditor);
}
void TransportBar::beginTempoEdit() {
    beginFieldEdit(tempo, juce::String(root_.bpm(), 2), [this](const std::string &t) { return root_.setTempoAtPlay(t); });
}
void TransportBar::beginPositionEdit() {
    const auto at = barPosition(root_.timelineTick(), root_.reader().meters());
    beginFieldEdit(position, formatPosition(at, "."),
                   [this](const std::string &t) { return root_.setPositionAt(t); });
}
void TransportBar::beginLoopStartEdit() {
    const auto at = barPosition(root_.reader().transport().loopStartTicks, root_.reader().meters());
    beginFieldEdit(loopStart, formatPosition(at, "."),
                   [this](const std::string &t) { return root_.setLoopStartAt(t); });
}
void TransportBar::beginLoopLengthEdit() {
    const auto &t = root_.reader().transport();
    const auto length = barSpan(t.loopStartTicks, t.loopEndTicks - t.loopStartTicks, root_.reader().meters());
    beginFieldEdit(loopLength, formatPosition(length, "."),
                   [this](const std::string &text) { return root_.setLoopLengthAt(text); });
}
void TransportBar::beginSignatureEdit() {
    const auto &meter = meterAt(root_.timelineTick(), root_.reader().meters());
    beginFieldEdit(signature, juce::String(meter.numerator) + "/" + juce::String(meter.denominator),
                   [this](const std::string &t) { return root_.setMeterAtPlay(t); });
}
} // namespace adi::ui
