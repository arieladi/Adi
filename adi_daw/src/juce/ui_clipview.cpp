// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_clipview.hpp"
#include "ui_shell.hpp"
#include "adi/ui/meters.hpp"
#include "adi/ui/waveform.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
namespace adi::ui {
namespace {
// Live's position fields read bar. beat. sixteenth from 1 (manual 8.2).
juce::String positionText(std::int64_t tick, const std::vector<textproj::Meter> &meters) {
    const bool before = tick < 0;
    const auto at = barPosition(before ? -tick : tick, meters);
    return (before ? juce::String("-") : juce::String()) + juce::String(at.bar) + ". " + juce::String(at.beat) + ". " +
           juce::String(at.sixteenth);
}
juce::String lengthText(std::int64_t ticks, const textproj::Meter &meter) {
    const auto l = barsBeats(ticks, meter);
    return juce::String(l.bars) + ". " + juce::String(l.beats) + ". " + juce::String(l.sixteenths);
}
// The time ruler both editors share: the song's bars along the foot of a dark band, as the
// arrangement's beat-time ruler draws them, and beats when they have room.
void paintRuler(juce::Graphics &g, juce::Rectangle<int> area, const ArrangementGeometry &time,
                const std::vector<textproj::Meter> &meters) {
    g.setColour(look::band);
    g.fillRect(area);
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(area);
    const auto from = time.tick(0.), to = time.tick(static_cast<double>(area.getWidth()));
    const auto &first = meterAt(from, meters);
    const double beatPx = 4. / std::max<double>(1, first.denominator) * time.scale;
    std::int64_t beatStep = 1;
    while (beatStep < first.numerator && static_cast<double>(beatStep) * beatPx < 56)
        beatStep *= 2;
    const bool beats = beatStep < first.numerator;
    std::int64_t barStep = 1;
    while (static_cast<double>(barStep) * beatPx * first.numerator < 56)
        barStep *= 2;
    const int foot = area.getBottom();
    g.setFont(look::font(11.f, true));
    auto label = [&](std::int64_t tick, const juce::String &text) {
        const int x = area.getX() + static_cast<int>(time.x(tick));
        g.setColour(juce::Colour(0xff7d7d7d));
        g.fillRect(x, foot - 9, 1, 7);
        g.setColour(look::textOnBand);
        g.drawText(text, x + 4, foot - 14, 60, 13, juce::Justification::centredLeft);
    };
    forEachBar(from - first.barTicks(), to, meters, [&](std::int64_t tick, std::int64_t bar) {
        if (!beats) {
            if ((bar - 1) % barStep == 0)
                label(tick, juce::String(bar));
            return;
        }
        label(tick, juce::String(bar));
        const auto &m = meterAt(tick, meters);
        const auto beatTicks = textproj::kWhole / std::max<std::int64_t>(1, m.denominator);
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
// The editing grid's lines and the bar lines over them, across `area`.
void paintGrid(juce::Graphics &g, juce::Rectangle<int> area, const ArrangementGeometry &time,
               const std::vector<textproj::Meter> &meters) {
    const auto from = time.tick(0.), to = time.tick(static_cast<double>(area.getWidth()));
    g.setColour(look::lane.darker(0.2f));
    time.forEachGridLine(from, to, [&](std::int64_t tick) {
        g.fillRect(area.getX() + static_cast<int>(time.x(tick)), area.getY(), 1, area.getHeight());
    });
    g.setColour(look::lane.darker(0.5f));
    forEachBar(from, to, meters, [&](std::int64_t tick, std::int64_t) {
        g.fillRect(area.getX() + static_cast<int>(time.x(tick)), area.getY(), 1, area.getHeight());
    });
}
// What lies outside the clip is there, and dimmed.
void shadeOutside(juce::Graphics &g, juce::Rectangle<int> area, const ArrangementGeometry &time,
                  const ClipWindow &window) {
    const int start = area.getX() + static_cast<int>(std::clamp(time.x(window.pos), -1., 1e6));
    const int end = area.getX() + static_cast<int>(std::clamp(time.x(window.end()), -1., 1e6));
    g.setColour(juce::Colours::black.withAlpha(0.16f));
    if (start > area.getX())
        g.fillRect(area.getX(), area.getY(), start - area.getX(), area.getHeight());
    if (end < area.getRight())
        g.fillRect(std::max(area.getX(), end), area.getY(), area.getRight() - std::max(area.getX(), end),
                   area.getHeight());
}
// The wheel in an editor: Ctrl/Cmd zooms time around the pointer, Shift (or a sideways wheel)
// scrolls time, as in the arrangement (manual 10.2).
bool timeWheel(ArrangementGeometry &time, const juce::MouseEvent &e, const juce::MouseWheelDetails &w, int originX) {
    if (e.mods.isCommandDown()) {
        time.zoom(std::pow(2., static_cast<double>(w.deltaY) * 2.), static_cast<double>(e.x - originX));
        return true;
    }
    if (e.mods.isShiftDown() || std::abs(w.deltaX) > std::abs(w.deltaY)) {
        const float delta = e.mods.isShiftDown() && std::abs(w.deltaX) < 1e-6f ? w.deltaY : w.deltaX;
        time.pan(static_cast<double>(-delta) * 240.);
        return true;
    }
    return false;
}
} // namespace

// ---------------------------------------------------------------------------------------------
NoteEditor::NoteEditor(ClipViewPanel &p) : panel(p) {
    setTitle("MIDI Note Editor");
    setOpaque(true);
    setWantsKeyboardFocus(true);
    addChildComponent(playhead);
}
void NoteEditor::frame() {
    // Live draws the playhead only while it plays; here over the key tracks and the Velocity lane.
    const auto area = notesArea();
    const int x = static_cast<int>(std::clamp(time.x(panel.root.timelineTick()), -1e7, 1e7));
    playhead.setVisible(panel.details() != nullptr && panel.root.playing() && x >= 0 && x < area.getWidth());
    playhead.setBounds(area.getX() + x, area.getY(), 1, getHeight() - area.getY());
}
void NoteEditor::setLaneShape(LaneShape shape) {
    shape_ = shape;
    repaint();
}
juce::Rectangle<int> NoteEditor::shapeBox() const noexcept {
    const auto lane = velocityArea();
    return lane.isEmpty() ? juce::Rectangle<int>() : juce::Rectangle<int>(2, lane.getY() + 18, kKeysWidth - 5, 13);
}
juce::PopupMenu NoteEditor::shapeMenu() const {
    juce::PopupMenu menu;
    int id = 1;
    for (const auto *name : kLaneShapeNames) {
        menu.addItem(id, name, true, static_cast<int>(shape_) == id - 1);
        ++id;
    }
    return menu;
}
bool NoteEditor::chooseShape(int item) {
    if (item < 1 || item > static_cast<int>(kLaneShapeNames.size()))
        return false;
    setLaneShape(static_cast<LaneShape>(item - 1));
    return true;
}
void NoteEditor::shapeStroke() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    const auto lane = velocityArea().withTrimmedTop(1);
    const double top = lane.getY() + 5, height = lane.getHeight() - 10;
    const auto window = ClipWindow::of(*clip);
    const auto xOf = [&](const engine::ClipNote &n) { return lane.getX() + time.x(window.toArrangement(n.startTicks)); };
    if (shape_ == LaneShape::Paint) {
        // Paint: every note the pointer passes takes the value under it, and keeps it.
        const int value = velocityAt(to_.y, top, height);
        for (const auto &n : clip->notes) {
            if (std::abs(xOf(n) - to_.x) > 4)
                continue;
            const auto had = std::find_if(shapePreview_.begin(), shapePreview_.end(),
                                          [&](const auto &v) { return v.first == n.id; });
            if (had != shapePreview_.end())
                had->second = value;
            else
                shapePreview_.emplace_back(n.id, value);
        }
        return;
    }
    // The others: every note between the stroke's ends takes the shape's value at its place.
    shapePreview_.clear();
    const double first = velocityAt(from_.y, top, height), last = velocityAt(to_.y, top, height);
    const double x0 = from_.x, x1 = to_.x;
    const double cycles = std::abs(x1 - x0) / time.scale * static_cast<double>(textproj::kPPQ) /
                          (4. * static_cast<double>(gridTicks()));
    for (const auto &n : clip->notes) {
        const double x = xOf(n);
        if (x < std::min(x0, x1) - .5 || x > std::max(x0, x1) + .5)
            continue;
        const double t = x1 == x0 ? 1. : (x - x0) / (x1 - x0);
        shapePreview_.emplace_back(
            n.id, std::clamp(static_cast<int>(std::lround(shapeValue(shape_, t, first, last, cycles))), 1, 127));
    }
}
void NoteEditor::setTrimMode(bool on) {
    trimMode_ = on;
    if (on)
        drawMode_ = false;
    setMouseCursor(on ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    panel.repaint();
}
void NoteEditor::trimStroke() {
    trimPreview_.clear();
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    // Each note the stroke crosses (its key track between the stroke's ends, the stroke inside its
    // length) ends where the stroke is, on the grid unless snapping is bypassed.
    const auto area = notesArea();
    const auto window = ClipWindow::of(*clip);
    const auto at = time.tick(static_cast<double>(to_.x - area.getX()));
    const auto cut = window.toSource(time.grid.snap ? time.snap(at, false) : at);
    const int top = std::min(from_.y, to_.y), bottom = std::max(from_.y, to_.y);
    for (const auto &n : clip->notes) {
        const auto box = boxOf(n);
        if (!box || box->y > bottom || box->y + box->height <= top)
            continue;
        if (static_cast<double>(to_.x) <= box->x || static_cast<double>(to_.x) >= box->x + box->width)
            continue;
        if (cut > n.startTicks && cut < n.startTicks + n.durationTicks)
            trimPreview_.emplace_back(n.id, cut - n.startTicks);
    }
}
juce::Rectangle<int> NoteEditor::selectedVelocityFrame() const noexcept {
    const auto *clip = panel.details();
    const auto lane = velocityArea().withTrimmedTop(1);
    if (clip == nullptr || lane.isEmpty() || selected_.size() < 2 || shape_ != LaneShape::Pointer)
        return {};
    const double top = lane.getY() + 5, height = lane.getHeight() - 10;
    const auto window = ClipWindow::of(*clip);
    int left = 0, right = 0, high = 0, low = 0;
    bool any = false;
    for (const auto &stored : clip->notes) {
        if (!isSelected(stored.id))
            continue;
        const auto n = shown(stored);
        const int x = lane.getX() + static_cast<int>(std::clamp(time.x(window.toArrangement(n.startTicks)), -1e6, 1e6));
        const int y = static_cast<int>(velocityY(n.velocity, top, height));
        left = any ? std::min(left, x) : x;
        right = any ? std::max(right, x) : x;
        high = any ? std::min(high, y) : y;
        low = any ? std::max(low, y) : y;
        any = true;
    }
    return any ? juce::Rectangle<int>(left, high, std::max(1, right - left), std::max(1, low - high)) : juce::Rectangle<int>();
}
juce::Rectangle<int> NoteEditor::handleBox(LaneHandle handle) const noexcept {
    const auto frame = selectedVelocityFrame();
    if (frame.isEmpty() || handle == LaneHandle::None)
        return {};
    // Above the selection's frame: the tilt handles at its ends, Scale at its middle; Compress under Scale.
    const int y = std::max(velocityArea().getY() + 2, frame.getY() - 9);
    const int x = handle == LaneHandle::TiltLeft    ? frame.getX() - 3
                  : handle == LaneHandle::TiltRight ? frame.getRight() - 3
                                                    : frame.getCentreX() - 3;
    return {x, handle == LaneHandle::Compress ? y + 9 : y, 7, 7};
}
LaneHandle NoteEditor::handleAt(juce::Point<int> at) const noexcept {
    for (const auto handle : {LaneHandle::TiltLeft, LaneHandle::TiltRight, LaneHandle::Scale, LaneHandle::Compress})
        if (const auto box = handleBox(handle); !box.isEmpty() && box.expanded(2).contains(at))
            return handle;
    return LaneHandle::None;
}
void NoteEditor::handleDrag() {
    shapePreview_.clear();
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    // The drag in lane units: up raises.
    const auto lane = velocityArea().withTrimmedTop(1);
    const double height = std::max(1, lane.getHeight() - 10);
    const double by = static_cast<double>(from_.y - to_.y) * 127. / height;
    std::int64_t first = 0, last = 0;
    double sum = 0, highest = 0;
    std::size_t count = 0;
    for (const auto &n : clip->notes) {
        if (!isSelected(n.id))
            continue;
        first = count == 0 ? n.startTicks : std::min(first, n.startTicks);
        last = count == 0 ? n.startTicks : std::max(last, n.startTicks);
        sum += n.velocity;
        highest = std::max(highest, static_cast<double>(n.velocity));
        ++count;
    }
    if (count == 0)
        return;
    const double mean = sum / static_cast<double>(count);
    for (const auto &n : clip->notes) {
        if (!isSelected(n.id))
            continue;
        const double t = last == first ? 0. : static_cast<double>(n.startTicks - first) / static_cast<double>(last - first);
        const double value = handleValue(handle_, n.velocity, t, by, mean, highest);
        shapePreview_.emplace_back(n.id, std::clamp(static_cast<int>(std::lround(value)), 1, 127));
    }
}
void NoteEditor::setDrawMode(bool on) {
    drawMode_ = on;
    if (on)
        trimMode_ = false;
    setMouseCursor(on ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    panel.repaint();
}
std::vector<std::int64_t> NoteEditor::drawnSteps() const {
    std::vector<std::int64_t> out;
    const auto *clip = panel.details();
    if (clip == nullptr || drag_ != Drag::Draw)
        return out;
    // Every grid step from where the stroke began to where it is, whichever way it went.
    const auto area = notesArea();
    const auto window = ClipWindow::of(*clip);
    const auto a = time.snap(time.tick(static_cast<double>(std::min(from_.x, to_.x) - area.getX())), false);
    const auto b = time.tick(static_cast<double>(std::max(from_.x, to_.x) - area.getX()));
    const auto step = gridTicks();
    for (auto at = a; at <= b && out.size() < 4096; at += step)
        out.push_back(window.toSource(at));
    return out;
}
void NoteEditor::setFold(bool fold) {
    const auto *clip = panel.details();
    const int height = notesArea().getHeight();
    if (fold && clip != nullptr && !clip->notes.empty()) {
        keys.fold(clip->notes, height);
        folded_ = true;
    } else {
        const bool was = folded_;
        folded_ = fold && clip != nullptr;
        keys.unfold(height);
        // Unfolded, the notes come back into view.
        if (was && clip != nullptr)
            if (const auto range = keyRange(clip->notes))
                keys.show(range->low, range->high, height);
    }
    repaint();
}
void NoteEditor::zoomToNotes() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    std::vector<engine::ClipNote> shownNotes;
    for (const auto &n : clip->notes)
        if (selected_.empty() || isSelected(n.id))
            shownNotes.push_back(n);
    const auto range = keyRange(shownNotes);
    if (!range)
        return;
    std::int64_t from = shownNotes.front().startTicks, to = from;
    for (const auto &n : shownNotes) {
        from = std::min(from, n.startTicks);
        to = std::max(to, n.startTicks + n.durationTicks);
    }
    const auto window = ClipWindow::of(*clip);
    const auto area = notesArea();
    const auto margin = std::max<std::int64_t>(1, (to - from) / 24);
    time.fit(std::max<std::int64_t>(0, window.toArrangement(from)), window.toArrangement(to) + margin, area.getWidth());
    keys.show(range->low, range->high, area.getHeight());
    repaint();
}
void NoteEditor::zoomBack() {
    time.back();
    repaint();
}
bool NoteEditor::duplicate() {
    const auto *clip = panel.details();
    if (clip == nullptr || selected_.empty())
        return false;
    return commit(copyNotes(*clip, selected_, spanOf(*clip, selected_), 0,
                            selected_.size() == 1 ? "Duplicate Note" : "Duplicate Notes"));
}
juce::Rectangle<int> NoteEditor::rulerArea() const noexcept {
    return {kKeysWidth, 0, std::max(0, getWidth() - kKeysWidth), kRulerHeight};
}
juce::Rectangle<int> NoteEditor::velocityArea() const noexcept {
    const int h = getHeight() - kRulerHeight >= kVelocityHeight + 48 ? kVelocityHeight : 0;
    return {kKeysWidth, getHeight() - h, std::max(0, getWidth() - kKeysWidth), h};
}
juce::Rectangle<int> NoteEditor::notesArea() const noexcept {
    const int bottom = getHeight() - velocityArea().getHeight();
    return {kKeysWidth, kRulerHeight, std::max(0, getWidth() - kKeysWidth), std::max(0, bottom - kRulerHeight)};
}
juce::Rectangle<int> NoteEditor::keysArea() const noexcept {
    const auto n = notesArea();
    return {0, n.getY(), kKeysWidth, n.getHeight()};
}
std::optional<NoteBox> NoteEditor::boxOf(const engine::ClipNote &note) const noexcept {
    const auto *clip = panel.details();
    const auto y = keys.y(note.key);
    if (clip == nullptr || !y)
        return std::nullopt;
    const auto window = ClipWindow::of(*clip);
    const auto area = notesArea();
    const double x = time.x(window.toArrangement(note.startTicks));
    const double end = time.x(window.toArrangement(note.startTicks + note.durationTicks));
    return NoteBox{static_cast<double>(area.getX()) + x, std::max(1., end - x), area.getY() + *y, keys.keyHeight};
}
std::optional<juce::Range<int>> NoteEditor::loopBrace() const noexcept {
    const auto *clip = panel.details();
    if (clip == nullptr || loopLengthOf(*clip) <= 0)
        return std::nullopt;
    const auto window = ClipWindow::of(*clip);
    const auto ruler = rulerArea();
    const auto at = [&](std::int64_t source) {
        return ruler.getX() + static_cast<int>(std::clamp(time.x(window.toArrangement(source)), -1e6, 1e6));
    };
    return juce::Range<int>(at(clip->loopStartTicks), at(clip->loopStartTicks + loopLengthOf(*clip)));
}
bool NoteEditor::isSelected(std::uint64_t id) const noexcept {
    return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}
void NoteEditor::select(std::vector<std::uint64_t> ids) {
    selected_ = std::move(ids);
    repaint();
}
void NoteEditor::prune() {
    const auto *clip = panel.details();
    std::erase_if(selected_, [clip](std::uint64_t id) {
        return clip == nullptr || note_edits_detail::find(*clip, id) == nullptr;
    });
    if (folded_)
        setFold(true);
}
std::int64_t NoteEditor::gridTicks() const noexcept {
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(time.gridQuarters() * static_cast<double>(textproj::kPPQ)));
}
bool NoteEditor::snapping(const juce::ModifierKeys &mods) const noexcept {
    // Alt (Cmd on the Mac) bypasses snapping; with the grid off, it snaps while held (manual 10.2.1).
#if JUCE_MAC
    const bool bypass = mods.isCommandDown();
#else
    const bool bypass = mods.isAltDown();
#endif
    return time.grid.snap != bypass;
}
const engine::ClipNote *NoteEditor::noteAt(juce::Point<int> at, NotePart *part) const noexcept {
    const auto *clip = panel.details();
    if (clip == nullptr || !notesArea().contains(at))
        return nullptr;
    for (auto it = clip->notes.rbegin(); it != clip->notes.rend(); ++it)
        if (const auto box = boxOf(*it))
            if (const auto p = notePart(*box, at.x, at.y); p != NotePart::None) {
                if (part != nullptr)
                    *part = p;
                return &*it;
            }
    return nullptr;
}
engine::ClipNote NoteEditor::shown(const engine::ClipNote &note) const noexcept {
    auto out = note;
    if (drag_ == Drag::Trim) {
        for (const auto &[id, length] : trimPreview_)
            if (id == note.id)
                out.durationTicks = length;
        return out;
    }
    if (drag_ == Drag::Shape || drag_ == Drag::Handle) {
        for (const auto &[id, value] : shapePreview_)
            if (id == note.id)
                out.velocity = static_cast<std::uint8_t>(value);
        return out;
    }
    if (drag_ == Drag::None || drag_ == Drag::Band || !isSelected(note.id))
        return out;
    if (drag_ == Drag::Move) {
        out.startTicks += dragTicks_;
        out.key = static_cast<std::uint8_t>(std::clamp(static_cast<int>(note.key) + dragKeys_, 0, kNoteKeys - 1));
    } else if (drag_ == Drag::Start) {
        const auto by = std::min(dragTicks_, std::max<std::int64_t>(0, note.durationTicks - kShortestNote));
        out.startTicks += by;
        out.durationTicks -= by;
    } else if (drag_ == Drag::End) {
        out.durationTicks = std::max(kShortestNote, note.durationTicks + dragTicks_);
    } else if (drag_ == Drag::Velocity) {
        out.velocity = static_cast<std::uint8_t>(std::clamp(static_cast<int>(note.velocity) + dragVelocity_, 1, 127));
    }
    return out;
}
bool NoteEditor::commit(NoteEdit edit) {
    repaint();
    if (!edit.refusal.empty()) {
        panel.root.say(std::move(edit.refusal));
        return false;
    }
    if (edit.ops.empty())
        return true;
    // One batch, one undo step; the clip is read again at once, so the next event sees it.
    if (!panel.root.submitEdits(std::move(edit.ops)))
        return false;
    selected_ = std::move(edit.select);
    panel.reread();
    return true;
}
void NoteEditor::mouseDown(const juce::MouseEvent &e) {
    grabKeyboardFocus();
    drag_ = Drag::None;
    copying_ = false;
    dragTicks_ = 0;
    dragKeys_ = dragVelocity_ = 0;
    from_ = to_ = e.getPosition();
    const auto *clip = panel.details();
    if (clip == nullptr || e.mods.isPopupMenu())
        return;
    if (shapeBox().contains(from_)) {
        juce::Component::SafePointer<NoteEditor> safe(this);
        shapeMenu().showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(shapeBox())),
            [safe](int item) {
                if (safe != nullptr && item > 0)
                    safe->chooseShape(item);
            });
        return;
    }
    if (const auto lane = velocityArea(); lane.contains(from_)) {
        if (shape_ != LaneShape::Pointer && from_.x >= lane.getX()) {
            // A line tool's stroke begins.
            shapePreview_.clear();
            drag_ = Drag::Shape;
            shapeStroke();
            repaint();
            return;
        }
        if (const auto handle = handleAt(from_); handle != LaneHandle::None) {
            handle_ = handle;
            drag_ = Drag::Handle;
            handleDrag();
            repaint();
            return;
        }
        // The marker nearest the pointer, within a few pixels.
        const auto window = ClipWindow::of(*clip);
        const engine::ClipNote *best = nullptr;
        double nearest = 6;
        for (const auto &n : clip->notes) {
            const double d = std::abs(lane.getX() + time.x(window.toArrangement(n.startTicks)) - from_.x);
            if (d < nearest) {
                nearest = d;
                best = &n;
            }
        }
        if (best != nullptr) {
            if (!isSelected(best->id))
                selected_ = {best->id};
            anchor_ = best->id;
            drag_ = Drag::Velocity;
        }
        repaint();
        return;
    }
    if (trimMode_ && notesArea().contains(from_)) {
        drag_ = Drag::Trim;
        trimStroke();
        repaint();
        return;
    }
    if (drawMode_ && notesArea().contains(from_)) {
        // On a note, it deletes it; on empty space, a stroke begins on that key.
        if (const auto *n = noteAt(from_)) {
            commit(deleteNotes(*clip, {n->id}));
        } else if (const auto key = keys.keyAt(from_.y - notesArea().getY())) {
            drawKey_ = *key;
            selected_.clear();
            drag_ = Drag::Draw;
        }
        repaint();
        return;
    }
    NotePart part = NotePart::None;
    if (const auto *n = noteAt(from_, &part)) {
        // A click selects the note; Shift adds it to the selection or takes it out (manual 10.5.2).
        if (e.mods.isShiftDown()) {
            if (isSelected(n->id))
                std::erase(selected_, n->id);
            else
                selected_.push_back(n->id);
        } else if (!isSelected(n->id)) {
            selected_ = {n->id};
        }
        if (isSelected(n->id)) {
            anchor_ = n->id;
            drag_ = part == NotePart::Start ? Drag::Start : part == NotePart::End ? Drag::End : Drag::Move;
        }
    } else if (notesArea().contains(from_)) {
        if (!e.mods.isShiftDown())
            selected_.clear();
        bandBase_ = selected_;
        drag_ = Drag::Band;
    }
    repaint();
}
void NoteEditor::mouseDrag(const juce::MouseEvent &e) {
    const auto *clip = panel.details();
    if (clip == nullptr || drag_ == Drag::None)
        return;
    to_ = e.getPosition();
#if JUCE_MAC
    copying_ = drag_ == Drag::Move && e.mods.isAltDown();
#else
    copying_ = drag_ == Drag::Move && e.mods.isCtrlDown();
#endif
    const auto area = notesArea();
    if (drag_ == Drag::Move || drag_ == Drag::Start || drag_ == Drag::End) {
        // Notes keep their offset from the grid: the drag moves by whole grid steps (manual 10.2.1).
        const double raw = static_cast<double>(to_.x - from_.x) / time.scale * static_cast<double>(textproj::kPPQ);
        const auto step = static_cast<double>(gridTicks());
        dragTicks_ = snapping(e.mods) ? static_cast<std::int64_t>(std::llround(raw / step) * static_cast<long long>(step))
                                      : static_cast<std::int64_t>(std::llround(raw));
    }
    if (drag_ == Drag::Move) {
        const auto a = keys.keyAt(from_.y - area.getY());
        const auto b = keys.keyAt(std::clamp(to_.y - area.getY(), 0, std::max(0, area.getHeight() - 1)));
        int low = kNoteKeys - 1, high = 0;
        for (const auto &n : clip->notes)
            if (isSelected(n.id)) {
                low = std::min(low, static_cast<int>(n.key));
                high = std::max(high, static_cast<int>(n.key));
            }
        dragKeys_ = a && b && low <= high ? std::clamp(*b - *a, -low, kNoteKeys - 1 - high) : 0;
    } else if (drag_ == Drag::Velocity) {
        const auto lane = velocityArea().withTrimmedTop(1);
        if (const auto *n = note_edits_detail::find(*clip, anchor_))
            dragVelocity_ = velocityAt(to_.y, lane.getY() + 5, lane.getHeight() - 10) - static_cast<int>(n->velocity);
    } else if (drag_ == Drag::Shape) {
        shapeStroke();
    } else if (drag_ == Drag::Trim) {
        trimStroke();
    } else if (drag_ == Drag::Handle) {
        handleDrag();
    } else if (drag_ == Drag::Band) {
        // The notes the rectangle touches, added to what Shift kept.
        const juce::Rectangle<int> band(from_, to_);
        selected_ = bandBase_;
        for (const auto &n : clip->notes)
            if (const auto box = boxOf(n); box && !isSelected(n.id) &&
                                             band.toDouble().intersects(juce::Rectangle<double>(
                                                 box->x, static_cast<double>(box->y), box->width,
                                                 static_cast<double>(box->height))))
                selected_.push_back(n.id);
    }
    repaint();
}
void NoteEditor::mouseUp(const juce::MouseEvent &) {
    const auto kind = std::exchange(drag_, Drag::None);
    const auto ticks = std::exchange(dragTicks_, 0);
    const int keysBy = std::exchange(dragKeys_, 0), velocity = std::exchange(dragVelocity_, 0);
    const auto *clip = panel.details();
    repaint();
    if (clip == nullptr)
        return;
    const bool copy = std::exchange(copying_, false);
    if (kind == Drag::Move && copy && (ticks != 0 || keysBy != 0))
        commit(copyNotes(*clip, selected_, ticks, keysBy, selected_.size() == 1 ? "Copy Note" : "Copy Notes"));
    else if (kind == Drag::Move)
        commit(moveNotes(*clip, selected_, ticks, keysBy));
    else if (kind == Drag::Start)
        commit(trimNotes(*clip, selected_, ticks));
    else if (kind == Drag::End)
        commit(resizeNotes(*clip, selected_, ticks));
    else if (kind == Drag::Velocity)
        commit(changeVelocities(*clip, selected_, velocity));
    else if (kind == Drag::Shape) {
        const auto values = std::exchange(shapePreview_, {});
        commit(drawVelocities(*clip, values));
    } else if (kind == Drag::Handle) {
        const auto values = std::exchange(shapePreview_, {});
        const auto handle = std::exchange(handle_, LaneHandle::None);
        commit(drawVelocities(*clip, values,
                              handle == LaneHandle::Scale      ? "Scale Velocities"
                              : handle == LaneHandle::Compress ? "Compress Velocities"
                                                               : "Tilt Velocities"));
    } else if (kind == Drag::Trim) {
        const auto lengths = std::exchange(trimPreview_, {});
        const auto keep = selected_;
        if (commit(trimNotesTo(*clip, lengths)) && !lengths.empty())
            select(keep);
    } else if (kind == Drag::Draw) {
        drag_ = Drag::Draw;
        const auto steps = drawnSteps();
        drag_ = Drag::None;
        commit(drawNotes(*clip, steps, gridTicks(), drawKey_));
    }
}
void NoteEditor::mouseDoubleClick(const juce::MouseEvent &e) {
    const auto *clip = panel.details();
    if (clip == nullptr || e.mods.isPopupMenu())
        return;
    drag_ = Drag::None;
    if (drawMode_)
        return;
    const auto at = e.getPosition();
    const auto area = notesArea();
    // On a note, it deletes it; on empty space, it adds one a grid step long (manual 10.4).
    if (const auto *n = noteAt(at)) {
        commit(deleteNotes(*clip, {n->id}));
        return;
    }
    if (!area.contains(at))
        return;
    const auto key = keys.keyAt(at.y - area.getY());
    if (!key)
        return;
    const auto tick = time.tick(static_cast<double>(at.x - area.getX()));
    const auto start = snapping(e.mods) ? time.snap(tick, false) : tick;
    commit(addNote(*clip, ClipWindow::of(*clip).toSource(start), gridTicks(), *key));
}
bool NoteEditor::keyPressed(const juce::KeyPress &k) {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();
    // Delete is the editor's while it has the keyboard: with no note selected it does nothing,
    // and never reaches the arrangement's selection.
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
        if (!selected_.empty())
            commit(deleteNotes(*clip, selected_));
        return true;
    }
    if (k == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)) {
        // Duplicate is the editor's while it has the keyboard, with or without notes selected.
        duplicate();
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'b' || code == 'B')) {
        setDrawMode(!drawMode_);
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'f' || code == 'F')) {
        setFold(!folded_);
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'z' || code == 'Z')) {
        zoomToNotes();
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'x' || code == 'X')) {
        zoomBack();
        return true;
    }
    if (k == juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) {
        selected_.clear();
        for (const auto &n : clip->notes)
            selected_.push_back(n.id);
        repaint();
        return true;
    }
    if (code == juce::KeyPress::escapeKey && !selected_.empty()) {
        select({});
        return true;
    }
    // 0 deactivates the selected notes, or activates them when all are off (manual 10.5.7).
    if (code == '0' && !mods.isAnyModifierKeyDown() && !selected_.empty()) {
        commit(toggleNotesActive(*clip, selected_));
        return true;
    }
    if (mods.isCommandDown() || mods.isAltDown() || selected_.empty())
        return false;
    // The arrows move the selected notes by the grid and by a key; with Shift, up and down move an
    // octave and left and right change the length (manual 10.5.4, 10.5.5).
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
        const auto by = code == juce::KeyPress::rightKey ? gridTicks() : -gridTicks();
        commit(mods.isShiftDown() ? resizeNotes(*clip, selected_, by) : moveNotes(*clip, selected_, by, 0));
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
        const int by = (code == juce::KeyPress::upKey ? 1 : -1) * (mods.isShiftDown() ? 12 : 1);
        commit(moveNotes(*clip, selected_, 0, by));
        return true;
    }
    return false;
}
void NoteEditor::showClip() {
    fitPending_ = true;
    resized();
}
void NoteEditor::resized() {
    const auto area = notesArea();
    if (fitPending_ && area.getWidth() > 0 && area.getHeight() > 0) {
        if (const auto *clip = panel.details()) {
            fitPending_ = false;
            const auto window = ClipWindow::of(*clip);
            const auto length = std::max<std::int64_t>(textproj::kPPQ, window.length);
            time.fit(window.pos, window.pos + length + length / 24, area.getWidth(), false);
            // Live opens on the notes; an empty clip opens around middle C.
            folded_ = false;
            keys.unfold(area.getHeight());
            keys.keyHeight = KeyTracks::kDefaultHeight;
            const auto range = keyRange(clip->notes);
            keys.show(range ? range->low : 60, range ? range->high : 71, area.getHeight());
        }
    }
    keys.clampScroll(area.getHeight());
}
void NoteEditor::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    const auto area = notesArea();
    if (e.mods.isAltDown())
        // Alt/Option and the wheel zoom the key tracks (manual 10.2).
        keys.zoom(keys.keyHeight + (w.deltaY > 0 ? 2 : -2), e.y - area.getY(), area.getHeight());
    else if (!timeWheel(time, e, w, area.getX()))
        keys.scrollBy(static_cast<int>(std::lround(-w.deltaY * 180.f)), area.getHeight());
    repaint();
}
std::vector<juce::Rectangle<int>> NoteEditor::textAreas() const {
    auto out = labels_;
    out.push_back(rulerArea());
    return out;
}
void NoteEditor::paint(juce::Graphics &g) {
    labels_.clear();
    g.fillAll(look::lane);
    const auto ruler = rulerArea(), area = notesArea(), keyboard = keysArea(), velocity = velocityArea();
    g.setColour(look::band);
    g.fillRect(0, 0, kKeysWidth, kRulerHeight);
    const auto *clip = panel.details();
    if (clip == nullptr || time.meters == nullptr) {
        g.fillRect(ruler);
        return;
    }
    const auto &meters = *time.meters;
    paintRuler(g, ruler, time, meters);
    if (const auto brace = loopBrace()) {
        // The loop brace (manual 8.2.2): over the loop's window, lit while the clip loops.
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(ruler);
        g.setColour(clip->loopEnabled ? look::textOnBand : look::textOnBand.withAlpha(0.35f));
        g.fillRect(brace->getStart(), ruler.getY() + 1, std::max(1, brace->getLength()), 3);
        g.fillRect(brace->getStart(), ruler.getY() + 1, 2, 6);
        g.fillRect(brace->getEnd() - 2, ruler.getY() + 1, 2, 6);
    }
    const auto window = ClipWindow::of(*clip);
    const int rows = keys.rowCount();
    const int firstRow = std::max(0, keys.scrollY / std::max(1, keys.keyHeight));
    {
        // The key tracks: a black key's track is the darker (manual 10.1), an octave ends in a line.
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(area);
        for (int row = firstRow; row < rows; ++row) {
            const int y = area.getY() + row * keys.keyHeight - keys.scrollY;
            if (y >= area.getBottom())
                break;
            const int key = *keys.keyOfRow(row);
            g.setColour(blackKey(key) ? look::lane : look::laneAlt);
            g.fillRect(area.getX(), y, area.getWidth(), keys.keyHeight);
            if (key % 12 == 0 && !keys.isFolded()) {
                g.setColour(look::lane.darker(0.25f));
                g.fillRect(area.getX(), y + keys.keyHeight - 1, area.getWidth(), 1);
            }
        }
        paintGrid(g, area, time, meters);
        shadeOutside(g, area, time, window);
        // The notes: the clip's colour, lighter as the velocity falls; a deactivated note is grey
        // (manual 10.5.7).
        const auto base = panel.colour();
        std::vector<engine::ClipNote> drawn;
        for (const auto &stored : clip->notes) {
            // A drag that would copy shows the originals in their places and the copies under the pointer.
            if (copying_ && drag_ == Drag::Move && isSelected(stored.id))
                drawn.push_back(stored);
            drawn.push_back(shown(stored));
        }
        for (const auto &note : drawn) {
            const auto box = boxOf(note);
            if (!box || box->x > static_cast<double>(area.getRight()) || box->x + box->width < area.getX() ||
                box->y >= area.getBottom() || box->y + box->height <= area.getY())
                continue;
            const float light = 0.55f * static_cast<float>(127 - std::clamp<int>(note.velocity, 1, 127)) / 126.f;
            const auto fill = note.active ? base.interpolatedWith(juce::Colours::white, light)
                                          : juce::Colour(0xff9a9a9a);
            const juce::Rectangle<float> r(static_cast<float>(box->x), static_cast<float>(box->y),
                                           static_cast<float>(box->width), static_cast<float>(box->height));
            g.setColour(fill);
            g.fillRect(r);
            g.setColour(look::outline);
            g.drawRect(r, 1.f);
            // A selected note: a light line inside its edge.
            if (isSelected(note.id) && r.getWidth() > 4.f && r.getHeight() > 4.f) {
                g.setColour(juce::Colours::white);
                g.drawRect(r.reduced(1.f), 1.f);
            }
        }
        if (drag_ == Drag::Draw) {
            // The stroke held: the notes it would draw.
            const auto step = gridTicks();
            for (const auto start : drawnSteps()) {
                engine::ClipNote ghost;
                ghost.key = static_cast<std::uint8_t>(drawKey_);
                ghost.startTicks = start;
                ghost.durationTicks = step;
                if (const auto box = boxOf(ghost)) {
                    const juce::Rectangle<float> r(static_cast<float>(box->x), static_cast<float>(box->y),
                                                   static_cast<float>(box->width), static_cast<float>(box->height));
                    g.setColour(base.withAlpha(0.7f));
                    g.fillRect(r);
                    g.setColour(look::outline);
                    g.drawRect(r, 1.f);
                }
            }
        }
        if (drag_ == Drag::Trim) {
            g.setColour(juce::Colours::white.withAlpha(0.8f));
            g.fillRect(to_.x, std::min(from_.y, to_.y), 1, std::max(1, std::abs(to_.y - from_.y)));
        }
        if (drag_ == Drag::Band) {
            const juce::Rectangle<int> band(from_, to_);
            g.setColour(juce::Colours::white.withAlpha(0.18f));
            g.fillRect(band);
            g.setColour(juce::Colours::white.withAlpha(0.7f));
            g.drawRect(band, 1);
        }
    }
    {
        // The note ruler: Live's piano, the Cs named (C3 is middle C).
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(keyboard);
        g.setColour(juce::Colour(0xffe6e6e6));
        g.fillRect(keyboard);
        g.setFont(look::font(std::min(10.f, static_cast<float>(keys.keyHeight)), false));
        for (int row = firstRow; row < rows; ++row) {
            const int y = keyboard.getY() + row * keys.keyHeight - keys.scrollY;
            if (y >= keyboard.getBottom())
                break;
            const int key = *keys.keyOfRow(row);
            if (blackKey(key)) {
                g.setColour(juce::Colour(0xff2a2a2a));
                g.fillRect(keyboard.getX(), y, keyboard.getWidth() * 3 / 5, keys.keyHeight);
            } else if (keys.isFolded() || key % 12 == 0 || key % 12 == 5) {
                // The lines between E and F, and between B and C, where two white keys meet.
                g.setColour(juce::Colour(0xff9a9a9a));
                g.fillRect(keyboard.getX(), y + keys.keyHeight - 1, keyboard.getWidth(), 1);
            }
            if ((key % 12 == 0 || keys.isFolded()) && keys.keyHeight >= 9) {
                const juce::Rectangle<int> name(keyboard.getRight() - 26, y, 24, keys.keyHeight);
                labels_.push_back(name);
                g.setColour(blackKey(key) ? juce::Colours::white : look::text);
                g.drawText(noteName(key), name, juce::Justification::centredRight);
            }
        }
        g.setColour(look::outline);
        g.fillRect(keyboard.getRight() - 1, keyboard.getY(), 1, keyboard.getHeight());
    }
    if (!velocity.isEmpty()) {
        // The Velocity lane (manual 10.6): a marker for each note, as high as its velocity.
        juce::Graphics::ScopedSaveState state(g);
        g.setColour(look::band);
        g.fillRect(0, velocity.getY(), getWidth(), 1);
        g.setColour(look::panel);
        g.fillRect(0, velocity.getY() + 1, kKeysWidth, velocity.getHeight() - 1);
        const auto lane = velocity.withTrimmedTop(1);
        g.reduceClipRegion(lane);
        g.setColour(look::lane.darker(0.08f));
        g.fillRect(lane);
        paintGrid(g, lane, time, meters);
        const double top = lane.getY() + 5, height = lane.getHeight() - 10;
        for (const auto &stored : clip->notes) {
            const auto note = shown(stored);
            const float x = static_cast<float>(lane.getX() + time.x(window.toArrangement(note.startTicks)));
            if (x < static_cast<float>(lane.getX()) - 4.f || x > static_cast<float>(lane.getRight()) + 4.f)
                continue;
            const float y = static_cast<float>(velocityY(note.velocity, top, height));
            g.setColour(note.active ? look::text : juce::Colour(0xff6a6a6a));
            g.fillRect(x, y, 1.f, static_cast<float>(lane.getBottom()) - y);
            g.setColour(note.active ? panel.colour() : juce::Colour(0xff9a9a9a));
            g.fillEllipse(x - 2.5f, y - 2.5f, 6.f, 6.f);
            g.setColour(isSelected(note.id) ? juce::Colours::white : look::outline);
            g.drawEllipse(x - 2.5f, y - 2.5f, 6.f, 6.f, 1.f);
        }
    }
    if (!velocity.isEmpty()) {
        const juce::Rectangle<int> name(2, velocity.getY() + 3, kKeysWidth - 6, 12);
        labels_.push_back(name);
        g.setColour(look::text);
        g.setFont(look::font(10.f));
        g.drawText("Velocity", name, juce::Justification::centredLeft);
        // The line tool's chooser (ADR-0241 d2).
        const auto shape = shapeBox();
        look::box(g, shape.toFloat(), shape_ == LaneShape::Pointer ? look::button : look::accent);
        labels_.push_back(shape);
        g.setColour(look::text);
        g.setFont(look::font(9.5f));
        g.drawText(kLaneShapeNames[static_cast<std::size_t>(shape_)], shape.reduced(2, 0), juce::Justification::centred);
        for (const auto handle : {LaneHandle::TiltLeft, LaneHandle::TiltRight, LaneHandle::Scale, LaneHandle::Compress})
            if (const auto box = handleBox(handle); !box.isEmpty()) {
                g.setColour(juce::Colours::white);
                g.fillRect(box);
                g.setColour(look::outline);
                g.drawRect(box, 1);
            }
        if (drag_ == Drag::Shape && shape_ != LaneShape::Paint) {
            g.setColour(juce::Colours::white.withAlpha(0.6f));
            g.drawLine(static_cast<float>(from_.x), static_cast<float>(from_.y), static_cast<float>(to_.x),
                       static_cast<float>(to_.y), 1.f);
        }
    }
}

