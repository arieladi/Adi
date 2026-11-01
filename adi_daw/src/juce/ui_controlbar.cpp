// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_controlbar.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
namespace adi::ui {
namespace {
using G = BarItem::Glyph;
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
} // namespace
BarItem::BarItem(const juce::String &t, Glyph gl, bool b, bool d)
    : juce::Button(t), text(t), glyph(gl), boxed(b), dropdown(d) {}
void BarItem::setText(const juce::String &t) {
    if (t != text) {
        text = t;
        repaint();
    }
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
        g.fillRect(r);
    }
    auto content = r;
    if (dropdown) {
        look::triangle(g, {r.getRight() - 9, r.getCentreY() - 2.f, 4, 6},
                       juce::MathConstants<float>::halfPi, look::text);
        content = r.withTrimmedRight(10);
    }
    if (glyph != Glyph::None)
        paintGlyph(g, glyph, content, getToggleState() && !boxed ? look::accent : look::text);
    if (text.isNotEmpty()) {
        g.setColour(look::text);
        g.setFont(look::font(11.5f));
        g.drawText(text, content, juce::Justification::centred);
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
TransportBar::TransportBar(AdiRootComponent &root) : root_(root) {
    struct Info {
        BarItem *item;
        const char *title, *tip;
    };
    // The Info View's words for each item (ADR-0131), in Live's order.
    const Info infos[] = {
        {&link, "Link", "Ableton Link: tempo and phase with other applications (P1)."},
        {&tap, "Tap Tempo", "Tap to set the tempo."},
        {&tempo, "Tempo", "The project's tempo at the playhead, in BPM."},
        {&nudgeDown, "Nudge Down", "Slows playback for as long as it is held."},
        {&nudgeUp, "Nudge Up", "Speeds playback up for as long as it is held."},
        {&signature, "Time Signature", "The project's time signature."},
        {&metronome, "Metronome", "Switches the metronome on and off; it plays to Cue Out."},
        {&quantize, "Quantization Menu", "The global launch quantization (with Session View)."},
        {&follow, "Follow", "The arrangement follows the playhead."},
        {&position, "Arrangement Position", "Where the playhead is, in bars, beats and sixteenths."},
        {&play, "Play", "Starts playback from the insert marker, or stops it. Space."},
        {&stop, "Stop", "Stops playback."},
        {&record, "Arrangement Record", "Records into the arrangement. F9."},
        {&overdub, "MIDI Arrangement Overdub", "Records MIDI on top of what is there."},
        {&automationArm, "Automation Arm", "Records automation while recording."},
        {&reEnable, "Re-Enable Automation", "Brings back automation you overrode."},
        {&capture, "Capture MIDI", "Turns what you just played into a clip."},
        {&sessionRecord, "Session Record", "Records into Session View (P2)."},
        {&loopStart, "Loop Start", "Where the arrangement loop starts."},
        {&punchIn, "Punch-In", "Records only after the loop's start."},
        {&loop, "Loop Switch", "Loops the arrangement."},
        {&punchOut, "Punch-Out", "Records only before the loop's end."},
        {&loopLength, "Loop Length", "How long the arrangement loop is."},
        {&draw, "Draw Mode", "Draws notes and automation with the mouse."},
        {&keyboard, "Computer MIDI Keyboard", "Plays notes from the computer keyboard."},
        {&key, "Key Map Mode", "Assigns computer keys to controls."},
        {&midi, "MIDI Map Mode", "Assigns MIDI controllers to controls."},
        {&cpu, "CPU Load Meter", "How much of the audio thread's time is used."},
        {&indicators, "MIDI Indicators", "Light when MIDI comes in or goes out."}};
    for (const auto &info : infos) {
        info.item->setTitle(info.title);
        info.item->setTooltip(juce::String(info.title) + ". " + info.tip);
        addAndMakeVisible(*info.item);
        // The bar remembers the control that last had the focus; Link until one has.
        info.item->onFocus = [this, item = info.item] { lastFocused_ = item; };
    }
    play.onClick = [this] { root_.command(AppCommands::PlayStop); };
    stop.onClick = [this] { root_.command(AppCommands::Stop); };
    for (auto *item : items())
        if (item != &play && item != &stop)
            item->onClick = [this, item] {
                root_.say(item->getTitle().toStdString() + " is not built yet.");
            };
    automationArm.setToggleState(true, juce::dontSendNotification); // on in Live's default set
}
std::vector<BarItem *> TransportBar::items() {
    return {&link,     &tap,           &tempo,    &nudgeDown,     &nudgeUp,   &signature,
            &metronome, &quantize,     &follow,   &position,      &play,      &stop,
            &record,   &overdub,       &automationArm, &reEnable, &capture,   &sessionRecord,
            &loopStart, &punchIn,      &loop,     &punchOut,      &loopLength, &draw,
            &keyboard, &key,           &midi,     &cpu,           &indicators};
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
    x = getWidth() - 215;
    place(draw, 17, 2);
    place(keyboard, 17, 2);
    place(key, 26, 12);
    place(midi, 28, 2);
    place(cpu, 58, 2);
    place(indicators, 30);
}
void TransportBar::refresh() {
    if (const double bpm = root_.bpm(); bpm != shownBpm_) {
        shownBpm_ = bpm;
        tempo.setText(juce::String(bpm, 2));
    }
    // Bars, beats and sixteenths, as Live shows the position (4/4 until the snapshot
    // carries the meters: window log, N6).
    if (const auto sixteenths = root_.timelineTick() / (textproj::kPPQ / 4);
        sixteenths != shownSixteenths_) {
        shownSixteenths_ = sixteenths;
        position.setText(juce::String(sixteenths / 16 + 1) + ".  " +
                         juce::String(sixteenths / 4 % 4 + 1) + ".  " +
                         juce::String(sixteenths % 4 + 1));
    }
    play.setToggleState(root_.playing(), juce::dontSendNotification);
}
void TransportBar::paint(juce::Graphics &g) { g.fillAll(look::frame); }
void TransportBar::focus() { lastFocused_->grabKeyboardFocus(); }
} // namespace adi::ui
