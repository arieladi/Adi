// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixer.hpp"
#include "ui_shell.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/send_edits.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>
namespace adi::ui {
namespace {
// The Solo's half-coloured state (look::LiveLook draws it), named once.
const juce::Identifier kHalfLit{"live.half"};
// Live's fader runs from -inf to +6 dB; ADI's bottom stop is -inf. What the bottom stop writes is
// silence as the format defines it, at or below -150 dB (SPEC §6.9), never the -70 dB it stands
// on: -70 dB is quiet, not silent.
constexpr double kFloorDb = -70, kTopDb = 6, kSilenceDb = -150;
double writtenVolume(double db) noexcept { return db <= kFloorDb ? kSilenceDb : db; }
// Live 11's meter, measured: +6 dB at the top, then 0, -12, -24, -36, -48 and -60 dB
// at these depths; the scale is wider near 0 dB, as Live's.
constexpr std::array<std::pair<double, double>, 7> kScale{
    {{6, 0}, {0, .06}, {-12, .28}, {-24, .5}, {-36, .71}, {-48, .87}, {-60, 1}}};
// One of Live's In/Out choosers: its box, its label cut at the box's edge with no ellipsis, as Live
// cuts "All Channe" (Live 12 manual, Figure 6.1), and its arrow; an empty one has no arrow.
void drawChooser(juce::Graphics &g, juce::Rectangle<float> r, const char *label, bool enabled) {
    look::box(g, r, look::chooser);
    g.setColour(enabled ? look::text : look::textDim);
    g.setFont(look::font(11.5f));
    g.drawText(label, r.withTrimmedLeft(3).withTrimmedRight(12), juce::Justification::centredLeft, false);
    if (*label != 0)
        look::triangle(g, {r.getRight() - 10, r.getY() + 4, 7, 5}, juce::MathConstants<float>::halfPi,
                       enabled ? look::text : look::textDim);
}
// Live's monitoring switch: In, Auto (lit) and Off, `gap` apart.
void drawMonitor(juce::Graphics &g, float x, float y, float gap) {
    const std::array<std::pair<const char *, int>, 3> monitor{{{"In", 16}, {"Auto", 23}, {"Off", 19}}};
    for (const auto &[label, width] : monitor) {
        const juce::Rectangle<float> r(x, y, static_cast<float>(width), 13);
        const bool on = std::string_view(label) == "Auto";
        look::box(g, r, on ? look::accent : look::button);
        g.setColour(look::text);
        g.setFont(look::font(11.5f));
        g.drawText(label, r, juce::Justification::centred);
        x += static_cast<float>(width) + gap;
    }
}
} // namespace
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
void PeakHold::paintButton(juce::Graphics &g, bool over, bool) {
    // Live's pill: light, red once the track has clipped.
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(held >= 1 ? juce::Colour(0xffff3a34) : over ? look::chooser.brighter(0.1f) : look::chooser);
    g.fillRoundedRectangle(r, r.getHeight() / 2);
    g.setColour(look::outline);
    g.drawRoundedRectangle(r, r.getHeight() / 2, 1.f);
    g.setColour(look::text);
    g.setFont(look::font(11.5f));
    g.drawText(held <= 1.e-6f ? juce::String("-Inf") : juce::String(20 * std::log10(held), 1), r,
               juce::Justification::centred);
}
MixerStrip::MixerStrip(MixerPanel &panel) : panel_(panel) {
    if (panel.vertical()) {
        // Live's Mixer View: the fader is the ◁ beside the meter, on the meter's scale.
        volume.setSliderStyle(juce::Slider::LinearVertical);
        volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volume.getProperties().set("live.meterFader", true);
        volume.getProperties().set("live.faderX", 15);
        volume.setColour(juce::Slider::textBoxTextColourId, look::text);
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(peakHold);
        active.getProperties().set("live.font", 15.0); // Live's big number in its mixer
    } else {
        // Live's value boxes: the number in the box, the volume's filled in blue.
        for (auto *s : {&volume, &pan}) {
            s->setSliderStyle(juce::Slider::LinearBar);
            s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 0, 0);
            s->setSliderSnapsToMousePosition(false);
            s->setColour(juce::Slider::textBoxTextColourId, look::text);
        }
    }
    pan.getProperties().set("live.bipolar", true);
    if (panel.vertical())
        volume.setNormalisableRange(juce::NormalisableRange<double>(
            kFloorDb, kTopDb,
            [](double, double, double proportion) { return meterLevel(1 - proportion); },
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
    for (auto *s : {&volume, &pan}) {
        addAndMakeVisible(*s);
        // The wheel only scrolls, as Live (lookfeel.wheelChangesParameters).
        s->setScrollWheelEnabled(false);
        s->onDragStart = [this] { dragging_ = true; };
    }
    // A drag is one undo step: it commits when the mouse lets go. Typed and
    // double-click changes commit at once.
    volume.onDragEnd = [this] {
        dragging_ = false;
        panel_.submit("mixer.setVolume", {{"id", track_}, {"db", writtenVolume(volume.getValue())}},
                      "Change Track Volume");
    };
    pan.onDragEnd = [this] {
        dragging_ = false;
        panel_.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}},
                      "Change Track Panning");
    };
    volume.onValueChange = [this] {
        if (!dragging_)
            panel_.submit("mixer.setVolume", {{"id", track_}, {"db", writtenVolume(volume.getValue())}},
                          "Change Track Volume");
    };
    pan.onValueChange = [this] {
        if (!dragging_)
            panel_.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}},
                          "Change Track Panning");
    };
    active.setTitle("Track Activator");
    active.setTooltip("Turns the track's output on or off.");
    solo.setTitle("Solo");
    solo.setTooltip("Solos the track.");
    arm.setTitle("Arm Recording");
    arm.setTooltip("Arms the track for recording.");
    eq.setTitle("ADI-Q");
    eq.setTooltip("The channel EQ, built in and off until a band is touched (ADR-0204 d1).");
    limiter.setTitle("ADI-L");
    limiter.setTooltip("The channel limiter, built in and off (ADR-0204 d1).");
    for (juce::TextButton *b : {&active, &solo, static_cast<juce::TextButton *>(&arm), &eq, &limiter}) {
        addAndMakeVisible(*b);
        b->setColour(juce::TextButton::buttonColourId, look::button);
        // Only the keyboard focuses a mixer switch, so a focused one is ringed (Live 12).
        b->setMouseClickGrabsKeyboardFocus(false);
        b->getProperties().set("live.focusRing", true);
    }
    // Live's switches when on: the activator orange, Solo blue, Arm red.
    solo.setColour(juce::TextButton::buttonOnColourId, look::value);
    solo.getProperties().set(kHalfLit, false);
    arm.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffff3a34));
    pan.addMouseListener(this, false);
    active.setClickingTogglesState(false);
    // Live's Track Activator (manual 18.1): the selected tracks together when this one is among them.
    active.onClick = [this] { panel_.root.activateTrack(track_); };
    // Live's Solo (manual 18.1): exclusive unless Ctrl/Cmd is held; the selected tracks together.
    solo.onClick = [this] { panel_.root.soloTrack(track_, juce::ModifierKeys::currentModifiers.isCommandDown()); };
    arm.onClick = [this] { panel_.root.say("Arm Recording is not built yet."); };
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
    prePost.getProperties().set("live.font", 11.0);
    prePost.onClick = [this] { throwPrePost(track_); };
    addChildComponent(prePost);
    // Present but off: the slots exist on every strip; what fills them is ADI-Q and
    // ADI-L from the clones lane, in the engine's channel slots (LANES §3 item 7).
    // A slot opens the Mix View's STACK on its track, where every channel's EQ and limiter
    // are seen together (ADR-0204 d1's one window, ADR-0209 d11), and says what it waits for.
    auto open = [this](const char *why) {
        panel_.root.arrangement.geometry.selection.track = track_;
        panel_.root.publishSelection();
        panel_.root.command(AppCommands::ShowChannelEq);
        panel_.root.say(why);
    };
    eq.onClick = [open] { open("The channel EQ slot is off until ADI-Q is handed over."); };
    limiter.onClick = [open] { open("The channel limiter slot is off until ADI-L is handed over."); };
}
void ArmButton::paintButton(juce::Graphics &g, bool over, bool down) {
    getLookAndFeel().drawButtonBackground(g, *this, findColour(buttonColourId), over, down);
    // Live's Arm switch: a dark dot, light on red when armed.
    g.setColour(getToggleState() ? look::text : look::band);
    g.fillEllipse(getLocalBounds().toFloat().withSizeKeepingCentre(8, 8));
}
void MixerStrip::bind(std::int64_t id) {
    track_ = id;
    position_ = 0;
    tap_ = panel_.tap ? panel_.tap(id) : nullptr;
    peak_ = rms_ = 0;
    frame(true);
}
void MixerStrip::frame(bool changed) noexcept {
    const auto *t = panel_.root.reader().findTrack(track_);
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
            // A return's activator shows its letter, as Live's ("A", "B"); a track's its number. The
            // text changes only when the label does, so a steady frame allocates nothing.
            const int label = kind_ == "return"
                                  ? -1 - static_cast<int>(std::find(returns_.begin(), returns_.end(), track_) - returns_.begin())
                                  : static_cast<int>(position_);
            if (label != label_) {
                label_ = label;
                active.setButtonText(label < 0 ? juce::String::charToString(static_cast<juce::juce_wchar>('A' + (-1 - label) % 26))
                                               : juce::String(label));
            }
            const auto parent = rows[at].place.parent;
            const auto group = parent == 0 ? rows.size() : rows.find(parent);
            if (const auto place = group < rows.size() ? group + 1 : 0; place != group_) {
                group_ = place;
                repaint();
            }
        }
    }
    // The selected tracks' controls are lighter, as Live's, a selection across tracks lighting each.
    const bool selected = track_ != 0 && panel_.root.arrangement.geometry.inSelection(panel_.root.reader(), track_);
    if (selected != selected_) {
        selected_ = selected;
        repaint();
    }
    if (changed && t && !dragging_) {
        volume.setValue(std::max(kFloorDb, static_cast<double>(t->volumeDb)),
                        juce::dontSendNotification);
        pan.setValue(t->pan, juce::dontSendNotification);
        active.setToggleState(!t->muted, juce::dontSendNotification);
        solo.setToggleState(t->soloed, juce::dontSendNotification);
        // A group holding a soloed track shows its Solo half coloured (Live 12 manual 18.3). The
        // property is set once at construction, so a frame only changes its value.
        const bool half = kind_ == "group" && !t->soloed && holdsSoloed(panel_.root.reader().tracks(), track_);
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
        const auto &reader = panel_.root.reader();
        auto showPost = [&reader](juce::TextButton &b, std::int64_t ret) {
            const bool post = !returnPre(reader.findTrack(ret));
            if (post != b.getToggleState() || post != (b.getButtonText() == "Post")) {
                b.setButtonText(post ? "Post" : "Pre");
                b.setToggleState(post, juce::dontSendNotification);
            }
        };
        if (kind_ == "return")
            showPost(prePost, track_);
        for (std::size_t i = 0; i < returnSwitches_.size(); ++i)
            showPost(*returnSwitches_[i], returns_[i]);
        repaint();
    }
    // The meter reads the newest frames of the track's tap: wait-free, no allocation.
    float p = 0, r = 0;
    std::int64_t stamp = 0;
    if (tap_ && tap_->read(left_, right_, stamp)) {
        double energy = 0;
        for (std::size_t i = 0; i < left_.size(); ++i) {
            p = std::max(p, std::max(std::abs(left_[i]), std::abs(right_[i])));
            energy += static_cast<double>(left_[i]) * left_[i] +
                      static_cast<double>(right_[i]) * right_[i];
        }
        r = static_cast<float>(std::sqrt(energy / static_cast<double>(2 * left_.size())));
    }
    if (panel_.vertical() && p > peakHold.held) {
        peakHold.held = p;
        peakHold.repaint();
    }
    if (p != peak_ || r != rms_) {
        peak_ = p;
        rms_ = r;
        repaint(panel_.vertical() ? meterArea().expanded(1)
                                  : juce::Rectangle<int>((showsInOut() ? MixerPanel::kInOutWidth : 0) + 105, 0,
                                                         10, getHeight()));
    }
}
void MixerStrip::resized() {
    if (panel_.vertical()) {
        // Live 11's strip, measured: the name, ADI's slots where Live has the sends, then
        // the mixer: the peak level at its head; pan, the activator, Solo and Arm down
        // to its foot; the meter with the fader beside them.
        // A row of the Sends section for each return, on every strip, Main's too, so the mixers
        // line up as Live's; as many as leave the mixer its room. Live scrolls the rest, ADI leaves
        // them out.
        // Live 12's In/Out section under ADI's slots, when the Mixer Controls show it and the strip
        // has room for it and the mixer; then as many rows of sends as leave the mixer its room.
        io_ = panel_.root.state().mixerInOut && getHeight() - kMixerTop - kInOutHeight >= kMixerRoom ? kInOutHeight : 0;
        shown_ = showsSends() ? returnCount_ : 0;
        while (shown_ > 0 && getHeight() - sendsTop() - sendsHeight(shown_) < kMixerRoom)
            --shown_;
        for (std::size_t i = 0; i < sends_.size(); ++i) {
            sends_[i]->setVisible(i < shown_);
            sends_[i]->setBounds(sendBounds(i));
        }
        // Main's Sends section holds each return's Pre/Post switch where a track has its knobs.
        for (std::size_t i = 0; i < returnSwitches_.size(); ++i) {
            const auto b = sendBounds(i);
            returnSwitches_[i]->setVisible(i < shown_);
            returnSwitches_[i]->setBounds(b.getX(), b.getY() + 9, 34, 14);
        }
        // Live's Mixer: a return has its letter and Solo but no Arm; Main none of the three.
        active.setVisible(kind_ != "master");
        solo.setVisible(kind_ != "master");
        arm.setVisible(kind_ != "master" && kind_ != "return" && kind_ != "group");
        prePost.setVisible(false);
        // A MIDI track with no instrument has no mixer but its switches and its MIDI meter.
        volume.setVisible(audio_);
        pan.setVisible(audio_);
        peakHold.setVisible(audio_);
        const int top = mixerTop(), bottom = getHeight();
        eq.setBounds(4, 22, 29, 13);
        limiter.setBounds(37, 22, 29, 13);
        peakHold.setBounds(11, top + 5, 35, 12);
        pan.setBounds(15, bottom - 91, 27, 27);
        active.setBounds(18, bottom - 59, 21, 21);
        solo.setBounds(17, bottom - 33, 23, 12);
        arm.setBounds(17, bottom - 16, 23, 12);
        const bool tall = getHeight() >= MixerPanel::kTallHeight;
        if (tall != (volume.getTextBoxPosition() == juce::Slider::TextBoxBelow))
            volume.setTextBoxStyle(tall ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                                   false, tall ? 40 : 0, tall ? 15 : 0);
        const auto meter = meterArea();
        volume.setBounds(meter.getX() - 2, meter.getY(), tall ? 40 : 26,
                         meter.getHeight() + (tall ? 15 : 0));
        return;
    }
    // Live's rows: 13 pt boxes on a 17 pt pitch, 3 pt from the track's top. A row
    // shows only when the track is tall enough for it.
    const int x = showsInOut() ? MixerPanel::kInOutWidth : 0;
    const int rows = std::max(1, (getHeight() - 1) / 17);
    auto place = [rows](juce::Component &c, int row, juce::Rectangle<int> r) {
        c.setVisible(row < rows);
        c.setBounds(r);
    };
    if (kind_ == "master") {
        // Live's Main row: no activator, Solo or Arm; its volume where a track has its activator.
        for (juce::Component *c : std::initializer_list<juce::Component *>{&active, &solo, &arm, &prePost})
            c->setVisible(false);
        place(volume, 0, {x + 4, 3, 45, 13});
        place(pan, 1, {x + 54, 20, 45, 13});
    } else {
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
        place(volume, 1, {x + 4, 20, 35, 13});
        place(pan, 1, {x + 54, 20, 45, 13});
        // A MIDI track with no instrument: no volume or pan, as Live's.
        if (!audio_) {
            volume.setVisible(false);
            pan.setVisible(false);
        }
    }
    // Live's sends on the rows under volume and pan, its 29 pt boxes three to a row; ADI's slots
    // on the row after them, or under volume and pan while the sends are hidden.
    const bool sends = showsSends();
    for (std::size_t i = 0; i < sends_.size(); ++i) {
        const int row = 2 + static_cast<int>(i / 3), column = static_cast<int>(i % 3);
        place(*sends_[i], row, {x + 4 + 33 * column, 3 + 17 * row, 29, 13});
        if (!sends)
            sends_[i]->setVisible(false);
    }
    const int slots = (audio_ ? 2 : 1) + (sends ? sendRows() : 0);
    place(eq, slots, {x + 4, 3 + 17 * slots, 29, 13});
    place(limiter, slots, {x + 37, 3 + 17 * slots, 29, 13});
}
juce::Rectangle<int> MixerStrip::sendBounds(std::size_t index) const noexcept {
    // Live's Mixer View sends: A on the left, B to its right and lower, C under A, each 22 pt under
    // the one before; the knobs a little larger than pan's.
    const int i = static_cast<int>(index);
    return {(i % 2 == 0) ? 10 : 51, sendsTop() + 6 + 22 * i, 32, 32};
}
juce::Rectangle<int> MixerStrip::meterArea() const noexcept {
    // Under a tall strip's numeric field, the meter stops short of the foot.
    const bool tall = getHeight() >= MixerPanel::kTallHeight;
    return {53, mixerTop() + 6, 11, std::max(0, getHeight() - mixerTop() - 10 - (tall ? 15 : 0))};
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
    if (panel_.vertical()) {
        // Live's strip: grey, lighter when its track is selected; the name in the track's
        // colour; dark rules between the parts and down the right-hand edge.
        const int w = getWidth() - 2;
        g.fillAll(selected_ ? look::laneSelectedAlt : look::panel);
        g.setColour(look::separator);
        g.fillRect(w, 0, 2, getHeight());
        g.fillRect(0, 17, w, 2);
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
            g.setFont(look::font(11.5f));
            if (!sends_.empty() || !returnSwitches_.empty())
                g.drawText("Sends", 46, sendsTop() + 6, w - 50, 15, juce::Justification::centredRight);
            for (std::size_t i = 0; i < std::min(shown_, sends_.size()); ++i) {
                const auto b = sendBounds(i);
                g.setColour(look::text.withAlpha(sendOn_[i] != 0 ? 1.f : .45f));
                g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i % 26)), b.getX(),
                           b.getBottom() - 9, b.getWidth(), 12, juce::Justification::centred);
            }
        }
        if (position_ > 0) {
            g.setColour(panel_.root.arrangement.colourFor(position_ - 1));
            g.fillRect(0, 0, w, 17);
            // A track in a group: a band in the group's colour along its top, as Live's Session
            // View draws what a group holds (manual 18.3).
            if (group_ > 0) {
                g.setColour(panel_.root.arrangement.colourFor(group_ - 1));
                g.fillRect(0, 0, w, 3);
            }
            if (const auto *track = panel_.root.reader().findTrack(track_)) {
                g.setColour(look::text);
                g.setFont(look::font(11.5f, true));
                g.drawText(juce::String::fromUTF8(displayName(track->name, position_).c_str()), 4, 0, w - 8,
                           17, juce::Justification::centredLeft);
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
    if (audio_)
        paintMeter(g, {x + 105, 1, 8, std::max(0, getHeight() - 4)});
    else
        paintMidiMeter(g, {x + 105, 1, 8, std::max(0, getHeight() - 4)});
}
void MixerStrip::paintMidiMeter(juce::Graphics &g, juce::Rectangle<int> area) const {
    // Live's MIDI meter: a column of dots down the meter's place that light while notes play (Live 12
    // manual, the Mixer in Arrangement View: 2 MIDI). ADI has no note activity to show yet, so they
    // stay dark.
    const float d = std::min(5.5f, static_cast<float>(area.getWidth())), pitch = d + 2.5f;
    const float x = static_cast<float>(area.getCentreX()) - d / 2;
    g.setColour(look::display);
    for (float y = static_cast<float>(area.getY()); y + d <= static_cast<float>(area.getBottom()); y += pitch)
        g.fillEllipse(x, y, d, d);
}
void MixerStrip::paintInOut(juce::Graphics &g, int x) const {
    // The In/Out column as Live draws an audio track's: where it records from, the
    // monitoring switch, where it plays to. The snapshot does not carry a track's
    // routing yet (window log, N11): the column shows ADI's fixed routing, every
    // track to the Main track.
    const int rows = std::max(1, (getHeight() - 1) / 17);
    auto chooser = [&](int row, const char *label, bool enabled) {
        if (row < rows)
            drawChooser(g, {static_cast<float>(x + 3), static_cast<float>(3 + 17 * row), 70, 13}, label, enabled);
    };
    // Main plays to the outputs, a return and a group to Main or their group; none records, so none
    // has an input or the monitoring switch, as Live's (manual 18.3: a group "cannot contain clips").
    if (kind_ == "master" || kind_ == "return" || kind_ == "group") {
        chooser(0, outputLabel(), true);
        return;
    }
    // A MIDI track records from All Ins on All Channels, as Live's; an audio track from Ext. In.
    chooser(0, midi_ ? "All Ins" : "Ext. In", true);
    chooser(1, midi_ ? "All Channels" : "1/2", midi_);
    if (rows > 2)
        drawMonitor(g, static_cast<float>(x + 3), 37, 6);
    chooser(3, outputLabel(), true);
    chooser(4, "", false);
}
const char *MixerStrip::outputLabel() const noexcept {
    // Main plays to the outputs; a MIDI track with no instrument to nothing (No Output); a track in a
    // group to its group, as Live assigns a grouped track's Audio To (manual 18.3), shown as Live's
    // "Group"; every other track to Main. ADI's fixed routing: the snapshot carries none yet
    // (look-live11 row 5).
    if (kind_ == "master")
        return "1/2";
    if (!audio_)
        return "No Output";
    return inGroup_ ? "Group" : "Master";
}
void MixerStrip::paintMixerInOut(juce::Graphics &g) const {
    // Live 12's In/Out section in the Mixer (manual, the Mixer in Arrangement View): where the track
    // records from, its monitoring, where it plays to, each under its label; a return's output only,
    // level with the tracks'; Main's Cue Out and Main Out. ADI's fixed routing, as the track
    // controls' column shows it (the routing is not in the snapshot yet, look-live11 row 5).
    const int top = kMixerTop, w = getWidth() - 2;
    auto label = [&](int y, const char *text) {
        g.setColour(look::text);
        g.setFont(look::font(11.5f, true));
        g.drawText(text, 5, top + y, w - 10, 13, juce::Justification::centredLeft);
    };
    auto chooser = [&](int y, const char *text, bool enabled) {
        drawChooser(g, {4.f, static_cast<float>(top + y), static_cast<float>(w - 8), 13.f}, text, enabled);
    };
    if (kind_ == "master") {
        // Live's Cue Out waits on the cue path (N23, N43), so it shows dimmed.
        label(60, "Cue Out");
        chooser(73, "1/2", false);
        label(90, "Main Out");
        chooser(103, "1/2", true);
        return;
    }
    if (kind_ == "return" || kind_ == "group") {
        label(74, "Audio To");
        chooser(87, outputLabel(), true);
        chooser(103, "", false);
        return;
    }
    label(2, midi_ ? "MIDI From" : "Audio From");
    chooser(15, midi_ ? "All Ins" : "Ext. In", true);
    chooser(31, midi_ ? "All Channels" : "1/2", midi_);
    label(46, "Monitor");
    drawMonitor(g, 4.f, static_cast<float>(top + 59), 3);
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
    const auto left = area.withWidth(area.getWidth() / 2 - 1);
    const auto right = area.withTrimmedLeft(area.getWidth() / 2 + 1);
    const int rmsTop = depth(rms_), peakTop = depth(peak_);
    for (const auto &channel : {left, right}) {
        g.setColour(look::display);
        g.fillRect(channel);
        g.setColour(juce::Colour(0xff4fd84f));
        g.fillRect(channel.withTrimmedTop(rmsTop));
        g.setColour(peak_ >= 1 ? juce::Colour(0xffff3a34) : juce::Colour(0xffdcdcdc));
        if (peak_ > 1.e-6f)
            g.fillRect(channel.getX(), channel.getY() + peakTop, channel.getWidth(), 1);
    }
    if (!panel_.vertical())
        return;
    // Live's scale beside the fader: 0 to 60 dB every 12, every 6 on a tall strip.
    const bool tall = getHeight() >= MixerPanel::kTallHeight;
    g.setFont(look::font(11.5f));
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
    if (!panel_.vertical() || e.eventComponent != this || e.y >= 17 || track_ == 0)
        return;
    panel_.root.arrangement.geometry.selection.track = track_;
    panel_.root.arrangement.geometry.selection.toTrack = 0;
    panel_.root.arrangement.geometry.selection.picked.clear();
    panel_.root.publishSelection();
    panel_.root.showDeviceView();
}
bool MixerStrip::onSplitLine(const juce::MouseEvent &e) const noexcept {
    return !panel_.vertical() && track_ != 0 && e.eventComponent == this &&
           e.y >= getHeight() - TrackHeaderList::kSplitLine;
}
void MixerStrip::mouseMove(const juce::MouseEvent &e) {
    setMouseCursor(onSplitLine(e) ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}
void MixerStrip::mouseDrag(const juce::MouseEvent &e) {
    if (resizeFrom_ < 0)
        return;
    auto &view = panel_.root.arrangement;
    view.geometry.resizeTrack(track_, resizeFrom_ + e.getDistanceFromDragStartY(), e.mods.isAltDown());
    view.frame(true);
}
void MixerStrip::mouseUp(const juce::MouseEvent &) {
    if (resizeFrom_ < 0)
        return;
    resizeFrom_ = -1;
    dragging_ = false;
    panel_.root.arrangement.changed();
}
void MixerStrip::mouseDown(const juce::MouseEvent &e) {
    if (onSplitLine(e) && !e.mods.isPopupMenu()) {
        // Kept bound to its track while the drag resizes the rows (MixerPanel::place).
        resizeFrom_ = panel_.root.arrangement.geometry.heightFor(track_);
        dragging_ = true;
        return;
    }
    // A send's own clicks, and an off send's, which reach the strip: its menu, or what it waits for.
    for (std::size_t i = 0; i < sends_.size(); ++i) {
        auto &knob = *sends_[i];
        const bool own = e.eventComponent == &knob;
        if (!own && !(e.eventComponent == this && knob.isVisible() && knob.getBounds().contains(e.getPosition())))
            continue;
        if (e.mods.isPopupMenu())
            sendMenu(i);
        else if (!own)
            panel_.root.say("This send is off. Right-click it for Enable Send.");
        return;
    }
    if (!e.mods.isPopupMenu())
        return;
    if (e.eventComponent == &pan)
        panMenu();
    else
        panel_.contextMenu(e.getScreenPosition());
}
void MixerStrip::syncSends(const std::vector<DevicePanelData> *devices) {
    const auto &reader = panel_.root.reader();
    const auto *t = reader.findTrack(track_);
    returnsInOrder(reader.tracks(), order_);
    static const std::string none;
    const std::string &kind = t != nullptr ? t->kind : none;
    const bool midi = t != nullptr && isMidiTrack(*t);
    const bool audio = t == nullptr || hasAudioMixer(*t, devices);
    const bool grouped = t != nullptr && t->parent != 0;
    // A send knob per return on every track with an audio mixer but Main; on Main's Mixer View
    // strip, each return's Pre/Post switch instead (Live's Mixer).
    const auto knobs = t != nullptr && hasSends(*t) && audio ? order_.size() : 0;
    const auto switches = kind == "master" && panel_.vertical() ? order_.size() : 0;
    if (order_ == returns_ && kind == kind_ && knobs == sends_.size() && switches == returnSwitches_.size() &&
        midi == midi_ && audio == audio_ && grouped == inGroup_)
        return;
    kind_ = kind;
    midi_ = midi;
    audio_ = audio;
    inGroup_ = grouped;
    returns_ = order_;
    returnCount_ = returns_.size();
    sends_.resize(std::min(sends_.size(), knobs));
    while (sends_.size() < knobs)
        sends_.push_back(makeSend(sends_.size()));
    returnSwitches_.resize(std::min(returnSwitches_.size(), switches));
    while (returnSwitches_.size() < switches) {
        auto button = std::make_unique<juce::TextButton>("Post");
        const auto index = returnSwitches_.size();
        button->setTitle("Pre/Post " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + index % 26)));
        button->setTooltip("Whether this return's sends tap before or after their tracks' faders.");
        button->setClickingTogglesState(false);
        button->setColour(juce::TextButton::buttonColourId, look::button);
        button->setColour(juce::TextButton::buttonOnColourId, look::accent);
        button->setMouseClickGrabsKeyboardFocus(false);
        button->getProperties().set("live.font", 11.0);
        button->onClick = [this, index] {
            if (index < returns_.size())
                throwPrePost(returns_[index]);
        };
        addAndMakeVisible(*button);
        returnSwitches_.push_back(std::move(button));
    }
    sendOn_.assign(knobs, 1);
    resized();
    repaint();
}
void MixerStrip::throwPrePost(std::int64_t ret) {
    // From the CURRENT cell (ADR-0201): the return's own switch thrown, one op and one undo step,
    // which the engine spreads over every send into the return; it holds with no send (#405), so
    // setReturnPre never refuses and there is nothing to say.
    const auto current = panel_.root.current();
    if (!current)
        return;
    panel_.root.submitEdits(std::move(setReturnPre(ret, !returnPre(current->findTrack(ret))).ops));
}
std::unique_ptr<juce::Slider> MixerStrip::makeSend(std::size_t index) {
    auto knob = std::make_unique<juce::Slider>();
    if (panel_.vertical()) {
        knob->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    } else {
        // Live's send box: the number in it, filled in blue as the volume's.
        knob->setSliderStyle(juce::Slider::LinearBar);
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
    knob->setScrollWheelEnabled(false);
    // A drag is one undo step: it commits when the mouse lets go. A typed value commits at once.
    knob->onDragStart = [this] { dragging_ = true; };
    knob->onDragEnd = [this, index] {
        dragging_ = false;
        commitSend(index);
    };
    knob->onValueChange = [this, index] {
        if (!dragging_)
            commitSend(index);
    };
    knob->addMouseListener(this, false);
    addAndMakeVisible(*knob);
    return knob;
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
void MixerStrip::sendMenu(std::size_t index) {
    // Live's Enable Send, ticked while the send is on. A track's sends are on from the start; a
    // return's are off until it is chosen (ADR-0203 d3).
    juce::PopupMenu m;
    const bool on = sendOn(index);
    m.addItem("Enable Send", true, on, [this, index, on] { enableSend(index, !on); });
    // A return's sends start off; Live's menu also switches them all on at once (§18.4).
    if (kind_ == "return")
        m.addItem("Enable All Sends", [this] { enableAllSends(); });
    m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(sends_[index].get()));
}
void MixerStrip::panMenu() {
    juce::PopupMenu m;
    m.addItem(1, "Stereo Pan", true, true);
    // Live's Split Stereo Pan Mode (ADR-0203 d9) needs a pan mode and two pans per
    // strip in the format and the mixer: the engine's (collab/window.md, N12).
    m.addItem(2, "Split Stereo Pan", false, false);
    m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&pan));
}
MixerPanel::MixerPanel(AdiRootComponent &r, bool vertical) : root(r), vertical_(vertical) {}
bool MixerPanel::submit(const std::string &op, Payload payload, const char *label) {
    return root.submitEdit(op, std::move(payload), label);
}
void MixerPanel::resized() { place(); }
void MixerPanel::retap() {
    for (auto &s : strips_)
        if (s->track() != 0)
            s->bind(s->track());
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
    target->active.grabKeyboardFocus();
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
        top += geometry.heightFor(tracks[i].track->id);
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
    // Main has no activator, Solo or Arm, as Live's: the focus passes it by for those (its ADI-Q and
    // ADI-L it keeps).
    auto showing = [this, &geometry, slot](const engine::TrackNode *t) {
        return (vertical_ ? geometry.inView(t->id) : geometry.shown(t->id)) && !(t->kind == "master" && slot < 3) &&
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
            other->switches()[slot]->grabKeyboardFocus();
    return true;
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
        s.setVisible(true);
    }
    for (; n < strips_.size(); ++n)
        if (!strips_[n]->dragging())
            strips_[n]->setVisible(false);
}
void MixerPanel::place() {
    if (vertical_) {
        placeColumns();
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
        y += h;
    }
    while (strips_.size() < wanted) {
        auto strip = std::make_unique<MixerStrip>(*this);
        addChildComponent(*strip);
        strips_.push_back(std::move(strip));
    }
    y = bodyTop - geometry.scrollY;
    std::size_t used = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (i == foot)
            y = footY;
        const auto id = tracks[i].track->id;
        const int h = geometry.heightFor(id);
        if (inSight(i, y, h) && used < wanted) {
            auto &s = *strips_[used++];
            if (s.track() != id && !s.dragging())
                s.bind(id);
            s.setBounds(0, y, getWidth(), h);
            s.setVisible(true);
            // The foot's strips stand in front of a track's partly scrolled under them.
            if (i >= foot)
                s.toFront(false);
        }
        y += h;
    }
    for (std::size_t i = used; i < strips_.size(); ++i)
        if (!strips_[i]->dragging())
            strips_[i]->setVisible(false);
}
void MixerPanel::frame(bool changed) noexcept {
    if (changed)
        place();
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
    // H and W, Live's Optimize Arrangement Height and Width, beside the Overview's end.
    g.setFont(look::font(12.f));
    for (const auto &[label, x] : {std::pair{"H", getWidth() - 43}, std::pair{"W", getWidth() - 21}}) {
        const juce::Rectangle<float> r(static_cast<float>(x), 4, 18, 15);
        look::box(g, r, look::chooser);
        g.setColour(look::text);
        g.drawText(label, r, juce::Justification::centred);
    }
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
    if (vertical_)
        return;
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
FlipButton::FlipButton() : juce::Button("Flip Sides") {
    setTitle("Flip Sides");
    setTooltip("Flip sides");
    setClickingTogglesState(false);
    // A view command, reached from the keyboard through the View menu: it takes no focus.
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}
void FlipButton::paintButton(juce::Graphics &g, bool over, bool down) {
    // Two small arrows, one each way, as Bitwig draws its own.
    g.setColour(getToggleState() ? look::accent : over || down ? juce::Colours::white : look::textOnBand);
    const auto r = getLocalBounds().toFloat().reduced(3.f, 2.f);
    const float upper = r.getY() + r.getHeight() * .3f, lower = r.getY() + r.getHeight() * .7f;
    juce::Path arrows;
    arrows.addArrow({r.getX(), upper, r.getRight(), upper}, 1.4f, 5.f, 4.f);
    arrows.addArrow({r.getRight(), lower, r.getX(), lower}, 1.4f, 5.f, 4.f);
    g.fillPath(arrows);
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
    g.setColour(getToggleState() ? look::accent : over || down ? juce::Colours::white : look::textOnBand);
    const auto r = getLocalBounds().toFloat().reduced(3.f, 3.f);
    const float w = (r.getWidth() - 2.f) / 2.f, pitch = r.getHeight() / 3.f;
    for (int column = 0; column < 2; ++column)
        for (int bar = 0; bar < 3; ++bar)
            g.fillRect(r.getX() + static_cast<float>(column) * (w + 2.f), r.getY() + static_cast<float>(bar) * pitch, w,
                       std::max(1.f, pitch - 1.5f));
}
SelectorButton::SelectorButton(const juce::String &letters, const juce::String &tooltip) : juce::Button(letters) {
    setTooltip(tooltip);
    setClickingTogglesState(false);
    setMouseClickGrabsKeyboardFocus(false);
    setWantsKeyboardFocus(false);
}
void SelectorButton::paintButton(juce::Graphics &g, bool, bool) {
    const auto r = getLocalBounds().toFloat();
    const bool on = getToggleState();
    g.setColour(on ? look::accent : juce::Colour(0xff0a0a0a));
    g.fillEllipse(r);
    g.setColour(on ? look::text : look::textOnBand);
    g.setFont(look::font(getButtonText().length() > 1 ? 7.f : 9.f, true));
    g.drawText(getButtonText(), r, juce::Justification::centred);
}
SideStrip::SideStrip() {
    addAndMakeVisible(flip);
    addAndMakeVisible(tracks);
    for (auto *s : {&inOut, &returns, &mixer, &delay})
        addAndMakeVisible(*s);
    inOut.setToggleState(true, juce::dontSendNotification);
    returns.setToggleState(true, juce::dontSendNotification);
    mixer.setToggleState(true, juce::dontSendNotification);
    // Mixer and Delay only show their state: neither has a toggle to switch yet.
    mixer.setInterceptsMouseClicks(false, false);
    delay.setInterceptsMouseClicks(false, false);
}
void SideStrip::resized() {
    // Under the two view switches: the flip arrows, then the track height. At the foot, over the
    // time ruler's band: In/Out, Returns, Mixer, Delay.
    flip.setBounds(4, 47, 19, 15);
    tracks.setBounds(4, 64, 19, 14);
    int y = getHeight() - ArrangementView::kTimeRulerHeight - 4 * 19;
    for (auto *s : {&inOut, &returns, &mixer, &delay}) {
        s->setBounds(7, y, 15, 15);
        y += 19;
    }
}
void SideStrip::paint(juce::Graphics &g) {
    g.fillAll(look::band);
    // At the strip's head, Live's switches between Arrangement and Session View; the
    // Arrangement's is lit.
    for (int i = 0; i < 2; ++i) {
        const juce::Rectangle<float> r(4.f, 3.f + 22.f * static_cast<float>(i), 19, 19);
        g.setColour(juce::Colour(0xff3c3c3c));
        g.fillEllipse(r);
        g.setColour(i == 0 ? look::accent : look::panel);
        for (int line = 0; line < 3; ++line)
            if (i == 0)
                g.fillRect(r.getX() + 5, r.getY() + 6 + 3.f * static_cast<float>(line), 9.f, 1.6f);
            else
                g.fillRect(r.getX() + 6 + 3.f * static_cast<float>(line), r.getY() + 5, 1.6f, 9.f);
    }
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
    m.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}));
}
void MixerPanel::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (vertical_) {
        // The Mixer View scrolls sideways, whichever way the wheel turns.
        const float delta = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
        scrollX_ -= static_cast<int>(delta * 300);
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