// ---------------------------------------------------------------------------------------------
SampleEditor::SampleEditor(ClipViewPanel &p) : panel(p) {
    setTitle("Sample Editor");
    setOpaque(true);
    addChildComponent(playhead);
}
void SampleEditor::frame() {
    const auto area = waveArea();
    const int x = static_cast<int>(std::clamp(time.x(panel.root.timelineTick()), -1e7, 1e7));
    playhead.setVisible(panel.details() != nullptr && panel.root.playing() && x >= 0 && x < area.getWidth());
    playhead.setBounds(area.getX() + x, area.getY(), 1, area.getHeight());
}
juce::Rectangle<int> SampleEditor::waveArea() const noexcept {
    return {0, kRulerHeight, getWidth(), std::max(0, getHeight() - kRulerHeight)};
}
void SampleEditor::showClip() {
    fitPending_ = true;
    resized();
}
void SampleEditor::resized() {
    if (!fitPending_ || getWidth() <= 0)
        return;
    if (const auto *clip = panel.details()) {
        fitPending_ = false;
        const auto window = ClipWindow::of(*clip);
        const auto length = std::max<std::int64_t>(textproj::kPPQ, window.length);
        time.fit(window.pos, window.pos + length + length / 24, getWidth(), false);
    }
}
void SampleEditor::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
    if (timeWheel(time, e, w, 0))
        repaint();
}
void SampleEditor::paint(juce::Graphics &g) {
    g.fillAll(look::laneAlt);
    const juce::Rectangle<int> ruler(0, 0, getWidth(), kRulerHeight);
    const auto *clip = panel.details();
    if (clip == nullptr || time.meters == nullptr) {
        g.setColour(look::band);
        g.fillRect(ruler);
        return;
    }
    const auto &meters = *time.meters;
    paintRuler(g, ruler, time, meters);
    const auto area = waveArea();
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(area);
    paintGrid(g, area, time, meters);
    const auto window = ClipWindow::of(*clip);
    shadeOutside(g, area, time, window);
    // The clip's audio inside its source window, a band for each channel, as the arrangement
    // draws it (N40): the peaks land from their worker and the next frame draws them.
    const auto *node = panel.node();
    auto *waveforms = panel.root.arrangement.waveforms;
    if (node == nullptr || !node->audio || waveforms == nullptr)
        return;
    const auto peaks = waveforms->peaks(*node->audio);
    if (!peaks || peaks->channels == 0 || peaks->sampleRate == 0)
        return;
    const auto *snapshot = panel.root.reader().get();
    const ClipSourceMap source(*node, snapshot != nullptr ? snapshot->tempo.get() : nullptr,
                               static_cast<double>(peaks->sampleRate));
    const float top = static_cast<float>(area.getY()) + 4.f, bottom = static_cast<float>(area.getBottom()) - 4.f;
    const float band = (bottom - top) / static_cast<float>(peaks->channels);
    const int from = std::max(0, static_cast<int>(std::floor(time.x(window.pos)))),
              to = std::min(getWidth(), static_cast<int>(std::ceil(time.x(window.end()))));
    g.setColour(juce::Colours::black.withAlpha(node->muted ? 0.35f : 0.7f));
    for (int x = from; x < to; ++x) {
        const auto f0 = source.frameAt(time.tick(x)), f1 = source.frameAt(time.tick(x + 1));
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

// ---------------------------------------------------------------------------------------------
ClipViewPanel::ClipViewPanel(AdiRootComponent &r) : root(r), notes(*this), sample(*this) {
    setTitle("Clip View");
    setOpaque(true);
    addChildComponent(notes);
    addChildComponent(sample);
    // One editor for whichever field is typed into: Enter keeps the value, Esc and a click
    // elsewhere leave the field as it was.
    addChildComponent(editor_);
    editor_.setFont(look::font(11.f));
    editor_.setJustification(juce::Justification::centredRight);
    editor_.setIndents(2, 0);
    editor_.setBorder({});
    editor_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::white);
    editor_.setColour(juce::TextEditor::textColourId, look::text);
    editor_.setColour(juce::TextEditor::outlineColourId, look::outline);
    editor_.setColour(juce::TextEditor::focusedOutlineColourId, look::outline);
    editor_.setTitle("Clip field");
    editor_.onReturnKey = [this] {
        const auto field = editing_;
        const auto text = editor_.getText();
        closeEditor();
        if (field != Field::None)
            typeField(field, text);
    };
    editor_.onEscapeKey = [this] { closeEditor(); };
    editor_.onFocusLost = [this] { closeEditor(); };
}
juce::Rectangle<int> ClipViewPanel::titleArea() const noexcept { return {0, 0, kPanelWidth, kTitleHeight}; }
juce::Rectangle<int> ClipViewPanel::activatorArea() const noexcept { return {4, 3, 11, 11}; }
juce::Rectangle<int> ClipViewPanel::mainArea() const noexcept {
    return {0, kTitleHeight, kPanelWidth, std::max(0, getHeight() - kTitleHeight)};
}
juce::Rectangle<int> ClipViewPanel::headerArea() const noexcept {
    return {panelWidth() + 1, 0, std::max(0, getWidth() - panelWidth() - 1), kHeaderHeight};
}
juce::Rectangle<int> ClipViewPanel::foldBox() const noexcept {
    const auto header = headerArea();
    return midi() ? juce::Rectangle<int>(header.getX() + 6, header.getY() + 2, 36, 13) : juce::Rectangle<int>();
}
juce::Rectangle<int> ClipViewPanel::drawBox() const noexcept {
    const auto fold = foldBox();
    return fold.isEmpty() ? fold : fold.withX(fold.getRight() + 4);
}
juce::Rectangle<int> ClipViewPanel::trimBox() const noexcept {
    const auto draw = drawBox();
    return draw.isEmpty() ? draw : draw.withX(draw.getRight() + 4);
}
juce::Rectangle<int> ClipViewPanel::editorArea() const noexcept {
    return {panelWidth() + 1, kHeaderHeight, std::max(0, getWidth() - panelWidth() - 1),
            std::max(0, getHeight() - kHeaderHeight)};
}
void ClipViewPanel::resized() {
    notes.setBounds(editorArea());
    sample.setBounds(editorArea());
}
void ClipViewPanel::read(const SnapshotReader &reader) {
    details_.reset();
    problem_.clear();
    if (clip_ == 0)
        return;
    auto result = engine::readClipDetails(root.editStore(), clip_);
    if (!result.clip) {
        // The clip is gone (deleted, or undone away): the view shows none.
        clip_ = 0;
        return;
    }
    details_ = std::move(*result.clip);
    if (!details_->notesProblem.empty())
        problem_ = "This clip's notes cannot be shown: " + details_->notesProblem;
    else if (!details_->propertiesProblem.empty())
        problem_ = "This clip's properties cannot be shown: " + details_->propertiesProblem;
    // The clip's own signature, else the song's where the clip starts (manual 8.2.4).
    meter_ = textproj::Meter{};
    if (details_->signature.local) {
        meter_.numerator = static_cast<std::uint16_t>(std::clamp<std::int64_t>(details_->signature.numerator, 1, 255));
        meter_.denominator =
            static_cast<std::uint16_t>(std::clamp<std::int64_t>(details_->signature.denominator, 1, 128));
    } else {
        const auto &song = meterAt(details_->posTicks.value_or(0), reader.meters());
        meter_.numerator = song.numerator;
        meter_.denominator = song.denominator;
    }
    clipMeters_.assign(1, meter_);
}
void ClipViewPanel::reread() {
    read(root.reader());
    notes.prune();
    repaint();
}
void ClipViewPanel::frame(const SnapshotReader &reader) {
    // The selected clip; with none selected, the one already shown stays, as Live's does.
    const auto selected = root.arrangement.geometry.selection.clip;
    const auto wanted = selected != 0 ? selected : clip_;
    const auto generation = root.generation();
    // Asked once for each clip and each generation: a selected clip that is gone is not read again
    // until the project moves (an Undo may bring it back).
    const bool other = wanted != wanted_ || !read_;
    if (other || generation != generation_) {
        wanted_ = wanted;
        clip_ = wanted;
        generation_ = generation;
        read_ = true;
        read(reader);
        notes.setVisible(midi());
        sample.setVisible(audio());
        notes.prune();
        resized();
        if (other) {
            closeEditor();
            notes.select({});
            notes.showClip();
            sample.showClip();
        }
        repaint();
    }
    notes.frame();
    sample.frame();
    // The frame's own snapshot: the clip's node and its track's colour, and the song's meters for
    // the rulers. They live until the next frame.
    notes.time.meters = &reader.meters();
    sample.time.meters = &reader.meters();
    node_ = nullptr;
    colour_ = look::trackColour(0);
    if (details_) {
        const auto rows = root.arrangement.geometry.rows(reader);
        if (const auto at = rows.find(details_->trackId); at < rows.size()) {
            colour_ = details_->color ? look::paletteColour(*details_->color)
                                      : root.arrangement.colourFor(rows[at].position);
            for (const auto &c : rows[at].track->clips)
                if (c->id == clip_)
                    node_ = c.get();
        }
    }
}
ClipViewPanel::Readings ClipViewPanel::readings() const {
    Readings out;
    if (!details_)
        return out;
    const auto &d = *details_;
    out.start = positionText(d.startMarkerTicks, clipMeters_);
    out.end = positionText(endMarkerOf(d), clipMeters_);
    out.loop = d.loopEnabled;
    out.position = positionText(d.loopStartTicks, clipMeters_);
    out.length = lengthText(loopLengthOf(d), meter_);
    out.signature = juce::String(meter_.numerator) + " / " + juce::String(meter_.denominator);
    // The scale's root and its name, the clip's own or the one it inherits (manual 8.2.5).
    // A scale of the catalogue reads by its name; one from elsewhere, by its id.
    const auto *known = findScale(d.scale.id);
    auto scaleName = juce::String(d.scale.id);
    out.scale = juce::String(pitchClassName(static_cast<int>(d.scale.root))) + " " +
                (known != nullptr ? juce::String(known->name)
                                  : scaleName.substring(0, 1).toUpperCase() + scaleName.substring(1));
    // The Pitch and Time panel: the lowest and highest key of the notes it would act on, and the
    // transposition, which is nothing until its field is dragged or typed into.
    out.transpose = "0 st";
    {
        std::vector<engine::ClipNote> acted;
        for (const auto &n : d.notes)
            if (notes.selected().empty() || notes.isSelected(n.id))
                acted.push_back(n);
        if (const auto keys = keyRange(acted))
            out.range = juce::String(noteName(keys->low)) + " - " + juce::String(noteName(keys->high));
    }
    // While a field is dragged it reads where the drag would put it.
    if (dragged_ && pressed_ != Field::None) {
        if (pressed_ == Field::Transpose) {
            out.transpose = (dragSteps_ > 0 ? juce::String("+") : juce::String()) + juce::String(dragSteps_) + " st";
        } else if (pressed_ == Field::Signature) {
            const auto s = draggedSignature();
            out.signature = juce::String(s.first) + " / " + juce::String(s.second);
        } else if (pressed_ == Field::Length) {
            out.length = lengthText(draggedTicks(), meter_);
        } else {
            auto &shown = pressed_ == Field::Start ? out.start : pressed_ == Field::End ? out.end : out.position;
            shown = positionText(draggedTicks(), clipMeters_);
        }
    }
    const auto &time = midi() ? notes.time : sample.time;
    out.grid = juce::String(gridLabel(time.gridQuarters(), time.grid.triplet));
    return out;
}
// ---- The Main panel's fields (manual 8.2; worklist G2) ----------------------------------------
namespace {
constexpr int kFieldLabel = 15, kFieldBox = 16;
constexpr int kDenominators[] = {1, 2, 4, 8, 16};
}
juce::Rectangle<int> ClipViewPanel::fieldBox(Field field) const noexcept {
    const auto main = mainArea().reduced(6, 5);
    const int half = (main.getWidth() - 6) / 2;
    const int row = field == Field::Start || field == Field::End          ? 0
                    : field == Field::Position || field == Field::Length ? 58
                                                                         : 95;
    const int column = field == Field::End || field == Field::Length || field == Field::Scale ? 1 : 0;
    if (field == Field::None)
        return {};
    if (field == Field::Transpose) {
        const auto fit = toolBox(Tool::FitToScale);
        return fit.isEmpty() ? fit : fit.withX(toolsArea().getX() + 6);
    }
    return {main.getX() + column * (half + 6), main.getY() + row + kFieldLabel, half, kFieldBox};
}
// ---- The Pitch and Time panel (manual 8.5; STEP-7-PLAN 7.7 step 4) -----------------------------
juce::Rectangle<int> ClipViewPanel::toolsArea() const noexcept {
    return midi() ? juce::Rectangle<int>(kPanelWidth + 1, 0, kToolsWidth - 1, getHeight()) : juce::Rectangle<int>();
}
juce::Rectangle<int> ClipViewPanel::rangeBox() const noexcept {
    const auto inner = toolsArea().withTrimmedTop(kTitleHeight).reduced(6, 5);
    return inner.isEmpty() ? inner : inner.withHeight(kFieldBox);
}
juce::Rectangle<int> ClipViewPanel::toolBox(Tool tool) const noexcept {
    const auto inner = toolsArea().withTrimmedTop(kTitleHeight).reduced(6, 5);
    if (inner.isEmpty())
        return {};
    const int half = (inner.getWidth() - 6) / 2;
    // Two columns under the range: Transpose's field has the first place.
    int row = 1, column = 1;
    switch (tool) {
    case Tool::FitToScale:
        break;
    case Tool::Invert:
        row = 2, column = 0;
        break;
    case Tool::Reverse:
        row = 2;
        break;
    case Tool::Double:
        row = 3, column = 0;
        break;
    case Tool::Halve:
        row = 3;
        break;
    case Tool::Legato:
        row = 4, column = 0;
        break;
    case Tool::FixedLength:
        row = 4;
        break;
    case Tool::OverlapsMono:
        row = 5, column = 0;
        break;
    case Tool::OverlapsPoly:
        row = 5;
        break;
    case Tool::Chord:
        row = 6;
        break;
    case Tool::InvertUp:
        row = 7, column = 0;
        break;
    case Tool::InvertDown:
        row = 7;
        break;
    case Tool::Drop2:
        row = 8, column = 0;
        break;
    case Tool::Drop3:
        row = 8;
        break;
    }
    return {inner.getX() + column * (half + 6), inner.getY() + row * 22, half, kFieldBox};
}
const char *ClipViewPanel::toolName(Tool tool) noexcept {
    switch (tool) {
    case Tool::FitToScale:
        return "Fit to Scale";
    case Tool::Invert:
        return "Invert";
    case Tool::Reverse:
        return "Reverse";
    case Tool::Double:
        return "x2";
    case Tool::Halve:
        return "/2";
    case Tool::Legato:
        return "Legato";
    case Tool::FixedLength:
        return "Fixed Length";
    case Tool::OverlapsMono:
        return "Overlap Mono";
    case Tool::OverlapsPoly:
        return "Overlap Poly";
    case Tool::Chord:
        return "Add Chord";
    case Tool::InvertUp:
        return "Inversion Up";
    case Tool::InvertDown:
        return "Inversion Dn";
    case Tool::Drop2:
        return "Drop 2";
    case Tool::Drop3:
        return "Drop 3";
    }
    return "";
}
juce::Rectangle<int> ClipViewPanel::chordBox() const noexcept {
    const auto add = toolBox(Tool::Chord);
    return add.isEmpty() ? add : add.withX(toolsArea().getX() + 6);
}
juce::PopupMenu ClipViewPanel::chordMenu() const {
    juce::PopupMenu menu;
    int id = 1;
    for (const auto &type : kChordTypes) {
        menu.addItem(id, type.name, true, static_cast<std::size_t>(id - 1) == chord_);
        ++id;
    }
    return menu;
}
bool ClipViewPanel::chooseChord(int item) {
    if (item < 1 || item > static_cast<int>(kChordTypes.size()))
        return false;
    chord_ = static_cast<std::size_t>(item - 1);
    repaint();
    return true;
}
bool ClipViewPanel::runTool(Tool tool) {
    if (!details_ || !midi())
        return false;
    const auto &d = *details_;
    const auto &ids = notes.selected();
    switch (tool) {
    case Tool::FitToScale:
        return notes.apply(fitNotesToScale(d, ids, static_cast<int>(d.scale.root), d.scale.mask));
    case Tool::Invert:
        return notes.apply(invertNotes(d, ids));
    case Tool::Reverse:
        return notes.apply(reverseNotes(d, ids));
    case Tool::Double:
        return notes.apply(stretchNotes(d, ids, 2, 1));
    case Tool::Halve:
        return notes.apply(stretchNotes(d, ids, 1, 2));
    case Tool::Legato:
        return notes.apply(legatoNotes(d, ids));
    case Tool::FixedLength:
        return notes.apply(fixedLengths(d, ids, notes.gridTicks()));
    case Tool::OverlapsMono:
        return notes.apply(deleteOverlaps(d, ids, false));
    case Tool::OverlapsPoly:
        return notes.apply(deleteOverlaps(d, ids, true));
    case Tool::Chord:
        return notes.apply(makeChords(d, ids, chordType()));
    case Tool::InvertUp:
        return notes.apply(invertChords(d, ids, true));
    case Tool::InvertDown:
        return notes.apply(invertChords(d, ids, false));
    case Tool::Drop2:
        return notes.apply(dropVoicing(d, ids, {2}, "Drop 2"));
    case Tool::Drop3:
        return notes.apply(dropVoicing(d, ids, {3}, "Drop 3"));
    }
    return false;
}
bool ClipViewPanel::transpose(int semitones) {
    return details_ && midi() && notes.apply(transposeNotes(*details_, notes.selected(), semitones));
}
juce::PopupMenu ClipViewPanel::scaleMenu(bool roots) const {
    // Live's two choosers (manual 8.2.5): the root note, and the scale's name. Item ids are from 1.
    juce::PopupMenu menu;
    if (!details_)
        return menu;
    if (roots) {
        for (int note = 0; note < 12; ++note)
            menu.addItem(note + 1, pitchClassName(note), true, details_->scale.root == note);
    } else {
        int id = 1;
        for (const auto &scale : kScales)
            menu.addItem(id++, scale.name, true, details_->scale.id == scale.id);
    }
    return menu;
}
bool ClipViewPanel::chooseScale(bool roots, int item) {
    if (!details_ || item < 1)
        return false;
    const auto &d = *details_;
    if (roots)
        return item <= 12 && commit(setClipScale(d, item - 1, d.scale.id, d.scale.mask));
    if (item > static_cast<int>(kScales.size()))
        return false;
    const auto &scale = kScales[static_cast<std::size_t>(item - 1)];
    return commit(setClipScale(d, d.scale.root, scale.id, scale.mask));
}
void ClipViewPanel::showScaleMenu(bool roots) {
    juce::Component::SafePointer<ClipViewPanel> safe(this);
    scaleMenu(roots).showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(fieldBox(Field::Scale))),
        [safe, roots](int item) {
            if (safe != nullptr && item > 0)
                safe->chooseScale(roots, item);
        });
}
juce::Rectangle<int> ClipViewPanel::setBox(Field field) const noexcept {
    if (field != Field::Start && field != Field::End && field != Field::Position && field != Field::Length)
        return {};
    const auto box = fieldBox(field);
    return {box.getRight() - 24, box.getY() - kFieldLabel + 2, 24, 11};
}
juce::Rectangle<int> ClipViewPanel::loopBox() const noexcept {
    const auto main = mainArea().reduced(6, 5);
    return {main.getX(), main.getY() + 37, main.getWidth(), kFieldBox};
}
std::int64_t ClipViewPanel::fieldTicks(Field field) const noexcept {
    if (!details_)
        return 0;
    const auto &d = *details_;
    switch (field) {
    case Field::Start:
        return d.startMarkerTicks;
    case Field::End:
        return endMarkerOf(d);
    case Field::Position:
        return d.loopStartTicks;
    case Field::Length:
        return loopLengthOf(d);
    default:
        return 0;
    }
}
ClipViewPanel::Field ClipViewPanel::fieldAt(juce::Point<int> at) const noexcept {
    for (const auto field : {Field::Start, Field::End, Field::Position, Field::Length, Field::Signature, Field::Scale,
                             Field::Transpose})
        if (fieldBox(field).contains(at) && (midi() || (field != Field::Scale && field != Field::Transpose)))
            return field;
    return Field::None;
}
bool ClipViewPanel::commit(TrackEdit edit) {
    repaint();
    if (!edit.refusal.empty()) {
        root.say(std::move(edit.refusal));
        return false;
    }
    if (edit.ops.empty())
        return true;
    if (!root.submitEdits(std::move(edit.ops)))
        return false;
    reread();
    return true;
}
bool ClipViewPanel::setField(Field field, std::int64_t ticks) {
    if (!details_)
        return false;
    const auto &d = *details_;
    switch (field) {
    case Field::Start:
        return commit(setClipStart(d, ticks));
    case Field::End:
        return commit(setClipEnd(d, ticks));
    case Field::Position:
        return commit(setClipLoop(d, ticks, loopLengthOf(d)));
    case Field::Length:
        return commit(setClipLoop(d, d.loopStartTicks, ticks));
    default:
        return false;
    }
}
bool ClipViewPanel::typeField(Field field, const juce::String &typed) {
    if (!details_)
        return false;
    const auto text = typed.toStdString();
    if (field == Field::Transpose) {
        const auto trimmed = typed.trim().upToFirstOccurrenceOf(" ", false, false);
        if (trimmed.isEmpty() || !trimmed.containsOnly("+-0123456789") || trimmed.lastIndexOfAnyOf("+-") > 0 ||
            trimmed.length() > 4 || !trimmed.containsAnyOf("0123456789")) {
            root.say("Type a transposition in semitones: 12 or -7.");
            return false;
        }
        return transpose(trimmed.getIntValue());
    }
    if (field == Field::Scale)
        return false;
    if (field == Field::Signature) {
        const auto signature = parseSignature(text);
        if (!signature) {
            root.say("Type a time signature as 3/4.");
            return false;
        }
        return commit(setClipSignature(*details_, signature->first, signature->second));
    }
    if (field == Field::Length) {
        const auto length = parseBarsBeats(text);
        if (!length) {
            root.say("Type a length as bars, beats and sixteenths: 2.0.0.");
            return false;
        }
        return setField(field, ticksOf(*length, meter_));
    }
    // A position: bar, beat and sixteenth from 1, in the clip's own time.
    const auto position = parsePosition(text);
    const auto tick = position ? tickOf(*position, clipMeters_) : std::optional<std::int64_t>{};
    if (!tick) {
        root.say("Type a position as bar, beat and sixteenth: 1.1.1.");
        return false;
    }
    return setField(field, *tick);
}
bool ClipViewPanel::setFieldHere(Field field) {
    // Live's Set buttons take the playhead while the song plays (manual 8.2.1); stopped, ADI takes
    // the arrangement's insert marker. In the clip's own time. On the loop they also switch it on:
    // Set Position moves the loop's start there; Set Length moves its end there, or, when the loop
    // was off, keeps its length.
    if (!details_)
        return false;
    const auto &d = *details_;
    const auto at = root.playing() ? root.timelineTick() : root.arrangement.geometry.selection.insert;
    const auto here = ClipWindow::of(d).toSource(at);
    if (field == Field::Position)
        return commit(setClipLoop(d, here, loopLengthOf(d), true));
    if (field == Field::Length)
        return commit(setClipLoop(d, d.loopStartTicks, d.loopEnabled ? here - d.loopStartTicks : loopLengthOf(d), true));
    return setField(field, here);
}
bool ClipViewPanel::toggleLoop() { return details_ && commit(toggleClipLoop(*details_)); }
void ClipViewPanel::openEditor(Field field) {
    editing_ = field;
    const auto r = readings();
    const auto value = field == Field::Start       ? r.start
                       : field == Field::End       ? r.end
                       : field == Field::Position  ? r.position
                       : field == Field::Length    ? r.length
                       : field == Field::Transpose ? juce::String("0")
                                                   : r.signature;
    editor_.setBounds(fieldBox(field));
    editor_.setText(value.removeCharacters(" "), false);
    editor_.setVisible(true);
    editor_.selectAll();
    editor_.grabKeyboardFocus();
}
void ClipViewPanel::closeEditor() {
    editing_ = Field::None;
    editor_.setVisible(false);
}
void ClipViewPanel::mouseDown(const juce::MouseEvent &e) {
    pressed_ = Field::None;
    dragged_ = false;
    dragSteps_ = 0;
    if (!details_ || e.mods.isPopupMenu())
        return;
    const auto at = e.getPosition();
    // The Clip Activator (manual 8.1.1): one click, one undo step.
    if (activatorArea().expanded(2).contains(at)) {
        root.submitEdit("clip.setMute", {{"id", clip_}, {"muted", !details_->muted}},
                        details_->muted ? "Activate Clip" : "Deactivate Clip");
        return;
    }
    for (const auto field : {Field::Start, Field::End, Field::Position, Field::Length})
        if (setBox(field).contains(at)) {
            setFieldHere(field);
            return;
        }
    if (loopBox().contains(at)) {
        toggleLoop();
        return;
    }
    if (foldBox().contains(at)) {
        notes.setFold(!notes.folded());
        repaint();
        return;
    }
    if (drawBox().contains(at)) {
        notes.setDrawMode(!notes.drawMode());
        return;
    }
    if (trimBox().contains(at)) {
        notes.setTrimMode(!notes.trimMode());
        return;
    }
    for (const auto tool : kTools)
        if (toolBox(tool).contains(at)) {
            runTool(tool);
            return;
        }
    if (chordBox().contains(at)) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        chordMenu().showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(chordBox())),
            [safe](int item) {
                if (safe != nullptr && item > 0)
                    safe->chooseChord(item);
            });
        return;
    }
    // A field: a drag changes it, by bars, beats or sixteenths as the part of the field grabbed; a
    // click opens it for typing.
    pressed_ = fieldAt(at);
    if (pressed_ != Field::None) {
        const auto box = fieldBox(pressed_);
        pressAt_ = at;
        pressPart_ = pressed_ == Field::Signature ? (at.x < box.getCentreX() ? 0 : 1)
                                                  : std::clamp((at.x - box.getX()) * 3 / std::max(1, box.getWidth()), 0, 2);
    }
}
void ClipViewPanel::mouseDrag(const juce::MouseEvent &e) {
    if (pressed_ == Field::None)
        return;
    if (pressed_ == Field::Scale)
        return;
    const int steps = (pressAt_.y - e.y) / 4;
    if (steps != 0)
        dragged_ = true;
    if (steps != dragSteps_) {
        dragSteps_ = steps;
        repaint();
    }
}
std::int64_t ClipViewPanel::draggedTicks() const noexcept {
    const std::int64_t beat = meter_.denominator == 0 ? 0 : textproj::kWhole / meter_.denominator;
    const std::int64_t unit = pressPart_ == 0 ? meter_.barTicks() : pressPart_ == 1 ? beat : textproj::kPPQ / 4;
    return fieldTicks(pressed_) + dragSteps_ * unit;
}
std::pair<std::int64_t, std::int64_t> ClipViewPanel::draggedSignature() const noexcept {
    std::int64_t numerator = meter_.numerator, denominator = meter_.denominator;
    if (pressPart_ == 0)
        numerator = std::clamp<std::int64_t>(numerator + dragSteps_, 1, 99);
    else {
        int at = 2;
        for (int i = 0; i < 5; ++i)
            if (kDenominators[i] == denominator)
                at = i;
        denominator = kDenominators[std::clamp(at + dragSteps_, 0, 4)];
    }
    return {numerator, denominator};
}
void ClipViewPanel::mouseUp(const juce::MouseEvent &) {
    // The drag's value, read before the state is cleared; one op, one undo step.
    const auto field = pressed_;
    const bool dragged = dragged_;
    const auto ticks = draggedTicks();
    const auto signature = draggedSignature();
    const int steps = dragSteps_;
    pressed_ = Field::None;
    dragged_ = false;
    dragSteps_ = 0;
    if (field == Field::None || !details_)
        return;
    if (field == Field::Scale)
        // The root's part of the field opens the root notes; the rest, the scales.
        showScaleMenu(pressPart_ == 0);
    else if (!dragged)
        openEditor(field);
    else if (field == Field::Transpose)
        transpose(steps);
    else if (field == Field::Signature)
        commit(setClipSignature(*details_, signature.first, signature.second));
    else
        setField(field, ticks);
}
std::vector<juce::Rectangle<int>> ClipViewPanel::textAreas() const {
    auto out = labels_;
    if (notes.isVisible())
        for (auto r : notes.textAreas())
            out.push_back(r + notes.getPosition());
    if (sample.isVisible())
        out.push_back(juce::Rectangle<int>(0, 0, sample.getWidth(), SampleEditor::kRulerHeight) + sample.getPosition());
    return out;
}
void ClipViewPanel::paint(juce::Graphics &g) {
    labels_.clear();
    g.fillAll(look::panel);
    auto text = [&](const juce::String &s, juce::Rectangle<int> r, juce::Justification j, juce::Colour c,
                    float height = 11.f, bool bold = false) {
        labels_.push_back(r);
        g.setColour(c);
        g.setFont(look::font(height, bold));
        g.drawText(s, r, j);
    };
    const auto title = titleArea(), header = headerArea();
    g.setColour(look::field);
    g.fillRect(header);
    g.setColour(look::band);
    g.fillRect(kPanelWidth, 0, 1, getHeight());
    if (midi())
        g.fillRect(panelWidth(), 0, 1, getHeight());
    if (!details_) {
        g.setColour(look::field);
        g.fillRect(title);
        g.setColour(look::lane);
        g.fillRect(editorArea());
        return;
    }
    const auto &d = *details_;
    // The title bar in the clip's colour: Live's round activator, lit while the clip plays, and
    // the clip's name.
    g.setColour(d.muted ? juce::Colour(0xff9a9a9a) : colour_);
    g.fillRect(title);
    const auto dot = activatorArea().toFloat();
    g.setColour(d.muted ? look::button : look::accent);
    g.fillEllipse(dot);
    g.setColour(look::text);
    g.drawEllipse(dot.reduced(0.5f), 1.f);
    text(juce::String(d.name), title.withTrimmedLeft(20).withTrimmedRight(4), juce::Justification::centredLeft,
         look::text, 12.f, true);

    // The Main panel (manual 8.2). Its fields read the clip; they act with the clip's own ops.
    const auto r = readings();
    auto field = [&](Field which, const juce::String &name, const juce::String &value) {
        const auto box = fieldBox(which);
        const auto set = setBox(which);
        text(name, {box.getX(), box.getY() - 15, box.getWidth() - (set.isEmpty() ? 0 : 26), 15},
             juce::Justification::centredLeft, look::text, 10.5f);
        // Live's Set button: a small pill beside the field's name.
        if (!set.isEmpty()) {
            g.setColour(look::button);
            g.fillRoundedRectangle(set.toFloat(), 5.f);
            g.setColour(look::outline);
            g.drawRoundedRectangle(set.toFloat().reduced(0.5f), 5.f, 1.f);
            text("Set", set, juce::Justification::centred, look::text, 9.f);
        }
        look::box(g, box.toFloat(), look::chooser);
        text(value, box.reduced(3, 0), juce::Justification::centredRight, look::text, 11.f);
    };
    field(Field::Start, "Start", r.start);
    field(Field::End, "End", r.end);
    const auto loop = loopBox();
    look::box(g, loop.toFloat(), r.loop ? look::accent : look::button);
    text("Loop", loop, juce::Justification::centred, look::text, 11.f);
    field(Field::Position, "Position", r.position);
    field(Field::Length, "Length", r.length);
    field(Field::Signature, "Signature", r.signature);
    if (midi())
        field(Field::Scale, "Scale", r.scale);

    if (const auto tools = toolsArea(); !tools.isEmpty()) {
        // The Pitch and Time panel (manual 8.5): its title, the range of the notes it acts on,
        // Transpose, and the utilities.
        g.setColour(look::field);
        g.fillRect(tools.withHeight(kTitleHeight));
        text("Pitch and Time", tools.withHeight(kTitleHeight).withTrimmedLeft(6), juce::Justification::centredLeft,
             look::text, 11.f, true);
        const auto range = rangeBox();
        look::box(g, range.toFloat(), look::display);
        text(r.range, range.reduced(4, 0), juce::Justification::centred, juce::Colours::white, 11.f);
        const auto semitones = fieldBox(Field::Transpose);
        look::box(g, semitones.toFloat(), look::chooser);
        text(r.transpose, semitones.reduced(3, 0), juce::Justification::centredRight, look::text, 11.f);
        for (const auto tool : kTools) {
            const auto box = toolBox(tool);
            look::box(g, box.toFloat(), look::button);
            text(toolName(tool), box.reduced(2, 0), juce::Justification::centred, look::text, 10.f);
        }
        // The chord type's chooser, beside Add Chord.
        const auto chord = chordBox();
        look::box(g, chord.toFloat(), look::chooser);
        text(chordType().name, chord.reduced(3, 0), juce::Justification::centredLeft, look::text, 10.f);
    }

    // The editor's header: its tab, and the editing grid at its right (manual 10.2.1).
    if (const auto fold = foldBox(); !fold.isEmpty()) {
        look::box(g, fold.toFloat(), notes.folded() ? look::accent : look::button);
        text("Fold", fold, juce::Justification::centred, look::text, 10.f);
        const auto draw = drawBox();
        look::box(g, draw.toFloat(), notes.drawMode() ? look::accent : look::button);
        text("Draw", draw, juce::Justification::centred, look::text, 10.f);
        const auto trim = trimBox();
        look::box(g, trim.toFloat(), notes.trimMode() ? look::accent : look::button);
        text("Trim", trim, juce::Justification::centred, look::text, 10.f);
    }
    text(midi() ? "Notes" : audio() ? "Sample" : "", header.withTrimmedLeft(midi() ? 132 : 8).withWidth(60),
         juce::Justification::centredLeft, look::text, 11.5f, true);
    text(r.grid, header.withTrimmedRight(8).removeFromRight(70), juce::Justification::centredRight, look::text, 11.f);
    if (!problem_.empty())
        text(juce::String(problem_), header.withTrimmedLeft(76).withTrimmedRight(84), juce::Justification::centredLeft,
             look::text, 11.f);
    if (!midi() && !audio()) {
        g.setColour(look::lane);
        g.fillRect(editorArea());
    }
}

// ---------------------------------------------------------------------------------------------
ClipViewResizer::ClipViewResizer(AdiRootComponent &r) : root(r) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    setTitle("Clip View height");
}
void ClipViewResizer::mouseDown(const juce::MouseEvent &) {
    start = root.clipArea().getHeight();
    closing = false;
}
void ClipViewResizer::mouseDrag(const juce::MouseEvent &e) {
    const int wanted = start - e.getDistanceFromDragStartY();
    // Dragged to the foot of its space, the view closes when the mouse is let go.
    closing = wanted < 40;
    root.resizeClipView(wanted);
}
void ClipViewResizer::mouseUp(const juce::MouseEvent &) {
    if (closing && root.state().clipView)
        root.command(AppCommands::ToggleClipView);
    else
        root.persist();
    closing = false;
}
} // namespace adi::ui
