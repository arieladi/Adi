// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_arrangement.hpp"
#include "ui_shell.hpp"
namespace adi::ui {
ArrangementCanvas::ArrangementCanvas(ArrangementView &v) : view(v) {
    setWantsKeyboardFocus(true);
    setTitle("Arrangement clips");
    setDescription("Select a track or clip with arrow keys; use View menu to zoom and navigate. "
                   "Drop audio files onto audio tracks.");
}
std::unique_ptr<juce::AccessibilityHandler> ArrangementCanvas::createAccessibilityHandler() {
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::list);
}
ArrangementView::ArrangementView(AdiRootComponent &r)
    : root(r), ruler(*this), headers(*this), canvas(*this) {

    for (auto *c :
         std::array<juce::Component *, 6>{&ruler, &headers, &canvas, &playhead, &add, &navigation})
        addAndMakeVisible(c);
    add.onClick = [this] {
        if (addTrack)
            addTrack();
    };
    navigation.onClick = [this] {
        juce::PopupMenu m;
        const char *names[] = {"Zoom in",       "Zoom out",       "Fit selection", "Previous zoom",
                               "Fit width",     "Fit height",     "Scroll left",   "Scroll right",
                               "Taller tracks", "Shorter tracks", "Go to start",   "Scroll up",
                               "Scroll down"};
        for (int i = 0; i < 13; ++i)
            m.addItem(i + 1, names[i]);
        juce::Component::SafePointer<ArrangementView> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&navigation),
                        [safe](int id) {
                            if (safe && id > 0)
                                safe->command(AppCommands::ZoomIn + id - 1);
                        });
    };
}
const SnapshotReader &ArrangementView::reader() const { return root.reader(); }
void ArrangementView::resized() {
    const int h = std::min(120, std::max(0, getWidth() / 3));
    add.setBounds(0, 0, h / 2, 28);
    navigation.setBounds(h / 2, 0, h - h / 2, 28);
    ruler.setBounds(h, 0, std::max(0, getWidth() - h), 28);
    headers.setBounds(0, 28, h, std::max(0, getHeight() - 28));
    canvas.setBounds(h, 28, std::max(0, getWidth() - h), std::max(0, getHeight() - 28));
    frame(true);
}
void ArrangementView::frame(bool modelChanged) noexcept {
    const int x = static_cast<int>(std::clamp(geometry.x(root.timelineTick()), -1e7, 1e7));
    playhead.setVisible(x >= 0 && x < canvas.getWidth());
    playhead.setBounds(canvas.getX() + x, 28, 1, canvas.getHeight());
    playhead.toFront(false);
    if (modelChanged) {
        canvas.repaint();
        headers.repaint();
        ruler.repaint();
    }
}
void ArrangementView::changed() {
    root.state().timelineLeft = geometry.left;
    root.state().pixelsPerQuarter = geometry.scale;
    root.state().laneHeight = geometry.height;
    root.state().laneHeights = geometry.heights;
    root.persist();
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
    const auto anchor = geometry.selectionStart != geometry.selectionEnd ? geometry.selectionStart
                                                                         : root.timelineTick();
    switch (id) {
    case AppCommands::ZoomIn:
        geometry.zoom(1.25, geometry.x(anchor));
        break;
    case AppCommands::ZoomOut:
        geometry.zoom(.8, geometry.x(anchor));
        break;
    case AppCommands::FitSelection:
        fit(geometry.selectionStart != geometry.selectionEnd);
        return true;
    case AppCommands::PreviousZoom:
        geometry.back();
        break;
    case AppCommands::FitWidth:
        fit(false);
        return true;
    case AppCommands::FitHeight: {
        int count = 0;
        for (const auto &t : reader().tracks())
            if (t->kind != "master")
                ++count;
        geometry.height = std::clamp(canvas.getHeight() / std::max(1, count), 24, 512);
        geometry.scrollY = 0;
        geometry.heights.clear();
        break;
    }
    case AppCommands::ScrollLeft:
        geometry.pan(-canvas.getWidth() / 2.);
        break;
    case AppCommands::ScrollRight:
        geometry.pan(canvas.getWidth() / 2.);
        break;
    case AppCommands::TallerTracks:
        geometry.resizeTrack(geometry.selectedTrack,
                             geometry.heightFor(geometry.selectedTrack) + 8);
        break;
    case AppCommands::ShorterTracks:
        geometry.resizeTrack(geometry.selectedTrack,
                             geometry.heightFor(geometry.selectedTrack) - 8);
        break;
    case AppCommands::ScrollUp:
        geometry.scrollY = std::max(0, geometry.scrollY - canvas.getHeight() / 2);
        break;
    case AppCommands::ScrollDown:
        geometry.scrollY += canvas.getHeight() / 2;
        break;
    case AppCommands::GoStart:
        geometry.insert = 0;
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
        geometry.selectionStart = c ? c->posTicks : tick;
        geometry.selectionEnd = c ? c->posTicks + c->lengthTicks : tick;
        locate();
        changed();
    }
}
void ArrangementView::wheel(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (e.mods.isCommandDown())
        geometry.zoom(std::exp(w.deltaY * 2), e.position.x);
    else if (e.mods.isAltDown()) {
        if (const auto *track = geometry.trackAt(reader(), e.y))
            geometry.wheelHeight(reader(), track->id, static_cast<int>(w.deltaY * 100));
    } else if (e.mods.isShiftDown())
        geometry.pan(-w.deltaY * 300);
    else {
        geometry.pan(-w.deltaX * 300);
        geometry.scrollY = std::max(0, geometry.scrollY - static_cast<int>(w.deltaY * 300));
    }
    changed();
}
void TimelineRuler::paint(juce::Graphics &g) {
    g.fillAll(juce::Colour(0xff303944));
    g.setColour(juce::Colour(0xffd1d8e0));
    g.setFont(juce::FontOptions(12.f));
    const double step = std::max(1., std::pow(2., std::ceil(std::log2(32. / view.geometry.scale))));
    for (double q = std::floor(view.geometry.left / step) * step;
         q < view.geometry.left + getWidth() / view.geometry.scale; q += step) {
        const int x = static_cast<int>((q - view.geometry.left) * view.geometry.scale);
        g.drawVerticalLine(x, 20, 28);
        g.drawText(juce::String(
                       view.reader().get()
                           ? textproj::renderPosition(static_cast<std::int64_t>(q * textproj::kPPQ),
                                                      view.reader().get()->meters)
                           : "1|1|0"),
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
        if (t->kind == "master")
            continue;
        g.setColour(juce::Colour(t->id == view.geometry.selectedTrack ? 0xff416976 : 0xff343e4b));
        g.fillRect(0, y, getWidth() - 1, view.geometry.heightFor(t->id) - 1);
        g.setColour(juce::Colour(0xffe0e5ed));
        g.drawText(juce::String::fromUTF8(t->name.c_str()), 8, y + 4, getWidth() - 12, 22,
                   juce::Justification::centredLeft);
        y += view.geometry.heightFor(t->id);
    }
}
void TrackHeaderList::mouseDown(const juce::MouseEvent &e) {
    if (auto *t = view.geometry.trackAt(view.reader(), e.y)) {
        view.geometry.selectedTrack = t->id;
        resizeId = t->id;
        initialHeight = view.geometry.heightFor(t->id);
        view.changed();
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
        if (t->kind == "master")
            continue;
        g.setColour(juce::Colour(0xff313b47));
        g.drawHorizontalLine(y + view.geometry.heightFor(t->id) - 1, 0,
                             static_cast<float>(getWidth()));
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
                       static_cast<float>(view.geometry.heightFor(t->id) - 7));
            g.setColour(juce::Colour(0xff10232b));
            g.drawText(juce::String::fromUTF8(c->name.c_str()), static_cast<int>(left) + 4, y + 5,
                       std::max(0, static_cast<int>(right - left) - 8), 22,
                       juce::Justification::centredLeft);
        }
        y += view.geometry.heightFor(t->id);
    }
    if (view.geometry.selectionStart != view.geometry.selectionEnd) {
        const float x = static_cast<float>(
            view.geometry.x(std::min(view.geometry.selectionStart, view.geometry.selectionEnd)));
        const float end = static_cast<float>(
            view.geometry.x(std::max(view.geometry.selectionStart, view.geometry.selectionEnd)));
        g.setColour(juce::Colour(0x2265c4d0));
        g.fillRect(x, 0.f, end - x, static_cast<float>(getHeight()));
    }
}
void ArrangementCanvas::mouseDown(const juce::MouseEvent &e) {
    grabKeyboardFocus();
    last = e.position;
    panning = e.mods.isCommandDown() && e.mods.isAltDown();
    if (!panning)
        view.select(e.x, e.y);
}
void ArrangementCanvas::mouseDrag(const juce::MouseEvent &e) {
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
    if (k.getKeyCode() == juce::KeyPress::upKey || k.getKeyCode() == juce::KeyPress::downKey) {
        const auto &tracks = view.reader().tracks();
        std::size_t at = tracks.size();
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i]->id == view.geometry.selectedTrack) {
                at = i;
                break;
            }
        if (k.getKeyCode() == juce::KeyPress::downKey) {
            for (std::size_t i = at == tracks.size() ? 0 : at + 1; i < tracks.size(); ++i)
                if (tracks[i]->kind != "master") {
                    view.geometry.selectedTrack = tracks[i]->id;
                    break;
                }
        } else
            for (std::size_t i = at; i > 0;) {
                --i;
                if (tracks[i]->kind != "master") {
                    view.geometry.selectedTrack = tracks[i]->id;
                    break;
                }
            }
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
            i = std::min(i + 1, t->clips.size() - 1);
        else
            i = i > 0 ? i - 1 : 0;
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
void ArrangementCanvas::filesDropped(const juce::StringArray &files, int x, int y) {
    if (files.size() != 1 || !view.drop)
        return;
    if (const auto *t = view.geometry.trackAt(view.reader(), y))
        view.drop(files[0], t->id, view.geometry.tick(x));
}
} // namespace adi::ui
