// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_channel_eq.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <cmath>
namespace adi::ui {
namespace {
// EQ Eight's display: 10 Hz to 22 kHz on a logarithmic axis, ±15 dB.
constexpr double kLowHz = 10, kHighHz = 22000, kRangeDb = 15;
float xFor(double hz, juce::Rectangle<float> r) {
    return r.getX() + r.getWidth() * static_cast<float>(std::log(hz / kLowHz) / std::log(kHighHz / kLowHz));
}
float yFor(double db, juce::Rectangle<float> r) {
    return r.getCentreY() - r.getHeight() / 2 * static_cast<float>(db / kRangeDb);
}
// The columns of the channel list, as Live's track header lays out a row.
constexpr int kNameX = 26, kEqX = 230, kLimiterX = 330, kModeX = 410, kSwitchWidth = 60;
} // namespace
ChannelEqRow::ChannelEqRow(ChannelEqView &view, std::int64_t track) : view_(view), track_(track) {
    eq.setTitle("ADI-Q");
    eq.setTooltip("This channel's EQ: built in, present but off until a band is touched (ADR-0204 d1).");
    limiter.setTitle("ADI-L");
    limiter.setTooltip("This channel's limiter: built in, off, a stage of the Loudness Drive when it is used.");
    for (auto *b : {&eq, &limiter}) {
        b->setColour(juce::TextButton::buttonColourId, look::button);
        addAndMakeVisible(*b);
    }
    // Present but off: what fills the slots is ADI-Q and ADI-L, in the engine's channel
    // slots (LANES §3 item 7). Until then a switch says so and stays off.
    eq.onClick = [this] {
        view_.focus(track_);
        view_.root.say("The channel EQ slot is off until ADI-Q is handed over.");
    };
    limiter.onClick = [this] {
        view_.focus(track_);
        view_.root.say("The channel limiter slot is off until ADI-L is handed over.");
    };
}
void ChannelEqRow::resized() {
    eq.setBounds(kEqX, 3, kSwitchWidth, kHeight - 6);
    limiter.setBounds(kLimiterX, 3, kSwitchWidth, kHeight - 6);
}
void ChannelEqRow::paint(juce::Graphics &g) {
    const bool focused = view_.focused() == track_;
    g.fillAll(focused ? look::laneSelectedAlt : look::panel);
    g.setColour(look::separator);
    g.fillRect(0, getHeight() - 1, getWidth(), 1);
    const auto &tracks = view_.root.reader().tracks();
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i]->id == track_) {
            g.setColour(view_.root.arrangement.colourFor(i));
            g.fillRect(8, 5, 11, 11);
            g.setColour(look::text);
            g.setFont(look::font(12.f, true));
            g.drawText(juce::String::fromUTF8(displayName(tracks[i]->name, i + 1).c_str()), kNameX, 0,
                       kEqX - kNameX - 8, kHeight, juce::Justification::centredLeft, true);
            break;
        }
    // ADI-L's mode (ADR-0204 d1): light on a channel, full on the master's, which the
    // snapshot cannot point out yet (N6).
    g.setColour(look::textDim);
    g.setFont(look::font(11.5f));
    g.drawText("Light", kModeX, 0, 60, kHeight, juce::Justification::centredLeft);
}
void ChannelEqRow::mouseDown(const juce::MouseEvent &) { view_.focus(track_); }
ChannelEqView::ChannelEqView(AdiRootComponent &r) : root(r) {
    setTitle("Channel EQ");
    setLookAndFeel(&look::liveLook());
    scroll_.setViewedComponent(&list_, false);
    scroll_.setScrollBarsShown(true, false);
    addAndMakeVisible(scroll_);
    setSize(560, 460);
}
ChannelEqView::~ChannelEqView() { setLookAndFeel(nullptr); }
juce::Rectangle<int> ChannelEqView::display() const noexcept {
    return {look::gap, look::gap, std::max(0, getWidth() - 2 * look::gap), kDisplayHeight};
}
void ChannelEqView::frame(const SnapshotReader &reader) {
    // Rows follow the tracks; a frame that changes none of them builds nothing.
    const auto &tracks = reader.tracks();
    bool same = tracks.size() == rows_.size();
    for (std::size_t i = 0; same && i < tracks.size(); ++i)
        same = rows_[i]->track() == tracks[i]->id;
    if (same) {
        if (reader.seq() != shown_) {
            shown_ = reader.seq();
            repaint();
        }
        return;
    }
    shown_ = reader.seq();
    rows_.clear();
    for (const auto &t : tracks) {
        rows_.push_back(std::make_unique<ChannelEqRow>(*this, t->id));
        list_.addAndMakeVisible(*rows_.back());
    }
    resized();
    repaint();
}
void ChannelEqView::focus(std::int64_t track) {
    focused_ = track;
    for (auto &row : rows_) {
        row->repaint();
        if (row->track() == track)
            scroll_.setViewPosition(0, std::max(0, row->getY() - ChannelEqRow::kHeight));
    }
}
void ChannelEqView::resized() {
    const int top = kDisplayHeight + 2 * look::gap + kHeaderHeight;
    scroll_.setBounds(look::gap, top, std::max(0, getWidth() - 2 * look::gap),
                      std::max(0, getHeight() - top - look::gap));
    const int width = std::max(0, scroll_.getWidth() - scroll_.getScrollBarThickness());
    int y = 0;
    for (auto &row : rows_) {
        row->setBounds(0, y, width, ChannelEqRow::kHeight);
        y += ChannelEqRow::kHeight;
    }
    list_.setSize(width, y);
}
void ChannelEqView::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    // EQ Eight's display: dark, its frequency and gain grid, the flat line every EQ that
    // is off leaves, and in its track's colour each engaged EQ's curve.
    const auto area = display().toFloat();
    g.setColour(look::display);
    g.fillRect(area);
    const auto plot = area.reduced(24, 12);
    g.setFont(look::font(11.5f));
    for (double decade = 10; decade < kHighHz; decade *= 10)
        for (int step = 1; step < 10; ++step) {
            const double hz = decade * step;
            if (hz < kLowHz || hz > kHighHz)
                continue;
            const float x = xFor(hz, plot);
            g.setColour(step == 1 ? juce::Colour(0xff3c3c3c) : juce::Colour(0xff262626));
            g.fillRect(x, plot.getY(), 1.f, plot.getHeight());
            if (step == 1 && hz >= 100) {
                g.setColour(look::textDim.brighter(0.3f));
                g.drawText(hz >= 1000 ? juce::String(static_cast<int>(hz / 1000)) + "k" : juce::String(static_cast<int>(hz)),
                           juce::Rectangle<float>(x + 3, plot.getBottom() - 14, 40, 14), juce::Justification::centredLeft);
            }
        }
    for (int db = -12; db <= 12; db += 6) {
        const float y = yFor(db, plot);
        g.setColour(juce::Colour(0xff3c3c3c));
        g.fillRect(plot.getX(), y, plot.getWidth(), 1.f);
        g.setColour(look::textDim.brighter(0.3f));
        g.drawText(juce::String(db), juce::Rectangle<float>(area.getX() + 2, y - 7, 20, 14), juce::Justification::centredRight);
    }
    g.setColour(look::value);
    g.fillRect(plot.getX(), yFor(0, plot) - 0.5f, plot.getWidth(), 1.5f);
    g.setColour(look::textDim.brighter(0.4f));
    g.drawText("No channel EQ is on. Switch one on below.", plot.withTrimmedBottom(plot.getHeight() / 2),
               juce::Justification::centred);
    // The list's head, as Live's column names.
    const juce::Rectangle<int> head(look::gap, kDisplayHeight + 2 * look::gap, getWidth() - 2 * look::gap,
                                    kHeaderHeight);
    g.setColour(look::columnHead);
    g.fillRect(head);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    g.drawText("Channel", head.withTrimmedLeft(kNameX), juce::Justification::centredLeft);
    g.drawText("ADI-Q", head.getX() + kEqX, head.getY(), kSwitchWidth, head.getHeight(), juce::Justification::centred);
    g.drawText("ADI-L", head.getX() + kLimiterX, head.getY(), kSwitchWidth, head.getHeight(),
               juce::Justification::centred);
    g.drawText("Mode", head.getX() + kModeX, head.getY(), 60, head.getHeight(), juce::Justification::centredLeft);
}
} // namespace adi::ui
