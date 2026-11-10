// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_arrangement.hpp"
#include "adi/ui/meter_edits.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/midi_tracks.hpp"
#include "adi/ui/range_edits.hpp"
#include "ui_browser.hpp"
#include "ui_shell.hpp"
#include <limits>
namespace adi::ui {
ArrangementOverview::ArrangementOverview(ArrangementView &v) : view(v) {
    setTitle("Overview");
    setTooltip("The whole arrangement. Drag sideways to scroll, up or down to zoom; "
               "double-click to see all of it.");
}
namespace {
// Live's lanes: grey, every other sixteen grid steps a shade lighter, lighter again
// for the selected track, with the grid (§6.10) a quarter darker than the lane under it.
void paintLanes(juce::Graphics &g, const ArrangementGeometry &geo, int width, int top, int height,
                bool selected) {
    // The shading follows the plain step, so triplets keep it on the bars.
    const double grid = geo.gridQuarters();
    const double group = geo.plainGridQuarters() * 16;
    for (double q = std::floor(geo.left / group) * group; q < geo.left + width / geo.scale; q += group) {
        const bool alt = static_cast<std::int64_t>(std::llround(q / group)) % 2 != 0;
        const auto lane = selected ? (alt ? look::laneSelectedAlt : look::laneSelected)
                                   : (alt ? look::laneAlt : look::lane);
        const double x0 = (q - geo.left) * geo.scale, x1 = x0 + group * geo.scale;
        g.setColour(lane);
        g.fillRect(static_cast<float>(x0), static_cast<float>(top), static_cast<float>(x1 - x0),
                   static_cast<float>(height));
        if (geo.metered())
            continue;
        g.setColour(lane.darker(0.33f));
        for (double line = q; line < q + group && line < geo.left + width / geo.scale; line += grid)
            if (line >= geo.left)
                g.fillRect(static_cast<int>((line - geo.left) * geo.scale), top, 1, height);
    }
    // In the project's meters the lines run from each bar line (manual 6.10), each a quarter darker
    // than the lane under it.
    if (geo.metered())
        geo.forEachGridLine(geo.tick(0.), geo.tick(static_cast<double>(width)), [&](std::int64_t line) {
            const double q = static_cast<double>(line) / textproj::kPPQ;
            const bool alt = static_cast<std::int64_t>(std::floor(q / group)) % 2 != 0;
            const auto lane = selected ? (alt ? look::laneSelectedAlt : look::laneSelected)
                                       : (alt ? look::laneAlt : look::lane);
            g.setColour(lane.darker(0.33f));
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
    // track's colour, and a black outline around what is on screen.
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
        g.setColour(view.colourFor(r.position));
        for (const auto &c : r.track->clips)
            g.fillRect(static_cast<float>(static_cast<double>(c->posTicks) / total * w), y,
                       std::max(1.f, static_cast<float>(static_cast<double>(c->lengthTicks) / total * w)),
                       line);
        y += row;
    }
    g.setColour(juce::Colours::black);
    g.drawRect(outline(), 2.f);
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
    const float w = 18.f + juce::GlyphArrangement::getStringWidth(look::font(11.5f), text);
    return {x, kBraceHeight, w, 15.f}; // under the loop brace
}
juce::Rectangle<float> ScrubArea::locatorBox(const Locator &l) const {
    const float x = static_cast<float>(view.geometry.x(l.pos));
    const float w = 18.f + juce::GlyphArrangement::getStringWidth(look::font(11.5f), juce::String::fromUTF8(l.name.c_str()));
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
        editor.grabKeyboardFocus();
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
        g.setColour(juce::Colour(0xffb4b4b4));
        g.fillRect(band);
        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(band.getSmallestIntegerContainer());
            g.setColour(juce::Colour(0xff6e6e6e));
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
            g.setColour(t.loopEnabled ? juce::Colour(0xff3c3c3c) : juce::Colour(0xff9a9a9a));
            g.fillRect(box);
            juce::Path ends;
            const float h = box.getHeight(), mid = box.getCentreY();
            ends.addTriangle(box.getX() + 1.f, box.getY() + 1.f, box.getX() + 1.f, box.getBottom() - 1.f,
                             box.getX() + h - 1.f, mid);
            ends.addTriangle(box.getRight() - 1.f, box.getY() + 1.f, box.getRight() - 1.f, box.getBottom() - 1.f,
                             box.getRight() - h + 1.f, mid);
            g.setColour(juce::Colour(0xffe8e8e8));
            g.fillPath(ends);
            if (view.loopSelected) {
                g.setColour(look::selection);
                g.drawRect(box, 1.5f);
            }
        }
    }
    g.setFont(look::font(11.5f));
    for (std::size_t i = 1; i < meters.size(); ++i) {
        auto m = meters[i];
        if (m.start_ticks == dragged_ && movingTo_ >= 0)
            m.start_ticks = movingTo_; // drawn where the drag would put it
        const auto box = markerBox(m);
        if (box.getRight() < 0 || box.getX() > static_cast<float>(getWidth()))
            continue;
        const float x = box.getX();
        g.setColour(meters[i].start_ticks == view.selectedMeter ? look::selection : juce::Colour(0xffb4b4b4));
        g.fillRect(box);
        g.setColour(look::separator);
        g.drawRect(box, 1.5f);
        juce::Path notch;
        notch.addTriangle(x + 2.f, box.getBottom() - 3.f, x + 2.f, box.getY() + 2.f, x + 10.f, box.getY() + 2.f);
        g.setColour(juce::Colour(0xffe8e8e8));
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
            g.setColour(lit ? look::selection : juce::Colour(0xffb4b4b4));
            g.fillRect(box);
            g.setColour(look::separator);
            g.drawRect(box, 1.5f);
            juce::Path mark;
            mark.addTriangle(box.getX() + 2.f, box.getY() + 2.f, box.getX() + 2.f, box.getBottom() - 2.f,
                             box.getX() + 10.f, box.getCentreY());
            g.setColour(juce::Colour(0xffe8e8e8));
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
        view.canvas.grabKeyboardFocus(); // the arrows go to the lanes, which act on the brace
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
        view.canvas.grabKeyboardFocus(); // Delete and the arrows go to the lanes, which act on it
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
            view.canvas.grabKeyboardFocus();
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
        editor.grabKeyboardFocus();
    repaint();
    return true;
}
void ScrubArea::endMeterEdit() {
    editing_ = -1;
    locatorEditing_ = 0;
    editor.setVisible(false);
    if (view.canvas.isShowing())
        view.canvas.grabKeyboardFocus();
    repaint();
}
TimelineRuler::TimelineRuler(ArrangementView &v) : view(v) {
    setTitle("Beat-time ruler");
    setTooltip("Drag sideways to scroll, up or down to zoom; double-click to zoom to the selection.");
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
    setTooltip("Click to select a track; drag its lower part to resize it, with Alt to resize "
               "all. Right-click for the track's commands.");
}
ArrangementCanvas::ArrangementCanvas(ArrangementView &v) : view(v) {
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
            return juce::String::fromUTF8(displayName(t->name, i + 1).c_str()) + ", " + clip +
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
    for (auto *c : std::array<juce::Component *, 7>{&overview, &ruler, &scrub, &headers, &canvas,
                                                     &playhead, &projectView})
        addAndMakeVisible(c);
    // The tree the snapshot carries (#299).
    trackRows_ = rowsFromSnapshot;
    projectView.onClick = [this] {
        projectViewMenu().showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&projectView));
    };
}
namespace {
// Bitwig's project view menu, as the director's screenshot shows it (28 October 2026): a dark
// menu with a thin light edge, large rows, the view in use on a light grey row.
constexpr juce::uint32 kMenuBack = 0xff2b2b2b, kMenuEdge = 0xff5a5a5a, kRowCurrent = 0xffc8c8c8,
                       kTextCurrent = 0xff2b2b2b, kRowHover = 0xff484848, kText = 0xffe2e2e2;
class ProjectViewMenuLook final : public juce::LookAndFeel_V4 {
  public:
    void drawPopupMenuBackground(juce::Graphics &g, int width, int height) override {
        g.fillAll(juce::Colour(kMenuBack));
        g.setColour(juce::Colour(kMenuEdge));
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
                g.setColour(i == current_ ? juce::Colours::white : juce::Colours::white.withAlpha(.6f));
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
        width = 56 + depth_ * kIndent + juce::roundToInt(juce::GlyphArrangement::getStringWidth(look::font(15.f), name_));
        height = kRow;
    }
    void paint(juce::Graphics &g) override {
        if (current_ || isItemHighlighted()) {
            g.setColour(juce::Colour(current_ ? kRowCurrent : kRowHover));
            g.fillRect(getLocalBounds());
        }
        // A folder: its body and the tab on its top left.
        const float x = 10.f + static_cast<float>(depth_ * kIndent);
        juce::Path folder;
        folder.addRoundedRectangle(x, 8.f, 16.f, 12.f, 1.5f);
        folder.addRoundedRectangle(x, 6.f, 7.f, 4.f, 1.f);
        g.setColour(colour_);
        g.fillPath(folder);
        g.setColour(juce::Colour(current_ ? kTextCurrent : kText));
        g.setFont(look::font(15.f));
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
    setTooltip("Project view: see the whole project, or only a group and what is inside it.");
    // A view command, as Bitwig's: it takes no keyboard focus.
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}
void ProjectViewButton::paintButton(juce::Graphics &g, bool over, bool down) {
    // Bitwig's icon: a list with its lower lines indented, and the menu's arrow beside it.
    g.setColour(getToggleState() ? look::accent : over || down ? juce::Colours::white : look::textOnBand);
    const auto r = getLocalBounds().toFloat().reduced(2.f, 3.f);
    for (int line = 0; line < 3; ++line)
        g.fillRect(r.getX() + (line == 0 ? 0.f : 4.f), r.getY() + static_cast<float>(line) * 4.f,
                   line == 0 ? 12.f : 8.f, 2.f);
    const float ax = r.getX() + 15.f, ay = r.getY() + 3.f;
    juce::Path arrow;
    arrow.addTriangle(ax, ay, ax + 7.f, ay, ax + 3.5f, ay + 4.f);
    g.fillPath(arrow);
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
        headers.beginRename(headers.renaming());
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
    projectView.setToggleState(group != 0, juce::dontSendNotification);
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
void ArrangementView::toggleFold(std::int64_t group) {
    auto &folded = root.state().foldedGroups;
    if (folded.erase(group) == 0)
        folded.insert(group);
    leaveOut();
    // Live keeps the selection in sight: a track folded away gives it to its group.
    if (!geometry.shown(geometry.selection.track) && geometry.shown(group)) {
        geometry.selection.track = group;
        geometry.selection.clip = 0;
        geometry.selection.pickedClips.clear();
    }
    firstShownIfHidden();
    changed();
}
void ArrangementView::unfoldAll() {
    root.state().foldedGroups.clear();
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
    add(0, 0, "Project", juce::Colour(0xff8a8a8a));
    for (const auto row : geometry.rows(reader()))
        if (row.place.group) {
            // Its name as its title shows it (a # is its number).
            const auto *stored = tree_.find(row.track->id);
            const auto &name = stored != nullptr ? stored->name : row.track->name;
            add(row.track->id, static_cast<int>(row.place.depth) + 1,
                juce::String::fromUTF8(displayName(name, row.position + 1).c_str()), colourFor(row.position));
        }
    // The ★ Reference track's toggle (ADR-0209 §4, d11), hidden by default.
    menu.addSeparator();
    menu.addCommandItem(&root.commands().manager, AppCommands::ToggleReferenceTrack);
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
    // Bitwig's place for it: the top of the track names, level with the ruler.
    projectView.setBounds(headers.getX() + 4, above + 5, 30, 17);
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
        return {hx + 21, top + 15, 12, 12};
    case TopButton::Automation:
        return {hx + 45, top + 15, 22, 12};
    case TopButton::Lock:
        break;
    }
    return {hx + 72, top + 15, 22, 12};
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
    for (const auto &[button, words] :
         {std::pair{B::Automation, "Automation Mode waits for the engine's automation lanes (window log, N26)."},
          std::pair{B::Lock, "Lock Envelopes waits for the engine's automation lanes (window log, N26)."}})
        if (topButton(button).expanded(1.f).contains(e.position)) {
            root.say(words);
            return;
        }
}
void ArrangementView::paint(juce::Graphics &g) {
    // The arrangement's frame is Live's dark band; the rulers and the header column's
    // top sit on it.
    g.fillAll(look::band);
    // Above the headers: Live's locator buttons ("Set", previous, next) and its two
    // toggles, Automation Mode and Lock Envelopes.
    const auto set = topButton(TopButton::Set);
    g.setColour(look::button);
    g.fillRoundedRectangle(set, 6.f);
    g.setColour(look::outline);
    g.drawRoundedRectangle(set.reduced(0.5f), 6.f, 1.f);
    g.setColour(look::text);
    g.setFont(look::font(11.f));
    g.drawText(selectedLocator != 0 ? "Del" : "Set", set, juce::Justification::centred);
    for (int i = 0; i < 2; ++i) {
        const auto c = topButton(i == 0 ? TopButton::Previous : TopButton::Next);
        g.setColour(look::button);
        g.fillEllipse(c);
        g.setColour(look::outline);
        g.drawEllipse(c.reduced(0.5f), 1.f);
        // Live's arrows: back to the previous locator, on to the next.
        const float dir = i == 0 ? -1.f : 1.f, cy = c.getCentreY(), cx = c.getCentreX();
        g.setColour(i == 0 ? look::text : look::textDim);
        g.drawLine(cx - 3.f * dir, cy, cx + 3.f * dir, cy, 1.5f);
        g.drawLine(cx + 3.f * dir, cy, cx, cy - 3.f, 1.5f);
        g.drawLine(cx + 3.f * dir, cy, cx, cy + 3.f, 1.5f);
    }
    const auto automation = topButton(TopButton::Automation);
    const float ax = automation.getX(), ay = automation.getY();
    look::box(g, automation, look::button);
    g.setColour(look::text);
    g.drawLine(ax + 6, ay + 9, ax + 16, ay + 3, 1.2f);
    g.fillEllipse(ax + 4.5f, ay + 7.5f, 3, 3);
    g.fillEllipse(ax + 14.5f, ay + 1.5f, 3, 3);
    const auto lock = topButton(TopButton::Lock);
    const float lx = lock.getX(), ly = lock.getY();
    look::box(g, lock, juce::Colour(0xfff0773a));
    g.fillRect(lx + 7.f, ly + 5.5f, 8.f, 4.5f);
    g.drawRoundedRectangle(lx + 8.5f, ly + 2.5f, 5.f, 5.f, 2.f, 1.f);
    // Under the lanes: the time ruler, in minutes and seconds (manual 6.1).
    const int clipsW = canvas.getWidth();
    const juce::Rectangle<int> time(canvas.getX(), canvas.getBottom(), clipsW, kTimeRulerHeight);
    g.setColour(look::band);
    g.fillRect(time);
    // The grid's spacing sits at the ruler's right end, as Live shows it (§6.10).
    const juce::Rectangle<int> spacing(time.getRight() - 48, time.getY() + 2, 46, 14);
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
        g.setFont(look::font(11.5f));
        for (double t = std::ceil(from / step) * step; t <= to; t += step) {
            const int x = canvas.getX() + static_cast<int>(geometry.x(tempo->secondsToTicks(t)));
            g.setColour(look::textOnBand);
            g.fillRect(x, time.getY(), 1, 5);
            const int sec = static_cast<int>(t + 0.5);
            // A time that would run into the grid's spacing keeps its mark only.
            if (x + 3 + 32 < spacing.getX())
                g.drawText(juce::String(sec / 60) + ":" + juce::String(sec % 60).paddedLeft('0', 2),
                           x + 3, time.getY() + 2, 50, 14, juce::Justification::centredLeft);
        }
    }
    g.setColour(look::band);
    g.fillRect(spacing);
    g.setColour(look::textOnBand);
    g.setFont(look::font(11.5f));
    g.drawText(juce::String(gridLabel(geometry.gridQuarters(), geometry.grid.triplet)), spacing,
               juce::Justification::centredRight);
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
    if (modelChanged) {
        canvas.realiseAccessible();
        overview.repaint();
        canvas.repaint();
        headers.repaint();
        ruler.repaint();
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
    state.largeTracks = geometry.largeTracks;
    state.gridSnap = geometry.grid.snap;
    state.gridFixed = geometry.grid.fixed;
    state.gridTriplet = geometry.grid.triplet;
    state.gridNarrow = geometry.grid.narrow;
    state.gridFixedQuarters = geometry.grid.fixedQuarters;
    root.persist();
    root.publishSelection();
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
        geometry.selection.insert = 0;
        geometry.selection.marked = true;
        locate();
        break;
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
void ArrangementView::locate() { root.locate(geometry.selection.insert); }
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
juce::PopupMenu ArrangementView::headerMenu() {
    auto &commands = root.commands();
    commands.activate(root);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
    menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
    if (geometry.selection.track == 0)
        return menu;
    menu.addSeparator();
    menu.addCommandItem(&commands.manager, AppCommands::RenameTrack);
    menu.addCommandItem(&commands.manager, AppCommands::DeleteTrack);
    menu.addSeparator();
    menu.addCommandItem(&commands.manager, AppCommands::GroupTracks);
    menu.addCommandItem(&commands.manager, AppCommands::UngroupTracks);
    // Live's colour chooser at the menu's foot (Live 12 manual 2.3.2); on a group, the command that
    // gives what it holds its colour (manual 18.3).
    menu.addSeparator();
    if (const auto *t = reader().findTrack(geometry.selection.track); t != nullptr && t->kind == "group")
        menu.addItem("Assign Track Color to Grouped Tracks and Clips", [this] { assignGroupColour(); });
    menu.addCustomItem(1, std::make_unique<ColourChooser>(*this, colourIndex(geometry.selection.track)), nullptr,
                       "Colour");
    return menu;
}
void ArrangementView::contextMenu(juce::Point<int> where, bool header) {
    auto menu = header ? headerMenu() : juce::PopupMenu();
    if (!header) {
        auto &commands = root.commands();
        commands.activate(root);
        menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
        menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
        // The lanes' menu sets the grid as well (§6.10).
        menu.addSeparator();
        addGridItems(menu);
    }
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea(
        juce::Rectangle<int>(where.x, where.y, 1, 1)));
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
bool ArrangementView::assignGroupColour() {
    const auto current = root.current();
    const auto *group = reader().findTrack(geometry.selection.track);
    if (!current || group == nullptr || group->kind != "group") {
        root.say("Select a group track to give what it holds its colour.");
        return false;
    }
    // Its colour from the CURRENT cell (ADR-0201), so a colour picked a moment ago counts; the one its
    // place gives it otherwise.
    const auto *now = track_edits_detail::find(current->tracks, group->id);
    const auto &own = now != nullptr ? now->color : group->color;
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
    // Live's beat-time ruler: bar numbers along its foot in light grey on the dark band, each after
    // its hook; a dot at every bar. In the project's meters (#299): a signature change starts a new
    // bar where it falls, and a bar's beats are its signature's (Live 12 manual 6.1).
    g.fillAll(look::band);
    const auto &geo = view.geometry;
    const auto &meters = view.reader().meters();
    const auto from = geo.tick(0.), to = geo.tick(static_cast<double>(getWidth()));
    // The spacing from the signature at the left edge: every beat, or every few beats, while they
    // are 56 points apart; else every bar, every second, every fourth and so on.
    const auto &first = meterAt(from, meters);
    const double beatPx = 4. / std::max<double>(1, first.denominator) * geo.scale;
    std::int64_t beatStep = 1;
    while (beatStep < first.numerator && static_cast<double>(beatStep) * beatPx < 56)
        beatStep *= 2;
    const bool beats = beatStep < first.numerator;
    std::int64_t barStep = 1;
    while (static_cast<double>(barStep) * beatPx * first.numerator < 56)
        barStep *= 2;
    const int foot = getHeight();
    g.setFont(look::font(12.f, true));
    auto label = [&](std::int64_t tick, const juce::String &text) {
        const int x = static_cast<int>(geo.x(tick));
        g.setColour(juce::Colour(0xff7d7d7d));
        g.fillRect(x, foot - 11, 1, 8);
        g.fillRect(x, foot - 11, 3, 1);
        g.setColour(look::textOnBand);
        g.drawText(text, x + 5, foot - 15, 60, 14, juce::Justification::centredLeft);
    };
    const bool dots = beatPx * first.numerator >= 8;
    // From a bar before the left edge, so a bar's later beats still show.
    forEachBar(from - first.barTicks(), to, meters, [&](std::int64_t tick, std::int64_t bar) {
        if (dots) {
            g.setColour(juce::Colour(0xff7d7d7d));
            g.fillRect(static_cast<int>(geo.x(tick)), foot - 2, 1, 1);
        }
        if (!beats) {
            if ((bar - 1) % barStep == 0)
                label(tick, juce::String(bar));
            return;
        }
        label(tick, juce::String(bar));
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
        for (std::int64_t b = beatStep; b < m.numerator && tick + b * beatTicks < until; b += beatStep)
            label(tick + b * beatTicks, juce::String(bar) + "." + juce::String(b + 1));
    });
}
void TimelineRuler::mouseDown(const juce::MouseEvent &e) {
    last = e.position;
    view.followPaused = true; // a click on the beat-time ruler pauses Follow (§6.2)
}
void TimelineRuler::mouseDrag(const juce::MouseEvent &e) {
    auto d = e.position - last;
    view.geometry.pan(-d.x);
    view.geometry.zoom(std::exp(-d.y * .02), e.position.x);
    last = e.position;
    view.changed();
}
void TimelineRuler::mouseDoubleClick(const juce::MouseEvent &) {
    view.fit(view.geometry.selection.start != view.geometry.selection.end);
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
        for (int band = 0; 2 + band * ArrangementView::kGroupBand < x; ++band) {
            g.setColour(groups[static_cast<std::size_t>(band)]);
            g.fillRect(2 + band * ArrangementView::kGroupBand, y, ArrangementView::kGroupBand - 1, h - 2);
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
        } else
            look::triangle(g, fold.reduced(3.5f), juce::MathConstants<float>::halfPi, ink);
        if (t->id != renaming_) {
            g.setColour(look::text);
            g.setFont(look::font(11.5f, true));
            g.drawText(juce::String::fromUTF8(displayName(t->name, row.position + 1).c_str()), x + 23, y + 2,
                       getWidth() - x - 25, 15, juce::Justification::centredLeft);
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
            g.setColour(look::text);
            g.drawRect(box, 4);
            g.setColour(juce::Colours::white);
            g.drawRect(box.reduced(1), 2);
        } else {
            g.setColour(look::text);
            g.fillRect(x, drop_.y - 2, getWidth() - x, 4);
            g.setColour(juce::Colours::white);
            g.fillRect(x, drop_.y - 1, getWidth() - x, 2);
        }
    }
}
void TrackHeaderList::resized() {
    if (renaming_ != 0)
        beginRename(renaming_);
}
bool TrackHeaderList::beginRename(std::int64_t track) {
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
    view.geometry.selection.track = track;
    view.root.publishSelection();
    const int x = nameLeft(depth) + 2;
    editor.setBounds(x, y + 3, std::max(0, getWidth() - x - 4), 24);
    if (!editor.isVisible() || editor.getName() != juce::String(track)) {
        editor.setName(juce::String(track));
        editor.setText(juce::String::fromUTF8(t->name.c_str()), false);
        editor.selectAll();
    }
    editor.setVisible(true);
    if (editor.isShowing())
        editor.grabKeyboardFocus();
    repaint();
    return true;
}
bool TrackHeaderList::commitRename() {
    const auto id = renaming_;
    if (id == 0)
        return false;
    const auto text = editor.getText().toStdString();
    const auto now = view.root.current();
    const auto *t = now->findTrack(id);
    if (t == nullptr || t->name == text)
        return true;
    return view.root.submitEdit("track.rename", {{"id", id}, {"name", text}}, "Rename Track");
}
void TrackHeaderList::endRename() {
    renaming_ = 0;
    editor.setName({});
    editor.setVisible(false);
    if (view.canvas.isShowing())
        view.canvas.grabKeyboardFocus();
    repaint();
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
    // A group's fold switch folds or unfolds it (Live 12 manual 18.3), and selects nothing. Any other
    // track's Unfold Track button shows its automation lanes (manual 6.9), which wait for the engine:
    // a click says so.
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    foldPressed_ = false;
    if (const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
        !e.mods.isPopupMenu() && at < rows.size() &&
        foldSwitch(rows[at].place.depth, top).expanded(2.f).contains(e.position)) {
        resizeId = moveId_ = 0;
        foldPressed_ = true;
        if (rows[at].place.group)
            view.toggleFold(rows[at].track->id);
        else
            view.root.say(ArrangementView::kUnfoldWaits);
        return;
    }
    resizeId = 0;
    moveId_ = 0;
    moving_ = selectOnUp_ = false;
    if (auto *t = view.geometry.trackAt(view.reader(), e.y)) {
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
void ArrangementCanvas::paint(juce::Graphics &g) {
    // Live's lanes: grey, every other group of bars a shade lighter, the selected
    // track's lanes lighter still, the bar grid, a dark rule between tracks.
    ++view.canvasPaints;
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    const auto foot = geo.footStart(rows);
    int y = -geo.scrollY;
    auto lanes = [&](int top, int height, bool selected) {
        paintLanes(g, geo, getWidth(), top, height, selected);
    };
    // Under the last track, Live's drop area, down to the foot or the bottom.
    auto dropArea = [&](int top, int bottom) {
        if (top >= bottom)
            return;
        lanes(top, bottom - top, false);
        g.setColour(look::text);
        g.setFont(look::font(12.f));
        g.drawText("Drop Files and Devices Here", 0, top, getWidth(), bottom - top, juce::Justification::centred);
    };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto row = rows[i];
        // The returns and Main at the foot, over the tracks scrolled under it.
        if (i == foot) {
            const int footY = geo.footTop(view.reader());
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
        auto drawClip = [&](const engine::ClipNode &clip) {
            const auto *c = &clip;
            // A clip being moved by its bar is drawn where it would land.
            const bool along = moving_ != 0 && !geo.selection.pickedClips.empty() && geo.clipSelected(c->id);
            const auto pos = c->id == moving_ ? movingTo_ : along ? c->posTicks + movingDelta() : c->posTicks;
            const auto length = c->id == resizing_ ? resizeLength_ : c->lengthTicks;
            const double x = geo.x(pos), end = geo.x(pos + length);
            if (end < 0 || x > getWidth())
                return;
            const float left = static_cast<float>(std::max(-1., x)),
                        right = static_cast<float>(std::min(end, static_cast<double>(getWidth() + 1)));
            const bool picked = geo.clipSelected(c->id);
            // A clip in its track's colour: a solid title strip, a lighter body. A
            // deactivated clip is grey, as Live's.
            const auto clipColour = c->muted ? juce::Colour(0xff9a9a9a) : colour;
            g.setColour(picked ? clipColour.brighter(0.25f) : clipColour.withMultipliedAlpha(0.75f));
            g.fillRect(left, static_cast<float>(y + 1), std::max(1.f, right - left), static_cast<float>(h - 4));
            g.setColour(clipColour);
            g.fillRect(left, static_cast<float>(y + 1), std::max(1.f, right - left), static_cast<float>(kClipBarHeight));
            paintWaveform(g, *c, pos, left, right, static_cast<float>(y + 1 + kClipBarHeight),
                          static_cast<float>(y + h - 3));
            // Its name in its title strip, cut at the clip's end, as Live's (the manual's demo
            // arrangement).
            if (!c->name.empty() && right - left > 6.f) {
                g.setColour(look::text);
                g.setFont(look::font(10.f));
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
        };
        // A clip dragged onto another track is drawn on that track's lane, in its colour; so is
        // each of several selected clips dragged across tracks.
        for (const auto &c : t->clips)
            if (const auto to = movingTargetOf(c->id); to == 0 || to == t->id)
                drawClip(*c);
        if (moving_ != 0)
            for (const auto &other : view.reader().tracks())
                if (other->id != t->id)
                    for (const auto &c : other->clips)
                        if (movingTargetOf(c->id) == t->id)
                            drawClip(*c);
        y += h;
    }
    if (foot == rows.size())
        dropArea(y, getHeight());
    // The insert marker, a thin blue line in the selected track.
    if (geo.selection.start == geo.selection.end && geo.selection.marked) {
        const int footY = geo.footTop(view.reader());
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
                g.setColour(juce::Colour(0xff5dc9e8));
                g.fillRect(static_cast<int>(geo.x(geo.selection.insert)), top, 1, std::max(0, bottom - top));
            }
            top += geo.heightFor(id);
        }
    }
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
    const float band = (bottom - top) / static_cast<float>(peaks->channels);
    g.setColour(juce::Colours::black.withAlpha(clip.muted ? 0.35f : 0.6f));
    const int from = std::max(0, static_cast<int>(std::floor(left))),
              to = std::min(getWidth(), static_cast<int>(std::ceil(right)));
    for (int x = from; x < to; ++x) {
        const auto f0 = source.frameAt(geo.tick(x)), f1 = source.frameAt(geo.tick(x + 1));
        if (!f0)
            continue;
        const double end = f1 ? *f1 : *f0 + 1.;
        for (std::uint32_t ch = 0; ch < peaks->channels; ++ch) {
            const auto span = peakSpan(*peaks, std::min(*f0, end), std::max(*f0 + 1., end), ch);
            if (!span)
                continue;
            const float mid = top + band * (static_cast<float>(ch) + .5f), half = band * .5f;
            const float y0 = mid - std::clamp(span->maximum, -1.f, 1.f) * half,
                        y1 = mid - std::clamp(span->minimum, -1.f, 1.f) * half;
            g.fillRect(static_cast<float>(x), y0, 1.f, std::max(1.f, y1 - y0));
        }
    }
}
void ArrangementCanvas::paintGroupLane(juce::Graphics &g, std::size_t at, int y, int h, juce::Colour colour) const {
    // Live's group track (manual 18.3, its figure of a folded group): a darker lane, and in it an
    // overview of the clips of every track the group holds, at any depth, a thin row each, in
    // the group's colour. Folded or not.
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    g.setColour(juce::Colours::black.withAlpha(0.16f));
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
    grabKeyboardFocus();
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
    keyResize_ = false; // a click ends a resize begun from the keyboard, as Esc does
    panning = e.mods.isCommandDown() && e.mods.isAltDown();
    if (e.mods.isPopupMenu()) {
        view.select(e.x, e.y, view.snaps(e.mods));
        view.contextMenu(e.getScreenPosition(), false);
    } else if (!panning) {
        // Held still at the lanes' edge, a drag repeats, so they keep scrolling.
        beginDragAutoRepeat(ArrangementView::kAutoRepeatMs);
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
        // (Live's "Select Multiple Items", §42.5); it moves nothing. A plain click lets them all go.
        if (e.mods.isCommandDown() && view.geometry.selection.clip != 0)
            if (const auto *t = view.geometry.trackAt(view.reader(), e.y))
                if (const auto *c = view.geometry.hit(*t, view.geometry.tick(e.x));
                    c != nullptr && c->id != view.geometry.selection.clip) {
                    auto &picked = view.geometry.selection.pickedClips;
                    if (const auto at = std::find(picked.begin(), picked.end(), c->id); at != picked.end())
                        picked.erase(at);
                    else
                        picked.push_back(c->id);
                    view.changed();
                    return;
                }
        // A plain press on the bar of one of several selected clips keeps them all for the drag,
        // which moves them together; released without a drag, it selects that clip alone.
        selectOnUp_ = false;
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
    std::int64_t track = 0;
    bool edge = false;
    const auto *c = barAt(e.x, e.y, track, edge);
    setMouseCursor(c != nullptr && edge ? juce::MouseCursor::LeftRightResizeCursor
                                        : juce::MouseCursor::NormalCursor);
}
void ArrangementCanvas::mouseDrag(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu())
        return;
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
        if (e.mouseWasDraggedSinceMouseDown())
            selectOnUp_ = false;
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
void ArrangementCanvas::mouseUp(const juce::MouseEvent &) {
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
                auto edit = moveClips(current->tracks, view.geometry.selectedClips(), delta, across);
                if (!edit.refusal.empty())
                    view.root.say(edit.refusal);
                else if (!edit.ops.empty() && view.root.submitEdits(std::move(edit.ops))) {
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
        } else if (selectOnUp_)
            view.select(pressedAt_.x, pressedAt_.y, view.snaps(juce::ModifierKeys::currentModifiers));
        selectOnUp_ = false;
        repaint();
    }
    if (moving_ != 0) {
        const auto clip = moving_, to = movingTo_, from = movingFrom_, track = movingTrack_;
        const bool across = movingTrack_ != movingSource_;
        moving_ = 0;
        // One drag, one undo step (§6.7), in time and to another track at once; the selection
        // follows the clip.
        if (to != from || across) {
            auto &sel = view.geometry.selection;
            const auto length = sel.end - sel.start;
            if (clipEdit("clip.move", {{"id", clip}, {"to", to}, {"track", track}}, "Move Clip")) {
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
    auto &sel = view.geometry.selection;
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
    // U folds or unfolds the selected group; Alt+U unfolds them all (§42.16, §42.19). The other
    // tracks' fold, which shows their automation, waits for the lanes, and says so.
    if (k == juce::KeyPress('u')) {
        if (view.tree().isGroup(sel.track))
            view.toggleFold(sel.track);
        else if (sel.track != 0)
            view.root.say(ArrangementView::kUnfoldWaits);
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
            view.changed();
            announce(view.root.selectionText());
            return true;
        }
        sel.track = tracks[to].track->id;
        sel.toTrack = 0;
        sel.picked.clear();
        sel.clip = 0;
        sel.pickedClips.clear();
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
    if (sel.clip == 0 && sel.start != sel.end &&
        (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey || k == juce::KeyPress('0'))) {
        rangeEdit(k == juce::KeyPress('0'));
        return true;
    }
    // Live's Delete, and 0 to deactivate (§6.9), on the selected clips: the one clicked and those
    // picked besides it with Ctrl/Cmd+click, one undo step for them all, from the CURRENT cell.
    if (view.geometry.selection.clip != 0) {
        const bool erase = k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey;
        if (erase || k == juce::KeyPress('0')) {
            const auto current = view.root.current();
            if (!current)
                return true;
            const auto ids = view.geometry.selectedClips();
            auto edit = erase ? deleteClips(current->tracks, ids) : deactivateClips(current->tracks, ids);
            // A refused edit is still the key's: the Status Bar says why.
            if (!edit.refusal.empty()) {
                view.root.say(edit.refusal);
                return true;
            }
            if (!view.root.submitEdits(std::move(edit.ops)) || !erase)
                return true;
            sel.clip = 0;
            sel.pickedClips.clear();
            sel.end = sel.start;
            sel.insert = sel.start;
            view.changed();
            return true;
        }
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
                    if (target != from &&
                        clipEdit("clip.move", {{"id", id}, {"to", target}, {"track", sel.track}}, "Move Clip")) {
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
        // A time selection moves by the grid when it holds no clip; moving part of a clip
        // needs it split first.
        const auto lo = std::min(sel.start, sel.end), hi = std::max(sel.start, sel.end);
        if (track != nullptr)
            for (const auto &c : track->clips)
                if (c->posTicks < hi && c->posTicks + c->lengthTicks > lo) {
                    view.root.say("Moving part of a clip waits for the engine's split (window log, N17).");
                    return true;
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
    return !BrowserPanel::droppedFiles(details.description).isEmpty();
}
void ArrangementCanvas::itemDropped(const SourceDetails &details) {
    filesDropped(BrowserPanel::droppedFiles(details.description), details.localPosition.x, details.localPosition.y);
}
void ArrangementCanvas::filesDropped(const juce::StringArray &files, int x, int y) {
    // Live's drop (manual 6.1): the clip starts where the file was dropped, on the nearest grid
    // line while snapping; on a track's lane, that track; below the tracks, a new one. The clip is
    // named after the file, as Live names it.
    if (files.isEmpty())
        return;
    const auto raw = std::max<std::int64_t>(0, view.geometry.tick(x));
    const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
    const auto at = !view.snaps(juce::ModifierKeys::currentModifiers) ? raw : (raw - down <= up - raw ? down : up);
    const auto *track = view.geometry.trackAt(view.reader(), y);
    // Several files go one after another on that track, or each on a track of its own with Ctrl/Cmd
    // held, as Live places them; one undo step for them all.
    std::vector<std::pair<std::string, std::string>> dropped;
    for (const auto &path : files)
        dropped.emplace_back(path.toStdString(),
                             juce::File::createFileWithoutCheckingPath(path).getFileNameWithoutExtension().toStdString());
    view.root.dropAudioFiles(dropped, track != nullptr ? track->id : 0, at,
                             juce::ModifierKeys::currentModifiers.isCommandDown());
}
} // namespace adi::ui
