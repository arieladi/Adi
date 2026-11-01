// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_stack.hpp"
#include "ui_look.hpp"
#include "ui_shell.hpp"
#include <algorithm>
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
// The display's grid: the decades and their steps, the gain lines every 6 dB; their labels where
// there is room. Dimmer for a slot that is off (ADR-0209 §1: dormant, drawn dimmed).
void paintGrid(juce::Graphics &g, juce::Rectangle<float> area, bool labels, bool dim) {
    g.setColour(look::display);
    g.fillRect(area);
    const auto plot = labels ? area.reduced(24, 10) : area.reduced(2, 2);
    const auto major = dim ? juce::Colour(0xff2e2e2e) : juce::Colour(0xff3c3c3c);
    const auto minor = dim ? juce::Colour(0xff222222) : juce::Colour(0xff262626);
    g.setFont(look::font(11.f));
    for (double decade = 10; decade < kHighHz; decade *= 10)
        for (int step = 1; step < 10; ++step) {
            const double hz = decade * step;
            if (hz < kLowHz || hz > kHighHz)
                continue;
            // A row's small graph keeps the decades only: a dormant slot reads as its flat line.
            if (!labels && step != 1)
                continue;
            const float x = xFor(hz, plot);
            g.setColour(step == 1 ? major : minor);
            g.fillRect(x, plot.getY(), 1.f, plot.getHeight());
            if (labels && step == 1 && hz >= 100) {
                g.setColour(look::textDim.brighter(0.3f));
                g.drawText(hz >= 1000 ? juce::String(static_cast<int>(hz / 1000)) + "k"
                                      : juce::String(static_cast<int>(hz)),
                           juce::Rectangle<float>(x + 3, plot.getBottom() - 13, 40, 13),
                           juce::Justification::centredLeft);
            }
        }
    for (int db = -12; db <= 12 && labels; db += 6) {
        const float y = yFor(db, plot);
        g.setColour(major);
        g.fillRect(plot.getX(), y, plot.getWidth(), 1.f);
        {
            g.setColour(look::textDim.brighter(0.3f));
            g.drawText(juce::String(db), juce::Rectangle<float>(area.getX() + 2, y - 7, 20, 14),
                       juce::Justification::centredRight);
        }
    }
}
// The 0 dB line an EQ that is off leaves: dimmed, or in the track's colour once engaged.
void paintFlat(juce::Graphics &g, juce::Rectangle<float> area, bool labels, juce::Colour colour) {
    const auto plot = labels ? area.reduced(24, 10) : area.reduced(2, 2);
    g.setColour(colour);
    g.fillRect(plot.getX(), yFor(0, plot) - 0.75f, plot.getWidth(), 1.5f);
}
// A drawer: a slot beside ADI-Q's graph, its name and its state; lit in the accent once engaged.
void paintDrawer(juce::Graphics &g, juce::Rectangle<int> r, const char *name, const juce::String &state,
                 bool engaged, bool zoomed) {
    look::box(g, r.toFloat(), engaged ? look::accent : look::button);
    g.setColour(engaged ? look::text : look::textDim);
    if (!zoomed) {
        g.setFont(look::font(11.5f));
        g.drawText(juce::String(name) + "  " + state, r.reduced(6, 0), juce::Justification::centredLeft, true);
        return;
    }
    g.setFont(look::font(12.f, true));
    g.drawText(name, r.getX() + 6, r.getY() + 4, r.getWidth() - 12, 16, juce::Justification::centredLeft);
    g.setFont(look::font(11.5f));
    g.drawText(state, r.getX() + 6, r.getY() + 20, r.getWidth() - 12, 16, juce::Justification::centredLeft);
}
} // namespace
StackRow::StackRow(StackTab &stack, std::int64_t track) : stack_(stack), track_(track) {
    setTooltip("A channel's ADI-Q, sidechain slot and ADI-L: built in, present but off until used (ADR-0204 d1, "
               "ADR-0209). The row under the mouse opens for editing.");
}
bool StackRow::zoomed() const noexcept { return stack_.hovered() == track_; }
juce::Rectangle<int> StackRow::limiterArea() const noexcept {
    const int m = zoomed() ? 6 : 4;
    return {getWidth() - look::gap - kDrawerWidth, m, kDrawerWidth, std::max(0, getHeight() - 2 * m - 1)};
}
juce::Rectangle<int> StackRow::sidechainArea() const noexcept {
    return limiterArea().translated(-(kDrawerWidth + look::gap), 0);
}
juce::Rectangle<int> StackRow::eqArea() const noexcept {
    const auto drawer = sidechainArea();
    return {kNameWidth, drawer.getY(), std::max(0, drawer.getX() - look::gap - kNameWidth), drawer.getHeight()};
}
const char *StackRow::mode() const noexcept {
    if (const auto *t = stack_.root.reader().findTrack(track_))
        return stack_.provider().slots(*t).limiterFull ? "Full" : "Light";
    return "Light";
}
void StackRow::paint(juce::Graphics &g) {
    const bool focused = stack_.focused() == track_, open = zoomed();
    g.fillAll(focused ? look::laneSelectedAlt : look::panel);
    g.setColour(look::separator);
    g.fillRect(0, getHeight() - 1, getWidth(), 1);
    const auto &geo = stack_.root.arrangement.geometry;
    const auto rows = geo.rows(stack_.root.reader());
    const auto at = rows.find(track_);
    if (at >= rows.size())
        return;
    const auto &track = *rows[at].track;
    const auto colour = stack_.root.arrangement.colourFor(at);
    // The track's colour and its name, by its place in the arrangement's order.
    g.setColour(colour);
    g.fillRect(8, open ? 10 : (kHeight - 11) / 2, 11, 11);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    g.drawText(juce::String::fromUTF8(displayName(track.name, at + 1).c_str()), 26, open ? 5 : 0,
               kNameWidth - 34, open ? 20 : kHeight - 1, juce::Justification::centredLeft, true);
    const auto slots = stack_.provider().slots(track);
    if (open) {
        g.setColour(look::textDim);
        g.setFont(look::font(11.5f));
        g.drawText(slots.eq || slots.limiter || slots.sidechain != SidechainMode::Off ? "In use" : "Every slot off",
                   26, 24, kNameWidth - 34, 16, juce::Justification::centredLeft, true);
    }
    // ADI-Q: its graph, dormant as a flat dimmed line (ADR-0209 §1); opened, EQ Eight's grid with
    // its labels, ready to edit when the slot arrives.
    const auto eq = eqArea().toFloat();
    paintGrid(g, eq, open, !slots.eq);
    paintFlat(g, eq, open, slots.eq ? colour : look::textDim.brighter(0.25f));
    if (open && !slots.eq) {
        g.setColour(look::textDim.brighter(0.5f));
        g.setFont(look::font(11.5f));
        g.drawText("ADI-Q is off on this channel.", eq.reduced(24, 10).withTrimmedBottom(eq.getHeight() / 2),
                   juce::Justification::centred);
    }
    // The drawers: the sidechain slot, then ADI-L.
    paintDrawer(g, sidechainArea(), "Sidechain", sidechainName(slots.sidechain), slots.sidechain != SidechainMode::Off,
                open);
    paintDrawer(g, limiterArea(), "ADI-L", juce::String(mode()) + (slots.limiter ? "" : ", off"), slots.limiter, open);
    if (open) {
        // The sidechain slot's modes (ADR-0209 §3), and ADI-L's gain reduction, empty while off.
        g.setFont(look::font(11.f));
        g.setColour(look::textDim);
        const auto sc = sidechainArea();
        int y = sc.getY() + 42;
        for (auto m : {SidechainMode::Rmsc, SidechainMode::Spacer, SidechainMode::Fuser}) {
            g.drawText(sidechainName(m), sc.getX() + 14, y, sc.getWidth() - 18, 14, juce::Justification::centredLeft);
            y += 15;
        }
        const auto l = limiterArea();
        const juce::Rectangle<float> meter(static_cast<float>(l.getX() + 8), static_cast<float>(l.getY() + 44),
                                           static_cast<float>(l.getWidth() - 16), 10);
        g.setColour(look::display);
        g.fillRect(meter);
        g.setColour(look::outline);
        g.drawRect(meter, 1.f);
        g.setColour(look::textDim);
        g.drawText("Gain reduction", l.getX() + 8, l.getY() + 58, l.getWidth() - 12, 14,
                   juce::Justification::centredLeft);
    }
}
void StackRow::mouseEnter(const juce::MouseEvent &) { stack_.hover(track_); }
void StackRow::mouseExit(const juce::MouseEvent &) {
    if (stack_.hovered() == track_)
        stack_.hover(0);
}
void StackRow::mouseDown(const juce::MouseEvent &e) {
    stack_.focus(track_);
    // A slot says what it waits for; elsewhere the click selects the channel's track, as Live
    // selects a track from its mixer strip.
    const auto at = e.getPosition();
    if (eqArea().contains(at))
        stack_.root.say("The channel EQ slot is off until ADI-Q is handed over.");
    else if (sidechainArea().contains(at))
        stack_.root.say("The sidechain slot is off until the engine's channel slots arrive.");
    else if (limiterArea().contains(at))
        stack_.root.say("The channel limiter slot is off until ADI-L is handed over.");
    else {
        stack_.root.arrangement.geometry.selection.track = track_;
        stack_.root.publishSelection();
    }
}
StackTab::StackTab(AdiRootComponent &r, const MixViewProvider &p) : root(r), provider_(&p) {
    setTitle("STACK");
    scroll_.setViewedComponent(&list_, false);
    scroll_.setScrollBarsShown(true, false);
    addAndMakeVisible(scroll_);
}
juce::Rectangle<int> StackTab::display() const noexcept {
    return {look::gap, look::gap, std::max(0, getWidth() - 2 * look::gap), kDisplayHeight};
}
juce::String StackTab::scope() const {
    const auto group = root.state().viewGroup;
    if (group != 0)
        if (const auto *t = root.reader().findTrack(group))
            return juce::String::fromUTF8(t->name.c_str());
    return "Project";
}
void StackTab::frame(const SnapshotReader &reader) {
    // Every channel the project view shows, in the arrangement's order; refilled in place.
    const auto &geo = root.arrangement.geometry;
    wanted_.clear();
    for (const auto row : geo.rows(reader))
        if (geo.inView(row.track->id))
            wanted_.push_back(row.track->id);
    bool same = wanted_.size() == rows_.size();
    for (std::size_t i = 0; same && i < wanted_.size(); ++i)
        same = rows_[i]->track() == wanted_[i];
    if (same) {
        if (reader.seq() != shown_) {
            shown_ = reader.seq();
            repaint();
            list_.repaint();
        }
        return;
    }
    shown_ = reader.seq();
    rows_.clear();
    for (const auto id : wanted_) {
        rows_.push_back(std::make_unique<StackRow>(*this, id));
        list_.addAndMakeVisible(*rows_.back());
    }
    if (std::find(wanted_.begin(), wanted_.end(), hovered_) == wanted_.end())
        hovered_ = 0;
    layoutRows();
    repaint();
}
void StackTab::focus(std::int64_t track) {
    focused_ = track;
    for (auto &row : rows_) {
        row->repaint();
        if (row->track() == track)
            scroll_.setViewPosition(0, std::max(0, row->getY() - StackRow::kHeight));
    }
}
void StackTab::hover(std::int64_t track) {
    if (track == hovered_)
        return;
    hovered_ = track;
    layoutRows();
}
void StackTab::layoutRows() {
    const int width = std::max(0, scroll_.getWidth() - scroll_.getScrollBarThickness());
    int y = 0;
    for (auto &row : rows_) {
        const int h = row->track() == hovered_ ? StackRow::kZoomedHeight : StackRow::kHeight;
        row->setBounds(0, y, width, h);
        y += h;
    }
    list_.setSize(width, y);
    list_.repaint();
}
void StackTab::resized() {
    const int top = kDisplayHeight + 2 * look::gap + kHeaderHeight;
    scroll_.setBounds(look::gap, top, std::max(0, getWidth() - 2 * look::gap), std::max(0, getHeight() - top - look::gap));
    layoutRows();
}
void StackTab::paint(juce::Graphics &g) {
    g.fillAll(look::frame);
    // The shared display, Pro-Q's instances over one grid: each engaged ADI-Q's curve in its
    // track's colour; while none is, the flat line every EQ that is off leaves.
    const auto area = display().toFloat();
    paintGrid(g, area, true, false);
    bool any = false;
    const auto &geo = root.arrangement.geometry;
    const auto all = geo.rows(root.reader());
    for (std::size_t i = 0; i < all.size(); ++i)
        if (geo.inView(all[i].track->id) && provider().slots(*all[i].track).eq) {
            any = true;
            paintFlat(g, area, true, root.arrangement.colourFor(i));
        }
    if (!any) {
        paintFlat(g, area, true, look::value);
        g.setColour(look::textDim.brighter(0.4f));
        g.setFont(look::font(11.5f));
        g.drawText("No channel EQ is on.", area.reduced(24, 10).withTrimmedBottom(area.getHeight() / 2),
                   juce::Justification::centred);
    }
    // The list's head, as Live's column names, over the rows' own columns.
    const juce::Rectangle<int> head(look::gap, kDisplayHeight + 2 * look::gap, getWidth() - 2 * look::gap,
                                    kHeaderHeight);
    g.setColour(look::columnHead);
    g.fillRect(head);
    g.setColour(look::text);
    g.setFont(look::font(12.f, true));
    const int width = std::max(0, scroll_.getWidth() - scroll_.getScrollBarThickness());
    const int limiterX = width - look::gap - StackRow::kDrawerWidth,
              sidechainX = limiterX - look::gap - StackRow::kDrawerWidth;
    g.drawText("Channels: " + scope(), head.getX() + 26, head.getY(), StackRow::kNameWidth - 30, head.getHeight(),
               juce::Justification::centredLeft, true);
    g.drawText("ADI-Q", head.getX() + StackRow::kNameWidth, head.getY(), 60, head.getHeight(),
               juce::Justification::centredLeft);
    g.drawText("Sidechain", head.getX() + sidechainX, head.getY(), StackRow::kDrawerWidth, head.getHeight(),
               juce::Justification::centredLeft);
    g.drawText("ADI-L", head.getX() + limiterX, head.getY(), StackRow::kDrawerWidth, head.getHeight(),
               juce::Justification::centredLeft);
}
} // namespace adi::ui
