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
        // Commit, then move to the neighbouring title bar to rename a series.
        const auto from = renaming_;
        commitRename();
        const auto now = view.root.current();
        const auto &tracks = now->tracks;
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->id == from) {
                const bool has = forward ? i + 1 < tracks.size() : i > 0;
                if (has) {
                    beginRename(tracks[forward ? i + 1 : i - 1]->id);
                    return;
                }
                break;
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
    setDescription("Clips on a timeline. Arrow keys move between tracks and clips.");
    setTooltip("Click to place the insert marker or select a clip; drag to select time. "
               "Ctrl/Cmd+wheel zooms around the selection, Alt+wheel sizes tracks.");
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
    const auto &tracks = view.reader().tracks();
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->id != trackId)
            continue;
        for (const auto &c : tracks[i]->clips) {
            if (c->id != clipId)
                continue;
            // Bars and beats in 4/4 until the snapshot carries the meters (window log, N6).
            const auto beat = c->posTicks / textproj::kPPQ;
            const auto beats = static_cast<double>(c->lengthTicks) / textproj::kPPQ;
            return juce::String::fromUTF8(displayName(tracks[i]->name, i + 1).c_str()) +
                   ", clip at bar " + juce::String(beat / 4 + 1) + " beat " +
                   juce::String(beat % 4 + 1) + ", " + juce::String(beats, 2) + " beats" +
                   (c->muted ? ", muted" : "");
        }
    }
    return {};
}
void ArrangementCanvas::realiseAccessible() {
    std::size_t used = 0;
    int y = -view.geometry.scrollY;
    for (const auto &t : view.reader().tracks()) {
        const int h = view.geometry.heightFor(t->id);
        if (y + h > 0 && y < getHeight())
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
void ArrangementCanvas::announceSelection() {
    // The keyboard moves the selection; a screen reader says where it landed.
    lastAnnouncement = view.geometry.selection.clip != 0
                           ? describe(view.geometry.selection.track, view.geometry.selection.clip)
                           : juce::String();
    if (lastAnnouncement.isNotEmpty())
        juce::AccessibilityHandler::postAnnouncement(
            lastAnnouncement, juce::AccessibilityHandler::AnnouncementPriority::medium);
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
        canvas.realiseAccessible();
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
        const int count = static_cast<int>(reader().tracks().size());
        geometry.height = std::clamp(canvas.getHeight() / std::max(1, count), 24, 512);
        geometry.scrollY = 0;
        geometry.heights.clear();
        break;
    }
    case AppCommands::GoStart:
        geometry.selection.insert = 0;
        geometry.selection.marked = true;
        locate();
        break;
    default:
        return false;
    }
    changed();
    return true;
}
void ArrangementView::locate() { root.locate(geometry.selection.insert); }
void ArrangementView::select(int x, int y) {
    if (const auto *t = geometry.trackAt(reader(), y)) {
        geometry.selection.track = t->id;
        const auto tick = geometry.tick(x);
        const auto *c = geometry.hit(*t, tick);
        geometry.selection.clip = c ? c->id : 0;
        geometry.selection.insert = tick;
        geometry.selection.marked = true;
        geometry.selection.start = c ? c->posTicks : tick;
        geometry.selection.end = c ? c->posTicks + c->lengthTicks : tick;
        locate();
        changed();
    }
}
void ArrangementView::selectClip(std::int64_t track, std::int64_t clip) {
    const auto *t = reader().findTrack(track);
    if (t == nullptr)
        return;
    for (const auto &c : t->clips)
        if (c->id == clip) {
            geometry.selection.track = track;
            geometry.selection.clip = clip;
            geometry.selection.start = c->posTicks;
            geometry.selection.end = c->posTicks + c->lengthTicks;
            changed();
            return;
        }
}
void ArrangementView::contextMenu(juce::Point<int> where, bool header) {
    auto &commands = root.commands();
    commands.activate(root);
    juce::PopupMenu menu;
    menu.addCommandItem(&commands.manager, AppCommands::InsertAudioTrack);
    menu.addCommandItem(&commands.manager, AppCommands::InsertMidiTrack);
    if (header && geometry.selection.track != 0) {
        menu.addSeparator();
        menu.addCommandItem(&commands.manager, AppCommands::RenameTrack);
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
    view.fit(view.geometry.selection.start != view.geometry.selection.end);
}
void TrackHeaderList::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff242b35));
    int y = -view.geometry.scrollY;
    std::size_t position = 0;
    for (const auto &t : view.reader().tracks()) {
        const int h = view.geometry.heightFor(t->id);
        ++position;
        g.setColour(juce::Colour(t->id == view.geometry.selection.track ? 0xff416976 : 0xff343e4b));
        g.fillRect(1, y, getWidth() - 1, h - 1);
        g.setColour(juce::Colour(0xffe0e5ed));
        if (t->id != renaming_)
            g.drawText(juce::String::fromUTF8(displayName(t->name, position).c_str()), 8, y + 4,
                       getWidth() - 12, 22, juce::Justification::centredLeft);
        y += h;
    }
}
void TrackHeaderList::resized() {
    if (renaming_ != 0)
        beginRename(renaming_);
}
bool TrackHeaderList::beginRename(std::int64_t track) {
    // The name as it is now, not as this window last drew it.
    int y = -view.geometry.scrollY;
    const auto now = view.root.current();
    for (const auto &t : now->tracks) {
        if (t->id == track) {
            renaming_ = track;
            view.geometry.selection.track = track;
            view.root.publishSelection();
            editor.setBounds(4, y + 3, std::max(0, getWidth() - 8), 24);
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
        y += view.geometry.heightFor(t->id);
    }
    return false;
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
    if (auto *t = view.geometry.trackAt(view.reader(), e.y)) {
        view.geometry.selection.track = t->id;
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
                juce::Colour(c->id == view.geometry.selection.clip ? 0xff89c7cf : 0xff488a9a));
            g.fillRect(left, static_cast<float>(y + 3), std::max(1.f, right - left),
                       static_cast<float>(h - 7));
        }
        y += h;
    }
    if (view.geometry.selection.start != view.geometry.selection.end) {
        const float x = static_cast<float>(
            view.geometry.x(std::min(view.geometry.selection.start, view.geometry.selection.end)));
        const float end = static_cast<float>(
            view.geometry.x(std::max(view.geometry.selection.start, view.geometry.selection.end)));
        g.setColour(juce::Colour(0x2265c4d0));
        g.fillRect(x, 0.f, end - x, static_cast<float>(getHeight()));
    } else if (view.geometry.selection.marked) {
        g.setColour(juce::Colour(0xffe0e5ed));
        g.drawVerticalLine(static_cast<int>(view.geometry.x(view.geometry.selection.insert)), 0,
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
        view.geometry.selection.end = view.geometry.tick(e.x);
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
            if (tracks[i]->id == view.geometry.selection.track) {
                at = i;
                break;
            }
        if (tracks.empty())
            return true;
        if (k.getKeyCode() == juce::KeyPress::downKey)
            at = at == tracks.size() ? 0 : std::min(at + 1, tracks.size() - 1);
        else
            at = at == tracks.size() || at == 0 ? 0 : at - 1;
        view.geometry.selection.track = tracks[at]->id;
        view.geometry.selection.clip = 0;
        view.changed();
        return true;
    }
    if (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey) {
        const auto *t = view.reader().findTrack(view.geometry.selection.track);
        if (!t || t->clips.empty())
            return false;
        std::size_t i = 0;
        for (; i < t->clips.size(); ++i)
            if (t->clips[i]->id == view.geometry.selection.clip)
                break;
        if (k.getKeyCode() == juce::KeyPress::rightKey)
            i = i == t->clips.size() ? 0 : std::min(i + 1, t->clips.size() - 1);
        else
            i = i > 0 && i < t->clips.size() ? i - 1 : 0;
        const auto &c = t->clips[i];
        view.geometry.selection.clip = c->id;
        view.geometry.selection.start = c->posTicks;
        view.geometry.selection.end = c->posTicks + c->lengthTicks;
        view.changed();
        announceSelection();
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
