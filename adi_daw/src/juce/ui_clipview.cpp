// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_clipview.hpp"
#include "ui_shell.hpp"
#include "adi/ui/loop_edits.hpp"
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
    if (panel.details() != nullptr && panel.root.playing() && panel.root.state().follow && !panel.root.arrangement.followPaused &&
        area.getWidth() > 0) {
        // Follow (manual 8.7, 10.2.2): a page at a time, as the playhead leaves the editor.
        const auto tick = panel.root.timelineTick();
        if (const double at = time.x(tick); at < 0 || at >= area.getWidth()) {
            time.left = std::max(0., static_cast<double>(tick) / textproj::kPPQ);
            repaint();
        }
    }
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
bool NoteEditor::scaleOn() const noexcept {
    const auto *clip = panel.details();
    return clip != nullptr && clip->scale.local;
}
void NoteEditor::setScaleFold(bool fold) {
    scaleFolded_ = fold && scaleOn();
    refold();
}
void NoteEditor::refold() {
    if (folded_) {
        setFold(true);
        return;
    }
    const auto *clip = panel.details();
    const int height = notesArea().getHeight();
    const bool was = keys.isFolded();
    if (scaleFolded_ && scaleOn()) {
        // The scale's key tracks, and any that hold notes (manual 10.6).
        bool used[kNoteKeys] = {};
        for (const auto &n : clip->notes)
            if (n.key < kNoteKeys)
                used[n.key] = true;
        for (const auto &layer : panel.others())
            for (const auto &n : layer.details.notes)
                if (n.key < kNoteKeys)
                    used[n.key] = true;
        const int tonic = static_cast<int>(clip->scale.root);
        const auto mask = clip->scale.mask;
        keys.foldWhere([&](int key) { return used[key] || inScale(key, tonic, mask); }, height);
        if (!was) {
            // It opens on the notes, or around middle C.
            int low = kNoteKeys, high = -1;
            for (int key = 0; key < kNoteKeys; ++key)
                if (used[key]) {
                    low = std::min(low, key);
                    high = std::max(high, key);
                }
            if (high < 0)
                low = nearestInScale(60, tonic, mask), high = nearestInScale(71, tonic, mask);
            keys.show(low, high, height);
        }
    } else {
        keys.unfold(height);
        if (was) {
            auto every = clip != nullptr ? clip->notes : std::vector<engine::ClipNote>{};
            for (const auto &layer : panel.others())
                every.insert(every.end(), layer.details.notes.begin(), layer.details.notes.end());
            const auto range = keyRange(every);
            keys.show(range ? range->low : 60, range ? range->high : 71, height);
        }
    }
    repaint();
}
void NoteEditor::setFold(bool fold) {
    const auto *clip = panel.details();
    const int height = notesArea().getHeight();
    auto every = clip != nullptr ? clip->notes : std::vector<engine::ClipNote>{};
    for (const auto &layer : panel.others())
        every.insert(every.end(), layer.details.notes.begin(), layer.details.notes.end());
    if (fold && clip != nullptr && !every.empty()) {
        keys.fold(every, height);
        folded_ = true;
    } else {
        const bool was = folded_;
        folded_ = fold && clip != nullptr;
        if (!folded_ && scaleFolded_ && scaleOn()) {
            refold();
            return;
        }
        keys.unfold(height);
        // Unfolded, the notes come back into view.
        if (was && clip != nullptr)
            if (const auto range = keyRange(every))
                keys.show(range->low, range->high, height);
    }
    repaint();
}
void NoteEditor::zoomToNotes() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    if (const auto span = timeSelection(); span && !anySelected()) {
        time.fit(span->first, span->second + std::max<std::int64_t>(1, (span->second - span->first) / 24), notesArea().getWidth());
        repaint();
        return;
    }
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
bool NoteEditor::quantize() {
    const auto grid = panel.quantizeGrid();
    return commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) {
        // The grid's lines stand in the song; in the clip's own time one of them is the song's start.
        return quantizeNotes(of, ids, grid, ClipWindow::of(of).toSource(0), panel.quantizeSettings());
    }));
}
bool NoteEditor::splitOrChop() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    const auto grid = gridTicks();
    if (anySelected())
        return commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) {
            return chopNotes(of, ids, grid, ClipWindow::of(of).toSource(0));
        }));
    const auto window = ClipWindow::of(*clip);
    if (const auto span = timeSelection())
        return commit(splitNotesAtEach(*clip, {window.toSource(span->first), window.toSource(span->second)}));
    if (!insert_)
        return false;
    return commit(splitNotesAt(*clip, window.toSource(*insert_)));
}
bool NoteEditor::join() {
    if (!anySelected())
        return false;
    return commit(across([](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return joinNotes(of, ids); }));
}
bool NoteEditor::copySelected() {
    const auto *clip = panel.details();
    if (clip == nullptr || !anySelected())
        return false;
    // Each note with its place in the song, so notes of several clips keep their places to each
    // other; then counted from the earliest.
    std::vector<engine::ClipNote> copied;
    std::vector<std::int64_t> from;
    const auto take = [&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) {
        const auto window = ClipWindow::of(of);
        for (const auto &n : of.notes)
            if (std::find(ids.begin(), ids.end(), n.id) != ids.end()) {
                copied.push_back(n);
                copied.back().startTicks = window.toArrangement(n.startTicks);
                from.push_back(of.id);
            }
    };
    take(*clip, selected_);
    if (!panel.focus())
        for (const auto &layer : panel.others())
            take(layer.details, selectedIn(layer.details.id));
    if (copied.empty())
        return false;
    auto first = copied.front().startTicks;
    for (const auto &n : copied)
        first = std::min(first, n.startTicks);
    for (auto &n : copied)
        n.startTicks -= first;
    panel.noteClipboard = std::move(copied);
    panel.noteClipboardClips = std::move(from);
    panel.noteClipboardShown = panel.shownClips();
    panel.noteClipboardEdited = clip->id;
    return true;
}
bool NoteEditor::cutSelected() {
    if (!copySelected())
        return false;
    auto cut = across([](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return deleteNotes(of, ids); });
    for (auto &o : cut.ops)
        o.label = cut.ops.size() == 1 ? "Cut Note" : "Cut Notes";
    return commit(std::move(cut));
}
bool NoteEditor::paste() {
    const auto *clip = panel.details();
    if (clip == nullptr || panel.noteClipboard.empty())
        return false;
    // At the insert marker or the time selection's start, else at the clip's start; into the clip being edited.
    const auto window = ClipWindow::of(*clip);
    const auto span = timeSelection();
    const auto marker = span ? span->first : insert_.value_or(window.pos);
    // Notes cut or copied from several clips go back each into its own, while the same clips are
    // shown and the same one is being edited (manual 10.8); else all into the edited clip.
    const bool several = std::any_of(panel.noteClipboardClips.begin(), panel.noteClipboardClips.end(),
                                     [&](std::int64_t id) { return id != clip->id; });
    if (several && !panel.focus() && panel.noteClipboardClips.size() == panel.noteClipboard.size() &&
        panel.noteClipboardShown == panel.shownClips() && panel.noteClipboardEdited == clip->id) {
        const auto own = [&](std::int64_t id) {
            std::vector<engine::ClipNote> notes;
            for (std::size_t i = 0; i < panel.noteClipboard.size(); ++i)
                if (panel.noteClipboardClips[i] == id)
                    notes.push_back(panel.noteClipboard[i]);
            return notes;
        };
        auto all = pasteNotes(*clip, own(clip->id), window.toSource(marker));
        std::vector<std::pair<std::int64_t, std::vector<std::uint64_t>>> pasted;
        for (const auto &layer : panel.others()) {
            const auto notes = own(layer.details.id);
            if (notes.empty() || !all.refusal.empty())
                continue;
            auto part = pasteNotes(layer.details, notes, ClipWindow::of(layer.details).toSource(marker));
            if (!part.refusal.empty()) {
                all.refusal = std::move(part.refusal);
                break;
            }
            for (auto &o : part.ops)
                all.ops.push_back(std::move(o));
            pasted.emplace_back(layer.details.id, std::move(part.select));
        }
        for (auto &o : all.ops)
            o.label = "Paste Notes";
        if (!commit(std::move(all)))
            return false;
        // The pasted notes are the selection, in each clip.
        otherSelected_.clear();
        for (auto &entry : pasted)
            otherIds(entry.first) = std::move(entry.second);
        repaint();
        return true;
    }
    return commit(pasteNotes(*clip, panel.noteClipboard, window.toSource(marker)));
}
bool NoteEditor::duplicate() {
    const auto *clip = panel.details();
    if (clip == nullptr || selected_.empty())
        return false;
    return commit(copyNotes(*clip, selected_, spanOf(*clip, selected_), 0,
                            selected_.size() == 1 ? "Duplicate Note" : "Duplicate Notes"));
}
int NoteEditor::barsHeight() const noexcept {
    const auto count = panel.others().size();
    return count == 0 ? 0 : static_cast<int>(count + 1) * kLoopBarHeight + 2;
}
juce::Rectangle<int> NoteEditor::loopBar(std::size_t index) const noexcept {
    const auto *clip = index == 0 ? panel.details()
                                  : index <= panel.others().size() ? &panel.others()[index - 1].details : nullptr;
    if (clip == nullptr || barsHeight() == 0)
        return {};
    // Across the clip's place in the arrangement, as Live lays the bars out by time.
    const auto window = ClipWindow::of(*clip);
    const int from = kKeysWidth + static_cast<int>(std::clamp(time.x(window.pos), -1e6, 1e6));
    const int to = kKeysWidth + static_cast<int>(std::clamp(time.x(window.end()), -1e6, 1e6));
    return {from, static_cast<int>(index) * kLoopBarHeight + 1, std::max(2, to - from), kLoopBarHeight - 1};
}
std::int64_t NoteEditor::otherClipAt(juce::Point<int> at, std::uint64_t *note) const noexcept {
    const auto &others = panel.others();
    for (std::size_t i = 0; i < others.size(); ++i)
        if (loopBar(i + 1).contains(at))
            return others[i].details.id;
    if (!notesArea().contains(at))
        return 0;
    for (auto layer = others.rbegin(); layer != others.rend(); ++layer)
        for (auto n = layer->details.notes.rbegin(); n != layer->details.notes.rend(); ++n)
            if (const auto box = boxOf(*n, layer->details); box && box->contains(at.x, at.y)) {
                if (note != nullptr)
                    *note = n->id;
                return layer->details.id;
            }
    return 0;
}
juce::Rectangle<int> NoteEditor::rulerArea() const noexcept {
    return {kKeysWidth, barsHeight(), std::max(0, getWidth() - kKeysWidth), kRulerHeight};
}
juce::Rectangle<int> NoteEditor::velocityArea() const noexcept {
    // As high as its divider left it, while the key tracks keep their floor; with no room for both, no lane.
    const int room = getHeight() - kRulerHeight - barsHeight() - kStretchHeight - kKeysFloor;
    const int h = room >= kLaneFloor ? std::clamp(laneHeight_, kLaneFloor, room) : 0;
    return {kKeysWidth, getHeight() - h, std::max(0, getWidth() - kKeysWidth), h};
}
void NoteEditor::setLaneHeight(int height) {
    // Held to the room there is once the editor has a size; before that, kept as given.
    const int room = getHeight() - kRulerHeight - barsHeight() - kStretchHeight - kKeysFloor;
    laneHeight_ = getHeight() > 0 ? std::clamp(height, kLaneFloor, std::max(kLaneFloor, room)) : std::max(kLaneFloor, height);
    keys.clampScroll(notesArea().getHeight());
    repaint();
}
juce::Rectangle<int> NoteEditor::laneDivider() const noexcept {
    const auto lane = velocityArea();
    return lane.isEmpty() ? lane : juce::Rectangle<int>(0, lane.getY() - 3, getWidth(), 6);
}
juce::Rectangle<int> NoteEditor::stretchArea() const noexcept {
    return {kKeysWidth, barsHeight() + kRulerHeight, std::max(0, getWidth() - kKeysWidth), kStretchHeight};
}
juce::Rectangle<int> NoteEditor::notesArea() const noexcept {
    const int bottom = getHeight() - velocityArea().getHeight();
    const int top = barsHeight() + kRulerHeight + kStretchHeight;
    return {kKeysWidth, top, std::max(0, getWidth() - kKeysWidth), std::max(0, bottom - top)};
}
juce::Rectangle<int> NoteEditor::keysArea() const noexcept {
    const auto n = notesArea();
    return {0, n.getY(), kKeysWidth, n.getHeight()};
}
std::optional<NoteBox> NoteEditor::boxOf(const engine::ClipNote &note) const noexcept {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return std::nullopt;
    return boxOf(note, *clip);
}
std::optional<NoteBox> NoteEditor::boxOf(const engine::ClipNote &note, const engine::ClipDetails &of) const noexcept {
    const auto y = keys.y(note.key);
    if (!y)
        return std::nullopt;
    const auto window = ClipWindow::of(of);
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
    if (loopPreview_)
        return juce::Range<int>(at(loopPreview_->first), at(loopPreview_->first + loopPreview_->second));
    return juce::Range<int>(at(clip->loopStartTicks), at(clip->loopStartTicks + loopLengthOf(*clip)));
}
const std::vector<std::uint64_t> &NoteEditor::selectedIn(std::int64_t clip) const noexcept {
    static const std::vector<std::uint64_t> none;
    if (clip == panel.clip())
        return selected_;
    for (const auto &entry : otherSelected_)
        if (entry.first == clip)
            return entry.second;
    return none;
}
std::vector<std::uint64_t> &NoteEditor::otherIds(std::int64_t clip) {
    for (auto &entry : otherSelected_)
        if (entry.first == clip)
            return entry.second;
    otherSelected_.emplace_back(clip, std::vector<std::uint64_t>{});
    return otherSelected_.back().second;
}
bool NoteEditor::anySelected() const noexcept {
    if (!selected_.empty())
        return true;
    if (panel.focus())
        return false;
    return std::any_of(otherSelected_.begin(), otherSelected_.end(), [](const auto &entry) { return !entry.second.empty(); });
}
void NoteEditor::rehome(std::int64_t from, std::int64_t to) {
    auto mine = std::move(selected_);
    selected_ = std::move(otherIds(to));
    std::erase_if(otherSelected_, [to](const auto &entry) { return entry.first == to; });
    if (!mine.empty())
        otherIds(from) = std::move(mine);
}
NoteEdit NoteEditor::across(const Build &build) const {
    NoteEdit all;
    const auto *clip = panel.details();
    if (clip == nullptr)
        return all;
    // The edited clip first; then, with Focus off, every other shown clip that has notes selected.
    // One clip's refusal refuses the whole gesture.
    if (!selected_.empty() || !anySelected())
        all = build(*clip, selected_);
    if (!all.refusal.empty() || panel.focus())
        return all;
    for (const auto &layer : panel.others()) {
        const auto &ids = selectedIn(layer.details.id);
        if (ids.empty())
            continue;
        auto part = build(layer.details, ids);
        if (!part.refusal.empty()) {
            all.ops.clear();
            all.refusal = std::move(part.refusal);
            return all;
        }
        for (auto &o : part.ops)
            all.ops.push_back(std::move(o));
    }
    return all;
}
std::optional<std::pair<std::int64_t, std::int64_t>> NoteEditor::timeSelection() const noexcept {
    if (!insert_ || !head_ || *head_ == *insert_)
        return std::nullopt;
    return std::pair{std::min(*insert_, *head_), std::max(*insert_, *head_)};
}
void NoteEditor::selectTime(std::int64_t from, std::int64_t to) {
    insert_ = from;
    head_ = to;
    if (from == to)
        head_.reset();
    repaint();
}
void NoteEditor::placeMarker(std::int64_t at, bool extend) {
    if (extend) {
        if (!insert_)
            insert_ = at;
        head_ = at;
        if (*head_ == *insert_)
            head_.reset();
    } else {
        insert_ = at;
        head_.reset();
    }
    // Kept in view.
    const auto area = notesArea();
    if (const double x = time.x(at); x < 0)
        time.pan(x - 24);
    else if (area.getWidth() > 0 && x > area.getWidth())
        time.pan(x - area.getWidth() + 24);
    repaint();
}
bool NoteEditor::toggleTimeAndNotes() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    if (anySelected()) {
        // The notes give way to the time they occupy; a time selection that already holds them stays.
        std::int64_t from = std::numeric_limits<std::int64_t>::max(), to = std::numeric_limits<std::int64_t>::min(), last = to;
        const auto take = [&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) {
            const auto window = ClipWindow::of(of);
            for (const auto &n : of.notes)
                if (std::find(ids.begin(), ids.end(), n.id) != ids.end()) {
                    from = std::min(from, window.toArrangement(n.startTicks));
                    last = std::max(last, window.toArrangement(n.startTicks));
                    to = std::max(to, window.toArrangement(n.startTicks + n.durationTicks));
                }
        };
        take(*clip, selected_);
        if (!panel.focus())
            for (const auto &layer : panel.others())
                take(layer.details, selectedIn(layer.details.id));
        if (from >= to)
            return false;
        if (const auto span = timeSelection(); !span || span->first > from || span->second <= last) {
            insert_ = from;
            head_ = to;
        }
        selected_.clear();
        otherSelected_.clear();
        repaint();
        return true;
    }
    const auto span = timeSelection();
    if (!span)
        return false;
    // The notes the time holds: those that begin inside it.
    const auto inside = [&](const engine::ClipDetails &of, const engine::ClipNote &n) {
        const auto at = ClipWindow::of(of).toArrangement(n.startTicks);
        return at >= span->first && at < span->second;
    };
    for (const auto &n : clip->notes)
        if (inside(*clip, n))
            selected_.push_back(n.id);
    if (!panel.focus())
        for (const auto &layer : panel.others())
            for (const auto &n : layer.details.notes)
                if (inside(layer.details, n))
                    otherIds(layer.details.id).push_back(n.id);
    repaint();
    return true;
}
void NoteEditor::invertSelection() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    std::vector<std::uint64_t> inverted;
    for (const auto &n : clip->notes)
        if (!isSelected(n.id))
            inverted.push_back(n.id);
    if (!panel.focus())
        for (const auto &layer : panel.others()) {
            const auto was = selectedIn(layer.details.id);
            auto &ids = otherIds(layer.details.id);
            ids.clear();
            for (const auto &n : layer.details.notes)
                if (std::find(was.begin(), was.end(), n.id) == was.end())
                    ids.push_back(n.id);
        }
    selected_ = std::move(inverted);
    repaint();
}
bool NoteEditor::walk(bool sideways, bool forward, bool extend) {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    const auto window = ClipWindow::of(*clip);
    if (selected_.empty()) {
        if (sideways) {
            // The insert marker, or the time selection's moving end, to the next note edge.
            const auto from = extend ? head_.value_or(insert_.value_or(window.pos)) : insert_.value_or(window.pos);
            std::optional<std::int64_t> best;
            for (const auto &n : clip->notes)
                for (const auto edge : {window.toArrangement(n.startTicks), window.toArrangement(n.startTicks + n.durationTicks)})
                    if ((forward ? edge > from : edge < from) && (!best || (forward ? edge < *best : edge > *best)))
                        best = edge;
            if (best) {
                if (extend && !insert_)
                    insert_ = window.pos;
                placeMarker(*best, extend);
            }
            return true;
        }
        // The note nearest the insert marker.
        const auto at = insert_.value_or(window.pos);
        const engine::ClipNote *nearest = nullptr;
        for (const auto &n : clip->notes)
            if (nearest == nullptr ||
                std::abs(window.toArrangement(n.startTicks) - at) < std::abs(window.toArrangement(nearest->startTicks) - at))
                nearest = &n;
        if (nearest != nullptr) {
            selected_ = {nearest->id};
            anchor_ = nearest->id;
        }
        repaint();
        return true;
    }
    // From the note last picked: the next or the previous in time, or on its own key.
    const auto *from = note_edits_detail::find(*clip, isSelected(anchor_) ? anchor_ : selected_.back());
    if (from == nullptr)
        return true;
    const auto before = [](const engine::ClipNote &a, const engine::ClipNote &b) {
        return a.startTicks != b.startTicks ? a.startTicks < b.startTicks : a.key != b.key ? a.key < b.key : a.id < b.id;
    };
    const engine::ClipNote *next = nullptr;
    for (const auto &n : clip->notes) {
        if (n.id == from->id || (sideways && n.key != from->key))
            continue;
        if (forward ? !before(*from, n) : !before(n, *from))
            continue;
        if (next == nullptr || (forward ? before(n, *next) : before(*next, n)))
            next = &n;
    }
    if (next != nullptr) {
        if (!extend) {
            selected_.clear();
            otherSelected_.clear();
        }
        if (!isSelected(next->id))
            selected_.push_back(next->id);
        anchor_ = next->id;
        head_.reset();
    }
    repaint();
    return true;
}
int NoteEditor::markerX(bool end) const noexcept {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return 0;
    const auto window = ClipWindow::of(*clip);
    auto source = end ? endMarkerOf(*clip) : clip->startMarkerTicks;
    if (markerPreview_ && rulerDrag_ == (end ? RulerPart::EndMarker : RulerPart::StartMarker))
        source = *markerPreview_;
    return rulerArea().getX() + static_cast<int>(std::clamp(time.x(window.toArrangement(source)), -1e6, 1e6));
}
NoteEditor::RulerPart NoteEditor::rulerPartAt(juce::Point<int> at) const noexcept {
    const auto *clip = panel.details();
    const auto ruler = rulerArea();
    if (clip == nullptr || !ruler.contains(at))
        return RulerPart::None;
    // The brace has the ruler's upper half, the markers its lower.
    if (at.y < ruler.getY() + ruler.getHeight() / 2) {
        if (const auto brace = loopBrace()) {
            const int toStart = std::abs(at.x - brace->getStart()), toEnd = std::abs(at.x - brace->getEnd());
            if (std::min(toStart, toEnd) <= 4)
                return toEnd <= toStart ? RulerPart::LoopEnd : RulerPart::LoopStart;
            if (at.x > brace->getStart() && at.x < brace->getEnd())
                return RulerPart::Loop;
        }
        return RulerPart::None;
    }
    const int toStart = std::abs(at.x - markerX(false)), toEnd = std::abs(at.x - markerX(true));
    if (std::min(toStart, toEnd) > 6)
        return RulerPart::None;
    return toEnd < toStart ? RulerPart::EndMarker : RulerPart::StartMarker;
}
void NoteEditor::rulerDrag(const juce::ModifierKeys &mods) {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return;
    const auto window = ClipWindow::of(*clip);
    const auto area = rulerArea();
    // Where the pointer is, on the nearest grid line; and how far it has come, in whole grid steps.
    const auto raw = time.tick(static_cast<double>(std::max(0, to_.x - area.getX())));
    auto at = raw;
    if (snapping(mods)) {
        const auto below = time.snap(raw, false), above = time.snap(raw, true);
        at = raw - below <= above - raw ? below : above;
    }
    const double moved = static_cast<double>(to_.x - from_.x) / time.scale * static_cast<double>(textproj::kPPQ);
    const auto step = static_cast<double>(gridTicks());
    const auto by = snapping(mods) ? static_cast<std::int64_t>(std::llround(moved / step) * static_cast<long long>(step))
                                   : static_cast<std::int64_t>(std::llround(moved));
    const auto start = clip->loopStartTicks, length = loopLengthOf(*clip);
    loopPreview_.reset();
    markerPreview_.reset();
    if (to_ == from_)
        return;
    switch (rulerDrag_) {
    case RulerPart::LoopStart: {
        const auto to = std::clamp<std::int64_t>(window.toSource(at), 0, start + length - 1);
        loopPreview_ = {to, start + length - to};
        break;
    }
    case RulerPart::LoopEnd:
        loopPreview_ = {start, std::max<std::int64_t>(1, window.toSource(at) - start)};
        break;
    case RulerPart::Loop:
        loopPreview_ = {std::max<std::int64_t>(0, start + by), length};
        break;
    case RulerPart::StartMarker:
        markerPreview_ = std::clamp<std::int64_t>(window.toSource(at), 0, endMarkerOf(*clip) - 1);
        break;
    case RulerPart::EndMarker:
        markerPreview_ = std::max(window.toSource(at), clip->startMarkerTicks + 1);
        break;
    case RulerPart::None:
        break;
    }
}
bool NoteEditor::rulerKey(const juce::KeyPress &k) {
    const auto *clip = panel.details();
    if (clip == nullptr || picked_ == RulerPart::None)
        return false;
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();
    if (code == juce::KeyPress::escapeKey) {
        picked_ = RulerPart::None;
        repaint();
        return true;
    }
    if (picked_ == RulerPart::Loop && k == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)) {
        duplicateLoop();
        return true;
    }
    const bool sideways = code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
    const bool upDown = code == juce::KeyPress::upKey || code == juce::KeyPress::downKey;
    if (!sideways && !upDown)
        return false;
    const auto window = ClipWindow::of(*clip);
    if (picked_ == RulerPart::Loop) {
        if (mods.isShiftDown() || mods.isAltDown())
            return true;
        // As the Arrangement's brace (manual 8.9), on the song's grid.
        const auto key = code == juce::KeyPress::leftKey    ? BraceKey::Left
                         : code == juce::KeyPress::rightKey ? BraceKey::Right
                         : code == juce::KeyPress::upKey    ? BraceKey::Up
                                                            : BraceKey::Down;
        const auto from = window.toArrangement(clip->loopStartTicks);
        const auto to = braceKey(LoopRange{from, from + loopLengthOf(*clip)}, key, mods.isCommandDown(),
                                 [this](std::int64_t tick, bool forward) { return time.nextGridLine(tick, forward); });
        if (to)
            panel.commit(setClipLoop(*clip, window.toSource(to->start), to->length()));
        return true;
    }
    if (!sideways || mods.isCommandDown() || mods.isShiftDown())
        return true;
    // A marker: by the grid; with Alt/Option, the whole region, both markers together.
    const auto by = code == juce::KeyPress::rightKey ? gridTicks() : -gridTicks();
    const auto start = clip->startMarkerTicks, end = endMarkerOf(*clip);
    if (!mods.isAltDown()) {
        panel.commit(picked_ == RulerPart::StartMarker ? setClipStart(*clip, start + by) : setClipEnd(*clip, end + by));
        return true;
    }
    // The one that leads goes first, so the region is never empty between the two ops.
    auto moved = *clip;
    TrackEdit both;
    if (by > 0) {
        both = setClipEnd(*clip, end + by);
        moved.endMarkerTicks = end + by;
    } else {
        both = setClipStart(*clip, start + by);
        moved.startMarkerTicks = start + by;
    }
    if (both.refusal.empty()) {
        auto second = by > 0 ? setClipStart(moved, start + by) : setClipEnd(moved, end + by);
        both.refusal = std::move(second.refusal);
        for (auto &o : second.ops)
            both.ops.push_back(std::move(o));
        for (auto &o : both.ops)
            o.label = "Move Clip Region";
    }
    panel.commit(std::move(both));
    return true;
}
// ---- The Velocity lane as Live's (manual 10.5.12) ----------------------------------------------
juce::Rectangle<int> NoteEditor::readoutBox() const noexcept {
    const auto lane = velocityArea();
    return lane.isEmpty() ? juce::Rectangle<int>() : juce::Rectangle<int>(2, lane.getY() + 35, kKeysWidth - 5, 12);
}
juce::String NoteEditor::velocityReadout() const {
    if (typedVelocity_.isNotEmpty())
        return typedVelocity_;
    const auto *clip = panel.details();
    if (clip == nullptr || selected_.empty())
        return {};
    int low = 128, high = 0;
    for (const auto &n : clip->notes)
        if (isSelected(n.id)) {
            const int v = shown(n).velocity;
            low = std::min(low, v);
            high = std::max(high, v);
        }
    if (low > high)
        return {};
    return low == high ? juce::String(low) : juce::String(low) + " - " + juce::String(high);
}
void NoteEditor::beginVelocityDrag(bool byNote) {
    const auto *clip = panel.details();
    shapePreview_.clear();
    velocityByNote_ = byNote;
    velocityLast_ = 0;
    if (clip == nullptr)
        return;
    for (const auto &n : clip->notes)
        if (isSelected(n.id))
            shapePreview_.emplace_back(n.id, static_cast<int>(n.velocity));
    // In the lane the marker held goes to the pointer; on a note the drag counts from where it began.
    if (const auto *held = note_edits_detail::find(*clip, anchor_); held != nullptr && !byNote)
        velocityLast_ = held->velocity;
    drag_ = Drag::Velocity;
}
void NoteEditor::velocityDrag() {
    // In the lane the marker held follows the pointer, and the others move as far as it does: one
    // that reaches the top or the foot waits there, so the marker farthest from an end, dragged to
    // it and back, gives them all one value. On a note the drag counts a value a pixel.
    int target = from_.y - to_.y;
    if (!velocityByNote_) {
        const auto lane = velocityArea().withTrimmedTop(1);
        target = velocityAt(to_.y, lane.getY() + 5, lane.getHeight() - 10);
    }
    const int step = target - velocityLast_;
    velocityLast_ = target;
    for (auto &entry : shapePreview_)
        entry.second = std::clamp(entry.second + step, 1, 127);
}
void NoteEditor::laneDrawStroke(const juce::ModifierKeys &mods) {
    const auto *clip = panel.details();
    const auto lane = velocityArea().withTrimmedTop(1);
    if (clip == nullptr || lane.isEmpty())
        return;
    const auto window = ClipWindow::of(*clip);
    // From where the pointer last was to where it is, so a fast stroke misses no step: the value
    // at a place along it is the stroke's height there.
    const auto a = std::exchange(laneLast_, to_), b = to_;
    const auto valueAt = [&](double x) {
        const double t = a.x == b.x ? 1. : std::clamp((x - a.x) / static_cast<double>(b.x - a.x), 0., 1.);
        return velocityAt(a.y + t * (b.y - a.y), lane.getY() + 5, lane.getHeight() - 10);
    };
    const auto set = [&](std::uint64_t id, int value) {
        for (auto &entry : shapePreview_)
            if (entry.first == id) {
                entry.second = value;
                return;
            }
        shapePreview_.emplace_back(id, value);
    };
    const auto mine = [&](const engine::ClipNote &n) { return selected_.empty() || isSelected(n.id); };
    const int left = std::min(a.x, b.x), right = std::max(a.x, b.x);
    if (snapping(mods)) {
        // Every note that begins in a grid step the stroke crosses.
        const auto from = time.snap(time.tick(static_cast<double>(std::max(0, left - lane.getX()))), false);
        const auto to = time.nextGridLine(time.snap(time.tick(static_cast<double>(std::max(0, right - lane.getX()))), false), true);
        for (const auto &n : clip->notes) {
            const auto start = window.toArrangement(n.startTicks);
            if (!mine(n) || start < from || start >= to)
                continue;
            // The step's value is the stroke's where it crosses the step, nearest its marker.
            const double x = std::clamp(lane.getX() + time.x(start), static_cast<double>(left), static_cast<double>(right));
            set(n.id, valueAt(x));
        }
        return;
    }
    // Off the grid: the markers the stroke passes over, each alone.
    for (const auto &n : clip->notes)
        if (const double x = lane.getX() + time.x(window.toArrangement(n.startTicks)); mine(n) && x >= left - 5 && x <= right + 5)
            set(n.id, valueAt(x));
}
std::uint64_t NoteEditor::statusStamp() const noexcept {
    // The selection, the time selection and whatever a held drag is previewing.
    std::uint64_t stamp = 1469598103934665603ull;
    const auto mix = [&stamp](std::uint64_t value) { stamp = (stamp ^ value) * 1099511628211ull; };
    for (const auto id : selected_)
        mix(id);
    mix(selected_.size());
    mix(static_cast<std::uint64_t>(insert_.value_or(-1)));
    mix(static_cast<std::uint64_t>(head_.value_or(-1)));
    mix(static_cast<std::uint64_t>(drag_));
    mix(static_cast<std::uint64_t>(dragTicks_));
    mix(static_cast<std::uint64_t>(dragKeys_));
    // The Clip View Selector frames what the editor shows.
    mix(static_cast<std::uint64_t>(time.tick(0.)));
    mix(static_cast<std::uint64_t>(std::llround(time.scale * 1024.)));
    for (const auto &entry : shapePreview_)
        mix(entry.first * 131 + static_cast<std::uint64_t>(entry.second));
    return stamp;
}
juce::String NoteEditor::statusText(const std::vector<textproj::Meter> &meters) const {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return {};
    const auto where = [&meters](std::int64_t tick) {
        const auto at = barPosition(tick, meters);
        return juce::String(at.bar) + "." + juce::String(at.beat) + "." + juce::String(at.sixteenth);
    };
    const auto length = [&meters](std::int64_t from, std::int64_t to) {
        const auto span = barSpan(from, to - from, meters);
        return juce::String(span.bar) + "." + juce::String(span.beat) + "." + juce::String(span.sixteenth);
    };
    const auto window = ClipWindow::of(*clip);
    std::int64_t from = std::numeric_limits<std::int64_t>::max(), to = std::numeric_limits<std::int64_t>::min();
    int lowKey = 128, highKey = -1, lowVelocity = 128, highVelocity = -1, lowChance = 100000, highChance = -1, count = 0;
    for (const auto &stored : clip->notes) {
        if (!isSelected(stored.id))
            continue;
        const auto n = shown(stored);
        ++count;
        from = std::min(from, window.toArrangement(n.startTicks));
        to = std::max(to, window.toArrangement(n.startTicks + n.durationTicks));
        lowKey = std::min<int>(lowKey, n.key);
        highKey = std::max<int>(highKey, n.key);
        lowVelocity = std::min<int>(lowVelocity, n.velocity);
        highVelocity = std::max<int>(highVelocity, n.velocity);
        lowChance = std::min<int>(lowChance, n.probability);
        highChance = std::max<int>(highChance, n.probability);
    }
    if (count == 0) {
        if (const auto span = timeSelection())
            return "Time Selection   Start: " + where(span->first) + "   End: " + where(span->second) + "   Length: " +
                   length(span->first, span->second);
        return {};
    }
    const auto percent = [](int basisPoints) { return juce::String((basisPoints + 50) / 100) + "%"; };
    const auto pitch = lowKey == highKey ? juce::String(named(lowKey)) + " (" + juce::String(lowKey) + ")"
                                         : juce::String(named(lowKey)) + " - " + juce::String(named(highKey));
    return "Note Selection   Time: " + where(from) + " - " + where(to) + " (" + length(from, to) + ")   Pitch: " + pitch +
           "   Velocity: " + (lowVelocity == highVelocity ? juce::String(lowVelocity) : juce::String(lowVelocity) + " - " + juce::String(highVelocity)) +
           "   Probability: " + (lowChance == highChance ? percent(lowChance) : percent(lowChance) + " - " + percent(highChance));
}
juce::Colour NoteEditor::noteFill(const engine::ClipNote &note) const {
    if (!note.active)
        return juce::Colour(0xff9a9a9a);
    const auto *clip = panel.details();
    const int velocity = std::clamp<int>(note.velocity, 1, 127);
    switch (noteColours) {
    case NoteColours::Velocity:
        return juce::Colour::fromHSV(0.62f * static_cast<float>(127 - velocity) / 126.f, 0.62f, 0.95f, 1.f);
    case NoteColours::Pitch:
        return juce::Colour::fromHSV(static_cast<float>(note.key % 12) / 12.f, 0.5f, 0.95f, 1.f);
    case NoteColours::Channel:
        return juce::Colour::fromHSV(static_cast<float>(note.channel % 16) / 16.f, 0.58f, 0.92f, 1.f);
    case NoteColours::Degree: {
        if (clip == nullptr || !inScale(note.key, static_cast<int>(clip->scale.root), clip->scale.mask))
            return juce::Colour(0xff6e6e6e);
        // Which degree of the scale it is, and of how many.
        const int above = (note.key % 12 + 12 - static_cast<int>(clip->scale.root) % 12) % 12;
        int degree = 0, degrees = 0;
        for (int step = 0; step < 12; ++step)
            if (((clip->scale.mask >> step) & 1) != 0) {
                if (step < above)
                    ++degree;
                ++degrees;
            }
        return juce::Colour::fromHSV(static_cast<float>(degree) / static_cast<float>(std::max(1, degrees)), 0.55f, 0.95f, 1.f);
    }
    case NoteColours::Clip:
        break;
    }
    // Live's: the clip's colour, paler as the velocity falls.
    return panel.colour().interpolatedWith(juce::Colours::white, 0.55f * static_cast<float>(127 - velocity) / 126.f);
}
// ---- Getting about (manual 8.7, 10.2) ----------------------------------------------------------
void NoteEditor::fitWidth() {
    const auto *clip = panel.details();
    const auto area = notesArea();
    if (clip == nullptr || area.getWidth() <= 0)
        return;
    const auto window = ClipWindow::of(*clip);
    auto from = window.pos, to = window.end();
    for (const auto &layer : panel.others()) {
        const auto other = ClipWindow::of(layer.details);
        from = std::min(from, other.pos);
        to = std::max(to, other.end());
    }
    const auto length = std::max<std::int64_t>(textproj::kPPQ, to - from);
    time.fit(from, from + length + length / 24, area.getWidth());
    panel.root.arrangement.followPaused = true;
    repaint();
}
void NoteEditor::fitHeight() {
    const auto *clip = panel.details();
    const auto area = notesArea();
    if (clip == nullptr || area.getHeight() <= 0)
        return;
    if (keys.isFolded()) {
        keys.keyHeight = std::clamp(area.getHeight() / std::max(1, keys.rowCount()), KeyTracks::kMinHeight, KeyTracks::kMaxHeight);
        keys.scrollY = 0;
        keys.clampScroll(area.getHeight());
        repaint();
        return;
    }
    int low = 60, high = 71;
    bool any = false;
    const auto take = [&](const std::vector<engine::ClipNote> &notes) {
        for (const auto &n : notes) {
            low = any ? std::min<int>(low, n.key) : n.key;
            high = any ? std::max<int>(high, n.key) : n.key;
            any = true;
        }
    };
    take(clip->notes);
    for (const auto &layer : panel.others())
        take(layer.details.notes);
    keys.keyHeight = std::clamp(area.getHeight() / (high - low + 1), KeyTracks::kMinHeight, KeyTracks::kMaxHeight);
    keys.show(low, high, area.getHeight());
    repaint();
}
void NoteEditor::zoomTime(bool in) {
    const auto area = notesArea();
    const auto span = timeSelection();
    const auto at = span ? (span->first + span->second) / 2 : insert_ ? *insert_ : time.tick(area.getWidth() / 2.);
    time.zoomAround(in ? 1.25 : 0.8, at);
    panel.root.arrangement.followPaused = true;
    repaint();
    panel.repaint();
}
void NoteEditor::gridKey(int key) {
    // The arrangement's rule (manual 6.10, 10.2.1), on the editor's own grid.
    auto &grid = time.grid;
    switch (key) {
    case 1:
    case 2: {
        const bool narrower = key == 1;
        if (grid.fixed)
            grid.fixedQuarters = std::clamp(grid.fixedQuarters * (narrower ? .5 : 2.), ArrangementGeometry::kFinestGrid, ArrangementGeometry::kWidestGrid);
        else
            grid.narrow = std::clamp(grid.narrow + (narrower ? 1 : -1), -ArrangementGeometry::kGridNarrowest, ArrangementGeometry::kGridNarrowest);
        break;
    }
    case 3:
        grid.triplet = !grid.triplet;
        break;
    case 4:
        grid.snap = !grid.snap;
        break;
    case 5:
        if (!grid.fixed)
            grid.fixedQuarters = time.plainGridQuarters();
        grid.fixed = !grid.fixed;
        break;
    default:
        return;
    }
    repaint();
    panel.repaint();
    panel.keepSettings();
}
// ---- Time inside the clip (manual 10.7) --------------------------------------------------------
bool NoteEditor::crop() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    const auto window = ClipWindow::of(*clip);
    if (const auto span = timeSelection())
        return commit(cropNotes(*clip, window.toSource(span->first), window.toSource(span->second), true));
    if (clip->loopEnabled)
        return commit(cropNotes(*clip, clip->loopStartTicks, clip->loopStartTicks + loopLengthOf(*clip), true));
    return commit(cropNotes(*clip, clip->startMarkerTicks, endMarkerOf(*clip), false));
}
bool NoteEditor::timeCommand(TimeCommand command) {
    const auto *clip = panel.details();
    const auto span = timeSelection();
    if (clip == nullptr || !span)
        return false;
    const auto window = ClipWindow::of(*clip);
    const auto from = window.toSource(span->first), to = window.toSource(span->second);
    const bool done = commit(command == TimeCommand::Duplicate ? duplicateTime(*clip, from, to)
                             : command == TimeCommand::Delete  ? deleteTime(*clip, from, to)
                                                               : insertTime(*clip, from, to));
    // The deleted time is gone: the insert marker stands where it was.
    if (done && command == TimeCommand::Delete)
        placeMarker(span->first, false);
    return done;
}
bool NoteEditor::duplicateLoop() {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    const auto start = clip->loopStartTicks, length = loopLengthOf(*clip);
    if (length <= 0)
        return false;
    auto edit = duplicateTime(*clip, start, start + length, "Duplicate Loop");
    if (!edit.refusal.empty())
        return commit(std::move(edit));
    OpRequest loop;
    loop.opType = "clip.setLoop";
    loop.payload = {{"id", clip->id}, {"on", clip->loopEnabled}, {"start", start}, {"length", 2 * length}};
    loop.label = "Duplicate Loop";
    // The loop's op first: the step is named for it even when the loop holds no notes.
    edit.ops.insert(edit.ops.begin(), std::move(loop));
    if (!commit(std::move(edit)))
        return false;
    // Zoomed out as far as needed to show the whole loop.
    const auto window = ClipWindow::of(*clip);
    const auto area = notesArea();
    if (const auto end = window.toArrangement(start + 2 * length); time.x(end) > area.getWidth() || time.x(window.toArrangement(start)) < 0) {
        const auto from = std::min(window.pos, window.toArrangement(start));
        const auto to = std::max(window.end(), end);
        time.fit(from, to + (to - from) / 24, area.getWidth());
    }
    repaint();
    return true;
}
// ---- MIDI Note Stretch (manual 10.5.6) ---------------------------------------------------------
std::vector<std::uint64_t> NoteEditor::stretchTargets() const {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return {};
    if (selected_.size() >= 2)
        return selected_;
    std::vector<std::uint64_t> ids;
    if (const auto span = timeSelection(); span && selected_.empty()) {
        const auto window = ClipWindow::of(*clip);
        for (const auto &n : clip->notes)
            if (const auto at = window.toArrangement(n.startTicks); at >= span->first && at < span->second)
                ids.push_back(n.id);
    }
    return ids;
}
std::optional<std::pair<std::int64_t, std::int64_t>> NoteEditor::stretchSpan() const {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return std::nullopt;
    if (drag_ == Drag::Stretch)
        // Held: the pair as the drag has it.
        return stretchPart_ == StretchPart::Start ? std::pair{stretchNow_, stretchB_}
               : stretchPart_ == StretchPart::End ? std::pair{stretchA_, stretchNow_}
                                                  : std::pair{stretchA_, stretchB_};
    const auto window = ClipWindow::of(*clip);
    if (selected_.size() >= 2) {
        std::int64_t from = std::numeric_limits<std::int64_t>::max(), to = std::numeric_limits<std::int64_t>::min();
        for (const auto &n : clip->notes)
            if (isSelected(n.id)) {
                from = std::min(from, window.toArrangement(n.startTicks));
                to = std::max(to, window.toArrangement(n.startTicks + n.durationTicks));
            }
        return from < to ? std::optional(std::pair{from, to}) : std::nullopt;
    }
    if (const auto span = timeSelection(); span && selected_.empty())
        for (const auto &n : clip->notes)
            if (const auto at = window.toArrangement(n.startTicks); at >= span->first && at < span->second)
                return span;
    return std::nullopt;
}
NoteEditor::StretchPart NoteEditor::stretchPartAt(juce::Point<int> at) const {
    const auto area = stretchArea();
    const auto span = stretchSpan();
    if (!span || !area.contains(at))
        return StretchPart::None;
    const int a = area.getX() + static_cast<int>(std::clamp(time.x(span->first), -1e6, 1e6));
    const int b = area.getX() + static_cast<int>(std::clamp(time.x(span->second), -1e6, 1e6));
    const int toA = std::abs(at.x - a), toB = std::abs(at.x - b);
    if (std::min(toA, toB) <= 5)
        return toB < toA ? StretchPart::End : StretchPart::Start;
    return at.x > a && at.x < b ? StretchPart::Middle : StretchPart::None;
}
std::int64_t NoteEditor::stretchMap(std::int64_t songTick) const noexcept {
    switch (stretchPart_) {
    case StretchPart::Start:
        return stretchPoint(songTick, stretchA_, stretchB_, stretchNow_, stretchB_);
    case StretchPart::End:
        return stretchPoint(songTick, stretchA_, stretchB_, stretchA_, stretchNow_);
    case StretchPart::Middle:
        return stretchPointAround(songTick, stretchA_, stretchP_, stretchB_, stretchNow_);
    case StretchPart::None:
        break;
    }
    return songTick;
}
// ---- The editor's context menu -----------------------------------------------------------------
std::string NoteEditor::named(int key) const {
    const auto *clip = panel.details();
    return noteName(key, spelling, clip != nullptr ? static_cast<int>(clip->scale.root) : 0);
}
juce::PopupMenu NoteEditor::spellingMenu() const {
    juce::PopupMenu menu;
    static constexpr const char *kNames[] = {"Sharps (C#)", "Flats (Db)", "Sharps and Flats (C#/Db)", "Auto", "MIDI Note Numbers"};
    for (int i = 0; i < 5; ++i) {
        if (i == 4)
            menu.addSeparator();
        menu.addItem(i + 1, kNames[i], true, static_cast<int>(spelling) == i);
    }
    return menu;
}
bool NoteEditor::chooseSpelling(int item) {
    if (item < 1 || item > 5)
        return false;
    spelling = static_cast<NoteSpelling>(item - 1);
    repaint();
    panel.repaint();
    panel.keepSettings();
    panel.root.mark(DirtySet::Status);
    return true;
}
void NoteEditor::contextClick(juce::Point<int> at) {
    if (const auto *n = noteAt(at); n != nullptr && !isSelected(n->id)) {
        selected_ = {n->id};
        otherSelected_.clear();
        head_.reset();
    }
    repaint();
}
juce::PopupMenu NoteEditor::contextMenu() const {
    juce::PopupMenu menu;
    const auto *clip = panel.details();
    if (clip == nullptr)
        return menu;
    const bool notes = anySelected(), timed = timeSelection().has_value();
    bool allOff = notes;
    for (const auto &n : clip->notes)
        if (isSelected(n.id) && n.active)
            allOff = false;
    menu.addItem(kCut, "Cut", notes);
    menu.addItem(kCopy, "Copy", notes);
    menu.addItem(kPaste, "Paste", !panel.noteClipboard.empty());
    menu.addItem(kDuplicate, "Duplicate", !selected_.empty());
    menu.addItem(kDelete, "Delete", notes);
    menu.addSeparator();
    menu.addItem(kDeactivate, allOff ? "Activate Note(s)" : "Deactivate Note(s)", notes);
    menu.addSeparator();
    menu.addItem(kSelectAll, "Select All");
    menu.addItem(kInvertSelection, "Invert Selection", !clip->notes.empty());
    menu.addSeparator();
    menu.addItem(kQuantize, "Quantize", !clip->notes.empty());
    menu.addItem(kQuantizeSettings, "Quantize Settings...");
    menu.addItem(kSplit, notes ? "Chop Notes on Grid" : timed ? "Split Notes at Time Selection" : "Split Notes at Insert Marker",
                 notes || timed || insert_.has_value());
    menu.addItem(kJoin, "Join Notes", notes);
    menu.addSeparator();
    menu.addItem(kCrop, timed ? "Crop Clip to Time Selection" : "Crop Clip");
    menu.addItem(kDuplicateTime, "Duplicate Time", timed);
    menu.addItem(kDeleteTime, "Delete Time", timed);
    menu.addItem(kInsertTime, "Insert Time", timed);
    menu.addItem(kDuplicateLoop, "Duplicate Loop", loopLengthOf(*clip) > 0);
    menu.addSeparator();
    menu.addSubMenu("Grid", panel.gridMenu(kGridItems));
    menu.addItem(kFoldToNotes, "Fold to Notes", true, folded_);
    menu.addItem(kFoldToScale, "Fold to Scale", scaleOn(), scaleFolded_);
    menu.addItem(kHighlightScale, "Highlight Scales", true, highlightScale);
    return menu;
}
bool NoteEditor::chooseContextItem(int item) {
    const auto *clip = panel.details();
    if (clip == nullptr)
        return false;
    const auto key = [this](int code, int mods = 0) { return keyPressed(juce::KeyPress(code, juce::ModifierKeys(mods), 0)); };
    const int command = juce::ModifierKeys::commandModifier;
    switch (item) {
    case kCut:
        return cutSelected();
    case kCopy:
        return copySelected();
    case kPaste:
        return paste();
    case kDuplicate:
        return duplicate();
    case kDelete:
        return anySelected() && key(juce::KeyPress::deleteKey);
    case kDeactivate:
        return anySelected() && key('0');
    case kSelectAll:
        return key('a', command);
    case kInvertSelection:
        invertSelection();
        return true;
    case kQuantize:
        return quantize();
    case kQuantizeSettings:
        panel.setToolsTab(1);
        return true;
    case kSplit:
        return splitOrChop();
    case kJoin:
        return join();
    case kCrop:
        return crop();
    case kDuplicateTime:
        return timeCommand(TimeCommand::Duplicate);
    case kDeleteTime:
        return timeCommand(TimeCommand::Delete);
    case kInsertTime:
        return timeCommand(TimeCommand::Insert);
    case kDuplicateLoop:
        return duplicateLoop();
    case kFoldToNotes:
        setFold(!folded_);
        panel.repaint();
        return true;
    case kFoldToScale:
        setScaleFold(!scaleFolded_);
        panel.repaint();
        return true;
    case kHighlightScale:
        return key('k');
    default:
        return item >= kGridItems && panel.chooseGrid(item - kGridItems);
    }
}
bool NoteEditor::isSelected(std::uint64_t id) const noexcept {
    return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}
