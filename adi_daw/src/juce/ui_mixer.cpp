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
// A knob drawn to the edge of its bounds: JUCE's own insets ten pixels, which leaves
// nothing of a row-sized pan control.
struct SmallKnob final : juce::LookAndFeel_V4 {
    void drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h, float position,
                          float start, float end, juce::Slider &s) override {
        const auto b = juce::Rectangle<int>(x, y, w, h).toFloat().reduced(2);
        const float r = std::min(b.getWidth(), b.getHeight()) / 2;
        const float cx = b.getCentreX(), cy = b.getCentreY();
        g.setColour(s.findColour(juce::Slider::rotarySliderOutlineColourId));
        g.fillEllipse(cx - r, cy - r, 2 * r, 2 * r);
        const float a = start + position * (end - start);
        g.setColour(s.findColour(juce::Slider::rotarySliderFillColourId));
        g.drawLine(cx, cy, cx + r * std::sin(a), cy - r * std::cos(a), 2.f);
    }
};
juce::LookAndFeel &smallKnob() {
    static SmallKnob knob;
    return knob;
}
} // namespace
MixerStrip::MixerStrip(MixerPanel &panel) : panel_(panel) {
    if (panel.vertical()) {
        volume.setSliderStyle(juce::Slider::LinearVertical);
        volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        pan.setLookAndFeel(&smallKnob());
        pan.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff4a5463));
        pan.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff8fb4c8));
    } else {
        // Live's value boxes: the number in the box, the volume's filled in blue.
        for (auto *s : {&volume, &pan}) {
            s->setSliderStyle(juce::Slider::LinearBar);
            s->setTextBoxStyle(juce::Slider::TextBoxLeft, false, 0, 0);
            s->setSliderSnapsToMousePosition(false);
            s->setColour(juce::Slider::textBoxTextColourId, look::text);
        }
        pan.getProperties().set("live.bipolar", true);
    }
    volume.setRange(kFloorDb, kTopDb, .01);
    // Live's 0 dB sits at 83 % of the box, not at the linear 92 %.
    volume.setSkewFactor(2.27);
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
    eq.onClick = [this] {
        panel_.root.say("The channel EQ slot is off until ADI-Q is handed over.");
    };
    limiter.onClick = [this] {
        panel_.root.say("The channel limiter slot is off until ADI-L is handed over.");
    };
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
        // The Track Activator shows the track's number, its place in the list, as
        // Live's (and as a # in its name does); the text changes only when it moves.
        const auto &tracks = panel_.root.reader().tracks();
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->id == track_ && position_ != i + 1) {
                position_ = i + 1;
                active.setButtonText(juce::String(static_cast<int>(position_)));
                break;
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
    if (p != peak_ || r != rms_) {
        peak_ = p;
        rms_ = r;
        repaint(panel_.vertical() ? getWidth() - 18 : (showsInOut() ? MixerPanel::kInOutWidth : 0) + 105,
                0, 10, getHeight());
    }
}
void MixerStrip::resized() {
    if (panel_.vertical()) {
        // Live's channel strip, top to bottom: the slots, pan, the activator and Solo,
        // then the fader beside its meter.
        const int w = getWidth();
        eq.setBounds(4, 20, w / 2 - 6, 18);
        limiter.setBounds(w / 2 + 2, 20, w / 2 - 6, 18);
        pan.setBounds(w / 2 - 14, 41, 28, 28);
        active.setBounds(4, 72, w / 2 - 6, 18);
        solo.setBounds(w / 2 + 2, 72, w / 2 - 6, 18);
        const bool tall = getHeight() >= MixerPanel::kTallHeight;
        if (tall != (volume.getTextBoxPosition() == juce::Slider::TextBoxBelow))
            volume.setTextBoxStyle(tall ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox,
                                   false, tall ? w - 26 : 0, tall ? 18 : 0);
        volume.setBounds(2, 94, std::max(0, w - 24), std::max(0, getHeight() - 98));
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
bool MixerStrip::showsInOut() const noexcept {
    return !panel_.vertical() && getWidth() >= MixerPanel::kInOutWidth + MixerPanel::kControlsWidth;
}
void MixerStrip::paint(juce::Graphics &g) {
    if (panel_.vertical()) {
        g.fillAll(juce::Colour(0xff242b35));
        g.setColour(juce::Colour(0xff313b47));
        g.drawVerticalLine(getWidth() - 1, 0, static_cast<float>(getHeight()));
        const auto &tracks = panel_.root.reader().tracks();
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->id == track_) {
                g.setColour(juce::Colour(0xffe0e5ed));
                g.setFont(juce::FontOptions(12.f));
                g.drawText(juce::String::fromUTF8(displayName(tracks[i]->name, i + 1).c_str()),
                           4, 2, getWidth() - 8, 16, juce::Justification::centredLeft);
                break;
            }
        paintMeter(g, {getWidth() - 18, 94, 10, std::max(0, getHeight() - 98)});
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
    const int height = area.getHeight();
    g.setColour(look::display);
    if (panel_.vertical())
        g.fillRect(area);
    else {
        // Live's two channels, side by side.
        g.fillRect(area.withWidth(area.getWidth() / 2 - 1));
        g.fillRect(area.withTrimmedLeft(area.getWidth() / 2 + 1));
    }
    auto level = [height](float v) {
        return static_cast<int>(
            static_cast<float>(height) *
            std::clamp((20 * std::log10(std::max(v, 1.e-6f)) + 60) / 66, 0.f, 1.f));
    };
    const int rmsH = level(rms_);
    g.setColour(juce::Colour(0xff65cf91));
    g.fillRect(area.getX(), area.getBottom() - rmsH, area.getWidth(), rmsH);
    g.setColour(peak_ >= 1 ? juce::Colours::red : juce::Colours::white);
    g.fillRect(area.getX(), area.getBottom() - level(peak_), area.getWidth(), 1);
    // A tall strip gets tick marks every 6 dB beside the meter.
    if (panel_.vertical() && getHeight() >= MixerPanel::kTallHeight) {
        g.setColour(juce::Colour(0xff8b95a3));
        for (int db = 6; db >= -60; db -= 6) {
            const int y = area.getBottom() - level(std::pow(10.f, static_cast<float>(db) / 20));
            g.fillRect(area.getRight() + 1, y, 3, 1);
        }
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
void MixerPanel::placeColumns() {
    // Strips left to right, realising only the columns on screen (UI-ARCHITECTURE §10).
    const auto &tracks = root.reader().tracks();
    const int total = static_cast<int>(tracks.size()) * kStripWidth;
    scrollX_ = std::clamp(scrollX_, 0, std::max(0, total - getWidth()));
    const std::size_t first = static_cast<std::size_t>(scrollX_ / kStripWidth);
    const std::size_t wanted =
        std::min(tracks.size() - std::min(first, tracks.size()),
                 static_cast<std::size_t>(getWidth() / kStripWidth + 2));
    while (strips_.size() < wanted) {
        auto strip = std::make_unique<MixerStrip>(*this);
        addChildComponent(*strip);
        strips_.push_back(std::move(strip));
    }
    for (std::size_t n = 0; n < strips_.size(); ++n) {
        auto &s = *strips_[n];
        if (n >= wanted) {
            if (!s.dragging())
                s.setVisible(false);
            continue;
        }
        const auto i = first + n;
        if (s.track() != tracks[i]->id && !s.dragging())
            s.bind(tracks[i]->id);
        s.setBounds(static_cast<int>(i) * kStripWidth - scrollX_, 0, kStripWidth, getHeight());
        s.setVisible(true);
    }
}
void MixerPanel::place() {
    if (vertical_) {
        placeColumns();
        return;
    }
    // Realise what is visible and a margin, never a strip per track (UI-ARCHITECTURE
    // §10); rows follow the arrangement's lanes, so the two scroll as one.
    const auto &geometry = root.arrangement.geometry;
    const auto &tracks = root.reader().tracks();
    const int bodyTop = ArrangementView::kTopHeight; // level with the lanes
    std::size_t wanted = 0;
    int y = bodyTop - geometry.scrollY;
    std::size_t first = tracks.size();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const int h = geometry.heightFor(tracks[i]->id);
        if (y + h > bodyTop && y < getHeight()) {
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
        const int h = geometry.heightFor(tracks[i]->id);
        if (i >= first && used < wanted) {
            auto &s = *strips_[used++];
            if (s.track() != tracks[i]->id && !s.dragging())
                s.bind(tracks[i]->id);
            s.setBounds(0, y, std::max(0, getWidth() - kRightStrip), h);
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
        g.fillAll(juce::Colour(0xff21262e));
        return;
    }
    // Live's arrangement frame above the rows and down the right-hand edge; the
    // columns' grey under the last track.
    g.fillAll(look::band);
    const int strip = getWidth() - kRightStrip;
    int bottom = ArrangementView::kTopHeight - root.arrangement.geometry.scrollY;
    for (const auto &t : root.reader().tracks())
        bottom += root.arrangement.geometry.heightFor(t->id);
    bottom = std::clamp(bottom, ArrangementView::kTopHeight, getHeight());
    g.setColour(look::panel);
    g.fillRect(0, bottom, strip, std::max(0, getHeight() - bottom - ArrangementView::kTimeRulerHeight));
    g.setColour(look::separator);
    g.fillRect(0, bottom, 2, getHeight() - bottom);
    // H and W, Live's Optimize Arrangement Height and Width, beside the Overview's end.
    g.setFont(look::font(12.f));
    for (const auto &[label, x] : {std::pair{"H", strip - 43}, std::pair{"W", strip - 21}}) {
        const juce::Rectangle<float> r(static_cast<float>(x), 4, 18, 15);
        look::box(g, r, look::chooser);
        g.setColour(look::text);
        g.drawText(label, r, juce::Justification::centred);
    }
    // At the strip's head, Live's switches between Arrangement and Session View; the
    // Arrangement's is lit.
    for (int i = 0; i < 2; ++i) {
        const juce::Rectangle<float> r(static_cast<float>(strip) + 4, 3.f + 22.f * static_cast<float>(i), 19, 19);
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
        const juce::Rectangle<float> r(static_cast<float>(strip) + 7, y, 15, 15);
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
