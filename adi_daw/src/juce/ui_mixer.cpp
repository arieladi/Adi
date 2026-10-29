// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixer.hpp"
#include "ui_shell.hpp"
#include <algorithm>
#include <cmath>
#include <string_view>
namespace adi::ui {
namespace {
// Live's fader runs from -inf to +6 dB; ADI's bottom stop is -inf.
constexpr double kFloorDb = -70, kTopDb = 6;
// Live 11's meter, measured: +6 dB at the top, then 0, -12, -24, -36, -48 and -60 dB
// at these depths; the scale is wider near 0 dB, as Live's.
constexpr std::array<std::pair<double, double>, 7> kScale{
    {{6, 0}, {0, .06}, {-12, .28}, {-24, .5}, {-36, .71}, {-48, .87}, {-60, 1}}};
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
        panel_.submit("mixer.setVolume", {{"id", track_}, {"db", volume.getValue()}},
                      "Change Track Volume");
    };
    pan.onDragEnd = [this] {
        dragging_ = false;
        panel_.submit("mixer.setPan", {{"id", track_}, {"pan", pan.getValue()}},
                      "Change Track Panning");
    };
    volume.onValueChange = [this] {
        if (!dragging_)
            panel_.submit("mixer.setVolume", {{"id", track_}, {"db", volume.getValue()}},
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
    arm.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffff3a34));
    pan.addMouseListener(this, false);
    active.setClickingTogglesState(false);
    active.onClick = [this] {
        if (auto *t = panel_.root.reader().findTrack(track_))
            panel_.submit("track.setMute", {{"id", track_}, {"muted", !t->muted}},
                          t->muted ? "Activate Track" : "Deactivate Track");
    };
    solo.onClick = [this] {
        if (auto *t = panel_.root.reader().findTrack(track_))
            panel_.submit("track.setSolo", {{"id", track_}, {"soloed", !t->soloed}},
                          t->soloed ? "Unsolo Track" : "Solo Track");
    };
    arm.onClick = [this] { panel_.root.say("Arm Recording is not built yet."); };
    // Present but off: the slots exist on every strip; what fills them is ADI-Q and
    // ADI-L from the clones lane, in the engine's channel slots (LANES §3 item 7).
    // A slot opens the Channel EQ window on its track, where every channel's EQ and
    // limiter are seen together (ADR-0204 d1).
    auto open = [this](const char *why) {
        panel_.root.arrangement.geometry.selection.track = track_;
        panel_.root.publishSelection();
        if (!panel_.root.command(AppCommands::ShowChannelEq))
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
    if (changed) {
        // The Track Activator shows the track's number, its place in the arrangement's order,
        // as Live's (and as a # in its name does); the text changes only when it moves.
        const auto rows = panel_.root.arrangement.geometry.rows(panel_.root.reader());
        if (const auto at = rows.find(track_); at < rows.size()) {
            if (position_ != at + 1) {
                position_ = at + 1;
                active.setButtonText(juce::String(static_cast<int>(position_)));
            }
            const auto parent = rows[at].place.parent;
            const auto group = parent == 0 ? rows.size() : rows.find(parent);
            if (const auto place = group < rows.size() ? group + 1 : 0; place != group_) {
                group_ = place;
                repaint();
            }
        }
    }
    // The selected track's controls are lighter, as Live's.
    const bool selected = track_ != 0 && panel_.root.arrangement.geometry.selection.track == track_;
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
        const int top = kMixerTop, bottom = getHeight();
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
    place(active, 0, {x + 4, 3, 45, 13});
    place(solo, 0, {x + 54, 3, 20, 13});
    place(arm, 0, {x + 78, 3, 21, 13});
    place(volume, 1, {x + 4, 20, 35, 13});
    place(pan, 1, {x + 54, 20, 45, 13});
    place(eq, 2, {x + 4, 37, 29, 13});
    place(limiter, 2, {x + 37, 37, 29, 13});
}
juce::Rectangle<int> MixerStrip::meterArea() const noexcept {
    // Under a tall strip's numeric field, the meter stops short of the foot.
    const bool tall = getHeight() >= MixerPanel::kTallHeight;
    return {53, kMixerTop + 6, 11, std::max(0, getHeight() - kMixerTop - 10 - (tall ? 15 : 0))};
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
        paintMeter(g, meterArea());
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
    paintMeter(g, {x + 105, 1, 8, std::max(0, getHeight() - 4)});
}
void MixerStrip::paintInOut(juce::Graphics &g, int x) const {
    // The In/Out column as Live draws an audio track's: where it records from, the
    // monitoring switch, where it plays to. The snapshot does not carry a track's
    // routing yet (window log, N11): the column shows ADI's fixed routing, every
    // track to the Main track.
    const int rows = std::max(1, (getHeight() - 1) / 17);
    auto chooser = [&](int row, const char *label, bool enabled) {
        if (row >= rows)
            return;
        const juce::Rectangle<float> r(static_cast<float>(x + 3), static_cast<float>(3 + 17 * row), 70, 13);
        look::box(g, r, look::chooser);
        g.setColour(enabled ? look::text : look::textDim);
        g.setFont(look::font(11.5f));
        g.drawText(label, r.withTrimmedLeft(3).withTrimmedRight(12), juce::Justification::centredLeft);
        if (*label != 0)
            look::triangle(g, {r.getRight() - 10, r.getY() + 4, 7, 5}, juce::MathConstants<float>::halfPi,
                           enabled ? look::text : look::textDim);
    };
    chooser(0, "Ext. In", true);
    chooser(1, "1/2", false);
    if (rows > 2) {
        const std::array<std::pair<const char *, int>, 3> monitor{{{"In", 16}, {"Auto", 23}, {"Off", 19}}};
        float bx = static_cast<float>(x + 3);
        for (const auto &[label, width] : monitor) {
            const juce::Rectangle<float> r(bx, 37, static_cast<float>(width), 13);
            const bool on = std::string_view(label) == "Auto";
            look::box(g, r, on ? look::accent : look::button);
            g.setColour(look::text);
            g.setFont(look::font(11.5f));
            g.drawText(label, r, juce::Justification::centred);
            bx += static_cast<float>(width) + 6;
        }
    }
    chooser(3, "Master", true);
    chooser(4, "", false);
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
void MixerStrip::mouseDown(const juce::MouseEvent &e) {
    if (!e.mods.isPopupMenu())
        return;
    if (e.eventComponent == &pan)
        panMenu();
    else
        panel_.contextMenu(e.getScreenPosition());
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
    int top = 0;
    for (std::size_t i = 0; i < index; ++i)
        top += geometry.heightFor(tracks[i].track->id);
    const int bottom = top + geometry.heightFor(tracks[index].track->id);
    const int view = std::max(0, getHeight() - root.arrangement.topHeight());
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
    auto showing = [this, &geometry](std::int64_t id) { return vertical_ ? geometry.inView(id) : geometry.shown(id); };
    const auto i = tracks.find(from->track());
    if (i == tracks.size())
        return true;
    std::size_t to = tracks.size();
    if (next) {
        for (std::size_t j = i + 1; j < tracks.size() && to == tracks.size(); ++j)
            if (showing(tracks[j].track->id))
                to = j;
    } else {
        for (std::size_t j = i; j-- > 0 && to == tracks.size();)
            if (showing(tracks[j].track->id))
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
    std::size_t wanted = 0;
    int y = bodyTop - geometry.scrollY;
    std::size_t first = tracks.size();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const int h = geometry.heightFor(tracks[i].track->id);
        if (h > 0 && y + h > bodyTop && y < getHeight()) {
            first = std::min(first, i);
            ++wanted;
        }
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
        const auto id = tracks[i].track->id;
        const int h = geometry.heightFor(id);
        if (h > 0 && i >= first && used < wanted) {
            auto &s = *strips_[used++];
            if (s.track() != id && !s.dragging())
                s.bind(id);
            s.setBounds(0, y, getWidth(), h);
            s.setVisible(true);
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
    int bottom = top - root.arrangement.geometry.scrollY;
    for (const auto &t : root.reader().tracks())
        bottom += root.arrangement.geometry.heightFor(t->id);
    bottom = std::clamp(bottom, top, getHeight());
    g.setColour(look::panel);
    g.fillRect(0, bottom, getWidth(), std::max(0, getHeight() - bottom - ArrangementView::kTimeRulerHeight));
    g.setColour(look::separator);
    g.fillRect(0, bottom, 2, getHeight() - bottom);
    // H and W, Live's Optimize Arrangement Height and Width, beside the Overview's end.
    g.setFont(look::font(12.f));
    for (const auto &[label, x] : {std::pair{"H", getWidth() - 43}, std::pair{"W", getWidth() - 21}}) {
        const juce::Rectangle<float> r(static_cast<float>(x), 4, 18, 15);
        look::box(g, r, look::chooser);
        g.setColour(look::text);
        g.drawText(label, r, juce::Justification::centred);
    }
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
SideStrip::SideStrip() {
    addAndMakeVisible(flip);
    addAndMakeVisible(tracks);
}
void SideStrip::resized() {
    // Under the two view switches: the flip arrows, then the track height.
    flip.setBounds(4, 47, 19, 15);
    tracks.setBounds(4, 64, 19, 14);
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
    // The strip's round switches at its foot: In/Out, Returns, Mixer, Delay.
    const std::array<std::pair<const char *, bool>, 4> switches{
        {{"I-O", true}, {"R", true}, {"M", true}, {"D", false}}};
    float y = static_cast<float>(getHeight() - ArrangementView::kTimeRulerHeight) - 4 * 19.f;
    for (const auto &[label, on] : switches) {
        const juce::Rectangle<float> r(7.f, y, 15, 15);
        g.setColour(on ? look::accent : juce::Colour(0xff0a0a0a));
        g.fillEllipse(r);
        g.setColour(on ? look::text : look::textOnBand);
        g.setFont(look::font(std::string_view(label).size() > 1 ? 7.f : 9.f, true));
        g.drawText(label, r, juce::Justification::centred);
        y += 19;
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