void NoteEditor::select(std::vector<std::uint64_t> ids) {
    typedVelocity_.clear();
    if (ids.empty())
        otherSelected_.clear();
    selected_ = std::move(ids);
    repaint();
}
void NoteEditor::prune() {
    const auto *clip = panel.details();
    std::erase_if(selected_, [clip](std::uint64_t id) {
        return clip == nullptr || note_edits_detail::find(*clip, id) == nullptr;
    });
    // The other clips' selected notes go with their notes and their clips.
    for (auto &entry : otherSelected_) {
        const engine::ClipDetails *of = nullptr;
        for (const auto &layer : panel.others())
            if (layer.details.id == entry.first)
                of = &layer.details;
        std::erase_if(entry.second, [of](std::uint64_t id) { return of == nullptr || note_edits_detail::find(*of, id) == nullptr; });
    }
    std::erase_if(otherSelected_, [](const auto &entry) { return entry.second.empty(); });
    if (scaleFolded_ && !scaleOn())
        scaleFolded_ = false;
    if (folded_ || scaleFolded_ || keys.isFolded())
        refold();
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
    if (drag_ == Drag::Stretch) {
        if (std::find(stretchIds_.begin(), stretchIds_.end(), note.id) == stretchIds_.end())
            return out;
        if (const auto *clip = panel.details()) {
            const auto window = ClipWindow::of(*clip);
            const auto a = window.toSource(stretchMap(window.toArrangement(note.startTicks)));
            const auto b = window.toSource(stretchMap(window.toArrangement(note.startTicks + note.durationTicks)));
            out.startTicks = std::min(a, b);
            out.durationTicks = std::max(kShortestNote, std::abs(b - a));
        }
        return out;
    }
    if (drag_ == Drag::Trim) {
        for (const auto &[id, length] : trimPreview_)
            if (id == note.id)
                out.durationTicks = length;
        return out;
    }
    if (drag_ == Drag::Shape || drag_ == Drag::Handle || drag_ == Drag::Velocity || drag_ == Drag::LaneDraw) {
        for (const auto &[id, value] : shapePreview_)
            if (id == note.id)
                out.velocity = static_cast<std::uint8_t>(value);
        return out;
    }
    if (drag_ == Drag::None || drag_ == Drag::Band || !isSelected(note.id))
        return out;
    return dragged(note);
}
engine::ClipNote NoteEditor::shownIn(const engine::ClipNote &note, std::int64_t clip) const noexcept {
    const auto &ids = selectedIn(clip);
    const bool moving = drag_ == Drag::Move || drag_ == Drag::Start || drag_ == Drag::End;
    if (!moving || panel.focus() || std::find(ids.begin(), ids.end(), note.id) == ids.end())
        return note;
    return dragged(note);
}
engine::ClipNote NoteEditor::dragged(const engine::ClipNote &note) const noexcept {
    auto out = note;
    if (drag_ == Drag::Move) {
        out.startTicks += dragTicks_;
        out.key = static_cast<std::uint8_t>(std::clamp(static_cast<int>(note.key) + dragKeys_, 0, kNoteKeys - 1));
    } else if (drag_ == Drag::Start) {
        const auto by = std::min(dragTicks_, std::max<std::int64_t>(0, note.durationTicks - kShortestNote));
        out.startTicks += by;
        out.durationTicks -= by;
    } else if (drag_ == Drag::End) {
        out.durationTicks = std::max(kShortestNote, note.durationTicks + dragTicks_);
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
    // Live's overlap rule: a note placed over another on its key survives; the older is cut or removed.
    if (const auto *clip = panel.details())
        edit = withOverlaps(*clip, std::move(edit));
    for (const auto &layer : panel.others())
        edit = withOverlaps(layer.details, std::move(edit));
    // One batch, one undo step; the clip is read again at once, so the next event sees it.
    if (!panel.root.submitEdits(std::move(edit.ops)))
        return false;
    if (!edit.select.empty() || !selected_.empty())
        selected_ = std::move(edit.select);
    panel.reread();
    return true;
}
void NoteEditor::mouseDown(const juce::MouseEvent &e) {
    grabKeyboardFocus();
    drag_ = Drag::None;
    copying_ = false;
    dragTicks_ = 0;
    dragKeys_ = 0;
    typedVelocity_.clear();
    from_ = to_ = e.getPosition();
    const auto *clip = panel.details();
    if (clip != nullptr && e.mods.isPopupMenu()) {
        if (keysArea().contains(from_)) {
            // The piano ruler's own menu: how the notes are spelled.
            juce::Component::SafePointer<NoteEditor> piano(this);
            spellingMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [piano](int item) {
                if (piano != nullptr && item > 0)
                    piano->chooseSpelling(item);
            });
            return;
        }
        // A right-click: a note under it that is not selected is selected first; then the menu.
        contextClick(from_);
        juce::Component::SafePointer<NoteEditor> safe(this);
        contextMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe](int item) {
            if (safe != nullptr && item > 0)
                safe->chooseContextItem(item);
        });
        return;
    }
    if (clip == nullptr)
        return;
    if (std::uint64_t picked = 0; noteAt(from_) == nullptr) {
        if (const auto other = otherClipAt(from_, &picked); other != 0 && picked != 0 && e.mods.isShiftDown() && !panel.focus()) {
            auto &ids = otherIds(other);
            if (std::find(ids.begin(), ids.end(), picked) != ids.end())
                std::erase(ids, picked);
            else
                ids.push_back(picked);
            repaint();
            return;
        } else if (other != 0) {
            // Another clip's loop bar or note: that clip becomes the one being edited (manual 10.8).
            if (panel.activate(other) && picked != 0)
                selected_ = {picked};
            repaint();
            return;
        }
    }
    if (!e.mods.isShiftDown())
        panel.clearFilters(); // a plain click in the editor lets every applied filter go (manual 10.5.3)
    if (const auto part = rulerPartAt(from_); part != RulerPart::None) {
        // A ruler handle: it is selected, for the arrows, and a drag begins.
        selected_.clear();
        otherSelected_.clear();
        head_.reset();
        picked_ = part == RulerPart::LoopStart || part == RulerPart::LoopEnd ? RulerPart::Loop : part;
        rulerDrag_ = part;
        loopPreview_.reset();
        markerPreview_.reset();
        drag_ = Drag::Ruler;
        repaint();
        return;
    }
    picked_ = RulerPart::None; // anything else pressed lets the brace or the marker go
    if (rulerArea().contains(from_)) {
        // The time ruler, as the arrangement's: a drag scrolls sideways and zooms up and down.
        viewLast_ = from_;
        drag_ = Drag::Zoom;
        return;
    }
    if (const auto piano = keysArea(); piano.contains(from_)) {
        // A piano key selects its key track's notes; with Shift it adds them, or takes them out
        // when all of them are in (manual 10.5.2).
        if (const auto key = keys.keyAt(from_.y - piano.getY())) {
            std::vector<std::uint64_t> track;
            for (const auto &n : clip->notes)
                if (n.key == *key)
                    track.push_back(n.id);
            if (!e.mods.isShiftDown()) {
                otherSelected_.clear();
                selected_ = track;
            } else {
                const bool all = !track.empty() && std::all_of(track.begin(), track.end(), [this](std::uint64_t id) { return isSelected(id); });
                for (const auto id : track)
                    if (all)
                        std::erase(selected_, id);
                    else if (!isSelected(id))
                        selected_.push_back(id);
            }
            head_.reset();
        }
        viewLast_ = from_;
        zoomFrom_ = from_.x;
        drag_ = Drag::Keys;
        repaint();
        return;
    }
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
    if (laneDivider().contains(from_)) {
        laneFrom_ = velocityArea().getHeight();
        drag_ = Drag::LaneSize;
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
        if (drawMode_ && from_.x >= lane.getX()) {
            // Draw Mode in the lane (manual 10.5.12.1): the stroke sets the velocities it crosses.
            shapePreview_.clear();
            laneLast_ = from_;
            drag_ = Drag::LaneDraw;
            laneDrawStroke(e.mods);
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
        if (best != nullptr && e.mods.isShiftDown()) {
            // Shift adds the marker to the selection, or takes it out (manual 10.5.12).
            if (isSelected(best->id))
                std::erase(selected_, best->id);
            else
                selected_.push_back(best->id);
        } else if (best != nullptr) {
            if (!isSelected(best->id))
                selected_ = {best->id};
            anchor_ = best->id;
            beginVelocityDrag(false);
        }
        repaint();
        return;
    }
    if (const auto part = stretchPartAt(from_); part != StretchPart::None) {
        const auto span = *stretchSpan();
        stretchIds_ = stretchTargets();
        stretchA_ = span.first;
        stretchB_ = span.second;
        // The place grabbed between the two, on the nearest grid line unless snapping is bypassed.
        auto grabbed = time.tick(static_cast<double>(from_.x - notesArea().getX()));
        if (snapping(e.mods)) {
            const auto below = time.snap(grabbed, false), above = time.snap(grabbed, true);
            grabbed = grabbed - below <= above - grabbed ? below : above;
        }
        stretchP_ = std::clamp(grabbed, span.first + 1, span.second - 1);
        stretchNow_ = part == StretchPart::Start ? span.first : part == StretchPart::End ? span.second : stretchP_;
        stretchPart_ = part;
        drag_ = Drag::Stretch;
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
        head_.reset();
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
#if JUCE_MAC
            const bool velocity = e.mods.isCommandDown();
#else
            const bool velocity = e.mods.isAltDown();
#endif
            // Alt (Cmd on the Mac) held at the press: a vertical drag changes velocity (manual 10.5.12).
            if (velocity && !e.mods.isShiftDown())
                beginVelocityDrag(true);
        }
    } else if (notesArea().contains(from_)) {
        if (!e.mods.isShiftDown()) {
            selected_.clear();
            otherSelected_.clear();
            // The insert marker goes where the click is, on the grid (manual 10.5.2).
            const auto at = time.tick(static_cast<double>(from_.x - notesArea().getX()));
            insert_ = snapping(e.mods) ? time.snap(at, false) : at;
            head_.reset();
        }
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
        if (!panel.focus())
            for (const auto &layer : panel.others())
                for (const auto id : selectedIn(layer.details.id))
                    if (const auto *n = note_edits_detail::find(layer.details, id)) {
                        low = std::min(low, static_cast<int>(n->key));
                        high = std::max(high, static_cast<int>(n->key));
                    }
        dragKeys_ = a && b && low <= high ? std::clamp(*b - *a, -low, kNoteKeys - 1 - high) : 0;
    } else if (drag_ == Drag::Velocity) {
        velocityDrag();
    } else if (drag_ == Drag::LaneDraw) {
        laneDrawStroke(e.mods);
    } else if (drag_ == Drag::LaneSize) {
        setLaneHeight(laneFrom_ + (from_.y - to_.y));
    } else if (drag_ == Drag::Stretch) {
        // The marker follows the pointer, on the nearest grid line unless snapping is bypassed; the
        // marker of a drag begun between the two stays between them.
        const auto raw = time.tick(static_cast<double>(std::max(0, to_.x - area.getX())));
        auto at = raw;
        if (snapping(e.mods)) {
            const auto below = time.snap(raw, false), above = time.snap(raw, true);
            at = raw - below <= above - raw ? below : above;
        }
        stretchNow_ = stretchPart_ == StretchPart::Middle ? std::clamp(at, stretchA_ + 1, stretchB_ - 1) : at;
    } else if (drag_ == Drag::Zoom) {
        // Sideways scrolls; down zooms in and up out, around the pointer.
        time.pan(static_cast<double>(viewLast_.x - to_.x));
        time.zoom(std::pow(1.012, static_cast<double>(to_.y - viewLast_.y)), static_cast<double>(to_.x - rulerArea().getX()));
        viewLast_ = to_;
        panel.root.arrangement.followPaused = true;
        panel.repaint();
    } else if (drag_ == Drag::Keys) {
        // The piano dragged: up and down scrolls it; sideways zooms the key tracks, a step every six pixels.
        keys.scrollBy(viewLast_.y - to_.y, area.getHeight());
        if (const int steps = (to_.x - zoomFrom_) / 6; steps != 0) {
            keys.zoom(keys.keyHeight + steps, to_.y - area.getY(), area.getHeight());
            zoomFrom_ += steps * 6;
        }
        viewLast_ = to_;
    } else if (drag_ == Drag::Ruler) {
        rulerDrag(e.mods);
    } else if (drag_ == Drag::Shape) {
        shapeStroke();
    } else if (drag_ == Drag::Trim) {
        trimStroke();
    } else if (drag_ == Drag::Handle) {
        handleDrag();
    } else if (drag_ == Drag::Band) {
        // The notes the rectangle touches, added to what Shift kept.
        const juce::Rectangle<int> band(from_, to_);
        if (!e.mods.isShiftDown() && insert_) {
            // And the time it spans, from the insert marker, on the grid (manual 10.5.2).
            const auto at = time.tick(static_cast<double>(std::max(0, to_.x - area.getX())));
            head_ = snapping(e.mods) ? time.snap(at, to_.x > from_.x) : at;
            if (*head_ == *insert_)
                head_.reset();
        }
        if (!e.mods.isShiftDown())
            otherSelected_.clear();
        if (!panel.focus())
            for (const auto &layer : panel.others())
                for (const auto &n : layer.details.notes)
                    if (const auto box = boxOf(n, layer.details);
                        box && band.toDouble().intersects(juce::Rectangle<double>(box->x, static_cast<double>(box->y), box->width,
                                                                                 static_cast<double>(box->height)))) {
                        auto &ids = otherIds(layer.details.id);
                        if (std::find(ids.begin(), ids.end(), n.id) == ids.end())
                            ids.push_back(n.id);
                    }
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
    const int keysBy = std::exchange(dragKeys_, 0);
    const auto *clip = panel.details();
    repaint();
    if (clip == nullptr)
        return;
    const bool copy = std::exchange(copying_, false);
    if (kind == Drag::Move && copy && (ticks != 0 || keysBy != 0))
        commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) {
            return copyNotes(of, ids, ticks, keysBy, ids.size() == 1 ? "Copy Note" : "Copy Notes");
        }));
    else if (kind == Drag::Move)
        commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return moveNotes(of, ids, ticks, keysBy); }));
    else if (kind == Drag::Start)
        commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return trimNotes(of, ids, ticks); }));
    else if (kind == Drag::End)
        commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return resizeNotes(of, ids, ticks); }));
    else if (kind == Drag::Velocity) {
        const auto values = std::exchange(shapePreview_, {});
        commit(drawVelocities(*clip, values, "Change Velocity"));
    } else if (kind == Drag::LaneDraw) {
        const auto values = std::exchange(shapePreview_, {});
        const auto keep = selected_;
        if (commit(drawVelocities(*clip, values)))
            select(keep);
    } else if (kind == Drag::Shape) {
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
    } else if (kind == Drag::LaneSize) {
        panel.keepSettings();
    } else if (kind == Drag::Stretch) {
        const auto window = ClipWindow::of(*clip);
        const auto ids = std::exchange(stretchIds_, {});
        const auto timed = timeSelection().has_value() && selected_.empty();
        const auto part = stretchPart_;
        auto stretched = remapNotes(*clip, ids, [&](std::int64_t source) { return window.toSource(stretchMap(window.toArrangement(source))); });
        const auto a = part == StretchPart::Start ? stretchNow_ : stretchA_, b = part == StretchPart::End ? stretchNow_ : stretchB_;
        stretchPart_ = StretchPart::None;
        const auto keep = selected_;
        if (commit(std::move(stretched))) {
            // The selection it began from stays: the notes, or the time, now as long as the pair.
            select(keep);
            if (timed)
                selectTime(std::min(a, b), std::max(a, b));
        }
    } else if (kind == Drag::Ruler) {
        const auto part = std::exchange(rulerDrag_, RulerPart::None);
        const auto loop = std::exchange(loopPreview_, std::nullopt);
        const auto marker = std::exchange(markerPreview_, std::nullopt);
        if (loop)
            panel.commit(setClipLoop(*clip, loop->first, loop->second));
        else if (marker)
            panel.commit(part == RulerPart::StartMarker ? setClipStart(*clip, *marker) : setClipEnd(*clip, *marker));
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
    if (rulerArea().contains(e.getPosition()) || keysArea().contains(e.getPosition())) {
        zoomToNotes();
        return;
    }
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
    if (rulerKey(k))
        return true;
    {
        // A velocity typed for the selected notes (manual 10.5.12): digits, then Enter. A 0 typed
        // first is the deactivate key, as before.
        const int digit = code >= '0' && code <= '9'                                              ? code - '0'
                          : code >= juce::KeyPress::numberPad0 && code <= juce::KeyPress::numberPad9 ? code - juce::KeyPress::numberPad0
                                                                                                     : -1;
        if (digit >= 0 && !mods.isAnyModifierKeyDown() && !selected_.empty() && (digit != 0 || typedVelocity_.isNotEmpty())) {
            if (typedVelocity_.length() < 3)
                typedVelocity_ << digit;
            repaint();
            return true;
        }
        if (typedVelocity_.isNotEmpty()) {
            const auto typed = std::exchange(typedVelocity_, {});
            repaint();
            if (code == juce::KeyPress::returnKey) {
                std::vector<std::pair<std::uint64_t, int>> values;
                for (const auto id : selected_)
                    values.emplace_back(id, typed.getIntValue());
                commit(drawVelocities(*clip, values, "Set Velocity"));
                return true;
            }
            if (code == juce::KeyPress::backspaceKey) {
                typedVelocity_ = typed.dropLastCharacters(1);
                return true;
            }
            if (code == juce::KeyPress::escapeKey)
                return true;
        }
        // Ctrl/Cmd and up or down: the selected notes' velocity by 10; with Shift, by 1.
        if (mods.isCommandDown() && !mods.isAltDown() && anySelected() &&
            (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)) {
            const int by = (code == juce::KeyPress::upKey ? 1 : -1) * (mods.isShiftDown() ? 1 : 10);
            commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return changeVelocities(of, ids, by); }));
            return true;
        }
    }
    // Time inside the clip (manual 10.7): Crop Clip, and the time commands over a time selection.
    if (k == juce::KeyPress('j', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        crop();
        return true;
    }
    if (timeSelection()) {
        const bool both = mods.isCommandDown() && mods.isShiftDown() && !mods.isAltDown();
        if (k == juce::KeyPress('d', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
            return timeCommand(TimeCommand::Duplicate), true;
        if (both && (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey))
            return timeCommand(TimeCommand::Delete), true;
        if (k == juce::KeyPress('i', juce::ModifierKeys::commandModifier, 0))
            return timeCommand(TimeCommand::Insert), true;
    }
    // Delete is the editor's while it has the keyboard: with no note selected it does nothing,
    // and never reaches the arrangement's selection.
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) {
        if (anySelected())
            commit(across([](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return deleteNotes(of, ids); }));
        return true;
    }
    // Quantize, Split or Chop, and Join are the editor's while it has the keyboard.
    if (k == juce::KeyPress('u', juce::ModifierKeys::commandModifier, 0)) {
        quantize();
        return true;
    }
    if (k == juce::KeyPress('u', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        // Quantize Settings: shown on the tools panel's Functions tab.
        panel.setToolsTab(1);
        return true;
    }
    if (k == juce::KeyPress('e', juce::ModifierKeys::commandModifier, 0)) {
        splitOrChop();
        return true;
    }
    if (k == juce::KeyPress('j', juce::ModifierKeys::commandModifier, 0)) {
        join();
        return true;
    }
    // Cut, Copy and Paste are the editor's while it has the keyboard (manual 10.5.4).
    if (k == juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0)) {
        copySelected();
        return true;
    }
    if (k == juce::KeyPress('x', juce::ModifierKeys::commandModifier, 0)) {
        cutSelected();
        return true;
    }
    if (k == juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)) {
        paste();
        return true;
    }
    if (k == juce::KeyPress('d', juce::ModifierKeys::commandModifier, 0)) {
        // Duplicate is the editor's while it has the keyboard, with or without notes selected.
        duplicate();
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'n' || code == 'N') && !panel.others().empty()) {
        panel.setFocus(!panel.focus());
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
    if (!mods.isAnyModifierKeyDown() && (code == 'g' || code == 'G')) {
        // Fold to Scale: only with a scale of the clip's own.
        setScaleFold(!scaleFolded_);
        panel.repaint();
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'k' || code == 'K')) {
        highlightScale = !highlightScale;
        repaint();
        panel.repaint();
        panel.keepSettings();
        return true;
    }
    if (code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey) {
        const bool up = code == juce::KeyPress::pageUpKey;
        const auto view = notesArea();
        if (mods.isCommandDown()) {
            time.pan((up ? -0.8 : 0.8) * view.getWidth());
            panel.root.arrangement.followPaused = true;
        } else {
            keys.page(up, mods.isShiftDown(), view.getHeight());
        }
        repaint();
        return true;
    }
    if (mods.isCommandDown() && !mods.isShiftDown() && !mods.isAltDown() && code >= '1' && code <= '5') {
        gridKey(code - '0');
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'w' || code == 'W')) {
        fitWidth();
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (code == 'h' || code == 'H')) {
        fitHeight();
        return true;
    }
    if (!mods.isCommandDown() && !mods.isAltDown() && (code == '+' || code == '=' || code == '-')) {
        zoomTime(code != '-');
        return true;
    }
    if (k == juce::KeyPress('e', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)) {
        // The Clip View at its full height, and back (manual 10.2).
        panel.root.toggleClipViewFull();
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
        // With several clips shown and Focus off, every clip's notes.
        otherSelected_.clear();
        if (!panel.focus())
            for (const auto &layer : panel.others())
                for (const auto &n : layer.details.notes)
                    otherIds(layer.details.id).push_back(n.id);
        repaint();
        return true;
    }
    if (code == juce::KeyPress::escapeKey && (anySelected() || timeSelection())) {
        // Esc collapses the note selection and the time selection (manual 10.5.2).
        head_.reset();
        select({});
        return true;
    }
    if (code == juce::KeyPress::returnKey && !mods.isAnyModifierKeyDown())
        return toggleTimeAndNotes();
    if (k == juce::KeyPress('f', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        // Show or hide the note filters (as measured on Live: Ctrl+Shift+F is the filter bar's key).
        panel.showFilters(!panel.filtersShown());
        return true;
    }
    if (k == juce::KeyPress('a', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        invertSelection();
        return true;
    }
    {
        const bool sideways = code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
        const bool upDown = code == juce::KeyPress::upKey || code == juce::KeyPress::downKey;
        const bool forward = code == juce::KeyPress::rightKey || code == juce::KeyPress::downKey;
        const auto window = ClipWindow::of(*clip);
        // Home and End take the insert marker to the clip's start and end.
        if ((code == juce::KeyPress::homeKey || code == juce::KeyPress::endKey) && !mods.isAnyModifierKeyDown()) {
            placeMarker(code == juce::KeyPress::homeKey ? window.pos : window.end(), false);
            return true;
        }
        // Alt and the arrows walk the notes and their edges. The manual's shortcut table gives Alt on
        // Windows and Option on the Mac; its text says Ctrl on Windows, which the table gives to velocity.
        if (mods.isAltDown() && !mods.isCommandDown() && (sideways || upDown))
            return walk(sideways, forward, mods.isShiftDown());
        // With no note selected, left and right move the insert marker by the grid; with Shift they
        // select time from it; over a time selection, an arrow alone collapses it to that side.
        if (sideways && !anySelected() && !mods.isCommandDown() && !mods.isAltDown()) {
            const bool extend = mods.isShiftDown();
            if (const auto span = timeSelection(); span && !extend) {
                placeMarker(forward ? span->second : span->first, false);
                return true;
            }
            if (extend && !insert_)
                insert_ = window.pos;
            const auto from = extend ? head_.value_or(*insert_) : insert_.value_or(window.pos);
            placeMarker(std::clamp(time.nextGridLine(from, forward), window.pos, window.end()), extend);
            return true;
        }
    }
    // 0 deactivates the selected notes, or activates them when all are off (manual 10.5.7).
    if (code == '0' && !mods.isAnyModifierKeyDown() && anySelected()) {
        commit(across([](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return toggleNotesActive(of, ids); }));
        return true;
    }
    if (mods.isCommandDown() || mods.isAltDown() || !anySelected())
        return false;
    // The arrows move the selected notes by the grid and by a key; with Shift, up and down move an
    // octave and left and right change the length (manual 10.5.4, 10.5.5).
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
        const auto by = code == juce::KeyPress::rightKey ? gridTicks() : -gridTicks();
        const bool lengthen = mods.isShiftDown();
        commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) {
            return lengthen ? resizeNotes(of, ids, by) : moveNotes(of, ids, by, 0);
        }));
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
        const int by = (code == juce::KeyPress::upKey ? 1 : -1) * (mods.isShiftDown() ? 12 : 1);
        commit(across([&](const engine::ClipDetails &of, const std::vector<std::uint64_t> &ids) { return moveNotes(of, ids, 0, by); }));
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
            // From the first shown clip's start to the last one's end (manual 10.8, the Arrangement).
            const auto window = ClipWindow::of(*clip);
            auto from = window.pos, to = window.end();
            auto every = clip->notes;
            for (const auto &layer : panel.others()) {
                const auto other = ClipWindow::of(layer.details);
                from = std::min(from, other.pos);
                to = std::max(to, other.end());
                every.insert(every.end(), layer.details.notes.begin(), layer.details.notes.end());
            }
            const auto length = std::max<std::int64_t>(textproj::kPPQ, to - from);
            time.fit(from, from + length + length / 24, area.getWidth(), false);
            // Live opens on the notes; an empty clip opens around middle C.
            folded_ = false;
            insert_.reset();
            head_.reset();
            keys.unfold(area.getHeight());
            keys.keyHeight = KeyTracks::kDefaultHeight;
            const auto range = keyRange(every);
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
    else
        panel.root.arrangement.followPaused = true; // a scroll or zoom in time pauses Follow
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
    g.fillRect(0, 0, kKeysWidth, barsHeight() + kRulerHeight + kStretchHeight);
    const auto *clip = panel.details();
    if (barsHeight() > 0) {
        // The loop bars: each clip's in its colour, the edited clip's outlined; with Focus on the
        // others are grey.
        g.fillRect(kKeysWidth, 0, getWidth() - kKeysWidth, barsHeight());
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(kKeysWidth, 0, getWidth() - kKeysWidth, barsHeight());
        for (std::size_t i = 0; i <= panel.others().size(); ++i) {
            const auto bar = loopBar(i);
            const auto own = i == 0 ? panel.colour() : panel.others()[i - 1].colour;
            g.setColour(i != 0 && panel.focus() ? juce::Colour(0xff8a8a8a) : own);
            g.fillRect(bar);
            if (i == 0) {
                g.setColour(juce::Colours::black);
                g.drawRect(bar, 1);
            }
        }
    }
    if (clip == nullptr || time.meters == nullptr) {
        g.fillRect(ruler);
        return;
    }
    const auto &meters = *time.meters;
    paintRuler(g, ruler, time, meters);
    // The stretch markers' strip, under the ruler.
    g.setColour(look::lane.darker(0.25f));
    g.fillRect(stretchArea());
    if (const auto brace = loopBrace()) {
        // The loop brace (manual 8.2.2): over the loop's window, lit while the clip loops.
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(ruler);
        const auto lit = picked_ == RulerPart::Loop ? juce::Colours::white : look::textOnBand;
        g.setColour(clip->loopEnabled ? lit : lit.withAlpha(0.35f));
        g.fillRect(brace->getStart(), ruler.getY() + 1, std::max(1, brace->getLength()), 3);
        g.fillRect(brace->getStart(), ruler.getY() + 1, 2, 6);
        g.fillRect(brace->getEnd() - 2, ruler.getY() + 1, 2, 6);
    }
    {
        // The clip's Start and End markers (manual 8.8): flags along the ruler's foot, pointing into the clip.
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(ruler);
        const auto foot = static_cast<float>(ruler.getBottom());
        for (const bool end : {false, true}) {
            const auto x = static_cast<float>(markerX(end));
            juce::Path flag;
            flag.addTriangle(x, foot - 8.f, x + (end ? -6.f : 6.f), foot - 4.f, x, foot);
            g.setColour(picked_ == (end ? RulerPart::EndMarker : RulerPart::StartMarker) ? juce::Colours::white : look::textOnBand);
            g.fillPath(flag);
        }
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
            if (highlightScale && clip->scale.local && inScale(key, static_cast<int>(clip->scale.root), clip->scale.mask)) {
                // Highlight Scale (manual 10.6): the scale's key tracks are tinted.
                g.setColour(look::accent.withAlpha(0.13f));
                g.fillRect(area.getX(), y, area.getWidth(), keys.keyHeight);
            }
            if (key % 12 == 0 && !keys.isFolded()) {
                g.setColour(look::lane.darker(0.25f));
                g.fillRect(area.getX(), y + keys.keyHeight - 1, area.getWidth(), 1);
            }
        }
        paintGrid(g, area, time, meters);
        shadeOutside(g, area, time, window);
        // The notes: the clip's colour, lighter as the velocity falls; a deactivated note is grey
        // (manual 10.5.7).
        // The other shown clips' notes, under the edited clip's: each in its own clip's colour, or
        // grey while Focus is on (manual 10.8).
        for (const auto &layer : panel.others())
            for (const auto &stored : layer.details.notes) {
                const auto note = shownIn(stored, layer.details.id);
                const auto box = boxOf(note, layer.details);
                if (!box || box->x > static_cast<double>(area.getRight()) || box->x + box->width < area.getX() ||
                    box->y >= area.getBottom() || box->y + box->height <= area.getY())
                    continue;
                const juce::Rectangle<float> r(static_cast<float>(box->x), static_cast<float>(box->y),
                                               static_cast<float>(box->width), static_cast<float>(box->height));
                g.setColour(panel.focus() || !note.active ? juce::Colour(0xffa4a4a4) : layer.colour);
                g.fillRect(r);
                g.setColour(look::outline.withAlpha(0.7f));
                g.drawRect(r, 1.f);
                const auto &ids = selectedIn(layer.details.id);
                if (!panel.focus() && std::find(ids.begin(), ids.end(), note.id) != ids.end() && r.getWidth() > 4.f) {
                    g.setColour(juce::Colours::white);
                    g.drawRect(r.reduced(1.f), 1.f);
                }
            }
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
            const auto fill = noteFill(note);
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
        if (const auto span = timeSelection()) {
            // The time selection: a lighter band down the key tracks.
            const int left = area.getX() + static_cast<int>(std::clamp(time.x(span->first), -1e6, 1e6));
            const int right = area.getX() + static_cast<int>(std::clamp(time.x(span->second), -1e6, 1e6));
            g.setColour(juce::Colours::white.withAlpha(0.16f));
            g.fillRect(juce::Rectangle<int>(left, area.getY(), std::max(1, right - left), area.getHeight()).getIntersection(area));
        } else if (insert_) {
            // The insert marker: a thin line down the key tracks.
            const int x = area.getX() + static_cast<int>(std::clamp(time.x(*insert_), -1e6, 1e6));
            g.setColour(look::text);
            g.fillRect(x, area.getY(), 1, area.getHeight());
        }
        if (const auto pair = stretchSpan()) {
            // The Note Stretch markers: a pair of downward pointers at the selection's start and end.
            const auto strip = stretchArea();
            const auto top = static_cast<float>(strip.getY());
            for (const auto tick : {pair->first, pair->second}) {
                const auto x = static_cast<float>(area.getX()) + static_cast<float>(std::clamp(time.x(tick), -1e6, 1e6));
                juce::Path pointer;
                pointer.addTriangle(x - 4.f, top + 1.f, x + 4.f, top + 1.f, x, top + static_cast<float>(kStretchHeight));
                g.setColour(juce::Colours::white);
                g.fillPath(pointer);
                g.setColour(look::outline);
                g.strokePath(pointer, juce::PathStrokeType(1.f));
            }
            if (drag_ == Drag::Stretch && stretchPart_ == StretchPart::Middle) {
                const int x = area.getX() + static_cast<int>(std::clamp(time.x(stretchNow_), -1e6, 1e6));
                g.setColour(juce::Colours::white.withAlpha(0.7f));
                g.fillRect(x, area.getY(), 1, area.getHeight());
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
            if (clip->scale.local && inScale(key, static_cast<int>(clip->scale.root), clip->scale.mask)) {
                // Scale Mode (manual 8.2.4): the scale's keys are marked on the piano; with
                // Highlight Scale, the root more strongly.
                const bool tonic = (key % 12 + 12 - static_cast<int>(clip->scale.root) % 12) % 12 == 0;
                g.setColour(look::accent.withAlpha(highlightScale && tonic ? 0.9f : 0.45f));
                g.fillRect(keyboard.getX(), y + 1, highlightScale && tonic ? 6 : 4, std::max(1, keys.keyHeight - 2));
            }
            if ((key % 12 == 0 || keys.isFolded()) && keys.keyHeight >= 9) {
                const juce::Rectangle<int> name(keyboard.getRight() - 26, y, 24, keys.keyHeight);
                labels_.push_back(name);
                g.setColour(blackKey(key) ? juce::Colours::white : look::text);
                g.drawText(named(key), name, juce::Justification::centredRight);
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
        // The selected notes' velocity, or their range, or the value being typed (manual 10.5.12).
        const auto readout = readoutBox();
        labels_.push_back(readout);
        g.setColour(typedVelocity_.isNotEmpty() ? juce::Colours::white : look::text);
        g.setFont(look::font(9.5f));
        g.drawText(velocityReadout(), readout, juce::Justification::centredLeft);
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
// ---- Find and Select Notes (manual 10.5.3) -----------------------------------------------------
juce::Rectangle<int> ClipViewPanel::filterArea() const noexcept {
    const auto area = editorArea();
    return midi() && filtersShown_ ? area.withHeight(std::min(kStripHeight, area.getHeight())) : juce::Rectangle<int>();
}
juce::Rectangle<int> ClipViewPanel::findBox() const noexcept {
    const auto highlight = highlightBox();
    return highlight.isEmpty() ? highlight : highlight.withX(highlight.getRight() + 4).withWidth(34);
}
void ClipViewPanel::selectorDrag(double clipShare, int downBy) {
    if (!details_)
        return;
    auto &time = midi() ? notes.time : sample.time;
    const int width = midi() ? notes.notesArea().getWidth() : sample.getWidth();
    time.left = std::max(0., time.left + clipShare * static_cast<double>(details_->lengthTicks.value_or(0)) / textproj::kPPQ);
    if (downBy != 0)
        time.zoom(std::pow(1.012, static_cast<double>(downBy)), width / 2.);
    // A scroll of the user's pauses Follow, as in the editor itself.
    root.arrangement.followPaused = true;
    notes.repaint();
    sample.repaint();
    repaint();
}
std::pair<std::int64_t, std::int64_t> ClipViewPanel::shownSpan() const noexcept {
    const auto &time = midi() ? notes.time : sample.time;
    const int width = midi() ? notes.notesArea().getWidth() : sample.getWidth();
    return {time.tick(0.), time.tick(static_cast<double>(std::max(0, width)))};
}
void ClipViewPanel::applyState(const WindowState &state) {
    notes.time.grid = {state.noteGridSnap, state.noteGridFixed, state.noteGridTriplet, state.noteGridNarrow, state.noteGridFixedQuarters};
    notes.highlightScale = state.highlightScale;
    notes.setLaneHeight(state.noteLaneHeight);
    notes.spelling = static_cast<NoteSpelling>(std::clamp(state.noteSpelling, 0, 4));
    notes.noteColours = static_cast<NoteEditor::NoteColours>(std::clamp(state.noteColours, 0, 4));
    quantizeValue_ = static_cast<std::size_t>(std::clamp(state.quantizeValue, 0, static_cast<int>(kQuantizeValues.size()) - 1));
    quantizeAmount = state.quantizeAmount;
    quantizeSwing = state.quantizeSwing;
    quantizeStarts = state.quantizeStarts;
    quantizeEnds = state.quantizeEnds;
}
void ClipViewPanel::keepSettings() {
    auto &state = root.state();
    const auto &grid = notes.time.grid;
    state.noteGridSnap = grid.snap;
    state.noteGridFixed = grid.fixed;
    state.noteGridTriplet = grid.triplet;
    state.noteGridNarrow = grid.narrow;
    state.noteGridFixedQuarters = grid.fixedQuarters;
    state.highlightScale = notes.highlightScale;
    state.noteLaneHeight = notes.laneHeight();
    state.noteSpelling = static_cast<int>(notes.spelling);
    state.noteColours = static_cast<int>(notes.noteColours);
    state.quantizeValue = static_cast<int>(quantizeValue_);
    state.quantizeAmount = quantizeAmount;
    state.quantizeSwing = quantizeSwing;
    state.quantizeStarts = quantizeStarts;
    state.quantizeEnds = quantizeEnds;
    root.persist();
}
juce::Rectangle<int> ClipViewPanel::gridBox() const noexcept {
    return midi() ? headerArea().withTrimmedRight(8).removeFromRight(70) : juce::Rectangle<int>();
}
juce::PopupMenu ClipViewPanel::gridMenu(int idOffset) const {
    using Geometry = ArrangementGeometry;
    const auto &grid = notes.time.grid;
    juce::PopupMenu menu, adaptive, fixed;
    for (std::size_t i = 0; i < Geometry::kAdaptiveNames.size(); ++i)
        adaptive.addItem(idOffset + static_cast<int>(i) + 1, Geometry::kAdaptiveNames[i], true,
                         !grid.fixed && grid.narrow == static_cast<int>(i) - Geometry::kGridNarrowest);
    int id = idOffset + 10;
    for (double quarters = Geometry::kWidestGrid; quarters >= Geometry::kFinestGrid; quarters /= 2)
        fixed.addItem(id++, gridLabel(quarters, false), true, grid.fixed && grid.fixedQuarters == quarters);
    menu.addSubMenu("Adaptive Grid", adaptive);
    menu.addSubMenu("Fixed Grid", fixed);
    menu.addItem(idOffset + 30, "Triplet Grid", true, grid.triplet);
    menu.addItem(idOffset + 31, "Snap to Grid", true, grid.snap);
    return menu;
}
bool ClipViewPanel::chooseGrid(int item) {
    using Geometry = ArrangementGeometry;
    auto &grid = notes.time.grid;
    if (item >= 1 && item <= static_cast<int>(Geometry::kAdaptiveNames.size())) {
        grid.fixed = false;
        grid.narrow = item - 1 - Geometry::kGridNarrowest;
    } else if (item >= 10 && item < 30) {
        double quarters = Geometry::kWidestGrid;
        for (int i = 10; i < item; ++i)
            quarters /= 2;
        if (quarters < Geometry::kFinestGrid)
            return false;
        grid.fixed = true;
        grid.fixedQuarters = quarters;
    } else if (item == 30) {
        grid.triplet = !grid.triplet;
    } else if (item == 31) {
        grid.snap = !grid.snap;
    } else {
        return false;
    }
    notes.repaint();
    repaint();
    keepSettings();
    return true;
}
juce::Rectangle<int> ClipViewPanel::coloursBox() const noexcept {
    const auto find = findBox();
    return find.isEmpty() ? find : find.withX(find.getRight() + 4).withWidth(62);
}
juce::PopupMenu ClipViewPanel::coloursMenu() const {
    juce::PopupMenu menu;
    for (std::size_t i = 0; i < NoteEditor::kNoteColourNames.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, NoteEditor::kNoteColourNames[i], true, static_cast<std::size_t>(notes.noteColours) == i);
    return menu;
}
bool ClipViewPanel::chooseColours(int item) {
    if (item < 1 || item > static_cast<int>(NoteEditor::kNoteColourNames.size()))
        return false;
    notes.noteColours = static_cast<NoteEditor::NoteColours>(item - 1);
    notes.repaint();
    repaint();
    keepSettings();
    return true;
}
juce::Rectangle<int> ClipViewPanel::filterChooserBox() const noexcept {
    const auto bar = filterArea();
    return bar.isEmpty() ? bar : juce::Rectangle<int>(bar.getX() + 6, bar.getY() + 3, 84, 13);
}
juce::Rectangle<int> ClipViewPanel::filterInvertBox() const noexcept {
    const auto bar = filterArea();
    return bar.isEmpty() ? bar : juce::Rectangle<int>(bar.getRight() - 98, bar.getY() + 3, 44, 13);
}
juce::Rectangle<int> ClipViewPanel::filterSelectBox() const noexcept {
    const auto bar = filterArea();
    return bar.isEmpty() ? bar : juce::Rectangle<int>(bar.getRight() - 50, bar.getY() + 3, 44, 13);
}
int ClipViewPanel::filterFieldCount() const noexcept {
    switch (filter_) {
    case NoteFilter::Time:
        return 3;
    case NoteFilter::Chance:
    case NoteFilter::Count:
    case NoteFilter::Duration:
    case NoteFilter::Velocity:
        return 2;
    default:
        return 0;
    }
}
const char *ClipViewPanel::filterFieldName(int slot) const noexcept {
    if (filter_ == NoteFilter::Time)
        return slot == 0 ? "Start" : slot == 1 ? "Length" : "Repeat";
    if (filter_ == NoteFilter::Count)
        return slot == 0 ? "Every" : "Offset";
    return slot == 0 ? "Min" : "Max";
}
int ClipViewPanel::filterToggleCount() const noexcept {
    return filter_ == NoteFilter::Pitch ? 12 : filter_ == NoteFilter::Condition ? 3 : filter_ == NoteFilter::Count || filter_ == NoteFilter::Scale ? 1 : 0;
}
const char *ClipViewPanel::filterToggleName(int index) const noexcept {
    static constexpr const char *kClasses[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (filter_ == NoteFilter::Pitch)
        return kClasses[std::clamp(index, 0, 11)];
    if (filter_ == NoteFilter::Condition)
        return index == 0 ? "Active" : index == 1 ? "Chance" : "Deviation";
    return filter_ == NoteFilter::Count ? "Quantized" : "Clip Scale";
}
juce::Rectangle<int> ClipViewPanel::filterToggleBox(int index) const noexcept {
    const auto chooser = filterChooserBox();
    if (chooser.isEmpty() || index < 0 || index >= filterToggleCount())
        return {};
    const int left = chooser.getRight() + 8 + filterFieldCount() * 92;
    const int width = filter_ == NoteFilter::Pitch ? 22 : filter_ == NoteFilter::Condition ? 58 : 66;
    return {left + index * (width + 2), chooser.getY(), width, chooser.getHeight()};
}
bool ClipViewPanel::filterToggle(int index) const noexcept {
    switch (filter_) {
    case NoteFilter::Pitch:
        return ((filters.pitches >> index) & 1) != 0;
    case NoteFilter::Condition:
        return index == 0 ? filters.conditionActive : index == 1 ? filters.conditionChance : filters.conditionDeviation;
    case NoteFilter::Count:
        return filters.quantized;
    case NoteFilter::Scale:
        return filters.useClipScale;
    default:
        return false;
    }
}
bool ClipViewPanel::clickFilterToggle(int index) {
    if (index < 0 || index >= filterToggleCount())
        return false;
    switch (filter_) {
    case NoteFilter::Pitch:
        filters.pitches ^= 1 << index;
        break;
    case NoteFilter::Condition:
        (index == 0 ? filters.conditionActive : index == 1 ? filters.conditionChance : filters.conditionDeviation) = !filterToggle(index);
        break;
    case NoteFilter::Count:
        filters.quantized = !filters.quantized;
        break;
    case NoteFilter::Scale:
        filters.useClipScale = !filters.useClipScale;
        break;
    default:
        break;
    }
    return applyFilter();
}
void ClipViewPanel::showFilters(bool show) {
    filtersShown_ = show && midi();
    if (!filtersShown_)
        filters.clear();
    closeEditor();
    resized();
    repaint();
}
void ClipViewPanel::clearFilters() {
    if (!filters.any())
        return;
    filters.clear();
    repaint();
}
juce::PopupMenu ClipViewPanel::filterMenu() const {
    juce::PopupMenu menu;
    for (std::size_t i = 0; i < kNoteFilterCount; ++i)
        // An applied filter has a dot beside its name.
        menu.addItem(static_cast<int>(i) + 1, juce::String(kNoteFilterNames[i]) + (filters.applied[i] ? juce::String::fromUTF8("  \xe2\x97\x8f") : juce::String()),
                     true, static_cast<std::size_t>(filter_) == i);
    return menu;
}
bool ClipViewPanel::chooseFilter(int item) {
    if (item < 1 || item > static_cast<int>(kNoteFilterCount))
        return false;
    filter_ = static_cast<NoteFilter>(item - 1);
    closeEditor();
    repaint();
    return true;
}
bool ClipViewPanel::toggleFilterInvert() {
    const auto at = static_cast<std::size_t>(filter_);
    filters.invert[at] = !filters.invert[at];
    return applyFilter();
}
bool ClipViewPanel::applyFilter() {
    if (!details_ || !midi())
        return false;
    // The bar's whole beats and sixteenths, in the clip's ticks; beats count from the clip's Start.
    filters.timeStart = details_->startMarkerTicks + static_cast<std::int64_t>(filterTimeStart) * textproj::kPPQ;
    filters.timeLength = static_cast<std::int64_t>(filterTimeLength) * textproj::kPPQ;
    filters.timeRepeat = static_cast<std::int64_t>(filterTimeRepeat) * textproj::kPPQ;
    filters.durationMin = static_cast<std::int64_t>(filterDurationMin) * textproj::kPPQ / 4;
    filters.durationMax = static_cast<std::int64_t>(filterDurationMax) * textproj::kPPQ / 4;
    filters.applied[static_cast<std::size_t>(filter_)] = true;
    notes.select(findNotes(*details_, filters, notes.gridTicks(), ClipWindow::of(*details_).toSource(0)));
    repaint();
    return true;
}
// ---- The Quantize settings (manual 10.5.11) ----------------------------------------------------
juce::Rectangle<int> ClipViewPanel::quantizeChooserBox() const noexcept {
    const auto button = toolBox(Tool::Quantize);
    if (button.isEmpty())
        return {};
    return button.withX(button.getRight() + 6);
}
juce::Rectangle<int> ClipViewPanel::quantizeStartBox() const noexcept {
    const auto button = toolBox(Tool::Quantize);
    return button.isEmpty() ? button : button.withY(button.getY() + 22);
}
juce::Rectangle<int> ClipViewPanel::quantizeEndBox() const noexcept {
    const auto start = quantizeStartBox();
    return start.isEmpty() ? start : start.withX(start.getRight() + 6);
}
juce::PopupMenu ClipViewPanel::quantizeMenu() const {
    juce::PopupMenu menu;
    for (std::size_t i = 0; i < kQuantizeValues.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, kQuantizeValues[i].name, true, i == quantizeValue_);
    return menu;
}
bool ClipViewPanel::chooseQuantize(int item) {
    if (item < 1 || item > static_cast<int>(kQuantizeValues.size()))
        return false;
    quantizeValue_ = static_cast<std::size_t>(item - 1);
    repaint();
    keepSettings();
    return true;
}
std::int64_t ClipViewPanel::quantizeGrid() const noexcept {
    const auto fixed = kQuantizeValues[quantizeValue_].ticks;
    return fixed > 0 ? fixed : notes.gridTicks();
}
QuantizeSettings ClipViewPanel::quantizeSettings() const noexcept {
    QuantizeSettings s;
    s.strength = std::clamp(quantizeAmount, 0, 100) * 100;
    s.swing = std::clamp(quantizeSwing, 0, 100) * 100;
    s.starts = quantizeStarts;
    s.ends = quantizeEnds;
    return s;
}
juce::Rectangle<int> ClipViewPanel::velocityStripArea() const noexcept {
    const auto area = editorArea();
    return midi() && area.getHeight() >= 140 ? area.withTop(area.getBottom() - kStripHeight) : juce::Rectangle<int>();
}
juce::Rectangle<int> ClipViewPanel::randomizeBox() const noexcept {
    const auto strip = velocityStripArea();
    return strip.isEmpty() ? strip : juce::Rectangle<int>(strip.getX() + 6, strip.getY() + 3, 62, 13);
}
bool ClipViewPanel::randomize() {
    if (!details_ || !midi())
        return false;
    const auto offset = [this](int amount) {
        if (randomOffset)
            return randomOffset(amount);
        return std::uniform_int_distribution<int>(-amount, amount)(random_);
    };
    return notes.apply(randomizeVelocities(*details_, notes.selected(), randomAmount, offset));
}
bool ClipViewPanel::ramp() {
    if (!details_ || !midi())
        return false;
    return notes.apply(rampVelocities(*details_, notes.selected(), rampStart, rampEnd));
}
void ClipViewPanel::numberChanged(Field field, bool typed) {
    if (field == Field::RampStart || field == Field::RampEnd)
        ramp();
    else if (field == Field::RandomAmount && typed)
        randomize();
    else if (field == Field::FilterA || field == Field::FilterB || field == Field::FilterC)
        applyFilter();
    else if (field == Field::QuantizeAmount || field == Field::QuantizeSwing)
        keepSettings();
}
void ClipViewPanel::resized() {
    notes.setBounds(editorArea().withTrimmedTop(filterArea().getHeight()).withTrimmedBottom(velocityStripArea().getHeight()));
    sample.setBounds(editorArea());
}
void ClipViewPanel::read(const SnapshotReader &reader) {
    details_.reset();
    problem_.clear();
    others_.clear();
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
    // The other selected MIDI clips (manual 10.8): up to eight clips in all; only beside a MIDI clip.
    if (midi())
        for (const auto id : shown_) {
            if (id == clip_ || others_.size() + 1 >= kMaxClips)
                continue;
            auto other = engine::readClipDetails(root.editStore(), id);
            if (other.clip && other.clip->kind == "midi" && other.clip->notesProblem.empty())
                others_.push_back(ClipLayer{std::move(*other.clip), look::trackColour(0)});
        }
}
bool ClipViewPanel::activate(std::int64_t clip) {
    const bool shown = std::any_of(others_.begin(), others_.end(), [clip](const ClipLayer &l) { return l.details.id == clip; });
    if (!shown)
        return false;
    // The view stays where it is; only the clip being edited changes.
    notes.rehome(clip_, clip);
    clip_ = clip;
    wanted_ = clip;
    closeEditor();
    read(root.reader());
    frame(root.reader());
    repaint();
    return true;
}
void ClipViewPanel::setFocus(bool on) {
    focus_ = on;
    repaint();
}
juce::Rectangle<int> ClipViewPanel::scaleFoldBox() const noexcept {
    const auto trim = trimBox(), focus = focusBox();
    return trim.isEmpty() ? trim : trim.withX((focus.isEmpty() ? trim : focus).getRight() + 4).withWidth(38);
}
juce::Rectangle<int> ClipViewPanel::highlightBox() const noexcept {
    const auto fold = scaleFoldBox();
    return fold.isEmpty() ? fold : fold.withX(fold.getRight() + 4).withWidth(54);
}
juce::Rectangle<int> ClipViewPanel::scaleModeBox() const noexcept {
    if (!midi())
        return {};
    const auto box = fieldBox(Field::Scale);
    return {box.getRight() - 24, box.getY() - 15 + 2, 24, 11};
}
bool ClipViewPanel::toggleScaleMode() {
    if (!details_ || !midi())
        return false;
    // On, every shown clip takes the edited clip's scale as its own; off, each holds none.
    const bool on = !details_->scale.local;
    const auto tonic = details_->scale.root;
    const auto id = details_->scale.id;
    const auto mask = details_->scale.mask;
    const bool done = commitForAll([=](const engine::ClipDetails &clip) {
        if (!on)
            return setClipScaleMode(clip, false);
        auto taken = setClipScale(clip, tonic, id, mask);
        for (auto &o : taken.ops)
            o.label = "Activate Scale";
        return taken;
    });
    notes.prune();
    return done;
}
juce::Rectangle<int> ClipViewPanel::focusBox() const noexcept {
    const auto trim = trimBox();
    return trim.isEmpty() || others_.empty() ? juce::Rectangle<int>() : trim.withX(trim.getRight() + 4).withWidth(40);
}
void ClipViewPanel::reread() {
    read(root.reader());
    notes.prune();
    repaint();
}
void ClipViewPanel::frame(const SnapshotReader &reader) {
    // The selected clips; with none selected, what is already shown stays, as Live's does. The clip
    // being edited stays the same while it is still among them; else it is the one clicked last.
    // Read in place: a steady frame allocates nothing (UI-ARCHITECTURE 8).
    const auto &sel = root.arrangement.geometry.selection;
    const auto selected = sel.clip;
    const bool none = selected == 0 && sel.pickedClips.empty();
    const auto picked = [&](std::int64_t id) {
        return id != 0 && (id == selected || std::find(sel.pickedClips.begin(), sel.pickedClips.end(), id) != sel.pickedClips.end());
    };
    // Whether the selected clips are the ones shown, in the order the arrangement gives them: the
    // clicked clip first, then the others picked.
    const auto sameAsShown = [&] {
        std::size_t at = 0;
        if (selected != 0 && (at >= shown_.size() || shown_[at++] != selected))
            return false;
        for (const auto id : sel.pickedClips)
            if (id != selected && (at >= shown_.size() || shown_[at++] != id))
                return false;
        return at == shown_.size();
    };
    const bool keep = picked(clip_);
    const auto wanted = keep ? clip_ : selected != 0 ? selected : !none ? sel.pickedClips.front() : clip_;
    const auto generation = root.generation();
    // Asked once for each clip and each generation: a selected clip that is gone is not read again
    // until the project moves (an Undo may bring it back).
    const bool other = wanted != wanted_ || !read_ || (!none && !sameAsShown());
    if (other || generation != generation_) {
        wanted_ = wanted;
        clip_ = wanted;
        if (!none) {
            shown_.clear();
            if (selected != 0)
                shown_.push_back(selected);
            for (const auto id : sel.pickedClips)
                if (id != selected)
                    shown_.push_back(id);
        }
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
    // The Status Bar reads the selected notes: it is drawn again when what it would read changes.
    if (const auto stamp = midi() ? notes.statusStamp() : 0; stamp != statusStamp_) {
        statusStamp_ = stamp;
        root.mark(DirtySet::Status);
    }
    // The frame's own snapshot: the clip's node and its track's colour, and the song's meters for
    // the rulers. They live until the next frame.
    notes.time.meters = &reader.meters();
    sample.time.meters = &reader.meters();
    node_ = nullptr;
    trackName_ = {};
    colour_ = look::trackColour(0);
    if (details_) {
        const auto rows = root.arrangement.geometry.rows(reader);
        if (const auto at = rows.find(details_->trackId); at < rows.size()) {
            trackName_ = rows[at].track->name;
            colour_ = details_->color ? look::paletteColour(*details_->color)
                                      : root.arrangement.colourFor(rows[at].position);
            for (const auto &c : rows[at].track->clips)
                if (c->id == clip_)
                    node_ = c.get();
        }
        for (auto &layer : others_)
            if (const auto at = rows.find(layer.details.trackId); at < rows.size())
                layer.colour = layer.details.color ? look::paletteColour(*layer.details.color)
                                                   : root.arrangement.colourFor(rows[at].position);
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
    bool sameScale = true;
    for (const auto &layer : others_) {
        out.loopMixed = out.loopMixed || layer.details.loopEnabled != d.loopEnabled;
        sameScale = sameScale && layer.details.scale.root == d.scale.root && layer.details.scale.id == d.scale.id &&
                    layer.details.scale.local == d.scale.local;
    }
    // The title: the clip's name; with several clips shown, how many (manual 10.8).
    // A clip with no name reads its track's (manual 10.8).
    out.title = !others_.empty() ? juce::String(static_cast<int>(others_.size()) + 1) + " MIDI Clips"
                : !d.name.empty() ? juce::String(d.name)
                                  : juce::String(juce::CharPointer_UTF8(trackName_.data()), juce::CharPointer_UTF8(trackName_.data() + trackName_.size()));
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
    // Root and scale are shown only when every shown clip agrees; else an asterisk (manual 10.8).
    if (!sameScale)
        out.scale = "*";
    // The Pitch and Time panel: the lowest and highest key of the notes it would act on, and the
    // transposition, which is nothing until its field is dragged or typed into.
    out.transpose = "0 st";
    {
        std::vector<engine::ClipNote> acted;
        for (const auto &n : d.notes)
            if (notes.selected().empty() || notes.isSelected(n.id))
                acted.push_back(n);
        if (const auto keys = keyRange(acted))
            out.range = juce::String(notes.named(keys->low)) + " - " + juce::String(notes.named(keys->high));
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
    if (field == Field::Interval) {
        const auto add = toolBox(Tool::AddInterval);
        return add.isEmpty() ? add : add.withX(toolsArea().getX() + 6);
    }
    if (field == Field::Stretch) {
        // In the first column, beside x2 and /2.
        const auto fit = toolBox(Tool::FitToScale), twice = toolBox(Tool::Double);
        return fit.isEmpty() || twice.isEmpty() ? juce::Rectangle<int>() : fit.withX(toolsArea().getX() + 6).withY(twice.getY());
    }
    if (field == Field::FilterA || field == Field::FilterB || field == Field::FilterC) {
        // In the filter bar: each after its name, beside the chooser.
        const int slot = field == Field::FilterA ? 0 : field == Field::FilterB ? 1 : 2;
        const auto chooser = filterChooserBox();
        if (chooser.isEmpty() || slot >= filterFieldCount())
            return {};
        return {chooser.getRight() + 8 + slot * 92 + 46, chooser.getY(), 40, chooser.getHeight()};
    }
    if (field == Field::Name)
        return titleArea().withTrimmedLeft(20).reduced(0, 1);
    if (field == Field::QuantizeAmount || field == Field::QuantizeSwing) {
        // Under Start and End, at the foot of the Functions tab.
        const auto above = field == Field::QuantizeAmount ? quantizeStartBox() : quantizeEndBox();
        return above.isEmpty() ? above : above.withY(above.getY() + 22);
    }
    if (field == Field::RandomAmount || field == Field::RampStart || field == Field::RampEnd) {
        // In the strip under the lanes: the amount beside Randomize, then the Ramp's two values.
        const auto button = randomizeBox();
        if (button.isEmpty())
            return {};
        const int x = field == Field::RandomAmount ? button.getRight() + 4 : field == Field::RampStart ? button.getRight() + 96 : button.getRight() + 136;
        return {x, button.getY(), 36, button.getHeight()};
    }
    if (field == Field::VelocityAmount || field == Field::VelocityPercent || field == Field::VelocityLow ||
        field == Field::VelocityHigh || field == Field::Voices) {
        // Each sits in the first column beside its button; Limit's two share a row above it.
        const auto add = toolBox(Tool::VelocityAdd);
        if (add.isEmpty())
            return {};
        const int left = toolsArea().getX() + 6;
        const int below = field == Field::VelocityAmount ? 0 : field == Field::VelocityPercent ? 1 : field == Field::Voices ? 6 : 2;
        const auto box = add.withY(add.getY() + below * 22);
        return field == Field::VelocityHigh ? box : box.withX(left);
    }
    return {main.getX() + column * (half + 6), main.getY() + row + kFieldLabel, half, kFieldBox};
}
// ---- The Pitch and Time panel (manual 8.5; STEP-7-PLAN 7.7 step 4) -----------------------------
juce::Rectangle<int> ClipViewPanel::toolsArea() const noexcept {
    return midi() ? juce::Rectangle<int>(kPanelWidth + 1, 0, kToolsWidth - 1, getHeight()) : juce::Rectangle<int>();
}
juce::Rectangle<int> ClipViewPanel::rangeBox() const noexcept {
    const auto inner = toolsArea().withTrimmedTop(kTitleHeight).reduced(6, 5);
    return inner.isEmpty() || toolsTab_ != 0 ? juce::Rectangle<int>() : inner.withHeight(kFieldBox);
}
juce::Rectangle<int> ClipViewPanel::toolBox(Tool tool) const noexcept {
    const auto inner = toolsArea().withTrimmedTop(kTitleHeight).reduced(6, 5);
    if (inner.isEmpty())
        return {};
    if (tabOf(tool) != toolsTab_)
        return {};
    const int half = (inner.getWidth() - 6) / 2;
    // Two columns of rows; on Pitch and Time the range has the first row, and a field often the first column.
    int row = 1, column = 1;
    switch (tool) {
    // Functions.
    case Tool::VelocityAdd:
        break;
    case Tool::VelocityScale:
        row = 2;
        break;
    case Tool::VelocityLimit:
        row = 4, column = 0;
        break;
    case Tool::DeleteDoubles:
        row = 6, column = 0;
        break;
    case Tool::RestrictPolyphony:
        row = 7;
        break;
    case Tool::Quantize:
        row = 8, column = 0;
        break;
    // Pitch and Time, as Live's.
    case Tool::FitToScale:
        break;
    case Tool::Invert:
        row = 2, column = 0;
        break;
    case Tool::Reverse:
        row = 2;
        break;
    case Tool::AddInterval:
        row = 3;
        break;
    case Tool::Double:
    case Tool::Halve: {
        // x2 and /2 share the place beside the Stretch control.
        const int each = (half - 4) / 2;
        return {inner.getX() + half + 6 + (tool == Tool::Halve ? each + 4 : 0), inner.getY() + 4 * 22, each, kFieldBox};
    }
    case Tool::FixedLength:
        row = 5;
        break;
    case Tool::Legato:
        row = 6, column = 0;
        break;
    // Chords.
    case Tool::Chord:
        row = 1;
        break;
    case Tool::InvertUp:
        row = 2, column = 0;
        break;
    case Tool::InvertDown:
        row = 2;
        break;
    case Tool::Drop2:
        row = 3, column = 0;
        break;
    case Tool::Drop3:
        row = 3;
        break;
    case Tool::OverlapsMono:
        row = 5, column = 0;
        break;
    case Tool::OverlapsPoly:
        row = 5;
        break;
    }
    return {inner.getX() + column * (half + 6), inner.getY() + row * 22, half, kFieldBox};
}
int ClipViewPanel::tabOf(Tool tool) noexcept {
    switch (tool) {
    case Tool::VelocityAdd:
    case Tool::VelocityScale:
    case Tool::VelocityLimit:
    case Tool::DeleteDoubles:
    case Tool::RestrictPolyphony:
    case Tool::Quantize:
        return 1;
    case Tool::Chord:
    case Tool::InvertUp:
    case Tool::InvertDown:
    case Tool::Drop2:
    case Tool::Drop3:
    case Tool::OverlapsMono:
    case Tool::OverlapsPoly:
        return 2;
    default:
        return 0;
    }
}
void ClipViewPanel::setToolsTab(int tab) {
    toolsTab_ = std::clamp(tab, 0, kToolTabs - 1);
    closeEditor();
    repaint();
}
juce::Rectangle<int> ClipViewPanel::tabBox(int tab) const noexcept {
    const auto title = toolsArea().withHeight(kTitleHeight);
    if (title.isEmpty() || tab < 0 || tab >= kToolTabs)
        return {};
    return tab == 0 ? title.withWidth(88) : tab == 1 ? title.withTrimmedLeft(88).withWidth(62) : title.withTrimmedLeft(150);
}
juce::Rectangle<int> ClipViewPanel::durationBox() const noexcept {
    const auto set = toolBox(Tool::FixedLength);
    return set.isEmpty() ? set : set.withX(toolsArea().getX() + 6);
}
juce::PopupMenu ClipViewPanel::durationMenu() const {
    juce::PopupMenu menu;
    for (std::size_t i = 0; i < kDurations.size(); ++i)
        menu.addItem(static_cast<int>(i) + 1, kDurations[i].name, true, i == duration_);
    return menu;
}
bool ClipViewPanel::chooseDuration(int item) {
    if (item < 1 || item > static_cast<int>(kDurations.size()))
        return false;
    duration_ = static_cast<std::size_t>(item - 1);
    repaint();
    return true;
}
std::int64_t ClipViewPanel::durationTicks() const noexcept {
    const auto fixed = kDurations[duration_].ticks;
    return fixed > 0 ? fixed : notes.gridTicks();
}
bool ClipViewPanel::stretch(int percent) {
    if (!details_ || !midi())
        return false;
    if (percent < 10 || percent > 1000) {
        root.say("A stretch is 10 to 1000 per cent.");
        return false;
    }
    if (percent == 100)
        return true;
    return notes.apply(stretchNotes(*details_, notes.selected(), percent, 100));
}
int *ClipViewPanel::plainField(Field field, int *low, int *high) noexcept {
    const auto range = [&](int a, int b) {
        if (low != nullptr)
            *low = a;
        if (high != nullptr)
            *high = b;
    };
    if (field == Field::FilterA || field == Field::FilterB || field == Field::FilterC) {
        const int slot = field == Field::FilterA ? 0 : field == Field::FilterB ? 1 : 2;
        if (!filtersShown_ || slot >= filterFieldCount())
            return nullptr;
        switch (filter_) {
        case NoteFilter::Time:
            range(slot == 1 ? 1 : 0, 9999);
            return slot == 0 ? &filterTimeStart : slot == 1 ? &filterTimeLength : &filterTimeRepeat;
        case NoteFilter::Chance:
            range(0, 100);
            return slot == 0 ? &filters.chanceMin : &filters.chanceMax;
        case NoteFilter::Count:
            range(slot == 0 ? 1 : 0, 64);
            return slot == 0 ? &filters.every : &filters.offset;
        case NoteFilter::Duration:
            range(1, 512);
            return slot == 0 ? &filterDurationMin : &filterDurationMax;
        case NoteFilter::Velocity:
            range(1, 127);
            return slot == 0 ? &filters.velocityMin : &filters.velocityMax;
        default:
            return nullptr;
        }
    }
    switch (field) {
    case Field::VelocityAmount:
        range(-127, 127);
        return &velocityAmount;
    case Field::VelocityPercent:
        range(0, 300);
        return &velocityPercent;
    case Field::VelocityLow:
        range(1, 127);
        return &velocityLow;
    case Field::VelocityHigh:
        range(1, 127);
        return &velocityHigh;
    case Field::Voices:
        range(1, 16);
        return &voices;
    case Field::Interval:
        range(-48, 48);
        return &intervalSize;
    case Field::QuantizeAmount:
        range(0, 100);
        return &quantizeAmount;
    case Field::QuantizeSwing:
        range(0, 100);
        return &quantizeSwing;
    case Field::RandomAmount:
        range(0, 127);
        return &randomAmount;
    case Field::RampStart:
        range(1, 127);
        return &rampStart;
    case Field::RampEnd:
        range(1, 127);
        return &rampEnd;
    default:
        return nullptr;
    }
}
const char *ClipViewPanel::toolName(Tool tool) noexcept {
    switch (tool) {
    case Tool::VelocityAdd:
        return "Add";
    case Tool::VelocityScale:
        return "Scale";
    case Tool::VelocityLimit:
        return "Limit";
    case Tool::DeleteDoubles:
        return "Del. Doubles";
    case Tool::RestrictPolyphony:
        return "Restrict Poly";
    case Tool::Quantize:
        return "Quantize";
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
        return "Set Length";
    case Tool::AddInterval:
        return "Add Interval";
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
    case Tool::FixedLength: {
        // Set Length (manual 10.5.9.6): the Duration chooser's length for the selected notes, or all.
        auto set = fixedLengths(d, ids, durationTicks());
        for (auto &o : set.ops)
            o.label = "Set Length";
        return notes.apply(std::move(set));
    }
    case Tool::AddInterval:
        return notes.apply(addInterval(d, ids, intervalSize, d.scale.local ? d.scale.mask : 0, static_cast<int>(d.scale.root)));
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
    case Tool::VelocityAdd:
        return notes.apply(addToVelocities(d, ids, velocityAmount));
    case Tool::VelocityScale:
        return notes.apply(scaleVelocities(d, ids, velocityPercent));
    case Tool::VelocityLimit:
        return notes.apply(limitVelocities(d, ids, velocityLow, velocityHigh));
    case Tool::DeleteDoubles:
        return notes.apply(deleteDoubles(d, ids));
    case Tool::RestrictPolyphony:
        return notes.apply(restrictPolyphony(d, ids, voices));
    case Tool::Quantize:
        return notes.quantize();
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
    // Every shown clip takes the edited clip's scale as it now is (manual 10.8).
    if (roots) {
        if (item > 12)
            return false;
        const auto tonic = static_cast<std::int64_t>(item - 1);
        const auto id = d.scale.id;
        const auto mask = d.scale.mask;
        return commitForAll([=](const engine::ClipDetails &clip) { return setClipScale(clip, tonic, id, mask); });
    }
    if (item > static_cast<int>(kScales.size()))
        return false;
    const auto &scale = kScales[static_cast<std::size_t>(item - 1)];
    const auto tonic = d.scale.root;
    const std::string id = scale.id;
    const auto mask = scale.mask;
    return commitForAll([=](const engine::ClipDetails &clip) { return setClipScale(clip, tonic, id, mask); });
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
                             Field::Transpose, Field::VelocityAmount, Field::VelocityPercent, Field::VelocityLow,
                             Field::VelocityHigh, Field::Voices, Field::RandomAmount, Field::RampStart, Field::RampEnd,
                             Field::FilterA, Field::FilterB, Field::FilterC, Field::QuantizeAmount, Field::QuantizeSwing,
                             Field::Interval, Field::Stretch})
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
    if (field == Field::Name)
        return rename(typed);
    const auto text = typed.toStdString();
    {
        int low = 0, high = 0;
        if (auto *value = plainField(field, &low, &high)) {
            const auto trimmed = typed.trim().upToFirstOccurrenceOf(" ", false, false);
            if (trimmed.isEmpty() || !trimmed.containsOnly("+-0123456789") || trimmed.lastIndexOfAnyOf("+-") > 0 ||
                trimmed.length() > 5 || !trimmed.containsAnyOf("0123456789")) {
                root.say("Type a whole number.");
                return false;
            }
            *value = std::clamp(trimmed.getIntValue(), low, high);
            repaint();
            numberChanged(field, true);
            return true;
        }
    }
    if (field == Field::Stretch) {
        const auto trimmed = typed.trim().upToFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf("%", false, false);
        if (trimmed.isEmpty() || !trimmed.containsOnly("0123456789") || trimmed.length() > 4) {
            root.say("Type a stretch in per cent: 150.");
            return false;
        }
        return stretch(trimmed.getIntValue());
    }
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
        return commitForAll([&](const engine::ClipDetails &clip) { return setClipSignature(clip, signature->first, signature->second); });
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
std::vector<std::int64_t> ClipViewPanel::shownClips() const {
    std::vector<std::int64_t> ids;
    if (details_)
        ids.push_back(details_->id);
    for (const auto &layer : others_)
        ids.push_back(layer.details.id);
    return ids;
}
bool ClipViewPanel::commitForAll(const std::function<TrackEdit(const engine::ClipDetails &)> &build) {
    if (!details_)
        return false;
    auto all = build(*details_);
    if (all.refusal.empty())
        for (const auto &layer : others_) {
            auto part = build(layer.details);
            if (!part.refusal.empty())
                continue;
            for (auto &o : part.ops) {
                // The step is named for the edit, whichever clip's op comes first.
                all.ops.push_back(std::move(o));
            }
        }
    return commit(std::move(all));
}
juce::PopupMenu ClipViewPanel::titleMenu() const {
    juce::PopupMenu menu, colours;
    menu.addItem(1, "Rename");
    for (int i = 0; i < kTitleColours; ++i) {
        juce::PopupMenu::Item item("Color " + juce::String(i + 1));
        item.itemID = 100 + i;
        item.isTicked = details_ && details_->color && *details_->color == i;
        auto swatch = std::make_unique<juce::DrawableRectangle>();
        swatch->setRectangle(juce::Parallelogram<float>(juce::Rectangle<float>(0.f, 0.f, 14.f, 14.f)));
        swatch->setFill(look::paletteColour(i));
        item.image = std::move(swatch);
        colours.addItem(std::move(item));
    }
    menu.addSubMenu("Color", colours);
    return menu;
}
bool ClipViewPanel::chooseTitleItem(int item) {
    if (!details_)
        return false;
    if (item == 1) {
        openEditor(Field::Name);
        return true;
    }
    return item >= 100 && item < 100 + kTitleColours && recolour(item - 100);
}
bool ClipViewPanel::rename(const juce::String &name) {
    const auto text = name.trim().toStdString();
    return commitForAll([&](const engine::ClipDetails &clip) { return renameClip(clip, text); });
}
bool ClipViewPanel::recolour(int colour) {
    return commitForAll([colour](const engine::ClipDetails &clip) { return recolourClip(clip, colour); });
}
bool ClipViewPanel::toggleLoop() {
    if (!details_)
        return false;
    // Every shown clip takes the state the edited clip is switched to.
    const bool on = !details_->loopEnabled;
    return commitForAll([on](const engine::ClipDetails &clip) { return clip.loopEnabled == on ? TrackEdit{} : toggleClipLoop(clip); });
}
void ClipViewPanel::openEditor(Field field) {
    editing_ = field;
    const auto r = readings();
    const auto value = field == Field::Start       ? r.start
                       : field == Field::End       ? r.end
                       : field == Field::Position  ? r.position
                       : field == Field::Length    ? r.length
                       : field == Field::Transpose ? juce::String("0")
                       : field == Field::Stretch   ? juce::String("100")
                       : plainField(field) != nullptr ? juce::String(*plainField(field))
                                                   : r.signature;
    editor_.setBounds(fieldBox(field));
    editor_.setJustification(field == Field::Name ? juce::Justification::centredLeft : juce::Justification::centredRight);
    // A name keeps its spaces; the other fields are read without them.
    editor_.setText(field == Field::Name ? juce::String(details_->name) : value.removeCharacters(" "), false);
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
    if (details_ && e.mods.isPopupMenu() && titleArea().contains(e.getPosition())) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        titleMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe](int item) {
            if (safe != nullptr && item > 0)
                safe->chooseTitleItem(item);
        });
        return;
    }
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
    for (int tab = 0; tab < kToolTabs; ++tab)
        if (tabBox(tab).contains(at)) {
            setToolsTab(tab);
            return;
        }
    if (randomizeBox().contains(at)) {
        randomize();
        return;
    }
    if (findBox().contains(at)) {
        showFilters(!filtersShown_);
        return;
    }
    if (gridBox().contains(at)) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        gridMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(gridBox())),
                                 [safe](int item) {
                                     if (safe != nullptr && item > 0)
                                         safe->chooseGrid(item);
                                 });
        return;
    }
    if (coloursBox().contains(at)) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        coloursMenu().showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(coloursBox())),
            [safe](int item) {
                if (safe != nullptr && item > 0)
                    safe->chooseColours(item);
            });
        return;
    }
    if (durationBox().contains(at)) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        durationMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(durationBox())),
                                     [safe](int item) {
                                         if (safe != nullptr && item > 0)
                                             safe->chooseDuration(item);
                                     });
        return;
    }
    if (quantizeChooserBox().contains(at)) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        quantizeMenu().showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(quantizeChooserBox())),
            [safe](int item) {
                if (safe != nullptr && item > 0)
                    safe->chooseQuantize(item);
            });
        return;
    }
    if (quantizeStartBox().contains(at)) {
        quantizeStarts = !quantizeStarts;
        repaint();
        keepSettings();
        return;
    }
    if (quantizeEndBox().contains(at)) {
        quantizeEnds = !quantizeEnds;
        repaint();
        keepSettings();
        return;
    }
    if (filterChooserBox().contains(at)) {
        juce::Component::SafePointer<ClipViewPanel> safe(this);
        filterMenu().showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(localAreaToGlobal(filterChooserBox())),
            [safe](int item) {
                if (safe != nullptr && item > 0)
                    safe->chooseFilter(item);
            });
        return;
    }
    for (int i = 0; i < filterToggleCount(); ++i)
        if (filterToggleBox(i).contains(at)) {
            clickFilterToggle(i);
            return;
        }
    if (filterInvertBox().contains(at)) {
        toggleFilterInvert();
        return;
    }
    if (filterSelectBox().contains(at)) {
        applyFilter();
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
    if (focusBox().contains(at)) {
        setFocus(!focus_);
        return;
    }
    if (scaleFoldBox().contains(at)) {
        notes.setScaleFold(!notes.scaleFolded());
        repaint();
        return;
    }
    if (highlightBox().contains(at)) {
        notes.highlightScale = !notes.highlightScale;
        notes.repaint();
        repaint();
        keepSettings();
        return;
    }
    if (scaleModeBox().contains(at)) {
        toggleScaleMode();
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
    if (int low = 0, high = 0; plainField(field, &low, &high) != nullptr) {
        // A field of the Functions tab holds a number: a drag changes it, a click opens it for typing.
        if (dragged) {
            auto *value = plainField(field);
            *value = std::clamp(*value + steps, low, high);
            repaint();
            numberChanged(field, false);
        } else {
            openEditor(field);
        }
        return;
    }
    if (field == Field::Scale)
        // The root's part of the field opens the root notes; the rest, the scales.
        showScaleMenu(pressPart_ == 0);
    else if (!dragged)
        openEditor(field);
    else if (field == Field::Transpose)
        transpose(steps);
    else if (field == Field::Stretch)
        stretch(std::clamp(100 + 5 * steps, 10, 1000));
    else if (field == Field::Signature)
        commitForAll([&](const engine::ClipDetails &clip) { return setClipSignature(clip, signature.first, signature.second); });
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
    // The Main panel (manual 8.2). Its fields read the clip; they act with the clip's own ops.
    const auto r = readings();
    if (!others_.empty()) {
        // Several clips shown: a striped title (manual 10.8).
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(title.withTrimmedLeft(20));
        g.setColour(juce::Colours::white.withAlpha(0.22f));
        for (int x = title.getX() - title.getHeight(); x < title.getRight(); x += 8)
            g.drawLine(static_cast<float>(x), static_cast<float>(title.getBottom()), static_cast<float>(x + title.getHeight()),
                       static_cast<float>(title.getY()), 2.f);
    }
    text(r.title, title.withTrimmedLeft(20).withTrimmedRight(4), juce::Justification::centredLeft, look::text, 12.f, true);
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
    look::box(g, loop.toFloat(), r.loop && !r.loopMixed ? look::accent : look::button);
    if (r.loopMixed) {
        // Looped and unlooped clips together: the button is half lit (manual 10.8).
        g.setColour(look::accent);
        g.fillRect(loop.reduced(1).withWidth(loop.getWidth() / 2 - 1));
    }
    text("Loop", loop, juce::Justification::centred, look::text, 11.f);
    field(Field::Position, "Position", r.position);
    field(Field::Length, "Length", r.length);
    field(Field::Signature, "Signature", r.signature);
    if (midi()) {
        field(Field::Scale, "Scale", r.scale);
        // Scale Mode's switch (manual 8.2.4): lit while the clip holds a scale of its own.
        const auto mode = scaleModeBox();
        g.setColour(details_->scale.local ? look::accent : look::button);
        g.fillRoundedRectangle(mode.toFloat(), 5.f);
        g.setColour(look::outline);
        g.drawRoundedRectangle(mode.toFloat().reduced(0.5f), 5.f, 1.f);
        text(details_->scale.local ? "On" : "Off", mode, juce::Justification::centred, look::text, 9.f);
    }

    if (const auto tools = toolsArea(); !tools.isEmpty()) {
        // The Pitch and Time panel (manual 8.5): its title, the range of the notes it acts on,
        // Transpose, and the utilities.
        static constexpr const char *kTabNames[kToolTabs] = {"Pitch and Time", "Functions", "Chords"};
        for (int tab = 0; tab < kToolTabs; ++tab) {
            const auto box = tabBox(tab);
            g.setColour(tab == toolsTab_ ? look::field : look::columnHead);
            g.fillRect(box);
            text(kTabNames[tab], box.withTrimmedLeft(tab == 0 ? 6 : 4), juce::Justification::centredLeft, look::text, 10.5f, tab == toolsTab_);
        }
        if (toolsTab_ == 1) {
            // ADI's Functions tab: the velocity functions with their amounts, and the note functions.
            const auto inner = tools.withTrimmedTop(kTitleHeight).reduced(6, 5);
            text("Velocity", inner.withHeight(kFieldBox), juce::Justification::centredLeft, look::text, 10.5f, true);
            text("Notes", inner.withY(inner.getY() + 5 * 22).withHeight(kFieldBox), juce::Justification::centredLeft, look::text, 10.5f, true);
            const auto number = [&](Field which, const juce::String &suffix) {
                const auto box = fieldBox(which);
                int low = 0, high = 0;
                const int shown = std::clamp(*plainField(which, &low, &high) + (dragged_ && pressed_ == which ? dragSteps_ : 0), low, high);
                look::box(g, box.toFloat(), look::chooser);
                text(juce::String(shown) + suffix, box.reduced(3, 0), juce::Justification::centredRight, look::text, 11.f);
            };
            number(Field::VelocityAmount, "");
            number(Field::VelocityPercent, " %");
            number(Field::VelocityLow, "");
            number(Field::VelocityHigh, "");
            number(Field::Voices, "");
            // The Quantize settings: the value, which ends move, how far, and swing.
            const auto value = quantizeChooserBox();
            look::box(g, value.toFloat(), look::chooser);
            text(kQuantizeValues[quantizeValue_].name, value.reduced(3, 0), juce::Justification::centredLeft, look::text, 10.f);
            const auto starts = quantizeStartBox(), ends = quantizeEndBox();
            look::box(g, starts.toFloat(), quantizeStarts ? look::accent : look::button);
            text("Start", starts, juce::Justification::centred, look::text, 10.f);
            look::box(g, ends.toFloat(), quantizeEnds ? look::accent : look::button);
            text("End", ends, juce::Justification::centred, look::text, 10.f);
            const auto named = [&](Field which, const char *name) {
                const auto box = fieldBox(which);
                int low = 0, high = 0;
                const int shown = std::clamp(*plainField(which, &low, &high) + (dragged_ && pressed_ == which ? dragSteps_ : 0), low, high);
                look::box(g, box.toFloat(), look::chooser);
                text(name, box.reduced(3, 0), juce::Justification::centredLeft, look::text.withAlpha(0.7f), 9.f);
                text(juce::String(shown) + "%", box.reduced(3, 0), juce::Justification::centredRight, look::text, 10.5f);
            };
            named(Field::QuantizeAmount, "Amt");
            named(Field::QuantizeSwing, "Swing");
        }
        if (toolsTab_ == 0) {
            const auto range = rangeBox();
            look::box(g, range.toFloat(), look::display);
            text(r.range, range.reduced(4, 0), juce::Justification::centred, juce::Colours::white, 11.f);
            const auto semitones = fieldBox(Field::Transpose);
            look::box(g, semitones.toFloat(), look::chooser);
            text(r.transpose, semitones.reduced(3, 0), juce::Justification::centredRight, look::text, 11.f);
            // The Interval size: semitones, or the scale's degrees while Scale Mode is on.
            const auto interval = fieldBox(Field::Interval);
            const int size = std::clamp(intervalSize + (dragged_ && pressed_ == Field::Interval ? dragSteps_ : 0), -48, 48);
            look::box(g, interval.toFloat(), look::chooser);
            text((size > 0 ? "+" : "") + juce::String(size) + (details_->scale.local ? " sd" : " st"), interval.reduced(3, 0),
                 juce::Justification::centredRight, look::text, 11.f);
            // The Stretch control: 100 per cent until it is dragged or typed.
            const auto factor = fieldBox(Field::Stretch);
            look::box(g, factor.toFloat(), look::chooser);
            text("Stretch", factor.reduced(3, 0), juce::Justification::centredLeft, look::text.withAlpha(0.7f), 9.f);
            text(juce::String(dragged_ && pressed_ == Field::Stretch ? std::clamp(100 + 5 * dragSteps_, 10, 1000) : 100) + "%", factor.reduced(3, 0),
                 juce::Justification::centredRight, look::text, 11.f);
            // The Duration chooser, beside Set Length.
            const auto length = durationBox();
            look::box(g, length.toFloat(), look::chooser);
            text(kDurations[duration_].name, length.reduced(3, 0), juce::Justification::centredLeft, look::text, 10.f);
        }
        if (toolsTab_ == 2) {
            // ADI's Chords tab (ADR-0241 d2, from Cubase): the chord tools, and the overlap functions.
            const auto inner = tools.withTrimmedTop(kTitleHeight).reduced(6, 5);
            text("Chords", inner.withHeight(kFieldBox), juce::Justification::centredLeft, look::text, 10.5f, true);
            text("Overlaps", inner.withY(inner.getY() + 4 * 22).withHeight(kFieldBox), juce::Justification::centredLeft, look::text, 10.5f, true);
            // The chord type's chooser, beside Add Chord.
            const auto chord = chordBox();
            look::box(g, chord.toFloat(), look::chooser);
            text(chordType().name, chord.reduced(3, 0), juce::Justification::centredLeft, look::text, 10.f);
        }
        for (const auto tool : kTools) {
            const auto box = toolBox(tool);
            if (box.isEmpty())
                continue;
            look::box(g, box.toFloat(), look::button);
            text(toolName(tool), box.reduced(2, 0), juce::Justification::centred, look::text, 10.f);
        }
    }

    if (const auto strip = velocityStripArea(); !strip.isEmpty()) {
        // Live's Velocity Controls, under the lanes.
        g.setColour(look::field);
        g.fillRect(strip);
        g.setColour(look::band);
        g.fillRect(strip.withHeight(1));
        const auto button = randomizeBox();
        look::box(g, button.toFloat(), look::button);
        text("Randomize", button, juce::Justification::centred, look::text, 10.f);
        const auto number = [&](Field which) {
            const auto box = fieldBox(which);
            int low = 0, high = 0;
            const int shown = std::clamp(*plainField(which, &low, &high) + (dragged_ && pressed_ == which ? dragSteps_ : 0), low, high);
            look::box(g, box.toFloat(), look::chooser);
            text(juce::String(shown), box.reduced(3, 0), juce::Justification::centredRight, look::text, 10.5f);
        };
        number(Field::RandomAmount);
        const auto start = fieldBox(Field::RampStart);
        text("Ramp", {start.getX() - 34, start.getY(), 30, start.getHeight()}, juce::Justification::centredRight, look::text, 10.f);
        number(Field::RampStart);
        number(Field::RampEnd);
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
        if (const auto focus = focusBox(); !focus.isEmpty()) {
            look::box(g, focus.toFloat(), focus_ ? look::accent : look::button);
            text("Focus", focus, juce::Justification::centred, look::text, 10.f);
        }
        // Fold to Scale and Highlight Scale; Fold to Scale is dimmed while the clip holds no scale of its own.
        const auto scaleFold = scaleFoldBox(), highlight = highlightBox();
        look::box(g, scaleFold.toFloat(), notes.scaleFolded() ? look::accent : look::button);
        text("Scale", scaleFold, juce::Justification::centred, notes.scaleOn() ? look::text : look::text.withAlpha(0.4f), 10.f);
        look::box(g, highlight.toFloat(), notes.highlightScale ? look::accent : look::button);
        text("Highlight", highlight, juce::Justification::centred, look::text, 10.f);
        const auto find = findBox();
        look::box(g, find.toFloat(), filtersShown_ ? look::accent : look::button);
        text("Find", find, juce::Justification::centred, look::text, 10.f);
        // What the notes are coloured by (ADI's addition).
        const auto colours = coloursBox();
        look::box(g, colours.toFloat(), look::chooser);
        text(NoteEditor::kNoteColourNames[static_cast<std::size_t>(notes.noteColours)], colours.reduced(4, 0), juce::Justification::centredLeft,
             look::text, 10.f);
    }
    if (const auto bar = filterArea(); !bar.isEmpty()) {
        // Find and Select Notes: the chooser, the chosen filter's controls, Invert and Select.
        g.setColour(look::field);
        g.fillRect(bar);
        g.setColour(look::band);
        g.fillRect(bar.withY(bar.getBottom() - 1).withHeight(1));
        const auto chooser = filterChooserBox();
        look::box(g, chooser.toFloat(), look::chooser);
        text(kNoteFilterNames[static_cast<std::size_t>(filter_)], chooser.reduced(4, 0), juce::Justification::centredLeft, look::text, 10.5f);
        if (filters.applied[static_cast<std::size_t>(filter_)]) {
            g.setColour(look::accent);
            g.fillEllipse(static_cast<float>(chooser.getRight() - 10), static_cast<float>(chooser.getY() + 4), 5.f, 5.f);
        }
        for (int slot = 0; slot < filterFieldCount(); ++slot) {
            const auto which = slot == 0 ? Field::FilterA : slot == 1 ? Field::FilterB : Field::FilterC;
            const auto box = fieldBox(which);
            int low = 0, high = 0;
            const int shown = std::clamp(*plainField(which, &low, &high) + (dragged_ && pressed_ == which ? dragSteps_ : 0), low, high);
            text(filterFieldName(slot), {box.getX() - 46, box.getY(), 42, box.getHeight()}, juce::Justification::centredRight, look::text, 10.f);
            look::box(g, box.toFloat(), look::chooser);
            text(juce::String(shown), box.reduced(3, 0), juce::Justification::centredRight, look::text, 10.5f);
        }
        for (int i = 0; i < filterToggleCount(); ++i) {
            const auto box = filterToggleBox(i);
            look::box(g, box.toFloat(), filterToggle(i) ? look::accent : look::button);
            text(filterToggleName(i), box, juce::Justification::centred, look::text, 9.5f);
        }
        const auto invert = filterInvertBox(), select = filterSelectBox();
        look::box(g, invert.toFloat(), filters.invert[static_cast<std::size_t>(filter_)] ? look::accent : look::button);
        text("Invert", invert, juce::Justification::centred, look::text, 10.f);
        look::box(g, select.toFloat(), look::button);
        text("Select", select, juce::Justification::centred, look::text, 10.f);
    }
    text(midi() ? "Notes" : audio() ? "Sample" : "", header.withTrimmedLeft(midi() ? coloursBox().getRight() - header.getX() + 10 : 8).withWidth(60),
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
