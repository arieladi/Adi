// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_arrangement.hpp"
#include "ui_shell.hpp"
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
        g.setColour(lane.darker(0.33f));
        for (double line = q; line < q + group && line < geo.left + width / geo.scale; line += grid)
            if (line >= geo.left)
                g.fillRect(static_cast<int>((line - geo.left) * geo.scale), top, 1, height);
    }
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
    setTooltip("Click to play from that point.");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}
void ScrubArea::paint(juce::Graphics &g) {
    // Live's locator band: the lanes' grey and grid, where the loop brace and the
    // locators sit.
    paintLanes(g, view.geometry, getWidth(), 0, getHeight(), false);
}
void ScrubArea::mouseDown(const juce::MouseEvent &e) {
    if (e.mods.isPopupMenu())
        return;
    view.followPaused = false;
    // From the point clicked; Live's global launch quantization arrives with Session
    // View's launch settings (ADR-0203 d10), so nothing is quantized yet.
    view.root.playFrom(view.geometry.tick(e.position.x));
}
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
            // Bars and beats in 4/4 until the snapshot carries the meters (window log, N6).
            const auto beat = c->posTicks / textproj::kPPQ;
            const auto beats = static_cast<double>(c->lengthTicks) / textproj::kPPQ;
            return juce::String::fromUTF8(displayName(t->name, i + 1).c_str()) +
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
    for (const auto row : view.geometry.rows(view.reader())) {
        const auto *t = row.track;
        const int h = view.geometry.heightFor(t->id);
        if (h > 0 && y + h > 0 && y < getHeight())
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
    // Each track alone, its name only, until the snapshot carries more (N6).
    trackRows_ = [](const SnapshotReader &reader, std::vector<TrackRow> &rows) {
        const auto &tracks = reader.tracks();
        rows.resize(tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            auto &row = rows[i];
            row.id = tracks[i]->id;
            row.parent = row.index = 0;
            row.group = row.main = false;
            row.colour = 0;
            row.name = tracks[i]->name;
        }
    };
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
    // Rows the last way filled are not this one's: it starts from none (track_rows.hpp reads the
    // project file only when the rows it has may be stale).
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
        std::sort(geometry.hidden.begin(), geometry.hidden.end());
    }
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
                juce::String::fromUTF8(displayName(name, row.position + 1).c_str()),
                stored != nullptr && stored->colour != 0 ? juce::Colour(0xff000000 | stored->colour)
                                                         : colourFor(row.position));
        }
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
    headers.setBounds(namesFirst_ ? 0 : kLeftMargin + clipsW, top, headerW, body);
    // Bitwig's place for it: the top of the track names, level with the ruler.
    projectView.setBounds(headers.getX() + 4, above + 5, 30, 17);
    frame(true);
}
juce::Colour ArrangementView::colourFor(std::size_t position) const {
    return look::trackColour(position);
}
void ArrangementView::paint(juce::Graphics &g) {
    // The arrangement's frame is Live's dark band; the rulers and the header column's
    // top sit on it.
    g.fillAll(look::band);
    // Above the headers: Live's locator buttons ("Set", previous, next) and its two
    // switches, Follow's lock and the automation's arrow.
    const float hx = static_cast<float>(headers.getX());
    const float top = static_cast<float>(overviewHeight() + kRulerHeight) - 2.f;
    const juce::Rectangle<float> set(hx + 5, top, 28, 12);
    g.setColour(look::button);
    g.fillRoundedRectangle(set, 6.f);
    g.setColour(look::outline);
    g.drawRoundedRectangle(set.reduced(0.5f), 6.f, 1.f);
    g.setColour(look::text);
    g.setFont(look::font(11.f));
    g.drawText("Set", set, juce::Justification::centred);
    for (int i = 0; i < 2; ++i) {
        const juce::Rectangle<float> c(hx + 4 + static_cast<float>(i) * 17, top + 15, 12, 12);
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
    look::box(g, {hx + 45, top + 15, 22, 12}, look::button);
    g.setColour(look::text);
    g.drawLine(hx + 51, top + 24, hx + 61, top + 18, 1.2f);
    g.fillEllipse(hx + 49.5f, top + 22.5f, 3, 3);
    g.fillEllipse(hx + 59.5f, top + 16.5f, 3, 3);
    look::box(g, {hx + 72, top + 15, 22, 12}, juce::Colour(0xfff0773a));
    g.fillRect(hx + 79.f, top + 20.5f, 8.f, 4.5f);
    g.drawRoundedRectangle(hx + 80.5f, top + 17.5f, 5.f, 5.f, 2.f, 1.f);
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
        const auto at = geometry.tick(x);
        // The clip under the mouse is hit where it is; the insert marker snaps (§6.10).
        const auto *c = geometry.hit(*t, at);
        const auto tick = snapToGrid ? geometry.snap(at, false) : at;
        geometry.selection.clip = c ? c->id : 0;
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
    if (!header) {
        // The lanes' menu sets the grid as well (§6.10).
        menu.addSeparator();
        addGridItems(menu);
    }
    menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetScreenArea(
        juce::Rectangle<int>(where.x, where.y, 1, 1)));
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
        geometry.scrollY = std::clamp(geometry.scrollY - static_cast<int>(w.deltaY * 300), 0,
                                      std::max(0, geometry.contentHeight(reader()) - 24));
    }
    changed();
}
void TimelineRuler::paint(juce::Graphics &g) {
    // Live's beat-time ruler: bar numbers along its foot in light grey on the dark
    // band, each after its hook; a dot at the bars between them. In 4/4 until the
    // snapshot carries the meters (window log, N6).
    g.fillAll(look::band);
    const auto &geo = view.geometry;
    double stepQ = 4; // quarters between labels
    while (stepQ * geo.scale < 56)
        stepQ *= 2;
    while (stepQ > 1 && stepQ / 2 * geo.scale >= 56)
        stepQ /= 2;
    const int foot = getHeight();
    g.setFont(look::font(12.f, true));
    for (double q = std::floor(geo.left / stepQ) * stepQ; q < geo.left + getWidth() / geo.scale; q += stepQ) {
        const int x = static_cast<int>((q - geo.left) * geo.scale);
        const auto quarter = static_cast<std::int64_t>(std::llround(q));
        const auto bar = quarter / 4 + 1, beat = quarter % 4 + 1;
        g.setColour(juce::Colour(0xff7d7d7d));
        g.fillRect(x, foot - 11, 1, 8);
        g.fillRect(x, foot - 11, 3, 1);
        g.setColour(look::textOnBand);
        g.drawText(beat == 1 ? juce::String(bar) : juce::String(bar) + "." + juce::String(beat), x + 5,
                   foot - 15, 60, 14, juce::Justification::centredLeft);
    }
    if (4 * geo.scale >= 8) {
        g.setColour(juce::Colour(0xff7d7d7d));
        for (double q = std::floor(geo.left / 4) * 4; q < geo.left + getWidth() / geo.scale; q += 4)
            g.fillRect(static_cast<int>((q - geo.left) * geo.scale), foot - 2, 1, 1);
    }
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
    for (const auto row : view.geometry.rows(view.reader())) {
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
    int y = -view.geometry.scrollY;
    std::uint32_t depth = 0;
    const auto at = rows.find(track);
    for (std::size_t i = 0; i < std::min(at, rows.size()); ++i)
        y += view.geometry.heightFor(rows[i].track->id);
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
    // A group's fold switch folds or unfolds it (Live 12 manual 18.3), and selects nothing.
    int top = 0;
    const auto rows = view.geometry.rows(view.reader());
    if (const auto at = view.geometry.rowAt(view.reader(), e.y, &top);
        !e.mods.isPopupMenu() && at < rows.size() && rows[at].place.group &&
        foldSwitch(rows[at].place.depth, top).expanded(2.f).contains(e.position)) {
        resizeId = 0;
        view.toggleFold(rows[at].track->id);
        return;
    }
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
    // Live's lanes: grey, every other group of bars a shade lighter, the selected
    // track's lanes lighter still, the bar grid, a dark rule between tracks.
    ++view.canvasPaints;
    const auto &geo = view.geometry;
    const auto rows = geo.rows(view.reader());
    int y = -geo.scrollY;
    auto lanes = [&](int top, int height, bool selected) {
        paintLanes(g, geo, getWidth(), top, height, selected);
    };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto row = rows[i];
        const auto *t = row.track;
        const int h = geo.heightFor(t->id);
        if (h == 0)
            continue; // left out by the project view, or folded away: no room
        const bool selected = t->id == geo.selection.track;
        lanes(y, h - 2, selected);
        g.setColour(look::separator);
        g.fillRect(0, y + h - 2, getWidth(), 2);
        // The time selection lights the selected track's lanes, as Live's.
        if (selected && geo.selection.start != geo.selection.end) {
            const float x0 = static_cast<float>(geo.x(std::min(geo.selection.start, geo.selection.end)));
            const float x1 = static_cast<float>(geo.x(std::max(geo.selection.start, geo.selection.end)));
            g.setColour(look::selection.withAlpha(0.85f));
            g.fillRect(x0, static_cast<float>(y), x1 - x0, static_cast<float>(h - 2));
        }
        const auto colour = view.colourFor(row.position);
        if (row.place.group)
            paintGroupLane(g, i, y, h, colour);
        for (const auto &c : t->clips) {
            // A clip being moved by its bar is drawn where it would land.
            const auto pos = c->id == moving_ ? movingTo_ : c->posTicks;
            const auto length = c->id == resizing_ ? resizeLength_ : c->lengthTicks;
            const double x = geo.x(pos), end = geo.x(pos + length);
            if (end < 0 || x > getWidth())
                continue;
            const float left = static_cast<float>(std::max(-1., x)),
                        right = static_cast<float>(std::min(end, static_cast<double>(getWidth() + 1)));
            const bool picked = c->id == geo.selection.clip;
            // A clip in its track's colour: a solid title strip, a lighter body. A
            // deactivated clip is grey, as Live's.
            const auto clipColour = c->muted ? juce::Colour(0xff9a9a9a) : colour;
            g.setColour(picked ? clipColour.brighter(0.25f) : clipColour.withMultipliedAlpha(0.75f));
            g.fillRect(left, static_cast<float>(y + 1), std::max(1.f, right - left), static_cast<float>(h - 4));
            g.setColour(clipColour);
            g.fillRect(left, static_cast<float>(y + 1), std::max(1.f, right - left), static_cast<float>(kClipBarHeight));
            g.setColour(clipColour.darker(0.5f));
            g.drawRect(left, static_cast<float>(y + 1), std::max(1.f, right - left),
                       static_cast<float>(h - 4), 1.f);
        }
        y += h;
    }
    // Under the last track, Live's drop area.
    if (y < getHeight()) {
        lanes(y, getHeight() - y, false);
        g.setColour(look::text);
        g.setFont(look::font(12.f));
        g.drawText("Drop Files and Devices Here", 0, y, getWidth(), getHeight() - y,
                   juce::Justification::centred);
    }
    // The insert marker, a thin blue line in the selected track.
    if (geo.selection.start == geo.selection.end && geo.selection.marked) {
        int top = -geo.scrollY;
        for (const auto row : rows) {
            const auto id = row.track->id;
            if (id == geo.selection.track && geo.heightFor(id) > 0) {
                g.setColour(juce::Colour(0xff5dc9e8));
                g.fillRect(static_cast<int>(geo.x(geo.selection.insert)), top, 1, geo.heightFor(id) - 2);
            }
            top += geo.heightFor(id);
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
    view.followPaused = false; // a click in the Arrangement starts Follow again (§6.2)
    last = e.position;
    moving_ = resizing_ = 0;
    keyResize_ = false; // a click ends a resize begun from the keyboard, as Esc does
    panning = e.mods.isCommandDown() && e.mods.isAltDown();
    if (e.mods.isPopupMenu()) {
        view.select(e.x, e.y, view.snaps(e.mods));
        view.contextMenu(e.getScreenPosition(), false);
    } else if (!panning) {
        // Only a clip's bar moves it, not its body; its right edge changes its length (§6.7).
        std::int64_t track = 0;
        bool edge = false;
        if (const auto *c = barAt(e.x, e.y, track, edge); c != nullptr && edge) {
            view.geometry.selection.track = track;
            view.geometry.selection.clip = c->id;
            view.geometry.selection.start = c->posTicks;
            view.geometry.selection.end = c->posTicks + c->lengthTicks;
            resizing_ = c->id;
            resizeStart_ = c->posTicks;
            resizeFrom_ = resizeLength_ = c->lengthTicks;
            view.changed();
            return;
        }
        view.select(e.x, e.y, view.snaps(e.mods));
        if (const auto *c = barAt(e.x, e.y, track, edge); c != nullptr && c->id == view.geometry.selection.clip) {
            moving_ = c->id;
            movingFrom_ = movingTo_ = c->posTicks;
            grabTicks_ = view.geometry.tick(e.x) - c->posTicks;
        }
    }
}
const engine::ClipNode *ArrangementCanvas::barAt(int x, int y, std::int64_t &track, bool &edge) const {
    const auto &geo = view.geometry;
    int top = -geo.scrollY;
    for (const auto row : geo.rows(view.reader())) {
        const auto *t = row.track;
        if (geo.shown(t->id) && y >= top + 1 && y < top + 1 + kClipBarHeight)
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
        // shorter than one grid step.
        const auto start = resizeStart_;
        const auto step = static_cast<std::int64_t>(view.geometry.gridQuarters() * textproj::kPPQ);
        const auto raw = view.geometry.tick(e.x);
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        const auto end = !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up);
        resizeLength_ = std::max(std::max<std::int64_t>(1, step), end - start);
        repaint();
        return;
    }
    if (moving_ != 0) {
        // The clip's start follows the mouse, to the nearest grid line while snapping;
        // it stays on its own track until the snapshot says which tracks can take it (N6).
        const auto raw = std::max<std::int64_t>(0, view.geometry.tick(e.x) - grabTicks_);
        const auto down = view.geometry.snap(raw, false), up = view.geometry.snap(raw, true);
        movingTo_ = !view.snaps(e.mods) ? raw : (raw - down <= up - raw ? down : up);
        repaint();
        return;
    }
    if (panning) {
        view.geometry.pan(last.x - e.position.x);
        view.geometry.scrollY =
            std::max(0, view.geometry.scrollY + static_cast<int>(last.y - e.position.y));
        last = e.position;
    } else {
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
    if (moving_ != 0) {
        const auto clip = moving_, to = movingTo_, from = movingFrom_;
        moving_ = 0;
        // One drag, one undo step (§6.7); the selection follows the clip.
        if (to != from) {
            auto &sel = view.geometry.selection;
            const auto length = sel.end - sel.start;
            if (clipEdit("clip.move", {{"id", clip}, {"to", to}, {"track", sel.track}}, "Move Clip")) {
                sel.start = to;
                sel.end = to + length;
            }
        }
        repaint();
    }
    view.changed();
}
bool ArrangementCanvas::clipEdit(const char *op, Payload payload, const char *label) {
    return view.root.submitEdit(op, std::move(payload), label);
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
    // tracks' fold, which shows their automation, waits for the lanes.
    if (k == juce::KeyPress('u')) {
        if (view.tree().isGroup(sel.track))
            view.toggleFold(sel.track);
        return true;
    }
    if (k == juce::KeyPress('u', juce::ModifierKeys::altModifier, 0)) {
        view.unfoldAll();
        return true;
    }
    const auto tracks = view.geometry.rows(view.reader());
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
        // Between the tracks shown, in the lanes' order: those the project view leaves out or a
        // folded group holds are passed over. At either end the selection stays; with none, it
        // goes to the first track shown.
        const auto at = tracks.find(sel.track);
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
        sel.track = tracks[to].track->id;
        sel.clip = 0;
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
    // Live's Delete, and 0 to deactivate (§6.9), on the selected clip.
    if (const auto clip = view.geometry.selection.clip; clip != 0) {
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) {
            if (!clipEdit("clip.delete", {{"id", clip}}, "Delete Clip"))
                return true;
            sel.clip = 0;
            sel.end = sel.start;
            sel.insert = sel.start;
            view.changed();
            return true;
        }
        if (k == juce::KeyPress('0')) {
            // A refused edit is still the key's: the Status Bar says why.
            if (const auto *t = view.reader().findTrack(view.geometry.selection.track))
                for (const auto &c : t->clips)
                    if (c->id == clip)
                        clipEdit("clip.setMute", {{"id", clip}, {"muted", !c->muted}},
                                 c->muted ? "Activate Clip" : "Deactivate Clip");
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
        sel.marked = true;
        const auto lo = std::min(sel.start, sel.end), hi = std::max(sel.start, sel.end);
        if (track != nullptr)
            for (const auto &c : track->clips)
                if (c->posTicks == lo && c->posTicks + c->lengthTicks == hi)
                    sel.clip = c->id;
        view.changed();
        announce(sel.clip != 0 ? describe(sel.track, sel.clip) : view.root.selectionText());
        return true;
    }
    if (edgeKey) {
        // The previous or next clip edge on the track (the locators are not built yet).
        const auto from = sel.start == sel.end ? sel.insert
                          : forward            ? std::max(sel.start, sel.end)
                                               : std::min(sel.start, sel.end);
        sel.insert = sel.start = sel.end = clipEdge(from, forward);
        sel.clip = 0;
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
    auto best = tick;
    if (const auto *t = view.reader().findTrack(view.geometry.selection.track))
        for (const auto &c : t->clips)
            for (const auto edge : {c->posTicks, c->posTicks + c->lengthTicks})
                if (forward ? edge > tick && (best == tick || edge < best)
                            : edge < tick && (best == tick || edge > best))
                    best = edge;
    return best;
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
