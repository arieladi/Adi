// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_arrangement.hpp"
#include "ui_shell.hpp"
namespace adi::ui {
ArrangementOverview::ArrangementOverview(ArrangementView &v) : view(v) {
    setTitle("Overview");
    setTooltip("The whole arrangement. Drag sideways to scroll, up or down to zoom; "
               "double-click to see all of it.");
}
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
    g.fillAll(juce::Colour(0xff1b2129));
    const auto &tracks = view.reader().tracks();
    const double total = static_cast<double>(span()), w = getWidth();
    const float row = std::max(1.f, static_cast<float>(getHeight() - 2) /
                                        static_cast<float>(std::max<std::size_t>(1, tracks.size())));
    g.setColour(juce::Colour(0xff488a9a));
    float y = 1;
    for (const auto &t : tracks) {
        for (const auto &c : t->clips)
            g.fillRect(static_cast<float>(static_cast<double>(c->posTicks) / total * w), y,
                       std::max(1.f, static_cast<float>(static_cast<double>(c->lengthTicks) /
                                                        total * w)),
                       std::max(1.f, row - 1));
        y += row;
    }
    g.setColour(juce::Colour(0xffe0e5ed));
    g.drawRect(outline(), 1.f);
}
void ArrangementOverview::mouseDown(const juce::MouseEvent &e) { last = e.position; }
void ArrangementOverview::mouseDrag(const juce::MouseEvent &e) {
    const auto d = e.position - last;
    last = e.position;
    // Sideways moves the outline: the overview's pixels are the whole span's.
    const double quarters = d.x * static_cast<double>(span()) / textproj::kPPQ /
                            std::max(1, getWidth());
    view.geometry.pan(quarters * view.geometry.scale);
    // Vertically zooms around what is in the middle of the screen.
    view.geometry.zoom(std::exp(-d.y * .02), view.canvas.getWidth() / 2.);
    view.changed();
}
void ArrangementOverview::mouseDoubleClick(const juce::MouseEvent &) { view.fit(false); }
TimelineRuler::TimelineRuler(ArrangementView &v) : view(v) {
    setTitle("Beat-time ruler");
    // The snapshot carries no time signatures yet (collab/window.md, N6).
    setTooltip("Drag sideways to scroll, up or down to zoom; double-click to zoom to the "
               "selection. Bars are counted in 4/4 until the project's meters reach the view.");
}
TrackHeaderList::TrackHeaderList(ArrangementView &v) : view(v) {
    setTitle("Track headers");
    setTooltip("Click to select a track; drag its lower part to resize it, with Alt to resize "
               "all. Right-click for the track's commands.");
}
ArrangementCanvas::ArrangementCanvas(ArrangementView &v) : view(v) {
    setWantsKeyboardFocus(true);
    setTitle("Arrangement");
    setDescription("Clips on a timeline. Arrow keys move between tracks and clips.");
    setTooltip("Click to place the insert marker or select a clip; drag to select time. "
               "Ctrl/Cmd+wheel zooms around the selection, Alt+wheel sizes tracks.");
}
std::unique_ptr<juce::AccessibilityHandler> ArrangementCanvas::createAccessibilityHandler() {
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::list);
}
ArrangementView::ArrangementView(AdiRootComponent &r)
    : root(r), overview(*this), ruler(*this), headers(*this), canvas(*this) {
    for (auto *c :
         std::array<juce::Component *, 5>{&overview, &ruler, &headers, &canvas, &playhead})
        addAndMakeVisible(c);
}
const SnapshotReader &ArrangementView::reader() const { return root.reader(); }
void ArrangementView::resized() {
    const int headerW = std::min(kHeaderWidth, getWidth() / 3);
    const int clipsW = std::max(0, getWidth() - headerW);
    const int body = std::max(0, getHeight() - kTopHeight);
    overview.setBounds(0, 0, clipsW, kOverviewHeight);
    ruler.setBounds(0, kOverviewHeight, clipsW, kRulerHeight);
    canvas.setBounds(0, kTopHeight, clipsW, body);
    headers.setBounds(clipsW, kTopHeight, headerW, body);
    frame(true);
}
void ArrangementView::frame(bool modelChanged) noexcept {
    const int x = static_cast<int>(std::clamp(geometry.x(root.timelineTick()), -1e7, 1e7));
    playhead.setVisible(x >= 0 && x < canvas.getWidth());
    playhead.setBounds(canvas.getX() + x, kTopHeight, 1, canvas.getHeight());
    playhead.toFront(false);
    if (modelChanged) {
        overview.repaint();
        canvas.repaint();
        headers.repaint();
        ruler.repaint();
    }
}
void ArrangementView::changed() {
    auto &state = root.state();
    state.timelineLeft = geometry.left;
    state.pixelsPerQuarter = geometry.scale;
    state.laneHeight = geometry.height;
    state.laneHeights = geometry.heights;
    root.persist();
    // The mixer's rows follow these lanes: re-place them on the next frame.
    root.mark(DirtySet::Layout);
    frame(true);
}
void ArrangementView::fit(bool selection) {
    geometry.fit(selection ? std::min(geometry.selectionStart, geometry.selectionEnd) : 0,
                 selection ? std::max(geometry.selectionStart, geometry.selectionEnd)
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
        fit(geometry.selectionStart != geometry.selectionEnd);
        return true;
    case AppCommands::ZoomBack:
        geometry.back();
        break;
    case AppCommands::OptimizeWidth:
        fit(false);
        return true;
    case AppCommands::OptimizeHeight: {
        const int count = static_cast<int>(reader().tracks().size());
        geometry.height = std::clamp(canvas.getHeight() / std::max(1, count), 24, 512);
        geometry.scrollY = 0;
        geometry.heights.clear();
        break;
    }
    case AppCommands::GoStart:
        geometry.insert = 0;
        geometry.marked = true;
        locate();
        break;
    default:
        return false;
    }
    changed();
    return true;
}
void ArrangementView::locate() { root.locate(geometry.insert); }
void ArrangementView::select(int x, int y) {
    if (const auto *t = geometry.trackAt(reader(), y)) {
        geometry.selectedTrack = t->id;
        const auto tick = geometry.tick(x);
        const auto *c = geometry.hit(*t, tick);
        geometry.selectedClip = c ? c->id : 0;
        geometry.insert = tick;
        geometry.marked = true;
        geometry.selectionStart = c ? c->posTicks : tick;
        geometry.selectionEnd = c ? c->posTicks + c->lengthTicks : tick;
        locate();
        changed();
    }
}
void ArrangementView::contextMenu(juce::Point<int> where, bool header) {
    auto &commands = root.commands();
    commands.activate(root);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
    menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
    if (header && geometry.selectedTrack != 0) {
        menu.addSeparator();
        menu.addCommandItem(&commands.manager, AppCommands::DeleteTrack);
    }
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea(
        juce::Rectangle<int>(where.x, where.y, 1, 1)));
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
    } else if (e.mods.isShiftDown())
        geometry.pan(-w.deltaY * 300);
    else {
        geometry.pan(-w.deltaX * 300);
        geometry.scrollY = std::clamp(geometry.scrollY - static_cast<int>(w.deltaY * 300), 0,
                                      std::max(0, geometry.contentHeight(reader()) - 24));
    }
    changed();
}
void TimelineRuler::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff303944));
    g.setColour(juce::Colour(0xffd1d8e0));
    g.setFont(juce::FontOptions(12.f));
    // One label per bar at most, then per beat as the zoom allows.
    const double step = std::max(1., std::pow(2., std::ceil(std::log2(32. / view.geometry.scale))));
    for (double q = std::floor(view.geometry.left / step) * step;
         q < view.geometry.left + getWidth() / view.geometry.scale; q += step) {
        const int x = static_cast<int>((q - view.geometry.left) * view.geometry.scale);
        g.drawVerticalLine(x, 20, 28);
        g.drawText(juce::String(textproj::renderPosition(
                       static_cast<std::int64_t>(q * textproj::kPPQ), {})),
                   x + 3, 0, 52, 20, juce::Justification::centredLeft);
    }
}
void TimelineRuler::mouseDown(const juce::MouseEvent &e) { last = e.position; }
void TimelineRuler::mouseDrag(const juce::MouseEvent &e) {
    auto d = e.position - last;
    view.geometry.pan(-d.x);
    view.geometry.zoom(std::exp(-d.y * .02), e.position.x);
    last = e.position;
    view.changed();
}
void TimelineRuler::mouseDoubleClick(const juce::MouseEvent &) {
    view.fit(view.geometry.selectionStart != view.geometry.selectionEnd);
}
void TrackHeaderList::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff242b35));
    int y = -view.geometry.scrollY;
    for (const auto &t : view.reader().tracks()) {
        const int h = view.geometry.heightFor(t->id);
        g.setColour(juce::Colour(t->id == view.geometry.selectedTrack ? 0xff416976 : 0xff343e4b));
        g.fillRect(1, y, getWidth() - 1, h - 1);
        g.setColour(juce::Colour(0xffe0e5ed));
        g.drawText(juce::String::fromUTF8(t->name.c_str()), 8, y + 4, getWidth() - 12, 22,
                   juce::Justification::centredLeft);
        y += h;
    }
}
void TrackHeaderList::mouseDown(const juce::MouseEvent &e) {
    if (auto *t = view.geometry.trackAt(view.reader(), e.y)) {
        view.geometry.selectedTrack = t->id;
        resizeId = t->id;
        initialHeight = view.geometry.heightFor(t->id);
        view.changed();
    }
    if (e.mods.isPopupMenu()) {
        resizeId = 0;
        view.contextMenu(e.getScreenPosition(), true);
    }
}
void TrackHeaderList::mouseDrag(const juce::MouseEvent &e) {
    if (resizeId) {
        view.geometry.resizeTrack(resizeId, initialHeight + e.getDistanceFromDragStartY(),
                                  e.mods.isAltDown());
        view.frame(true);
    }
}
void TrackHeaderList::mouseUp(const juce::MouseEvent &) {
    resizeId = 0;
    view.changed();
}
void TrackHeaderList::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    view.wheel(e, w);
}
void ArrangementCanvas::paint(juce::Graphics &g) {
    ++view.canvasPaints;
    g.fillAll(juce::Colour(0xff171d25));
    const double grid = std::max(1., std::pow(2., std::ceil(std::log2(20. / view.geometry.scale))));
    g.setColour(juce::Colour(0xff222c36));
    for (double q = std::floor(view.geometry.left / grid) * grid;
         q < view.geometry.left + getWidth() / view.geometry.scale; q += grid)
        g.drawVerticalLine(static_cast<int>((q - view.geometry.left) * view.geometry.scale), 0,
                           static_cast<float>(getHeight()));
    int y = -view.geometry.scrollY;
    for (const auto &t : view.reader().tracks()) {
        const int h = view.geometry.heightFor(t->id);
        g.setColour(juce::Colour(0xff313b47));
        g.drawHorizontalLine(y + h - 1, 0, static_cast<float>(getWidth()));
        for (const auto &c : t->clips) {
            const double x = view.geometry.x(c->posTicks),
                         end = view.geometry.x(c->posTicks + c->lengthTicks);
            if (end < 0 || x > getWidth())
                continue;
            const float left = static_cast<float>(std::max(-1., x)),
                        right =
                            static_cast<float>(std::min(end, static_cast<double>(getWidth() + 1)));
            g.setColour(
                juce::Colour(c->id == view.geometry.selectedClip ? 0xff89c7cf : 0xff488a9a));
            g.fillRect(left, static_cast<float>(y + 3), std::max(1.f, right - left),
                       static_cast<float>(h - 7));
        }
        y += h;
    }
    if (view.geometry.selectionStart != view.geometry.selectionEnd) {
        const float x = static_cast<float>(
            view.geometry.x(std::min(view.geometry.selectionStart, view.geometry.selectionEnd)));
        const float end = static_cast<float>(
            view.geometry.x(std::max(view.geometry.selectionStart, view.geometry.selectionEnd)));
        g.setColour(juce::Colour(0x2265c4d0));
        g.fillRect(x, 0.f, end - x, static_cast<float>(getHeight()));
    } else if (view.geometry.marked) {
        g.setColour(juce::Colour(0xffe0e5ed));
        g.drawVerticalLine(static_cast<int>(view.geometry.x(view.geometry.insert)), 0,
                           static_cast<float>(getHeight()));
    }
}
void ArrangementCanvas::mouseDown(const juce::MouseEvent &e) {
    grabKeyboardFocus();
    last = e.position;
    panning = e.mods.isCommandDown() && e.mods.isAltDown();
    if (e.mods.isPopupMenu()) {
        view.select(e.x, e.y);
        view.contextMenu(e.getScreenPosition(), false);
    } else if (!panning)
        view.select(e.x, e.y);
}
void ArrangementCanvas::mouseDrag(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu())
        return;
    if (panning) {
        view.geometry.pan(last.x - e.position.x);
        view.geometry.scrollY =
            std::max(0, view.geometry.scrollY + static_cast<int>(last.y - e.position.y));
        last = e.position;
    } else
        view.geometry.selectionEnd = view.geometry.tick(e.x);
    view.frame(true);
}
void ArrangementCanvas::mouseUp(const juce::MouseEvent &) { view.changed(); }
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
    const auto &tracks = view.reader().tracks();
    if (k.getKeyCode() == juce::KeyPress::upKey || k.getKeyCode() == juce::KeyPress::downKey) {
        std::size_t at = tracks.size();
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->id == view.geometry.selectedTrack) {
                at = i;
                break;
            }
        if (tracks.empty())
            return true;
        if (k.getKeyCode() == juce::KeyPress::downKey)
            at = at == tracks.size() ? 0 : std::min(at + 1, tracks.size() - 1);
        else
            at = at == tracks.size() || at == 0 ? 0 : at - 1;
        view.geometry.selectedTrack = tracks[at]->id;
        view.geometry.selectedClip = 0;
        view.changed();
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey) {
        const auto *t = view.reader().findTrack(view.geometry.selectedTrack);
        if (!t || t->clips.empty())
            return false;
        std::size_t i = 0;
        for (; i < t->clips.size(); ++i)
            if (t->clips[i]->id == view.geometry.selectedClip)
                break;
        if (k.getKeyCode() == juce::KeyPress::rightKey)
            i = i == t->clips.size() ? 0 : std::min(i + 1, t->clips.size() - 1);
        else
            i = i > 0 && i < t->clips.size() ? i - 1 : 0;
        const auto &c = t->clips[i];
        view.geometry.selectedClip = c->id;
        view.geometry.selectionStart = c->posTicks;
        view.geometry.selectionEnd = c->posTicks + c->lengthTicks;
        view.changed();
        return true;
    }
    return view.root.keyPressed(k);
}
bool ArrangementCanvas::isInterestedInFileDrag(const juce::StringArray &files) {
    return files.size() == 1;
}
void ArrangementCanvas::filesDropped(const juce::StringArray &, int, int) {
    // There is no undoable path from a file to a clip that plays yet, and step 7
    // writes no rows of its own (STEP-7-PLAN §3.0; collab/window.md, N5).
    view.root.say("Dropping audio onto a track waits for the engine's audio-clip op.");
}
} // namespace adi::ui
