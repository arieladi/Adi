// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_mixer.hpp"
#include "ui_shell.hpp"
#include <algorithm>
#include <cmath>
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
    volume.setSliderStyle(juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
    volume.setRange(kFloorDb, kTopDb, .01);
    volume.textFromValueFunction = [](double v) {
        return v <= kFloorDb ? juce::String("-inf") : juce::String(v, 1) + " dB";
    };
    volume.updateText();
    volume.setTitle("Track Volume");
    volume.setTooltip("Adjusts the track's output level.");
    pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    pan.setRange(-1, 1, .01);
    // Double-clicking the Pan control resets it to its default (manual 18.1).
    pan.setDoubleClickReturnValue(true, 0);
    pan.setLookAndFeel(&smallKnob());
    pan.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff4a5463));
    pan.setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff8fb4c8));
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
    eq.setTitle("ADI-Q");
    eq.setTooltip("The channel EQ, built in and off until a band is touched (ADR-0204 d1).");
    limiter.setTitle("ADI-L");
    limiter.setTooltip("The channel limiter, built in and off (ADR-0204 d1).");
    for (auto *b : {&active, &solo, &eq, &limiter})
        addAndMakeVisible(*b);
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
    // Present but off: the slots exist on every strip; what fills them is ADI-Q and
    // ADI-L from the clones lane, in the engine's channel slots (LANES §3 item 7).
    eq.onClick = [this] {
        panel_.root.say("The channel EQ slot is off until ADI-Q is handed over.");
    };
    limiter.onClick = [this] {
        panel_.root.say("The channel limiter slot is off until ADI-L is handed over.");
    };
}
void MixerStrip::bind(std::int64_t id) {
    track_ = id;
    // The Track Activator shows the track's number, as Live's.
    active.setButtonText(juce::String(id));
    tap_ = panel_.tap ? panel_.tap(id) : nullptr;
    peak_ = rms_ = 0;
    frame(true);
}
void MixerStrip::frame(bool changed) noexcept {
    const auto *t = panel_.root.reader().findTrack(track_);
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
        repaint(getWidth() - 10, 2, 8, getHeight() - 4);
    }
}
void MixerStrip::resized() {
    const int meterW = 12, h = std::min(getHeight() - 4, 22);
    int x = 4;
    active.setBounds(x, 3, 24, h);
    x += 26;
    solo.setBounds(x, 3, 22, h);
    x += 24;
    pan.setBounds(x, 2, h + 2, h + 2);
    x += h + 6;
    eq.setBounds(getWidth() - meterW - 54, 3, 26, h);
    limiter.setBounds(getWidth() - meterW - 28, 3, 26, h);
    volume.setBounds(x, 3, std::max(0, getWidth() - meterW - 58 - x), h);
}
void MixerStrip::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff242b35));
    g.setColour(juce::Colour(0xff313b47));
    g.drawHorizontalLine(getHeight() - 1, 0, static_cast<float>(getWidth()));
    const int mx = getWidth() - 10, top = 2, height = getHeight() - 5;
    g.setColour(juce::Colour(0xff11161c));
    g.fillRect(mx, top, 8, height);
    auto level = [height](float v) {
        return static_cast<int>(
            static_cast<float>(height) *
            std::clamp((20 * std::log10(std::max(v, 1.e-6f)) + 60) / 66, 0.f, 1.f));
    };
    const int rmsH = level(rms_);
    g.setColour(juce::Colour(0xff65cf91));
    g.fillRect(mx, top + height - rmsH, 8, rmsH);
    g.setColour(peak_ >= 1 ? juce::Colours::red : juce::Colours::white);
    g.fillRect(mx, top + height - level(peak_), 8, 1);
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
MixerPanel::MixerPanel(AdiRootComponent &r) : root(r) {}
bool MixerPanel::submit(const std::string &op, Payload payload, const char *label) {
    return root.submitEdit(op, std::move(payload), label);
}
void MixerPanel::resized() { place(); }
void MixerPanel::retap() {
    for (auto &s : strips_)
        if (s->track() != 0)
            s->bind(s->track());
}
void MixerPanel::place() {
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
    g.fillAll(juce::Colour(0xff21262e));
    g.setColour(juce::Colour(0xff303944));
    g.fillRect(0, 0, getWidth(), ArrangementView::kTopHeight);
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
    // The rows belong to the arrangement's lanes: the wheel scrolls both.
    root.arrangement.wheel(e.getEventRelativeTo(&root.arrangement.canvas), w);
}
} // namespace adi::ui
