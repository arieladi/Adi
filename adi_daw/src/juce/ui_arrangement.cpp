// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_arrangement.hpp"
#include "adi/automation_shapes.hpp"
#include "adi/automation_splice_store.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/automation_override.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/engine/curves.hpp"
#include "adi/ui/clip_edits.hpp"
#include "adi/ui/fade_edits.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_time.hpp"
#include "adi/row_ids.hpp"
#include "adi/take_lane_ops.hpp"
#include "adi/ui/meter_edits.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/range_edits.hpp"
#include "adi/ui/recording_edits.hpp"
#include "adi/warp_commands.hpp"
#include "ui_browser.hpp"
#include "ui_controls.hpp"
#include "ui_decimal.hpp"
#include "ui_shell.hpp"
#include "ui_value_entry.hpp"
#include <array>
#include <cmath>
#include <iterator>
#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <string_view>
namespace adi::ui {
namespace {
void paintLoopTurn(juce::Graphics &g, int x, float top, float bottom, bool muted) {
    g.setColour(look::ink.withAlpha(muted ? .22f : .47f));
    g.drawVerticalLine(x, top, bottom);
}
const juce::MouseCursor &arrangementZoomCursor() {
    static const juce::MouseCursor cursor = [] {
        constexpr int size = 18, scale = 2;
        juce::Image image(juce::Image::ARGB, size * scale, size * scale, true, juce::SoftwareImageType{});
        {
            juce::Graphics g(image);
            g.addTransform(juce::AffineTransform::scale(static_cast<float>(scale)));
            const juce::Rectangle<float> lens{2.f, 2.f, 10.f, 10.f};
            const juce::Line<float> handle{11.f, 11.f, 15.5f, 15.5f};
            g.setColour(juce::Colours::white);
            g.drawEllipse(lens, 3.5f);
            g.drawLine(handle, 4.f);
            g.setColour(juce::Colours::black);
            g.drawEllipse(lens, 1.5f);
            g.drawLine(handle, 2.f);
        }
        return juce::MouseCursor(image, 7, 7, static_cast<float>(scale));
    }();
    return cursor;
}
class ColourChooser final : public juce::PopupMenu::CustomComponent {
  public:
    static constexpr int kSwatch = 14, kPitch = 17, kColumns = 14, kInset = 4;
    static constexpr int kRows = static_cast<int>(kPaletteColours) / kColumns;
    static_assert(kRows * kColumns == kPaletteColours);
    ColourChooser(std::function<void(int)> pick, std::int64_t current)
        : juce::PopupMenu::CustomComponent(false), pick_(std::move(pick)), current_(current) {}
    void getIdealSize(int &width, int &height) override {
        width = 2 * kInset + kColumns * kPitch;
        height = 2 * kInset + (kRows - 1) * kPitch + kSwatch;
    }
    void paint(juce::Graphics &g) override {
        for (int i = 0; i < kRows * kColumns; ++i) {
            const juce::Rectangle<int> r(kInset + (i % kColumns) * kPitch, kInset + (i / kColumns) * kPitch,
                                         kSwatch, kSwatch);
            g.setColour(look::paletteColour(i));
            g.fillRect(r);
            if (i == current_ || i == hover_) {
                g.setColour(i == current_ ? look::textBright : look::textBright.withAlpha(.6f));
                g.drawRect(r.toFloat().expanded(1.5f), 1.5f);
            }
        }
    }
    void mouseMove(const juce::MouseEvent &e) override {
        if (const int at = swatchAt(e.getPosition()); at != hover_) {
            hover_ = at;
            repaint();
        }
    }
    void mouseExit(const juce::MouseEvent &) override {
        hover_ = -1;
        repaint();
    }
    void mouseUp(const juce::MouseEvent &e) override {
        if (const int at = swatchAt(e.getPosition()); at >= 0) {
            pick_(at);
            if (getParentComponent() != nullptr)
                triggerMenuItem();
        }
    }

  private:
    static int swatchAt(juce::Point<int> point) noexcept {
        const int x = point.x - kInset, y = point.y - kInset;
        if (x < 0 || y < 0 || x / kPitch >= kColumns || y / kPitch >= kRows ||
            x % kPitch >= kSwatch || y % kPitch >= kSwatch)
            return -1;
        return (y / kPitch) * kColumns + x / kPitch;
    }
    std::function<void(int)> pick_;
    std::int64_t current_;
    int hover_ = -1;
};
} // namespace
void paintPlaybackProblemMark(juce::Graphics &g, juce::Rectangle<float> bounds) {
    g.setColour(look::error);
    g.fillRect(bounds);
    if (bounds.getWidth() < 5.f)
        return;
    g.setColour(look::error.contrasting());
    const auto centre = bounds.getCentreX();
    g.fillRect(centre - .75f, bounds.getY() + 1.5f, 1.5f, 3.5f);
    g.fillRect(centre - .75f, bounds.getY() + 6.f, 1.5f, 1.5f);
}
ArrangementOverview::ArrangementOverview(ArrangementView &v) : view(v) {
    setTitle("Overview");
    setMouseCursor(arrangementZoomCursor());
    setTooltip("The whole arrangement. Drag sideways to scroll, up or down to zoom; "
               "double-click to see all of it.");
}
namespace {
constexpr auto kAssignClipColour = "Assign Track Color to Clips";
constexpr auto kAssignGroupedClipColour = "Assign Track Color to Grouped Tracks and Clips";

template <typename Peaks, typename FrameAt>
void paintPeakColumns(juce::Graphics &g, const Peaks &peaks, int from, int to, float top, float bottom,
                      std::uint32_t sourceChannels, std::span<const std::uint32_t> channelMap, FrameAt frameAt,
                      double sourceEnd = std::numeric_limits<double>::infinity(), float gain = 1.f) {
    const auto channels = channelMap.empty() ? sourceChannels : static_cast<std::uint32_t>(channelMap.size());
    if (channels == 0 || bottom - top < 4.f)
        return;
    const float band = (bottom - top) / static_cast<float>(channels);
    for (int x = from; x < to; ++x) {
        const auto frame = frameAt(x);
        if (!frame || *frame >= sourceEnd)
            continue;
        const double end = frameAt(x + 1).value_or(*frame + 1.);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const auto source = channelMap.empty() ? ch : channelMap[ch];
            const auto peak = peakSpan(peaks, std::min(*frame, end), std::min(sourceEnd, std::max(*frame + 1., end)),
                                       source);
            if (!peak)
                continue;
            const float mid = top + band * (static_cast<float>(ch) + .5f), half = band * .5f;
            const float y0 = mid - std::clamp(peak->maximum * gain, -1.f, 1.f) * half,
                        y1 = mid - std::clamp(peak->minimum * gain, -1.f, 1.f) * half;
            g.fillRect(static_cast<float>(x), y0, 1.f, std::max(1.f, y1 - y0));
        }
    }
}

// Live's lanes alternate shades every sixteen grid steps. Bar lines have more contrast
// than beat lines against the underlying lane in both selected and unselected tracks.
void paintLanes(juce::Graphics &g, const ArrangementGeometry &geo, int width, int top, int height,
                bool selected) {
    // The shading follows the plain step, so triplets keep it on the bars.
    const double grid = geo.gridQuarters();
    const double group = geo.plainGridQuarters() * 16;
    const bool metered = geo.metered();
    const auto shade = [selected](bool alternate) {
        return selected ? (alternate ? look::laneSelectedAlt : look::laneSelected)
                        : (alternate ? look::laneAlt : look::lane);
    };
    for (double q = std::floor(geo.left / group) * group; q < geo.left + width / geo.scale; q += group) {
        const bool alt = static_cast<std::int64_t>(std::llround(q / group)) % 2 != 0;
        const double x0 = (q - geo.left) * geo.scale, x1 = x0 + group * geo.scale;
        const auto lane = shade(alt);
        g.setColour(lane);
        g.fillRect(static_cast<float>(x0), static_cast<float>(top), static_cast<float>(x1 - x0),
                   static_cast<float>(height));
        if (metered)
            continue;
        const auto barInk = lane.darker(.66f), beatInk = lane.darker(.33f);
        for (double line = q; line < q + group && line < geo.left + width / geo.scale; line += grid)
            if (line >= geo.left) {
                const bool bar = std::llround(line * textproj::kPPQ) % textproj::kWhole == 0;
                g.setColour(bar ? barInk : beatInk);
                g.fillRect(static_cast<int>((line - geo.left) * geo.scale), top, 1, height);
            }
    }
    if (metered)
        geo.forEachGridLine(geo.tick(0.), geo.tick(static_cast<double>(width)), [&](std::int64_t line) {
            const double q = static_cast<double>(line) / textproj::kPPQ;
            const bool alt = static_cast<std::int64_t>(std::floor(q / group)) % 2 != 0;
            const bool bar = geo.barHolding(line).start == line;
            g.setColour(shade(alt).darker(bar ? .66f : .33f));
            g.fillRect(static_cast<int>(geo.x(line)), top, 1, height);
        });
}
} // namespace
std::int64_t ArrangementOverview::span() const noexcept {
    const auto &g = view.geometry;
    const auto visibleEnd = g.tick(static_cast<double>(view.canvas.getWidth()));
    return std::max<std::int64_t>({g.end(view.reader()), visibleEnd, textproj::kPPQ});
}
juce::Rectangle<float> ArrangementOverview::outline() const noexcept {
    const auto &g = view.geometry;
    const double total = static_cast<double>(span()), w = getWidth();
    const double from = g.left * textproj::kPPQ;
    const double to = static_cast<double>(g.tick(static_cast<double>(view.canvas.getWidth())));
    return {static_cast<float>(from / total * w), 0.f,
            static_cast<float>(std::max(2., (to - from) / total * w)),
            static_cast<float>(getHeight())};
}
void ArrangementOverview::paint(juce::Graphics &g) {
    // Live's Overview: a grey line per track on the dark band, its clips in the
    // clip's colour, and a black outline around what is on screen.
    g.fillAll(look::band);
    const auto rows = view.geometry.rows(view.reader());
    const double total = static_cast<double>(span()), w = getWidth();
    // The tracks the lanes show, in their order.
    std::size_t shown = 0;
    for (const auto r : rows)
        if (view.geometry.shown(r.track->id))
            ++shown;
    const float row = std::max(2.f, static_cast<float>(getHeight() - 4) /
                                        static_cast<float>(std::max<std::size_t>(4, shown)));
    float y = 2;
    for (const auto r : rows) {
        if (!view.geometry.shown(r.track->id))
            continue;
        const float line = std::max(1.f, row - 1);
        g.setColour(look::panel);
        g.fillRect(0.f, y, static_cast<float>(w), line);
        const auto trackColour = view.colourFor(r.position);
        for (const auto &c : r.track->clips) {
            const auto colour = c->color ? look::paletteColour(*c->color) : trackColour;
            g.setColour(r.track->muted || c->muted ? look::mutedClip : colour);
            g.fillRect(static_cast<float>(static_cast<double>(c->posTicks) / total * w), y,
                       std::max(1.f, static_cast<float>(static_cast<double>(c->lengthTicks) / total * w)),
                       line);
        }
        y += row;
    }
    g.setColour(look::overviewFrame);
    g.drawRect(outline(), look::heavyLine);
}
void ArrangementOverview::mouseDown(const juce::MouseEvent &e) { last = e.position; }
void ArrangementOverview::mouseDrag(const juce::MouseEvent &e) {
    const auto d = e.position - last;
    last = e.position;
    view.followPaused = true; // a sideways scroll
    // Sideways moves the outline: the overview's pixels are the whole span's.
    const double quarters = d.x * static_cast<double>(span()) / textproj::kPPQ /
                            std::max(1, getWidth());
    view.geometry.pan(quarters * view.geometry.scale);
    // Vertically zooms around what is in the middle of the screen.
    view.geometry.zoom(std::exp(-d.y * .02), view.canvas.getWidth() / 2.);
    view.changed();
}
void ArrangementOverview::mouseDoubleClick(const juce::MouseEvent &) { view.fit(false); }
ScrubArea::ScrubArea(ArrangementView &v) : view(v) {
    setTitle("Scrub area");
    setTooltip("Click to play from that point. Click a time signature marker to select it; "
               "right-click for Insert Time Signature Change.");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    addChildComponent(editor);
    editor.setTitle("Time signature");
    editor.setJustification(juce::Justification::centredLeft);
    editor.onCommit = [this] {
        if (locatorEditing_ != 0) {
            const auto id = locatorEditing_;
            const auto text = editor.getText().toStdString();
            if (editingInfo_ ? view.root.setLocatorInfo(id, text) : view.root.renameLocator(id, text))
                endMeterEdit();
            else if (editor.isShowing())
                editor.selectAll();
            return;
        }
        if (editing_ < 0)
            return;
        const auto pos = editing_;
        const auto text = editor.getText().toStdString();
        // A value refused keeps the field open to type again; the Status Bar says why.
        if (inserting_ ? view.root.insertMeter(pos, text) : view.root.editMeter(pos, text))
            endMeterEdit();
        else if (editor.isShowing())
            editor.selectAll();
    };
    editor.onCancel = [this] { endMeterEdit(); };
}
juce::Rectangle<float> ScrubArea::markerBox(const textproj::Meter &m) const {
    const auto text = juce::String(m.numerator) + "/" + juce::String(m.denominator);
    const float x = static_cast<float>(view.geometry.x(m.start_ticks));
    const float w = 18.f + juce::GlyphArrangement::getStringWidth(look::font(look::fontBody), text);
    return {x, kBraceHeight, w, 15.f}; // under the loop brace
}
juce::Rectangle<float> ScrubArea::locatorBox(const Locator &l) const {
    const float x = static_cast<float>(view.geometry.x(l.pos));
    const float w = 18.f + juce::GlyphArrangement::getStringWidth(look::font(look::fontBody), juce::String::fromUTF8(l.name.c_str()));
    return {x, kBraceHeight, w, 15.f};
}
std::int64_t ScrubArea::locatorAt(float x) const {
    const auto *snapshot = view.reader().get();
    if (snapshot == nullptr)
        return 0;
    const auto locators = locatorsOf(*snapshot);
    // The later one is drawn over the earlier where two touch.
    for (auto it = locators.rbegin(); it != locators.rend(); ++it)
        if (const auto box = locatorBox(*it); x >= box.getX() && x < box.getRight())
            return it->id;
    return 0;
}
bool ScrubArea::beginLocatorEdit(std::int64_t id, bool info) {
    const auto *snapshot = view.reader().get();
    const auto locators = snapshot != nullptr ? locatorsOf(*snapshot) : std::vector<Locator>{};
    const auto *l = locatorById(locators, id);
    if (l == nullptr)
        return false;
    if (const auto x = view.geometry.x(l->pos); x < 0 || x > getWidth() - 120) {
        view.geometry.left = std::max(0., static_cast<double>(l->pos) / textproj::kPPQ -
                                              static_cast<double>(getWidth()) / 4. / std::max(view.geometry.scale, 1.));
        view.changed();
    }
    editing_ = -1;
    locatorEditing_ = id;
    editingInfo_ = info;
    const int x = std::clamp(static_cast<int>(view.geometry.x(l->pos)) + 12, 0, std::max(0, getWidth() - 120));
    editor.setTitle(info ? "Locator info text" : "Locator name");
    editor.setBounds(x, 2, 120, std::max(0, getHeight() - 4));
    editor.setText(juce::String::fromUTF8((info ? l->note : l->name).c_str()), false);
    editor.setVisible(true);
    editor.selectAll();
    if (editor.isShowing())
        focusIfShowing(editor);
    repaint();
    return true;
}
std::int64_t ScrubArea::markerAt(float x) const {
    const auto &meters = view.reader().meters();
    // The later one is drawn over the earlier where two touch.
    for (std::size_t i = meters.size(); i-- > 1;)
        if (const auto box = markerBox(meters[i]); x >= box.getX() && x < box.getRight())
            return meters[i].start_ticks;
    return -1;
}
juce::Rectangle<float> ScrubArea::braceBox(LoopRange r) const {
    const float x0 = static_cast<float>(view.geometry.x(r.start)), x1 = static_cast<float>(view.geometry.x(r.end));
    return {x0, 0.f, std::max(2.f, x1 - x0), kBraceHeight};
}
ScrubArea::BracePart ScrubArea::bracePart(juce::Point<float> at) const {
    if (at.y < 0 || at.y >= kBraceHeight)
        return BracePart::None;
    const auto box = braceBox(loopOf(view.reader().transport()));
    // An edge within kBraceEdge of the mouse, the nearer one where a short brace puts both there.
    const float toStart = std::abs(at.x - box.getX()), toEnd = std::abs(at.x - box.getRight());
    if (std::min(toStart, toEnd) <= kBraceEdge)
        return toStart < toEnd ? BracePart::Start : BracePart::End;
    return at.x > box.getX() && at.x < box.getRight() ? BracePart::Bar : BracePart::None;
}
void ScrubArea::paint(juce::Graphics &g) {
    // Live's locator band: the lanes' grey and grid, where the loop brace and the
    // locators sit.
    paintLanes(g, view.geometry, getWidth(), 0, getHeight(), false);
    // A marker at each change of time signature (Live 12 manual 6.4.1, its figure): a grey box
    // with a dark edge, a notch at its top left, the signature in it. The first signature is the
    // control bar's and has none. The selected one is lit (its colour to be measured on Live).
    const auto &meters = view.reader().meters();
    // Live's fragmentary bars (manual 6.5, its figure): a crosshatched band in the markers' row, from
    // the bar line before a change placed too early to the change.
    forEachFragmentaryBar(meters, [&](std::int64_t start, std::int64_t end) {
        const float x0 = static_cast<float>(view.geometry.x(start)), x1 = static_cast<float>(view.geometry.x(end));
        if (x1 < 0 || x0 > static_cast<float>(getWidth()))
            return;
        const juce::Rectangle<float> band(x0, kBraceHeight, x1 - x0, 15.f);
        g.setColour(look::loopBand);
        g.fillRect(band);
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(band.getSmallestIntegerContainer());
            g.setColour(look::gridLine);
            for (float x = x0 - band.getHeight(); x < x1; x += 6.f)
                g.drawLine(x, band.getBottom(), x + band.getHeight(), band.getY(), 1.5f);
        }
        g.setColour(look::separator);
        g.drawRect(band, 1.5f);
    });
    // Live's loop brace (manual 6.6, its figure): a bar along the band's top over the loop, with a
    // light triangle at each end pointing in; dark while the loop is on, light while it is off. Its
    // colours and the selected brace's outline are ADI's until they are measured on Live 11.
    {
        const auto &t = view.reader().transport();
        const auto range = braceMoving().value_or(loopOf(t));
        if (const auto box = braceBox(range); box.getRight() >= 0 && box.getX() <= static_cast<float>(getWidth())) {
            g.setColour(t.loopEnabled ? look::loopOn : look::loopOff);
            g.fillRect(box);
            juce::Path ends;
            const float h = box.getHeight(), mid = box.getCentreY();
            ends.addTriangle(box.getX() + 1.f, box.getY() + 1.f, box.getX() + 1.f, box.getBottom() - 1.f,
                             box.getX() + h - 1.f, mid);
            ends.addTriangle(box.getRight() - 1.f, box.getY() + 1.f, box.getRight() - 1.f, box.getBottom() - 1.f,
                             box.getRight() - h + 1.f, mid);
            g.setColour(look::markerText);
            g.fillPath(ends);
            if (view.loopSelected) {
                g.setColour(look::selection);
                g.drawRect(box, 1.5f);
            }
        }
    }
    g.setFont(look::font(look::fontBody));
    for (std::size_t i = 1; i < meters.size(); ++i) {
        auto m = meters[i];
        if (m.start_ticks == dragged_ && movingTo_ >= 0)
            m.start_ticks = movingTo_; // drawn where the drag would put it
        const auto box = markerBox(m);
        if (box.getRight() < 0 || box.getX() > static_cast<float>(getWidth()))
            continue;
        const float x = box.getX();
        g.setColour(meters[i].start_ticks == view.selectedMeter ? look::selection : look::marker);
        g.fillRect(box);
        g.setColour(look::separator);
        g.drawRect(box, 1.5f);
        juce::Path notch;
        notch.addTriangle(x + 2.f, box.getBottom() - 3.f, x + 2.f, box.getY() + 2.f, x + 10.f, box.getY() + 2.f);
        g.setColour(look::markerText);
        g.fillPath(notch);
        g.setColour(look::text);
        g.drawText(juce::String(meterText(m)), box.withTrimmedLeft(12.f), juce::Justification::centredLeft, false);
    }
    // Live's locators (manual 6.4, its figures): a light box with a dark edge from each, a triangle
    // pointing right at its start, its name after it; the selected one lit. Its colours are ADI's
    // until measured on Live 11.
    if (const auto *snapshot = view.reader().get())
        for (auto l : locatorsOf(*snapshot)) {
            if (l.id == locatorDragged_ && locatorTo_ >= 0)
                l.pos = locatorTo_; // drawn where the drag would put it
            const auto box = locatorBox(l);
            if (box.getRight() < 0 || box.getX() > static_cast<float>(getWidth()))
                continue;
            const bool lit = l.id == view.selectedLocator;
            g.setColour(lit ? look::selection : look::marker);
            g.fillRect(box);
            g.setColour(look::separator);
            g.drawRect(box, 1.5f);
            juce::Path mark;
            mark.addTriangle(box.getX() + 2.f, box.getY() + 2.f, box.getX() + 2.f, box.getBottom() - 2.f,
                             box.getX() + 10.f, box.getCentreY());
            g.setColour(look::markerText);
            g.fillPath(mark);
            g.setColour(look::separator);
            g.strokePath(mark, juce::PathStrokeType(1.f));
            g.setColour(look::text);
            g.drawText(juce::String::fromUTF8(l.name.c_str()), box.withTrimmedLeft(13.f), juce::Justification::centredLeft,
                       false);
        }
}
void ScrubArea::mouseEnter(const juce::MouseEvent &e) { mouseMove(e); }
void ScrubArea::mouseMove(const juce::MouseEvent &e) {
    const auto part = bracePart(e.position);
    const bool resizes = part == BracePart::Start || part == BracePart::End;
    setMouseCursor(resizes ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::PointingHandCursor);
}
void ScrubArea::mouseExit(const juce::MouseEvent &) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
void ScrubArea::mouseDown(const juce::MouseEvent &e) {
    view.canvas.clearAutomationSelection();
    // The loop brace pressed is selected, and the material in the loop with it (manual 6.6: "Clicking
    // on the loop brace is a shortcut for executing the Edit menu's Select Loop command"); it plays
    // nothing, and a drag moves an edge or the whole brace.
    brace_ = BracePart::None;
    if (const auto part = bracePart(e.position); part != BracePart::None && !e.mods.isPopupMenu()) {
        brace_ = part;
        braceGrab_ = view.geometry.tick(e.position.x);
        braceFrom_ = braceTo_ = loopOf(view.reader().transport());
        view.selectedMeter = -1;
        view.selectedLocator = 0; // one thing selected: Delete and the arrows must not reach a locator
        view.selectLoop();
        view.loopSelected = true;
        focusIfShowing(view.canvas); // the arrows go to the lanes, which act on the brace
        beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
        repaint();
        return;
    }
    if (view.loopSelected) {
        view.loopSelected = false; // anything else pressed here lets the brace go
        repaint();
    }
    // A marker clicked is selected, and plays nothing; its menu acts on it; a drag moves it.
    const auto marker = e.position.y >= kBraceHeight ? markerAt(e.position.x) : -1;
    // A locator pressed is selected (manual 6.4): let go without a drag, it jumps there; a drag moves
    // it; its menu acts on it. A time signature marker drawn over it is pressed first.
    locatorDragged_ = 0;
    if (const auto locator = marker < 0 && e.position.y >= kBraceHeight ? locatorAt(e.position.x) : 0; locator != 0) {
        view.selectedLocator = locator;
        view.selectedMeter = -1;
        const auto *snapshot = view.reader().get();
        const auto locators = snapshot != nullptr ? locatorsOf(*snapshot) : std::vector<Locator>{};
        const auto pos = locatorById(locators, locator) != nullptr ? locatorById(locators, locator)->pos : 0;
        if (e.mods.isPopupMenu()) {
            repaint();
            menu(view.geometry.tick(e.position.x))
                .showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
            return;
        }
        locatorDragged_ = locator;
        locatorGrab_ = view.geometry.tick(e.position.x) - pos;
        locatorTo_ = pos;
        locatorMoved_ = false;
        focusIfShowing(view.canvas); // Delete and the arrows go to the lanes, which act on it
        beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
        repaint();
        return;
    }
    if (view.selectedLocator != 0) {
        view.selectedLocator = 0; // anything else pressed here lets the locator go
        repaint();
    }
    dragged_ = e.mods.isPopupMenu() ? -1 : marker;
    movingTo_ = marker;
    grabbed_ = marker >= 0 ? view.geometry.tick(e.position.x) - marker : 0;
    if (marker >= 0 || view.selectedMeter >= 0) {
        view.selectedMeter = marker;
        if (marker >= 0) {
            // Delete and the arrows go to the lanes, which act on the marker while it is selected.
            auto &sel = view.geometry.selection;
            sel.clip = 0;
            sel.pickedClips.clear();
            focusIfShowing(view.canvas);
        }
        repaint();
    }
    if (e.mods.isPopupMenu()) {
        menu(view.geometry.tick(e.position.x))
            .showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
        return;
    }
    if (marker >= 0)
        return;
    view.followPaused = false;
    // From the point clicked; Live's global launch quantization arrives with Session
    // View's launch settings (ADR-0203 d10), so nothing is quantized yet.
    view.root.playFrom(view.geometry.tick(e.position.x));
}
void ScrubArea::mouseDrag(const juce::MouseEvent &e) {
    const auto pan = [&] {
        if (e.mouseWasDraggedSinceMouseDown() && view.edgeScroll(e.x, e.y, true, false))
            view.frame(true);
    };
    if (locatorDragged_ != 0) {
        if (e.getDistanceFromDragStart() < 3 && !locatorMoved_)
            return; // a click, not a drag
        locatorMoved_ = true;
        pan();
        const auto raw = view.geometry.tick(e.position.x) - locatorGrab_;
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        locatorTo_ = std::max<std::int64_t>(0, !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up));
        repaint();
        return;
    }
    if (brace_ != BracePart::None) {
        pan();
        // To the nearest grid line while snapping (manual 6.6: "dragging from the left or right edge
        // adjusts the loop start/end points, while dragging the brace bar horizontally moves the loop
        // without changing its length"); never before the start, never shorter than a grid step.
        const auto nearest = [this, &e](std::int64_t raw) {
            if (!view.snaps(e.mods))
                return raw;
            const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
            return raw - down <= up - raw ? down : up;
        };
        const auto delta = view.geometry.tick(e.position.x) - braceGrab_;
        auto to = braceFrom_;
        switch (brace_) {
        case BracePart::Start:
            to.start = std::clamp<std::int64_t>(nearest(braceFrom_.start + delta), 0,
                                                view.geometry.nextGridLine(braceFrom_.end, false));
            break;
        case BracePart::End:
            to.end = std::max(nearest(braceFrom_.end + delta), view.geometry.nextGridLine(braceFrom_.start, true));
            break;
        case BracePart::Bar:
            to.start = std::max<std::int64_t>(0, nearest(braceFrom_.start + delta));
            to.end = to.start + braceFrom_.length();
            break;
        case BracePart::None:
            break;
        }
        if (to.end <= to.start)
            return;
        braceTo_ = to;
        repaint();
        return;
    }
    if (dragged_ < 0)
        return;
    const auto raw = view.geometry.tick(e.position.x) - grabbed_;
    const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
    const auto snapped = !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up);
    // Never onto the start, which is the control bar's: at the first grid line at the earliest.
    movingTo_ = std::max(snapped, view.snaps(e.mods) ? view.geometry.nextGridLine(0, true) : std::int64_t{1});
    repaint();
}
void ScrubArea::mouseUp(const juce::MouseEvent &) {
    if (locatorDragged_ != 0) {
        const auto id = locatorDragged_, to = locatorTo_;
        const bool moved = locatorMoved_;
        locatorDragged_ = 0;
        locatorTo_ = -1;
        locatorMoved_ = false;
        if (moved)
            view.root.moveLocator(id, to);
        else if (const auto *snapshot = view.reader().get())
            if (const auto locators = locatorsOf(*snapshot); const auto *l = locatorById(locators, id))
                view.root.jumpTo(l->pos); // "You can jump to locators by clicking on them"
        repaint();
        return;
    }
    if (brace_ != BracePart::None) {
        brace_ = BracePart::None;
        if (braceTo_ != braceFrom_ && view.root.setLoopRange(braceTo_))
            view.selectLoop(); // the selection follows the brace it selected
        repaint();
        return;
    }
    const auto from = dragged_, to = movingTo_;
    dragged_ = -1;
    movingTo_ = -1;
    if (from >= 0 && to >= 0 && to != from)
        view.root.moveMeter(from, to);
    repaint();
}
void ScrubArea::mouseDoubleClick(const juce::MouseEvent &e) {
    // "If the transport is stopped, double-clicking on a locator will select it and start playback
    // from that point" (manual 6.4).
    if (view.root.playing() || e.position.y < kBraceHeight)
        return;
    const auto id = locatorAt(e.position.x);
    const auto *snapshot = view.reader().get();
    if (id == 0 || snapshot == nullptr)
        return;
    const auto locators = locatorsOf(*snapshot);
    if (const auto *l = locatorById(locators, id)) {
        view.selectedLocator = id;
        view.root.playFrom(l->pos);
        repaint();
    }
}
juce::PopupMenu ScrubArea::menu(std::int64_t at) {
    // Live's scrub area menu (manual 6.5): Insert Time Signature Change at the insert marker; on a
    // marker, Edit Time Signature and Delete (the Edit menu's Rename and Delete, which act on the
    // selected marker).
    auto &commands = view.root.commands();
    commands.activate(view.root);
    juce::PopupMenu out;
    // Add Locator (manual 6.4: "the Add Locator option in the scrub area's context menu"); on a
    // locator, Rename, Edit Info Text, Delete Locator and Loop to Next Locator.
    out.addCommandItem(&commands.manager, AppCommands::AddLocator);
    if (view.selectedLocator != 0) {
        out.addCommandItem(&commands.manager, AppCommands::RenameTrack);
        out.addCommandItem(&commands.manager, AppCommands::EditInfoText);
        out.addCommandItem(&commands.manager, AppCommands::DeleteLocator);
        out.addItem("Loop to Next Locator", [this, id = view.selectedLocator] { view.root.loopToNextLocator(id); });
    }
    out.addSeparator();
    out.addCommandItem(&commands.manager, AppCommands::InsertTimeSignatureChange);
    if (view.selectedMeter >= 0) {
        out.addSeparator();
        out.addCommandItem(&commands.manager, AppCommands::RenameTrack);
        out.addCommandItem(&commands.manager, AppCommands::DeleteTrack);
    }
    if (at >= 0 && fragmentAt(at, view.reader().meters()).first >= 0) {
        out.addSeparator();
        const juce::Component::SafePointer<ScrubArea> safe(this);
        out.addItem("Delete Fragmentary Bar Time", [safe, at] {
            if (safe != nullptr)
                safe->repairFragmentaryBar(at, false);
        });
        out.addItem("Complete Fragmentary Bar (Insert Time)", [safe, at] {
            if (safe != nullptr)
                safe->repairFragmentaryBar(at, true);
        });
    }
    return out;
}
void ScrubArea::repairFragmentaryBar(std::int64_t at, bool complete) {
    const auto current = view.root.current();
    if (!current)
        return;
    const auto [start, end] = fragmentAt(at, current->meters);
    if (start < 0) {
        view.root.say("The fragmentary bar is no longer there.");
        return;
    }
    // Completion inserts the missing portion at the beginning of the fragment (Live 12 manual 6.5).
    const auto missing = meterAt(start, current->meters).barTicks() - (end - start);
    auto &store = view.root.editStore();
    const auto rate = view.root.editRate();
    auto edit = complete ? clip_edit::prepareCompleteFragmentaryBar(store, start, missing, rate)
                         : clip_edit::prepareDeleteTime(store, start, end, rate);
    if (!edit.requests.empty())
        edit.requests.front().label = complete ? "Complete Fragmentary Bar" : "Delete Fragmentary Bar Time";
    if (view.root.commitEdit(std::move(edit))) {
        view.selectedMeter = -1;
        view.changed();
    }
}
bool ScrubArea::beginMeterEdit(std::int64_t pos, bool inserting) {
    if (pos < 0)
        return false;
    // The marker's place on screen: the lanes scroll to it first when it is off them.
    if (const auto x = view.geometry.x(pos); x < 0 || x > getWidth() - 64) {
        view.geometry.left = static_cast<double>(pos) / textproj::kPPQ -
                             static_cast<double>(getWidth()) / 4. / std::max(view.geometry.scale, 1.);
        view.geometry.left = std::max(0., view.geometry.left);
        view.changed();
    }
    editing_ = pos;
    inserting_ = inserting;
    locatorEditing_ = 0;
    editor.setTitle("Time signature");
    const int x = std::clamp(static_cast<int>(view.geometry.x(pos)), 0, std::max(0, getWidth() - 64));
    editor.setBounds(x, 2, 64, std::max(0, getHeight() - 4));
    editor.setText(juce::String(meterText(meterAt(pos, view.reader().meters()))), false);
    editor.setVisible(true);
    editor.selectAll();
    if (editor.isShowing())
        focusIfShowing(editor);
    repaint();
    return true;
}
void ScrubArea::endMeterEdit() {
    editing_ = -1;
    locatorEditing_ = 0;
    editor.setVisible(false);
    if (view.canvas.isShowing())
        focusIfShowing(view.canvas);
    repaint();
}
TimelineRuler::TimelineRuler(ArrangementView &v) : view(v) {
    setTitle("Beat-time ruler");
    setMouseCursor(arrangementZoomCursor());
    setTooltip("Drag sideways to scroll, up or down to zoom. Double-click to zoom to the selection. "
               "Right-click to insert or edit a tempo change.");
    addChildComponent(tempoEditor);
    tempoEditor.setTitle("Tempo in BPM");
    tempoEditor.onCommit = [this] {
        if (view.root.setTempoChange(tempoEditing_, tempoEditor.getText().toStdString()))
            tempoEditor.setVisible(false);
        else
            tempoEditor.selectAll();
    };
    tempoEditor.onCancel = [this] { tempoEditor.setVisible(false); };
}
bool RenameEditor::keyPressed(const juce::KeyPress &k) {
    if (k == juce::KeyPress::returnKey) {
        if (onCommit)
            onCommit();
        return true;
    }
    if (k == juce::KeyPress::escapeKey) {
        if (onCancel)
            onCancel();
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::tabKey) {
        if (onTab)
            onTab(!k.getModifiers().isShiftDown());
        return true;
    }
    return juce::TextEditor::keyPressed(k);
}
TrackHeaderList::TrackHeaderList(ArrangementView &v) : view(v) {
    addChildComponent(editor);
    editor.setTitle("Track name");
    editor.onCommit = [this] {
        commitRename();
        endRename();
    };
    editor.onCancel = [this] { endRename(); };
    editor.onTab = [this](bool forward) {
        // Commit, then move to the neighbouring title bar shown to rename a series.
        const auto from = renaming_;
        commitRename();
        const auto rows = view.geometry.rows(view.reader());
        if (const auto at = rows.find(from); at < rows.size())
            for (auto i = at; forward ? i + 1 < rows.size() : i > 0;) {
                i = forward ? i + 1 : i - 1;
                if (view.geometry.shown(rows[i].track->id)) {
                    beginRename(rows[i].track->id);
                    return;
                }
            }
        endRename();
    };
    editor.onFocusLost = [this] {
        if (renaming_ != 0) {
            commitRename();
            endRename();
        }
    };
    setTitle("Track headers");
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(false);
    setTooltip("Click to select a track. Drag its edge to resize, with Alt to resize all. "
               "Main and Returns at the foot resize from the top. Right-click for track commands.");
}
ArrangementCanvas::ArrangementCanvas(ArrangementView &v) : view(v) {
    addChildComponent(clipEditor);
    clipEditor.setTitle("Clip name");
    clipEditor.onCommit = [this] {
        commitClipRename();
        endClipRename();
    };
    clipEditor.onCancel = [this] { endClipRename(); };
    clipEditor.onTab = [this](bool) {
        commitClipRename();
        endClipRename();
    };
    clipEditor.onFocusLost = [this] {
        if (renamingClip_ != 0) {
            commitClipRename();
            endClipRename();
        }
    };
    addChildComponent(automationValueEditor_);
    automationValueEditor_.setTitle("Automation breakpoint value");
    automationValueEditor_.setInputRestrictions(64);
    automationValueEditor_.onCommit=[this] {commitAutomationValue();};
    automationValueEditor_.onCancel=[this] {endAutomationValueEdit();};
    automationValueEditor_.onTab=[this](bool) {commitAutomationValue();};
    automationValueEditor_.onFocusLost=[this] {endAutomationValueEdit(false);};
    setWantsKeyboardFocus(true);
    setTitle("Arrangement");
    setDescription("Clips on a timeline. Up and down move between tracks; left and right move the "
                   "insert marker or the selection by the grid, or to a clip's edge with Ctrl "
                   "(Option on the Mac); Shift selects.");
    // In the Info View's shape: what it is, then its keys in brackets, as Live's.
    setTooltip("Shows every track's clips in time. Click to place the insert marker or "
               "select a clip; drag to select time, drag a clip's bar to move it, or its right "
               "edge to change its length.\n\n"
               "[Ctrl/Cmd + Scroll Wheel] Zoom In/Out\n"
               "[Alt + Scroll Wheel] Adjust Track Height\n"
               "[Up/Down] Move Between Tracks\n"
               "[Left/Right] Move Insert Marker or Selection\n"
               "[Ctrl/Option + Left/Right] Previous/Next Clip Edge\n"
               "[Shift + Left/Right] Select\n"
               "[Enter] at a Clip's Edge: Resize Clip\n"
               "[Delete] Delete Clip\n"
               "[0] Deactivate/Activate Clip");
}
void ArrangementCanvas::setPlaybackProblemIds(std::vector<std::int64_t> ids) {
    if (playbackProblemIds_ == ids)
        return;
    playbackProblemIds_ = std::move(ids);
    repaint();
}
bool ArrangementCanvas::hasPlaybackProblem(std::int64_t id) const noexcept {
    return std::binary_search(playbackProblemIds_.begin(), playbackProblemIds_.end(), id);
}
bool ArrangementCanvas::hasInfoAt(juce::Point<int> point) const {
    if (!getLocalBounds().contains(point))
        return false;
    const auto &geo = view.geometry;
    const auto &reader = view.reader();
    const auto tick = geo.tick(point.x);
    if (const auto *track = geo.trackAt(reader, point.y))
        return geo.hit(*track, tick) != nullptr;
    if (const auto take = geo.takeLaneAt(reader, point.y))
        return std::ranges::any_of(take->track->takeClips, [&](const auto &clip) {
            return clip->laneId == take->lane->id && tick >= clip->posTicks && tick - clip->posTicks < clip->lengthTicks;
        });
    return false;
}
juce::String ArrangementCanvas::getTooltip() {
    if (!playbackProblemIds_.empty()) {
        const auto mouse = getMouseXYRelative();
        std::int64_t track = 0;
        ResizeEdge edge = ResizeEdge::None;
        if (const auto *clip = barAt(mouse.x, mouse.y, track, edge); clip && hasPlaybackProblem(clip->id))
            return "This clip has a playback problem. See Playback Problems in the Status Bar.";
    }
    return juce::SettableTooltipClient::getTooltip();
}
std::unique_ptr<juce::AccessibilityHandler> ArrangementCanvas::createAccessibilityHandler() {
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::list);
}
ClipAccessible::ClipAccessible(ArrangementView &v) : view(v) {
    setInterceptsMouseClicks(false, false);
    setWantsKeyboardFocus(false);
}
void ClipAccessible::bind(std::int64_t t, std::int64_t c) {
    track = t;
    clip = c;
}
namespace {
// The title is built when a screen reader asks, never in the frame: a frame with no
// reader listening allocates nothing for accessibility.
class ClipHandler final : public juce::AccessibilityHandler {
  public:
    ClipHandler(ClipAccessible &c, ArrangementView &v)
        : AccessibilityHandler(c, juce::AccessibilityRole::listItem,
                               juce::AccessibilityActions().addAction(
                                   juce::AccessibilityActionType::press,
                                   [&c, &v] { v.selectClip(c.track, c.clip); })),
          clip_(c), view_(v) {}
    juce::String getTitle() const override { return view_.canvas.describe(clip_.track, clip_.clip); }

  private:
    ClipAccessible &clip_;
    ArrangementView &view_;
};
} // namespace
std::unique_ptr<juce::AccessibilityHandler> ClipAccessible::createAccessibilityHandler() {
    return std::make_unique<ClipHandler>(*this, view);
}
juce::String ArrangementCanvas::describe(std::int64_t trackId, std::int64_t clipId) const {
    const auto rows = view.geometry.rows(view.reader());
    if (const auto i = rows.find(trackId); i < rows.size()) {
        const auto *t = rows[i].track;
        for (const auto &c : t->clips) {
            if (c->id != clipId)
                continue;
            // Bar and beat in the project's meters (#299).
            const auto at = barPosition(c->posTicks, view.reader().meters());
            const auto beats = static_cast<double>(c->lengthTicks) / textproj::kPPQ;
            // The clip's name when it has one: "2 Audio, Verse, at bar 9 beat 1, 16.00 beats".
            const auto clip = c->name.empty() ? juce::String("clip") : juce::String::fromUTF8(c->name.c_str()) + ",";
            return juce::String::fromUTF8(displayName(t->name, rows[i].number()).c_str()) + ", " + clip +
                   " at bar " + juce::String(at.bar) + " beat " + juce::String(at.beat) + ", " +
                   juce::String(beats, 2) + " beats" +
                   (c->muted ? ", muted" : "") + (hasPlaybackProblem(clipId) ? ", playback problem" : "");
        }
    }
    return {};
}
void ArrangementCanvas::realiseAccessible() {
    std::size_t used = 0;
    const auto rows = view.geometry.rows(view.reader());
    const auto foot = view.geometry.footStart(rows);
    const int footY = view.geometry.footTop(view.reader());
    int y = -view.geometry.scrollY;
    for (const auto row : rows) {
        if (row.position == foot)
            y = footY;
        const auto *t = row.track;
        const int h = view.geometry.heightFor(t->id);
        // A track scrolled under the foot is out of sight.
        const int below = row.position < foot ? std::min(footY, getHeight()) : getHeight();
        if (h > 0 && y + h > 0 && y < below)
            for (const auto &c : t->clips) {
                const double x = view.geometry.x(c->posTicks),
                             end = view.geometry.x(c->posTicks + c->lengthTicks);
                if (end < 0 || x > getWidth())
                    continue;
                if (used == accessible_.size()) {
                    accessible_.push_back(std::make_unique<ClipAccessible>(view));
                    addChildComponent(*accessible_.back());
                }
                auto &a = *accessible_[used++];
                a.bind(t->id, c->id);
                const int left = static_cast<int>(std::max(0., x));
                a.setBounds(left, y + 3, std::max(1, static_cast<int>(std::min(
                                                         end, static_cast<double>(getWidth()))) -
                                                         left),
                            std::max(1, h - 7));
                a.setVisible(true);
            }
        y += view.geometry.rowHeight(*t);
    }
    for (std::size_t i = used; i < accessible_.size(); ++i)
        accessible_[i]->setVisible(false);
}
void ArrangementCanvas::announce(const juce::String &words) {
    // The keyboard moves the selection; a screen reader says where it landed.
    lastAnnouncement = words;
    if (words.isNotEmpty())
        juce::AccessibilityHandler::postAnnouncement(words,
                                                     juce::AccessibilityHandler::AnnouncementPriority::medium);
}
ArrangementView::ArrangementView(AdiRootComponent &r)
    : root(r), overview(*this), ruler(*this), scrub(*this), headers(*this), canvas(*this) {
    for (auto *c : std::array<juce::Component *, 6>{&overview, &ruler, &scrub, &headers, &canvas, &playhead})
        addAndMakeVisible(c);
    waveformZoom_.setComponentID("waveformVerticalZoom");
    waveformZoom_.setName("Waveform Vertical Zoom");
    waveformZoom_.setTitle("Waveform Vertical Zoom");
    waveformZoom_.setTooltip("Waveform Vertical Zoom for all audio clips in the Arrangement");
    waveformZoom_.setSliderStyle(juce::Slider::LinearHorizontal);
    waveformZoom_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 35, 15);
    waveformZoom_.setTextValueSuffix(" dB");
    waveformZoom_.setRange(0., 36., 1.);
    waveformZoom_.setDoubleClickReturnValue(true, 0.);
    waveformZoom_.onValueChange = [this] { canvas.repaint(); };
    addAndMakeVisible(waveformZoom_);
    // The tree the snapshot carries (#299).
    trackRows_ = rowsFromSnapshot;
    auto &show = root.namesBar.show;
    show.onClick = [this, &show] {
        projectViewMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&show));
    };
}
namespace {
// Bitwig's project view menu, as the director's screenshot shows it (28 October 2026): a dark
// menu with a thin light edge, large rows, the view in use on a light grey row.
class ProjectViewMenuLook final : public juce::LookAndFeel_V4 {
  public:
    void drawPopupMenuBackground(juce::Graphics &g, int width, int height) override {
        g.fillAll(look::menuBack);
        g.setColour(look::menuEdge);
        g.drawRect(0, 0, width, height, 1);
    }
    int getPopupMenuBorderSize() override { return 3; }
};
// One entry: indented by how deep it sits, a folder in its group's colour, its name.
class ViewMenuItem final : public juce::PopupMenu::CustomComponent {
  public:
    static constexpr int kIndent = 14, kRow = 26;
    ViewMenuItem(juce::String name, int depth, juce::Colour colour, bool current)
        : name_(std::move(name)), depth_(depth), colour_(colour), current_(current) {}
    void getIdealSize(int &width, int &height) override {
        width = 56 + depth_ * kIndent + juce::roundToInt(juce::GlyphArrangement::getStringWidth(look::font(look::fontTitle), name_));
        height = kRow;
    }
    void paint(juce::Graphics &g) override {
        if (current_ || isItemHighlighted()) {
            g.setColour(current_ ? look::menuRowCurrent : look::menuRowHover);
            g.fillRect(getLocalBounds());
        }
        // A folder: its body and the tab on its top left.
        const float x = 10.f + static_cast<float>(depth_ * kIndent);
        juce::Path folder;
        folder.addRoundedRectangle(x, 8.f, 16.f, 12.f, 1.5f);
        folder.addRoundedRectangle(x, 6.f, 7.f, 4.f, 1.f);
        g.setColour(colour_);
        g.fillPath(folder);
        g.setColour(current_ ? look::menuTextCurrent : look::menuText);
        g.setFont(look::font(look::fontTitle));
        const int textX = juce::roundToInt(x) + 25;
        g.drawText(name_, textX, 0, std::max(0, getWidth() - textX - 8), getHeight(),
                   juce::Justification::centredLeft);
    }

  private:
    juce::String name_;
    int depth_;
    juce::Colour colour_;
    bool current_;
};
} // namespace
ProjectViewButton::ProjectViewButton() : juce::Button("Project View") {
    setTitle("Project View");
    setButtonText(kAllGroups);
    setTooltip("Show: see the whole project, or only a group and what is inside it.");
    // A view command, as Bitwig's: it takes no keyboard focus.
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}
void ProjectViewButton::paintButton(juce::Graphics &g, bool over, bool down) {
    // The shared chooser (language.png part D) with its arrow; in it "Show", dim, then the view.
    const auto r = getLocalBounds().toFloat();
    controls::chooser(g, r, {}, look::fontCaption);
    const auto font = look::font(look::fontCaption);
    constexpr const char *kShow = "Show";
    // Narrower than the bar's least (the names alone), the view's name alone, with the arrow.
    const bool labelled = getWidth() >= NamesBar::kShowMin;
    const float label = labelled ? juce::GlyphArrangement::getStringWidth(font, kShow) : -4.f;
    const float left = r.getX() + 5.f, arrow = 13.f;
    g.setFont(font);
    g.setColour(over || down ? look::text : look::textDim);
    if (labelled)
        g.drawText(kShow, juce::Rectangle<float>(left, r.getY(), label + 1.f, r.getHeight()),
                   juce::Justification::centredLeft, false);
    // The view chosen, cut plain before the arrow as the shared chooser cuts its text (Live 12
    // manual, Figure 6.1).
    const float from = left + label + 4.f;
    const auto room = juce::Rectangle<float>(from, r.getY(), std::max(0.f, r.getRight() - arrow - from), r.getHeight());
    g.setFont(look::font(look::fontCaption, true));
    g.setColour(getToggleState() ? look::accent : look::text);
    juce::Graphics::ScopedSaveState keep(g);
    g.reduceClipRegion(room.getSmallestIntegerContainer());
    g.drawText(getButtonText(), room.withWidth(1000.f), juce::Justification::centredLeft, false);
}
BarButton::BarButton(const juce::String &name, const juce::String &tooltip, bool isCell, Glyph glyph)
    : juce::Button(name), cell(isCell), glyph_(std::move(glyph)) {
    setTitle(name);
    setTooltip(tooltip);
    setClickingTogglesState(false);
    // A view command, reached from the keyboard through the View menu: it takes no focus.
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}
void BarButton::paintButton(juce::Graphics &g, bool over, bool down) {
    const auto r = getLocalBounds().toFloat();
    const bool on = getToggleState();
    juce::Colour ink;
    if (cell) {
        // A cell of its group (part B): lit in the accent while on, its letters in the ink; off, dim,
        // brighter under the pointer.
        if (on) {
            g.setColour(look::accent);
            g.fillRoundedRectangle(r.reduced(1.f, 2.f), look::radiusBox);
        }
        ink = on ? look::ink : over || down ? look::text : look::textDim;
    } else {
        const auto state = controls::stateOf(on, over, down, isEnabled());
        controls::button(g, r, state);
        ink = controls::labelColour(state);
    }
    if (glyph_) {
        glyph_(g, r, ink);
        return;
    }
    g.setColour(ink);
    g.setFont(look::font(look::fontTiny, true));
    g.drawText(getButtonText(), r, juce::Justification::centred, false);
}
namespace {
// The bar's glyphs, as the sheet draws them: Flip Sides' two arrows, one each way; Automation Mode's
// envelope, a line between two breakpoints; Lock Envelopes' padlock.
void flipGlyph(juce::Graphics &g, juce::Rectangle<float> r, juce::Colour ink) {
    g.setColour(ink);
    const auto c = r.getCentre();
    juce::Path arrows;
    arrows.addArrow({c.x - 6.f, c.y - 2.5f, c.x + 6.f, c.y - 2.5f}, 1.4f, 5.f, 3.5f);
    arrows.addArrow({c.x + 6.f, c.y + 2.5f, c.x - 6.f, c.y + 2.5f}, 1.4f, 5.f, 3.5f);
    g.fillPath(arrows);
}
void automationGlyph(juce::Graphics &g, juce::Rectangle<float> r, juce::Colour ink) {
    g.setColour(ink);
    const auto c = r.getCentre();
    juce::Path curve;
    curve.startNewSubPath(c.x - 6.f, c.y + 5.f);
    curve.cubicTo(c.x - 1.f, c.y + 5.f, c.x - 1.f, c.y - 4.f, c.x + 4.f, c.y - 4.f);
    g.strokePath(curve, juce::PathStrokeType(1.5f));
    g.fillEllipse(c.x + 2.5f, c.y - 6.f, 4.f, 4.f);
}
void lockGlyph(juce::Graphics &g, juce::Rectangle<float> r, juce::Colour ink) {
    g.setColour(ink);
    const auto c = r.getCentre();
    g.drawRoundedRectangle(c.x - 4.5f, c.y - 0.5f, 9.f, 6.5f, 1.5f, 1.3f);
    juce::Path shackle;
    shackle.addCentredArc(c.x, c.y - 2.5f, 2.5f, 2.5f, 0.f, -juce::MathConstants<float>::halfPi,
                          juce::MathConstants<float>::halfPi, true);
    shackle.lineTo(c.x + 2.5f, c.y - 0.5f);
    shackle.startNewSubPath(c.x - 2.5f, c.y - 2.5f);
    shackle.lineTo(c.x - 2.5f, c.y - 0.5f);
    g.strokePath(shackle, juce::PathStrokeType(1.4f));
}
} // namespace
NamesBar::NamesBar()
    : flip("Flip Sides", "Flip sides", false, flipGlyph),
      inOut(juce::String::fromUTF8("I\xc2\xb7O"), "In/Out: show or hide the track controls' In/Out (Ctrl/Cmd+Alt+I).", true),
      sends("S", "Sends: show or hide the track controls' sends (Ctrl/Cmd+Alt+S).", true),
      returns("R", "Return Tracks: show or hide the return tracks (Ctrl/Cmd+Alt+R).", true),
      mixer("M", "Mixer: show or hide volume, pan and meters in the track controls.", true),
      delay("D", "Track Delay: show or hide the track controls' Track Delay (Live's Track Options).", true),
      height("H", "Optimize Arrangement Height (H).", true), width("W", "Optimize Arrangement Width (W).", true),
      automation("Automation Mode", "Automation Mode", false, automationGlyph),
      lock("Lock Envelopes", "Lock Envelopes", false, lockGlyph) {
    setOpaque(true);
    for (juce::Component *c : std::initializer_list<juce::Component *>{
             &flip, &show, &inOut, &sends, &returns, &mixer, &delay, &height, &width, &automation, &lock})
        addAndMakeVisible(c);
    for (auto *b : {&inOut, &sends, &returns, &mixer})
        b->setToggleState(true, juce::dontSendNotification);
}
void NamesBar::resized() {
    // The sheet's buttons (arrangement.png; its source, arrangement.html), 20 high and level in the
    // ruler's row, each a little narrower than drawn there, as the window's names and track controls
    // are Live's width, narrower than the sheet's; Show takes what is left. A bar too narrow for every
    // part (the track controls hidden, a small window) keeps Flip and Show, Show at least kShowMin wide,
    // and leaves out from the end: Automation Mode and Lock Envelopes, then H and W, then the sections.
    constexpr int kEdge = 2, kGap = 3, kPad = 2, kFlip = 22, kCell = 18, kSize = 18, kAutomation = 22, kLock = 20;
    const int h = std::min(20, std::max(0, getHeight() - 4));
    const int y = (getHeight() - h) / 2;
    const int sectionsW = 5 * kCell + 2 * kPad, sizesW = 2 * kSize + 2 * kPad;
    const int base = kEdge + kFlip + kGap + kEdge;
    const std::array<int, 3> extra{kGap + sectionsW, kGap + sizesW, kGap + kAutomation + kGap + kLock};
    int parts = 3, fixed = base + extra[0] + extra[1] + extra[2];
    while (parts > 0 && getWidth() - fixed < kShowMin) {
        --parts;
        fixed -= extra[static_cast<std::size_t>(parts)];
    }
    const int showW = std::max(0, getWidth() - fixed);
    int x = kEdge;
    flip.setBounds(x, y, kFlip, h);
    x += kFlip + kGap;
    show.setBounds(x, y, showW, h);
    x += showW;
    for (auto *b : {&inOut, &sends, &returns, &mixer, &delay})
        b->setVisible(parts >= 1);
    for (auto *b : {&height, &width})
        b->setVisible(parts >= 2);
    for (auto *b : {&automation, &lock})
        b->setVisible(parts >= 3);
    sections_ = sizes_ = {};
    if (parts >= 1) {
        x += kGap;
        sections_ = juce::Rectangle<int>(x, y, sectionsW, h).toFloat();
        x += kPad;
        for (auto *b : {&inOut, &sends, &returns, &mixer, &delay}) {
            b->setBounds(x, y, kCell, h);
            x += kCell;
        }
        x += kPad;
    }
    if (parts >= 2) {
        x += kGap;
        sizes_ = juce::Rectangle<int>(x, y, sizesW, h).toFloat();
        height.setBounds(x + kPad, y, kSize, h);
        width.setBounds(x + kPad + kSize, y, kSize, h);
        x += sizesW;
    }
    if (parts >= 3) {
        x += kGap;
        automation.setBounds(x, y, kAutomation, h);
        x += kAutomation + kGap;
        lock.setBounds(x, y, kLock, h);
    }
}
void NamesBar::paint(juce::Graphics &g) {
    // The ruler's band, and the two groups' boxes (part B: related buttons share one), H and W with
    // the line between them.
    g.fillAll(look::band);
    if (!sections_.isEmpty())
        controls::group(g, sections_);
    if (!sizes_.isEmpty()) {
        controls::group(g, sizes_);
        controls::separator(g, static_cast<float>(height.getRight()), sizes_.getY() + 3.f, sizes_.getHeight() - 6.f);
    }
}
void ArrangementView::setTrackRows(TrackRows rows) {
    trackRows_ = std::move(rows);
    // Rows the last way filled are not this one's: it starts from none.
    tree_.rows().clear();
    applyView();
    // Drawn again on the next frame; nothing of the view changed, so nothing is saved.
    root.mark(DirtySet::All);
}
void ArrangementView::refreshTree() {
    if (trackRows_)
        trackRows_(reader(), tree_.rows());
    else
        tree_.rows().clear();
    geometry.order.build(reader(), tree_.rows());
}
void ArrangementView::applyView() {
    // The grid follows the project's meters (manual 6.10), from this snapshot's.
    geometry.meters = &reader().meters();
    refreshTree();
    leaveOut();
    // A title being renamed stays on its track, wherever the new rows put it.
    if (headers.renaming() != 0)
        headers.beginRename(headers.renaming(), false);
}
void ArrangementView::leaveOut() {
    auto &group = root.state().viewGroup;
    geometry.hidden.clear();
    if (group != 0) {
        if (!tree_.isGroup(group))
            group = 0;
        else
            for (const auto &row : tree_.rows())
                if (!tree_.shows(group, row))
                    geometry.hidden.push_back(row.id);
    }
    // Hide/Show Return Tracks (§18.4): the returns left out, as Live leaves them out of the
    // Arrangement and the Mixer.
    if (root.state().hideReturns)
        for (const auto &t : reader().tracks())
            if (t->kind == "return" && std::find(geometry.hidden.begin(), geometry.hidden.end(), t->id) == geometry.hidden.end())
                geometry.hidden.push_back(t->id);
    // The ★ Reference track, hidden until the project view's [Reference Track] shows it (ADR-0209
    // §4, d11); its strip in the Mix View shows it either way.
    if (const auto reference = root.referenceTrack(); reference != 0 && !root.state().showReference &&
                                                      std::find(geometry.hidden.begin(), geometry.hidden.end(),
                                                                reference) == geometry.hidden.end())
        geometry.hidden.push_back(reference);
    std::sort(geometry.hidden.begin(), geometry.hidden.end());
    // Show says the view chosen: All groups, or the group's name as the tree has it, lit. Its text is
    // set only when it changes, so a frame with the same view allocates nothing.
    auto &show = root.namesBar.show;
    show.setToggleState(group != 0, juce::dontSendNotification);
    const TrackRow *chosen = nullptr;
    for (const auto &row : tree_.rows())
        if (group != 0 && row.id == group)
            chosen = &row;
    if (chosen == nullptr) {
        if (show.getButtonText() != ProjectViewButton::kAllGroups)
            show.setButtonText(ProjectViewButton::kAllGroups);
    } else if (chosen->name.empty() || chosen->name.front() != '#') {
        if (show.getButtonText() != juce::CharPointer_UTF8(chosen->name.c_str()))
            show.setButtonText(juce::String::fromUTF8(chosen->name.c_str()));
    } else
        // A name beginning with # shows the group's number, as its title and the menu show it (Live 12
        // manual 18.2).
        for (const auto row : geometry.rows(reader()))
            if (row.track->id == group) {
                const auto shown = juce::String::fromUTF8(displayName(chosen->name, row.number()).c_str());
                if (show.getButtonText() != shown)
                    show.setButtonText(shown);
            }
    // Live's folded groups: what each holds, at any depth, down to the next row as shallow as
    // it. A group the project view leaves out folds nothing, so a view inside it still shows.
    geometry.foldedAway.clear();
    const auto &folded = root.state().foldedGroups;
    if (folded.empty())
        return;
    std::uint32_t foldDepth = 0;
    bool inFold = false;
    for (const auto row : geometry.rows(reader())) {
        if (inFold && row.place.depth > foldDepth) {
            geometry.foldedAway.push_back(row.track->id);
            continue;
        }
        inFold = row.place.group && folded.count(row.track->id) != 0 && geometry.inView(row.track->id);
        foldDepth = row.place.depth;
    }
    std::sort(geometry.foldedAway.begin(), geometry.foldedAway.end());
}
void ArrangementView::firstShownIfHidden() {
    if (geometry.shown(geometry.selection.track))
        return;
    geometry.selection.track = 0;
    geometry.selection.clip = 0;
    geometry.selection.pickedClips.clear();
    for (const auto row : geometry.rows(reader()))
        if (geometry.shown(row.track->id)) {
            geometry.selection.track = row.track->id;
            break;
        }
}
void ArrangementView::chooseView(std::int64_t group) {
    root.state().viewGroup = group;
    applyView();
    firstShownIfHidden();
    geometry.scrollY = 0;
    changed();
}
void ArrangementView::toggleFold(std::int64_t track) {
    if (!tree_.isGroup(track)) {
        if (reader().findTrack(track) == nullptr)
            return;
        auto &folded = geometry.foldedTracks;
        if (folded.erase(track) == 0)
            folded.insert(track);
        geometry.scrollY = std::min(geometry.scrollY, geometry.maxScroll(reader()));
        changed();
        return;
    }
    auto &folded = root.state().foldedGroups;
    if (folded.erase(track) == 0)
        folded.insert(track);
    leaveOut();
    // Live keeps the selection in sight: a track folded away gives it to its group.
    if (!geometry.shown(geometry.selection.track) && geometry.shown(track)) {
        geometry.selection.track = track;
        geometry.selection.clip = 0;
        geometry.selection.pickedClips.clear();
    }
    firstShownIfHidden();
    changed();
}
void ArrangementView::unfoldAll() {
    root.state().foldedGroups.clear();
    geometry.foldedTracks.clear();
    leaveOut();
    changed();
}
juce::PopupMenu ArrangementView::projectViewMenu() {
    refreshTree();
    juce::PopupMenu menu;
    if (menuLook_ == nullptr)
        menuLook_ = std::make_unique<ProjectViewMenuLook>();
    menu.setLookAndFeel(menuLook_.get());
    const auto chosen = root.state().viewGroup;
    int itemId = 0;
    auto add = [&](std::int64_t group, int depth, const juce::String &name, juce::Colour colour) {
        juce::PopupMenu::Item item(name);
        item.itemID = ++itemId;
        item.isTicked = group == chosen;
        item.customComponent = new ViewMenuItem(name, depth, colour, item.isTicked);
        item.action = [safe = juce::Component::SafePointer<ArrangementView>(this), group] {
            if (safe)
                safe->chooseView(group);
        };
        menu.addItem(std::move(item));
    };
    // Project's folder is grey, as Bitwig's; a group's is its track's colour as the window draws
    // it, unless the project gives one. The groups in the arrangement's order.
    add(0, 0, "Project", look::neutral);
    for (const auto row : geometry.rows(reader()))
        if (row.place.group) {
            // Its name as its title shows it (a # is its number).
            const auto *stored = tree_.find(row.track->id);
            const auto &name = stored != nullptr ? stored->name : row.track->name;
            add(row.track->id, static_cast<int>(row.place.depth) + 1,
                juce::String::fromUTF8(displayName(name, row.number()).c_str()), colourFor(row.position));
        }
    // The ★ Reference track's toggle (ADR-0209 §4, d11), hidden by default.
    menu.addSeparator();
    root.commands().activate(root);
    root.addReferenceCommand(menu, AppCommands::ToggleReferenceTrack);
    return menu;
}
const SnapshotReader &ArrangementView::reader() const { return root.reader(); }
float ArrangementView::waveformZoomScale() const noexcept {
    return std::pow(10.f, static_cast<float>(waveformZoom_.getValue()) / 20.f);
}
int ArrangementView::overviewHeight() const noexcept { return root.state().overview ? kOverviewHeight : 0; }
void ArrangementView::setNamesFirst(bool first, int controls) {
    if (first == namesFirst_ && controls == controlsGap_)
        return;
    namesFirst_ = first;
    controlsGap_ = first ? controls : 0;
    resized();
}
void ArrangementView::resized() {
    const int headerW = headerWidth(std::max(0, getWidth() - controlsGap_));
    const int clipsW = std::max(0, getWidth() - headerW - controlsGap_ - kLeftMargin);
    const int top = topHeight(), above = overviewHeight();
    const int body = std::max(0, getHeight() - top - kTimeRulerHeight);
    // Live's order: the lanes, then the names. Swapped: the names, the track controls' gap,
    // then the lanes.
    const int lanesX = namesFirst_ ? headerW + controlsGap_ + kLeftMargin : kLeftMargin;
    ruler.setBounds(lanesX, above, clipsW, kRulerHeight);
    scrub.setBounds(lanesX, above + kRulerHeight, clipsW, kScrubHeight);
    canvas.setBounds(lanesX, top, clipsW, body);
    geometry.lanesHeight = body;
    headers.setBounds(namesFirst_ ? 0 : kLeftMargin + clipsW, top, headerW, body);
    waveformZoom_.setBounds(headers.getX() + 2, canvas.getBottom(), std::max(0, headerW - 4), kTimeRulerHeight);
    waveformZoom_.setVisible(headerW >= 52);
    frame(true);
}
juce::Colour ArrangementView::colourFor(std::size_t position) const {
    const auto rows = geometry.rows(reader());
    if (position < rows.size())
        if (const auto &own = rows[position].track->color; own && *own >= 0 && *own < kPaletteColours)
            return look::paletteColour(*own);
    return look::trackColour(position);
}
juce::Rectangle<float> ArrangementView::topButton(TopButton which) const noexcept {
    const float hx = static_cast<float>(headers.getX());
    const float top = static_cast<float>(overviewHeight() + kRulerHeight) - 2.f;
    switch (which) {
    case TopButton::Set:
        return {hx + 5, top, 28, 12};
    case TopButton::Previous:
        return {hx + 4, top + 15, 12, 12};
    case TopButton::Next:
        break;
    }
    return {hx + 21, top + 15, 12, 12};
}
void ArrangementView::mouseDown(const juce::MouseEvent &e) {
    // The window's rule for a control that cannot act yet: a click says what it waits for.
    using B = TopButton;
    // Live's locator controls (manual 6.4).
    if (topButton(B::Set).expanded(1.f).contains(e.position)) {
        root.setLocator();
        return;
    }
    if (topButton(B::Previous).expanded(1.f).contains(e.position) || topButton(B::Next).expanded(1.f).contains(e.position)) {
        root.jumpLocator(topButton(B::Next).expanded(1.f).contains(e.position));
        return;
    }
}
void ArrangementView::paint(juce::Graphics &g) {
    // The arrangement's frame is Live's dark band; the rulers and the header column's
    // top sit on it.
    g.fillAll(look::band);
    // Under the bar over the headers: Live's locator buttons ("Set", previous, next).
    // The shared button (language.png part B); none of these tracks the pointer, so each is at rest.
    const auto set = topButton(TopButton::Set);
    controls::button(g, set, controls::ButtonState::Rest, root.setLocatorDeletes() ? "Del" : "Set", look::fontLabel);
    for (int i = 0; i < 2; ++i) {
        const auto c = topButton(i == 0 ? TopButton::Previous : TopButton::Next);
        // Live's round locator buttons, in the shared button's colours at rest.
        g.setColour(look::buttonRest);
        g.fillEllipse(c);
        g.setColour(look::buttonEdge);
        g.drawEllipse(c.reduced(look::hairline / 2), look::hairline);
        // Live's arrows: back to the previous locator, on to the next.
        const float dir = i == 0 ? -1.f : 1.f, cy = c.getCentreY(), cx = c.getCentreX();
        g.setColour(i == 0 ? controls::labelColour(controls::ButtonState::Rest) : look::textDim);
        g.drawLine(cx - 3.f * dir, cy, cx + 3.f * dir, cy, 1.5f);
        g.drawLine(cx + 3.f * dir, cy, cx, cy - 3.f, 1.5f);
        g.drawLine(cx + 3.f * dir, cy, cx, cy + 3.f, 1.5f);
    }
    // Under the lanes: the time ruler, in minutes and seconds (manual 6.1).
    const int clipsW = canvas.getWidth();
    const juce::Rectangle<int> time(canvas.getX(), canvas.getBottom(), clipsW, kTimeRulerHeight);
    g.setColour(look::band);
    g.fillRect(time);
    juce::Graphics::ScopedSaveState clip(g);
    g.reduceClipRegion(time);
    if (const auto *tempo = reader().tempo()) {
        const double from = tempo->ticksToSeconds(geometry.tick(0));
        const double to = tempo->ticksToSeconds(geometry.tick(static_cast<double>(clipsW)));
        const double span = std::max(1e-3, to - from);
        double step = 1;
        for (double s : {1., 5., 15., 30., 60., 300., 900.})
            if (span / s <= 10) {
                step = s;
                break;
            }
        g.setFont(look::font(look::fontBody));
        for (double t = std::ceil(from / step) * step; t <= to; t += step) {
            const int x = canvas.getX() + static_cast<int>(geometry.x(tempo->secondsToTicks(t)));
            g.setColour(look::textOnBand);
            g.fillRect(x, time.getY(), 1, 5);
            const int sec = static_cast<int>(t + 0.5);
            if (x + 3 + 32 <= time.getRight())
                g.drawText(juce::String(sec / 60) + ":" + juce::String(sec % 60).paddedLeft('0', 2),
                           x + 3, time.getY() + 2, 50, 14, juce::Justification::centredLeft);
        }
    }
}
void ArrangementView::frame(bool modelChanged) noexcept {
    const int x = static_cast<int>(std::clamp(geometry.x(root.timelineTick()), -1e7, 1e7));
    // Live draws the playhead only while it plays.
    playhead.setVisible(root.playing() && x >= 0 && x < canvas.getWidth());
    playhead.setBounds(canvas.getX() + x, canvas.getY(), 1, canvas.getHeight());
    playhead.toFront(false);
    // The waveforms follow the document; peaks that have landed since the last frame are drawn.
    if (waveforms != nullptr) {
        if (modelChanged)
            if (const auto *snapshot = reader().get())
                waveforms->follow(*snapshot, root.generation());
        if (const auto landed = waveforms->delivered(); landed != waveformsSeen) {
            waveformsSeen = landed;
            canvas.repaint();
        }
    }
    // Keep the capture visible until the document has finished adding it.
    if (liveTake_) {
        if (!root.isRecording() && !liveTake_->landing)
            liveTake_.reset();
        canvas.repaint();
    }
    const bool hasLiveMidiTake = root.liveMidiTakes && !root.liveMidiTakes().empty();
    if (hasLiveMidiTake || hadLiveMidiTake_)
        canvas.repaint();
    hadLiveMidiTake_ = hasLiveMidiTake;
    if (modelChanged) {
        const auto hasLane = [this](std::int64_t track, std::int64_t lane, bool canAudition) {
            const auto *target = reader().findTrack(track);
            return target && (!canAudition || !target->frozen) &&
                   std::ranges::any_of(target->takeLanes, [lane, canAudition](const auto &candidate) {
                       return candidate.id == lane && (!canAudition || !candidate.muted);
                   });
        };
        std::erase_if(auditionedTakes, [&](const auto &entry) { return !hasLane(entry.first, entry.second, true); });
        if (takeSelection && !hasLane(takeSelection->track, takeSelection->lane, false))
            takeSelection.reset();
        canvas.realiseAccessible();
        overview.repaint();
        canvas.repaint();
        headers.repaint();
        ruler.repaint();
    }
}
void ArrangementView::beginLiveTake(std::int64_t start, std::vector<std::int64_t> tracks) {
    if (tracks.empty())
        liveTake_.reset();
    else
        liveTake_ = LiveTake{start, std::move(tracks), {}, 0, false};
    canvas.repaint();
}
void ArrangementView::setRecordingPreview(std::shared_ptr<const engine::AudioRecordingPreview> preview,
                                           std::uint64_t capturedFrames,
                                           bool landing) noexcept {
    if (liveTake_) {
        if (preview)
            liveTake_->preview = std::move(preview);
        liveTake_->capturedFrames = capturedFrames;
        liveTake_->landing = landing;
    }
}
bool ArrangementView::follow(std::int64_t tick, bool page) noexcept {
    const double width = canvas.getWidth();
    if (width <= 0)
        return false;
    const double at = static_cast<double>(tick) / textproj::kPPQ;
    double left = geometry.left;
    if (!page)
        left = std::max(0., at - width / 2 / geometry.scale);
    else if (const double x = geometry.x(tick); x < 0 || x >= width)
        left = at;
    if (left == geometry.left)
        return false;
    geometry.left = left;
    return true;
}
void ArrangementView::dragTrackHeight(std::int64_t track, int height, bool all) {
    geometry.resizeTrack(track, height, all);
    frame(true);
    if (root.mixer.isVisible()) {
        root.mixer.frame(true);
        root.mixer.repaint();
    }
}
void ArrangementView::changed() {
    auto &state = root.state();
    state.timelineLeft = geometry.left;
    state.pixelsPerQuarter = geometry.scale;
    state.laneHeight = geometry.height;
    state.laneHeights = geometry.heights;
    state.foldedTracks = geometry.foldedTracks;
    state.largeTracks = geometry.largeTracks;
    state.gridSnap = geometry.grid.snap;
    state.gridFixed = geometry.grid.fixed;
    state.gridTriplet = geometry.grid.triplet;
    state.gridNarrow = geometry.grid.narrow;
    state.gridFixedQuarters = geometry.grid.fixedQuarters;
    root.persist();
    root.publishSelection(AdiRootComponent::SelectionOrigin::Arrangement);
    // The mixer's rows follow these lanes: re-place them on the next frame.
    root.mark(DirtySet::Layout);
    frame(true);
}
void ArrangementView::fit(bool selection) {
    geometry.fit(selection ? std::min(geometry.selection.start, geometry.selection.end) : 0,
                 selection ? std::max(geometry.selection.start, geometry.selection.end)
                           : geometry.end(reader()),
                 canvas.getWidth());
    changed();
}
bool ArrangementView::command(int id) {
    switch (id) {
    case AppCommands::TimeZoomIn:
        geometry.zoomAround(1.25, geometry.anchor(root.timelineTick()));
        break;
    case AppCommands::TimeZoomOut:
        geometry.zoomAround(.8, geometry.anchor(root.timelineTick()));
        break;
    case AppCommands::ZoomToSelection:
        fit(geometry.selection.start != geometry.selection.end);
        return true;
    case AppCommands::ZoomBack:
        geometry.back();
        break;
    case AppCommands::OptimizeWidth:
        fit(false);
        return true;
    case AppCommands::OptimizeHeight: {
        int count = 0;
        for (const auto &t : reader().tracks())
            if (geometry.shown(t->id))
                ++count;
        geometry.resizeTrack(0, canvas.getHeight() / std::max(1, count), true);
        geometry.scrollY = 0;
        break;
    }
    case AppCommands::GoStart:
        placeInsertMarker(0);
        return true;
    case AppCommands::InsertMarkerToPlayhead: {
        // Ctrl/Cmd+Shift+Space (§42.16): the insert marker, a point, where the playhead is;
        // Space then plays from there.
        const auto at = root.timelineTick();
        geometry.selection.insert = geometry.selection.start = geometry.selection.end = at;
        geometry.selection.clip = 0;
        geometry.selection.pickedClips.clear();
        geometry.selection.marked = true;
        break;
    }
    case AppCommands::NarrowGrid:
    case AppCommands::WidenGrid: {
        // Narrowing doubles the lines' density, widening halves it (§6.10), within Live's range.
        const bool narrower = id == AppCommands::NarrowGrid;
        if (geometry.grid.fixed)
            geometry.grid.fixedQuarters =
                std::clamp(geometry.grid.fixedQuarters * (narrower ? .5 : 2.), ArrangementGeometry::kFinestGrid,
                           ArrangementGeometry::kWidestGrid);
        else
            geometry.grid.narrow = std::clamp(geometry.grid.narrow + (narrower ? 1 : -1),
                                              -ArrangementGeometry::kGridNarrowest, ArrangementGeometry::kGridNarrowest);
        break;
    }
    case AppCommands::TripletGrid:
        geometry.grid.triplet = !geometry.grid.triplet;
        break;
    case AppCommands::SnapToGrid:
        geometry.grid.snap = !geometry.grid.snap;
        break;
    case AppCommands::FixedGrid:
        // Fixed keeps the spacing on screen when it is switched on, as Live's.
        if (!geometry.grid.fixed)
            geometry.grid.fixedQuarters = geometry.plainGridQuarters();
        geometry.grid.fixed = !geometry.grid.fixed;
        break;
    case AppCommands::LargeTracks:
        // Bitwig's Use Large Track Height: every track at its own height, or at half of it.
        geometry.largeTracks = !geometry.largeTracks;
        break;
    case AppCommands::GrowTracks:
    case AppCommands::ShrinkTracks:
        // Alt/Option + and - (§42.16): the selected track, or, with a time selection, every
        // track with selected content, as Alt+wheel. The step is provisional: Live's is to
        // be measured.
        geometry.wheelHeight(reader(), geometry.selection.track,
                             id == AppCommands::GrowTracks ? kHeightStep : -kHeightStep);
        break;
    default:
        return false;
    }
    changed();
    return true;
}
void ArrangementView::placeInsertMarker(std::int64_t tick, bool fromInsertMarker) {
    root.locate(tick, fromInsertMarker);
    showInsertMarkerAt(tick);
}
void ArrangementView::showInsertMarkerAt(std::int64_t tick) {
    auto &selection = geometry.selection;
    selection.insert = tick;
    selection.clearMaterial();
    selection.marked = true;
    follow(tick, true); // Reveal an explicit jump even when playback Follow is off.
    changed();
}
void ArrangementView::locate() { root.locate(geometry.selection.insert, true); }
void ArrangementView::select(int x, int y, bool snapToGrid, SelectionPurpose purpose) {
    if (purpose == SelectionPurpose::Edit)
        canvas.clearAutomationSelection();
    if (const auto *t = geometry.trackAt(reader(), y)) {
        const auto at = geometry.tick(x);
        // The clip under the mouse is hit where it is; the insert marker snaps (§6.10).
        const auto *c = purpose == SelectionPurpose::InsertMarker ? nullptr : geometry.hit(*t, at);
        const auto tick = snapToGrid ? geometry.snap(at, false) : at;
        if (purpose == SelectionPurpose::ContextMenu) {
            auto &sel = geometry.selection;
            if (c != nullptr && geometry.clipSelected(c->id)) {
                if (sel.pickedClips.empty()) {
                    // Split from a single clip's menu uses the point where it was opened.
                    sel.insert = tick;
                    sel.marked = true;
                    changed();
                }
                return;
            }
            const auto [start, end] = std::minmax(sel.start, sel.end);
            if (sel.clip == 0 && sel.pickedClips.empty() && at >= start && at < end &&
                geometry.inSelection(reader(), t->id))
                return;
            canvas.clearAutomationSelection();
        }
        geometry.selection.track = t->id;
        geometry.selection.toTrack = 0;
        geometry.selection.picked.clear();
        geometry.selection.clip = c ? c->id : 0;
        geometry.selection.pickedClips.clear();
        geometry.selection.insert = tick;
        geometry.selection.marked = true;
        geometry.selection.start = c ? c->posTicks : tick;
        geometry.selection.end = c ? c->posTicks + c->lengthTicks : tick;
        if (purpose != SelectionPurpose::ContextMenu)
            locate();
        changed();
    }
}
bool ArrangementView::snaps(const juce::ModifierKeys &mods) const noexcept {
    // Alt (Cmd on the Mac) bypasses snapping; with the grid off, it snaps while held.
#if JUCE_MAC
    const bool bypass = mods.isCommandDown();
#else
    const bool bypass = mods.isAltDown();
#endif
    return geometry.grid.snap != bypass;
}
void ArrangementView::selectClip(std::int64_t track, std::int64_t clip) {
    canvas.clearAutomationSelection();
    const auto *t = reader().findTrack(track);
    if (t == nullptr)
        return;
    for (const auto &c : t->clips)
        if (c->id == clip) {
            geometry.selection.track = track;
            geometry.selection.toTrack = 0;
            geometry.selection.picked.clear();
            geometry.selection.clip = clip;
            geometry.selection.pickedClips.clear();
            geometry.selection.start = c->posTicks;
            geometry.selection.end = c->posTicks + c->lengthTicks;
            changed();
            return;
        }
}
bool ArrangementView::selectAll() {
    canvas.clearAutomationSelection();
    std::int64_t first = 0, last = 0, from = 0, to = 0, only = 0, clips = 0;
    for (const auto row : geometry.rows(reader())) {
        const auto *t = row.track;
        if (!geometry.shown(t->id) || t->clips.empty())
            continue;
        for (const auto &c : t->clips) {
            from = clips == 0 ? c->posTicks : std::min(from, c->posTicks);
            to = clips == 0 ? c->posTicks + c->lengthTicks : std::max(to, c->posTicks + c->lengthTicks);
            only = c->id;
            ++clips;
        }
        if (first == 0)
            first = t->id;
        last = t->id;
    }
    if (clips == 0) {
        root.say("There is nothing to select.");
        return false;
    }
    auto &sel = geometry.selection;
    sel.track = first;
    sel.toTrack = last == first ? 0 : last;
    sel.clip = clips == 1 ? only : 0;
    sel.pickedClips.clear();
    sel.start = from;
    sel.end = to;
    sel.insert = from;
    sel.marked = true;
    locate();
    changed();
    return true;
}
bool ArrangementView::selectLoop() {
    canvas.clearAutomationSelection();
    std::int64_t first = 0, last = 0;
    for (const auto row : geometry.rows(reader())) {
        if (!geometry.shown(row.track->id))
            continue;
        if (first == 0)
            first = row.track->id;
        last = row.track->id;
    }
    // From the CURRENT cell (ADR-0201): a brace just moved is there before the next frame presents it.
    const auto current = root.current();
    const auto loop = loopOf(current ? current->transport : reader().transport());
    auto &sel = geometry.selection;
    sel.track = first;
    sel.toTrack = last == first ? 0 : last;
    sel.clip = 0;
    sel.pickedClips.clear();
    sel.start = loop.start;
    sel.end = loop.end;
    sel.insert = loop.start;
    sel.marked = true;
    locate();
    changed();
    return true;
}
namespace {
bool supportsTakeLanes(const engine::TrackNode *track) {
    return track && (track->kind == "audio" || track->kind == "midi" || track->kind == "instrument");
}
}
bool ArrangementView::toggleTakeLanes(std::int64_t track) {
    const auto *target = reader().findTrack(track);
    if (!supportsTakeLanes(target))
        return false;
    if (target->takeLanes.empty() && !geometry.shownTakeLanes.contains(track)) {
        root.say("This track has no take lanes yet. Insert Take Lane adds one.");
        return false;
    }
    if (geometry.shownTakeLanes.erase(track) == 0) {
        geometry.shownTakeLanes.insert(track);
        geometry.foldedTracks.erase(track);
    } else {
        if (takeSelection && takeSelection->track == track)
            takeSelection.reset();
        const auto audition = auditionedTakes.find(track);
        if (audition != auditionedTakes.end())
            toggleTakeAudition(track, audition->second);
    }
    geometry.scrollY = std::min(geometry.scrollY, geometry.maxScroll(reader()));
    changed();
    return true;
}
bool ArrangementView::insertTakeLane(std::int64_t track) {
    const auto *target = reader().findTrack(track);
    if (!supportsTakeLanes(target) || target->frozen)
        return false;
    try {
        auto &store = root.editStore();
        auto request = createTakeLane(store, RowIds(store.db()).next("lanes"), track, "Take");
        request.label = "Insert Take Lane";
        if (!root.submitEdits({std::move(request)}))
            return false;
        geometry.shownTakeLanes.insert(track);
        geometry.foldedTracks.erase(track);
        changed();
        return true;
    } catch (const std::exception &error) {
        root.say(error.what());
        return false;
    }
}
bool ArrangementView::takeAuditioned(std::int64_t track, std::int64_t lane) const noexcept {
    const auto found = auditionedTakes.find(track);
    return found != auditionedTakes.end() && found->second == lane;
}
bool ArrangementView::toggleTakeAudition(std::int64_t track, std::int64_t lane) {
    const auto next = takeAuditioned(track, lane) ? 0 : lane;
    if (!root.setTakeLaneAudition || !root.setTakeLaneAudition(track, next)) {
        root.say("The take lane could not be auditioned.");
        return false;
    }
    if (next == 0)
        auditionedTakes.erase(track);
    else
        auditionedTakes[track] = next;
    frame(true);
    return true;
}
bool ArrangementView::copyTakeSelectionToMain() {
    if (!takeSelection)
        return false;
    const auto selected = *takeSelection;
    if (selected.start == selected.end)
        return false;
    if (!root.commitEdit(clip_edit::prepareTakeToMain(root.editStore(), selected.track, selected.lane,
            {0, std::min(selected.start, selected.end)}, {0, std::max(selected.start, selected.end)}, root.editRate())))
        return false;
    if (const auto active = auditionedTakes.find(selected.track); active != auditionedTakes.end())
        toggleTakeAudition(selected.track, active->second);
    return true;
}
juce::PopupMenu ArrangementView::headerMenu(std::int64_t contextTrack) {
    auto &commands = root.commands();
    commands.activate(root);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
    menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
    const auto track = contextTrack != 0 ? contextTrack : geometry.selection.track;
    if (track == 0)
        return menu;
    const auto addTrackAction = [&](int command, const char *name, bool enabled, std::function<void()> action) {
        juce::PopupMenu::Item item(name);
        const auto keys = commands.manager.getKeyMappings()->getKeyPressesAssignedToCommand(command);
        if (!keys.isEmpty())
            item.shortcutKeyDescription = keys[0].getTextDescriptionWithIcons();
        item.isEnabled = enabled;
        item.action = std::move(action);
        menu.addItem(std::move(item));
    };
    menu.addSeparator();
    if (contextTrack == 0) {
        menu.addCommandItem(&commands.manager, AppCommands::RenameTrack);
    } else {
        // The chosen mixer channel can be outside the visible Arrangement rows or be picked
        // alongside the primary track. Reveal its existing editor without replacing that choice.
        addTrackAction(AppCommands::RenameTrack, "Rename", true, [safe = SafePointer<ArrangementView>(this), track] {
            if (!safe)
                return;
            const auto current = safe->root.current();
            const auto *target = current ? current->findTrack(track) : nullptr;
            if (!target) {
                safe->root.say("That track is gone.");
                return;
            }
            // The Mixer includes folded groups' children. Open only this track's ancestors.
            auto &folded = safe->root.state().foldedGroups;
            bool unfolded = false;
            for (std::size_t depth = 0; target && target->parent != 0 && depth < current->tracks.size(); ++depth) {
                unfolded |= folded.erase(target->parent) != 0;
                target = current->findTrack(target->parent);
            }
            if (unfolded)
                safe->applyView();
            if (safe->geometry.reveal(safe->reader(), track) || unfolded)
                safe->changed();
            if (safe && !safe->headers.beginRename(track, false))
                safe->root.say("That track is gone.");
        });
    }
    for (const auto id : {AppCommands::Cut, AppCommands::Copy, AppCommands::Paste, AppCommands::Duplicate})
        menu.addCommandItem(&commands.manager, id);
    const auto *selected = reader().findTrack(geometry.selection.track);
    if (contextTrack != 0 && selected && selected->kind == "master") {
        juce::PopupMenu::Item remove("Delete Track");
        remove.isEnabled = false;
        menu.addItem(std::move(remove));
    } else {
        menu.addCommandItem(&commands.manager, AppCommands::DeleteTrack);
    }
    root.addTrackAutomationDeleteItem(menu, track);
    root.addTrackRenderItems(menu, track);
    if (const auto *target = reader().findTrack(track); supportsTakeLanes(target)) {
        const bool lanesShown = geometry.shownTakeLanes.contains(track);
        menu.addItem(lanesShown ? "Hide Take Lanes" : "Show Take Lanes", lanesShown || !target->takeLanes.empty(), false,
                     [safe = SafePointer<ArrangementView>(this), track] { if (safe) safe->toggleTakeLanes(track); });
        menu.addItem("Insert Take Lane", !target->frozen, false,
                     [safe = SafePointer<ArrangementView>(this), track] { if (safe) safe->insertTakeLane(track); });
    }
    // Live's lane chooser (DF-W37; manual 25.2): the track's mixer, then each device's parameters; the
    // one chosen shows on the track's lane in Automation Mode.
    {
        const auto shown = root.automationTarget(track);
        juce::PopupMenu show;
        std::map<std::string, juce::PopupMenu> byDevice;
        std::vector<std::string> order;
        const auto publication = root.devices.publication ? root.devices.publication() : nullptr;
        for (const auto &t : root.automationChoices(track)) {
            const auto key = t.ownerKind == "device" ? t.device + "#" + std::to_string(t.ownerId) : std::string("Mixer");
            if (!byDevice.count(key))
                order.push_back(key);
            auto label = juce::String::fromUTF8(t.paramName.c_str());
            if (t.lastTouched) label += " (last touched)";
            byDevice[key].addItem(label, true, t == shown,
                                  [safe = SafePointer<ArrangementView>(this), track, t,
                                   touchSource = t.lastTouched ? publication : nullptr] {
                                      if (!safe) return;
                                      if (touchSource) {
                                          const auto captured = std::find_if(touchSource->begin(), touchSource->end(),
                                              [&](const auto &d) { return d.id == t.ownerId; });
                                          if (captured == touchSource->end() ||
                                              !safe->root.devices.addLastTouchedParameter(*captured))
                                              return;
                                      }
                                      if (safe) safe->root.showAutomation(track, t);
                                  });
        }
        for (const auto &key : order)
            show.addSubMenu(juce::String::fromUTF8(key.substr(0, key.rfind('#') == std::string::npos ? key.size() : key.rfind('#')).c_str()),
                            byDevice[key]);
        menu.addSeparator();
        menu.addSubMenu("Show Automation", show, show.getNumItems() > 0);
    }
    if (root.automationMode()) {
        const juce::Component::SafePointer<ArrangementView> safe(this);
        const auto laneTarget = root.automationTarget(track);
        menu.addItem("Add Automation Lane", root.automationOverlayShown(track), false, [safe, track, laneTarget] {
            if (safe)
                safe->root.addAutomationLane(track, laneTarget);
        });
        menu.addItem("Add Lane for Each Automated Envelope", [safe, track] {
            if (safe)
                safe->root.addAutomatedLanes(track);
        });
    }
    menu.addSeparator();
    menu.addCommandItem(&commands.manager, AppCommands::GroupTracks);
    const auto *target = reader().findTrack(track);
    const bool isGroup = target && target->kind == "group";
    if (contextTrack == 0)
        menu.addCommandItem(&commands.manager, AppCommands::UngroupTracks);
    else
        addTrackAction(AppCommands::UngroupTracks, "Ungroup Tracks", isGroup, [safe = SafePointer<ArrangementView>(this), track] {
            if (safe)
                safe->root.groupTracks(true, track);
        });
    // Live's colour chooser at the menu's foot (Live 12 manual 2.3.2); on a group, the command that
    // gives what it holds its colour (manual 18.3).
    menu.addSeparator();
    if (isGroup) {
        menu.addItem(kAssignGroupedClipColour, [safe = SafePointer<ArrangementView>(this), track] {
            if (safe)
                safe->assignGroupColour(track);
        });
        // The way into a group for a return (ADR-0247 d2): a new return inside it, after what it
        // holds, lettered with the group's initial ("DA"). The Create menu's keeps Live's foot.
        menu.addItem("Insert Return Track in Group", [safe = SafePointer<ArrangementView>(this), track] {
            if (safe)
                safe->root.insertReturnTrack(track);
        });
    } else if (target && (!target->clips.empty() || !target->takeClips.empty())) {
        menu.addItem(kAssignClipColour, [safe = SafePointer<ArrangementView>(this), track] {
            if (safe)
                safe->assignColourToClips(track, false);
        });
    }
    menu.addCustomItem(1, std::make_unique<ColourChooser>([safe = SafePointer<ArrangementView>(this)](int colour) {
        if (safe != nullptr)
            safe->recolour(colour);
    }, colourIndex(track)), nullptr, "Colour");
    return menu;
}
void ArrangementView::contextMenu(juce::Point<int> where, bool header, const engine::AutomationLaneNode *lane,
                                  const std::function<void(juce::PopupMenu &)> &configure) {
    auto menu = contextMenuItems(header, lane);
    if (configure)
        configure(menu);
    presentContextMenu(where, header, lane, std::move(menu));
}
void ArrangementView::presentContextMenu(juce::Point<int> where, bool header,
                                        const engine::AutomationLaneNode *lane, juce::PopupMenu menu) {
    if (onContextMenu)
        onContextMenu(where, header, lane, menu);
    else
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea({where.x, where.y, 1, 1}));
}
const engine::AutomationLaneNode *ArrangementView::automationLaneAt(int y) const {
    if (!root.automationMode())
        return nullptr;
    const auto *current = root.current().get(); // the root holds it: the pointer outlives this call
    if (current == nullptr)
        return nullptr;
    int top = 0;
    const auto row = geometry.rowAt(reader(), y, &top);
    const auto rows = geometry.rows(reader());
    if (row >= rows.size())
        return nullptr;
    const auto id = rows[row].track->id;
    if (geometry.foldedTracks.contains(id))
        return nullptr;
    top += geometry.heightFor(id);
    if (y < top)
        return root.automationOverlayShown(id) ? envelopeOf(*current, root.automationTarget(id)).lane : nullptr;
    if (root.automationLanesExpanded(id))
        for (const auto &lane : root.additionalAutomationLanes(id)) {
            top += lane.height;
            if (y < top)
                return envelopeOf(*current, lane.target).lane;
        }
    return nullptr;
}
juce::PopupMenu ArrangementView::contextMenuItems(bool header, const engine::AutomationLaneNode *lane) {
    auto menu = header ? headerMenu() : juce::PopupMenu();
    if (!header) {
        auto &commands = root.commands();
        commands.activate(root);
        using Command = AppCommands;
        const auto addActive = [&commands, &menu](Command::Id id) {
            juce::ApplicationCommandInfo info(id);
            commands.getCommandInfo(id, info);
            if ((info.flags & juce::ApplicationCommandInfo::isDisabled) == 0)
                menu.addCommandItem(&commands.manager, id);
        };
        const auto &sel = geometry.selection;
        const bool clips = sel.clip != 0 || !sel.pickedClips.empty();
        const bool time = !clips && sel.start != sel.end;
        if (canvas.hasAutomationSelection()) {
            for (const auto id : {Command::Cut, Command::Copy, Command::Paste, Command::Duplicate, Command::DeleteTrack})
                menu.addCommandItem(&commands.manager, id);
            menu.addSeparator();
        } else if (clips || time) {
            for (const auto id : {Command::Cut, Command::Copy, Command::Paste, Command::Duplicate})
                addActive(id);
            juce::Component::SafePointer<ArrangementView> safe(this);
            menu.addItem("Delete", [safe] {
                if (safe != nullptr)
                    safe->canvas.editMaterialSelection(false);
            });
            if (const auto current = root.current()) {
                if (clips) {
                    const auto ids = geometry.selectedClips();
                    const auto activation = deactivateClips(current->tracks, ids);
                    if (!activation.ops.empty())
                        menu.addItem(activation.ops.front().payload.value("muted", false) ? "Deactivate" : "Activate", [safe] {
                            if (safe != nullptr)
                                safe->canvas.editMaterialSelection(true);
                        });
                    if (!deleteFades(current->tracks, ids).ops.empty())
                        addActive(Command::DeleteFades);
                    auto colour = colourIndex(sel.track);
                    if (const auto *track = reader().findTrack(sel.track))
                        for (const auto &clip : track->clips)
                            if (clip->id == sel.clip)
                                colour = clip->color.value_or(colour);
                    menu.addCustomItem(1, std::make_unique<ColourChooser>([safe, ids](int index) {
                        if (safe == nullptr)
                            return;
                        std::vector<OpRequest> ops;
                        for (const auto id : ids) {
                            const auto details = engine::readClipDetails(safe->root.editStore(), id, false);
                            if (!details.clip) {
                                safe->root.say(details.error);
                                return;
                            }
                            auto edit = recolourClip(*details.clip, index);
                            for (auto &op : edit.ops)
                                ops.push_back(std::move(op));
                        }
                        if (!ops.empty() && safe->root.submitEdits(std::move(ops)))
                            safe->changed();
                    }, colour), nullptr, "Clip Colour");
                } else if (!createFades(current->tracks, geometry.selectedTracks(reader()), sel.start, sel.end).ops.empty()) {
                    addActive(Command::CreateFade);
                }
            }
            addActive(Command::ConsolidateMidi);
            addActive(Command::LoopSelection);
            addActive(Command::ReverseAudio);
            menu.addSeparator();
            if (clips) {
                addActive(Command::Split);
                addActive(Command::RenameTrack);
                if (const auto snapshot = root.current(); snapshot && root.locateClipMedia) {
                    const auto clipId = sel.clip;
                    for (const auto &track : snapshot->tracks) {
                        if (!track) continue;
                        const auto found = std::find_if(track->clips.begin(), track->clips.end(), [clipId](const auto &clip) {
                            return clip->id == clipId;
                        });
                        if (found == track->clips.end()) continue;
                        const auto &clip = **found;
                        if (clip.audio && root.clipMediaOffline(clip) && clip.audio->mediaInfo) {
                            menu.addItem("Locate Missing Sample...", [safe, clipId] {
                                if (safe && safe->root.locateClipMedia) safe->root.locateClipMedia(clipId);
                            });
                        }
                        break;
                    }
                }
            } else {
                for (const auto id : {Command::CutTime, Command::CopyTime, Command::PasteTime, Command::DuplicateTime,
                                      Command::DeleteTime, Command::InsertSilence})
                    addActive(id);
            }
            menu.addSeparator();
        } else if (!commands.clipClipboard().empty()) {
            addActive(Command::Paste);
            menu.addSeparator();
        }
        menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
        menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
        // The lanes' menu sets the grid as well (§6.10).
        menu.addSeparator();
        addGridItems(menu);
        root.addReenableAutomation(menu, lane);
        if (lane) {
            AutomationTarget target;
            target.ownerKind = lane->ownerKind;
            target.ownerId = lane->ownerId;
            target.paramRef = lane->paramRef;
            root.addAutomationDeleteItem(menu, target);
        }
    }
    return menu;
}
std::int64_t ArrangementView::colourIndex(std::int64_t track) const {
    const auto rows = geometry.rows(reader());
    const auto at = rows.find(track);
    if (at == rows.size())
        return -1;
    if (const auto &own = rows[at].track->color; own && *own >= 0 && *own < kPaletteColours)
        return *own;
    // The colour its place gives it (look::trackColour): the palette's measured row, in turn.
    return static_cast<std::int64_t>(at % 14);
}
bool ArrangementView::recolour(std::int64_t index) {
    const auto current = root.current();
    if (!current)
        return false;
    auto edit = colourTracks(current->tracks, geometry.selectedTracks(reader()), index, "Recolour Track");
    if (!edit.refusal.empty()) {
        root.say(edit.refusal);
        return false;
    }
    return edit.ops.empty() || root.submitEdits(std::move(edit.ops));
}
bool ArrangementView::assignGroupColour(std::int64_t groupId) {
    return assignColourToClips(groupId != 0 ? groupId : geometry.selection.track, true);
}
bool ArrangementView::assignColourToClips(std::int64_t trackId, bool grouped) {
    const auto current = root.current();
    const auto *source = current ? current->findTrack(trackId) : nullptr;
    if (!source || grouped != (source->kind == "group")) {
        root.say(grouped ? "Select a group track to give what it holds its colour." : "That track is gone.");
        return false;
    }
    // Read the current palette index when the menu action runs (ADR-0201).
    const auto &own = source->color;
    const auto index = own && *own >= 0 && *own < kPaletteColours ? *own : colourIndex(trackId);
    if (index < 0)
        return false;
    auto tracks = grouped ? heldBy(current->tracks, trackId) : std::vector<std::int64_t>{};
    const auto *label = grouped ? kAssignGroupedClipColour : kAssignClipColour;
    auto edit = grouped ? colourTracks(current->tracks, tracks, index, label) : TrackEdit{};
    tracks.push_back(trackId);
    for (const auto id : tracks) {
        const auto *track = current->findTrack(id);
        if (!track)
            continue;
        for (const auto *clips : {&track->clips, &track->takeClips})
            for (const auto &clip : *clips)
                if (clip && clip->color != index)
                    edit.ops.push_back(track_edits_detail::op("clip.setColor", {{"id", clip->id}, {"color", index}}, label));
    }
    return root.submitEdits(std::move(edit.ops));
}
void ArrangementView::addGridItems(juce::PopupMenu &menu) {
    using Geometry = ArrangementGeometry;
    const auto &grid = geometry.grid;
    // The menu answers after this returns: a choice made once the window has gone does nothing.
    juce::Component::SafePointer<ArrangementView> safe(this);
    auto choose = [safe](bool fixed, int narrow, double quarters) {
        return [safe, fixed, narrow, quarters] {
            if (safe == nullptr)
                return;
            auto &chosen = safe->geometry.grid;
            chosen.fixed = fixed;
            if (fixed)
                chosen.fixedQuarters = quarters;
            else
                chosen.narrow = narrow;
            safe->changed();
        };
    };
    juce::PopupMenu adaptive, fixed;
    for (std::size_t i = 0; i < Geometry::kAdaptiveNames.size(); ++i) {
        const int narrow = static_cast<int>(i) - Geometry::kGridNarrowest;
        adaptive.addItem(Geometry::kAdaptiveNames[i], true, !grid.fixed && grid.narrow == narrow,
                         choose(false, narrow, 0));
    }
    for (double quarters = Geometry::kWidestGrid; quarters >= Geometry::kFinestGrid; quarters /= 2)
        fixed.addItem(gridLabel(quarters, false), true, grid.fixed && grid.fixedQuarters == quarters,
                      choose(true, 0, quarters));
    menu.addSubMenu("Adaptive Grid", adaptive);
    menu.addSubMenu("Fixed Grid", fixed);
    menu.addCommandItem(&root.commands().manager, AppCommands::TripletGrid);
    menu.addCommandItem(&root.commands().manager, AppCommands::SnapToGrid);
}
int ArrangementView::edgeScrollStep(int position, int length) noexcept {
    // How far past the edge zone's inner line, as a step: a few points at its rim, up to a track
    // a step well past it.
    const int depth = position < kScrollEdge ? position - kScrollEdge
                    : position > length - kScrollEdge ? position - (length - kScrollEdge) : 0;
    return depth == 0 ? 0 : (depth > 0 ? 1 : -1) * std::min(48, 2 + std::abs(depth) / 2);
}
bool ArrangementView::edgeScroll(int x, int y, bool sideways, bool upAndDown) {
    bool moved = false;
    if (upAndDown) {
        // The tracks scroll above the foot: its top is their bottom.
        const int bottom = geometry.footTop(reader());
        const int before = geometry.scrollY;
        geometry.scrollY = std::clamp(before + edgeScrollStep(y, bottom), 0, geometry.maxScroll(reader()));
        moved = geometry.scrollY != before;
    }
    if (sideways) {
        const int width = canvas.getWidth();
        const double before = geometry.left;
        geometry.pan(edgeScrollStep(x, width));
        if (geometry.left != before) {
            moved = true;
            followPaused = true; // a sideways scroll pauses Follow (§6.2)
        }
    }
    // The mixer's rows follow the lanes: re-placed on the next frame.
    if (moved)
        root.mark(DirtySet::Layout);
    return moved;
}
void ArrangementView::wheel(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (e.mods.isCommandDown()) {
        // Live zooms around the selection (manual 6.2, ADR-0203 d7); around the
        // cursor only when Zoom on Selection is switched off.
        const double factor = std::exp(w.deltaY * 2);
        if (root.commands().zoomOnSelection())
            geometry.zoomAround(factor, geometry.anchor(root.timelineTick()));
        else
            geometry.zoom(factor, e.position.x);
    } else if (e.mods.isAltDown()) {
        if (const auto *track = geometry.trackAt(reader(), e.y))
            geometry.wheelHeight(reader(), track->id, static_cast<int>(w.deltaY * 100));
    } else if (e.mods.isShiftDown()) {
        geometry.pan(-w.deltaY * 300);
        followPaused = true; // a sideways scroll pauses Follow (§6.2)
    } else {
        geometry.pan(-w.deltaX * 300);
        if (w.deltaX != 0)
            followPaused = true;
        geometry.scrollY =
            std::clamp(geometry.scrollY - static_cast<int>(w.deltaY * 300), 0, geometry.maxScroll(reader()));
    }
    changed();
}
void TimelineRuler::paint(juce::Graphics &g) {
    // The beat-time ruler draws plain labels above thin ticks. A signature change starts a new
    // bar where it falls, and the beats follow its signature (Live 12 manual 6.1).
    g.fillAll(look::band);
    const auto &geo = view.geometry;
    const auto &meters = view.reader().meters();
    const auto from = geo.tick(0.), to = geo.tick(static_cast<double>(getWidth()));
    // The spacing from the signature at the left edge: every beat, or every few beats, while they
    // are 56 points apart; else every bar, every second, every fourth and so on.
    const auto &first = meterAt(from, meters);
    const double beatPx = 4. / std::max<double>(1, first.denominator) * geo.scale;
    constexpr auto labelSpacing = ArrangementGeometry::kRulerLabelSpacing;
    std::int64_t beatStep = 1;
    while (beatStep < first.numerator && static_cast<double>(beatStep) * beatPx < labelSpacing)
        beatStep *= 2;
    const bool beats = beatStep < first.numerator;
    std::int64_t barStep = 1;
    while (static_cast<double>(barStep) * beatPx * first.numerator < labelSpacing)
        barStep *= 2;
    const int foot = getHeight();
    const auto ink = look::rulerLabel.interpolatedWith(look::band, .5f);
    g.setFont(look::font(look::fontField, true));
    const auto tickMark = [&](int x, int height) {
        g.setColour(ink);
        g.fillRect(x, foot - height, 1, height);
    };
    const auto label = [&](int x, const juce::String &text) {
        g.setColour(look::textOnBand);
        g.drawText(text, x + 4, 1, 60, 16, juce::Justification::centredLeft);
    };
    // From a bar before the left edge, so a bar's later beats still show.
    forEachBar(from - first.barTicks(), to, meters, [&](std::int64_t tick, std::int64_t bar) {
        const int x = static_cast<int>(geo.x(tick));
        if (beats || (bar - 1) % barStep == 0) {
            label(x, juce::String(bar));
            tickMark(x, 12);
        } else if (beatPx * first.numerator >= 8)
            tickMark(x, 5);
        if (!beats)
            return;
        const auto &m = meterAt(tick, meters);
        const auto beatTicks = textproj::kWhole / std::max<std::int64_t>(1, m.denominator);
        // A change part-way through a bar cuts it short (Live's fragmentary bar, manual 6.5): its
        // beats stop where the next signature begins. Live crosshatches the fragment; ADI does not yet.
        auto until = std::numeric_limits<std::int64_t>::max();
        for (const auto &next : meters)
            if (next.start_ticks > tick) {
                until = next.start_ticks;
                break;
            }
        for (std::int64_t b = beatStep; b < m.numerator && tick + b * beatTicks < until; b += beatStep) {
            const int beatX = static_cast<int>(geo.x(tick + b * beatTicks));
            label(beatX, juce::String(bar) + "." + juce::String(b + 1));
            tickMark(beatX, 5);
        }
    });
    geo.forEachRulerSubdivision(from, to, meters, [&](std::int64_t tick, const std::optional<BarPosition> &at) {
        const int x = static_cast<int>(geo.x(tick));
        if (at)
            label(x, juce::String(at->bar) + "." + juce::String(at->beat) + "." + juce::String(at->sixteenth));
        tickMark(x, at ? 5 : 3);
    });
    paintTempos(g);
}
void TimelineRuler::paintTempos(juce::Graphics &g) const {
    const auto *tempo = view.reader().tempo();
    if (!tempo || tempo->events.size() < 2 || getWidth() <= 0) return;
    const auto &geometry = view.geometry;
    const auto from = geometry.tick(0.), to = geometry.tick(getWidth());
    const auto &events = tempo->events;
    auto event = std::upper_bound(events.begin(), events.end(), from,
                                  [](auto tick, const auto &e) { return tick < e.posTicks; });
    if (event != events.begin()) --event;
    const int top = getHeight() - kTempoRowHeight;
    g.setFont(look::font(look::fontCaption));
    for (; event != events.end() && event->posTicks <= to; ++event) {
        const int x = juce::roundToInt(std::clamp(geometry.x(event->posTicks), 0., static_cast<double>(getWidth())));
        const auto next = std::next(event);
        const int right = next != events.end() && next->posTicks <= to
            ? juce::roundToInt(geometry.x(next->posTicks)) : getWidth();
        g.setColour(look::accent);
        if (event->posTicks >= from) g.fillRect(x, top + 2, 1, 8);
        g.drawText(juce::String(event->bpm, std::round(event->bpm) == event->bpm ? 0 : 2) + " BPM",
                   x + 4, top, std::clamp(right - x - 4, 0, kTempoLabelWidth), kTempoRowHeight,
                   juce::Justification::centredLeft, false);
    }
}
std::int64_t TimelineRuler::tempoPosition(const juce::MouseEvent &e) const {
    const auto &geometry = view.geometry;
    if (e.getPosition().y >= getHeight() - kTempoRowHeight)
        if (const auto *tempo = view.reader().tempo())
            for (const auto &event : tempo->events)
                if (std::abs(geometry.x(event.posTicks) - e.position.x) <= 3.) return event.posTicks;
    const auto at = geometry.tick(e.position.x);
    return std::max<std::int64_t>(0, view.snaps(e.mods) ? geometry.snap(at, false) : at);
}
void TimelineRuler::beginTempoEdit(std::int64_t at) {
    const auto *tempo = view.reader().tempo();
    if (!tempo) return;
    tempoEditing_ = at;
    const int width = std::min(kTempoLabelWidth, getWidth());
    const int x = juce::roundToInt(std::clamp(view.geometry.x(at), 0., static_cast<double>(getWidth() - width)));
    tempoEditor.setBounds(x, 0, width, getHeight());
    tempoEditor.setText(juce::String(tempo->bpmAt(at), 2), false);
    tempoEditor.setVisible(true);
    tempoEditor.selectAll();
    focusIfShowing(tempoEditor);
}
void TimelineRuler::mouseDown(const juce::MouseEvent &e) {
    view.canvas.clearAutomationSelection();
    tempoMenuPressed_ = e.mods.isPopupMenu();
    if (tempoMenuPressed_) {
        const auto *tempo = view.reader().tempo();
        if (!tempo) return;
        const auto at = tempoPosition(e);
        const bool exists = std::ranges::any_of(tempo->events, [at](const auto &event) { return event.posTicks == at; });
        juce::PopupMenu menu;
        menu.addItem(exists ? "Edit Tempo Change" : "Insert Tempo Change", [safe = SafePointer<TimelineRuler>(this), at] {
            if (safe) safe->beginTempoEdit(at);
        });
        if (onTempoMenu)
            onTempoMenu(menu);
        else
            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
        return;
    }
    last = e.position;
    view.followPaused = true; // a click on the beat-time ruler pauses Follow (§6.2)
}
void TimelineRuler::mouseDrag(const juce::MouseEvent &e) {
    if (tempoMenuPressed_) return;
    auto d = e.position - last;
    view.geometry.pan(-d.x);
    view.geometry.zoom(std::exp(-d.y * .02), e.position.x);
    last = e.position;
    view.changed();
}
void TimelineRuler::mouseDoubleClick(const juce::MouseEvent &e) {
    if (!e.mods.isPopupMenu()) view.fit(view.geometry.selection.start != view.geometry.selection.end);
}
int TrackHeaderList::nameLeft(std::uint32_t depth) const noexcept {
    // One band per group the track sits in, as many as leave the name some room.
    const auto room = static_cast<std::uint32_t>(std::max(0, getWidth() - 60) / ArrangementView::kGroupBand);
    return 2 + static_cast<int>(std::min({depth, room, kMaxBands})) * ArrangementView::kGroupBand;
}
juce::Rectangle<float> TrackHeaderList::foldSwitch(std::uint32_t depth, int top) const noexcept {
    return {static_cast<float>(nameLeft(depth) + 3), static_cast<float>(top) + 3, 13, 13};
}
namespace {
bool sameAutomationDevice(const AutomationTarget &a, const AutomationTarget &b) noexcept {
    const bool device = a.ownerKind == "device";
    return device == (b.ownerKind == "device") && (!device || a.ownerId == b.ownerId);
}
juce::String automationChooserLabel(const AutomationTarget &target, bool device) {
    if (device)
        return target.ownerKind == "device" ? juce::String::fromUTF8(target.device.c_str()) : juce::String("Mixer");
    if (target.ownerKind == "track") {
        if (target.paramRef == "volume")
            return "Track Volume";
        if (target.paramRef == "pan")
            return "Track Panning";
    }
    return juce::String::fromUTF8(target.paramName.c_str());
}
juce::String automationLaneLabel(const AutomationTarget &target) {
    const auto parameter = automationChooserLabel(target, false);
    return target.ownerKind == "device" ? automationChooserLabel(target, true) + " / " + parameter : parameter;
}
bool hasAutomationPoints(const engine::Snapshot &snapshot, const AutomationTarget &target) {
    const auto envelope = envelopeOf(snapshot, target);
    return envelope.data != nullptr && !envelope.data->points.empty();
}
struct HeaderChooserBoxes {
    juce::Rectangle<int> device, parameter, recordQuantize, playQuantize;
};
HeaderChooserBoxes headerChooserBoxes(int x, int width, int top, int height, bool midi, bool automation) noexcept {
    HeaderChooserBoxes boxes;
    const bool quantizeFits = midi && height >= 56 && width >= 50;
    if (quantizeFits) {
        boxes.recordQuantize = {x, top + 21, width, 14};
        boxes.playQuantize = {x, top + 37, width, 14};
    }
    if (!automation || width < 38)
        return boxes;

    const int split = TrackHeaderList::kSplitLine;
    const int standardHeight = 16;
    const int standardEnd = 46 + standardHeight;
    const int standardWithQuantizeEnd = 81 + 14;
    const int compactWithQuantizeEnd = 67 + 14;
    const int chooserWidth = std::min(width, 116);
    if (quantizeFits && height < compactWithQuantizeEnd + split) {
        // Keep quantize beside automation when the row cannot hold four fields.
        constexpr int quantizeWidth = 36, gap = 3, minimumChooserWidth = 38;
        const int automationWidth = width - quantizeWidth - gap;
        if (automationWidth >= minimumChooserWidth) {
            boxes.device = {x, top + 21, automationWidth, 14};
            boxes.parameter = {x, top + 37, automationWidth, 14};
            boxes.recordQuantize = {x + automationWidth + gap, top + 21, quantizeWidth, 14};
            boxes.playQuantize = {x + automationWidth + gap, top + 37, quantizeWidth, 14};
        }
    } else if (height >= (quantizeFits ? standardWithQuantizeEnd : standardEnd) + split) {
        boxes.device = {x, top + 26, chooserWidth, standardHeight};
        boxes.parameter = {x, top + 46, chooserWidth, standardHeight};
        if (quantizeFits) {
            boxes.recordQuantize.setY(top + 65);
            boxes.playQuantize.setY(top + 81);
        }
    } else {
        // Fit both or leave the collapsed header to its title, fold and menu.
        constexpr int compactTop = 19, gap = 2;
        const int field = std::min(14, (height - split - compactTop - gap) / 2);
        if (field >= 9) {
            boxes.device = {x, top + compactTop, chooserWidth, field};
            boxes.parameter = {x, top + compactTop + field + gap, chooserWidth, field};
            if (quantizeFits) {
                boxes.recordQuantize.setY(top + 51);
                boxes.playQuantize.setY(top + 67);
            }
        }
    }
    return boxes;
}
} // namespace

void TrackHeaderList::paint(juce::Graphics &g) {
    // Live's track header's first column: the track's colour, its fold switch and its name;
    // the panel's grey under the last track. A track in a group sits past a band in the group's
    // colour, one for each group it is in (Live 12 manual 18.3).
    g.fillAll(look::panel);
    int y = -view.geometry.scrollY;
    // The colour of the group at each depth: the walk goes down the tree, so above any row they
    // are its own groups'.
    std::array<juce::Colour, kMaxBands> groups{};
    const auto &folded = view.root.state().foldedGroups;
    const auto automationSnapshot = view.root.automationMode() ? view.root.current() : nullptr;
    const auto rows = view.geometry.rows(view.reader());
    const auto foot = view.geometry.footStart(rows);
    for (const auto row : rows) {
        // The foot stands at the bottom, over what scrolls under it, a rule above it.
        if (row.position == foot) {
            y = view.geometry.footTop(view.reader());
            g.setColour(look::panel);
            g.fillRect(0, y, getWidth(), getHeight() - y);
            g.setColour(look::separator);
            g.fillRect(0, y - 2, getWidth(), 2);
        }
        const auto *t = row.track;
        const auto colour = view.colourFor(row.position);
        if (row.place.group && row.place.depth < groups.size())
            groups[row.place.depth] = colour;
        const int h = view.geometry.heightFor(t->id);
        if (h == 0)
            continue; // left out by the project view, or folded away
        g.setColour(look::separator);
        g.fillRect(0, y, 2, h);
        g.fillRect(0, y + h - 2, getWidth(), 2);
        const int x = nameLeft(row.place.depth);
        // A line in each of its groups' colours down the whole row, so a group's lines run unbroken
        // past its tracks' feet (the Arrangement sheet).
        for (int band = 0; 2 + band * ArrangementView::kGroupBand < x; ++band) {
            g.setColour(groups[static_cast<std::size_t>(band)]);
            g.fillRect(2 + band * ArrangementView::kGroupBand, y, ArrangementView::kGroupLine, h);
        }
        g.setColour(colour);
        g.fillRect(x, y, getWidth() - x, h - 2);
        const auto fold = foldSwitch(row.place.depth, y);
        const auto ink = colour.darker(1.6f);
        g.setColour(ink);
        g.drawEllipse(fold.reduced(0.6f), 1.2f);
        if (row.place.group) {
            // Live 12's group switch: three lines, the top one out to the left while the group
            // is open, level while it is folded, as the manual's two figures show it.
            const bool closed = folded.count(t->id) != 0;
            for (int line = 0; line < 3; ++line) {
                const float shift = closed ? 0.f : line == 0 ? -1.f : 1.f;
                g.fillRect(fold.getCentreX() - 3.f + shift, fold.getCentreY() - 3.6f + 3.f * static_cast<float>(line),
                           6.f, 1.2f);
            }
        } else {
            const float angle = view.geometry.foldedTracks.contains(t->id) ? 0.f : juce::MathConstants<float>::halfPi;
            look::triangle(g, fold.reduced(3.5f), angle, ink);
        }
        const auto addLane = automationLaneButtonBox(t->id, row.place.depth, y, true);
        const auto toggleLanes = automationLaneButtonBox(t->id, row.place.depth, y, false);
        if (t->id != renaming_) {
            const int titleEnd = !toggleLanes.isEmpty() ? toggleLanes.getX()
                               : !addLane.isEmpty()     ? addLane.getX() : getWidth();
            const int badgeSpace = t->frozen ? 14 : 0;
            g.setColour(look::ink);
            g.setFont(look::font(look::fontBody, true));
            g.drawText(juce::String::fromUTF8(displayName(t->name, row.number()).c_str()), x + 23, y + 2,
                       std::max(0, titleEnd - x - 25 - badgeSpace), 15, juce::Justification::centredLeft);
            if (t->frozen)
                controls::frozenTrackBadge(g, {static_cast<float>(titleEnd - 14), static_cast<float>(y + 3), 11, 11});
        }
        if (!addLane.isEmpty())
            controls::button(g, addLane.toFloat(), controls::ButtonState::Rest, "+", look::fontLabel);
        if (!toggleLanes.isEmpty()) {
            const bool expanded = view.root.automationLanesExpanded(t->id);
            controls::button(g, toggleLanes.toFloat(), controls::ButtonState::Rest);
            look::triangle(g, toggleLanes.toFloat().reduced(4.f),
                           expanded ? juce::MathConstants<float>::halfPi : 0.f, look::text);
        }
        if (automationSnapshot) {
            const bool midi = isMidiTrack(*t);
            const auto device = automationChooserBox(row.place.depth, y, h, true, midi);
            const auto parameter = automationChooserBox(row.place.depth, y, h, false, midi);
            if (!device.isEmpty()) {
                const float font = std::min(static_cast<float>(look::fontSmall), static_cast<float>(device.getHeight() - 1));
                if (view.root.automationOverlayShown(t->id)) {
                    const auto target = view.root.automationTarget(t->id);
                    controls::chooser(g, device.toFloat(), automationChooserLabel(target, true), font);
                    controls::chooser(g, parameter.toFloat(), automationChooserLabel(target, false), font);
                    if (hasAutomationPoints(*automationSnapshot, target)) {
                        g.setColour(look::record);
                        g.fillEllipse(static_cast<float>(parameter.getX() + 2),
                                      static_cast<float>(parameter.getCentreY()) - 1.5f, 3.f, 3.f);
                    }
                } else {
                    controls::chooser(g, device.toFloat(), "None", font);
                    controls::chooser(g, parameter.toFloat(), {}, font, false);
                }
            }
        }
        // ADI's Rec-Q and Play-Q (MR V.2 §9) on a MIDI track's header: what new recordings are
        // quantized to ([1/16] or [Free]) and the live quantization (a grid or Off).
        if (isMidiTrack(*t))
            for (const bool record : {true, false})
                if (const auto box = quantizeBox(row.place.depth, y, h, record); !box.isEmpty()) {
                    // Each opens a menu: the shared chooser, which draws the arrow.
                    const auto value = record ? recordQuantizeText(t->recording) : playQuantizeText(t->recording);
                    controls::chooser(g, box.toFloat(), juce::String(record ? "Rec-Q " : "Play-Q ") + juce::String(value),
                                      look::fontSmall);
                    if (!record && t->recording.playEnabled) {
                        g.setColour(look::playQuantize);
                        g.drawRoundedRectangle(box.toFloat().reduced(.5f), look::radiusBox, 1.f);
                    }
                }
        const int stackHeight = view.geometry.stackHeightFor(t->id);
        if (stackHeight > h) {
            int laneTop = y + h;
            for (const auto &lane : view.root.additionalAutomationLanes(t->id)) {
                const int laneHeight = lane.height;
                if (laneTop < getHeight() && laneTop + laneHeight > 0) {
                    g.setColour(look::separator);
                    g.fillRect(0, laneTop, getWidth(), laneHeight);
                    for (int band = 0; 2 + band * ArrangementView::kGroupBand < x; ++band) {
                        g.setColour(groups[static_cast<std::size_t>(band)]);
                        g.fillRect(2 + band * ArrangementView::kGroupBand, laneTop, ArrangementView::kGroupLine,
                                   laneHeight);
                    }
                    g.setColour(look::panel);
                    g.fillRect(x, laneTop, getWidth() - x, laneHeight - 1);
                    g.setColour(colour);
                    g.fillRect(x, laneTop, 2, laneHeight - 1);
                    const auto remove = automationLaneRemoveBox(row.place.depth, laneTop, laneHeight);
                    controls::button(g, remove.toFloat(), controls::ButtonState::Rest,
                                     juce::String::fromUTF8("−"), look::fontLabel);
                    g.setColour(look::text);
                    g.setFont(look::font(look::fontSmall));
                    g.drawText(automationLaneLabel(lane.target), x + 6, laneTop,
                               std::max(0, remove.getX() - x - 10), laneHeight - 1, juce::Justification::centredLeft);
                }
                laneTop += laneHeight;
            }
        }
        y += stackHeight;
        for (const auto &take : view.geometry.takeLanes(*t)) {
            const int laneHeight = ArrangementGeometry::takeLaneHeight;
            const bool audition = view.takeAuditioned(t->id, take.id);
            g.setColour(audition ? colour : look::panel);
            g.fillRect(x, y, getWidth() - x, laneHeight - 2);
            g.setColour(audition ? look::ink : look::textOnBand);
            g.setFont(look::font(look::fontSmall));
            const auto name = juce::String::fromUTF8(take.name.c_str());
            const juce::String action = audition ? "Auditioning" : take.muted ? "Muted" : "Audition";
            auto box = juce::Rectangle<int>{x + 7, y, std::max(0, getWidth() - x - 12), laneHeight - 2};
            g.drawText(action, box.removeFromRight(std::min(68, box.getWidth() / 2)), juce::Justification::centredRight);
            g.drawFittedText(name, box, juce::Justification::centredLeft, 1);
            g.setColour(look::separator);
            g.fillRect(x, y + laneHeight - 2, getWidth() - x, 2);
            y += laneHeight;
        }
    }
    // Where the dragged tracks would land: a line at their depth, or the group they would go into.
    // White, edged in black, to read on every title's colour and on the rule between rows.
    if (moving_) {
        const int x = nameLeft(drop_.depth);
        if (drop_.into != 0) {
            const juce::Rectangle<int> box(x, view.geometry.rowTop(view.reader(), drop_.into), getWidth() - x,
                                           view.geometry.heightFor(drop_.into) - 2);
            g.setColour(look::ink);
            g.drawRect(box, 4);
            g.setColour(look::textBright);
            g.drawRect(box.reduced(1), 2);
        } else {
            g.setColour(look::ink);
            g.fillRect(x, drop_.y - 2, getWidth() - x, 4);
            g.setColour(look::textBright);
            g.fillRect(x, drop_.y - 1, getWidth() - x, 2);
        }
    }
}
void TrackHeaderList::resized() {
    if (renaming_ != 0)
        beginRename(renaming_, false);
}
bool TrackHeaderList::beginRename(std::int64_t track, bool selectTrack) {
    // The name as it is now, not as this window last drew it.
    const auto now = view.root.current();
    const auto *t = now->findTrack(track);
    if (t == nullptr)
        return false;
    // Its title bar where this window draws it; a track newer than this frame below the rows,
    // until the next frame's rows place it (ArrangementView::applyView).
    const auto rows = view.geometry.rows(view.reader());
    const int top = view.geometry.rowTop(view.reader(), track);
    const int y = top >= 0 ? top : view.geometry.tracksHeight(view.reader()) - view.geometry.scrollY;
    std::uint32_t depth = 0;
    const auto at = rows.find(track);
    if (at < rows.size())
        depth = rows[at].place.depth;
    renaming_ = track;
    if (selectTrack) {
        view.geometry.selection.track = track;
        view.root.publishSelection(AdiRootComponent::SelectionOrigin::Arrangement);
    }
    const int x = nameLeft(depth) + 2;
    editor.setBounds(x, y + 3, std::max(0, getWidth() - x - 4), 24);
    if (!editor.isVisible() || editor.getName() != juce::String(track)) {
        editor.setName(juce::String(track));
        editor.setText(juce::String::fromUTF8(t->name.c_str()), false);
        editor.selectAll();
    }
    editor.setVisible(true);
    if (editor.isShowing())
        focusIfShowing(editor);
    repaint();
    return true;
}
bool TrackHeaderList::commitRename() {
    const auto id = renaming_;
    if (id == 0)
        return false;
    const auto now = view.root.current();
    const auto *t = now->findTrack(id);
    if (t == nullptr)
        return true;
    // An empty name gives the track its default name back, as Live (walk-A A34).
    const auto text = renamedTrackName(*t, editor.getText().toStdString());
    if (t->name == text)
        return true;
    return view.root.submitEdit("track.rename", {{"id", id}, {"name", text}}, "Rename Track");
}
void TrackHeaderList::endRename() {
    renaming_ = 0;
    editor.setName({});
    editor.setVisible(false);
    if (view.canvas.isShowing())
        focusIfShowing(view.canvas);
    repaint();
}
juce::Rectangle<int> TrackHeaderList::quantizeBox(std::uint32_t depth, int top, int height, bool record) const noexcept {
    const int x = nameLeft(depth) + 4;
    const auto boxes = headerChooserBoxes(x, getWidth() - x - 6, top, height, true, view.root.automationMode());
    return record ? boxes.recordQuantize : boxes.playQuantize;
}
juce::Rectangle<int> TrackHeaderList::automationChooserBox(std::uint32_t depth, int top, int height,
                                                           bool device, bool midi) const noexcept {
    const int x = nameLeft(depth) + 4;
    const auto boxes = headerChooserBoxes(x, getWidth() - x - 6, top, height, midi, view.root.automationMode());
    return device ? boxes.device : boxes.parameter;
}
juce::Rectangle<int> TrackHeaderList::automationLaneButtonBox(std::int64_t track, std::uint32_t depth, int top,
                                                              bool add) const noexcept {
    if (!view.root.automationMode() || view.geometry.foldedTracks.contains(track) || getWidth() - nameLeft(depth) < 60)
        return {};
    if (add ? !view.root.automationOverlayShown(track) : view.root.additionalAutomationLanes(track).empty())
        return {};
    constexpr int size = 14, gap = 3, padding = 4;
    const int offset = add ? 0 : size + gap;
    return {getWidth() - padding - size - offset, top + 2, size, size};
}
juce::Rectangle<int> TrackHeaderList::automationLaneRemoveBox(std::uint32_t depth, int top, int height) const noexcept {
    constexpr int size = 14, padding = 4;
    return getWidth() - nameLeft(depth) > padding + size && height >= size
               ? juce::Rectangle<int>(getWidth() - padding - size, top + (height - size) / 2, size, size)
               : juce::Rectangle<int>();
}
juce::PopupMenu TrackHeaderList::automationChooserMenu(std::int64_t track, bool device) {
    juce::PopupMenu menu;
    const auto current = view.root.current();
    if (!current || current->findTrack(track) == nullptr)
        return menu;
    const bool overlay = view.root.automationOverlayShown(track);
    if (!device && !overlay)
        return menu;
    const auto shown = view.root.automationTarget(track);
    const auto choices = view.root.automationChoices(track);
    const juce::Component::SafePointer<TrackHeaderList> safe(this);
    if (device)
        menu.addItem("None", true, !overlay, [safe, track] {
            if (safe)
                safe->view.root.hideAutomationOverlay(track);
        });
    for (auto it = choices.begin(); it != choices.end(); ++it) {
        const auto &target = *it;
        const auto inDevice = [&target](const auto &other) { return sameAutomationDevice(target, other); };
        if (device ? std::any_of(choices.begin(), it, inDevice) : !inDevice(shown))
            continue;

        juce::PopupMenu::Item item(automationChooserLabel(target, device));
        item.itemID = menu.getNumItems() + 1;
        item.isTicked = overlay && (device ? inDevice(shown) : target == shown);
        const bool authored = device
            ? std::any_of(choices.begin(), choices.end(), [&](const auto &parameter) {
                return inDevice(parameter) && hasAutomationPoints(*current, parameter);
            })
            : hasAutomationPoints(*current, target);
        if (authored) {
            auto dot = std::make_unique<juce::DrawablePath>();
            juce::Path circle;
            circle.addEllipse(0.f, 0.f, 6.f, 6.f);
            dot->setPath(circle);
            dot->setFill(static_cast<juce::Colour>(look::record));
            item.image = std::move(dot);
        }
        auto choice = target;
        if (device && !overlay && view.root.automationLanesExpanded(track)) {
            const auto &pinned = view.root.additionalAutomationLanes(track);
            const auto available = std::find_if(choices.begin(), choices.end(), [&](const auto &candidate) {
                return inDevice(candidate) && std::none_of(pinned.begin(), pinned.end(), [&](const auto &lane) {
                    return lane.target == candidate;
                });
            });
            if (available != choices.end())
                choice = *available;
        }
        item.action = [safe, track, device, choice = std::move(choice)] {
            if (!safe)
                return;
            auto &root = safe->view.root;
            const auto shownNow = root.automationTarget(track);
            if (device && root.automationOverlayShown(track) && sameAutomationDevice(choice, shownNow))
                return;
            root.showAutomation(track, choice);
        };
        menu.addItem(std::move(item));
    }
    return menu;
}
juce::PopupMenu TrackHeaderList::quantizeMenu(std::int64_t track, bool record) {
    juce::PopupMenu menu;
    const auto *t = view.reader().findTrack(track);
    if (t == nullptr)
        return menu;
    const auto &o = t->recording;
    const bool on = record ? o.recordEnabled : o.playEnabled;
    const auto &grid = record ? o.recordGrid : o.playGrid;
    menu.addSectionHeader(record ? "Record Quantization" : "Live Quantization");
    menu.addItem(record ? "Free" : "Off", true, !on, [this, track, record] { view.root.setQuantize(track, record, "off"); });
    for (const char *choice : kQuantizeGrids)
        menu.addItem(choice, true, on && grid == choice,
                     [this, track, record, choice] { view.root.setQuantize(track, record, choice); });
    return menu;
}
void TrackHeaderList::mouseDown(const juce::MouseEvent &e) {
    // A lane action can change the row under a double-click before the second press arrives.
    const bool repeated = e.getNumberOfClicks() > 1;
    if (repeated && automationLaneButtonPressed_)
        return;
    automationLaneButtonPressed_ = false;
    if (const auto take = view.geometry.takeLaneAt(view.reader(), e.y)) {
        focusIfShowing(*this);
        foldPressed_ = true;
        resizeId = moveId_ = 0;
        if (e.mods.isPopupMenu()) {
            const auto track = take->track->id;
            const auto lane = take->lane->id;
            juce::PopupMenu menu;
            menu.addItem("Audition Take Lane", !take->lane->muted, view.takeAuditioned(track, lane),
                [safe = SafePointer<TrackHeaderList>(this), track, lane] {
                    if (safe)
                        safe->view.toggleTakeAudition(track, lane);
                });
            view.presentContextMenu(localPointToGlobal(e.getPosition()), true, nullptr, std::move(menu));
        } else if (!take->lane->muted) {
            view.toggleTakeAudition(take->track->id, take->lane->id);
        }
        return;
    }
    view.takeSelection.reset();
    view.canvas.clearAutomationSelection();
    if (view.loopSelected || view.selectedLocator != 0) {
        view.loopSelected = false;
        view.selectedLocator = 0;
        view.scrub.repaint();
        view.repaint();
    }
    if (view.selectedMeter >= 0) {
        view.selectedMeter = -1; // a click on a title lets a selected marker go
        view.scrub.repaint();
    }
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
    const auto position = e.getPosition();
    const bool popup = e.mods.isPopupMenu();
    const bool anchored = at < rows.size() && rows[at].place.foot();
    const bool split = at < rows.size() && onResizeEdge(e.y - top, view.geometry.heightFor(rows[at].track->id), anchored);
    foldPressed_ = false;
    resizeId = moveId_ = 0;
    moving_ = false;
    selectOnUp_ = 0;
    if (at < rows.size()) {
        const auto &row = rows[at];
        const auto track = row.track->id;
        const auto depth = row.place.depth;
        const int height = view.geometry.heightFor(track);
        if (int laneTop = top + height; e.y >= laneTop) {
            for (const auto &lane : view.root.additionalAutomationLanes(track)) {
                const int laneHeight = lane.height;
                if (e.y < laneTop + laneHeight) {
                    if (popup) {
                        const auto current = view.root.current();
                        const auto *envelope = current ? envelopeOf(*current, lane.target).lane : nullptr;
                        const juce::Component::SafePointer<TrackHeaderList> safe(this);
                        juce::PopupMenu menu;
                        menu.addItem("Remove Automation Lane", [safe, track, target = lane.target] {
                            if (safe)
                                safe->view.root.removeAutomationLane(track, target);
                        });
                        const auto where = e.getScreenPosition();
                        if (view.onContextMenu)
                            view.onContextMenu(where, true, envelope, menu);
                        else
                            menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this)
                                                   .withTargetScreenArea({where.x, where.y, 1, 1}));
                    } else if (!repeated && automationLaneRemoveBox(depth, laneTop, laneHeight).contains(position)) {
                        automationLaneButtonPressed_ = true;
                        const auto target = lane.target;
                        view.root.removeAutomationLane(track, target);
                    }
                    return;
                }
                laneTop += laneHeight;
            }
            return;
        }
        if (!popup && !(anchored && split)) {
            // The fold switch and header controls operate without selecting the track.
            if (foldSwitch(depth, top).expanded(2.f).contains(e.position)) {
                foldPressed_ = true;
                view.toggleFold(track);
                return;
            }
            if (automationLaneButtonBox(track, depth, top, true).contains(position)) {
                if (!repeated) {
                    automationLaneButtonPressed_ = true;
                    view.root.addAutomationLane(track, view.root.automationTarget(track));
                }
                return;
            }
            if (automationLaneButtonBox(track, depth, top, false).contains(position)) {
                if (!repeated) {
                    automationLaneButtonPressed_ = true;
                    view.root.toggleAutomationLanes(track);
                }
                return;
            }
            const bool midi = isMidiTrack(*row.track);
            for (const bool device : {true, false})
                if (const auto box = automationChooserBox(depth, top, height, device, midi);
                    box.contains(position)) {
                    if (device || view.root.automationOverlayShown(track))
                        automationChooserMenu(track, device)
                            .showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this)
                                               .withTargetScreenArea(localAreaToGlobal(box)));
                    return;
                }
            if (midi)
                for (const bool record : {true, false})
                    if (quantizeBox(depth, top, height, record).contains(position)) {
                        quantizeMenu(track, record)
                            .showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
                        return;
                    }
        }
    }
    if (auto *t = view.geometry.trackAt(view.reader(), e.y)) {
        focusIfShowing(*this);
        auto &sel = view.geometry.selection;
        if (popup && sel.clip == 0 && sel.pickedClips.empty() && sel.start == sel.end &&
            view.geometry.inSelection(view.reader(), t->id)) {
            view.contextMenu(e.getScreenPosition(), true);
            return;
        }
        // The split line resizes the row. The rest of the title moves it, except Main.
        const bool plain = !e.mods.isCommandDown() && !e.mods.isShiftDown() && !popup;
        // Ctrl/Cmd+click picks a track apart from the others, or lets it go again (Live: non-adjacent
        // tracks); Shift+click on another header reaches the selection across the tracks between.
        // A plain press on one of several selected tracks keeps them for a drag; released
        // without one, it selects that track alone.
        if (plain && !split && view.geometry.inSelection(view.reader(), t->id) &&
            view.geometry.selectedTracks(view.reader()).size() > 1)
            selectOnUp_ = t->id;
        else if (e.mods.isCommandDown() && sel.track != 0 && !popup) {
            auto &picked = sel.picked;
            if (t->id != sel.track) {
                if (const auto pickedAt = std::find(picked.begin(), picked.end(), t->id); pickedAt != picked.end())
                    picked.erase(pickedAt);
                else
                    picked.push_back(t->id);
            }
        } else if (e.mods.isShiftDown() && sel.track != 0 && !popup)
            sel.toTrack = t->id == sel.track ? 0 : t->id;
        else {
            sel.track = t->id;
            sel.toTrack = 0;
            sel.picked.clear();
        }
        // A title chooses tracks. The insert marker can stay at its previous position.
        sel.clearMaterial();
        if (split) {
            resizeId = t->id;
            resizeFromTop_ = anchored;
        } else if (!popup && at < rows.size() && !rows[at].place.main) {
            moveId_ = t->id;
            movingReturn_ = rows[at].place.ret;
            // Held still at the lanes' edge, the drag repeats, so they keep scrolling.
            beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
        }
        initialHeight = view.geometry.heightFor(t->id);
        view.changed();
    }
    if (popup) {
        resizeId = moveId_ = 0;
        view.contextMenu(e.getScreenPosition(), true);
    }
}
void TrackHeaderList::mouseMove(const juce::MouseEvent &e) {
    // The split line shows the resize cursor, as Live's.
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
    const bool split = at < rows.size() && onResizeEdge(e.y - top, view.geometry.heightFor(rows[at].track->id),
                                                        rows[at].place.foot());
    setMouseCursor(split ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}
TrackHeaderList::Drop TrackHeaderList::returnDropAt(int y) const {
    const auto &reader = view.reader();
    const auto rows = view.geometry.rows(reader);
    const auto foot = view.geometry.footStart(rows);
    Drop drop;
    // Over the tracks, into a group (ADR-0247 d2): a group's row or a row it holds takes the return
    // to the end of that group's returns; a return in a group, before it in its upper half, after it
    // in its lower, among that group's returns.
    int rowTop = 0;
    if (const auto at = view.geometry.rowAt(reader, y, &rowTop); at < foot) {
        const auto row = rows[at];
        if (row.place.ret && row.place.parent != 0) {
            const int h = view.geometry.heightFor(row.track->id);
            const bool upper = (y - rowTop) * 2 < h;
            drop.to = {row.place.parent, row.track->id};
            if (!upper) {
                drop.to.before = 0;
                for (auto j = at + 1; j < foot && drop.to.before == 0; ++j)
                    if (rows[j].place.ret && rows[j].place.parent == row.place.parent)
                        drop.to.before = rows[j].track->id;
            }
            drop.y = upper ? rowTop : rowTop + view.geometry.rowHeight(*row.track);
            drop.depth = row.place.depth;
            return drop;
        }
        const auto group = row.place.group ? row.track->id : row.place.parent;
        if (group != 0) {
            const auto g = rows.find(group);
            drop.to = {group, 0};
            drop.y = view.geometry.rowTop(reader, group);
            drop.depth = g < rows.size() ? rows[g].place.depth : 0;
            drop.into = group;
            return drop;
        }
    }
    // The returns' rows, top to bottom, where the foot draws them.
    int top = view.geometry.footTop(reader);
    drop.y = top;
    bool first = true;
    for (auto i = foot; i < rows.size(); ++i) {
        const auto row = rows[i];
        const int h = view.geometry.heightFor(row.track->id);
        const int stackHeight = view.geometry.rowHeight(*row.track);
        if (!row.place.ret) {
            top += stackHeight;
            continue;
        }
        if (first && y < top) {
            drop.to = {0, row.track->id};
            return drop;
        }
        first = false;
        if (y < top + h / 2) {
            drop.to = {0, row.track->id};
            drop.y = top;
            return drop;
        }
        top += stackHeight;
        drop.to = {0, 0};
        drop.y = top;
        // The next return, if one follows, is the one it goes before.
        for (auto j = i + 1; j < rows.size(); ++j)
            if (rows[j].place.ret) {
                drop.to = {0, rows[j].track->id};
                break;
            }
        if (y < top)
            return drop;
    }
    return drop;
}
TrackHeaderList::Drop TrackHeaderList::dropAt(int y) const {
    const auto &reader = view.reader();
    const auto rows = view.geometry.rows(reader);
    const auto foot = view.geometry.footStart(rows);
    Drop drop;
    int top = 0;
    const auto at = view.geometry.rowAt(reader, y, &top);
    if (at >= foot) {
        // Below the tracks, or over the returns and Main: after the last track at the top.
        drop.y = -view.geometry.scrollY;
        for (std::size_t i = 0; i < foot; ++i)
            drop.y += view.geometry.rowHeight(*rows[i].track);
        if (foot < rows.size())
            drop.y = std::min(drop.y, view.geometry.footTop(reader) - 2);
        return drop;
    }
    const auto row = rows[at];
    const auto &place = row.place;
    const auto id = row.track->id;
    const int h = view.geometry.heightFor(id);
    const int stackBottom = top + view.geometry.rowHeight(*row.track);
    const int down = y - top;
    // What follows the row in what holds it, past everything it holds; 0 at the end.
    auto next = [&] {
        auto j = at + 1;
        while (j < foot && rows[j].place.depth > place.depth)
            ++j;
        return j < foot && rows[j].place.parent == place.parent ? rows[j].track->id : 0;
    };
    auto above = [&] {
        drop.to = {place.parent, id};
        drop.y = top;
        drop.depth = place.depth;
    };
    auto below = [&] {
        drop.to = {place.parent, next()};
        drop.y = stackBottom;
        drop.depth = place.depth;
    };
    if (!place.group) {
        // A track: above it in its upper half, below it in its lower.
        if (down * 2 < h)
            above();
        else
            below();
        return drop;
    }
    // A group: above it in its top quarter. Open and holding tracks, under it, at the top of what
    // it holds; folded or empty, into it (at the end of what it holds), or below it in its last
    // quarter.
    const bool open = view.root.state().foldedGroups.count(id) == 0;
    const bool holds = at + 1 < foot && rows[at + 1].place.parent == id;
    if (down * 4 < h)
        above();
    else if (open && holds) {
        drop.to = {id, rows[at + 1].track->id};
        drop.y = stackBottom;
        drop.depth = place.depth + 1;
    } else if (down * 4 < h * 3) {
        drop.to = {id, 0};
        drop.y = top;
        drop.depth = place.depth;
        drop.into = id;
    } else
        below();
    return drop;
}
void TrackHeaderList::mouseDoubleClick(const juce::MouseEvent &e) {
    // Live 12 manual, Live Concepts: "Double-clicking a track's title bar brings up the Device View".
    // The first click selected the track; a double-click on a group's fold switch only folds it twice.
    if (e.mods.isPopupMenu() || automationLaneButtonPressed_ || view.geometry.takeLaneAt(view.reader(), e.y))
        return;
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
    if (at >= rows.size() || foldSwitch(rows[at].place.depth, top).expanded(2.f).contains(e.position))
        return;
    const auto &row = rows[at];
    const auto track = row.track->id;
    const auto depth = row.place.depth;
    const int height = view.geometry.heightFor(track);
    const auto position = e.getPosition();
    if (e.y >= top + height || automationLaneButtonBox(track, depth, top, true).contains(position) ||
        automationLaneButtonBox(track, depth, top, false).contains(position))
        return;
    const bool midi = isMidiTrack(*row.track);
    for (const bool device : {true, false})
        if (automationChooserBox(depth, top, height, device, midi).contains(position))
            return;
    view.root.showDeviceView();
}
void TrackHeaderList::mouseDrag(const juce::MouseEvent &e) {
    if (resizeId) {
        const int dy = e.getDistanceFromDragStartY();
        view.dragTrackHeight(resizeId, initialHeight + (resizeFromTop_ ? -dy : dy), e.mods.isAltDown());
    } else if (moveId_ != 0 && (moving_ || std::abs(e.getDistanceFromDragStartY()) > 4)) {
        // Live: "Tracks can be reordered by selecting and dragging them above or below other
        // tracks"; the line shows where they would land.
        moving_ = true;
        selectOnUp_ = 0;
        // At the lanes' top or foot they scroll, and the line follows; the returns stand still.
        if (!movingReturn_ && view.edgeScroll(0, e.y, false))
            view.frame(true);
        drop_ = movingReturn_ ? returnDropAt(e.y) : dropAt(e.y);
        repaint();
    }
}
void TrackHeaderList::mouseUp(const juce::MouseEvent &e) {
    if (std::exchange(foldPressed_, false))
        return;
    const auto dragged = moveId_;
    const bool moved = moving_;
    resizeId = moveId_ = 0;
    moving_ = false;
    if (const auto selected = std::exchange(selectOnUp_, 0); selected != 0 && !moved) {
        view.geometry.selection.track = selected;
        view.geometry.selection.toTrack = 0;
        view.geometry.selection.picked.clear();
    }
    view.changed();
    // After the window's state is kept, which clears the Status Bar: a refusal stays on it.
    if (moved) {
        repaint();
        view.root.moveTracks(dragged, (movingReturn_ ? returnDropAt(e.y) : dropAt(e.y)).to);
    }
}
void TrackHeaderList::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    view.wheel(e, w);
}
bool TrackHeaderList::keyPressed(const juce::KeyPress &key) {
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        return view.root.keyPressed(key);
    const auto code = key.getKeyCode();
    const auto &selection = view.geometry.selection;
    if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) &&
        key.getModifiers().getRawFlags() == juce::ModifierKeys::commandModifier &&
        renaming_ == 0 && !view.loopSelected && view.selectedLocator == 0 && view.selectedMeter < 0 &&
        selection.clip == 0 && selection.pickedClips.empty() && selection.start == selection.end) {
        view.root.nudgeSelectedTracks(code == juce::KeyPress::downKey);
        return true;
    }
    return view.canvas.keyPressed(key);
}
bool TrackHeaderList::isInterestedInDragSource(const SourceDetails &details) {
    return view.root.devices.isMovingDevice(details) || view.root.browser.acceptsDeviceDrop(details) ||
           !BrowserPanel::droppedFiles(details.description).isEmpty();
}
void TrackHeaderList::itemDragMove(const SourceDetails &details) {
    if (view.root.devices.isMovingDevice(details) || view.root.browser.acceptsDeviceDrop(details)) {
        view.canvas.previewDeviceDrop(details.localPosition.y, true);
        return;
    }
    fileDragMove(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
void TrackHeaderList::itemDragExit(const SourceDetails &) { view.canvas.clearDropPreview(); }
void TrackHeaderList::itemDropped(const SourceDetails &details) {
    view.canvas.clearDropPreview();
    if (view.root.devices.isMovingDevice(details)) {
        view.dropMovedDevice(details);
        return;
    }
    if (view.root.browser.acceptsDeviceDrop(details)) {
        if (const auto *target = view.geometry.trackAt(view.reader(), details.localPosition.y))
            view.root.browser.loadDroppedDevice(details, target->id);
        return;
    }
    filesDropped(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
bool TrackHeaderList::isInterestedInFileDrag(const juce::StringArray &files) {
    return !files.isEmpty();
}
void TrackHeaderList::fileDragMove(const juce::StringArray &files, int, int y) {
    view.canvas.previewSampleDrop(y, view.geometry.selection.insert, files.size(), true);
}
void TrackHeaderList::fileDragExit(const juce::StringArray &) { view.canvas.clearDropPreview(); }
void TrackHeaderList::filesDropped(const juce::StringArray &files, int, int y) {
    view.canvas.clearDropPreview();
    if (const auto *target = view.geometry.trackAt(view.reader(), y))
        view.root.dropAudioPaths(files, target->id, view.geometry.selection.insert);
}
void ArrangementCanvas::paint(juce::Graphics &g) {
    // Live's lanes: grey, every other group of bars a shade lighter, the selected
    // track's lanes lighter still, the bar grid, a dark rule between tracks.
    ++view.canvasPaints;
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    const auto midiTakes = view.root.liveMidiTakes ? view.root.liveMidiTakes() : std::span<const LiveMidiTake>{};
    const auto foot = geo.footStart(rows);
    const int footY = foot < rows.size() ? geo.footTop(view.reader()) : getHeight();
    const auto paintBounds = g.getClipBounds();
    int y = -geo.scrollY;
    auto lanes = [&](int top, int height, bool selected) {
        paintLanes(g, geo, getWidth(), top, height, selected);
    };
    // Under the last track, Live's drop area, down to the foot or the bottom.
    auto dropArea = [&](int top, int bottom) {
        if (top >= bottom)
            return;
        lanes(top, bottom - top, false);
        // Its words, faint as the Arrangement sheet draws them, only where the area holds them.
        const auto font = look::font(look::fontField);
        if (static_cast<float>(bottom - top) < font.getHeight() + 6.f)
            return;
        g.setColour(look::textFaint);
        g.setFont(font);
        g.drawText("Drop Files and Devices Here", 0, top, getWidth(), bottom - top, juce::Justification::centred);
    };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto row = rows[i];
        // The returns and Main at the foot, over the tracks scrolled under it.
        if (i == foot) {
            dropArea(y, footY);
            y = footY;
            g.setColour(look::band);
            g.fillRect(0, y, getWidth(), getHeight() - y);
            g.setColour(look::separator);
            g.fillRect(0, y - 2, getWidth(), 2);
        }
        const auto *t = row.track;
        const int h = geo.heightFor(t->id);
        if (h == 0)
            continue; // left out by the project view, or folded away: no room
        const int visibleBottom = i < foot ? std::min(footY, paintBounds.getBottom()) : paintBounds.getBottom();
        const int stack = geo.stackHeightFor(t->id);
        const auto paintAdditional = [&] {
            int below = y + h;
            if (stack == h || below >= visibleBottom || y + stack <= paintBounds.getY())
                return;
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(0, below, getWidth(), visibleBottom - below);
            for (const auto &lane : view.root.additionalAutomationLanes(t->id)) {
                lanes(below, lane.height - 2, false);
                below += lane.height;
                g.setColour(look::separator);
                g.fillRect(0, below - 2, getWidth(), 2);
            }
        };
        if (y >= visibleBottom || y + geo.rowHeight(*t) <= paintBounds.getY()) {
            y += geo.rowHeight(*t);
            continue;
        }
        // Every track the selection covers is lit, as Live lights a selection across tracks.
        const bool selected = geo.inSelection(view.reader(), t->id) && !hasAutomationSelection() && !view.takeSelection;
        lanes(y, h - 2, selected);
        g.setColour(look::separator);
        g.fillRect(0, y + h - 2, getWidth(), 2);
        if (selected && geo.selection.start != geo.selection.end) {
            const float x0 = static_cast<float>(geo.x(std::min(geo.selection.start, geo.selection.end)));
            const float x1 = static_cast<float>(geo.x(std::max(geo.selection.start, geo.selection.end)));
            g.setColour(look::selection.withAlpha(0.85f));
            g.fillRect(x0, static_cast<float>(y), x1 - x0, static_cast<float>(h - 2));
        }
        const auto colour = view.colourFor(row.position);
        if (row.place.group)
            paintGroupLane(g, i, y, h, colour);
        auto drawClip = [&](const engine::ClipNode &clip, int clipY, int clipHeight, bool selectedRange, bool home = false) {
            const auto *c = &clip;
            // A clip being moved by its bar is drawn where it would land; `home`, where it is.
            const bool along = !home && moving_ != 0 && !geo.selection.pickedClips.empty() && geo.clipSelected(c->id);
            const auto contentPos = home ? c->posTicks : c->id == moving_ ? movingTo_
                                                                        : along ? c->posTicks + movingDelta() : c->posTicks;
            const auto length = c->id == resizing_ ? resizedLength() : c->lengthTicks;
            const auto pos = c->id == resizing_ && resizeEdge_ == ResizeEdge::Left
                                 ? resizeStart_ + resizeFrom_ - length
                                 : contentPos;
            const double x = geo.x(pos), end = geo.x(pos + length);
            if (end < 0 || x > getWidth())
                return;
            const float left = static_cast<float>(std::max(-1., x)),
                        right = static_cast<float>(std::min(end, static_cast<double>(getWidth() + 1)));
            const bool picked = geo.clipSelected(c->id);
            // A clip as Live 12 draws it (the director's reference pictures, 18 November): the whole
            // clip in its colour, solid, title strip and body alike, and what it holds drawn dark on
            // it. A deactivated clip or a clip on a deactivated track is grey.
            const auto ownColour = c->color ? look::paletteColour(*c->color) : colour;
            const auto clipColour = c->muted || t->muted ? look::mutedClip : ownColour;
            g.setColour(picked ? clipColour.brighter(0.25f) : clipColour);
            g.fillRect(left, static_cast<float>(clipY + 1), std::max(1.f, right - left), static_cast<float>(clipHeight - 4));
            g.setColour(clipColour);
            g.fillRect(left, static_cast<float>(clipY + 1), std::max(1.f, right - left), static_cast<float>(kClipBarHeight));
            const auto tintSelectedBody = [&] {
                if (!selectedRange || geo.selection.start == geo.selection.end)
                    return;
                const float s0 = std::max(left, static_cast<float>(geo.x(std::min(geo.selection.start, geo.selection.end))));
                const float s1 = std::min(right, static_cast<float>(geo.x(std::max(geo.selection.start, geo.selection.end))));
                if (s1 <= s0)
                    return;
                g.setColour(look::selection.withAlpha(0.85f));
                g.fillRect(s0, static_cast<float>(clipY + 1 + kClipBarHeight), s1 - s0,
                           static_cast<float>(std::max(0, clipHeight - 4 - kClipBarHeight)));
            };
            // Keep the loop turns and repeated content readable through the time selection.
            if (c->loopEnabled)
                tintSelectedBody();
            const bool offline = view.root.clipMediaOffline(*c);
            const bool unavailable = offline ||
                (view.root.clipPlaybackUnavailable && view.root.clipPlaybackUnavailable(c->id));
            if (!offline)
                paintWaveform(g, *c, contentPos, left, right, static_cast<float>(clipY + 1 + kClipBarHeight),
                              static_cast<float>(clipY + clipHeight - 3));
            if (!c->audio)
                paintNotes(g, *c, contentPos, pos + length, left, right, static_cast<float>(clipY + 1 + kClipBarHeight),
                           static_cast<float>(clipY + clipHeight - 3));
            // Its name in its title strip, cut at the clip's end, as Live's (the manual's demo
            // arrangement).
            const bool problem = unavailable || hasPlaybackProblem(c->id);
            const float nameEnd = problem ? right - kPlaybackProblemMarkSize - 1.f : right;
            if (!c->name.empty() && nameEnd - left > 6.f) {
                g.setColour(look::ink);
                g.setFont(look::font(look::fontSmall));
                g.drawText(juce::String::fromUTF8(c->name.c_str()),
                           juce::Rectangle<float>(left + 3.f, static_cast<float>(clipY + 1), nameEnd - left - 5.f,
                                                  static_cast<float>(kClipBarHeight)),
                           juce::Justification::centredLeft, false);
            }
            g.setColour(clipColour.darker(0.5f));
            g.drawRect(left, static_cast<float>(clipY + 1), std::max(1.f, right - left),
                       static_cast<float>(clipHeight - 4), 1.f);
            if (problem) {
                const float markWidth = std::clamp(right - left - 2.f, 1.f, kPlaybackProblemMarkSize);
                const float markX = std::max(left, right - markWidth - 1.f);
                paintPlaybackProblemMark(g, {markX, static_cast<float>(clipY + 2), markWidth, kPlaybackProblemMarkSize});
            }
            if (!c->loopEnabled)
                tintSelectedBody();
            // Its fades, and their handles (manual 6.8), over the waveform and any selection tint;
            // not while it moves.
            if (c->audio && (home || (c->id != moving_ && !along)) && c->id != resizing_)
                paintFades(g, *c, static_cast<float>(x), static_cast<float>(end), static_cast<float>(clipY + 1 + kClipBarHeight),
                           static_cast<float>(clipY + clipHeight - 3));
            if (unavailable) {
                const auto banner = juce::Rectangle<float>(left + 1.f, static_cast<float>(clipY + 1 + kClipBarHeight),
                    std::max(0.f, right - left - 2.f), static_cast<float>(std::min(13, std::max(0, clipHeight - kClipBarHeight - 4))));
                g.setColour(look::error);
                g.fillRect(banner);
                g.setColour(look::ink);
                g.setFont(look::font(look::fontSmall));
                g.drawText(offline ? "Sample Offline" : "Playback unavailable", banner.reduced(3.f, 0.f),
                           juce::Justification::centredLeft, true);
            }
        };
        // A clip dragged across tracks is drawn on the destination lane, using that track's
        // colour if the clip has no colour of its own.
        for (const auto &c : t->clips) {
            // A copy being dragged leaves its original drawn where it is.
            if (copying_ && moving_ != 0 &&
                (c->id == moving_ || (!geo.selection.pickedClips.empty() && geo.clipSelected(c->id))))
                drawClip(*c, y, h, selected, true);
            if (const auto to = movingTargetOf(c->id); to == 0 || to == t->id)
                drawClip(*c, y, h, selected);
        }
        if (moving_ != 0)
            for (const auto &other : view.reader().tracks())
                if (other->id != t->id)
                    for (const auto &c : other->clips)
                        if (movingTargetOf(c->id) == t->id)
                            drawClip(*c, y, h, selected);
        if (selected && rangeDrag_ && !rangeDrag_->cancelled) {
            const auto [start, end] = std::minmax(rangeDrag_->from.start, rangeDrag_->from.end);
            const auto length = end - start;
            if (const auto to = rangeDrag_->to; to != start && to <= std::numeric_limits<std::int64_t>::max() - length) {
                const auto left = static_cast<float>(geo.x(to));
                const auto right = static_cast<float>(geo.x(to + length));
                g.setColour(look::selection);
                g.drawRect(left, static_cast<float>(y + 1), right - left, static_cast<float>(h - 4), 1.f);
            }
        }
        // Draw the captured audio over the lane from its actual first sample. The preview can lag
        // the playhead while the writer drains, and the playhead can keep rolling after Record stops.
        if (const auto &live = view.liveTake();
            live && std::find(live->tracks.begin(), live->tracks.end(), t->id) != live->tracks.end()) {
            auto start = live->start;
            auto end = live->landing ? start : view.root.timelineTick();
            const auto *preview = live->preview.get();
            const auto *snapshot = view.reader().get();
            const auto *tempo = preview && preview->loop && preview->loop->tempo
                ? preview->loop->tempo.get() : snapshot ? snapshot->tempo.get() : nullptr;
            const bool havePreview = preview && tempo && preview->peaks.sampleRate > 0;
            const engine::AudioRecordingPreview::Track *mapping = nullptr;
            double startSeconds = 0., sourceStartFrame = 0., sourceEndFrame = 0.;
            if (havePreview) {
                const auto track = std::find_if(preview->tracks.begin(), preview->tracks.end(),
                    [&](const auto &item) { return item.trackId == t->id; });
                if (track != preview->tracks.end())
                    mapping = &*track;
                const auto offset = mapping ? mapping->placementOffsetSamples : std::nullopt;
                if (const auto &punch = preview->punchedPlacement; !preview->loop && punch) {
                    const auto frames = std::max(preview->peaks.frames, live->capturedFrames);
                    const auto range = punchedPreviewRange(*punch, frames, preview->peaks.sampleRate, *tempo, offset);
                    if (range) {
                        start = range->startTicks;
                        end = range->endTicks;
                        startSeconds = range->startSeconds;
                        sourceStartFrame = static_cast<double>(range->sourceStartFrame);
                        sourceEndFrame = static_cast<double>(std::min(preview->peaks.frames, range->sourceEndFrame));
                    } else {
                        end = start;
                    }
                } else {
                    const auto rate = static_cast<double>(preview->peaks.sampleRate);
                    const auto pass = preview->latestPass(live->capturedFrames, offset);
                    startSeconds = static_cast<double>(pass.startSample) / rate;
                    sourceStartFrame = static_cast<double>(pass.sourceStartFrame);
                    sourceEndFrame = static_cast<double>(std::min(preview->peaks.frames, pass.sourceStartFrame + pass.frames));
                    start = tempo->secondsToTicks(startSeconds);
                    end = tempo->secondsToTicks(startSeconds + static_cast<double>(pass.frames) / rate);
                }
                if (preview->loop && snapshot && snapshot->transport.loopEnabled)
                    end = std::min(end, snapshot->transport.loopEndTicks);
            } else if (snapshot) {
                const auto &transport = snapshot->transport;
                if (transport.punchIn)
                    start = std::max(start, transport.loopStartTicks);
                if (transport.punchOut && !transport.loopEnabled)
                    end = std::min(end, transport.loopEndTicks);
            }
            if (end > start) {
                const double x0 = geo.x(start), x1 = geo.x(end);
                if (x1 >= 0 && x0 <= getWidth()) {
                    const float left = static_cast<float>(std::max(-1., x0)),
                                right = static_cast<float>(std::min(x1, static_cast<double>(getWidth() + 1)));
                    const float top = static_cast<float>(y + 1), width = std::max(1.f, right - left);
                    g.setColour(look::record);
                    g.fillRect(left, top, width, static_cast<float>(h - 4));
                    g.setColour(look::record.darker(0.3f));
                    g.fillRect(left, top, width, static_cast<float>(kClipBarHeight));
                    if (mapping && !mapping->channels.empty()) {
                        g.setColour(look::ink.withAlpha(0.85f));
                        const int from = std::max(0, static_cast<int>(std::floor(left)));
                        const int to = std::min(getWidth(), static_cast<int>(std::ceil(right)));
                        paintPeakColumns(g, preview->peaks, from, to, top + kClipBarHeight,
                                         top + static_cast<float>(h - 4), preview->peaks.channels, mapping->channels,
                                         [&](int x) -> std::optional<double> {
                            const auto seconds = tempo->ticksToSeconds(geo.tick(x)) - startSeconds;
                            return sourceStartFrame + std::max(0., seconds * preview->peaks.sampleRate);
                        }, sourceEndFrame, view.waveformZoomScale());
                    }
                    if (right - left > 6.f) {
                        g.setColour(look::ink);
                        g.setFont(look::font(look::fontSmall));
                        g.drawText(juce::String::fromUTF8(displayName(t->name, row.number()).c_str()),
                                   juce::Rectangle<float>(left + 3.f, top, right - left - 5.f,
                                                          static_cast<float>(kClipBarHeight)),
                                   juce::Justification::centredLeft, false);
                    }
                    g.setColour(look::record.darker(0.5f));
                    g.drawRect(left, top, width, static_cast<float>(h - 4), 1.f);
                }
            }
        }
        const auto midiTake = std::find_if(midiTakes.begin(), midiTakes.end(),
                                          [t](const LiveMidiTake &take) { return take.track == t->id; });
        if (midiTake != midiTakes.end())
            paintLiveMidiTake(g, *midiTake, *t, row.number(), y, h);
        const auto paintTakes = [&] {
            int takeTop = y + stack;
            for (const auto &take : geo.takeLanes(*t)) {
                const int laneHeight = ArrangementGeometry::takeLaneHeight;
                if (takeTop < visibleBottom && takeTop + laneHeight > paintBounds.getY()) {
                    const bool audition = view.takeAuditioned(t->id, take.id);
                    const bool selectedTake = view.takeSelection && view.takeSelection->track == t->id &&
                                              view.takeSelection->lane == take.id;
                    lanes(takeTop, laneHeight - 2, audition || selectedTake);
                    for (const auto &clip : t->takeClips)
                        if (clip->laneId == take.id)
                            drawClip(*clip, takeTop, laneHeight, false);
                    if (selectedTake) {
                        const auto &range = *view.takeSelection;
                        const float left = static_cast<float>(geo.x(std::min(range.start, range.end)));
                        const float right = static_cast<float>(geo.x(std::max(range.start, range.end)));
                        g.setColour(look::handleChosen.withAlpha(0.4f));
                        g.fillRect(left, static_cast<float>(takeTop + 1), std::max(1.f, right - left),
                                   static_cast<float>(laneHeight - 3));
                    }
                    g.setColour(look::separator);
                    g.fillRect(0, takeTop + laneHeight - 2, getWidth(), 2);
                }
                takeTop += laneHeight;
            }
        };
        paintAdditional();
        paintTakes();
        y += geo.rowHeight(*t);
    }
    if (foot == rows.size())
        dropArea(y, getHeight());
    // The insert marker has a one-pixel stem and a flag at the top of its lane.
    if (geo.selection.start == geo.selection.end && geo.selection.marked && !hasAutomationSelection()) {
        int top = -geo.scrollY;
        for (const auto row : rows) {
            if (row.position == foot)
                top = footY;
            const auto id = row.track->id;
            if (id == geo.selection.track && geo.heightFor(id) > 0) {
                // A track scrolled under the foot shows its marker down to the foot's rule.
                const int bottom = row.position < foot && foot < rows.size()
                                       ? std::min(top + geo.heightFor(id), footY) - 2
                                       : top + geo.heightFor(id) - 2;
                if (bottom > top) {
                    const auto x = static_cast<int>(geo.x(geo.selection.insert));
                    g.setColour(look::handleChosen);
                    g.fillRect(x, top, 1, bottom - top);
                    const auto flagTop = static_cast<float>(top);
                    constexpr float flagHalfWidth = 3.5f;
                    constexpr int flagHeight = 5;
                    const float centre = static_cast<float>(x) + .5f;
                    juce::Path flag;
                    flag.addTriangle(centre - flagHalfWidth, flagTop, centre + flagHalfWidth, flagTop,
                                     centre, flagTop + static_cast<float>(std::min(flagHeight, bottom - top)));
                    g.fillPath(flag);
                }
            }
            top += geo.rowHeight(*row.track);
        }
    }
    if (view.root.automationMode()) {
        paintAutomation(g);
        paintAutomationTransform(g);
        paintAutomationValueReadout(g);
    }
    // The returns and Main stay pinned over the scrolled tracks. Mark the bottom of
    // the scrolling band when another part of the arrangement continues underneath.
    const int scrollBottom = foot < rows.size() ? std::min(getHeight(), geo.footTop(view.reader())) : getHeight();
    if (scrollBottom >= 12 && geo.tracksHeight(view.reader()) - geo.scrollY > scrollBottom) {
        const juce::Rectangle<int> badge{8, scrollBottom - 10, 16, 8};
        g.setColour(look::frame);
        g.fillRect(badge);
        look::triangle(g, badge.toFloat().withSizeKeepingCentre(5.f, 8.f),
                       juce::MathConstants<float>::halfPi, look::text);
    }
    if (!dropPreview_.bounds.isEmpty()) {
        g.setColour(look::dropTarget);
        if (dropPreview_.device)
            g.drawRect(dropPreview_.bounds, 2);
        else
            g.fillRect(dropPreview_.bounds);
    }
    // The grid spacing sits inside the lower right of the lanes, above the time ruler.
    auto corner = getLocalBounds().reduced(2);
    const auto spacing = corner.removeFromBottom(14).removeFromRight(46);
    g.setColour(look::band);
    g.fillRect(spacing);
    g.setColour(look::textOnBand);
    g.setFont(look::font(look::fontBody));
    g.drawText(juce::String(gridLabel(geo.gridQuarters(), geo.grid.triplet)), spacing.reduced(2, 0),
               juce::Justification::centredRight);
}
std::vector<ArrangementCanvas::LaneBody> ArrangementCanvas::laneBodies() const {
    std::vector<LaneBody> out;
    if (!view.root.automationMode())
        return out;
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    const auto foot = geo.footStart(rows);
    const int footY = geo.footTop(view.reader());
    int top = -geo.scrollY;
    for (const auto row : rows) {
        if (row.position == foot)
            top = footY;
        const auto id = row.track->id;
        const int h = geo.heightFor(id);
        const bool aboveFoot = row.position < foot && foot < rows.size();
        const auto addBody = [&](int start, int end, const AutomationTarget &target) {
            const int bottom = aboveFoot ? std::min(end, footY) : end;
            if (bottom > start + 4)
                out.push_back({row.track, static_cast<float>(start), static_cast<float>(bottom),
                               static_cast<float>(end), target});
        };
        if (h > 0 && !geo.foldedTracks.contains(id) && view.root.automationOverlayShown(id))
            addBody(top + 1 + kClipBarHeight, top + h - 3, view.root.automationTarget(id));
        if (geo.stackHeightFor(id) > h) {
            int below = top + h;
            for (const auto &lane : view.root.additionalAutomationLanes(id)) {
                addBody(below + 2, below + lane.height - 3, lane.target);
                below += lane.height;
            }
        }
        top += geo.rowHeight(*row.track);
    }
    return out;
}
namespace {
// A value's height in a lane body, and back: the range's top at the body's top.
float automationY(double value, const AutomationRange &r, float top, float bottom) {
    const auto display = steppedAutomationValue(r, std::clamp(value, r.minimum, r.maximum));
    const auto level = (display - r.minimum) / (r.maximum - r.minimum);
    return bottom - static_cast<float>(r.flipped ? 1.0 - level : level) * (bottom - top);
}
double automationValue(float y, const AutomationRange &r, float top, float bottom) {
    const auto level = std::clamp(static_cast<double>((bottom - y) / std::max(1.f, bottom - top)), 0.0, 1.0);
    return steppedAutomationValue(r, std::lerp(r.minimum, r.maximum, r.flipped ? 1.0 - level : level));
}
double automationDragValue(const engine::AutomationLaneNode &lane, const AutomationTarget &target,
                           double original, double delta) {
    // A purely horizontal move retains the original dB value, including a quiet value below the drawn floor.
    if (delta == 0)
        return original;
    const auto range = automationRange(target);
    const auto before = steppedAutomationValue(range, std::clamp(original, range.minimum, range.maximum));
    auto value = writtenAutomationValue(target, before + (range.flipped ? -delta : delta) *
                                                      (range.maximum - range.minimum));
    if (lane.minValue)
        value = std::max(value, *lane.minValue);
    if (lane.maxValue)
        value = std::min(value, *lane.maxValue);
    return value;
}
constexpr auto kAutomationHold = static_cast<std::uint8_t>(engine::CurveShape::Hold);
constexpr auto kAutomationLinear = static_cast<std::uint8_t>(engine::CurveShape::Linear);
constexpr float kAutomationCurveSampleSpacing = 4.f;
constexpr int kAutomationCurveSampleLimit = 512;

struct AutomationMark {
    juce::Point<float> at;
    std::int64_t time = 0;
    double value = 0.0;
    std::uint8_t curve = kAutomationLinear;
    float tension = 0.f;
    const engine::AutomationCurveSlice *slice = nullptr;
    std::uint64_t id = 0;
    bool selected = false;
    const engine::AutomationPointNode *sliceEnd = nullptr;
};

struct AutomationCurveProjection {
    const ArrangementGeometry &geometry;
    const AutomationRange &range;
    float top, bottom;
    const engine::TempoMap *tempo;
    double rate;

    AutomationMark mark(std::int64_t time, double value, std::uint8_t curve = kAutomationLinear,
                        float tension = 0.f, const engine::AutomationCurveSlice *slice = nullptr,
                        std::uint64_t id = 0, bool selected = false,
                        const engine::AutomationPointNode *sliceEnd = nullptr) const {
        return {{static_cast<float>(geometry.x(time)), automationY(value, range, top, bottom)},
                time, value, curve, tension, slice, id, selected, sliceEnd};
    }

    bool held(const AutomationMark &point) const { return range.stepped || point.curve == kAutomationHold; }

    void appendSegment(juce::Path &line, const AutomationMark &from, const AutomationMark &to,
                       float left, float right) const {
        if (held(from)) {
            line.lineTo(to.at.x, from.at.y);
        } else {
            const float visibleFrom = std::max(from.at.x, left), visibleTo = std::min(to.at.x, right);
            // Point edits retain the source domain when the two endpoints retain their values
            // and remain neighbors, even when their times change.
            const auto *slice = from.sliceEnd && (from.sliceEnd->id != to.id || from.sliceEnd->value != to.value)
                                    ? nullptr : from.slice;
            const bool clamped = std::min(from.value, to.value) < range.minimum ||
                                 std::max(from.value, to.value) > range.maximum;
            const bool straight = !slice && !clamped && (from.curve == kAutomationLinear || from.tension == 0.f) &&
                                  (!tempo || tempo->events.size() < 2);
            if (!straight && visibleFrom < visibleTo) {
                const auto first = automationCurveTime(from.time, tempo, rate);
                const auto last = automationCurveTime(to.time, tempo, rate);
                const int steps = static_cast<int>(std::clamp(
                    std::ceil((visibleTo - visibleFrom) / kAutomationCurveSampleSpacing), 1.f,
                    static_cast<float>(kAutomationCurveSampleLimit)));
                for (int sample = 0; sample <= steps; ++sample) {
                    const float x = std::lerp(visibleFrom, visibleTo,
                                              static_cast<float>(sample) / static_cast<float>(steps));
                    if (x == from.at.x || x == to.at.x)
                        continue;
                    const auto tick = geometry.tick(x);
                    const auto phase = last > first ? (automationCurveTime(tick, tempo, rate) - first) / (last - first)
                                                   : (x - from.at.x) / (to.at.x - from.at.x);
                    const auto value = automationSegmentValue(from.value, to.value, phase, from.curve,
                                                              from.tension, slice);
                    line.lineTo(x, automationY(value, range, top, bottom));
                }
            }
        }
        line.lineTo(to.at);
    }

    juce::Path path(std::span<const AutomationMark> points, float left, float right) const {
        juce::Path line;
        line.startNewSubPath(left, points.front().at.y);
        line.lineTo(points.front().at);
        for (std::size_t i = 1; i < points.size(); ++i)
            appendSegment(line, points[i - 1], points[i], left, right);
        line.lineTo(right, points.back().at.y);
        return line;
    }
};
constexpr float kPointHit = 5.f;
constexpr float kAutomationDragSlop = 4.f;
constexpr float kAutomationNeighborSnap = 4.f;
constexpr float kCurveTensionDragPixels = 60.f;
constexpr float kAutomationCurveHitPixels = 5.f;
juce::String automationValueText(const AutomationTarget &target,double value,bool forEditing=false) {
    juce::String number;
    if(target.footIsSilence && value==kAutomationSilenceDb)number="-inf";
    else if(forEditing) {
        // Enter on the initial text must round-trip the stored breakpoint.
        const auto formatted = decimal::format(value);
        number = juce::String::fromUTF8(formatted.c_str());
    } else {
        const auto display = steppedAutomationValue(automationRange(target), value);
        number=juce::String(display,6).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    }
    if(!target.unit.empty())number+=" "+juce::String::fromUTF8(target.unit.c_str());
    return number;
}
std::optional<double> parseAutomationValue(juce::String text,const AutomationTarget &target) {
    text=text.trim().replaceCharacter(0x2212,'-');
    const auto unit=juce::String::fromUTF8(target.unit.c_str());
    if(unit.isNotEmpty() && text.endsWithIgnoreCase(unit))
        text=text.dropLastCharacters(unit.length()).trimEnd();
    if(target.footIsSilence && text.equalsIgnoreCase("-inf"))return kAutomationSilenceDb;
    const auto bytes=text.toStdString();
    std::string_view input(bytes);
    if(!input.empty() && input.front()=='+')input.remove_prefix(1);
    return decimal::parse(input);
}
} // namespace
std::optional<ArrangementCanvas::AutomationPick> ArrangementCanvas::automationPointAt(juce::Point<float> at) const {
    std::int64_t track = 0;
    ResizeEdge edge = ResizeEdge::None;
    if(barAt(juce::roundToInt(at.x),juce::roundToInt(at.y),track,edge))return std::nullopt;
    const auto current = view.root.current();
    if (!current)
        return std::nullopt;
    std::optional<AutomationPick> hit;
    float nearest = 2 * kPointHit + 1.f;
    for (const auto &body : laneBodies()) {
        if (at.y < body.top - kPointHit || at.y > body.bottom + kPointHit)
            continue;
        const auto range = automationRange(body.target);
        const auto env = envelopeOf(*current, body.target);
        if (env.data == nullptr)
            continue;
        for (const auto &p : env.data->points) {
            const float dx = std::abs(static_cast<float>(view.geometry.x(p.time)) - at.x);
            const float dy = std::abs(automationY(p.value, range, body.top, body.scaleBottom) - at.y);
            if (dx <= kPointHit && dy <= kPointHit && dx + dy < nearest) {
                nearest = dx + dy;
                hit = AutomationPick{body.track->id, p.id, body.target, env.lane->id, env.data->id};
            }
        }
    }
    return hit;
}
bool ArrangementCanvas::automationPickIsCurrent(const AutomationPick &pick) const {
    const auto current = view.root.current();
    if (!current || !view.root.automationTargetShown(pick.track, pick.target))
        return false;
    const auto envelope = envelopeOf(*current, pick.target);
    return envelope.lane && envelope.lane->id == pick.lane && envelope.data && envelope.data->id == pick.data &&
           std::any_of(envelope.data->points.begin(), envelope.data->points.end(),
                       [&](const auto &point) { return point.id == pick.point; });
}
void ArrangementCanvas::paintAutomation(juce::Graphics &g) const {
    // Each track's Volume envelope across its lane: the line, and a square on each breakpoint; with no
    // breakpoint, the fader's level as a line. The dragged point where the mouse has it.
    const auto current = view.root.current();
    if (!current)
        return;
    const auto &geo = view.geometry;
    const auto &selection = geo.selection;
    const auto from = std::min(selection.start, selection.end), to = std::max(selection.start, selection.end);
    const auto *addDrag = automationAddDragIsCurrent() ? &*automationAddDrag_ : nullptr;
    const bool hasSpan = hasAutomationSelection();
    const auto *drag = automationDrag_ && !automationDrag_->cancelled &&
                       automationDrag_->generation == view.root.committedGeneration() ? &*automationDrag_ : nullptr;
    for (const auto &body : laneBodies()) {
        const auto &target = body.target;
        const auto range = automationRange(target);
        const auto env = envelopeOf(*current, target);
        const AutomationCurveProjection projection{geo, range, body.top, body.scaleBottom,
            env.lane && env.lane->timeBase != 0 ? nullptr : current->tempo.get(), view.root.editRate()};
        const auto curvePreview = automationCurvePreview(body, env);
        const bool selectedLane = hasSpan && automationLaneSelected(body);
        const auto *keyboard = keyboardAutomationDraft_ &&
            keyboardAutomationDraft_->address.track == body.track->id &&
            keyboardAutomationDraft_->address.target == target &&
            keyboardAutomationDraft_->address.lane == (env.lane ? env.lane->id : 0) &&
            keyboardAutomationDraft_->address.data == (env.data ? env.data->id : 0) ? &*keyboardAutomationDraft_ : nullptr;
        const AutomationDragLane *dragLane = nullptr;
        if (drag && env.lane && env.data) {
            const auto captured = std::find_if(drag->lanes.begin(), drag->lanes.end(), [&](const auto &lane) {
                return lane.track == body.track->id && lane.lane == env.lane->id && lane.data == env.data->id;
            });
            if (captured != drag->lanes.end())
                dragLane = &*captured;
        }
        // Drawn inside the lane's visible strip only: a lane under the foot is not drawn over it.
        juce::Graphics::ScopedSaveState keep(g);
        g.reduceClipRegion(0, static_cast<int>(body.top) - 4, getWidth(), static_cast<int>(body.bottom - body.top) + 8);
        if (selectedLane) {
            if (from != to) {
                const float x0 = static_cast<float>(geo.x(from));
                const float x1 = static_cast<float>(geo.x(to));
                g.setColour(look::selection.withAlpha(.85f));
                g.fillRect(x0, body.top, x1 - x0, body.bottom - body.top);
            } else if (geo.selection.marked) {
                g.setColour(look::handleChosen);
                g.drawVerticalLine(juce::roundToInt(geo.x(geo.selection.insert)), body.top, body.bottom);
            }
        }
        std::vector<AutomationMark> points;
        if (env.data != nullptr)
            for (std::size_t index = 0; index < env.data->points.size(); ++index) {
                const auto &p = env.data->points[index];
                const bool dragged = pointDrag_ && pointDrag_->track == body.track->id &&
                                     pointDrag_->target == target && pointDrag_->point == p.id;
                if (dragLane && std::binary_search(dragLane->overwritten.begin(), dragLane->overwritten.end(), p.id))
                    continue;
                const bool moved = dragLane && !drag->spliceBounds &&
                    std::ranges::binary_search(dragLane->points, p.id, {}, &AutomationDragPoint::id);
                const auto time = moved ? p.time + drag->timeDelta : dragged ? pointTick_ : p.time;
                const auto value = moved ? automationDragValue(*env.lane, target, p.value, drag->valueDelta)
                                         : dragged ? pointValue_
                                         : keyboard && keyboard->address.point == p.id ? keyboard->value : p.value;
                const bool selected = selectedLane && p.time >= from && p.time < to;
                const bool shaped = curvePreview && curvePreview->point == p.id;
                const auto *sliceEnd = !shaped && value == p.value && p.curveSlice && index + 1 < env.data->points.size()
                                           ? &env.data->points[index + 1] : nullptr;
                points.push_back(projection.mark(time, value, shaped ? curvePreview->curve : p.curve,
                    shaped ? curvePreview->tension : p.tension, sliceEnd ? &*p.curveSlice : nullptr,
                    p.id, selected || moved, sliceEnd));
            }
        const bool adding = addDrag && addDrag->track == body.track->id && addDrag->target == target;
        if (adding)
            points.push_back(projection.mark(addDrag->tick, addDrag->value, target.stepped ? kAutomationHold : kAutomationLinear,
                                             0.f, nullptr, 0, true));
        if (adding || (dragLane && drag->timeDelta != 0) ||
            (pointDrag_ && pointDrag_->track == body.track->id && pointDrag_->target == target))
            std::stable_sort(points.begin(), points.end(),
                             [](const auto &a, const auto &b) { return a.time < b.time; });
        const float right = static_cast<float>(getWidth());
        const auto paintStored = [&] {
            g.setColour(look::record.withAlpha(.9f));
            if (points.empty()) {
                const float y = automationY(view.root.automationCurrent(target), range, body.top, body.scaleBottom);
                g.drawLine(0, y, right, y, 1.f);
                return;
            }
            g.strokePath(projection.path(points, 0, right), juce::PathStrokeType(1.f));
            for (const auto &p : points) {
                const auto box = juce::Rectangle<float>(7, 7).withCentre(p.at);
                const auto matches = [&](const auto &pick) {
                    return pick && env.lane && env.data && pick->track == body.track->id && pick->target == target &&
                           pick->lane == env.lane->id && pick->data == env.data->id && pick->point == p.id;
                };
                const bool chosen = p.selected || matches(chosenPoint_);
                const bool focused = matches(keyboardAutomationFocus_);
                g.setColour(chosen ? look::handleChosen : focused ? look::accent : look::panel);
                g.fillRect(box);
                g.setColour(look::record);
                g.drawRect(box, 1.f);
            }
        };
        const auto paintReplacement = [&](std::span<const AutomationMark> draft, float left, float rightEdge) {
            {
                juce::Graphics::ScopedSaveState outside(g);
                const auto x0 = static_cast<int>(std::clamp(std::floor(left), 0.f, right));
                const auto x1 = static_cast<int>(std::clamp(std::ceil(rightEdge), 0.f, right));
                g.excludeClipRegion({x0, 0, x1 - x0, getHeight()});
                paintStored();
            }
            g.setColour(look::record);
            g.strokePath(projection.path(draft, left, std::max(rightEdge, left + 1.f)), juce::PathStrokeType(1.f));
        };
        if (dragLane && drag->spliceBounds && drag->valueDelta != 0) {
            std::vector<AutomationMark> draft;
            draft.reserve(dragLane->points.size());
            for (const auto &point : dragLane->points) {
                const auto value = automationDragValue(*env.lane, target, point.value, drag->valueDelta);
                draft.push_back(projection.mark(point.time, value, point.curve, point.tension));
            }
            paintReplacement(draft, static_cast<float>(geo.x(drag->spliceBounds->first)),
                             static_cast<float>(geo.x(drag->spliceBounds->second)));
            continue;
        }
        const auto *stroke = automationStroke_ ? &*automationStroke_ : nullptr;
        if (!stroke || stroke->cancelled || stroke->track != body.track->id || stroke->target != target ||
            stroke->generation != view.root.committedGeneration() || !view.root.drawMode() || stroke->values.empty()) {
            paintStored();
            if (keyboard && !keyboard->address.point) {
                const juce::Point<float> at{static_cast<float>(geo.x(keyboard->tick)),
                    automationY(keyboard->value, range, body.top, body.scaleBottom)};
                const auto box = juce::Rectangle<float>(7, 7).withCentre(at);
                g.setColour(look::handleChosen);
                g.fillRect(box);
                g.setColour(look::record);
                g.drawRect(box, 1.f);
            }
            continue;
        }
        // The draft replaces just its time range in the lane until the stroke is committed.
        const auto left = static_cast<float>(geo.x(stroke->values.begin()->first));
        const auto end = static_cast<float>(geo.x(stroke->end));
        std::vector<AutomationMark> draft;
        draft.reserve(stroke->values.size());
        for (const auto &[tick, value] : stroke->values)
            draft.push_back(projection.mark(tick, value, stroke->stepped ? kAutomationHold : kAutomationLinear));
        paintReplacement(draft, left, end);
    }
}
bool ArrangementCanvas::beginAutomationStroke(const juce::MouseEvent &e) {
    const auto current = view.root.current();
    if (!current)
        return false;
    for (const auto &body : laneBodies()) {
        if (e.position.y < body.top || e.position.y > body.bottom)
            continue;
        // The second press of a double-click should not create a second edit.
        if (e.getNumberOfClicks() > 1)
            return true;
        auto target = body.target;
        if (!editableEnvelope(envelopeOf(*current, target))) {
            view.root.say("Draw Mode needs an envelope on the musical timeline.");
            return true;
        }
        auto &selection = view.geometry.selection;
        if (selection.track != body.track->id) {
            selection.track = body.track->id;
            selection.toTrack = 0;
            selection.picked.clear();
            selection.clip = 0;
            selection.pickedClips.clear();
            view.changed();
        }
        // Assigned, not emplace(): Clang 14 refuses a nested struct with default member initializers
        // there (CWG 1397), inside the class that declares it.
        automationStroke_ = AutomationStroke{};
        auto &stroke = *automationStroke_;
        stroke.track = body.track->id;
        stroke.generation = view.root.committedGeneration();
        stroke.target = std::move(target);
        stroke.stepped = view.geometry.grid.snap && !e.mods.isAltDown();
        stroke.top = body.top;
        stroke.bottom = body.scaleBottom;
        stroke.lastY = stroke.valueY = e.position.y;
        updateAutomationStroke(e);
        return true;
    }
    return false;
}
void ArrangementCanvas::updateAutomationStroke(const juce::MouseEvent &e) {
    auto &stroke = *automationStroke_;
    if (stroke.cancelled || stroke.generation != view.root.committedGeneration())
        return;
    const auto &geometry = view.geometry;
    const auto raw = geometry.tick(e.position.x);
    const auto tick = stroke.stepped ? geometry.snap(raw, false) : raw;
    // Shift reduces the vertical travel without jumping when pressed or released.
    stroke.valueY = std::clamp(stroke.valueY + (e.position.y - stroke.lastY) * (e.mods.isShiftDown() ? .1f : 1.f),
                               stroke.top, stroke.bottom);
    stroke.lastY = e.position.y;
    const auto range = automationRange(stroke.target);
    const double value = automationValue(stroke.valueY, range, stroke.top, stroke.bottom);
    constexpr std::size_t kMaxStrokePoints = 16'384;
    const auto put = [&](std::int64_t at, double v) {
        stroke.values.insert_or_assign(at, v);
        if (stroke.values.size() <= kMaxStrokePoints)
            return true;
        stroke.cancelled = true;
        view.root.say("Draw Mode stroke crossed too many grid cells.");
        return false;
    };
    if (stroke.stepped && !stroke.values.empty() && raw != stroke.lastTick) {
        const auto previousCell = geometry.snap(stroke.lastTick, false);
        const auto low = std::min(previousCell, tick), high = std::max(previousCell, tick);
        // Pointer events can skip cells in either direction. Fill the crossed cells from the
        // segment between the two events, as if each cell's leading edge had received an event.
        for (auto cell = geometry.nextGridLine(low, true); cell < high;) {
            const double t = static_cast<double>(cell - stroke.lastTick) / static_cast<double>(raw - stroke.lastTick);
            if (!put(cell, std::lerp(stroke.lastValue, value, t)))
                break;
            const auto next = geometry.nextGridLine(cell, true);
            if (next <= cell)
                break;
            cell = next;
        }
    }
    if (!stroke.cancelled && put(tick, value)) {
        stroke.lastTick = raw;
        stroke.lastValue = value;
        const auto lastTick = stroke.values.rbegin()->first;
        stroke.end = stroke.stepped ? geometry.nextGridLine(lastTick, true) : lastTick + 1;
    }
    repaint();
}
std::optional<ArrangementCanvas::AutomationValueAddress> ArrangementCanvas::automationValueAt(
    juce::Point<float> at,bool snapToGrid,bool allowAdd) const {
    if(!view.root.automationMode())return std::nullopt;
    const auto current=view.root.current();
    if(!current)return std::nullopt;
    std::int64_t titleTrack = 0;
    ResizeEdge edge = ResizeEdge::None;
    if(barAt(juce::roundToInt(at.x),juce::roundToInt(at.y),titleTrack,edge))return std::nullopt;
    const auto hit=automationPointAt(at);
    if(!hit && !allowAdd)return std::nullopt;
    const auto bodies=laneBodies();
    const auto body=std::find_if(bodies.begin(),bodies.end(),[&](const auto &item) {
        return hit ? item.track->id == hit->track && item.target == hit->target
                   : at.y >= item.top && at.y <= item.bottom;
    });
    if(body==bodies.end())return std::nullopt;
    AutomationValueAddress result;
    result.track=body->track->id;
    result.target=body->target;
    const auto envelope=envelopeOf(*current,result.target);
    result.lane=envelope.lane?envelope.lane->id:0;
    result.data=envelope.data?envelope.data->id:0;
    result.anchor=at.roundToInt();
    if(hit) {
        if(!envelope.data)return std::nullopt;
        const auto point=std::find_if(envelope.data->points.begin(),envelope.data->points.end(),[&](const auto &item) {
            return item.id==hit->point;
        });
        if(point==envelope.data->points.end())return std::nullopt;
        result.point=point->id;result.tick=point->time;result.value=point->value;
    } else {
        if(!editableEnvelope(envelope))return std::nullopt;
        const auto raw=std::max<std::int64_t>(0,view.geometry.tick(at.x));
        result.tick=snapToGrid?view.geometry.snap(raw,false):raw;
        result.value=view.root.automationCurrent(result.target);
        if(envelope.data)result.value=envelopeValue(*envelope.data,result.tick,result.target.stepped,
                                                     current->tempo.get(),view.root.editRate()).value_or(result.value);
    }
    return result;
}

bool ArrangementCanvas::resolveAutomationValue(const AutomationValueAddress &address,const TrackEnvelope &envelope) const {
    if(!view.root.automationTargetShown(address.track,address.target) ||
       (envelope.lane?envelope.lane->id:0)!=address.lane ||
       (envelope.data?envelope.data->id:0)!=address.data)return false;
    if(!address.point)return editableEnvelope(envelope);
    return envelope.data && std::any_of(envelope.data->points.begin(),envelope.data->points.end(),[&](const auto &point) {
        return point.id==*address.point && point.time==address.tick && point.value==address.value;
    });
}

namespace {
constexpr std::pair<automation::Shape, const char *> kArrangementShapes[]{
    {automation::Shape::Sine, "Sine"}, {automation::Shape::Triangle, "Triangle"},
    {automation::Shape::Sawtooth, "Sawtooth"}, {automation::Shape::InverseSawtooth, "Inverse Sawtooth"},
    {automation::Shape::Square, "Square"},
    {automation::Shape::ExponentialRise, "Exponential Rise"}, {automation::Shape::ExponentialFall, "Exponential Fall"},
    {automation::Shape::LogarithmicRise, "Logarithmic Rise"}, {automation::Shape::LogarithmicFall, "Logarithmic Fall"},
    {automation::Shape::ADSR, "ADSR"},
};

bool discreteAutomationEnvelope(const AutomationTarget &target, const engine::AutomationLaneNode *lane) {
    return target.stepped || (lane && lane->valueDomain == "enum");
}
} // namespace

void ArrangementCanvas::automationContextMenu(const juce::MouseEvent &e) {
    const auto edit = automationValueAt(e.position, view.snaps(e.mods), true);
    const auto current = view.root.current();
    const auto envelope = edit && current ? envelopeOf(*current, edit->target) : TrackEnvelope{};
    const auto *lane = edit ? envelope.lane : view.automationLaneAt(e.y);
    const auto &geometry = view.geometry;
    const auto &selection = geometry.selection;
    auto [begin, end] = std::minmax(selection.start, selection.end);
    const auto at = geometry.tick(e.position.x);
    const bool useSelection = edit && hasAutomationSelection() && begin < end && begin <= at && at < end &&
        std::find(automationSelectionTargets_.begin(), automationSelectionTargets_.end(),
                  AutomationLaneKey{edit->track, edit->target}) != automationSelectionTargets_.end();
    std::int64_t rangeBegin = begin, rangeEnd = end;
    if (edit && !useSelection) {
        // For a point's menu, its tick is the local marker. Background
        // clicks already located the insert marker in the clicked lane.
        rangeBegin = edit->point ? edit->tick : selection.insert;
        if (rangeBegin < std::numeric_limits<std::int64_t>::max()) {
            const auto gridBegin = geometry.snap(rangeBegin, false);
            const auto cell = geometry.nextGridLine(gridBegin, true) - gridBegin;
            if (cell > 0 && cell <= std::numeric_limits<std::int64_t>::max() - rangeBegin)
                rangeEnd = rangeBegin + cell;
            else
                rangeEnd = rangeBegin;
        }
    }

    const bool simplifySelection = useSelection && automationSelectionTargets_.size() == 1 &&
                                   lane && envelope.data && envelope.data->points.size() >= 3 &&
        std::any_of(envelope.data->points.begin(), std::prev(envelope.data->points.end()),
                    [](const auto &point) { return point.curve != kAutomationHold; });
    const auto generation = view.root.committedGeneration();
    const juce::Component::SafePointer<ArrangementCanvas> safe(this);
    view.contextMenu(e.getScreenPosition(), false, lane,
                     [safe, edit, lane, rangeBegin, rangeEnd, generation, simplifySelection](juce::PopupMenu &menu) {
        if (safe == nullptr || !edit)
            return;
        menu.addSeparator();
        const auto name = juce::String::fromUTF8(edit->point ? "Edit Value\xE2\x80\xA6" : "Add Value\xE2\x80\xA6");
        menu.addItem(name, [safe, address = *edit] {
            if (safe != nullptr)
                safe->beginAutomationValueEdit(address);
        });
        if (rangeBegin < 0 || rangeEnd <= rangeBegin || (lane && lane->timeBase != 0))
            return;
        const bool discrete = discreteAutomationEnvelope(edit->target, lane);
        juce::PopupMenu shapes;
        for (const auto &[shape, label] : kArrangementShapes) {
            if (shape != automation::Shape::Square && discrete)
                continue;
            shapes.addItem(label, [safe, address = *edit, chosenShape = shape, rangeBegin, rangeEnd, generation] {
                if (safe != nullptr)
                    safe->insertAutomationShape(address, chosenShape, rangeBegin, rangeEnd, generation);
            });
        }
        menu.addSubMenu("Insert Shape", std::move(shapes));
        if (simplifySelection && !discrete)
            menu.addItem("Simplify Envelope", [safe, address = *edit, rangeBegin, rangeEnd, generation] {
                if (safe != nullptr)
                    safe->simplifyAutomationEnvelope(address, rangeBegin, rangeEnd, generation);
            });
    });
}

void ArrangementCanvas::insertAutomationShape(const AutomationValueAddress &address, automation::Shape shape,
                                              std::int64_t begin, std::int64_t end, std::uint64_t generation) {
    const auto current = view.root.current();
    if (!current || view.root.committedGeneration() != generation) {
        view.root.say("The envelope changed. Open Insert Shape again.");
        return;
    }
    const auto &target = address.target;
    const auto envelope = envelopeOf(*current, target);
    if (!resolveAutomationValue(address, envelope) || !editableEnvelope(envelope)) {
        view.root.say("That automation lane is no longer editable.");
        return;
    }
    if (shape != automation::Shape::Square &&
        discreteAutomationEnvelope(target, envelope.lane))
        return;

    const auto rate = view.root.editRate();
    const auto firstSample = automationCurveTime(begin, current->tempo.get(), rate);
    const auto lastSample = automationCurveTime(end, current->tempo.get(), rate);
    if (!std::isfinite(firstSample) || !std::isfinite(lastSample) || begin < 0 || end <= begin ||
        lastSample <= firstSample || lastSample > 0x1p50) {
        view.root.say("Select a time range that spans at least one audio sample.");
        return;
    }
    const auto low = envelope.lane && envelope.lane->minValue
        ? std::max(target.minimum, *envelope.lane->minValue) : target.minimum;
    const auto high = envelope.lane && envelope.lane->maxValue
        ? std::min(target.maximum, *envelope.lane->maxValue) : target.maximum;
    if (high < low) {
        view.root.say("The envelope has no values in the displayed range.");
        return;
    }

    const auto base = envelope.lane ? envelope.lane->defaultValue : view.root.automationCurrent(target);
    const auto valueAt = [&](std::int64_t tick) {
        return envelope.data ? envelopeValue(*envelope.data, tick, target.stepped,
                                             current->tempo.get(), rate).value_or(base) : base;
    };
    double before = valueAt(begin);
    if (envelope.data) {
        const auto &points = envelope.data->points;
        const auto first = std::lower_bound(points.begin(), points.end(), begin, [](const auto &point, auto tick) {
            return point.time < tick;
        });
        if (first != points.end() && first->time == begin)
            before = first->value; // the incoming value at a coincident boundary
    }
    const auto after = valueAt(end);
    using automation::Shape;
    const bool risingRamp = shape == Shape::ExponentialRise || shape == Shape::LogarithmicRise;
    const bool fallingRamp = shape == Shape::ExponentialFall || shape == Shape::LogarithmicFall;
    const bool ramp = risingRamp || fallingRamp;
    const bool adsr = shape == Shape::ADSR;
    automation::ShapeSpec spec;
    spec.shape = shape;
    spec.start = begin;
    spec.end = end;
    if (!adsr && !ramp) {
        spec.low = low;
        spec.high = high;
    }
    spec.stepsPerCycle = static_cast<std::uint32_t>(std::clamp<std::int64_t>(end - begin, 2, spec.stepsPerCycle));

    try {
        auto points = automation::generateShape(spec);
        if (ramp) {
            // Join the adjoining value at the end that the ramp eases towards or away from.
            points.front().value = risingRamp ? std::min(low, after) : before;
            points.back().value = fallingRamp ? std::min(low, before) : after;
        } else if (adsr) {
            // Scale the ADSR from the line between its adjoining values to the
            // top of the displayed range, using the generator's native phases.
            for (auto &point : points) {
                const auto fraction = static_cast<double>(point.time - begin) / static_cast<double>(end - begin);
                point.value = std::lerp(std::lerp(before, after, fraction), high, point.value);
            }
        }

        constexpr const char *label = "Insert Automation Shape";
        if (envelope.lane) {
            std::vector<automation::NativeSpliceKnot> knots;
            knots.reserve(points.size());
            for (const auto &point : points)
                knots.push_back({point, {}});
            if (view.root.replaceAutomationRange(envelope.lane->id, begin, end, knots, label))
                focusIfShowing(*this);
            return;
        }

        RowIds ids(view.root.editStore().db());
        const auto lane = automationLaneSpec(target, base, ids.next("automation_lanes"), true);
        auto knots = Payload::array();
        std::uint64_t pointId = 1;
        const auto add = [&](std::int64_t tick, double value, std::uint8_t curve, float tension) {
            knots.push_back({{"id", pointId++}, {"time", tick}, {"value", value},
                             {"curve", curve}, {"tension", tension}});
        };
        // Coincident knots preserve the control's original value on both sides.
        if (begin > 0)
            add(begin, lane.defaultValue, 0, 0.f);
        for (const auto &point : points)
            add(point.time, point.value, point.curve, point.tension);
        add(end, lane.defaultValue, 0, 0.f);
        auto create = automation::createLane(lane);
        auto write = automation::writePoints(ids.next("automation_data"), lane.id, nullptr, std::move(knots));
        create.label = write.label = label;
        if (view.root.submitEdits({std::move(create), std::move(write)}))
            focusIfShowing(*this);
    } catch (const std::exception &error) {
        view.root.say(error.what());
    }
}

void ArrangementCanvas::simplifyAutomationEnvelope(const AutomationValueAddress &address, std::int64_t begin,
                                                    std::int64_t end, std::uint64_t generation) {
    const auto current = view.root.current();
    if (!current || view.root.committedGeneration() != generation) {
        view.root.say("The envelope changed. Open Simplify Envelope again.");
        return;
    }
    const auto envelope = envelopeOf(*current, address.target);
    if (!envelope.lane || !envelope.data || !resolveAutomationValue(address, envelope) ||
        !editableEnvelope(envelope) || discreteAutomationEnvelope(address.target, envelope.lane))
        return;

    const auto tolerance = automation::kSimplifyRelativeTolerance * (address.target.maximum - address.target.minimum);
    if (view.root.simplifyAutomationRange(envelope.lane->id, begin, end, tolerance))
        focusIfShowing(*this);
}

void ArrangementCanvas::beginAutomationValueEdit(const AutomationValueAddress &address) {
    endAutomationValueEdit(false);
    const auto current=view.root.current();
    if(!current || current->findTrack(address.track)==nullptr ||
       !resolveAutomationValue(address,envelopeOf(*current,address.target))) {
        view.root.say("That automation point or lane changed. Select it again.");
        return;
    }
    clearAutomationValueReadout();
    automationValueAddress_=address;
    constexpr int width=116,height=24;
    const int left=std::clamp(address.anchor.x+8,0,std::max(0,getWidth()-width));
    const int top=std::clamp(address.anchor.y-12,0,std::max(0,getHeight()-height));
    automationValueEditor_.setBounds(left,top,width,height);
    automationValueEditor_.setText(automationValueText(address.target,address.value,true),false);
    automationValueEditor_.selectAll();
    automationValueEditor_.setVisible(true);
    focusIfShowing(automationValueEditor_);
}

void ArrangementCanvas::commitAutomationValue() {
    if(!automationValueAddress_)return;
    const auto address=*automationValueAddress_;
    const auto current=view.root.current();
    if(!current || current->findTrack(address.track)==nullptr) {
        endAutomationValueEdit();
        return;
    }
    const auto envelope=envelopeOf(*current,address.target);
    if(!resolveAutomationValue(address,envelope)) {
        view.root.say("That automation point or lane changed. Select it again.");
        endAutomationValueEdit();
        return;
    }
    const auto value=parseAutomationValue(automationValueEditor_.getText(),address.target);
    if(!value || !validExactAutomationValue(envelope,address.target,*value)) {
        view.root.say("Enter a value within the automation control's range.");
        return;
    }
    const bool done=address.point
        ? view.root.moveAutomationPoint(address.track,address.target,*address.point,address.tick,*value,true)
        : view.root.addAutomationPoint(address.track,address.target,address.tick,*value,true);
    if(!done)return;
    endAutomationValueEdit();
    showAutomationValue(address.target,*value,address.anchor.toFloat());
    view.changed();
}

void ArrangementCanvas::endAutomationValueEdit(bool refocus) {
    if(!automationValueAddress_)return;
    automationValueAddress_.reset();
    if (keyboardValueEditing_) {
        keyboardValueEditing_ = false;
        keyboardAutomationDraft_.reset();
        chosenPoint_.reset();
    }
    automationValueEditor_.setVisible(false);
    if(refocus)focusIfShowing(*this);
    repaint();
}

void ArrangementCanvas::resetAutomationValueInput() {
    endAutomationValueEdit(false);
    keyboardAutomationLane_.reset();
    keyboardAutomationFocus_.reset();
    keyboardAutomationDraft_.reset();
    cancelAutomationAddDrag();
    clearAutomationValueReadout();
}

void ArrangementCanvas::showAutomationValue(const AutomationTarget &target,double value,juce::Point<float> at) {
    automationValueReadout_=automationValueText(target,value);
    const auto font=look::font(look::fontBody,true);
    automationValueGlyphs_.clear();
    automationValueGlyphs_.addLineOfText(font,automationValueReadout_,0.f,0.f);
    const float width=juce::GlyphArrangement::getStringWidth(font,automationValueReadout_)+12.f;
    constexpr float height=20.f;
    const float x=std::clamp(at.x+9.f,0.f,std::max(0.f,static_cast<float>(getWidth())-width));
    const float y=std::clamp(at.y-height-4.f,0.f,std::max(0.f,static_cast<float>(getHeight())-height));
    automationValueReadoutBounds_={x,y,width,height};
    repaint();
}

void ArrangementCanvas::clearAutomationValueReadout() {
    if(automationValueReadout_.isEmpty())return;
    automationValueReadout_.clear();
    automationValueGlyphs_.clear();
    repaint();
}

void ArrangementCanvas::paintAutomationValueReadout(juce::Graphics &g) const {
    if(automationValueReadout_.isEmpty() || automationValueAddress_)return;
    g.setColour(look::band);
    g.fillRoundedRectangle(automationValueReadoutBounds_,3.f);
    g.setColour(look::rulerLabel);
    automationValueGlyphs_.draw(g,juce::AffineTransform::translation(
        automationValueReadoutBounds_.getX()+6.f,automationValueReadoutBounds_.getY()+14.f));
}

bool ArrangementCanvas::hasAutomationSelection() const {
    const auto &selection=view.geometry.selection;
    // History restores time coordinates through the ordinary selection advisory.
    // A restored range on the same selected lanes keeps its envelope context.
    return view.root.automationMode() && automationSelection_ && !automationSelectionTargets_.empty() &&
           !view.loopSelected && view.selectedLocator == 0 && view.selectedMeter < 0 &&
           selection.track==automationSelection_->track && selection.toTrack==automationSelection_->toTrack &&
           selection.picked==automationSelection_->picked && selection.clip==0 && selection.pickedClips.empty() &&
           (selection.marked || selection.start!=selection.end);
}
void ArrangementCanvas::clearAutomationSelection() {
    automationTransformHover_.reset();
    if (automationTransformDrag_)
        automationTransformDrag_->cancelled = true;
    automationSelection_.reset();
    automationSelectionAnchor_.reset();
    automationSelectionTargets_.clear();
    keyboardAutomationLane_.reset();
    keyboardAutomationFocus_.reset();
    keyboardAutomationDraft_.reset();
}
namespace {
constexpr float kAutomationTransformMinSize = 24.f;
constexpr float kAutomationTransformHandleSize = 7.f;
constexpr float kAutomationTransformHandleHit = 6.f;
constexpr float kAutomationTransformFineScale = .1f;
constexpr double kAutomationTransformMinSpanningValue = .01;
} // namespace

std::optional<ArrangementCanvas::AutomationTransformBounds> ArrangementCanvas::automationTransformBounds() const {
    if (!hasAutomationSelection() || view.root.drawMode() || automationSelectionTargets_.size() != 1)
        return std::nullopt;
    const auto &selection = view.geometry.selection;
    const auto [begin, end] = std::minmax(selection.start, selection.end);
    if (begin < 0 || begin == end || end > view.geometry.tick(std::numeric_limits<double>::max()))
        return std::nullopt;
    const auto &key = automationSelectionTargets_.front();
    const auto current = view.root.current();
    if (!current || !view.root.automationTargetShown(key.first, key.second))
        return std::nullopt;
    const auto envelope = envelopeOf(*current, key.second);
    if (!envelope.lane || !envelope.data || envelope.data->points.empty() || !editableEnvelope(envelope))
        return std::nullopt;
    for (const auto &body : laneBodies()) {
        if (body.track->id != key.first || body.target != key.second)
            continue;
        const AutomationTransformBounds bounds{{key.first, body.target}, envelope.lane->id, envelope.data->id, begin, end,
                                                body.top, body.scaleBottom, body.bottom};
        const auto box = automationTransformBox(bounds);
        if (box.getWidth() >= kAutomationTransformMinSize && box.getHeight() >= kAutomationTransformMinSize &&
            box.intersects(getLocalBounds().toFloat()))
            return bounds;
    }
    return std::nullopt;
}

juce::Rectangle<float> ArrangementCanvas::automationTransformBox(const AutomationTransformBounds &bounds) const {
    const auto &geo = view.geometry;
    const auto x = static_cast<float>(geo.x(bounds.begin));
    return {x, bounds.top, static_cast<float>(geo.x(bounds.end)) - x, bounds.bottom - bounds.top};
}

std::array<juce::Point<float>, 8> ArrangementCanvas::automationTransformHandles(
    const AutomationTransformBounds &bounds) const {
    const auto box = automationTransformBox(bounds);
    const auto centre = box.getCentre();
    return {box.getTopLeft(), {centre.x, box.getY()}, box.getTopRight(), {box.getRight(), centre.y},
            box.getBottomRight(), {centre.x, box.getBottom()}, box.getBottomLeft(), {box.getX(), centre.y}};
}

std::optional<ArrangementCanvas::AutomationTransformHandle> ArrangementCanvas::automationTransformHandleAt(
    const AutomationTransformBounds &bounds, juce::Point<float> position) const {
    if (position.y < bounds.top)
        return std::nullopt;
    const auto handles = automationTransformHandles(bounds);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const auto handle = static_cast<AutomationTransformHandle>(i);
        if (bounds.target.second.stepped && handle != AutomationTransformHandle::Left &&
            handle != AutomationTransformHandle::Right)
            continue;
        const auto point = handles[i];
        if (point.y <= bounds.visibleBottom && position.y <= bounds.visibleBottom + kAutomationTransformHandleHit &&
            point.getDistanceFrom(position) <= kAutomationTransformHandleHit)
            return handle;
    }
    return std::nullopt;
}

bool ArrangementCanvas::beginAutomationTransform(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu())
        return false;
    const auto bounds = automationTransformBounds();
    const auto handle = bounds ? automationTransformHandleAt(*bounds, e.position) : std::nullopt;
    if (!handle)
        return false;
    if (e.getNumberOfClicks() == 1) {
        AutomationTransformDrag drag{*bounds, *handle, e.position, {}, bounds->begin, bounds->end};
        drag.generation = view.root.committedGeneration();
        automationTransformDrag_ = std::move(drag);
        view.followPaused = true;
        repaint();
    }
    return true;
}

void ArrangementCanvas::updateAutomationTransform(const juce::MouseEvent &e) {
    auto &drag = *automationTransformDrag_;
    if (drag.cancelled || drag.generation != view.root.committedGeneration())
        return;
    const auto motion = e.position - drag.lastPosition;
    drag.lastPosition = e.position;
    const auto handle = drag.handle;
    const bool horizontal = handle == AutomationTransformHandle::Left || handle == AutomationTransformHandle::Right;
    drag.distance += motion * (!horizontal && e.mods.isShiftDown() ? kAutomationTransformFineScale : 1.f);
    const bool mirrored = e.mods.isAltDown();
    const auto &source = drag.bounds;
    if (horizontal) {
        const auto &geo = view.geometry;
        const auto maxTick = geo.tick(std::numeric_limits<double>::max());
        const bool left = handle == AutomationTransformHandle::Left;
        const auto edge = left ? source.begin : source.end;
        const auto rawMotion = static_cast<double>(drag.distance.x) * textproj::kPPQ / geo.scale;
        const auto motionTicks = std::llround(std::clamp(rawMotion, -static_cast<double>(maxTick),
                                                        static_cast<double>(maxTick)));
        auto requested = std::clamp<std::int64_t>(edge + motionTicks, 0, maxTick);
        if (motionTicks != 0 && view.snaps(e.mods)) {
            const auto earlier = geo.snap(requested, false), later = geo.snap(requested, true);
            requested = requested - earlier <= later - requested ? earlier : later;
        }
        const auto half = (source.end - source.begin - 1) / 2;
        if (left) {
            const auto low = mirrored ? std::max(-source.begin, source.end - maxTick) : -source.begin;
            const auto high = mirrored ? half : source.end - source.begin - 1;
            const auto delta = std::clamp(requested - edge, low, high);
            drag.begin = source.begin + delta;
            drag.end = source.end - (mirrored ? delta : 0);
        } else {
            const auto low = mirrored ? -half : source.begin - source.end + 1;
            const auto high = mirrored ? std::min(source.begin, maxTick - source.end) : maxTick - source.end;
            const auto delta = std::clamp(requested - edge, low, high);
            drag.end = source.end + delta;
            drag.begin = source.begin - (mirrored ? delta : 0);
        }
    } else {
        const bool upper = handle == AutomationTransformHandle::Top || handle == AutomationTransformHandle::TopLeft ||
                           handle == AutomationTransformHandle::TopRight;
        const bool centre = handle == AutomationTransformHandle::Top || handle == AutomationTransformHandle::Bottom;
        const bool left = handle == AutomationTransformHandle::TopLeft || handle == AutomationTransformHandle::BottomLeft;
        const auto maximumInward = (1.0 - kAutomationTransformMinSpanningValue) / (centre && mirrored ? 2.0 : 1.0);
        const auto raw = -static_cast<double>(drag.distance.y) / (source.bottom - source.top);
        const auto delta = upper ? std::clamp(raw, -maximumInward, 1.0) : std::clamp(raw, -1.0, maximumInward);
        const std::size_t chosenEdge = upper ? 1u : 0u, oppositeEdge = 1u - chosenEdge;
        drag.values = {0.0, 1.0, 0.0, 1.0};
        if (centre || left)
            drag.values[chosenEdge] += delta;
        if (centre || !left)
            drag.values[chosenEdge + 2] += delta;
        if (mirrored) {
            if (centre || !left)
                drag.values[oppositeEdge] -= delta;
            if (centre || left)
                drag.values[oppositeEdge + 2] -= delta;
        }
    }
    repaint();
}

void ArrangementCanvas::finishAutomationTransform(const juce::MouseEvent &e) {
    updateAutomationTransform(e);
    auto drag = std::move(*automationTransformDrag_);
    automationTransformDrag_.reset();
    repaint();
    const auto bounds = automationTransformBounds();
    if (!bounds || drag.cancelled || !e.mouseWasDraggedSinceMouseDown() ||
        drag.generation != view.root.committedGeneration() || drag.bounds.target != bounds->target ||
        drag.bounds.laneId != bounds->laneId || drag.bounds.dataId != bounds->dataId ||
        drag.bounds.begin != bounds->begin || drag.bounds.end != bounds->end)
        return;

    edit::AutomationTransform transform{bounds->begin, bounds->end, drag.begin, drag.end, std::nullopt};
    if (drag.values != std::array<double, 4>{0.0, 1.0, 0.0, 1.0}) {
        const auto range = automationRange(bounds->target.second);
        const auto value = [&](double fraction) { return std::lerp(range.minimum, range.maximum, fraction); };
        transform.values = edit::AutomationValueTransform{range.minimum, range.maximum,
            value(drag.values[0]), value(drag.values[1]), value(drag.values[2]), value(drag.values[3])};
    } else if (drag.begin == bounds->begin && drag.end == bounds->end) {
        return;
    }
    const auto label = drag.handle == AutomationTransformHandle::Left || drag.handle == AutomationTransformHandle::Right ||
        drag.handle == AutomationTransformHandle::Top || drag.handle == AutomationTransformHandle::Bottom
        ? "Stretch Automation" : "Skew Automation";
    if (view.root.transformAutomationRange(bounds->laneId, transform, label))
        selectAutomationSpan(drag.begin, drag.end);
}

void ArrangementCanvas::paintAutomationTransform(juce::Graphics &g) const {
    if (!automationTransformDrag_ && !automationTransformHover_)
        return;
    const auto bounds = automationTransformBounds();
    if (!bounds)
        return;
    g.setColour(look::handleChosen);
    if (automationTransformDrag_) {
        const auto &drag = *automationTransformDrag_;
        if (drag.cancelled || drag.generation != view.root.committedGeneration() ||
            drag.bounds.target != bounds->target || drag.bounds.laneId != bounds->laneId ||
            drag.bounds.dataId != bounds->dataId)
            return;
        const auto &geo = view.geometry;
        const float x0 = static_cast<float>(geo.x(drag.begin)), x1 = static_cast<float>(geo.x(drag.end));
        const auto y = [&](std::size_t edge) {
            return bounds->bottom - static_cast<float>(drag.values[edge]) * (bounds->bottom - bounds->top);
        };
        juce::Path outline;
        outline.startNewSubPath(x0, y(1));
        outline.lineTo(x1, y(3));
        outline.lineTo(x1, y(2));
        outline.lineTo(x0, y(0));
        outline.closeSubPath();
        g.strokePath(outline, juce::PathStrokeType(1.f));
        return;
    }
    const auto box = automationTransformBox(*bounds);
    if (!box.expanded(kAutomationTransformHandleHit).contains(*automationTransformHover_) ||
        automationTransformHover_->y > bounds->visibleBottom + kAutomationTransformHandleHit)
        return;
    juce::Graphics::ScopedSaveState keep(g);
    g.reduceClipRegion(0, 0, getWidth(), juce::roundToInt(bounds->visibleBottom + kAutomationTransformHandleSize));
    g.drawRect(box, 1.f);
    const auto handles = automationTransformHandles(*bounds);
    const auto hovered = automationTransformHandleAt(*bounds, *automationTransformHover_);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const auto handle = static_cast<AutomationTransformHandle>(i);
        if (bounds->target.second.stepped && handle != AutomationTransformHandle::Left &&
            handle != AutomationTransformHandle::Right)
            continue;
        if (handles[i].y > bounds->visibleBottom)
            continue;
        const auto marker = juce::Rectangle<float>(kAutomationTransformHandleSize, kAutomationTransformHandleSize)
                                .withCentre(handles[i]);
        g.setColour(hovered == handle ? look::handleChosen : look::panel);
        g.fillRect(marker);
        g.setColour(look::handleChosen);
        g.drawRect(marker, 1.f);
    }
}

bool ArrangementCanvas::automationLaneSelected(const LaneBody &body) const {
    return std::any_of(automationSelectionTargets_.begin(), automationSelectionTargets_.end(), [&](const auto &lane) {
        return lane.first == body.track->id && lane.second == body.target;
    });
}
void ArrangementCanvas::rememberAutomationSelection(const std::vector<LaneBody> &bodies,
                                                     std::size_t destination, bool restart) {
    if (destination >= bodies.size())
        return;
    const auto &to = bodies[destination];
    if (restart)
        automationSelectionAnchor_.emplace(to.track->id, to.target);
    if (!automationSelectionAnchor_)
        return;
    const auto &anchor = *automationSelectionAnchor_;
    const auto from = std::find_if(bodies.begin(), bodies.end(), [&](const auto &body) {
        return body.track->id == anchor.first && body.target == anchor.second;
    });
    if (from == bodies.end())
        return;
    const auto first = static_cast<std::size_t>(from - bodies.begin());
    automationSelectionTargets_.clear();
    for (std::size_t i = std::min(first,destination); i <= std::max(first,destination); ++i)
        automationSelectionTargets_.emplace_back(bodies[i].track->id,bodies[i].target);
    automationSelection_ = view.geometry.selection;
}
bool ArrangementCanvas::automationSelectionDown(const juce::MouseEvent &e) {
    if (!view.root.automationMode())
        return false;
    const auto bodies = laneBodies();
    const auto body = std::find_if(bodies.begin(), bodies.end(),
                                   [&](const auto &lane) { return e.position.y >= lane.top && e.position.y <= lane.bottom; });
    if (body == bodies.end())
        return false;

    auto &sel = view.geometry.selection;
    const auto track = body->track->id;
    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.x));
    const bool snap = view.snaps(e.mods);
    const bool extending = hasAutomationSelection() && e.mods.isShiftDown() && !e.mods.isPopupMenu();
    if (e.mods.isPopupMenu() && hasAutomationSelection() && raw >= std::min(sel.start, sel.end) &&
        raw < std::max(sel.start, sel.end) &&
        automationLaneSelected(*body)) {
        automationContextMenu(e);
        return true;
    }
    if (extending) {
        if (sel.start == sel.end)
            sel.start = sel.insert;
        sel.end = snap ? view.geometry.snap(raw, raw >= sel.start) : raw;
        sel.toTrack = track == sel.track ? 0 : track;
    } else {
        const auto tick = snap ? view.geometry.snap(raw, false) : raw;
        sel = {};
        sel.track = track;
        sel.insert = sel.start = sel.end = tick;
        sel.marked = true;
    }
    rememberAutomationSelection(bodies,static_cast<std::size_t>(body-bodies.begin()),!extending);
    if (!extending && !e.mods.isPopupMenu())
        view.locate();
    view.changed();
    if (e.mods.isPopupMenu())
        automationContextMenu(e);
    else
        beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
    return true;
}
std::optional<AutomationTarget> ArrangementCanvas::automationCommandTarget() const {
    if (!view.root.automationMode())
        return std::nullopt;
    if (chosenPoint_ && hasKeyboardFocus(true) && automationPickIsCurrent(*chosenPoint_))
        return chosenPoint_->target;
    if (hasAutomationSelection()) {
        if (automationSelectionTargets_.size() == 1) {
            const auto &[track, target] = automationSelectionTargets_.front();
            if (view.root.automationTargetShown(track, target))
                return target;
        }
        return std::nullopt;
    }
    const auto track = view.geometry.selection.track;
    if (view.geometry.shown(track) && !view.geometry.foldedTracks.contains(track) &&
        view.root.automationOverlayShown(track))
        return view.root.automationTarget(track);
    return std::nullopt;
}
std::optional<std::vector<automation::LaneSpec>> ArrangementCanvas::automationRangeLanes() const {
    const auto current = view.root.current();
    if (!current || !hasAutomationSelection())
        return std::nullopt;
    std::vector<automation::LaneSpec> lanes;
    for (const auto &[track, target] : automationSelectionTargets_) {
        if (current->findTrack(track) == nullptr || !view.geometry.shown(track) ||
            !view.root.automationTargetShown(track,target))
            return std::nullopt;
        const auto envelope = envelopeOf(*current, target);
        automation::LaneSpec spec;
        spec.id = envelope.lane ? envelope.lane->id : 0;
        spec.ownerKind = target.ownerKind;
        spec.ownerId = target.ownerId;
        spec.paramRef = target.paramRef;
        spec.paramName = target.paramName;
        spec.timeBase = envelope.lane ? envelope.lane->timeBase : 0;
        spec.valueDomain = target.valueDomain;
        spec.unit = target.unit;
        spec.defaultValue = writtenAutomationValue(target, view.root.automationCurrent(target));
        spec.minValue = target.footIsSilence ? kAutomationSilenceDb : target.minimum;
        spec.maxValue = target.maximum;
        lanes.push_back(std::move(spec));
    }
    return lanes;
}
void ArrangementCanvas::selectAutomationSpan(std::int64_t begin, std::int64_t end) {
    auto &sel = view.geometry.selection;
    sel.insert = sel.start = begin;
    sel.end = end;
    sel.marked = true;
    automationSelection_ = sel;
    view.changed();
}
bool ArrangementCanvas::beginSingleAutomationDrag(const AutomationPick &pick, const juce::MouseEvent &e) {
    if (!automationPickIsCurrent(pick))
        return false;
    const auto current = view.root.current();
    const auto envelope = envelopeOf(*current, pick.target);
    if (!editableEnvelope(envelope))
        return false;
    const auto &points = envelope.data->points;
    const auto point = std::find_if(points.begin(), points.end(), [&](const auto &entry) {
        return entry.id == pick.point && entry.time == pointTick_ && entry.value == pointValue_;
    });
    const auto bodies = laneBodies();
    const auto body = std::find_if(bodies.begin(), bodies.end(), [&](const auto &candidate) {
        return candidate.track->id == pick.track && candidate.target == pick.target;
    });
    if (point == points.end() || body == bodies.end())
        return false;

    AutomationDrag drag;
    drag.origin = drag.lastPointer = e.mouseDownPosition;
    drag.anchorTick = point->time;
    drag.anchorData = pick.data;
    drag.height = std::max(1.f, body->scaleBottom - body->top);
    drag.generation = view.root.committedGeneration();
    drag.lanes.push_back({pick.track, pick.lane, pick.data, pick.target,
        {{point->id, point->time, point->value, point->curve, point->tension}}, {point->time}, {}});
    automationDrag_ = std::move(drag);
    return true;
}

bool ArrangementCanvas::beginSelectedAutomationDrag(const AutomationPick &pick, const juce::MouseEvent &e) {
    if (!hasAutomationSelection() || e.mods.isCommandDown())
        return false;

    const auto &selection = view.geometry.selection;
    const auto [begin, end] = std::minmax(selection.start, selection.end);
    if (begin == end)
        return false;
    const auto bodies = laneBodies();
    const auto body = std::find_if(bodies.begin(), bodies.end(), [&](const auto &candidate) {
        return candidate.track->id == pick.track && candidate.target == pick.target;
    });
    const auto current = view.root.current();
    if (!current || body == bodies.end() || !automationLaneSelected(*body))
        return false;

    AutomationDrag drag;
    drag.origin = drag.lastPointer = e.position;
    drag.height = std::max(1.f, body->scaleBottom - body->top);
    drag.generation = view.root.committedGeneration();
    drag.movesSelection = true;
    bool capturedAnchor = false;
    for (const auto &[track, target] : automationSelectionTargets_) {
        if (!view.root.automationTargetShown(track, target))
            return false;
        const auto envelope = envelopeOf(*current, target);
        if (!envelope.data || !envelope.lane || !editableEnvelope(envelope))
            continue;
        AutomationDragLane lane{track, envelope.lane->id, envelope.data->id, target, {}, {}, {}};
        const auto &points = envelope.data->points;
        const auto first = std::lower_bound(points.begin(), points.end(), begin,
                                            [](const auto &point, auto tick) { return point.time < tick; });
        for (auto point = first; point != points.end() && point->time < end; ++point) {
            lane.points.push_back({point->id, point->time, point->value, point->curve, point->tension});
            lane.pointTimes.push_back(point->time);
            if (track == pick.track && target == pick.target && lane.lane == pick.lane && lane.data == pick.data &&
                point->id == pick.point) {
                drag.anchorTick = point->time;
                drag.anchorData = pick.data;
                capturedAnchor = true;
            }
        }
        if (!lane.points.empty()) {
            std::sort(lane.points.begin(), lane.points.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
            drag.lanes.push_back(std::move(lane));
        }
    }
    if (!capturedAnchor)
        return false;
    automationDrag_ = std::move(drag);
    return true;
}

bool ArrangementCanvas::beginAutomationSegmentDrag(const juce::MouseEvent &e) {
    if (e.getNumberOfClicks() > 1 || e.mods.isCommandDown())
        return false;
    constexpr float lineHit = 4.f, segmentHit = 8.f;
    const auto at = e.position;
    const auto segment = automationSegmentAt(at, segmentHit);
    if (!segment || (segment->distance <= lineHit && !e.mods.isShiftDown()))
        return false;
    const auto current = view.root.current();
    const auto &geometry = view.geometry;
    const auto &body = segment->body;
    const auto envelope = envelopeOf(*current, body.target);
    const auto &points = envelope.data->points;
    const auto &leftPoint = points[segment->rightIndex - 1], &rightPoint = points[segment->rightIndex];
    const auto range = automationRange(body.target);

    AutomationDrag drag;
    drag.origin = drag.lastPointer = at;
    drag.anchorTick = geometry.tick(at.x);
    drag.height = std::max(1.f, body.scaleBottom - body.top);
    drag.generation = view.root.committedGeneration();
    drag.verticalOnly = true;
    AutomationDragLane lane{body.track->id, envelope.lane->id, envelope.data->id, body.target, {}, {}, {}};
    const auto [begin, end] = std::minmax(geometry.selection.start, geometry.selection.end);
    if (hasAutomationSelection() && automationLaneSelected(body) &&
        begin <= drag.anchorTick && drag.anchorTick < end) {
        drag.spliceBounds.emplace(begin, end);

        const auto boundary = [&](std::int64_t tick, auto next) {
            if (next == points.begin())
                return AutomationDragPoint{0, tick, next->value, 0, 0.f};
            const auto &previous = *(next - 1);
            double value = previous.value;
            if (next != points.end() && tick != previous.time && !range.stepped && previous.curve != 0) {
                const auto tempo = current->tempo.get();
                const auto rate = view.root.editRate();
                const auto first = automationCurveTime(previous.time, tempo, rate);
                const auto lastSample = automationCurveTime(next->time, tempo, rate);
                const auto fraction = lastSample > first
                    ? (automationCurveTime(tick, tempo, rate) - first) / (lastSample - first)
                    : static_cast<double>(tick - previous.time) / static_cast<double>(next->time - previous.time);
                value = automationSegmentValue(previous.value, next->value, fraction, previous.curve,
                                               previous.tension, previous.curveSlice ? &*previous.curveSlice : nullptr);
            }
            const auto curve = next == points.end() || range.stepped ? std::uint8_t{0} : previous.curve;
            return AutomationDragPoint{0, tick, value, curve, previous.tension};
        };
        // Begin takes the settled value after any coincident points. End takes the incoming
        // value before the untouched envelope resumes, which matters at a held step.
        const auto afterBegin = std::upper_bound(points.begin(), points.end(), begin,
                                                [](auto tick, const auto &point) { return tick < point.time; });
        const auto beforeEnd = std::lower_bound(points.begin(), points.end(), end,
                                                [](const auto &point, auto tick) { return point.time < tick; });
        lane.points.push_back(boundary(begin, afterBegin));
        for (auto point = afterBegin; point != beforeEnd; ++point)
            lane.points.push_back({point->id, point->time, point->value, point->curve, point->tension});
        lane.points.push_back(boundary(end, beforeEnd));
    } else {
        lane.points = {{leftPoint.id, leftPoint.time, leftPoint.value, leftPoint.curve, leftPoint.tension},
                       {rightPoint.id, rightPoint.time, rightPoint.value, rightPoint.curve, rightPoint.tension}};
        if (lane.points[0].id > lane.points[1].id)
            std::swap(lane.points[0], lane.points[1]);
    }
    drag.lanes.push_back(std::move(lane));
    automationDrag_ = std::move(drag);
    return true;
}

void ArrangementCanvas::updateOverwrittenAutomationPoints(AutomationDrag &drag, const engine::Snapshot &current) {
    const auto delta = drag.timeDelta;
    for (auto &lane : drag.lanes) {
        lane.overwritten.clear();
        const auto envelope = envelopeOf(current, lane.target);
        if (delta == 0 || lane.pointTimes.empty() || !envelope.data)
            continue;
        const auto low = lane.pointTimes.front() + std::min<std::int64_t>(0, delta);
        const auto high = lane.pointTimes.back() + std::max<std::int64_t>(0, delta);
        const auto &points = envelope.data->points;
        auto point = std::lower_bound(points.begin(), points.end(), low, [](const auto &entry, auto tick) {
            return entry.time < tick;
        });
        for (; point != points.end() && point->time <= high; ++point) {
            if (std::ranges::binary_search(lane.points, point->id, {}, &AutomationDragPoint::id))
                continue;
            const auto &times = lane.pointTimes;
            // Erase only stationary points swept by at least one moved point,
            // including its destination but excluding its original tick.
            bool crossed;
            if (delta > 0) {
                const auto after = std::lower_bound(times.begin(), times.end(), point->time);
                crossed = after != times.begin() && point->time - *std::prev(after) <= delta;
            } else {
                const auto next = std::upper_bound(times.begin(), times.end(), point->time);
                crossed = next != times.end() && *next - point->time <= -delta;
            }
            if (crossed)
                lane.overwritten.push_back(point->id);
        }
        std::sort(lane.overwritten.begin(), lane.overwritten.end());
    }
}

void ArrangementCanvas::updateAutomationDrag(const juce::MouseEvent &e) {
    auto &drag = *automationDrag_;
    const auto current = view.root.current();
    if (drag.cancelled || !current || !view.root.automationMode() ||
        drag.generation != view.root.committedGeneration())
        return;

    auto step = e.position - drag.lastPointer;
    drag.lastPointer = e.position;
    if (step == juce::Point<float>{})
        return;
    if (!e.mods.isShiftDown()) {
        drag.shiftAxis = AutomationDrag::Axis::Undecided;
        drag.pendingShift = {};
    } else if (!drag.verticalOnly) {
        if (drag.shiftAxis == AutomationDrag::Axis::Undecided) {
            drag.pendingShift += step;
            if (drag.pendingShift.getDistanceFromOrigin() < kAutomationDragSlop)
                return;
            step = drag.pendingShift;
            drag.pendingShift = {};
            drag.shiftAxis = std::abs(step.x) >= std::abs(step.y)
                ? AutomationDrag::Axis::Horizontal : AutomationDrag::Axis::Vertical;
        }
        if (drag.shiftAxis == AutomationDrag::Axis::Horizontal)
            step.y = 0;
        else
            step.x = 0;
    }

    const auto &geo = view.geometry;
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    auto minimumTime = -limit, maximumTime = limit;
    double minimumValue = -1, maximumValue = 1;
    for (const auto &lane : drag.lanes) {
        const auto range = automationRange(lane.target);
        for (const auto &point : lane.points) {
            minimumTime = std::max(minimumTime, -point.time);
            maximumTime = std::min(maximumTime, limit - point.time);
            const auto displayed = steppedAutomationValue(range, std::clamp(point.value, range.minimum, range.maximum));
            auto normalized = (displayed - range.minimum) / (range.maximum - range.minimum);
            if (range.flipped)
                normalized = 1 - normalized;
            minimumValue = std::max(minimumValue, -normalized);
            maximumValue = std::min(maximumValue, 1 - normalized);
        }
    }
    if (drag.movesSelection) {
        minimumTime = std::max(minimumTime, -std::min(geo.selection.start, geo.selection.end));
        maximumTime = std::min(maximumTime, limit - std::max(geo.selection.start, geo.selection.end));
    }
    if (!drag.verticalOnly && step.x != 0) {
        drag.horizontalTravel += step.x;
        const auto travelled = geo.tick(drag.origin.x + drag.horizontalTravel) - geo.tick(drag.origin.x);
        const auto wanted = drag.anchorTick + std::clamp(travelled, -drag.anchorTick, limit - drag.anchorTick);
        auto tick = wanted;
        if (travelled != 0 && view.snaps(e.mods)) {
            tick = geo.snap(wanted, false);
            const auto anchor = std::find_if(drag.lanes.begin(), drag.lanes.end(), [&](const auto &lane) {
                return lane.data == drag.anchorData;
            });
            if (anchor != drag.lanes.end()) {
                const auto envelope = envelopeOf(*current, anchor->target);
                if (envelope.data) {
                    const auto x = geo.x(wanted);
                    auto best = static_cast<double>(kAutomationNeighborSnap);
                    const auto &points = envelope.data->points;
                    auto point = std::lower_bound(points.begin(), points.end(), geo.tick(x - best),
                        [](const auto &entry, auto at) { return entry.time < at; });
                    for (; point != points.end() && geo.x(point->time) <= x + kAutomationNeighborSnap; ++point) {
                        const auto distance = std::abs(geo.x(point->time) - x);
                        if (distance < best &&
                            !std::ranges::binary_search(anchor->points, point->id, {}, &AutomationDragPoint::id)) {
                            best = distance;
                            tick = point->time;
                        }
                    }
                }
            }
        }
        const auto timeDelta = std::clamp(tick - drag.anchorTick, minimumTime, maximumTime);
        if (timeDelta != drag.timeDelta) {
            drag.timeDelta = timeDelta;
            updateOverwrittenAutomationPoints(drag, *current);
        }
    }
    // Use the same fine resolution as Draw Mode. Integrating pointer steps
    // keeps the value steady when Shift is pressed or released mid-drag.
    const auto vertical = -static_cast<double>(step.y) / std::max(1.f, drag.height);
    drag.valueDelta = std::clamp(drag.valueDelta + vertical * (e.mods.isShiftDown() ? .1 : 1.),
                                 minimumValue, maximumValue);
    if (!drag.movesSelection && !drag.verticalOnly) {
        const auto &lane = drag.lanes.front();
        const auto envelope = envelopeOf(*current, lane.target);
        if (envelope.lane)
            showAutomationValue(lane.target,
                automationDragValue(*envelope.lane, lane.target, lane.points.front().value, drag.valueDelta),
                e.position);
    }
    repaint();
}

bool ArrangementCanvas::commitAutomationDrag(const AutomationDrag &drag) {
    const auto current = view.root.current();
    if (drag.cancelled || !current || !view.root.automationMode() ||
        drag.generation != view.root.committedGeneration() || (drag.timeDelta == 0 && drag.valueDelta == 0))
        return false;
    std::vector<OpRequest> requests;
    std::vector<TrackEnvelope> envelopes;
    for (const auto &lane : drag.lanes) {
        const auto envelope = envelopeOf(*current, lane.target);
        if (!view.root.automationTargetShown(lane.track, lane.target) || !editableEnvelope(envelope) ||
            !envelope.lane || !envelope.data || envelope.lane->id != lane.lane || envelope.data->id != lane.data) {
            view.root.say("That automation lane changed during the drag.");
            return false;
        }

        if (drag.spliceBounds) {
            const auto [begin, end] = *drag.spliceBounds;
            std::vector<automation::NativeSpliceKnot> knots;
            knots.reserve(lane.points.size());
            for (const auto &point : lane.points) {
                const auto value = automationDragValue(*envelope.lane, lane.target, point.value, drag.valueDelta);
                knots.push_back({{point.time, value, point.tension, point.curve, 0, 0, 0}, {}});
            }
            return view.root.replaceAutomationRange(lane.lane, begin, end, knots, "Move Automation Segment");
        }

        auto points = Payload::array();
        for (const auto &point : lane.points) {
            const auto value = automationDragValue(*envelope.lane, lane.target, point.value, drag.valueDelta);
            if (drag.timeDelta != 0 || value != point.value)
                points.push_back({{"id", point.id}, {"time", point.time + drag.timeDelta}, {"value", value}});
        }
        if (!points.empty()) {
            envelopes.push_back(envelope);
            const char *label = drag.verticalOnly ? "Move Automation Segment"
                               : drag.movesSelection ? "Move Automation Breakpoints" : "Move Automation Point";
            if (!lane.overwritten.empty()) {
                auto remove = automation::deletePoints(lane.data, lane.lane, nullptr, Payload(lane.overwritten));
                remove.label = label;
                requests.push_back(std::move(remove));
            }
            auto move = automation::movePoints(lane.data, lane.lane, nullptr, std::move(points));
            move.label = label;
            requests.push_back(std::move(move));
        }
    }
    if (requests.empty() || !view.root.submitAutomationMove(std::move(requests), envelopes))
        return false;
    if (drag.movesSelection && drag.timeDelta != 0) {
        const auto &selection = view.geometry.selection;
        selectAutomationSpan(selection.start + drag.timeDelta, selection.end + drag.timeDelta);
    }
    return true;
}

std::optional<ArrangementCanvas::AutomationCurvePreview>
ArrangementCanvas::automationCurvePreview(const LaneBody &body, const TrackEnvelope &envelope) const {
    if (!automationCurveDrag_ || view.root.drawMode() || !envelope.lane || !envelope.data)
        return std::nullopt;
    const auto &drag = *automationCurveDrag_;
    if (drag.cancelled || drag.generation != view.root.committedGeneration() ||
        drag.tension == drag.initialTension || drag.left.track != body.track->id ||
        drag.left.target != body.target || drag.left.lane != envelope.lane->id || drag.left.data != envelope.data->id)
        return std::nullopt;
    const auto shape = drag.tension == 0 ? engine::CurveShape::Linear : engine::CurveShape::Bezier;
    return AutomationCurvePreview{drag.left.point, static_cast<std::uint8_t>(shape), drag.tension};
}

void ArrangementCanvas::commitAutomationCurve(const AutomationPick &left, std::uint64_t right,
                                              std::uint8_t shape, float tension) {
    if (!view.root.automationMode() || view.root.drawMode() || !automationPickIsCurrent(left))
        return;
    const auto current = view.root.current();
    const auto envelope = envelopeOf(*current, left.target);
    if (!editableEnvelope(envelope))
        return;
    const auto &points = envelope.data->points;
    const auto point = std::find_if(points.begin(), points.end(), [&](const auto &value) {
        return value.id == left.point;
    });
    if (point == points.end() || std::next(point) == points.end() || std::next(point)->id != right ||
        (point->curve == shape && point->tension == tension))
        return;
    auto request = automation::setCurve(left.data, left.lane, nullptr, Payload::array({left.point}), shape, tension);
    request.label = shape == static_cast<std::uint8_t>(engine::CurveShape::Linear) ? "Straighten Automation Segment"
                                                                                : "Bend Automation Segment";
    if (view.root.submitEdits({std::move(request)}))
        view.changed();
}

void ArrangementCanvas::finishAutomationPointClick(const AutomationPick &pick) {
    if (automationPointClick_ == AutomationPointClick::None || !automationPickIsCurrent(pick))
        return;
    const auto current = view.root.current();
    if (!current)
        return;
    const auto envelope = envelopeOf(*current, pick.target);
    if (!envelope.data)
        return;
    const auto point = std::find_if(envelope.data->points.begin(), envelope.data->points.end(),
                                    [&](const auto &entry) { return entry.id == pick.point; });
    if (point == envelope.data->points.end())
        return;
    const auto tick = point->time;
    if (automationPointClick_ == AutomationPointClick::Delete) {
        chosenPoint_.reset();
        suppressAutomationDoubleClick_ = view.root.deleteAutomationPoint(pick.track, pick.target, pick.point);
        if (suppressAutomationDoubleClick_) {
            focusAutomationKeyboardLane({pick.track, pick.target}, tick);
            clearAutomationValueReadout();
        }
        return;
    }
    focusAutomationKeyboardLane({pick.track, pick.target}, tick, pick);
    chosenPoint_ = pick;
}

bool ArrangementCanvas::automationDown(const juce::MouseEvent &e) {
    // With Draw Mode off, a plain breakpoint click deletes and a drag moves it
    // (Live 12 manual 26.7.1). Alt-click selects it at the insert marker (41.5.2).
    if (!view.root.automationMode() || e.mods.isPopupMenu())
        return false;
    std::int64_t titleTrack = 0;
    ResizeEdge titleEdge = ResizeEdge::None;
    if (barAt(e.x, e.y, titleTrack, titleEdge) != nullptr)
        return false;
    if (view.root.drawMode())
        return beginAutomationStroke(e);
    if (suppressAutomationDoubleClick_ && e.getNumberOfClicks() > 1) {
        ignoringAutomationClick_ = true;
        return true;
    }
    const auto at = e.position;
    const auto current = view.root.current();
    if (!current)
        return false;
    if (const auto hit = automationPointAt(at)) {
        chosenPoint_ = hit;
        const auto mods = e.mods.withoutMouseButtons();
        if (!e.mods.isMiddleButtonDown() && !e.mods.isRightButtonDown()) {
            if (!mods.isAnyModifierKeyDown())
                automationPointClick_ = AutomationPointClick::Delete;
            else if (mods == juce::ModifierKeys(juce::ModifierKeys::altModifier))
                automationPointClick_ = AutomationPointClick::Select;
        }
        if (beginSelectedAutomationDrag(*hit, e))
            return true;
        pointDrag_ = hit;
        pointTick_ = 0;
        if (const auto env = envelopeOf(*current, hit->target); env.data != nullptr)
            for (const auto &point : env.data->points)
                if (point.id == hit->point) {
                    pointTick_ = point.time;
                    pointValue_ = point.value;
                    break;
                }
        repaint();
        return true;
    }
    if (e.mods.withoutMouseButtons() == juce::ModifierKeys(juce::ModifierKeys::altModifier)) {
        if (const auto segment = automationSegmentAt(at, kAutomationCurveHitPixels);
            segment && !segment->held && segment->diagonal) {
            const float tension = segment->curve == static_cast<std::uint8_t>(engine::CurveShape::Bezier)
                                      ? segment->tension : 0.f;
            automationCurveDrag_ = AutomationCurveDrag{segment->left, segment->right.point,
                view.root.committedGeneration(), at.y, tension, tension,
                segment->rightValue > segment->leftValue ? 1.f : -1.f};
            return true;
        }
    }
    if (beginAutomationSegmentDrag(e))
        return true;
    // Shift extends and Ctrl/Cmd picks, as without the mode.
    if (e.mods.isShiftDown() || e.mods.isCommandDown())
        return false;
    // On the line a click adds a breakpoint there, on the line, and its drag moves it on; off the line
    // the lane selects as without the mode (Live 12 manual 25.5.2).
    const auto line = automationLineAt(at);
    if (!line)
        return false;
    // A double-click's second press adds nothing more.
    if (e.getNumberOfClicks() > 1)
        return true;
    const auto track = line->track->id;
    const auto &target = line->target;
    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(at.x));
    const auto tick = view.snaps(e.mods) ? view.geometry.snap(raw, false) : raw;
    const auto env = envelopeOf(*current, target);
    auto value = view.root.automationCurrent(target);
    if (env.data)
        value = envelopeValue(*env.data, tick, target.stepped, current->tempo.get(),
                              view.root.editRate()).value_or(value);
    // The draft is painted with the envelope, and only the final point becomes an edit.
    automationAddDrag_ = AutomationAddDrag{target, track, tick, value, view.root.committedGeneration(),
                                          !editableEnvelope(env)};
    repaint();
    return true;
}
bool ArrangementCanvas::automationAddDragIsCurrent() const {
    return automationAddDrag_ && !automationAddDrag_->cancelled && view.root.automationMode() && !view.root.drawMode() &&
           automationAddDrag_->generation == view.root.committedGeneration() &&
           view.root.automationTargetShown(automationAddDrag_->track, automationAddDrag_->target);
}
void ArrangementCanvas::cancelAutomationAddDrag() {
    if (!automationAddDrag_ || automationAddDrag_->cancelled)
        return;
    automationAddDrag_->cancelled = true;
    clearAutomationValueReadout();
    repaint();
}
void ArrangementCanvas::updateAutomationAddDrag(const juce::MouseEvent &e) {
    if (!automationAddDragIsCurrent()) {
        cancelAutomationAddDrag();
        return;
    }
    auto &drag = *automationAddDrag_;
    for (const auto &body : laneBodies())
        if (body.track->id == drag.track && body.target == drag.target) {
            const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.position.x));
            drag.tick = view.snaps(e.mods) ? view.geometry.snap(raw, false) : raw;
            drag.value = automationValue(e.position.y, automationRange(drag.target), body.top, body.scaleBottom);
            showAutomationValue(drag.target, writtenAutomationValue(drag.target, drag.value), e.position);
            break;
        }
}
std::optional<ArrangementCanvas::AutomationSegmentHit> ArrangementCanvas::automationSegmentAt(
    juce::Point<float> at, float maxDistance) const {
    const auto current = view.root.current();
    if (!current)
        return std::nullopt;
    const auto &geometry = view.geometry;
    std::optional<AutomationSegmentHit> hit;
    for (const auto &body : laneBodies()) {
        if (at.y < body.top || at.y > body.bottom)
            continue;
        const auto envelope = envelopeOf(*current, body.target);
        if (!envelope.data || !envelope.lane || !editableEnvelope(envelope))
            continue;
        const auto &points = envelope.data->points;
        // The horizontal extensions outside the authored knots aren't editable segments.
        const auto right = std::upper_bound(points.begin(), points.end(), at.x, [&](float x, const auto &point) {
            return x < static_cast<float>(geometry.x(point.time));
        });
        if (right == points.begin() || right == points.end())
            continue;
        const auto &leftPoint = *(right - 1), &rightPoint = *right;
        const auto range = automationRange(body.target);
        const AutomationCurveProjection projection{geometry, range, body.top, body.scaleBottom,
            current->tempo.get(), view.root.editRate()};
        const auto from = projection.mark(leftPoint.time, leftPoint.value, leftPoint.curve, leftPoint.tension,
                                          leftPoint.curveSlice ? &*leftPoint.curveSlice : nullptr);
        const auto to = projection.mark(rightPoint.time, rightPoint.value);
        juce::Path path;
        path.startNewSubPath(from.at);
        projection.appendSegment(path, from, to, 0.f, static_cast<float>(getWidth()));
        juce::Point<float> nearest;
        path.getNearestPoint(at, nearest);
        const float distance = nearest.getDistanceFrom(at);
        if (distance > maxDistance || (hit && hit->distance <= distance))
            continue;
        const auto pick = [&](const auto &point) {
            return AutomationPick{body.track->id, point.id, body.target, envelope.lane->id, envelope.data->id};
        };
        hit = AutomationSegmentHit{body, pick(leftPoint), pick(rightPoint), leftPoint.value, rightPoint.value,
            leftPoint.curve, leftPoint.tension, distance, static_cast<std::size_t>(right - points.begin()),
            projection.held(from), from.at.y != to.at.y};
    }
    return hit;
}

std::optional<ArrangementCanvas::LaneBody> ArrangementCanvas::automationLineAt(juce::Point<float> at) const {
    // The lane whose envelope, as paintAutomation draws it, passes within a few pixels of `at`.
    const auto current = view.root.current();
    if (!current)
        return std::nullopt;
    constexpr float kLineHit = 4.f;
    for (const auto &body : laneBodies()) {
        if (at.y < body.top - kLineHit || at.y > body.bottom + kLineHit)
            continue;
        const auto &target = body.target;
        const auto range = automationRange(target);
        const auto env = envelopeOf(*current, target);
        const AutomationCurveProjection projection{view.geometry, range, body.top, body.scaleBottom,
            env.lane && env.lane->timeBase != 0 ? nullptr : current->tempo.get(), view.root.editRate()};
        std::vector<AutomationMark> points;
        if (env.data != nullptr)
            for (const auto &p : env.data->points)
                points.push_back(projection.mark(p.time, p.value, p.curve, p.tension,
                                                 p.curveSlice ? &*p.curveSlice : nullptr));
        if (points.empty())
            points.push_back(projection.mark(0, view.root.automationCurrent(target)));
        juce::Point<float> nearest;
        projection.path(points, 0, static_cast<float>(getWidth())).getNearestPoint(at, nearest);
        if (nearest.getDistanceFrom(at) <= kLineHit)
            return body;
    }
    return std::nullopt;
}
std::vector<ArrangementCanvas::ClipBody> ArrangementCanvas::clipBodies() const {
    // Every clip drawn, where it is drawn: the same walk as the paint, the foot over the tracks
    // scrolled under it.
    std::vector<ClipBody> out;
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    const auto foot = geo.footStart(rows);
    const int footY = geo.footTop(view.reader());
    int top = -geo.scrollY;
    for (const auto row : rows) {
        if (row.position == foot)
            top = footY;
        const auto *t = row.track;
        const int h = geo.heightFor(t->id);
        // A track scrolled under the returns' and Main's foot is cut at its top, as the paint covers
        // it there: nothing under the foot is hit (barAt's rule).
        const float bottom = static_cast<float>(row.position < foot && foot < rows.size() ? std::min(top + h - 3, footY) : top + h - 3);
        const float bodyTop = static_cast<float>(top + 1 + kClipBarHeight);
        if (h > 0 && bottom > bodyTop)
            for (const auto &c : t->clips)
                if (c)
                    out.push_back({t, c.get(), static_cast<float>(geo.x(c->posTicks)),
                                   static_cast<float>(geo.x(c->posTicks + c->lengthTicks)), bodyTop, bottom});
        top += geo.rowHeight(*t);
    }
    return out;
}
bool ArrangementCanvas::fadeHandlesShown(const engine::ClipNode &c) const {
    return c.audio && (showFades || (fadeDrag_ && fadeDrag_->clip == c.id) ||
                       (!view.root.automationMode() && (c.id == hoverClip_ || view.geometry.clipSelected(c.id))));
}
void ArrangementCanvas::fadesOf(const engine::ClipNode &c, engine::ClipFadeHandles &in, engine::ClipFadeHandles &out,
                                std::optional<double> &inTension, std::optional<double> &outTension) const {
    in = fadeHandles(c, true);
    out = fadeHandles(c, false);
    inTension = c.fadeInTension;
    outTension = c.fadeOutTension;
    if (!fadeDrag_)
        return;
    // The drag reaches the dragged clip and, when it is one of them, every other selected clip.
    const bool dragged = fadeDrag_->clip == c.id ||
                         (view.geometry.clipSelected(fadeDrag_->clip) && view.geometry.clipSelected(c.id));
    if (!dragged)
        return;
    if (fadeDrag_->curve)
        (fadeDrag_->in ? inTension : outTension) = fadeTension_;
    else
        (fadeDrag_->in ? in : out) = movedFadeHandle(c, fadeDrag_->in, fadeDrag_->start, fadeTicks_);
}
void ArrangementCanvas::paintFades(juce::Graphics &g, const engine::ClipNode &c, float left, float right, float top,
                                   float bottom) const {
    // Live's fades (the manual's Adjusted Fades figure): the part of the clip the fade takes away
    // darkened above its curve, the curve drawn; with the handles shown, a square at each fade's
    // peak and a diamond on each curve.
    if (bottom - top < 8.f || right - left < 4.f)
        return;
    engine::ClipFadeHandles in, out;
    std::optional<double> inTension, outTension;
    fadesOf(c, in, out, inTension, outTension);
    const auto &geo = view.geometry;
    const auto x = [&](bool incoming, std::int64_t ticks) {
        return static_cast<float>(geo.x(incoming ? c.posTicks + ticks : c.posTicks + c.lengthTicks - ticks));
    };
    const float xIn = x(true, in.peakTicks), xOut = x(false, out.peakTicks);
    const float startIn = x(true, in.zeroTicks), startOut = x(false, out.zeroTicks);
    const auto curveY = [&](double shape) { return bottom - static_cast<float>(shape) * (bottom - top); };
    const auto draw = [&](float x0, float x1, bool fadeIn, std::int64_t curve, const std::optional<double> &tension) {
        if (x1 - x0 < 1.f)
            return;
        juce::Path line, shade;
        const int steps = std::max(2, static_cast<int>((x1 - x0) / 3.f));
        for (int i = 0; i <= steps; ++i) {
            const double u = static_cast<double>(i) / steps;
            const float px = x0 + static_cast<float>(u) * (x1 - x0);
            const float py = curveY(engine::clipFadeShape(fadeIn ? u : 1.0 - u, curve, tension));
            if (i == 0)
                line.startNewSubPath(px, py);
            else
                line.lineTo(px, py);
        }
        shade = line;
        shade.lineTo(x1, top);
        shade.lineTo(x0, top);
        shade.closeSubPath();
        g.setColour(look::ink.withAlpha(0.22f));
        g.fillPath(shade);
        g.setColour(look::ink.withAlpha(0.85f));
        g.strokePath(line, juce::PathStrokeType(1.f));
    };
    const bool bendingIn = fadeDrag_ && fadeDrag_->curve && fadeDrag_->in && inTension != c.fadeInTension;
    const bool bendingOut = fadeDrag_ && fadeDrag_->curve && !fadeDrag_->in && outTension != c.fadeOutTension;
    const auto inCurve = bendingIn ? kBentFadeCurve : c.fadeInCurve, outCurve = bendingOut ? kBentFadeCurve : c.fadeOutCurve;
    g.setColour(look::ink.withAlpha(0.22f));
    if (startIn > left)
        g.fillRect(left, top, startIn - left, bottom - top);
    if (right > startOut)
        g.fillRect(startOut, top, right - startOut, bottom - top);
    draw(startIn, xIn, true, inCurve, inTension);
    draw(xOut, startOut, false, outCurve, outTension);
    if (!fadeHandlesShown(c))
        return;
    const auto &active = fadeDrag_ ? fadeDrag_ : selectedFade_;
    if (active && active->clip == c.id) {
        const auto limit = active->start ? (active->in ? in.peakTicks : out.peakTicks) : fadePeakLimit(c, active->in);
        const float at = x(active->in, limit);
        constexpr float dashes[] = {2.f, 2.f};
        g.setColour(look::ink.withAlpha(.8f));
        g.drawDashedLine({at, top, at, bottom}, dashes, 2);
    }
    const auto square = [&](float horizontal, float y, bool chosen) {
        const auto box = juce::Rectangle<float>(horizontal - 3.5f, y, 7.f, 7.f);
        g.setColour(chosen ? look::handleChosen : look::handle);
        g.fillRect(box);
        g.setColour(look::ink);
        g.drawRect(box, 1.f);
    };
    const auto diamond = [&](float horizontal, float y, bool chosen) {
        juce::Path d;
        d.addQuadrilateral(horizontal, y - 4.f, horizontal + 4.f, y, horizontal, y + 4.f, horizontal - 4.f, y);
        g.setColour(chosen ? look::handleChosen : look::handle);
        g.fillPath(d);
        g.setColour(look::ink);
        g.strokePath(d, juce::PathStrokeType(1.f));
    };
    const auto chosen = [&](bool fadeIn, bool curve, bool start = false) {
        return active && active->clip == c.id && active->in == fadeIn && active->curve == curve && active->start == start;
    };
    square(std::max(left + 3.5f, xIn), top, chosen(true, false));
    square(std::min(right - 3.5f, xOut), top, chosen(false, false));
    if (in.peakTicks > in.zeroTicks) {
        square(std::max(left + 3.5f, startIn), bottom - 7.f, chosen(true, false, true));
        diamond((startIn + xIn) / 2.f, curveY(engine::clipFadeShape(0.5, inCurve, inTension)), chosen(true, true));
    }
    if (out.peakTicks > out.zeroTicks) {
        square(std::min(right - 3.5f, startOut), bottom - 7.f, chosen(false, false, true));
        diamond((xOut + startOut) / 2.f, curveY(engine::clipFadeShape(0.5, outCurve, outTension)), chosen(false, true));
    }
}
std::optional<juce::Point<float>> ArrangementCanvas::fadeHandlePoint(const FadeHandle &h) const {
    for (const auto &b : clipBodies())
        if (b.clip->id == h.clip)
            return fadeHandlePoint(h, b);
    return std::nullopt;
}
std::optional<juce::Point<float>> ArrangementCanvas::fadeHandlePoint(const FadeHandle &h, const ClipBody &b) const {
    const auto &c = *b.clip;
    if (c.id != h.clip || !fadeHandlesShown(c) || b.bottom - b.top < 8.f)
        return std::nullopt;
    engine::ClipFadeHandles in, out;
    std::optional<double> inTension, outTension;
    fadesOf(c, in, out, inTension, outTension);
    const auto handles = h.in ? in : out;
    if ((h.start || h.curve) && handles.peakTicks <= handles.zeroTicks)
        return std::nullopt;
    const auto point = [&](std::int64_t ticks) {
        return static_cast<float>(view.geometry.x(h.in ? c.posTicks + ticks : c.posTicks + c.lengthTicks - ticks));
    };
    const auto peak = point(handles.peakTicks), start = point(handles.zeroTicks);
    if (!h.curve) {
        const auto raw = h.start ? start : peak;
        return juce::Point<float>(h.in ? std::max(b.left + 3.5f, raw) : std::min(b.right - 3.5f, raw),
                                  h.start ? b.bottom - 3.5f : b.top + 3.5f);
    }
    const bool bending = fadeDrag_ && fadeDrag_->curve && fadeDrag_->clip == h.clip && fadeDrag_->in == h.in;
    const auto curve = bending ? kBentFadeCurve : h.in ? c.fadeInCurve : c.fadeOutCurve;
    const auto shape = engine::clipFadeShape(0.5, curve, h.in ? inTension : outTension);
    return juce::Point<float>((start + peak) / 2.f, b.bottom - static_cast<float>(shape) * (b.bottom - b.top));
}
std::optional<ArrangementCanvas::FadeHandle> ArrangementCanvas::fadeHandleAt(int x, int y) const {
    const juce::Point<float> at(static_cast<float>(x), static_cast<float>(y));
    std::optional<FadeHandle> nearest;
    float distance = 6.f;
    for (const auto &b : clipBodies()) {
        if (!fadeHandlesShown(*b.clip))
            continue;
        for (const bool in : {true, false})
            for (const auto &h : {FadeHandle{b.clip->id, b.track->id, in, true},
                                  FadeHandle{b.clip->id, b.track->id, in, false},
                                  FadeHandle{b.clip->id, b.track->id, in, false, true}}) {
                if (const auto p = fadeHandlePoint(h, b); p && p->getDistanceFrom(at) < distance) {
                    distance = p->getDistanceFrom(at);
                    nearest = h;
                }
            }
    }
    return nearest;
}
void ArrangementCanvas::mouseExit(const juce::MouseEvent &) {
    if (automationTransformHover_) {
        automationTransformHover_.reset();
        repaint();
    }
    if(!pointDrag_)clearAutomationValueReadout();
    if (hoverClip_ != 0) {
        hoverClip_ = 0;
        repaint();
    }
}
bool ArrangementCanvas::beginClipRename(std::int64_t clip) {
    // The clip and its track as they are now, not as this window last drew them.
    const auto now = view.root.current();
    if (!now || clip == 0)
        return false;
    const engine::TrackNode *track = nullptr;
    const engine::ClipNode *node = nullptr;
    for (const auto &t : now->tracks)
        for (const auto &c : t->clips)
            if (c->id == clip) {
                track = t.get();
                node = c.get();
            }
    if (track == nullptr || node == nullptr)
        return false;
    // Its title bar where this window draws it, scrolled into sight first.
    auto &geo = view.geometry;
    if (geo.reveal(view.reader(), track->id))
        view.applyView();
    const int top = geo.rowTop(view.reader(), track->id);
    if (top < -kClipBarHeight || top >= getHeight())
        return false;
    const int left = std::max(0, static_cast<int>(geo.x(node->posTicks)));
    const int right = std::min(getWidth(), static_cast<int>(geo.x(node->posTicks + node->lengthTicks)));
    renamingClip_ = clip;
    clipEditor.setBounds(left, top + 1, std::max(120, right - left), 20);
    clipEditor.setText(juce::String::fromUTF8(node->name.c_str()), false);
    clipEditor.selectAll();
    clipEditor.setVisible(true);
    if (clipEditor.isShowing())
        focusIfShowing(clipEditor);
    repaint();
    return true;
}
bool ArrangementCanvas::commitClipRename() {
    const auto id = renamingClip_;
    if (id == 0)
        return false;
    const auto text = clipEditor.getText().trim().toStdString();
    const auto now = view.root.current();
    if (!now)
        return false;
    for (const auto &t : now->tracks)
        for (const auto &c : t->clips)
            if (c->id == id)
                return c->name == text || view.root.submitEdit("clip.setName", {{"id", id}, {"name", text}}, "Rename Clip");
    return false; // gone while it was being renamed
}
void ArrangementCanvas::endClipRename() {
    renamingClip_ = 0;
    clipEditor.setVisible(false);
    if (isShowing())
        focusIfShowing(*this);
    repaint();
}
void ArrangementCanvas::focusLost(FocusChangeType) {
    cancelAutomationAddDrag();
    clearReleasedFadeHandles();
}
void ArrangementCanvas::clearReleasedFadeHandles() {
    if (showFades && !view.root.input.down('f')) {
        showFades = false;
        repaint();
    }
}
bool ArrangementCanvas::keyStateChanged(bool) {
    clearReleasedFadeHandles();
    return false;
}
void ArrangementCanvas::paintWaveform(juce::Graphics &g, const engine::ClipNode &clip, std::int64_t pos, float left,
                                      float right, float top, float bottom) const {
    // Live's audio clip (manual 6.1, chapter 9): its waveform across the body, each channel in a
    // band of its own, inside the clip's source window only. The colour is ADI's until measured.
    if (!clip.audio || bottom - top < 4.f)
        return;
    const auto peaks = view.waveforms != nullptr ? view.waveforms->peaks(*clip.audio) : nullptr;
    const auto *finest = peaks ? &peaks->finest() : nullptr;
    if (!clip.loopEnabled && (!finest || finest->channels == 0))
        return;
    const auto rate = finest ? static_cast<double>(finest->sampleRate)
                             : clip.audio->mediaInfo ? static_cast<double>(clip.audio->mediaInfo->sampleRate.value_or(0)) : 0.;
    if (!(rate > 0.))
        return;
    const auto *snapshot = view.reader().get();
    const auto &geo = view.geometry;
    auto moved = clip;
    moved.posTicks = pos; // drawn where a drag would put it
    const ClipSourceMap source(moved, snapshot != nullptr ? snapshot->tempo.get() : nullptr, rate);
    const auto gain = std::abs(clip.gainDb) <= 120.f ? std::pow(10.f, clip.gainDb / 20.f) * view.waveformZoomScale() : 0.f;
    const auto waveColour = look::ink.withAlpha(clip.muted ? 0.4f : 0.85f);
    g.setColour(waveColour);
    const int from = std::max(0, static_cast<int>(std::floor(left))),
              to = std::min(getWidth(), static_cast<int>(std::ceil(right)));
    if (!clip.loopEnabled) {
        paintPeakColumns(g, *peaks, from, to, top, bottom, finest->channels, {},
                         [&](int x) { return source.frameAt(geo.tick(x)); }, static_cast<double>(finest->frames), gain);
        return;
    }
    const auto channels = finest ? finest->channels : 0;
    const float band = channels > 0 ? (bottom - top) / static_cast<float>(channels) : 0.f;
    for (int x = from; x < to; ++x) {
        const double rightEdge = std::min(static_cast<double>(x + 1), static_cast<double>(right));
        const auto column = source.framesAcross(geo.tick(x), geo.tick(rightEdge));
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            auto span = peakSpan(*peaks, column.first.from, column.first.to, ch);
            if (column.repeats) {
                if (const auto wrapped = peakSpan(*peaks, column.second.from, column.second.to, ch)) {
                    if (span)
                        mergePeakBins(*span, *wrapped);
                    else
                        span = wrapped;
                }
            }
            if (!span)
                continue;
            const float mid = top + band * (static_cast<float>(ch) + .5f), half = band * .5f;
            const float y0 = mid - std::clamp(span->maximum * gain, -1.f, 1.f) * half,
                        y1 = mid - std::clamp(span->minimum * gain, -1.f, 1.f) * half;
            g.fillRect(static_cast<float>(x), y0, 1.f, std::max(1.f, y1 - y0));
        }
        // At overview scales several short repeats can share one pixel and one boundary mark.
        if (column.repeats && x + 1 < to) {
            paintLoopTurn(g, x + 1, top, bottom, clip.muted);
            g.setColour(waveColour);
        }
    }
}
void ArrangementCanvas::paintLiveMidiTake(juce::Graphics &g, const LiveMidiTake &take, const engine::TrackNode &track,
                                         std::size_t trackNumber, int y, int height) const {
    const auto &geo = view.geometry;
    const double x0 = geo.x(take.start), x1 = geo.x(take.end);
    if (take.end <= take.start || x1 < 0 || x0 > getWidth())
        return;
    const float left = static_cast<float>(std::max(-1., x0));
    const float right = static_cast<float>(std::min(x1, static_cast<double>(getWidth() + 1)));
    const float top = static_cast<float>(y + 1), width = std::max(1.f, right - left);
    g.setColour(look::record);
    g.fillRect(left, top, width, static_cast<float>(height - 4));
    g.setColour(look::record.darker(0.3f));
    g.fillRect(left, top, width, static_cast<float>(kClipBarHeight));
    if (right - left > 6.f) {
        g.setColour(look::ink);
        g.setFont(look::font(look::fontSmall));
        g.drawText(juce::String::fromUTF8(displayName(track.name, trackNumber).c_str()),
                   juce::Rectangle<float>(left + 3.f, top, right - left - 5.f, static_cast<float>(kClipBarHeight)),
                   juce::Justification::centredLeft, false);
    }
    const float noteTop = top + static_cast<float>(kClipBarHeight);
    const float noteHeight = static_cast<float>(y + height - 3) - noteTop;
    if (!take.notes.empty() && right > left && noteHeight >= 4.f) {
        const auto [low, high] = std::minmax_element(take.notes.begin(), take.notes.end(),
                                                    [](const LiveMidiNote &a, const LiveMidiNote &b) {
                                                        return a.key < b.key;
                                                    });
        const int range = high->key - low->key + 1;
        const int keys = std::max(12, range), lowest = low->key - (keys - range) / 2;
        const float inner = noteHeight - 2.f;
        const float pitch = inner / static_cast<float>(keys);
        const float thick = std::clamp(pitch - (pitch >= 3.f ? 1.f : 0.f), 1.f, 4.f);
        g.setColour(look::ink.withAlpha(0.9f));
        for (const auto &note : take.notes) {
            const float start = std::max(left, static_cast<float>(geo.x(note.start)));
            const float end = std::min(right, static_cast<float>(geo.x(note.end)));
            if (end <= start)
                continue;
            const float noteY = noteTop + 1.f + inner - (static_cast<float>(note.key - lowest) + 1.f) * pitch +
                                (pitch - thick) * .5f;
            g.fillRect(start, noteY, std::min(std::max(1.f, end - start), right - start), thick);
        }
    }
    g.setColour(look::record.darker(0.5f));
    g.drawRect(left, top, width, static_cast<float>(height - 4), 1.f);
}
void ArrangementCanvas::paintNotes(juce::Graphics &g, const engine::ClipNode &clip, std::int64_t pos,
                                   std::int64_t clipEnd, float left, float right, float top, float bottom) const {
    if (bottom <= top || right <= left || !clip.midiPreview)
        return;
    const auto &held = *clip.midiPreview;
    const auto &geo = view.geometry;
    const auto visibleStart = std::max(pos, geo.tick(left));
    const auto visibleEnd = std::min(clipEnd, geo.tick(right) + 1);
    const auto period = held.loopLength;
    const auto offset = held.startMarker;
    const auto loopStart = held.loopStart;
    const auto loopEnd = loopStart + period;

    // The same source phase as MIDI playback: a lead-in plays once, or we
    // enter partway through the loop if the start marker is inside it.
    const auto firstRepeat = period <= 0 ? 0 : offset < loopStart ? loopEnd - offset : -((offset - loopStart) % period);
    // Below pixel resolution, one occurrence per column suffices. Begin at
    // the visible repeat so a long clip can be scrolled without replaying its prefix.
    const auto stride = period > 0
                            ? std::max<std::int64_t>(1, (geo.tick(left + 1.) - geo.tick(left)) / period)
                            : 1;
    const auto firstVisiblePass = [&](std::int64_t start) {
        return visibleStart > start && period > 0 ? (visibleStart - start) / period : 0;
    };
    if (period > 0) {
        const auto firstTurn = pos + (firstRepeat > 0 ? firstRepeat : firstRepeat + period);
        for (auto turn = firstTurn + firstVisiblePass(firstTurn) * period; turn < visibleEnd; turn += period * stride) {
            const int x = juce::roundToInt(geo.x(turn));
            if (static_cast<float>(x) > left && static_cast<float>(x) < right)
                paintLoopTurn(g, x, top, bottom, clip.muted);
        }
    }
    if (bottom - top < 4.f || held.bars.empty())
        return;

    // The keys in use over the body's height, never fewer than an octave so one note is not a slab.
    const int keys = std::max(12, held.high - held.low + 1);
    const int lowest = held.low - (keys - (held.high - held.low + 1)) / 2;
    const float inner = bottom - top - 2.f;
    const float pitch = inner / static_cast<float>(keys);
    const float thick = std::clamp(pitch - (pitch >= 3.f ? 1.f : 0.f), 1.f, 4.f);
    auto bar = [&](std::int64_t start, std::int64_t end, const engine::MidiClipPreview::Bar &n) {
        if (end <= std::max(start, visibleStart) || start >= visibleEnd)
            return;
        const float x0 = std::max(left, static_cast<float>(geo.x(std::max(start, pos)))),
                    x1 = std::min(right, static_cast<float>(geo.x(std::min(end, clipEnd))));
        if (x1 <= x0)
            return;
        const float y = top + 1.f + inner - (static_cast<float>(n.key - lowest) + 1.f) * pitch + (pitch - thick) * .5f;
        g.setColour(look::ink.withAlpha(clip.muted || !n.active ? 0.35f : 0.9f));
        g.fillRect(x0, y, std::min(std::max(1.f, x1 - x0), right - x0), thick);
    };
    for (const auto &n : held.bars) {
        const auto noteEnd = n.start + n.duration;
        if (period <= 0 || offset < loopStart)
            bar(pos + n.start - offset, pos + (period > 0 ? std::min(noteEnd, loopEnd) : noteEnd) - offset, n);
        if (period > 0 && n.start < loopEnd && noteEnd > loopStart) {
            const auto start = pos + firstRepeat + std::max(n.start, loopStart) - loopStart;
            const auto duration = std::min(noteEnd, loopEnd) - std::max(n.start, loopStart);
            // Keep one prior pass in case its tail crosses the viewport's left edge.
            const auto first = start + std::max<std::int64_t>(0, firstVisiblePass(start) - 1) * period;
            for (auto at = first; at < visibleEnd; at += period * stride)
                bar(at, at + duration, n);
        }
    }
}
void ArrangementCanvas::paintGroupLane(juce::Graphics &g, std::size_t at, int y, int h, juce::Colour colour) const {
    // Live's group track (manual 18.3, its figure of a folded group): a darker lane, and in it an
    // overview of the clips of every track the group holds, at any depth, a thin row each, in
    // the group's colour. Folded or not.
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    g.setColour(look::ink.withAlpha(0.16f));
    g.fillRect(0, y, getWidth(), h - 2);
    const auto depth = rows[at].place.depth;
    std::size_t held = 0;
    for (auto i = at + 1; i < rows.size() && rows[i].place.depth > depth; ++i)
        if (!rows[i].place.group)
            ++held;
    if (held == 0)
        return;
    const float pitch = std::min(8.f, static_cast<float>(h - 6) / static_cast<float>(held));
    const float bar = pitch >= 4.f ? pitch - 2.f : std::max(1.f, pitch);
    float top = static_cast<float>(y) + 3.f;
    g.setColour(colour.brighter(0.2f));
    for (auto i = at + 1; i < rows.size() && rows[i].place.depth > depth; ++i) {
        if (rows[i].place.group)
            continue;
        for (const auto &c : rows[i].track->clips) {
            const double x = geo.x(c->posTicks), end = geo.x(c->posTicks + c->lengthTicks);
            if (end < 0 || x > getWidth())
                continue;
            const float left = static_cast<float>(std::max(-1., x));
            g.fillRect(left, top, std::max(1.f, static_cast<float>(std::min(end, getWidth() + 1.)) - left), bar);
        }
        top += pitch;
    }
}
void ArrangementCanvas::mouseDown(const juce::MouseEvent &e) {
    focusIfShowing(*this);
    if (e.getNumberOfClicks() == 1)
        emptyTrackDoubleClicked_ = false;
    emptyTrackPress_ = e.mods == juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier) &&
                       !(view.root.automationMode() && view.root.drawMode()) && inEmptyTrackArea(e.getPosition());
    emptyTrackClick_ = emptyTrackPress_ && (e.getNumberOfClicks() == 1 || emptyTrackClick_);
    // An extra press in the same click series can now land on the track just added.
    if (emptyTrackPress_ || emptyTrackDoubleClicked_)
        return;
    if (view.loopSelected || view.selectedLocator != 0) {
        view.loopSelected = false; // a click in the lanes lets the brace and a locator go
        view.selectedLocator = 0;
        view.scrub.repaint();
        view.repaint();
    }
    if (view.selectedMeter >= 0) {
        view.selectedMeter = -1; // a click in the lanes lets a selected marker go
        view.scrub.repaint();
    }
    last = e.position;
    moving_ = resizing_ = 0;
    rangeDrag_.reset();
    copying_ = false;
    draggingTimeRange_ = false;
    keyResize_ = false; // a click ends a resize begun from the keyboard, as Esc does
    clipTimeBounds_.reset();
    selectedFade_.reset(); // a click chooses a fade handle again, or nothing
    clearReleasedFadeHandles();
    panning = e.mods.isCommandDown() && e.mods.isAltDown();
    automationStroke_.reset();
    automationDrag_.reset();
    automationCurveDrag_.reset();
    automationTransformDrag_.reset();
    pointDrag_.reset();
    automationPointClick_ = AutomationPointClick::None;
    ignoringAutomationClick_ = false;
    if (e.getNumberOfClicks() == 1)
        suppressAutomationDoubleClick_ = false;
    chosenPoint_.reset(); // chosen again below when the press is on a breakpoint
    automationAddDrag_.reset();
    keyboardAutomationLane_.reset();
    keyboardAutomationFocus_.reset();
    keyboardAutomationDraft_.reset();
    contextMenuPress_ = e.mods.isPopupMenu();
    if (!contextMenuPress_)
        view.followPaused = false; // a primary press in the Arrangement starts Follow again (§6.2)
    if (const auto take = view.geometry.takeLaneAt(view.reader(), e.y); take && !panning) {
        clearAutomationSelection();
        takeAnchor_ = view.geometry.tick(e.x);
        const auto &selection = view.takeSelection;
        const bool keepRange = contextMenuPress_ && selection && selection->track == take->track->id &&
                               selection->lane == take->lane->id &&
                               takeAnchor_ >= std::min(selection->start, selection->end) &&
                               takeAnchor_ < std::max(selection->start, selection->end);
        if (!keepRange) {
            view.takeSelection = ArrangementView::TakeSelection{take->track->id, take->lane->id, takeAnchor_, takeAnchor_};
            for (const auto &clip : take->track->takeClips)
                if (clip->laneId == take->lane->id && takeAnchor_ >= clip->posTicks &&
                    takeAnchor_ - clip->posTicks < clip->lengthTicks) {
                    view.takeSelection->start = clip->posTicks;
                    view.takeSelection->end = clip->posTicks + clip->lengthTicks;
                }
        }
        draggingTakeSelection_ = !contextMenuPress_;
        if (contextMenuPress_) {
            const auto track = take->track->id;
            const auto lane = take->lane->id;
            juce::PopupMenu menu;
            menu.addItem("Audition Take Lane", !take->lane->muted, view.takeAuditioned(track, lane),
                [safe = SafePointer<ArrangementCanvas>(this), track, lane] {
                    if (safe)
                        safe->view.toggleTakeAudition(track, lane);
                });
            menu.addItem("Copy Selection to Main Lane",
                view.takeSelection && view.takeSelection->start != view.takeSelection->end, false,
                [safe = SafePointer<ArrangementCanvas>(this)] {
                    if (safe)
                        safe->view.copyTakeSelectionToMain();
                });
            view.presentContextMenu(localPointToGlobal(e.getPosition()), false, nullptr, std::move(menu));
        }
        repaint();
        return;
    }
    view.takeSelection.reset();
    // F exposes clip fade handles above the automation controls.
    const auto fade = !panning && !contextMenuPress_ ? fadeHandleAt(e.x, e.y) : std::nullopt;
    if (!panning && !fade && beginAutomationTransform(e)) {
        clearAutomationValueReadout();
        return;
    }
    if (!panning && contextMenuPress_ && view.root.automationMode()) {
        if (const auto hit = automationPointAt(e.position)) {
            const auto &selection = view.geometry.selection;
            const auto [begin, end] = std::minmax(selection.start, selection.end);
            const auto at = view.geometry.tick(e.position.x);
            if (!hasAutomationSelection() || at < begin || at >= end ||
                std::find(automationSelectionTargets_.begin(), automationSelectionTargets_.end(),
                          AutomationLaneKey{hit->track, hit->target}) == automationSelectionTargets_.end())
                clearAutomationSelection();
            automationContextMenu(e);
            return;
        }
    }
    if (!panning && !fade && automationDown(e)) {
        if (!automationDrag_)
            clearAutomationSelection();
        if(const auto point=automationValueAt(e.position,false,false))
            showAutomationValue(point->target,point->value,e.position);
        else if (automationAddDragIsCurrent())
            showAutomationValue(automationAddDrag_->target, writtenAutomationValue(automationAddDrag_->target,
                                automationAddDrag_->value), e.position);
        else if(pointDrag_)
            showAutomationValue(pointDrag_->target,pointValue_,e.position);
        return;
    }
    clearAutomationValueReadout();
    if (!panning && !fade && automationSelectionDown(e))
        return;
    if (contextMenuPress_) {
        view.select(e.x, e.y, view.snaps(e.mods), ArrangementView::SelectionPurpose::ContextMenu);
        // In Automation Mode the menu is the envelope's too: its lane's Re-Enable Automation (AU-04).
        view.contextMenu(e.getScreenPosition(), false, view.automationLaneAt(e.y));
        return;
    }
    clearAutomationSelection();
    if (!panning) {
        // Held still at the lanes' edge, a drag repeats, so they keep scrolling.
        beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
        // A fade's handle (manual 6.8): its length or its curve, the clip chosen with it.
        if (const auto &h = fade) {
            for (const auto &b : clipBodies())
                if (b.clip->id == h->clip) {
                    auto &sel = view.geometry.selection;
                    if (!view.geometry.clipSelected(h->clip)) {
                        sel.track = h->track;
                        sel.toTrack = 0;
                        sel.picked.clear();
                        sel.clip = h->clip;
                        sel.pickedClips.clear();
                        sel.start = b.clip->posTicks;
                        sel.end = b.clip->posTicks + b.clip->lengthTicks;
                    }
                    fadeDrag_ = h;
                    selectedFade_ = h;
                    const auto handles = fadeHandles(*b.clip, h->in);
                    fadeTicks_ = h->start ? handles.zeroTicks : handles.peakTicks;
                    fadeTensionFrom_ = fadeTension_ = (h->in ? b.clip->fadeInTension : b.clip->fadeOutTension).value_or(0.0);
                    fadeFromY_ = e.y;
                }
            view.changed();
            return;
        }
        // The clip's title bar moves it; either edge of the bar changes its length (§6.7).
        std::int64_t track = 0;
        auto edge = ResizeEdge::None;
        const auto *barClip = barAt(e.x, e.y, track, edge);
        if (barClip && e.mods.isShiftDown() && e.mods.isAltDown() && !e.mods.isCommandDown()) {
            // The title drag makes a range inside this clip, even when the pointer leaves it (§42.16).
            const auto begin = barClip->posTicks, end = begin + barClip->lengthTicks;
            clipTimeBounds_ = std::pair{begin, end};
            auto &geo = view.geometry;
            auto &sel = geo.selection;
            const auto raw = geo.tick(e.x);
            const auto snapped = geo.grid.snap ? geo.snap(raw, false) : raw;
            sel.track = track;
            sel.toTrack = sel.clip = 0;
            sel.picked.clear();
            sel.pickedClips.clear();
            sel.marked = true;
            sel.insert = sel.start = sel.end = std::clamp(snapped, begin, end);
            view.locate();
            view.changed();
            return;
        }
        if (const auto *c = barClip; c != nullptr && edge != ResizeEdge::None) {
            view.geometry.selection.track = track;
            view.geometry.selection.toTrack = 0;
            view.geometry.selection.picked.clear();
            view.geometry.selection.clip = c->id;
            view.geometry.selection.pickedClips.clear();
            view.geometry.selection.start = c->posTicks;
            view.geometry.selection.end = c->posTicks + c->lengthTicks;
            beginResize(*c, track, edge, e.mods.isShiftDown());
            view.changed();
            return;
        }
        // Ctrl/Cmd+click on another clip picks it besides the selected one, or lets it go again
        // (Live's "Select Multiple Items", §42.5). A plain click lets them all go. On its bar the
        // press also begins a drag of every selected clip, which Ctrl copies (mouseUp); a picked
        // clip is let go only when released without one.
        selectOnUp_ = false;
        toggleOnUp_ = 0;
        if (e.mods.isCommandDown() && view.geometry.selection.clip != 0)
            if (const auto *t = view.geometry.trackAt(view.reader(), e.y))
                if (const auto *c = view.geometry.hit(*t, view.geometry.tick(e.x));
                    c != nullptr && c->id != view.geometry.selection.clip) {
                    const auto *bar = barAt(e.x, e.y, track, edge);
                    const bool onBar = bar != nullptr && bar->id == c->id;
                    auto &picked = view.geometry.selection.pickedClips;
                    if (const auto at = std::find(picked.begin(), picked.end(), c->id); at == picked.end())
                        picked.push_back(c->id);
                    else if (onBar)
                        toggleOnUp_ = c->id;
                    else
                        picked.erase(at);
                    if (onBar) {
                        moving_ = c->id;
                        movingFrom_ = movingTo_ = c->posTicks;
                        movingSource_ = movingTrack_ = track;
                        movingAcross_.clear();
                        grabTicks_ = view.geometry.tick(e.x) - c->posTicks;
                        pressedAt_ = e.getPosition();
                    }
                    view.changed();
                    return;
                }
        // A plain press on the bar of one of several selected clips keeps them all for the drag,
        // which moves them together; released without a drag, it selects that clip alone.
        if (!e.mods.isShiftDown() && !view.geometry.selection.pickedClips.empty())
            if (const auto *c = barAt(e.x, e.y, track, edge); c != nullptr && view.geometry.clipSelected(c->id)) {
                moving_ = c->id;
                movingFrom_ = movingTo_ = c->posTicks;
                movingSource_ = movingTrack_ = track;
                movingAcross_.clear();
                grabTicks_ = view.geometry.tick(e.x) - c->posTicks;
                selectOnUp_ = true;
                pressedAt_ = e.getPosition();
                return;
            }
        // Shift extends the selection, in the same track or across tracks (Live 12 manual 6.9).
        if (e.mods.isShiftDown() && view.geometry.selection.marked) {
            extendTo(e.x, e.y, view.snaps(e.mods));
            return;
        }
        const auto *bar = barAt(e.x, e.y, track, edge);
        const auto &sel = view.geometry.selection;
        const auto [start, end] = std::minmax(sel.start, sel.end);
        const auto at = view.geometry.tick(e.x);
        // A clip title inside a time selection moves the selected material on its existing tracks.
        // A click without a drag still selects the single clip, and modified presses work as before.
        if (bar != nullptr && !e.mods.isAnyModifierKeyDown() && !hasAutomationSelection() &&
            sel.clip == 0 && sel.pickedClips.empty() &&
            at >= start && at < end && view.geometry.inSelection(view.reader(), track)) {
            rangeDrag_ = RangeDrag{sel, start};
            grabTicks_ = at - start;
            pressedAt_ = e.getPosition();
            return;
        }
        using Purpose = ArrangementView::SelectionPurpose;
        const auto purpose = bar != nullptr ? Purpose::Edit : Purpose::InsertMarker;
        view.select(e.x, e.y, view.snaps(e.mods), purpose);
        if (bar != nullptr && bar->id == view.geometry.selection.clip) {
            moving_ = bar->id;
            movingFrom_ = movingTo_ = bar->posTicks;
            movingSource_ = movingTrack_ = track;
            grabTicks_ = view.geometry.tick(e.x) - bar->posTicks;
        }
    }
}
std::int64_t ArrangementCanvas::movingTargetOf(std::int64_t clip) const noexcept {
    // 0: drawn on its own track.
    if (moving_ == 0)
        return 0;
    if (view.geometry.selection.pickedClips.empty())
        return clip == moving_ ? movingTrack_ : 0;
    for (const auto &[id, target] : movingAcross_)
        if (id == clip)
            return target;
    return 0;
}
const engine::ClipNode *ArrangementCanvas::barAt(int x, int y, std::int64_t &track, ResizeEdge &edge) const {
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    const auto foot = geo.footStart(rows);
    const int footY = geo.footTop(view.reader());
    // A point in the foot is the foot's, never a track's scrolled under it.
    const bool inFoot = foot < rows.size() && y >= footY;
    int top = -geo.scrollY;
    for (const auto row : rows) {
        if (row.position == foot)
            top = footY;
        const auto *t = row.track;
        const bool covered = inFoot && row.position < foot;
        if (!covered && geo.shown(t->id) && y >= top + 1 && y < top + 1 + kClipBarHeight) {
            const engine::ClipNode *outside = nullptr;
            for (const auto &c : t->clips) {
                const int left = static_cast<int>(geo.x(c->posTicks));
                const int right = static_cast<int>(geo.x(c->posTicks + c->lengthTicks));
                if (x >= left && x < right) {
                    const auto fromLeft = x - left, fromRight = right - x;
                    edge = ResizeEdge::None;
                    if (fromLeft < kClipEdge && fromLeft < fromRight)
                        edge = ResizeEdge::Left;
                    else if (fromRight <= kClipEdge)
                        edge = ResizeEdge::Right;
                    track = t->id;
                    return c.get();
                }
                if (x >= right && x < right + kClipEdge / 2)
                    outside = c.get();
            }
            if (outside) {
                track = t->id;
                edge = ResizeEdge::Right;
                return outside;
            }
        }
        top += geo.rowHeight(*t);
    }
    return nullptr;
}
void ArrangementCanvas::mouseMove(const juce::MouseEvent &e) {
    if (showFades)
        if (const auto fade = fadeHandleAt(e.x, e.y)) {
            clearAutomationValueReadout();
            setMouseCursor(fade->curve ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
            return;
        }
    const auto bounds = automationTransformBounds();
    const bool wasHovering = automationTransformHover_.has_value();
    if (bounds && e.position.y <= bounds->visibleBottom + kAutomationTransformHandleHit &&
        automationTransformBox(*bounds).expanded(kAutomationTransformHandleHit).contains(e.position))
        automationTransformHover_ = e.position;
    else
        automationTransformHover_.reset();
    if (wasHovering || automationTransformHover_)
        repaint();
    if (bounds)
        if (const auto handle = automationTransformHandleAt(*bounds, e.position)) {
            const bool horizontal = *handle == AutomationTransformHandle::Left || *handle == AutomationTransformHandle::Right;
            clearAutomationValueReadout();
            setMouseCursor(horizontal ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
            return;
        }
    if(const auto point=automationValueAt(e.position,false,false))
        showAutomationValue(point->target,point->value,e.position);
    else clearAutomationValueReadout();
    clearReleasedFadeHandles();
    if (view.root.automationMode() && view.root.drawMode()) {
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
        return;
    }
    // The audio clip under the mouse shows its fade handles (manual 6.8).
    std::int64_t over = 0;
    for (const auto &b : clipBodies())
        if (const auto mx = static_cast<float>(e.x), my = static_cast<float>(e.y);
            b.clip->audio && mx >= b.left && mx < b.right && my >= b.top - kClipBarHeight && my <= b.bottom)
            over = b.clip->id;
    if (over != hoverClip_) {
        hoverClip_ = over;
        repaint();
    }
    if (const auto h = fadeHandleAt(e.x, e.y)) {
        setMouseCursor(h->curve ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    std::int64_t track = 0;
    auto edge = ResizeEdge::None;
    const auto *c = barAt(e.x, e.y, track, edge);
    setMouseCursor(c != nullptr && edge != ResizeEdge::None ? juce::MouseCursor::LeftRightResizeCursor
                                                             : juce::MouseCursor::NormalCursor);
}
void ArrangementCanvas::mouseDrag(const juce::MouseEvent &e) {
    if (emptyTrackPress_ || emptyTrackDoubleClicked_ || contextMenuPress_ || ignoringAutomationClick_ || e.mods.isPopupMenu())
        return;
    if (draggingTakeSelection_) {
        if (view.takeSelection && e.mouseWasDraggedSinceMouseDown()) {
            view.takeSelection->start = std::max<std::int64_t>(0, takeAnchor_);
            view.takeSelection->end = std::max<std::int64_t>(0, view.geometry.tick(e.x));
            repaint();
        }
        return;
    }
    if (automationTransformDrag_) {
        updateAutomationTransform(e);
        return;
    }
    if (automationCurveDrag_) {
        auto &drag = *automationCurveDrag_;
        if (!drag.cancelled) {
            const auto delta = (e.position.y - drag.originY) / kCurveTensionDragPixels;
            drag.tension = std::clamp(drag.initialTension + delta * drag.direction, -1.f, 1.f);
            repaint();
        }
        return;
    }
    if (automationStroke_) {
        if (view.root.automationMode() && view.root.drawMode())
            updateAutomationStroke(e);
        return;
    }
    if (automationDrag_) {
        updateAutomationDrag(e);
        return;
    }
    if (automationAddDrag_) {
        updateAutomationAddDrag(e);
        repaint();
        return;
    }
    if (pointDrag_) {
        if (e.position.getDistanceFrom(e.mouseDownPosition) >= kAutomationDragSlop &&
            beginSingleAutomationDrag(*pointDrag_, e)) {
            pointDrag_.reset();
            updateAutomationDrag(e);
        }
        repaint();
        return;
    }
    if (fadeDrag_) {
        // A peak's square follows the mouse along the clip; a curve's diamond, up and down.
        if (fadeDrag_->curve) {
            const double bend = (e.y - fadeFromY_) / 60.0;
            fadeTension_ = std::clamp(fadeTensionFrom_ + (e.mods.isAltDown() ? -bend : bend), -1.0, 1.0);
        } else {
            for (const auto &b : clipBodies())
                if (b.clip->id == fadeDrag_->clip) {
                    const auto at = view.geometry.tick(e.x);
                    const auto ticks = fadeDrag_->in ? at - b.clip->posTicks : b.clip->posTicks + b.clip->lengthTicks - at;
                    const auto before = fadeHandles(*b.clip, fadeDrag_->in);
                    const auto requested = !fadeDrag_->start && before.zeroTicks > 0 ? std::max(ticks, before.zeroTicks + 1) : ticks;
                    const auto next = movedFadeHandle(*b.clip, fadeDrag_->in, fadeDrag_->start, requested);
                    fadeTicks_ = fadeDrag_->start ? next.zeroTicks : next.peakTicks;
                }
        }
        repaint();
        return;
    }
    // JUCE's held-drag timer can dispatch at the press point before any motion.
    if (e.mouseWasDraggedSinceMouseDown() && e.position != e.mouseDownPosition)
        draggingTimeRange_ = true;
    if (clipTimeBounds_) {
        if (!draggingTimeRange_)
            return;
        view.edgeScroll(e.x, e.y, true, false);
        auto &geo = view.geometry;
        const auto raw = geo.tick(e.x);
        const auto snapped = geo.grid.snap ? geo.snap(raw, raw >= geo.selection.start) : raw;
        geo.selection.end = std::clamp(snapped, clipTimeBounds_->first, clipTimeBounds_->second);
        view.frame(true);
        return;
    }
    if (resizing_ != 0) {
        // Follow the mouse to the nearest grid line while snapping. The other edge stays anchored.
        const bool scrolled = e.mouseWasDraggedSinceMouseDown() && view.edgeScroll(e.x, e.y, true, false);
        const auto step = static_cast<std::int64_t>(view.geometry.gridQuarters() * textproj::kPPQ);
        const auto raw = view.geometry.tick(e.x);
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        const bool snaps = view.snaps(e.mods);
        const auto tick = !snaps ? raw : (raw - down <= up - raw ? down : up);
        resizeTo(tick, snaps || resizeEdge_ == ResizeEdge::Right ? std::max<std::int64_t>(1, step) : 1);
        if (scrolled)
            view.frame(true);
        repaint();
        return;
    }
    if (rangeDrag_) {
        if (!rangeDrag_->cancelled && draggingTimeRange_) {
            const bool scrolled = view.edgeScroll(e.x, e.y, true, false);
            const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.x) - grabTicks_);
            const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
            rangeDrag_->to = !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up);
            if (scrolled)
                view.frame(true);
            repaint();
        }
        return;
    }
    if (moving_ != 0) {
        // The clip's start follows the mouse, to the nearest grid line while snapping, and its
        // track follows the lane under it when that track can take it (Live 12 manual 6.7);
        // over one that cannot, it stays on the last that could. At the lanes' edge they scroll.
        const bool scrolled = e.mouseWasDraggedSinceMouseDown() && view.edgeScroll(e.x, e.y, true);
        if (e.mouseWasDraggedSinceMouseDown()) {
            selectOnUp_ = false;
            toggleOnUp_ = 0;
        }
        // Ctrl held (Option on the Mac) makes the drag a copy, the originals staying, as Live's and
        // the Note Editor's; it can be pressed or let go during the drag.
#if JUCE_MAC
        copying_ = e.mods.isAltDown();
#else
        copying_ = e.mods.isCtrlDown();
#endif
        // Several selected clips move together: in time, and across tracks each by the same rows
        // when every one lands on a track that can take it; else they stay on the last rows that
        // could.
        const bool several = !view.geometry.selection.pickedClips.empty();
        if (const auto *over = view.geometry.trackAt(view.reader(), e.y); over != nullptr && !several) {
            if (const auto *source = view.reader().findTrack(movingSource_);
                source != nullptr && over->id != movingTrack_ && canTakeClip(*over, *source))
                movingTrack_ = over->id;
        } else if (over != nullptr) {
            std::vector<std::int64_t> lanes;
            std::ptrdiff_t from = -1, to = -1;
            for (const auto row : view.geometry.rows(view.reader()))
                if (!row.place.foot() && view.geometry.heightFor(row.track->id) > 0) {
                    if (row.track->id == movingSource_)
                        from = static_cast<std::ptrdiff_t>(lanes.size());
                    if (row.track->id == over->id)
                        to = static_cast<std::ptrdiff_t>(lanes.size());
                    lanes.push_back(row.track->id);
                }
            if (from >= 0 && to >= 0)
                if (const auto current = view.root.current())
                    if (auto landed = clipsAcross(current->tracks, lanes, view.geometry.selectedClips(), to - from)) {
                        movingAcross_ = std::move(*landed);
                        movingTrack_ = over->id;
                    }
        }
        const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.x) - grabTicks_);
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        movingTo_ = !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up);
        if (several) {
            // None of them goes before the arrangement's start.
            std::int64_t earliest = movingFrom_;
            for (const auto &t : view.reader().tracks())
                for (const auto &c : t->clips)
                    if (view.geometry.clipSelected(c->id))
                        earliest = std::min(earliest, c->posTicks);
            movingTo_ = std::max(movingTo_, movingFrom_ - earliest);
        }
        if (scrolled)
            view.frame(true);
        repaint();
        return;
    }
    if (panning) {
        view.geometry.pan(last.x - e.position.x);
        view.geometry.scrollY = std::clamp(view.geometry.scrollY + static_cast<int>(last.y - e.position.y), 0,
                                           view.geometry.maxScroll(view.reader()));
        last = e.position;
    } else {
        if (!draggingTimeRange_)
            return;
        auto &sel = view.geometry.selection;
        // Dragged across lanes, the selection reaches every track between (Live 12 manual 6.9); at
        // the lanes' edge they scroll, and it reaches on.
        const bool envelopeSelection = hasAutomationSelection();
        view.edgeScroll(e.x, e.y, true);
        // A time selection covers whole grid cells while snapping (§6.10).
        const auto at = view.geometry.tick(e.x);
        sel.end = view.snaps(e.mods) ? view.geometry.snap(at, at >= sel.start) : at;
        if (envelopeSelection) {
            const auto bodies = laneBodies();
            if (!bodies.empty()) {
                auto over = std::find_if(bodies.begin(), bodies.end(), [&](const auto &body) {
                    return e.position.y <= body.bottom;
                });
                if (over == bodies.end())
                    --over;
                sel.toTrack = over->track->id == sel.track ? 0 : over->track->id;
                rememberAutomationSelection(bodies, static_cast<std::size_t>(over - bodies.begin()), false);
            }
        } else if (const auto *over = view.geometry.trackAt(view.reader(), e.y))
            sel.toTrack = over->id == sel.track ? 0 : over->id;
    }
    view.frame(true);
}
void ArrangementCanvas::mouseUp(const juce::MouseEvent &e) {
    if (std::exchange(emptyTrackPress_, false)) {
        emptyTrackClick_ = emptyTrackClick_ && !e.mouseWasDraggedSinceMouseDown() && inEmptyTrackArea(e.getPosition());
        return;
    }
    if (emptyTrackDoubleClicked_)
        return;
    clipTimeBounds_.reset();
    if (std::exchange(draggingTakeSelection_, false))
        return;
    if (std::exchange(contextMenuPress_, false))
        return;
    if (ignoringAutomationClick_)
        return;
    if (automationTransformDrag_) {
        finishAutomationTransform(e);
        return;
    }
    if (automationCurveDrag_) {
        auto drag = std::move(*automationCurveDrag_);
        automationCurveDrag_.reset();
        if (!drag.cancelled && e.mouseWasDraggedSinceMouseDown() &&
            drag.generation == view.root.committedGeneration() && drag.tension != drag.initialTension) {
            const auto shape = drag.tension == 0 ? engine::CurveShape::Linear : engine::CurveShape::Bezier;
            commitAutomationCurve(drag.left, drag.right, static_cast<std::uint8_t>(shape), drag.tension);
        }
        repaint();
        return;
    }
    if (automationStroke_) {
        const bool drawing = view.root.automationMode() && view.root.drawMode();
        if (drawing)
            updateAutomationStroke(e);
        auto stroke = std::move(*automationStroke_);
        automationStroke_.reset();
        if (drawing && !stroke.cancelled && !stroke.values.empty() && view.geometry.shown(stroke.track) &&
            !view.geometry.foldedTracks.contains(stroke.track))
            view.root.drawAutomationStroke(stroke.track, stroke.target, stroke.values.begin()->first,
                                            stroke.end, stroke.values, stroke.stepped, stroke.generation);
        repaint();
        return;
    }
    if (automationDrag_) {
        const bool dragged = e.mouseWasDraggedSinceMouseDown();
        if (dragged)
            updateAutomationDrag(e);
        auto drag = std::move(*automationDrag_);
        automationDrag_.reset();
        if (drag.movesSelection) {
            if (dragged || drag.cancelled)
                chosenPoint_.reset();
            else {
                clearAutomationSelection();
                if (const auto clicked = chosenPoint_)
                    finishAutomationPointClick(*clicked);
            }
        }
        if (dragged)
            commitAutomationDrag(drag);
        repaint();
        return;
    }
    if (automationAddDrag_) {
        if (e.mouseWasDraggedSinceMouseDown())
            updateAutomationAddDrag(e);
        const bool current = automationAddDragIsCurrent();
        auto draft = std::move(*automationAddDrag_);
        automationAddDrag_.reset();
        if (current && view.root.addAutomationPoint(draft.track, draft.target, draft.tick, draft.value))
            if (const auto snapshot = view.root.current())
                if (const auto env = envelopeOf(*snapshot, draft.target); env.lane && env.data)
                    for (const auto &point : env.data->points)
                        if (point.time == draft.tick && (!chosenPoint_ || point.id > chosenPoint_->point))
                            chosenPoint_ = AutomationPick{draft.track, point.id, draft.target, env.lane->id, env.data->id};
        repaint();
        return;
    }
    if (pointDrag_) {
        // The same press can begin a drag or act on an existing breakpoint.
        const auto drag = *pointDrag_;
        pointDrag_.reset();
        if (!e.mouseWasDraggedSinceMouseDown()) {
            finishAutomationPointClick(drag);
        }
        repaint();
        return;
    }
    if (fadeDrag_) {
        // One drag, one undo step: the dragged clip, and every other selected audio clip with it.
        const auto h = *fadeDrag_;
        fadeDrag_.reset();
        std::vector<std::int64_t> clips{h.clip};
        if (view.geometry.clipSelected(h.clip))
            for (const auto &b : clipBodies())
                if (b.clip->id != h.clip && view.geometry.clipSelected(b.clip->id))
                    clips.push_back(b.clip->id);
        const engine::ClipNode *dragged = nullptr;
        for (const auto &b : clipBodies())
            if (b.clip->id == h.clip)
                dragged = b.clip;
        const auto handles = dragged ? fadeHandles(*dragged, h.in) : engine::ClipFadeHandles{};
        const bool changed = dragged != nullptr &&
                             (h.curve ? (h.in ? dragged->fadeInTension : dragged->fadeOutTension).value_or(0.0) != fadeTension_
                                      : (h.start ? handles.zeroTicks : handles.peakTicks) != fadeTicks_);
        if (changed) {
            if (const auto current = view.root.current()) {
                auto edit = dragFades(current->tracks, clips, h.in, h.curve ? std::nullopt : std::optional<std::int64_t>(fadeTicks_),
                                      h.curve ? std::optional<double>(fadeTension_) : std::nullopt, h.start);
                if (!edit.ops.empty())
                    view.root.submitEdits(std::move(edit.ops));
            }
        }
        view.changed();
        return;
    }
    if (resizing_ != 0 && !keyResize_) {
        auto &sel = view.geometry.selection;
        if (const auto tick = commitResize()) {
            if (resizeEdge_ == ResizeEdge::Left)
                sel.start = *tick;
            else
                sel.end = *tick;
        }
        repaint();
    }
    if (const auto drag = std::exchange(rangeDrag_, std::nullopt)) {
        if (drag->cancelled)
            return;
        bool landed = false;
        if (view.geometry.selection == drag->from) {
            if (!draggingTimeRange_) {
                view.select(pressedAt_.x, pressedAt_.y, view.snaps(e.mods));
                return;
            }
            landed = drag->to != std::min(drag->from.start, drag->from.end) && moveTimeSelection(drag->to);
            if (landed)
                view.locate();
        }
        view.changed(); // Clear the destination outline even if the range stayed where it was.
        if (landed)
            view.root.linkLoopToSelection();
        return;
    }
    if (moving_ != 0 && !view.geometry.selection.pickedClips.empty()) {
        // Several selected clips: one drag moves them all by the same offset, one undo step; a
        // press without a drag selects the clip pressed alone.
        const auto delta = movingDelta();
        const auto clip = moving_;
        const auto across = std::move(movingAcross_);
        movingAcross_.clear();
        moving_ = 0;
        auto &sel = view.geometry.selection;
        if (delta != 0 || !across.empty()) {
            if (const auto current = view.root.current()) {
                // Placed by the engine's planner: what the clips land on is cleared, as Live (DF-W28).
                auto edit = moveClips(current->tracks, view.geometry.selectedClips(), delta, across);
                std::vector<AdiRootComponent::ClipPlace> places;
                for (const auto &op : edit.ops)
                    places.push_back({op.payload.at("id").get<std::int64_t>(), op.payload.at("track").get<std::int64_t>(),
                                      op.payload.at("to").get<std::int64_t>()});
                if (!edit.refusal.empty()) {
                    view.root.say(edit.refusal);
                } else if (copying_ && !places.empty()) {
                    // Copied: the originals stay, and the copies are what is selected after.
                    if (const auto copies = view.root.copyClips(places, places.size() == 1 ? "Copy Clip" : "Copy Clips");
                        !copies.empty()) {
                        sel.clip = 0;
                        sel.pickedClips.clear();
                        for (std::size_t i = 0; i < places.size(); ++i) {
                            if (places[i].clip == clip) {
                                sel.clip = copies[i];
                                sel.track = places[i].track;
                                sel.toTrack = 0;
                            } else {
                                sel.pickedClips.push_back(copies[i]);
                            }
                        }
                        sel.start += delta;
                        sel.end += delta;
                    }
                } else if (!places.empty() && view.root.placeClips(places, edit.ops.front().label.c_str())) {
                    sel.start += delta;
                    sel.end += delta;
                    // The selection follows the clip dragged, onto its new track.
                    for (const auto &[id, target] : across)
                        if (id == clip) {
                            sel.track = target;
                            sel.toTrack = 0;
                        }
                }
            }
        } else if (selectOnUp_) {
            view.select(pressedAt_.x, pressedAt_.y, view.snaps(juce::ModifierKeys::currentModifiers));
        } else if (toggleOnUp_ != 0) {
            // Ctrl/Cmd+click on a picked clip's bar, without a drag: it is let go.
            auto &picked = sel.pickedClips;
            picked.erase(std::remove(picked.begin(), picked.end(), toggleOnUp_), picked.end());
        }
        selectOnUp_ = false;
        toggleOnUp_ = 0;
        copying_ = false;
        repaint();
    }
    if (moving_ != 0) {
        const auto clip = moving_, to = movingTo_, from = movingFrom_, track = movingTrack_;
        const bool across = movingTrack_ != movingSource_;
        const bool copy = std::exchange(copying_, false);
        moving_ = 0;
        // One drag, one undo step (§6.7), in time and to another track at once; the selection
        // follows the clip, or its copy when Ctrl copied it (the original staying).
        if (to != from || across) {
            auto &sel = view.geometry.selection;
            const auto length = sel.end - sel.start;
            bool landed = false;
            if (!copy) {
                landed = view.root.placeClips({{clip, track, to}}, "Move Clip");
            } else if (const auto copies = view.root.copyClips({{clip, track, to}}, "Copy Clip"); !copies.empty()) {
                sel.clip = copies.front();
                landed = true;
            }
            if (landed) {
                sel.start = to;
                sel.end = to + length;
                sel.track = track;
                sel.toTrack = 0;
                sel.picked.clear();
            }
        }
        repaint();
    }
    view.changed();
    view.root.linkLoopToSelection(); // with the setting on, the brace follows a time selection made
}
void ArrangementCanvas::beginResize(const engine::ClipNode &clip, std::int64_t track, ResizeEdge edge, bool stretch) {
    resizing_ = clip.id;
    stretching_ = stretch && clip.audio && clip.audio->warpEnabled;
    resizeTrack_ = track;
    resizeStart_ = clip.posTicks;
    resizeFrom_ = resizeLength_ = clip.lengthTicks;
    resizeEdge_ = edge;
    resizeLimit_.reset();
    if (edge == ResizeEdge::Right && !stretching_ && clip.audio && !clip.loopEnabled) {
        try {
            auto &store = view.root.editStore();
            const edit::TimeMap time(store.db());
            if (const auto limit = clip_edit::audioResizeLimit(store, clip.id, time))
                resizeLimit_ = (limit->base == 0 ? limit->value : time.ticks(limit->value)) - resizeStart_;
        } catch (const std::exception&) {
            // The durable edit will validate the source and clock if the user commits.
        }
    }
}

std::int64_t ArrangementCanvas::resizedLength() const noexcept {
    return resizeLimit_ ? std::min(resizeLength_, *resizeLimit_) : resizeLength_;
}

void ArrangementCanvas::resizeTo(std::int64_t tick, std::int64_t minLength) {
    if (resizeEdge_ == ResizeEdge::Left) {
        const auto end = resizeStart_ + resizeFrom_;
        resizeLength_ = std::max(std::min(minLength, end), end - std::max<std::int64_t>(0, tick));
    } else {
        resizeLength_ = std::max(minLength, tick - resizeStart_);
    }
}

std::optional<std::int64_t> ArrangementCanvas::commitResize() {
    const auto id = std::exchange(resizing_, 0);
    if (resizedLength() == resizeFrom_)
        return std::nullopt;
    if (stretching_) {
        const bool left = resizeEdge_ == ResizeEdge::Left;
        const auto begin = left ? resizeStart_ + resizeFrom_ - resizeLength_ : resizeStart_;
        if (view.root.commitEdit(clip_edit::prepareStretchWarpedClip(view.root.editStore(), id,
                begin, resizeLength_, view.root.editRate())))
            return left ? begin : begin + resizeLength_;
        return std::nullopt;
    }
    if (resizeEdge_ == ResizeEdge::Right) {
        if (view.root.submitEdit("clip.resize", {{"id", id}, {"pos", resizeStart_}, {"length", resizeLength_}}, "Resize Clip"))
            return resizeStart_ + resizedLength();
        return std::nullopt;
    }
    const auto end = resizeStart_ + resizeFrom_;
    const auto begin = end - resizeLength_;
    // The scope covers both clip extents, including a fractional tick on a time-locked clip.
    const auto scope = edit_plan::materialScope(std::array{resizeTrack_}, edit_plan::PlacementClock::Ticks,
                                               {std::max<std::int64_t>(0, std::min(begin, resizeStart_) - 1), end + 1});
    const edit_plan::Options options{view.root.editRate(), edit_plan::capturePolicy(view.root.commandSettings())};
    auto prepared = edit_plan::prepareTrim(view.root.editStore(), scope, {id, {0, begin}}, options);
    if (prepared.ok && !prepared.requests.empty())
        prepared.requests.front().label = "Resize Clip";
    if (!view.root.commitEdit(std::move(prepared)))
        return std::nullopt;
    // The engine can stop an extension at source frame zero. Use the resulting edge.
    const SnapshotReader after(view.root.current());
    if (const auto *track = after.findTrack(resizeTrack_))
        for (const auto &clip : track->clips)
            if (clip->id == id)
                return clip->posTicks;
    return std::nullopt;
}
void ArrangementCanvas::extendTo(int x, int y, bool snapToGrid) {
    // From the selection's anchor, or the insert marker, to the point clicked: its time, and its
    // track when it is another one.
    auto &geo = view.geometry;
    auto &sel = geo.selection;
    if (sel.start == sel.end)
        sel.start = sel.end = sel.insert;
    const auto at = std::max<std::int64_t>(0, geo.tick(x));
    sel.end = snapToGrid ? geo.snap(at, at >= sel.start) : at;
    if (const auto *t = geo.trackAt(view.reader(), y))
        sel.toTrack = t->id == sel.track ? 0 : t->id;
    sel.clip = 0;
    sel.pickedClips.clear();
    view.changed();
    announce(view.root.selectionText());
}
namespace {
// These existing planner exclusions keep the window's whole-clip Delete behavior.
bool canDeleteWholeClips(const edit_plan::EditPolicy &policy, std::string_view error) {
    return !policy.deleteOverlaps || policy.mutedClipsAsDeleted ||
           error == "frozen placement requires TM-FC-04 policy" ||
           error == "frozen-group placement requires TM-FC-04 policy";
}
} // namespace

bool ArrangementCanvas::rangeEdit(bool deactivate) {
    // From the CURRENT cell, as every action is (ADR-0201).
    const auto current = view.root.current();
    if (!current)
        return false;
    const auto &sel = view.geometry.selection;
    const auto selected = view.geometry.selectedTracks(view.reader());
    const auto content = rangeContent(current->tracks, selected, sel.start, sel.end);
    // A clip crossing the selection's edge (E1, N17): Delete trims it there, or cuts the middle out
    // of a clip the selection lies inside (`prepareDeleteRange`, with the settings' editing policy);
    // 0 over one clip splits it at the selection's edges inside it and deactivates the part inside,
    // one batch.
    if (!deactivate && !selected.empty()) {
        const edit_plan::Options options{view.root.editRate(), edit_plan::capturePolicy(view.root.commandSettings())};
        if (options.policy.automationFollowsClips || content.crossing > 0) {
            const clip_edit::RangeSelection range{selected, {0, std::min(sel.start, sel.end)}, {0, std::max(sel.start, sel.end)}};
            auto prepared = clip_edit::prepareDeleteRange(view.root.editStore(), range, options);
            if (prepared.ok) {
                if (prepared.requests.empty()) {
                    view.root.say("There is nothing in the selection to delete.");
                    return false;
                }
                if (content.crossing == 0 && !content.inside.empty())
                    prepared.requests.front().label = content.inside.size() == 1 ? "Delete Clip" : "Delete Clips";
                if (!view.root.commitEdit(std::move(prepared)))
                    return false;
                view.changed();
                return true;
            }
            if (!canDeleteWholeClips(options.policy, prepared.error))
                return view.root.commitEdit(std::move(prepared));
            if (content.crossing > 0) {
                const auto why = "The clips that cross the selection stay whole: the engine will not trim them here (" +
                                 prepared.error + ").";
                auto whole = deleteRange(current->tracks, selected, sel.start, sel.end);
                if (whole.ops.empty()) {
                    view.root.say(why);
                    return false;
                }
                if (!view.root.submitEdits(std::move(whole.ops)))
                    return false;
                view.changed();
                view.root.say(why);
                return true;
            }
        }
    }
    if (deactivate)
        if (const auto split = edgeSplit(content, sel.start, sel.end)) {
            const auto right = audio_drop_detail::newClipId(*current);
            // Two cuts: the middle piece is `right`, the last one the next identity.
            auto prepared = split->second != 0
                                ? clip_edit::prepareSplit(view.root.editStore(), split->clip, right, right + 1,
                                                          clip_edit::TimePoint{0, split->at},
                                                          clip_edit::TimePoint{0, split->second}, view.root.editRate())
                                : clip_edit::prepareSplit(view.root.editStore(), split->clip, right,
                                                          clip_edit::TimePoint{0, split->at}, view.root.editRate());
            const auto inside = split->second != 0 || split->insideIsRight ? right : split->clip;
            // 0 toggles, as on whole clips: a clip already deactivated has its inside part activated.
            bool wasOff = false;
            for (const auto *c : content.crossers)
                if (c->id == split->clip)
                    wasOff = c->muted;
            auto mute = track_edits_detail::op("clip.setMute", {{"id", inside}, {"muted", !wasOff}},
                                               wasOff ? "Activate Clip" : "Deactivate Clip");
            mute.targetKind = "clip";
            mute.targetId = inside;
            if (!view.root.commitEdit(std::move(prepared), std::move(mute)))
                return false;
            view.changed();
            return true;
        }
    const auto note = rangeNote(current->tracks, selected, sel.start, sel.end);
    auto edit = deactivate ? deactivateRange(current->tracks, selected, sel.start, sel.end)
                           : deleteRange(current->tracks, selected, sel.start, sel.end);
    if (!edit.refusal.empty()) {
        view.root.say(edit.refusal);
        return false;
    }
    if (!view.root.submitEdits(std::move(edit.ops)))
        return false;
    // After the view is told: locating the playhead to the selection clears the Status Bar.
    view.changed();
    if (!note.empty())
        view.root.say(note);
    return true;
}
bool ArrangementCanvas::editMaterialSelection(bool deactivate) {
    auto &sel = view.geometry.selection;
    if (sel.clip == 0)
        return sel.start != sel.end && rangeEdit(deactivate);
    const auto current = view.root.current();
    if (!current)
        return false;
    const auto ids = view.geometry.selectedClips();
    auto edit = deactivate ? deactivateClips(current->tracks, ids) : deleteClips(current->tracks, ids);
    if (!edit.refusal.empty()) {
        view.root.say(std::move(edit.refusal));
        return false;
    }
    if (!deactivate && !edit.ops.empty()) {
        const edit_plan::Options options{view.root.editRate(), edit_plan::capturePolicy(view.root.commandSettings())};
        if (options.policy.automationFollowsClips) {
            std::vector<std::int64_t> present;
            for (const auto &op : edit.ops)
                present.push_back(op.payload.at("id").get<std::int64_t>());
            auto prepared = edit_plan::prepareDelete(view.root.editStore(), present, options);
            if (prepared.ok || !canDeleteWholeClips(options.policy, prepared.error)) {
                if (!prepared.requests.empty())
                    prepared.requests.front().label = edit.ops.front().label;
                if (!view.root.commitEdit(std::move(prepared)))
                    return false;
                edit.ops.clear();
            }
        }
    }
    if (!edit.ops.empty() && !view.root.submitEdits(std::move(edit.ops)))
        return false;
    if (!deactivate) {
        sel.insert = sel.start;
        sel.clearMaterial();
        view.changed();
    }
    return true;
}
bool ArrangementCanvas::inEmptyTrackArea(juce::Point<int> point) const {
    if (!getLocalBounds().contains(point))
        return false;
    const auto &geometry = view.geometry;
    const auto &reader = view.reader();
    const auto rows = geometry.rows(reader);
    const int bottom = geometry.footStart(rows) < rows.size() ? geometry.footTop(reader) - 2 : getHeight();
    return point.y >= geometry.tracksHeight(reader) - geometry.scrollY && point.y < bottom;
}
void ArrangementCanvas::addTrackInEmptyArea(juce::Point<int> where) {
    if (view.root.editStore().readOnly()) {
        view.root.say("The project is read-only.");
        return;
    }
    const auto add = [safe = SafePointer<ArrangementCanvas>(this)](const std::string &type, bool remember) {
        if (!safe)
            return;
        auto &root = safe->view.root;
        const bool inserted = type == "FX Channel" ? root.command(AppCommands::InsertReturnTrack)
                                                    : root.insertTrack(type == "MIDI Track", 0);
        if (inserted && remember && safe) {
            auto &preferences = root.commands().writableSettings();
            std::string why;
            if (!preferences.set(settings::kEmptyArrangementTrackKey, type, {}, why) || !preferences.save(why))
                root.say("The track type could not be remembered: " + why);
        }
    };
    const auto choice = view.root.commandSettings().get(settings::kEmptyArrangementTrackKey).get<std::string>();
    if (choice != "Ask") {
        add(choice, false);
        return;
    }
    juce::PopupMenu menu;
    for (const auto *type : {"MIDI Track", "Audio Track", "FX Channel"})
        menu.addItem(type, [add, type] { add(type, true); });
    view.presentContextMenu(where, false, nullptr, std::move(menu));
}
void ArrangementCanvas::mouseDoubleClick(const juce::MouseEvent &e) {
    if (emptyTrackDoubleClicked_)
        return;
    if (const auto bounds = automationTransformBounds(); bounds && automationTransformHandleAt(*bounds, e.position))
        return;
    const bool releasedInEmpty = inEmptyTrackArea(e.getPosition());
    const bool pressedInEmpty = inEmptyTrackArea(e.getMouseDownPosition());
    if (releasedInEmpty || pressedInEmpty) {
        if (std::exchange(emptyTrackClick_, false) && releasedInEmpty && pressedInEmpty &&
            e.mods == juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier)) {
            emptyTrackDoubleClicked_ = true;
            addTrackInEmptyArea(e.getScreenPosition());
        }
        return;
    }
    // Live: a double-click on a clip opens it in the Clip View (manual 8).
    if (e.mods.isPopupMenu() || view.geometry.takeLaneAt(view.reader(), e.y))
        return;
    // A breakpoint's hit slop does not cover a clip title.
    std::int64_t titleTrack = 0;
    ResizeEdge titleEdge = ResizeEdge::None;
    if (view.root.automationMode() && barAt(e.x, e.y, titleTrack, titleEdge) == nullptr) {
        const auto transform = automationTransformBounds();
        if (view.root.drawMode() || ignoringAutomationClick_ || automationPointAt(e.position) ||
            (transform && automationTransformHandleAt(*transform, e.position)))
            return;
        if (e.mods.withoutMouseButtons() == juce::ModifierKeys(juce::ModifierKeys::altModifier)) {
            if (const auto segment = automationSegmentAt(e.position, kAutomationCurveHitPixels);
                segment && !segment->held && segment->diagonal) {
                const auto shape = static_cast<std::uint8_t>(engine::CurveShape::Linear);
                commitAutomationCurve(segment->left, segment->right.point, shape, 0.f);
                return;
            }
        }
        // Elsewhere on a lane a double-click is the envelope's too: off the line it adds a breakpoint
        // there (manual 25.5.2; on the line the first press added it); it neither makes a clip nor opens one.
        for (const auto &body : laneBodies())
            if (e.position.y >= body.top && e.position.y <= body.bottom && !e.mods.isShiftDown() && !e.mods.isCommandDown()) {
                if (!automationLineAt(e.position)) {
                    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.position.x));
                    const auto &target = body.target;
                    const auto value = automationValue(e.position.y, automationRange(target), body.top, body.scaleBottom);
                    if (view.root.addAutomationPoint(body.track->id, target,
                                                    view.snaps(e.mods) ? view.geometry.snap(raw, false) : raw, value))
                        showAutomationValue(target,writtenAutomationValue(target,value),e.position);
                }
                return;
            }
    }
    const auto *t = view.geometry.trackAt(view.reader(), e.y);
    const auto *c = t != nullptr ? view.geometry.hit(*t, view.geometry.tick(e.x)) : nullptr;
    if (c == nullptr) {
        // On an empty part of a MIDI track's lane it makes a clip of the bar under the pointer and
        // opens it (Live 12 makes a clip on a double-click there; its length is to be measured).
        if (t == nullptr || !isMidiTrack(*t) || view.geometry.meters == nullptr)
            return;
        const auto bar = view.geometry.barHolding(view.geometry.tick(e.x));
        if (view.root.insertMidiClips({t->id}, bar.start, bar.end))
            view.root.showClipView();
        return;
    }
    if (view.geometry.selection.clip != c->id)
        view.selectClip(t->id, c->id);
    view.root.showClipView();
}
void ArrangementCanvas::mouseWheelMove(const juce::MouseEvent &e,
                                       const juce::MouseWheelDetails &w) {
    view.wheel(e, w);
}
void ArrangementCanvas::mouseMagnify(const juce::MouseEvent &e, float factor) {
    if (e.mods.isAltDown()) {
        if (const auto *t = view.geometry.trackAt(view.reader(), e.y))
            view.geometry.resizeTrack(
                t->id,
                static_cast<int>(static_cast<float>(view.geometry.heightFor(t->id)) * factor));
    } else
        view.geometry.zoom(factor, e.position.x);
    view.changed();
}
std::optional<ArrangementCanvas::AutomationLaneKey> ArrangementCanvas::automationKeyboardLane() const {
    if (!view.root.automationMode() || view.root.drawMode() || view.loopSelected ||
        view.selectedLocator != 0 || view.selectedMeter >= 0 || selectedFade_ || fadeDrag_ ||
        resizing_ != 0 || moving_ != 0)
        return std::nullopt;
    if (chosenPoint_ && automationPickIsCurrent(*chosenPoint_))
        return AutomationLaneKey{chosenPoint_->track, chosenPoint_->target};
    if (view.geometry.selection.clip != 0 || !view.geometry.selection.pickedClips.empty())
        return std::nullopt;
    if (keyboardAutomationLane_ && view.root.automationTargetShown(keyboardAutomationLane_->first,
                                                                  keyboardAutomationLane_->second))
        return keyboardAutomationLane_;
    if (hasAutomationSelection() && automationSelectionAnchor_ &&
        view.root.automationTargetShown(automationSelectionAnchor_->first, automationSelectionAnchor_->second))
        return automationSelectionAnchor_;
    const auto track = view.geometry.selection.track;
    if (track != 0 && view.root.automationOverlayShown(track)) {
        auto target = view.root.automationTarget(track);
        if (view.root.automationTargetShown(track, target))
            return AutomationLaneKey{track, std::move(target)};
    }
    if (track != 0 && view.root.automationLanesExpanded(track))
        for (const auto &lane : view.root.additionalAutomationLanes(track))
            if (view.root.automationTargetShown(track, lane.target))
                return AutomationLaneKey{track, lane.target};
    return std::nullopt;
}

void ArrangementCanvas::focusAutomationKeyboardLane(const AutomationLaneKey &lane, std::int64_t tick,
                                                      std::optional<AutomationPick> focused) {
    clearAutomationSelection();
    chosenPoint_.reset();
    auto &geo = view.geometry;
    auto &selection = geo.selection;
    selection = {};
    selection.track = lane.first;
    selection.insert = selection.start = selection.end = tick;
    selection.marked = true;
    automationSelectionAnchor_ = lane;
    automationSelectionTargets_.push_back(lane);
    automationSelection_ = selection;
    keyboardAutomationLane_ = lane;
    keyboardAutomationFocus_ = std::move(focused);
    geo.reveal(view.reader(), lane.first, false);
    const auto x = geo.x(tick);
    if (x < 0 || x >= getWidth())
        geo.pan(x - getWidth() / 2.);
    view.changed();
    const auto current = view.root.current();
    if (!current)
        return;
    const auto envelope = envelopeOf(*current, lane.second);
    double value = view.root.automationCurrent(lane.second);
    if (envelope.data)
        value = envelopeValue(*envelope.data, tick, lane.second.stepped,
                              current->tempo.get(), view.root.editRate()).value_or(value);
    for (const auto &line : laneBodies())
        if (line.track->id == lane.first && line.target == lane.second) {
            showAutomationValue(lane.second, value,
                {static_cast<float>(geo.x(tick)), automationY(value, automationRange(lane.second), line.top, line.scaleBottom)});
            break;
        }
}

bool ArrangementCanvas::navigateAutomationWithAlt(const juce::KeyPress &key, const AutomationLaneKey &lane) {
    const auto mods = key.getModifiers();
    if (!mods.isAltDown() || mods.isCommandDown() || mods.isCtrlDown())
        return false;
    const auto code = key.getKeyCode();
    const bool horizontal = code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
    const bool vertical = code == juce::KeyPress::upKey || code == juce::KeyPress::downKey;
    if ((!horizontal && !vertical) || (horizontal && mods.isShiftDown()))
        return false;

    const auto current = view.root.current();
    const auto &[track, target] = lane;
    if (!current || !view.root.automationTargetShown(track, target))
        return false;

    const auto marker = view.geometry.selection.insert;
    if (horizontal) {
        const auto envelope = envelopeOf(*current, target);
        if (!editableEnvelope(envelope) || !envelope.lane || !envelope.data)
            return true;
        const auto &points = envelope.data->points;
        const auto firstAfter = [&](auto first, std::int64_t tick) {
            return std::upper_bound(first, points.end(), tick, [](auto time, const auto &point) {
                return time < point.time;
            });
        };
        // Step by distinct times. At coincident points, focus the last stored
        // point at that time, which is the value the envelope plays there.
        auto point = points.begin();
        if (code == juce::KeyPress::rightKey) {
            point = firstAfter(points.begin(), marker);
            if (point == points.end())
                return true;
            point = firstAfter(point, point->time);
        } else {
            point = std::lower_bound(points.begin(), points.end(), marker, [](const auto &entry, auto time) {
                return entry.time < time;
            });
            if (point == points.begin())
                return true;
        }
        --point;
        focusAutomationKeyboardLane({track, target}, point->time,
            AutomationPick{track, point->id, target, envelope.lane->id, envelope.data->id});
        return true;
    }

    const auto choices = view.root.automationChoices(track);
    if (choices.empty())
        return true;
    auto candidate = std::find(choices.begin(), choices.end(), target);
    // Retain the chooser order, including the position of a current target
    // that Alt alone would skip because it has no arrangement points.
    for (std::size_t step = 0; step < choices.size(); ++step) {
        if (code == juce::KeyPress::downKey) {
            if (candidate == choices.end() || ++candidate == choices.end())
                candidate = choices.begin();
        } else {
            if (candidate == choices.begin())
                candidate = choices.end();
            --candidate;
        }
        if (*candidate == target)
            return true;
        const auto envelope = envelopeOf(*current, *candidate);
        if (!editableEnvelope(envelope) || (!mods.isShiftDown() && (!envelope.data || envelope.data->points.empty())))
            continue;

        if (!view.root.automationTargetShown(track, *candidate))
            view.root.showAutomation(track, *candidate);
        if (view.root.automationTargetShown(track, *candidate)) {
            focusAutomationKeyboardLane({track, *candidate}, marker);
            break;
        }
    }
    return true;
}

bool ArrangementCanvas::automationKeyPressed(const juce::KeyPress &key) {
    const auto lane = automationKeyboardLane();
    if (!lane)
        return false;
    if (navigateAutomationWithAlt(key, *lane))
        return true;
    if (view.geometry.selection.start != view.geometry.selection.end && !chosenPoint_)
        return false;
    const auto mods = key.getModifiers();
    if (mods.isAltDown() || mods.isCommandDown() || mods.isCtrlDown())
        return false;

    const auto current = view.root.current();
    const auto track = lane->first;
    const auto &target = lane->second;
    if (!current)
        return false;
    const auto envelope = envelopeOf(*current, target);
    if (!editableEnvelope(envelope))
        return false;

    const auto code = key.getKeyCode();
    const auto marker = view.geometry.selection.insert;
    const auto makeAddress = [&](std::int64_t tick, const engine::AutomationPointNode *point) {
        AutomationValueAddress address;
        address.target = target;
        address.track = track;
        address.lane = envelope.lane ? envelope.lane->id : 0;
        address.data = envelope.data ? envelope.data->id : 0;
        address.tick = tick;
        if (point) {
            address.point = point->id;
            address.value = point->value;
        } else {
            address.value = view.root.automationCurrent(target);
            if (envelope.data)
                address.value = envelopeValue(*envelope.data, tick, target.stepped,
                                              current->tempo.get(), view.root.editRate()).value_or(address.value);
        }
        const auto x = static_cast<float>(view.geometry.x(tick));
        auto y = static_cast<float>(getHeight()) / 2;
        for (const auto &row : laneBodies())
            if (row.track->id == track && row.target == target) {
                y = automationY(address.value, automationRange(target), row.top, row.scaleBottom);
                break;
            }
        address.anchor = juce::Point<float>{x, y}.roundToInt();
        return address;
    };
    const auto selectPoint = [&](const engine::AutomationPointNode &point) {
        focusAutomationKeyboardLane(*lane, point.time);
        chosenPoint_ = AutomationPick{track, point.id, target, envelope.lane->id, envelope.data->id};
        repaint();
    };
    const auto *points = envelope.data ? &envelope.data->points : nullptr;
    const engine::AutomationPointNode *selected = nullptr;
    if (points && envelope.lane && chosenPoint_ && chosenPoint_->track == track && chosenPoint_->target == target &&
        chosenPoint_->lane == envelope.lane->id && chosenPoint_->data == envelope.data->id) {
        const auto point = std::find_if(points->begin(), points->end(), [&](const auto &entry) {
            return entry.id == chosenPoint_->point;
        });
        if (point != points->end())
            selected = &*point;
    }

    if (code == juce::KeyPress::escapeKey && (keyboardAutomationDraft_ || selected)) {
        keyboardAutomationDraft_.reset();
        keyboardAutomationFocus_.reset();
        chosenPoint_.reset();
        clearAutomationValueReadout();
        repaint();
        return true;
    }
    if (code == juce::KeyPress::tabKey && selected) {
        const auto offset = selected - points->data();
        if (mods.isShiftDown() && offset > 0)
            selectPoint((*points)[static_cast<std::size_t>(offset - 1)]);
        else if (!mods.isShiftDown() && static_cast<std::size_t>(offset + 1) < points->size())
            selectPoint((*points)[static_cast<std::size_t>(offset + 1)]);
        return true;
    }
    if (code == juce::KeyPress::returnKey && !mods.isShiftDown()) {
        if (keyboardAutomationDraft_) {
            const auto draft = *keyboardAutomationDraft_;
            if (!resolveAutomationValue(draft.address, envelope)) {
                keyboardAutomationDraft_.reset();
                chosenPoint_.reset();
                view.root.say("That automation point or lane changed. Select it again.");
                repaint();
                return true;
            }
            const bool written = draft.address.point
                ? view.root.moveAutomationPoint(track, target, *draft.address.point, draft.tick, draft.value, true)
                : view.root.addAutomationPoint(track, target, draft.tick, draft.value, true);
            if (written)
                focusAutomationKeyboardLane(*lane, draft.tick);
            return true;
        }
        if (selected) {
            chosenPoint_.reset();
            repaint();
            return true;
        }
        if (points) {
            const auto after = std::upper_bound(points->begin(), points->end(), marker, [](auto tick, const auto &point) {
                return tick < point.time;
            });
            if (after != points->begin() && (after - 1)->time == marker) {
                selectPoint(*(after - 1));
                return true;
            }
        }
        const auto address = makeAddress(marker, nullptr);
        focusAutomationKeyboardLane(*lane, marker);
        keyboardAutomationDraft_ = KeyboardAutomationDraft{address, marker, address.value};
        repaint();
        return true;
    }

    if (const auto character = valueEntryStart(key); character != 0 && (keyboardAutomationDraft_ || selected)) {
        auto address = keyboardAutomationDraft_ ? keyboardAutomationDraft_->address : makeAddress(selected->time, selected);
        if (keyboardAutomationDraft_ && !address.point)
            address.tick = keyboardAutomationDraft_->tick;
        beginAutomationValueEdit(address);
        if (automationValueEditor_.isVisible()) {
            keyboardValueEditing_ = true;
            automationValueEditor_.setText(juce::String::charToString(character), false);
            automationValueEditor_.setCaretPosition(1);
        }
        return true;
    }

    if (code == juce::KeyPress::deleteKey && keyboardAutomationDraft_ && !keyboardAutomationDraft_->address.point) {
        keyboardAutomationDraft_.reset();
        repaint();
        return true;
    }
    const bool vertical = code == juce::KeyPress::upKey || code == juce::KeyPress::downKey;
    const bool horizontal = code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
    if (!mods.isShiftDown() && vertical && (keyboardAutomationDraft_ || selected)) {
        if (!keyboardAutomationDraft_)
            keyboardAutomationDraft_ = KeyboardAutomationDraft{makeAddress(selected->time, selected), selected->time, selected->value};
        auto &draft = *keyboardAutomationDraft_;
        constexpr double kAutomationKeyboardValueStep = .01;
        const auto range = automationRange(target);
        const auto step = range.stepped && range.stepCount > 1 ? 1.0 / (range.stepCount - 1)
                                                               : kAutomationKeyboardValueStep;
        const auto delta = (code == juce::KeyPress::upKey ? 1. : -1.) * step;
        if (envelope.lane)
            draft.value = automationDragValue(*envelope.lane, target, draft.value, delta);
        else
            draft.value = writtenAutomationValue(target, draft.value +
                (range.flipped ? -delta : delta) * (range.maximum - range.minimum));
        showAutomationValue(target, draft.value, makeAddress(draft.tick, nullptr).anchor.toFloat());
        return true;
    }
    if (!mods.isShiftDown() && horizontal && keyboardAutomationDraft_ && !keyboardAutomationDraft_->address.point) {
        auto &draft = *keyboardAutomationDraft_;
        draft.tick = view.geometry.nextGridLine(draft.tick, code == juce::KeyPress::rightKey);
        selectAutomationSpan(draft.tick, draft.tick);
        showAutomationValue(target, draft.value, makeAddress(draft.tick, nullptr).anchor.toFloat());
        return true;
    }
    if (!mods.isShiftDown() && horizontal && !selected) {
        const auto tick = view.geometry.nextGridLine(marker, code == juce::KeyPress::rightKey);
        focusAutomationKeyboardLane(*lane, tick);
        return true;
    }
    return false;
}

bool ArrangementCanvas::keyPressed(const juce::KeyPress &pressed) {
    const auto takeKey = pressed.getKeyCode();
    const auto takeMods = pressed.getModifiers();
    if ((takeKey == 'u' || takeKey == 'U') && takeMods.isCommandDown() && takeMods.isAltDown())
        return view.toggleTakeLanes(view.geometry.selection.track);
    if (view.takeSelection && !takeMods.isAnyModifierKeyDown()) {
        if (takeKey == 't' || takeKey == 'T')
            return view.toggleTakeAudition(view.takeSelection->track, view.takeSelection->lane);
        if (takeKey == juce::KeyPress::returnKey)
            return view.copyTakeSelectionToMain();
        if (takeKey == juce::KeyPress::escapeKey) {
            view.takeSelection.reset();
            repaint();
            return true;
        }
    }
    if (view.root.playComputerMidiKey(pressed))
        return true;
    const auto k = view.root.computerMidiShortcut(pressed);
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();
    if (rangeDrag_ && code == juce::KeyPress::escapeKey) {
        // Keep consuming this gesture until the held button is released.
        rangeDrag_->cancelled = true;
        repaint();
        return true;
    }
    if (automationAddDrag_ && code == juce::KeyPress::escapeKey) {
        cancelAutomationAddDrag();
        return true;
    }
    if (automationTransformDrag_ && code == juce::KeyPress::escapeKey) {
        automationTransformDrag_->cancelled = true;
        repaint();
        return true;
    }
    if (automationCurveDrag_ && code == juce::KeyPress::escapeKey) {
        automationCurveDrag_->cancelled = true;
        repaint();
        return true;
    }
    if (automationDrag_ && code == juce::KeyPress::escapeKey) {
        automationDrag_->cancelled = true;
        repaint();
        return true;
    }
    if (automationStroke_ && code == juce::KeyPress::escapeKey) {
        automationStroke_->cancelled = true;
        repaint();
        return true;
    }
    if (automationKeyPressed(k))
        return true;
    // Delete or Backspace takes the chosen breakpoint away (Automation Mode).
    if (chosenPoint_ && !view.geometry.foldedTracks.contains(chosenPoint_->track) && view.root.automationMode() &&
        !view.loopSelected && view.selectedLocator == 0 &&
        view.selectedMeter < 0 && (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) &&
        !mods.isAnyModifierKeyDown()) {
        const auto chosen = *chosenPoint_;
        chosenPoint_.reset();
        if (automationPickIsCurrent(chosen))
            view.root.deleteAutomationPoint(chosen.track, chosen.target, chosen.point);
        repaint();
        return true;
    }
    auto &sel = view.geometry.selection;
    // F held shows every audio clip's fade handles (manual 6.8).
    if ((code == 'f' || code == 'F') && !mods.isAnyModifierKeyDown()) {
        if (!showFades) {
            showFades = true;
            repaint();
        }
        return true;
    }
    // Delete with a fade's handle chosen deletes that fade (manual 6.8; Create Fades on Clip
    // Edges is off, ADR-0132 d6, so it goes to nothing, not to 4 ms).
    if ((code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) && !mods.isAnyModifierKeyDown() &&
        selectedFade_) {
        // Only while that fade's clip is still what is chosen, nothing else is (a locator, a marker,
        // the brace), and the fade has a length; else Delete is the selection's, as without one.
        const auto h = *selectedFade_;
        selectedFade_.reset();
        const engine::ClipNode *clip = nullptr;
        for (const auto &b : clipBodies())
            if (b.clip->id == h.clip)
                clip = b.clip;
        const bool stillChosen = clip != nullptr && sel.clip == h.clip && !view.loopSelected && view.selectedLocator == 0 &&
                                 view.selectedMeter < 0;
        if (stillChosen && (h.in ? clip->fadeInTicks : clip->fadeOutTicks) > 0)
            if (const auto current = view.root.current()) {
                auto edit = dragFades(current->tracks, {h.clip}, h.in, std::int64_t{0}, std::nullopt);
                for (auto &op : edit.ops)
                    op.label = "Delete Fade";
                if (!edit.ops.empty() && view.root.submitEdits(std::move(edit.ops))) {
                    view.changed();
                    return true;
                }
            }
    }
    // The arrows move the active edge by the grid. Enter keeps it as one undo step; Escape cancels.
    if (keyResizing()) {
        if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && !mods.isAnyModifierKeyDown()) {
            const auto step = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(view.geometry.gridQuarters() * textproj::kPPQ));
            const auto edge = resizeEdge_ == ResizeEdge::Left ? resizeStart_ + resizeFrom_ - resizeLength_
                                                               : resizeStart_ + resizedLength();
            resizeTo(view.geometry.nextGridLine(edge, code == juce::KeyPress::rightKey), step);
            repaint();
            announce("Length " + juce::String(static_cast<double>(resizedLength()) / textproj::kPPQ, 2) + " beats");
            return true;
        }
        keyResize_ = false;
        repaint();
        if (code == juce::KeyPress::returnKey) {
            if (const auto tick = commitResize())
                sel.insert = sel.start = sel.end = *tick;
            view.changed();
            announce(view.root.selectionText());
            return true;
        }
        resizing_ = 0;
        if (code == juce::KeyPress::escapeKey) {
            announce("Resize cancelled");
            return true;
        }
    }
    // U folds or unfolds the selected track; Alt+U unfolds every track.
    if (k == juce::KeyPress('u')) {
        view.toggleFold(sel.track);
        return true;
    }
    if (k == juce::KeyPress('u', juce::ModifierKeys::altModifier, 0)) {
        view.unfoldAll();
        return true;
    }
    // The selected loop brace's keys (manual 6.6, §42.8): left and right nudge it by the grid, up and
    // down move it by its length; with Ctrl/Cmd, left and right shorten or lengthen it by the grid,
    // up and down double or halve it.
    if (view.loopSelected && !mods.isShiftDown() && !mods.isAltDown() &&
        (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey ||
         code == juce::KeyPress::downKey)) {
        const auto key = code == juce::KeyPress::leftKey    ? BraceKey::Left
                         : code == juce::KeyPress::rightKey ? BraceKey::Right
                         : code == juce::KeyPress::upKey    ? BraceKey::Up
                                                            : BraceKey::Down;
        if (view.root.braceKey(key, mods.isCommandDown()))
            view.selectLoop();
        return true;
    }
    // A selected locator (manual 6.4): left and right move it a grid line, Delete removes it.
    if (view.selectedLocator != 0 && !mods.isAnyModifierKeyDown() &&
        (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)) {
        if (const auto *snapshot = view.reader().get())
            if (const auto locators = locatorsOf(*snapshot); const auto *l = locatorById(locators, view.selectedLocator))
                view.root.moveLocator(l->id, view.geometry.nextGridLine(l->pos, code == juce::KeyPress::rightKey));
        return true;
    }
    if (view.selectedLocator != 0 && (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)) {
        view.root.deleteLocator(view.selectedLocator);
        view.repaint();
        return true;
    }
    // Left and right move a selected time signature marker by the grid (Live 12 manual 6.5).
    if (view.selectedMeter >= 0 && (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) &&
        !mods.isAnyModifierKeyDown()) {
        const auto from = view.selectedMeter;
        view.root.moveMeter(from, view.geometry.nextGridLine(from, code == juce::KeyPress::rightKey));
        return true;
    }
    const auto tracks = view.geometry.rows(view.reader());
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
        clearAutomationSelection();
        // Between the tracks shown, in the lanes' order: those the project view leaves out or a
        // folded group holds are passed over. At either end the selection stays; with none, it
        // goes to the first track shown. With Shift, the selection's far track moves instead, so the
        // selection reaches across tracks (Live 12 manual 6.9).
        const bool extend = mods.isShiftDown() && sel.track != 0;
        const auto at = tracks.find(extend && sel.toTrack != 0 ? sel.toTrack : sel.track);
        std::size_t to = tracks.size();
        if (at == tracks.size()) {
            for (std::size_t i = 0; i < tracks.size() && to == tracks.size(); ++i)
                if (view.geometry.shown(tracks[i].track->id))
                    to = i;
        } else if (code == juce::KeyPress::downKey) {
            for (std::size_t i = at + 1; i < tracks.size() && to == tracks.size(); ++i)
                if (view.geometry.shown(tracks[i].track->id))
                    to = i;
        } else {
            for (std::size_t i = at; i-- > 0 && to == tracks.size();)
                if (view.geometry.shown(tracks[i].track->id))
                    to = i;
        }
        if (to == tracks.size())
            to = at;
        if (to == tracks.size())
            return true;
        if (extend) {
            if (sel.start == sel.end)
                sel.start = sel.end = sel.insert;
            const auto far = tracks[to].track->id;
            sel.toTrack = far == sel.track ? 0 : far;
            sel.clip = 0;
            sel.pickedClips.clear();
            view.geometry.reveal(view.reader(), far);
            view.changed();
            announce(view.root.selectionText());
            return true;
        }
        sel.track = tracks[to].track->id;
        sel.toTrack = 0;
        sel.picked.clear();
        sel.clip = 0;
        sel.pickedClips.clear();
        view.geometry.reveal(view.reader(), sel.track);
        view.changed();
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
        if (arrow(code == juce::KeyPress::rightKey, mods))
            return true;
        return view.root.keyPressed(k);
    }
    // Enter at either edge starts a keyboard resize. At a shared boundary use the earlier clip.
    if (code == juce::KeyPress::returnKey && !mods.isAnyModifierKeyDown() && sel.start == sel.end && sel.marked)
        if (const auto *t = view.reader().findTrack(sel.track)) {
            const engine::ClipNode *clip = nullptr;
            auto edge = ResizeEdge::Left;
            for (const auto &c : t->clips) {
                if (sel.insert == c->posTicks + c->lengthTicks) {
                    clip = c.get();
                    edge = ResizeEdge::Right;
                    break;
                }
                if (sel.insert == c->posTicks)
                    clip = c.get();
            }
            if (clip) {
                beginResize(*clip, t->id, edge);
                keyResize_ = true;
                announce("Resizing " + describe(sel.track, clip->id) +
                         ". Left and right change its length; Enter keeps it, Esc cancels.");
                repaint();
                return true;
            }
        }
    // Delete on a selected time signature marker (Live 12 manual 6.5).
    if (view.selectedMeter >= 0 && (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)) {
        view.root.deleteMeter(view.selectedMeter);
        return true;
    }
    if (hasAutomationSelection()) {
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) {
            view.root.deleteAutomationRange();
            return true;
        }
        if (k == juce::KeyPress('0'))
            return true;
    }
    // Live's Delete, and 0 to deactivate (§6.9), on a time selection: every selected track's
    // material, as one undo step.
    const bool erase = k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey;
    if (erase || (k == juce::KeyPress('0') && (sel.clip != 0 || sel.start != sel.end))) {
        editMaterialSelection(!erase);
        return true;
    }
    return view.root.keyPressed(k);
}
bool ArrangementCanvas::arrow(bool forward, const juce::ModifierKeys &mods) {
#if JUCE_MAC
    const bool edgeKey = mods.isAltDown(), other = mods.isCommandDown() || mods.isCtrlDown();
#else
    const bool edgeKey = mods.isCtrlDown(), other = mods.isAltDown();
#endif
    if (other)
        return false;
    auto &geo = view.geometry;
    auto &sel = geo.selection;
    const auto *track = view.reader().findTrack(sel.track);
    if (hasAutomationSelection() && !edgeKey) {
        if (mods.isShiftDown()) {
            const auto begin = sel.start == sel.end ? sel.insert : sel.start;
            const auto end = geo.nextGridLine(sel.start == sel.end ? sel.insert : sel.end, forward);
            selectAutomationSpan(begin, end);
        } else {
            const auto at = sel.start == sel.end
                ? geo.nextGridLine(sel.insert, forward)
                : (forward ? std::max(sel.start, sel.end) : std::min(sel.start, sel.end));
            selectAutomationSpan(at, at);
            view.locate();
        }
        view.root.linkLoopToSelection();
        announce(view.root.selectionText());
        return true;
    }
    if (mods.isShiftDown()) {
        // Shift selects from the insert marker, or moves the selection's far edge; a selection
        // that is exactly a clip selects that clip (§6.9).
        if (sel.start == sel.end)
            sel.start = sel.end = sel.insert;
        sel.end = edgeKey ? clipEdge(sel.end, forward) : geo.nextGridLine(sel.end, forward);
        sel.clip = 0;
        sel.pickedClips.clear();
        sel.marked = true;
        const auto lo = std::min(sel.start, sel.end), hi = std::max(sel.start, sel.end);
        if (track != nullptr)
            for (const auto &c : track->clips)
                if (c->posTicks == lo && c->posTicks + c->lengthTicks == hi)
                    sel.clip = c->id;
        view.changed();
        view.root.linkLoopToSelection();
        announce(sel.clip != 0 ? describe(sel.track, sel.clip) : view.root.selectionText());
        return true;
    }
    if (edgeKey) {
        // The previous or next clip edge on the track, or locator.
        const auto from = sel.start == sel.end ? sel.insert
                          : forward            ? std::max(sel.start, sel.end)
                                               : std::min(sel.start, sel.end);
        sel.insert = sel.start = sel.end = clipEdge(from, forward);
        sel.clip = 0;
        sel.pickedClips.clear();
        sel.marked = true;
        view.locate();
        view.changed();
        announce(view.root.selectionText());
        return true;
    }
    if (sel.start != sel.end && !view.root.commands().arrowKeysMoveClips()) {
        // Move Clips with Arrow Keys off (§41.1.1): left collapses the selection to its start,
        // right to its end.
        sel.insert = sel.start = sel.end = forward ? std::max(sel.start, sel.end) : std::min(sel.start, sel.end);
        sel.clip = 0;
        sel.pickedClips.clear();
        view.locate();
        view.changed();
        announce(view.root.selectionText());
        return true;
    }
    if (!sel.pickedClips.empty()) {
        std::vector<AdiRootComponent::ClipPlace> clips;
        constexpr auto maxTick = std::numeric_limits<std::int64_t>::max();
        auto first = maxTick;
        std::int64_t latestEnd = 0;
        if (const auto current = view.root.current())
            for (const auto &row : current->tracks)
                if (row)
                    for (const auto &clip : row->clips)
                        if (clip && geo.clipSelected(clip->id)) {
                            if (clip->posTicks < 0 || clip->lengthTicks < 0 ||
                                clip->lengthTicks > maxTick - clip->posTicks) {
                                view.root.say("A selected clip has an invalid timeline span.");
                                return true;
                            }
                            first = std::min(first, clip->posTicks);
                            latestEnd = std::max(latestEnd, clip->posTicks + clip->lengthTicks);
                            clips.push_back({clip->id, row->id, clip->posTicks});
                        }
        if (clips.size() != geo.selectedClips().size() || clips.empty()) {
            view.root.say("A selected clip is no longer in the Arrangement.");
            return true;
        }
        if (forward && first == maxTick) {
            view.root.say("The clips would move past the end of the timeline.");
            return true;
        }
        const auto delta = geo.nextGridLine(first, forward) - first;
        if (delta == 0)
            return true;
        if (delta > 0 && std::max({latestEnd, sel.start, sel.end}) > maxTick - delta) {
            view.root.say("The clips would move past the end of the timeline.");
            return true;
        }
        for (auto &clip : clips)
            clip.position += delta;
        if (view.root.placeClips(clips, "Move Clips")) {
            sel.start += delta;
            sel.end += delta;
            sel.insert = sel.start;
            view.changed();
            announce(view.root.selectionText());
        }
        return true;
    }
    if (sel.clip != 0) {
        // Move Clips with Arrow Keys, on by default in Live (§41.1.1): the selected clip moves
        // to the next or previous grid line, one undo step a press.
        if (track != nullptr)
            for (const auto &c : track->clips)
                if (c->id == sel.clip) {
                    const auto id = c->id, from = c->posTicks, length = c->lengthTicks;
                    const auto target = geo.nextGridLine(from, forward);
                    if (target != from && view.root.placeClips({{id, sel.track, target}}, "Move Clip")) {
                        sel.insert = sel.start = target;
                        sel.end = target + length;
                        view.changed();
                        announce(view.root.selectionText());
                    }
                    break;
                }
        return true;
    }
    if (sel.start != sel.end) {
        if (moveTimeSelection(geo.nextGridLine(std::min(sel.start, sel.end), forward))) {
            view.locate();
            view.changed();
            announce(view.root.selectionText());
        }
        return true;
    }
    // The insert marker moves by the grid.
    sel.insert = sel.start = sel.end = geo.nextGridLine(sel.marked ? sel.insert : 0, forward);
    sel.marked = true;
    view.locate();
    view.changed();
    announce(view.root.selectionText());
    return true;
}
bool ArrangementCanvas::moveTimeSelection(std::int64_t target) {
    auto &geo = view.geometry;
    auto &sel = geo.selection;
    const auto lo = std::min(sel.start, sel.end), hi = std::max(sel.start, sel.end);
    const auto delta = target - lo;
    if (delta > 0 && hi > std::numeric_limits<std::int64_t>::max() - delta) {
        view.root.say("The selection would move past the end of the timeline.");
        return false;
    }
    if (delta != 0)
        if (const auto current = view.root.current()) {
            auto selected = geo.selectedTracks(view.reader());
            std::erase_if(selected, [&](const auto id) {
                const auto *row = view.reader().findTrack(id);
                return row == nullptr || !geo.shown(id) || (row->kind != "audio" && !isMidiTrack(*row));
            });
            const auto content = rangeContent(current->tracks, selected, lo, hi);
            if (!content.inside.empty() || content.crossing != 0) {
                const clip_edit::RangeSelection range{std::move(selected), {0, lo}, {0, hi}};
                const edit_plan::Options options{view.root.editRate(), edit_plan::capturePolicy(view.root.commandSettings())};
                if (!view.root.commitEdit(clip_edit::prepareMoveRange(view.root.editStore(), range, {0, target}, options)))
                    return false;
            }
        }
    sel.start += delta;
    sel.end += delta;
    sel.insert = target;
    return true;
}
std::int64_t ArrangementCanvas::clipEdge(std::int64_t tick, bool forward) const {
    // The nearest clip edge on the selected track, or locator (manual 6.9: Ctrl/Option with the arrows
    // "snaps the insert marker to locators and the edges of clips in the selected track or tracks").
    auto best = tick;
    const auto consider = [&](std::int64_t edge) {
        if (forward ? edge > tick && (best == tick || edge < best) : edge < tick && (best == tick || edge > best))
            best = edge;
    };
    if (const auto *t = view.reader().findTrack(view.geometry.selection.track))
        for (const auto &c : t->clips) {
            consider(c->posTicks);
            consider(c->posTicks + c->lengthTicks);
        }
    if (const auto *snapshot = view.reader().get())
        for (const auto &l : locatorsOf(*snapshot))
            consider(l.pos);
    return best;
}
namespace {
bool isMidiFileDrop(const juce::String &path) {
    return path.endsWithIgnoreCase(".mid") || path.endsWithIgnoreCase(".smf");
}
} // namespace
bool ArrangementCanvas::isInterestedInFileDrag(const juce::StringArray &files) {
    // One file or several, as Live takes them; what each holds is read on the drop.
    return !files.isEmpty();
}
bool ArrangementCanvas::isInterestedInDragSource(const SourceDetails &details) {
    return view.root.devices.isMovingDevice(details) ||
           !BrowserPanel::droppedFiles(details.description).isEmpty() ||
           view.root.browser.acceptsDeviceDrop(details);
}
void ArrangementCanvas::setDropPreview(DropPreview next) {
    if (next == dropPreview_)
        return;
    repaint(dropPreview_.bounds);
    dropPreview_ = next;
    repaint(dropPreview_.bounds);
}
std::pair<const engine::TrackNode *, juce::Rectangle<int>> ArrangementCanvas::dropLane(int y) const {
    const auto &geo = view.geometry;
    const auto &reader = view.reader();
    const auto rows = geo.rows(reader);
    const auto foot = geo.footStart(rows);
    const int footTop = foot < rows.size() ? geo.footTop(reader) : getHeight();
    int top = 0;
    const auto row = geo.rowAt(reader, y, &top);
    const auto *track = row < rows.size() ? rows[row].track : nullptr;
    if (!track)
        top = std::max(0, geo.tracksHeight(reader) - geo.scrollY);
    const int height = track ? geo.heightFor(track->id) : geo.height;
    if (track && y >= top + height)
        return {}; // the track's additional automation rows accept envelope edits
    const int bottom = std::min(top + height, row >= foot && track ? getHeight() : footTop);
    top = std::max(0, top);
    return {track, {0, top, getWidth(), std::max(0, bottom - top)}};
}
std::int64_t ArrangementCanvas::audioDropTick(int x) const {
    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(x));
    // While files are dropped, Ctrl/Cmd means "each on a track of its own". On macOS the same key
    // also leaves the grid, so here it is taken out: a drop on separate tracks still lands on the grid.
    if (!view.snaps(juce::ModifierKeys::currentModifiers.withoutFlags(juce::ModifierKeys::commandModifier)))
        return raw;
    const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
    return raw - down <= up - raw ? down : up;
}
void ArrangementCanvas::fileDragEnter(const juce::StringArray &files, int x, int y) { fileDragMove(files, x, y); }
void ArrangementCanvas::fileDragMove(const juce::StringArray &files, int x, int y) {
    const auto tick = audioDropTick(x);
    const auto midiFiles = static_cast<int>(std::count_if(files.begin(), files.end(), isMidiFileDrop));
    if (midiFiles > 0 && view.reader().get()) {
        const auto [track, lane] = dropLane(y);
        if (!track || isMidiTrack(*track)) {
            const int left = static_cast<int>(view.geometry.x(tick));
            setDropPreview({lane.withX(left).withWidth(2).getIntersection(getLocalBounds())});
            return;
        }
    }
    previewSampleDrop(y, tick, files.size() - midiFiles, false);
}
void ArrangementCanvas::previewSampleDrop(int y, std::int64_t tick, int files, bool existingTrackOnly) {
    const auto [track, lane] = dropLane(y);
    if (files <= 0 || !view.reader().get() || (existingTrackOnly && !track)) {
        clearDropPreview();
        return;
    }
    if (track && track->kind != "audio") {
        if (files == 1 && view.root.loadSampleInstrument && view.root.sampleInstrumentTarget(track->id))
            setDropPreview({lane, true});
        else
            clearDropPreview();
        return;
    }
    const int left = static_cast<int>(view.geometry.x(tick));
    setDropPreview({lane.withX(left).withWidth(2).getIntersection(getLocalBounds())});
}
void ArrangementCanvas::previewDeviceDrop(int y, bool existingTrackOnly) {
    const auto [track, lane] = dropLane(y);
    setDropPreview({!existingTrackOnly || track ? lane : juce::Rectangle<int>{}, true});
}
void ArrangementCanvas::clearDropPreview() { setDropPreview({}); }
void ArrangementCanvas::fileDragExit(const juce::StringArray &) { clearDropPreview(); }
void ArrangementCanvas::itemDragEnter(const SourceDetails &details) { itemDragMove(details); }
void ArrangementCanvas::itemDragMove(const SourceDetails &details) {
    const bool moving = view.root.devices.isMovingDevice(details);
    if (moving || view.root.browser.acceptsDeviceDrop(details)) {
        previewDeviceDrop(details.localPosition.y, moving);
        return;
    }
    fileDragMove(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
void ArrangementCanvas::itemDragExit(const SourceDetails &) { clearDropPreview(); }
void ArrangementView::dropMovedDevice(const juce::DragAndDropTarget::SourceDetails &details) {
    if (const auto *track = geometry.trackAt(reader(), details.localPosition.y))
        root.devices.moveDroppedDevice(details, track->id);
    else
        root.say("Drop the device on a track to move it there.");
}
void ArrangementCanvas::itemDropped(const SourceDetails &details) {
    clearDropPreview();
    if (details.description.toString() == DevicePanelView::kDeviceMoveDrag) {
        view.dropMovedDevice(details);
        return;
    }
    // A lane targets its track's chain. The empty area below the lanes creates a track.
    if (view.root.browser.acceptsDeviceDrop(details)) {
        const auto [track, bounds] = dropLane(details.localPosition.y);
        if (!bounds.isEmpty())
            view.root.browser.loadDroppedDevice(details, track != nullptr ? track->id : 0);
        return;
    }
    filesDropped(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
void ArrangementCanvas::filesDropped(const juce::StringArray &files, int x, int y) {
    // Live's drop (manual 6.1): the clip starts where the file was dropped, on the nearest grid
    // line while snapping; on a track's lane, that track; below the tracks, a new one. The clip is
    // named after the file, as Live names it.
    clearDropPreview();
    if (files.isEmpty())
        return;
    const auto [track, bounds] = dropLane(y);
    if (bounds.isEmpty())
        return;
    // Several files go one after another on that track, or each on a track of its own with Ctrl/Cmd
    // held, as Live places them; one undo step for them all.
    juce::StringArray audio;
    std::vector<std::pair<std::string, std::string>> midi;
    for (const auto &path : files) {
        // The name through std::filesystem, as for audio: a path written with forward slashes would
        // otherwise name the clip with the whole path on Windows.
        if (isMidiFileDrop(path))
            midi.emplace_back(path.toStdString(), media::pathUtf8(media::pathFromUtf8(path.toStdString()).stem()));
        else
            audio.add(path);
    }
    const auto on = track != nullptr ? track->id : 0;
    const bool onMidi = track != nullptr && isMidiTrack(*track);
    const auto at = audioDropTick(x);
    const bool separate = juce::ModifierKeys::currentModifiers.isCommandDown();
    if (!audio.isEmpty())
        view.root.dropAudioPaths(audio, !midi.empty() && onMidi ? 0 : on, at, separate);
    if (!midi.empty())
        view.root.dropMidiFiles(midi, !audio.isEmpty() && !onMidi ? 0 : on, at, separate);
}
} // namespace adi::ui
