// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_arrangement.hpp"
#include "adi/ui/audio_drop.hpp"
#include "adi/ui/automation_override.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/clip_fade.hpp"
#include "adi/ui/fade_edits.hpp"
#include "adi/edit_plan.hpp"
#include "adi/ui/meter_edits.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/range_edits.hpp"
#include "adi/ui/recording_edits.hpp"
#include "ui_browser.hpp"
#include "ui_controls.hpp"
#include "ui_shell.hpp"
#include <limits>
#include <map>
#include <span>
namespace adi::ui {
ArrangementOverview::ArrangementOverview(ArrangementView &v) : view(v) {
    setTitle("Overview");
    setTooltip("The whole arrangement. Drag sideways to scroll, up or down to zoom; "
               "double-click to see all of it.");
}
namespace {
template <typename FrameAt>
void paintPeakColumns(juce::Graphics &g, const media::MediaPeaks &peaks, int from, int to, float top, float bottom,
                      std::span<const std::uint32_t> channelMap, FrameAt frameAt,
                      double sourceEnd = std::numeric_limits<double>::infinity()) {
    const auto channels = channelMap.empty() ? peaks.channels : static_cast<std::uint32_t>(channelMap.size());
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
            const float y0 = mid - std::clamp(peak->maximum, -1.f, 1.f) * half,
                        y1 = mid - std::clamp(peak->minimum, -1.f, 1.f) * half;
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
void ScrubArea::mouseDown(const juce::MouseEvent &e) {
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
    if (locatorDragged_ != 0) {
        if (e.getDistanceFromDragStart() < 3 && !locatorMoved_)
            return; // a click, not a drag
        locatorMoved_ = true;
        const auto raw = view.geometry.tick(e.position.x) - locatorGrab_;
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        locatorTo_ = std::max<std::int64_t>(0, !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up));
        repaint();
        return;
    }
    if (brace_ != BracePart::None) {
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
    // On a fragmentary bar, Live's two ways to make it whole (manual 6.5, its figure). Both delete or
    // insert time across every track, which waits for the time ops (the …Time commands' kind, N17);
    // the Status Bar says so.
    if (at >= 0 && fragmentAt(at, view.reader().meters()).first >= 0) {
        out.addSeparator();
        out.addItem(juce::PopupMenu::Item("Delete Fragmentary Bar Time").setEnabled(false));
        out.addItem(juce::PopupMenu::Item("Complete Fragmentary Bar (Insert Time)").setEnabled(false));
        view.root.say("Deleting or completing a fragmentary bar inserts or deletes time across the tracks, "
                      "which waits for the time ops.");
    }
    return out;
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
    setTooltip("Click to select a track; drag its lower part to resize it, with Alt to resize "
               "all. Right-click for the track's commands.");
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
               "[Enter] at a Clip's End: Resize Clip\n"
               "[Delete] Delete Clip\n"
               "[0] Deactivate/Activate Clip");
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
                   (c->muted ? ", muted" : "");
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
        y += h;
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
// Live's colour chooser in a track's menu (Live 12 manual 2.3.2): a row of swatches of Live's palette,
// the track's own outlined. Only the palette's first row is measured (ADR-0211 d3), so only it is
// offered; a click colours the selected tracks and closes the menu.
class ColourChooser final : public juce::PopupMenu::CustomComponent {
  public:
    static constexpr int kSwatch = 14, kPitch = 17, kColumns = 14;
    ColourChooser(ArrangementView &view, std::int64_t current)
        : juce::PopupMenu::CustomComponent(false), view_(view), current_(current) {}
    void getIdealSize(int &width, int &height) override {
        width = 8 + kColumns * kPitch;
        height = kSwatch + 8;
    }
    void paint(juce::Graphics &g) override {
        for (int i = 0; i < kColumns; ++i) {
            const juce::Rectangle<float> r(static_cast<float>(4 + i * kPitch), 4.f, kSwatch, kSwatch);
            g.setColour(look::paletteColour(i));
            g.fillRect(r);
            if (i == current_ || i == hover_) {
                g.setColour(i == current_ ? look::textBright : look::textBright.withAlpha(.6f));
                g.drawRect(r.expanded(1.5f), 1.5f);
            }
        }
    }
    void mouseMove(const juce::MouseEvent &e) override {
        if (const int at = swatchAt(e.x); at != hover_) {
            hover_ = at;
            repaint();
        }
    }
    void mouseExit(const juce::MouseEvent &) override {
        hover_ = -1;
        repaint();
    }
    void mouseUp(const juce::MouseEvent &e) override {
        if (const int at = swatchAt(e.x); at >= 0) {
            view_.recolour(at);
            triggerMenuItem();
        }
    }

  private:
    static int swatchAt(int x) noexcept {
        const int i = (x - 4) / kPitch;
        return x >= 4 && i < kColumns && (x - 4) % kPitch < kSwatch ? i : -1;
    }
    ArrangementView &view_;
    std::int64_t current_;
    int hover_ = -1;
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
    // Live's Set Locator, which reads Del on a selected locator, and Previous and Next (manual 6.4).
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
    controls::button(g, set, controls::ButtonState::Rest, selectedLocator != 0 ? "Del" : "Set", look::fontLabel);
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
void ArrangementView::select(int x, int y, bool snapToGrid) {
    if (const auto *t = geometry.trackAt(reader(), y)) {
        geometry.selection.track = t->id;
        geometry.selection.toTrack = 0;
        geometry.selection.picked.clear();
        const auto at = geometry.tick(x);
        // The clip under the mouse is hit where it is; the insert marker snaps (§6.10).
        const auto *c = geometry.hit(*t, at);
        const auto tick = snapToGrid ? geometry.snap(at, false) : at;
        geometry.selection.clip = c ? c->id : 0;
        geometry.selection.pickedClips.clear();
        geometry.selection.insert = tick;
        geometry.selection.marked = true;
        geometry.selection.start = c ? c->posTicks : tick;
        geometry.selection.end = c ? c->posTicks + c->lengthTicks : tick;
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
juce::PopupMenu ArrangementView::headerMenu(std::int64_t contextTrack) {
    auto &commands = root.commands();
    commands.activate(root);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
    menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
    if (geometry.selection.track == 0)
        return menu;
    const auto track = contextTrack != 0 ? contextTrack : geometry.selection.track;
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
        menu.addItem("Assign Track Color to Grouped Tracks and Clips", [safe = SafePointer<ArrangementView>(this), track] {
            if (safe)
                safe->assignGroupColour(track);
        });
        // The way into a group for a return (ADR-0247 d2): a new return inside it, after what it
        // holds, lettered with the group's initial ("DA"). The Create menu's keeps Live's foot.
        menu.addItem("Insert Return Track in Group", [safe = SafePointer<ArrangementView>(this), track] {
            if (safe)
                safe->root.insertReturnTrack(track);
        });
    }
    menu.addCustomItem(1, std::make_unique<ColourChooser>(*this, colourIndex(track)), nullptr,
                       "Colour");
    return menu;
}
void ArrangementView::contextMenu(juce::Point<int> where, bool header, const engine::AutomationLaneNode *lane) {
    if (onContextMenu) {
        onContextMenu(where, header, lane);
        return;
    }
    contextMenuItems(header, lane).showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea(
        juce::Rectangle<int>(where.x, where.y, 1, 1)));
}
const engine::AutomationLaneNode *ArrangementView::automationLaneAt(int y) const {
    if (!root.automationMode())
        return nullptr;
    const auto *current = root.current().get(); // the root holds it: the pointer outlives this call
    const auto *t = current != nullptr ? geometry.trackAt(reader(), y) : nullptr;
    if (!t || geometry.foldedTracks.contains(t->id))
        return nullptr;
    return envelopeOf(*current, root.automationTarget(t->id)).lane;
}
juce::PopupMenu ArrangementView::contextMenuItems(bool header, const engine::AutomationLaneNode *lane) {
    auto menu = header ? headerMenu() : juce::PopupMenu();
    if (!header) {
        auto &commands = root.commands();
        commands.activate(root);
        // A clip's menu (Live 12 manual 6.9): Cut, Copy, Paste and Duplicate, then Split at where it
        // was clicked, above the lanes' items. The lanes' own menu offers Paste.
        if (geometry.selection.clip != 0) {
            for (const auto id : {AppCommands::Cut, AppCommands::Copy, AppCommands::Paste, AppCommands::Duplicate,
                                  AppCommands::DeleteTrack})
                menu.addCommandItem(&commands.manager, id);
            menu.addSeparator();
            menu.addCommandItem(&commands.manager, AppCommands::Split);
            menu.addSeparator();
            // Rename, the clip's (walk-B B14): the same command as Ctrl/Cmd+R, which acts on the
            // selected clip.
            menu.addCommandItem(&commands.manager, AppCommands::RenameTrack);
            menu.addSeparator();
        } else if (!commands.clipClipboard().empty()) {
            menu.addCommandItem(&commands.manager, AppCommands::Paste);
            menu.addSeparator();
        }
        if (geometry.selection.clip == 0 && geometry.selection.start != geometry.selection.end)
            menu.addCommandItem(&commands.manager, AppCommands::DeleteTrack);
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
    const auto current = root.current();
    const auto *group = current ? current->findTrack(groupId != 0 ? groupId : geometry.selection.track) : nullptr;
    if (group == nullptr || group->kind != "group") {
        root.say("Select a group track to give what it holds its colour.");
        return false;
    }
    // Its colour from the CURRENT cell (ADR-0201), so a colour picked a moment ago counts; the one its
    // place gives it otherwise.
    const auto &own = group->color;
    const auto index = own && *own >= 0 && *own < kPaletteColours ? *own : colourIndex(group->id);
    auto edit = colourTracks(current->tracks, heldBy(current->tracks, group->id), index,
                             "Assign Track Color to Grouped Tracks and Clips");
    return edit.ops.empty() || root.submitEdits(std::move(edit.ops));
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
bool ArrangementView::edgeScroll(int x, int y, bool sideways, bool upAndDown) {
    // How far past the edge zone's inner line, as a step: a few points at its rim, up to a track
    // a step well past it.
    auto step = [](int depth) { return depth == 0 ? 0 : (depth > 0 ? 1 : -1) * std::min(48, 2 + std::abs(depth) / 2); };
    bool moved = false;
    if (upAndDown) {
        // The tracks scroll above the foot: its top is their bottom.
        const int bottom = geometry.footTop(reader());
        const int depth = y < kScrollEdge ? y - kScrollEdge : y > bottom - kScrollEdge ? y - (bottom - kScrollEdge) : 0;
        const int before = geometry.scrollY;
        geometry.scrollY = std::clamp(before + step(depth), 0, geometry.maxScroll(reader()));
        moved = geometry.scrollY != before;
    }
    if (sideways) {
        const int width = canvas.getWidth();
        const int depth = x < kScrollEdge ? x - kScrollEdge : x > width - kScrollEdge ? x - (width - kScrollEdge) : 0;
        const double before = geometry.left;
        geometry.pan(step(depth));
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
        if (t->id != renaming_) {
            g.setColour(look::ink);
            g.setFont(look::font(look::fontBody, true));
            const int badgeSpace = t->frozen ? 14 : 0;
            g.drawText(juce::String::fromUTF8(displayName(t->name, row.number()).c_str()), x + 23, y + 2,
                       getWidth() - x - 25 - badgeSpace, 15, juce::Justification::centredLeft);
            if (t->frozen)
                controls::frozenTrackBadge(g, {static_cast<float>(getWidth() - 14), static_cast<float>(y + 3), 11, 11});
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
                }
        y += h;
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
    // One under the other, the header's width each, in a row tall enough for both.
    if (height < 56)
        return {};
    const int x = nameLeft(depth) + 4, w = getWidth() - x - 6;
    if (w < 50)
        return {};
    return {x, top + (record ? 21 : 37), w, 14};
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
    // The fold switch acts on the row under the pointer without choosing it.
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    foldPressed_ = false;
    if (const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
        !e.mods.isPopupMenu() && at < rows.size() &&
        foldSwitch(rows[at].place.depth, top).expanded(2.f).contains(e.position)) {
        resizeId = moveId_ = 0;
        foldPressed_ = true;
        view.toggleFold(rows[at].track->id);
        return;
    }
    // Rec-Q and Play-Q open their menus, and select nothing.
    if (const auto at = view.geometry.rowAt(view.reader(), e.y, &top); at < rows.size() && isMidiTrack(*rows[at].track))
        for (const bool record : {true, false})
            if (quantizeBox(rows[at].place.depth, top, view.geometry.heightFor(rows[at].track->id), record)
                    .contains(e.getPosition())) {
                resizeId = moveId_ = 0;
                quantizeMenu(rows[at].track->id, record)
                    .showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(this).withMousePosition());
                return;
            }
    resizeId = 0;
    moveId_ = 0;
    moving_ = selectOnUp_ = false;
    if (auto *t = view.geometry.trackAt(view.reader(), e.y)) {
        focusIfShowing(*this);
        // The split line at the row's foot resizes it; the rest of the title moves it, Main and
        // the returns aside.
        const auto pressed = view.geometry.rowAt(view.reader(), e.y, &top);
        const bool split = e.y >= top + view.geometry.heightFor(t->id) - kSplitLine;
        const bool plain = !e.mods.isCommandDown() && !e.mods.isShiftDown() && !e.mods.isPopupMenu();
        // Ctrl/Cmd+click picks a track apart from the others, or lets it go again (Live: non-adjacent
        // tracks); Shift+click on another header reaches the selection across the tracks between.
        // A plain press on one of several selected tracks keeps them for a drag; released
        // without one, it selects that track alone.
        if (plain && !split && view.geometry.inSelection(view.reader(), t->id) &&
            view.geometry.selectedTracks(view.reader()).size() > 1)
            selectOnUp_ = true;
        else if (e.mods.isCommandDown() && view.geometry.selection.track != 0 && !e.mods.isPopupMenu()) {
            auto &picked = view.geometry.selection.picked;
            if (t->id != view.geometry.selection.track) {
                if (const auto at = std::find(picked.begin(), picked.end(), t->id); at != picked.end())
                    picked.erase(at);
                else
                    picked.push_back(t->id);
            }
        } else if (e.mods.isShiftDown() && view.geometry.selection.track != 0 && !e.mods.isPopupMenu())
            view.geometry.selection.toTrack = t->id == view.geometry.selection.track ? 0 : t->id;
        else {
            view.geometry.selection.track = t->id;
            view.geometry.selection.toTrack = 0;
            view.geometry.selection.picked.clear();
        }
        // A title chooses tracks. The insert marker can stay at its previous position.
        view.geometry.selection.clearMaterial();
        if (split)
            resizeId = t->id;
        else if (!e.mods.isPopupMenu() && pressed < rows.size() && !rows[pressed].place.main) {
            moveId_ = t->id;
            movingReturn_ = rows[pressed].place.ret;
            // Held still at the lanes' edge, the drag repeats, so they keep scrolling.
            beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
        }
        initialHeight = view.geometry.heightFor(t->id);
        view.changed();
    }
    if (e.mods.isPopupMenu()) {
        resizeId = moveId_ = 0;
        view.contextMenu(e.getScreenPosition(), true);
    }
}
void TrackHeaderList::mouseMove(const juce::MouseEvent &e) {
    // The split line shows the resize cursor, as Live's.
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
    const bool split = at < rows.size() && e.y >= top + view.geometry.heightFor(rows[at].track->id) - kSplitLine;
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
            drop.y = upper ? rowTop : rowTop + h;
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
        if (!row.place.ret) {
            top += h;
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
        top += h;
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
            drop.y += view.geometry.heightFor(rows[i].track->id);
        if (foot < rows.size())
            drop.y = std::min(drop.y, view.geometry.footTop(reader) - 2);
        return drop;
    }
    const auto row = rows[at];
    const auto &place = row.place;
    const auto id = row.track->id;
    const int h = view.geometry.heightFor(id);
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
        drop.y = top + h;
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
        drop.y = top + h;
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
    if (e.mods.isPopupMenu())
        return;
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
    if (at >= rows.size() || foldSwitch(rows[at].place.depth, top).expanded(2.f).contains(e.position))
        return;
    view.root.showDeviceView();
}
void TrackHeaderList::mouseDrag(const juce::MouseEvent &e) {
    if (resizeId) {
        view.geometry.resizeTrack(resizeId, initialHeight + e.getDistanceFromDragStartY(),
                                  e.mods.isAltDown());
        view.frame(true);
    } else if (moveId_ != 0 && (moving_ || std::abs(e.getDistanceFromDragStartY()) > 4)) {
        // Live: "Tracks can be reordered by selecting and dragging them above or below other
        // tracks"; the line shows where they would land.
        moving_ = true;
        selectOnUp_ = false;
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
    if (selectOnUp_ && !moved && dragged != 0) {
        view.geometry.selection.track = dragged;
        view.geometry.selection.toTrack = 0;
        view.geometry.selection.picked.clear();
    }
    selectOnUp_ = false;
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
        if (y >= visibleBottom || y + h <= paintBounds.getY()) {
            y += h;
            continue;
        }
        // Every track the selection covers is lit, as Live lights a selection across tracks.
        const bool selected = geo.inSelection(view.reader(), t->id);
        lanes(y, h - 2, selected);
        g.setColour(look::separator);
        g.fillRect(0, y + h - 2, getWidth(), 2);
        // The time selection lights the selected tracks' lanes, as Live's.
        if (selected && geo.selection.start != geo.selection.end) {
            const float x0 = static_cast<float>(geo.x(std::min(geo.selection.start, geo.selection.end)));
            const float x1 = static_cast<float>(geo.x(std::max(geo.selection.start, geo.selection.end)));
            g.setColour(look::selection.withAlpha(0.85f));
            g.fillRect(x0, static_cast<float>(y), x1 - x0, static_cast<float>(h - 2));
        }
        const auto colour = view.colourFor(row.position);
        if (row.place.group)
            paintGroupLane(g, i, y, h, colour);
        auto drawClip = [&](const engine::ClipNode &clip, bool home = false) {
            const auto *c = &clip;
            // A clip being moved by its bar is drawn where it would land; `home`, where it is.
            const bool along = !home && moving_ != 0 && !geo.selection.pickedClips.empty() && geo.clipSelected(c->id);
            const auto pos = home ? c->posTicks : c->id == moving_ ? movingTo_ : along ? c->posTicks + movingDelta() : c->posTicks;
            const auto length = c->id == resizing_ ? resizeLength_ : c->lengthTicks;
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
            g.fillRect(left, static_cast<float>(y + 1), std::max(1.f, right - left), static_cast<float>(h - 4));
            g.setColour(clipColour);
            g.fillRect(left, static_cast<float>(y + 1), std::max(1.f, right - left), static_cast<float>(kClipBarHeight));
            paintWaveform(g, *c, pos, left, right, static_cast<float>(y + 1 + kClipBarHeight),
                          static_cast<float>(y + h - 3));
            if (!c->audio)
                paintNotes(g, *c, pos, left, right, static_cast<float>(y + 1 + kClipBarHeight),
                           static_cast<float>(y + h - 3));
            // Its name in its title strip, cut at the clip's end, as Live's (the manual's demo
            // arrangement).
            if (!c->name.empty() && right - left > 6.f) {
                g.setColour(look::ink);
                g.setFont(look::font(look::fontSmall));
                g.drawText(juce::String::fromUTF8(c->name.c_str()),
                           juce::Rectangle<float>(left + 3.f, static_cast<float>(y + 1), right - left - 5.f,
                                                  static_cast<float>(kClipBarHeight)),
                           juce::Justification::centredLeft, false);
            }
            g.setColour(clipColour.darker(0.5f));
            g.drawRect(left, static_cast<float>(y + 1), std::max(1.f, right - left),
                       static_cast<float>(h - 4), 1.f);
            // Live tints the selected part of a clip's body, under its title strip (the manual's Cut
            // Time and Consolidate figures).
            if (selected && geo.selection.start != geo.selection.end) {
                const float s0 = std::max(left, static_cast<float>(geo.x(std::min(geo.selection.start, geo.selection.end))));
                const float s1 = std::min(right, static_cast<float>(geo.x(std::max(geo.selection.start, geo.selection.end))));
                if (s1 > s0) {
                    g.setColour(look::selection.withAlpha(0.85f));
                    g.fillRect(s0, static_cast<float>(y + 1 + kClipBarHeight), s1 - s0,
                               static_cast<float>(std::max(0, h - 4 - kClipBarHeight)));
                }
            }
            // Its fades, and their handles (manual 6.8), over the waveform and any selection tint;
            // not while it moves.
            if (c->audio && (home || (c->id != moving_ && !along)) && c->id != resizing_)
                paintFades(g, *c, static_cast<float>(x), static_cast<float>(end), static_cast<float>(y + 1 + kClipBarHeight),
                           static_cast<float>(y + h - 3));
        };
        // A clip dragged across tracks is drawn on the destination lane, using that track's
        // colour if the clip has no colour of its own.
        for (const auto &c : t->clips) {
            // A copy being dragged leaves its original drawn where it is.
            if (copying_ && moving_ != 0 &&
                (c->id == moving_ || (!geo.selection.pickedClips.empty() && geo.clipSelected(c->id))))
                drawClip(*c, true);
            if (const auto to = movingTargetOf(c->id); to == 0 || to == t->id)
                drawClip(*c);
        }
        if (moving_ != 0)
            for (const auto &other : view.reader().tracks())
                if (other->id != t->id)
                    for (const auto &c : other->clips)
                        if (movingTargetOf(c->id) == t->id)
                            drawClip(*c);
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
            double startSeconds = 0., sourceStartFrame = 0., sourceEndFrame = 0.;
            if (havePreview) {
                if (const auto &punch = preview->punchedPlacement; !preview->loop && punch) {
                    const auto frames = std::max(preview->peaks.frames, live->capturedFrames);
                    const auto range = punchedPreviewRange(*punch, frames, preview->peaks.sampleRate, *tempo);
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
                    const auto pass = preview->latestPass(live->capturedFrames);
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
                    if (havePreview) {
                        const auto mapping = std::find_if(preview->tracks.begin(), preview->tracks.end(),
                            [&](const auto &track) { return track.trackId == t->id; });
                        if (mapping != preview->tracks.end() && !mapping->channels.empty()) {
                            g.setColour(look::ink.withAlpha(0.85f));
                            const int from = std::max(0, static_cast<int>(std::floor(left)));
                            const int to = std::min(getWidth(), static_cast<int>(std::ceil(right)));
                            paintPeakColumns(g, preview->peaks, from, to, top + kClipBarHeight,
                                             top + static_cast<float>(h - 4), mapping->channels,
                                             [&](int x) -> std::optional<double> {
                                const auto seconds = tempo->ticksToSeconds(geo.tick(x)) - startSeconds;
                                return sourceStartFrame + std::max(0., seconds * preview->peaks.sampleRate);
                            }, sourceEndFrame);
                        }
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
        y += h;
    }
    if (foot == rows.size())
        dropArea(y, getHeight());
    // The insert marker, a thin blue line in the selected track.
    if (geo.selection.start == geo.selection.end && geo.selection.marked) {
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
                g.setColour(look::handleChosen);
                g.fillRect(static_cast<int>(geo.x(geo.selection.insert)), top, 1, std::max(0, bottom - top));
            }
            top += geo.heightFor(id);
        }
    }
    if (view.root.automationMode())
        paintAutomation(g);
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
    // The clip body's band of every track drawn, as clipBodies walks them; Main's and the returns' too.
    std::vector<LaneBody> out;
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    const auto foot = geo.footStart(rows);
    const int footY = geo.footTop(view.reader());
    int top = -geo.scrollY;
    for (const auto row : rows) {
        if (row.position == foot)
            top = footY;
        const int h = geo.heightFor(row.track->id);
        const float bottom = static_cast<float>(row.position < foot && foot < rows.size() ? std::min(top + h - 3, footY) : top + h - 3);
        const float bodyTop = static_cast<float>(top + 1 + kClipBarHeight);
        if (h > 0 && bottom > bodyTop + 4 && row.track->kind != "group" && !geo.foldedTracks.contains(row.track->id))
            out.push_back({row.track, bodyTop, bottom, static_cast<float>(top + h - 3)});
        top += h;
    }
    return out;
}
namespace {
// A value's height in a lane body, and back: the range's top at the body's top.
float automationY(double value, const AutomationRange &r, float top, float bottom) {
    auto t = (std::clamp(value, r.minimum, r.maximum) - r.minimum) / (r.maximum - r.minimum);
    if (r.flipped)
        t = t < .5 ? 1.0 : 0.0;
    return bottom - static_cast<float>(t) * (bottom - top);
}
double automationValue(float y, const AutomationRange &r, float top, float bottom) {
    auto t = std::clamp(static_cast<double>((bottom - y) / std::max(1.f, bottom - top)), 0.0, 1.0);
    if (r.flipped)
        t = t > .5 ? 0.0 : 1.0;
    return r.minimum + t * (r.maximum - r.minimum);
}
struct AutomationMark {
    juce::Point<float> at;
    bool hold = false;
    std::uint64_t id = 0;
};
juce::Path automationPath(std::span<const AutomationMark> points, float left, float right) {
    juce::Path line;
    line.startNewSubPath(left, points.front().at.y);
    const AutomationMark *previous = nullptr;
    for (const auto &p : points) {
        if (previous && previous->hold)
            line.lineTo(p.at.x, previous->at.y);
        line.lineTo(p.at);
        previous = &p;
    }
    line.lineTo(right, points.back().at.y);
    return line;
}
constexpr float kPointHit = 5.f;
} // namespace
std::optional<ArrangementCanvas::AutomationPick> ArrangementCanvas::automationPointAt(juce::Point<float> at) const {
    const auto current = view.root.current();
    if (!current)
        return std::nullopt;
    for (const auto &body : laneBodies()) {
        if (at.y < body.top - kPointHit || at.y > body.bottom + kPointHit)
            continue;
        const auto target = view.root.automationTarget(body.track->id);
        const auto range = automationRange(target);
        const auto env = envelopeOf(*current, target);
        if (env.data == nullptr)
            continue;
        for (const auto &p : env.data->points)
            if (std::abs(static_cast<float>(view.geometry.x(p.time)) - at.x) <= kPointHit &&
                std::abs(automationY(p.value, range, body.top, body.scaleBottom) - at.y) <= kPointHit)
                return AutomationPick{body.track->id, p.id};
    }
    return std::nullopt;
}
void ArrangementCanvas::paintAutomation(juce::Graphics &g) const {
    // Each track's Volume envelope across its lane: the line, and a square on each breakpoint; with no
    // breakpoint, the fader's level as a line. The dragged point where the mouse has it.
    const auto current = view.root.current();
    if (!current)
        return;
    const auto &geo = view.geometry;
    for (const auto &body : laneBodies()) {
        // The chosen target's envelope (DF-W37), the track's Volume until another is chosen.
        const auto target = view.root.automationTarget(body.track->id);
        const auto range = automationRange(target);
        const auto env = envelopeOf(*current, target);
        // Drawn inside the lane's visible strip only: a lane under the foot is not drawn over it.
        juce::Graphics::ScopedSaveState keep(g);
        g.reduceClipRegion(0, static_cast<int>(body.top) - 4, getWidth(), static_cast<int>(body.bottom - body.top) + 8);
        std::vector<AutomationMark> points;
        if (env.data != nullptr)
            for (const auto &p : env.data->points) {
                const bool dragged = pointDrag_ && pointDrag_->track == body.track->id && pointDrag_->point == p.id;
                points.push_back({{static_cast<float>(geo.x(dragged ? pointTick_ : p.time)),
                                   automationY(dragged ? pointValue_ : p.value, range, body.top, body.scaleBottom)},
                                  range.flipped || p.curve == 0, p.id});
            }
        if (pointDrag_ && pointDrag_->track == body.track->id)
            std::stable_sort(points.begin(), points.end(),
                             [](const auto &a, const auto &b) { return a.at.x < b.at.x; });
        const float right = static_cast<float>(getWidth());
        const auto paintStored = [&] {
            g.setColour(look::record.withAlpha(.9f));
            if (points.empty()) {
                const float y = automationY(view.root.automationCurrent(target), range, body.top, body.scaleBottom);
                g.drawLine(0, y, right, y, 1.f);
                return;
            }
            g.strokePath(automationPath(points, 0, right), juce::PathStrokeType(1.f));
            for (const auto &p : points) {
                const auto box = juce::Rectangle<float>(7, 7).withCentre(p.at);
                const bool chosen = chosenPoint_ && chosenPoint_->track == body.track->id &&
                                    chosenPoint_->point == p.id;
                g.setColour(chosen ? look::handleChosen : look::panel);
                g.fillRect(box);
                g.setColour(look::record);
                g.drawRect(box, 1.f);
            }
        };
        const auto *stroke = automationStroke_ ? &*automationStroke_ : nullptr;
        if (!stroke || stroke->cancelled || stroke->track != body.track->id || stroke->target != target ||
            stroke->generation != view.root.committedGeneration() || !view.root.drawMode() || stroke->values.empty()) {
            paintStored();
            continue;
        }
        // The draft replaces just its time range in the lane until the stroke is committed.
        const auto left = static_cast<float>(geo.x(stroke->values.begin()->first));
        const auto end = static_cast<float>(geo.x(stroke->end));
        {
            juce::Graphics::ScopedSaveState outside(g);
            const auto x0 = static_cast<int>(std::clamp(std::floor(left), 0.f, right));
            const auto x1 = static_cast<int>(std::clamp(std::ceil(end), 0.f, right));
            g.excludeClipRegion({x0, 0, x1 - x0, getHeight()});
            paintStored();
        }
        std::vector<AutomationMark> draft;
        draft.reserve(stroke->values.size());
        for (const auto &[tick, value] : stroke->values)
            draft.push_back({{static_cast<float>(geo.x(tick)), automationY(value, range, body.top, body.scaleBottom)},
                             stroke->stepped || range.flipped});
        g.setColour(look::record);
        g.strokePath(automationPath(draft, left, std::max(end, left + 1.f)), juce::PathStrokeType(1.f));
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
        auto target = view.root.automationTarget(body.track->id);
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
bool ArrangementCanvas::automationDown(const juce::MouseEvent &e) {
    // In Automation Mode a click on a breakpoint chooses it and a drag moves it; on the envelope's line
    // it adds one, on the grid as the view snaps (Live 12 manual 25.3, 25.5.2).
    if (!view.root.automationMode() || e.mods.isPopupMenu())
        return false;
    if (view.root.drawMode())
        return beginAutomationStroke(e);
    const auto at = e.position;
    if (const auto hit = automationPointAt(at)) {
        chosenPoint_ = hit;
        pointDrag_ = hit;
        pointTick_ = 0;
        if (const auto current = view.root.current())
            if (const auto env = envelopeOf(*current, view.root.automationTarget(hit->track)); env.data != nullptr)
                for (const auto &p : env.data->points)
                    if (p.id == hit->point) {
                        pointTick_ = p.time;
                        pointValue_ = p.value;
                    }
        repaint();
        return true;
    }
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
    const auto target = view.root.automationTarget(track);
    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(at.x));
    const auto tick = view.snaps(e.mods) ? view.geometry.snap(raw, false) : raw;
    auto value = view.root.automationCurrent(target);
    if (const auto current = view.root.current())
        if (const auto env = envelopeOf(*current, target); env.data != nullptr)
            value = envelopeValue(*env.data, tick, target.stepped).value_or(value);
    pointAdded_ = true;
    if (view.root.addAutomationPoint(track, tick, value))
        if (const auto current = view.root.current())
            if (const auto env = envelopeOf(*current, target); env.data != nullptr)
                for (const auto &p : env.data->points)
                    if (p.time == tick && (!pointDrag_ || p.id > pointDrag_->point)) {
                        // The newest point at the tick is the one added: chosen, and dragged.
                        pointDrag_ = chosenPoint_ = AutomationPick{track, p.id};
                        pointTick_ = p.time;
                        pointValue_ = p.value;
                    }
    repaint();
    return true;
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
        const auto target = view.root.automationTarget(body.track->id);
        const auto range = automationRange(target);
        const auto env = envelopeOf(*current, target);
        std::vector<AutomationMark> points;
        if (env.data != nullptr)
            for (const auto &p : env.data->points)
                points.push_back({{static_cast<float>(view.geometry.x(p.time)),
                                   automationY(p.value, range, body.top, body.scaleBottom)},
                                  range.flipped || p.curve == 0});
        if (points.empty())
            points.push_back({{0, automationY(view.root.automationCurrent(target), range, body.top, body.scaleBottom)}});
        juce::Point<float> nearest;
        automationPath(points, 0, static_cast<float>(getWidth())).getNearestPoint(at, nearest);
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
        top += h;
    }
    return out;
}
bool ArrangementCanvas::fadeHandlesShown(const engine::ClipNode &c) const {
    return c.audio && !view.root.automationMode() && (showFades || c.id == hoverClip_ || view.geometry.clipSelected(c.id) ||
                       (fadeDrag_ && fadeDrag_->clip == c.id));
}
void ArrangementCanvas::fadesOf(const engine::ClipNode &c, std::int64_t &in, std::int64_t &out,
                                std::optional<double> &inTension, std::optional<double> &outTension) const {
    in = c.fadeInTicks;
    out = c.fadeOutTicks;
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
        (fadeDrag_->in ? in : out) = fadeLimit(c, fadeDrag_->in, fadeTicks_);
}
void ArrangementCanvas::paintFades(juce::Graphics &g, const engine::ClipNode &c, float left, float right, float top,
                                   float bottom) const {
    // Live's fades (the manual's Adjusted Fades figure): the part of the clip the fade takes away
    // darkened above its curve, the curve drawn; with the handles shown, a square at each fade's
    // peak and a diamond on each curve.
    if (bottom - top < 8.f || right - left < 4.f)
        return;
    std::int64_t in = 0, out = 0;
    std::optional<double> inTension, outTension;
    fadesOf(c, in, out, inTension, outTension);
    const auto &geo = view.geometry;
    const float xIn = static_cast<float>(geo.x(c.posTicks + in)), xOut = static_cast<float>(geo.x(c.posTicks + c.lengthTicks - out));
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
    draw(left, xIn, true, inCurve, inTension);
    draw(xOut, right, false, outCurve, outTension);
    if (!fadeHandlesShown(c))
        return;
    const auto square = [&](float x, bool chosen) {
        const auto box = juce::Rectangle<float>(x - 3.5f, top, 7.f, 7.f);
        g.setColour(chosen ? look::handleChosen : look::handle);
        g.fillRect(box);
        g.setColour(look::ink);
        g.drawRect(box, 1.f);
    };
    const auto diamond = [&](float x, float y, bool chosen) {
        juce::Path d;
        d.addQuadrilateral(x, y - 4.f, x + 4.f, y, x, y + 4.f, x - 4.f, y);
        g.setColour(chosen ? look::handleChosen : look::handle);
        g.fillPath(d);
        g.setColour(look::ink);
        g.strokePath(d, juce::PathStrokeType(1.f));
    };
    const auto chosen = [&](bool fadeIn, bool curve) {
        const auto &h = fadeDrag_ ? fadeDrag_ : selectedFade_;
        return h && h->clip == c.id && h->in == fadeIn && h->curve == curve;
    };
    square(std::max(left + 3.5f, xIn), chosen(true, false));
    square(std::min(right - 3.5f, xOut), chosen(false, false));
    if (in > 0)
        diamond((left + xIn) / 2.f, curveY(engine::clipFadeShape(0.5, inCurve, inTension)), chosen(true, true));
    if (out > 0)
        diamond((xOut + right) / 2.f, curveY(engine::clipFadeShape(0.5, outCurve, outTension)), chosen(false, true));
}
std::optional<juce::Point<float>> ArrangementCanvas::fadeHandlePoint(const FadeHandle &h) const {
    for (const auto &b : clipBodies())
        if (b.clip->id == h.clip)
            return fadeHandlePoint(h, b);
    return std::nullopt;
}
std::optional<juce::Point<float>> ArrangementCanvas::fadeHandlePoint(const FadeHandle &h, const ClipBody &b) const {
    {
        if (b.clip->id != h.clip || !fadeHandlesShown(*b.clip) || b.bottom - b.top < 8.f)
            return std::nullopt;
        std::int64_t in = 0, out = 0;
        std::optional<double> inTension, outTension;
        fadesOf(*b.clip, in, out, inTension, outTension);
        const auto &geo = view.geometry;
        const float xIn = static_cast<float>(geo.x(b.clip->posTicks + in));
        const float xOut = static_cast<float>(geo.x(b.clip->posTicks + b.clip->lengthTicks - out));
        if (!h.curve)
            return juce::Point<float>(h.in ? std::max(b.left + 3.5f, xIn) : std::min(b.right - 3.5f, xOut), b.top + 3.5f);
        if ((h.in ? in : out) == 0)
            return std::nullopt;
        const bool bending = fadeDrag_ && fadeDrag_->curve && fadeDrag_->clip == h.clip && fadeDrag_->in == h.in;
        const auto curve = bending ? kBentFadeCurve : h.in ? b.clip->fadeInCurve : b.clip->fadeOutCurve;
        const auto shape = engine::clipFadeShape(0.5, curve, h.in ? inTension : outTension);
        return juce::Point<float>(h.in ? (b.left + xIn) / 2.f : (xOut + b.right) / 2.f,
                                  b.bottom - static_cast<float>(shape) * (b.bottom - b.top));
    }
}
std::optional<ArrangementCanvas::FadeHandle> ArrangementCanvas::fadeHandleAt(int x, int y) const {
    // The nearest shown handle within a few pixels; the curve's diamond before the squares, which
    // sit at the clip's top.
    const juce::Point<float> at(static_cast<float>(x), static_cast<float>(y));
    for (const auto &b : clipBodies()) {
        if (!fadeHandlesShown(*b.clip))
            continue;
        for (const bool curve : {true, false})
            for (const bool in : {true, false}) {
                const FadeHandle h{b.clip->id, b.track->id, in, curve};
                if (const auto p = fadeHandlePoint(h, b); p && p->getDistanceFrom(at) <= 6.f)
                    return h;
            }
    }
    return std::nullopt;
}
void ArrangementCanvas::mouseExit(const juce::MouseEvent &) {
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
    // F released where the canvas cannot hear it: the handles go back.
    if (showFades && !view.root.input.down('f')) {
        showFades = false;
        repaint();
    }
}
bool ArrangementCanvas::keyStateChanged(bool) {
    // F up: the handles go back to the clip under the mouse and the selected ones.
    if (showFades && !view.root.input.down('f')) {
        showFades = false;
        repaint();
    }
    return false;
}
void ArrangementCanvas::paintWaveform(juce::Graphics &g, const engine::ClipNode &clip, std::int64_t pos, float left,
                                      float right, float top, float bottom) const {
    // Live's audio clip (manual 6.1, chapter 9): its waveform across the body, each channel in a
    // band of its own, inside the clip's source window only. The colour is ADI's until measured.
    if (view.waveforms == nullptr || !clip.audio || bottom - top < 4.f)
        return;
    const auto peaks = view.waveforms->peaks(*clip.audio);
    if (!peaks || peaks->channels == 0 || peaks->sampleRate == 0)
        return;
    const auto *snapshot = view.reader().get();
    const auto &geo = view.geometry;
    auto moved = clip;
    moved.posTicks = pos; // drawn where a drag would put it
    const ClipSourceMap source(moved, snapshot != nullptr ? snapshot->tempo.get() : nullptr,
                               static_cast<double>(peaks->sampleRate));
    g.setColour(look::ink.withAlpha(clip.muted ? 0.4f : 0.85f));
    const int from = std::max(0, static_cast<int>(std::floor(left))),
              to = std::min(getWidth(), static_cast<int>(std::ceil(right)));
    paintPeakColumns(g, *peaks, from, to, top, bottom, {},
                     [&](int x) { return source.frameAt(geo.tick(x)); });
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
void ArrangementCanvas::paintNotes(juce::Graphics &g, const engine::ClipNode &clip, std::int64_t pos, float left,
                                   float right, float top, float bottom) const {
    if (bottom - top < 4.f || right <= left || !clip.midiPreview || clip.midiPreview->bars.empty())
        return;
    const auto &held = *clip.midiPreview;
    const auto &geo = view.geometry;
    // The keys in use over the body's height, never fewer than an octave so one note is not a slab.
    const int keys = std::max(12, held.high - held.low + 1);
    const int lowest = held.low - (keys - (held.high - held.low + 1)) / 2;
    const float inner = bottom - top - 2.f;
    const float pitch = inner / static_cast<float>(keys);
    const float thick = std::clamp(pitch - (pitch >= 3.f ? 1.f : 0.f), 1.f, 4.f);
    const std::int64_t clipEnd = pos + clip.lengthTicks;
    auto bar = [&](std::int64_t source, const engine::MidiClipPreview::Bar &n) {
        const auto start = pos + (source - held.startMarker);
        const auto end = std::min(start + n.duration, clipEnd);
        if (end <= pos || start >= clipEnd)
            return;
        const float x0 = std::max(left, static_cast<float>(geo.x(std::max(start, pos)))),
                    x1 = std::min(right, static_cast<float>(geo.x(end)));
        if (x1 <= left || x0 >= right)
            return;
        const float y = top + 1.f + inner - (static_cast<float>(n.key - lowest) + 1.f) * pitch + (pitch - thick) * .5f;
        g.setColour(look::ink.withAlpha(clip.muted || !n.active ? 0.35f : 0.9f));
        g.fillRect(x0, y, std::max(1.f, x1 - x0), thick);
    };
    for (const auto &n : held.bars) {
        if (held.loopLength > 0) {
            // A looped clip plays to the loop's end once, then the loop again and again: what lies
            // after the loop is never heard, and what lies in it is drawn at each pass.
            if (n.start >= held.loopStart + held.loopLength)
                continue;
            bar(n.start, n);
            if (n.start >= held.loopStart)
                for (std::int64_t pass = 1; pass < 4096; ++pass) {
                    const auto source = n.start + pass * held.loopLength;
                    if (pos + (source - held.startMarker) >= clipEnd)
                        break;
                    bar(source, n);
                }
        } else {
            bar(n.start, n);
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
    view.followPaused = false; // a click in the Arrangement starts Follow again (§6.2)
    last = e.position;
    moving_ = resizing_ = 0;
    copying_ = false;
    keyResize_ = false; // a click ends a resize begun from the keyboard, as Esc does
    selectedFade_.reset(); // a click chooses a fade handle again, or nothing
    focusLost(focusChangedDirectly);
    panning = e.mods.isCommandDown() && e.mods.isAltDown();
    automationStroke_.reset();
    pointDrag_.reset();
    chosenPoint_.reset(); // chosen again below when the press is on a breakpoint
    pointAdded_ = false;
    if (!panning && automationDown(e))
        return;
    if (e.mods.isPopupMenu()) {
        view.select(e.x, e.y, view.snaps(e.mods));
        // In Automation Mode the menu is the envelope's too: its lane's Re-Enable Automation (AU-04).
        view.contextMenu(e.getScreenPosition(), false, view.automationLaneAt(e.y));
    } else if (!panning) {
        // Held still at the lanes' edge, a drag repeats, so they keep scrolling.
        beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
        // A fade's handle (manual 6.8): its length or its curve, the clip chosen with it.
        if (const auto h = fadeHandleAt(e.x, e.y)) {
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
                    fadeTicks_ = h->in ? b.clip->fadeInTicks : b.clip->fadeOutTicks;
                    fadeTensionFrom_ = fadeTension_ = (h->in ? b.clip->fadeInTension : b.clip->fadeOutTension).value_or(0.0);
                    fadeFromY_ = e.y;
                }
            view.changed();
            return;
        }
        // Only a clip's bar moves it, not its body; its right edge changes its length (§6.7).
        std::int64_t track = 0;
        bool edge = false;
        if (const auto *c = barAt(e.x, e.y, track, edge); c != nullptr && edge) {
            view.geometry.selection.track = track;
            view.geometry.selection.toTrack = 0;
            view.geometry.selection.picked.clear();
            view.geometry.selection.clip = c->id;
            view.geometry.selection.pickedClips.clear();
            view.geometry.selection.start = c->posTicks;
            view.geometry.selection.end = c->posTicks + c->lengthTicks;
            resizing_ = c->id;
            resizeStart_ = c->posTicks;
            resizeFrom_ = resizeLength_ = c->lengthTicks;
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
        view.select(e.x, e.y, view.snaps(e.mods));
        if (const auto *c = barAt(e.x, e.y, track, edge); c != nullptr && c->id == view.geometry.selection.clip) {
            moving_ = c->id;
            movingFrom_ = movingTo_ = c->posTicks;
            movingSource_ = movingTrack_ = track;
            grabTicks_ = view.geometry.tick(e.x) - c->posTicks;
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
const engine::ClipNode *ArrangementCanvas::barAt(int x, int y, std::int64_t &track, bool &edge) const {
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
        if (!covered && geo.shown(t->id) && y >= top + 1 && y < top + 1 + kClipBarHeight)
            for (const auto &c : t->clips) {
                const int left = static_cast<int>(geo.x(c->posTicks));
                const int right = static_cast<int>(geo.x(c->posTicks + c->lengthTicks));
                if (x >= left && x < right + kClipEdge / 2) {
                    track = t->id;
                    edge = x >= right - kClipEdge;
                    return c.get();
                }
            }
        top += geo.heightFor(t->id);
    }
    return nullptr;
}
void ArrangementCanvas::mouseMove(const juce::MouseEvent &e) {
    focusLost(focusChangedDirectly);
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
    bool edge = false;
    const auto *c = barAt(e.x, e.y, track, edge);
    setMouseCursor(c != nullptr && edge ? juce::MouseCursor::LeftRightResizeCursor
                                        : juce::MouseCursor::NormalCursor);
}
void ArrangementCanvas::mouseDrag(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu())
        return;
    if (automationStroke_) {
        if (view.root.automationMode() && view.root.drawMode())
            updateAutomationStroke(e);
        return;
    }
    if (pointDrag_) {
        // A breakpoint follows the mouse: in time on the grid as the view snaps, in value up and down,
        // inside its lane; nothing is written until the mouse lets go.
        for (const auto &body : laneBodies())
            if (body.track->id == pointDrag_->track) {
                const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.position.x));
                pointTick_ = view.snaps(e.mods) ? view.geometry.snap(raw, false) : raw;
                pointValue_ = automationValue(e.position.y, automationRange(view.root.automationTarget(body.track->id)),
                                              body.top, body.scaleBottom);
            }
        repaint();
        return;
    }
    if (pointAdded_)
        return; // the press added a breakpoint it could not take up; its drag edits nothing else
    if (fadeDrag_) {
        // A peak's square follows the mouse along the clip; a curve's diamond, up and down.
        if (fadeDrag_->curve) {
            fadeTension_ = std::clamp(fadeTensionFrom_ - (e.y - fadeFromY_) / 60.0, -1.0, 1.0);
        } else {
            for (const auto &b : clipBodies())
                if (b.clip->id == fadeDrag_->clip) {
                    const auto at = view.geometry.tick(e.x);
                    fadeTicks_ = fadeDrag_->in ? at - b.clip->posTicks : b.clip->posTicks + b.clip->lengthTicks - at;
                    fadeTicks_ = fadeLimit(*b.clip, fadeDrag_->in, fadeTicks_);
                }
        }
        repaint();
        return;
    }
    if (resizing_ != 0) {
        // The clip's end follows the mouse, to the nearest grid line while snapping, never
        // shorter than one grid step; at the lanes' side the view scrolls along.
        const bool scrolled = e.mouseWasDraggedSinceMouseDown() && view.edgeScroll(e.x, e.y, true, false);
        const auto start = resizeStart_;
        const auto step = static_cast<std::int64_t>(view.geometry.gridQuarters() * textproj::kPPQ);
        const auto raw = view.geometry.tick(e.x);
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        const auto end = !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up);
        resizeLength_ = std::max(std::max<std::int64_t>(1, step), end - start);
        if (scrolled)
            view.frame(true);
        repaint();
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
        // Dragged across lanes, the selection reaches every track between (Live 12 manual 6.9); at
        // the lanes' edge they scroll, and it reaches on.
        if (e.mouseWasDraggedSinceMouseDown())
            view.edgeScroll(e.x, e.y, true);
        if (const auto *over = view.geometry.trackAt(view.reader(), e.y))
            view.geometry.selection.toTrack = over->id == view.geometry.selection.track ? 0 : over->id;
        // A time selection covers whole grid cells while snapping (§6.10).
        const auto at = view.geometry.tick(e.x);
        view.geometry.selection.end =
            view.snaps(e.mods) ? view.geometry.snap(at, at >= view.geometry.selection.start) : at;
    }
    view.frame(true);
}
void ArrangementCanvas::mouseUp(const juce::MouseEvent &e) {
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
    if (pointDrag_) {
        // One drag, one undo step; a click that did not move writes nothing.
        const auto drag = *pointDrag_;
        pointDrag_.reset();
        if (e.mouseWasDraggedSinceMouseDown() && !view.geometry.foldedTracks.contains(drag.track))
            view.root.moveAutomationPoint(drag.track, drag.point, pointTick_, pointValue_);
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
        const bool changed = dragged != nullptr &&
                             (h.curve ? (h.in ? dragged->fadeInTension : dragged->fadeOutTension).value_or(0.0) != fadeTension_
                                      : (h.in ? dragged->fadeInTicks : dragged->fadeOutTicks) != fadeTicks_);
        if (changed) {
            if (const auto current = view.root.current()) {
                auto edit = dragFades(current->tracks, clips, h.in, h.curve ? std::nullopt : std::optional<std::int64_t>(fadeTicks_),
                                      h.curve ? std::optional<double>(fadeTension_) : std::nullopt);
                if (!edit.ops.empty())
                    view.root.submitEdits(std::move(edit.ops));
            }
        }
        view.changed();
        return;
    }
    if (resizing_ != 0 && !keyResize_) {
        const auto clip = resizing_, length = resizeLength_;
        resizing_ = 0;
        // One drag, one undo step; the start, and the audio under it, stay where they are.
        auto &sel = view.geometry.selection;
        if (length != resizeFrom_ &&
            clipEdit("clip.resize", {{"id", clip}, {"pos", resizeStart_}, {"length", length}}, "Resize Clip"))
            sel.end = resizeStart_ + length;
        repaint();
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
bool ArrangementCanvas::clipEdit(const char *op, Payload payload, const char *label) {
    return view.root.submitEdit(op, std::move(payload), label);
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
    if (content.crossing > 0 && !deactivate) {
        clip_edit::RangeSelection range{selected, {0, std::min(sel.start, sel.end)}, {0, std::max(sel.start, sel.end)}};
        const edit_plan::Options options{view.root.editRate(), edit_plan::capturePolicy(view.root.commandSettings())};
        auto prepared = clip_edit::prepareDeleteRange(view.root.editStore(), range, options);
        if (prepared.ok) {
            if (!view.root.commitEdit(std::move(prepared)))
                return false;
            view.changed();
            return true;
        }
        // The engine will not trim here (a track whose automation follows its clips, a frozen track,
        // the editing settings): the whole clips inside go as before, and the Status Bar says why the
        // crossing ones stay.
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
    if (!view.root.submitEdits(std::move(edit.ops)))
        return false;
    if (!deactivate) {
        sel.insert = sel.start;
        sel.clearMaterial();
        view.changed();
    }
    return true;
}
void ArrangementCanvas::mouseDoubleClick(const juce::MouseEvent &e) {
    // Live: a double-click on a clip opens it in the Clip View (manual 8).
    if (e.mods.isPopupMenu())
        return;
    // In Automation Mode a double-click on a breakpoint deletes it (manual 25.3).
    if (view.root.automationMode()) {
        if (view.root.drawMode())
            return;
        if (const auto hit = automationPointAt(e.position)) {
            pointDrag_.reset();
            chosenPoint_.reset();
            view.root.deleteAutomationPoint(hit->track, hit->point);
            repaint();
            return;
        }
        // Elsewhere on a lane a double-click is the envelope's too: off the line it adds a breakpoint
        // there (manual 25.5.2; on the line the first press added it); it neither makes a clip nor opens one.
        for (const auto &body : laneBodies())
            if (e.position.y >= body.top && e.position.y <= body.bottom && !e.mods.isShiftDown() && !e.mods.isCommandDown()) {
                if (!automationLineAt(e.position)) {
                    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.position.x));
                    view.root.addAutomationPoint(body.track->id, view.snaps(e.mods) ? view.geometry.snap(raw, false) : raw,
                                                 automationValue(e.position.y, automationRange(view.root.automationTarget(body.track->id)),
                                                                 body.top, body.scaleBottom));
                    repaint();
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
bool ArrangementCanvas::keyPressed(const juce::KeyPress &k) {
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();
    if (automationStroke_ && code == juce::KeyPress::escapeKey) {
        automationStroke_->cancelled = true;
        repaint();
        return true;
    }
    // Delete or Backspace takes the chosen breakpoint away (Automation Mode).
    if (chosenPoint_ && !view.geometry.foldedTracks.contains(chosenPoint_->track) && view.root.automationMode() &&
        !view.loopSelected && view.selectedLocator == 0 &&
        view.selectedMeter < 0 && (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) &&
        !mods.isAnyModifierKeyDown()) {
        const auto chosen = *chosenPoint_;
        chosenPoint_.reset();
        view.root.deleteAutomationPoint(chosen.track, chosen.point);
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
    // A clip's length from the keyboard (online §41.4.1.3): the arrows move its end by the
    // grid, Enter keeps it as one undo step, Esc or any other key puts it back.
    if (keyResizing()) {
        if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && !mods.isAnyModifierKeyDown()) {
            const auto step = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(view.geometry.gridQuarters() * textproj::kPPQ));
            const auto end = view.geometry.nextGridLine(resizeStart_ + resizeLength_,
                                                        code == juce::KeyPress::rightKey);
            resizeLength_ = std::max(step, end - resizeStart_);
            repaint();
            announce("Length " + juce::String(static_cast<double>(resizeLength_) / textproj::kPPQ, 2) + " beats");
            return true;
        }
        const auto clip = resizing_, length = resizeLength_;
        resizing_ = 0;
        keyResize_ = false;
        repaint();
        if (code == juce::KeyPress::returnKey) {
            if (length != resizeFrom_ &&
                clipEdit("clip.resize", {{"id", clip}, {"pos", resizeStart_}, {"length", length}}, "Resize Clip"))
                sel.insert = sel.start = sel.end = resizeStart_ + length;
            view.changed();
            announce(view.root.selectionText());
            return true;
        }
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
    // Enter with the insert marker at a clip's end changes its length from the keyboard; at
    // its start it would move the audio under it, which waits for a content offset.
    if (code == juce::KeyPress::returnKey && !mods.isAnyModifierKeyDown() && sel.start == sel.end && sel.marked)
        if (const auto *t = view.reader().findTrack(sel.track)) {
            for (const auto &c : t->clips)
                if (sel.insert == c->posTicks + c->lengthTicks) {
                    resizing_ = c->id;
                    keyResize_ = true;
                    resizeStart_ = c->posTicks;
                    resizeFrom_ = resizeLength_ = c->lengthTicks;
                    announce("Resizing " + describe(sel.track, c->id) +
                             ". Left and right change its length; Enter keeps it, Esc cancels.");
                    repaint();
                    return true;
                }
            for (const auto &c : t->clips)
                if (sel.insert == c->posTicks) {
                    view.root.say("Moving a clip's start waits for a content offset in clip.resize; "
                                  "without one its audio would slide.");
                    return true;
                }
        }
    // Delete on a selected time signature marker (Live 12 manual 6.5).
    if (view.selectedMeter >= 0 && (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)) {
        view.root.deleteMeter(view.selectedMeter);
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
        // A time selection moves by the grid when it holds no clip. Part of a clip moves once Split
        // has cut it there (which selects the part inside); a whole clip moves once selected.
        const auto lo = std::min(sel.start, sel.end), hi = std::max(sel.start, sel.end);
        if (track != nullptr) {
            bool crosses = false, holds = false;
            for (const auto &c : track->clips) {
                const auto a = c->posTicks, b = c->posTicks + c->lengthTicks;
                if (a < hi && b > lo)
                    (a < lo || b > hi ? crosses : holds) = true;
            }
            if (crosses || holds) {
                view.root.say(crosses ? "The arrow keys move whole clips: split the clip at the selection first "
                                        "(Ctrl/Cmd+E), which selects the part inside."
                                      : "The arrow keys move a selected clip: click the clip to move it.");
                return true;
            }
        }
        const auto delta = geo.nextGridLine(lo, forward) - lo;
        sel.start += delta;
        sel.end += delta;
        sel.insert = lo + delta;
        view.locate();
        view.changed();
        announce(view.root.selectionText());
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
    const int bottom = std::min(top + height, row >= foot && track ? getHeight() : footTop);
    top = std::max(0, top);
    return {track, {0, top, getWidth(), std::max(0, bottom - top)}};
}
std::int64_t ArrangementCanvas::audioDropTick(int x) const {
    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(x));
    if (!view.snaps(juce::ModifierKeys::currentModifiers))
        return raw;
    const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
    return raw - down <= up - raw ? down : up;
}
void ArrangementCanvas::fileDragEnter(const juce::StringArray &files, int x, int y) { fileDragMove(files, x, y); }
void ArrangementCanvas::fileDragMove(const juce::StringArray &files, int x, int y) {
    previewSampleDrop(y, audioDropTick(x), files.size(), false);
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
        const auto *track = view.geometry.trackAt(view.reader(), details.localPosition.y);
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
    const auto *track = view.geometry.trackAt(view.reader(), y);
    // Several files go one after another on that track, or each on a track of its own with Ctrl/Cmd
    // held, as Live places them; one undo step for them all.
    view.root.dropAudioPaths(files, track != nullptr ? track->id : 0, audioDropTick(x),
                            juce::ModifierKeys::currentModifiers.isCommandDown());
}
} // namespace adi::ui
