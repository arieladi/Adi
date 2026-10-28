// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/arrangement_geometry.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace adi::ui {
class AdiRootComponent;
class ArrangementView;
// The Overview (Live 12 manual 6.1, ADR-0129): the whole arrangement in miniature,
// an outline around the part on screen. Drag sideways to scroll, vertically to zoom;
// a double-click zooms out to the whole arrangement.
class ArrangementOverview final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit ArrangementOverview(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    // The span the overview draws, in ticks: the arrangement, or what is on screen if longer.
    std::int64_t span() const noexcept;
    // The outline around the visible part, in the overview's pixels.
    juce::Rectangle<float> outline() const noexcept;

  private:
    ArrangementView &view;
    juce::Point<float> last;
};
// The scrub area (Live 12 manual 6.1, p. 161): a click anywhere in it launches
// playback from that point. Holding to loop waits for the transport's loop.
class ScrubArea final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit ScrubArea(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;

  private:
    ArrangementView &view;
};
// The beat-time ruler (Live 12 manual 6.1): bars-beats-sixteenths; drag sideways
// scrolls, vertically zooms; a double-click zooms to the selection, or out to the
// whole arrangement when nothing is selected.
class TimelineRuler final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit TimelineRuler(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;

  private:
    ArrangementView &view;
    juce::Point<float> last;
};
// One row per track, on the RIGHT of the clips as in Live's Arrangement View (on the left,
// before the track controls, with the side panels swapped).
// The title bar's own editor for Rename: Enter commits, Esc cancels, Tab commits
// and moves on to the next track's title (Shift+Tab, the previous), as Live.
class RenameEditor final : public juce::TextEditor {
  public:
    bool keyPressed(const juce::KeyPress &) override;
    std::function<void()> onCommit, onCancel;
    std::function<void(bool forward)> onTab;
};
class TrackHeaderList final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit TrackHeaderList(ArrangementView &);
    void paint(juce::Graphics &) override;
    void resized() override;
    // Rename (Ctrl/Cmd+R, the Edit menu): edit this track's name in its title bar.
    bool beginRename(std::int64_t track);
    std::int64_t renaming() const noexcept { return renaming_; }
    RenameEditor editor;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

  private:
    bool commitRename();
    void endRename();
    ArrangementView &view;
    std::int64_t resizeId = 0, renaming_ = 0;
    int initialHeight = 0;
};
// What a screen reader finds for one clip on screen (UI-ARCHITECTURE §6). The
// canvas paints every clip itself; JUCE gathers accessibility children only from
// child components, so each visible clip gets one of these: it paints nothing, the
// mouse passes through it, and it is recycled as the view scrolls (§10).
class ClipAccessible final : public juce::Component {
  public:
    explicit ClipAccessible(ArrangementView &);
    void bind(std::int64_t track, std::int64_t clip);
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    std::int64_t track = 0, clip = 0;

  private:
    ArrangementView &view;
};
// ONE component paints every clip and hit-tests them itself (ADR-0044).
class ArrangementCanvas final : public juce::Component,
                                public juce::FileDragAndDropTarget,
                                public juce::SettableTooltipClient {
  public:
    explicit ArrangementCanvas(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void mouseMagnify(const juce::MouseEvent &, float) override;
    bool isInterestedInFileDrag(const juce::StringArray &) override;
    void filesDropped(const juce::StringArray &, int, int) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    // One accessible child per clip on screen; on a model or view change only.
    void realiseAccessible();
    // What the reader says for a clip: its track, where it starts, how long it is.
    juce::String describe(std::int64_t track, std::int64_t clip) const;
    const std::vector<std::unique_ptr<ClipAccessible>> &accessible() const noexcept {
        return accessible_;
    }
    juce::String lastAnnouncement;
    // The clip being moved by its bar (§6.7), and where it would land; 0 when none is.
    std::int64_t movingClip() const noexcept { return moving_; }
    std::int64_t movingTo() const noexcept { return movingTo_; }
    // The clip whose right edge is being dragged (§6.7), and its length so far; 0 when none.
    std::int64_t resizingClip() const noexcept { return resizing_; }
    std::int64_t resizedLength() const noexcept { return resizeLength_; }
    // A clip's bar, Live's clip title strip: where a drag moves the clip (§6.7), and the
    // width of its right edge, where a drag changes its length.
    static constexpr int kClipBarHeight = 11, kClipEdge = 4;
    void mouseMove(const juce::MouseEvent &) override;
    // Whether the clip's length is being changed from the keyboard: Enter with the insert
    // marker at its end, the arrows, then Enter to keep it or Esc to cancel (§41.4.1.3).
    bool keyResizing() const noexcept { return resizing_ != 0 && keyResize_; }

  private:
    // What a screen reader hears after a key moves the selection.
    void announce(const juce::String &);
    // Live's left and right arrows (online §41.4.1.3): by the grid, or to a clip's edge with
    // Ctrl (Option on the Mac); with Shift they select.
    bool arrow(bool forward, const juce::ModifierKeys &);
    // The nearest clip edge on the selected track after `tick`, or before it; `tick` if none.
    std::int64_t clipEdge(std::int64_t tick, bool forward) const;
    // Deletes or switches the selected clip: one op, through the root.
    bool clipEdit(const char *op, Payload payload, const char *label);
    std::int64_t moving_ = 0, movingFrom_ = 0, movingTo_ = 0, grabTicks_ = 0;
    std::int64_t resizing_ = 0, resizeStart_ = 0, resizeFrom_ = 0, resizeLength_ = 0;
    bool keyResize_ = false;
    // The clip's bar under a point, and whether the point is on its right edge.
    const engine::ClipNode *barAt(int x, int y, std::int64_t &track, bool &edge) const;
    std::vector<std::unique_ptr<ClipAccessible>> accessible_;
    ArrangementView &view;
    juce::Point<float> last;
    bool panning = false;
};
// One pixel wide, above the canvas, transparent to the mouse (ADR-0050 d2): moving
// it repaints two thin strips, never the arrangement.
class ArrangementPlayhead final : public juce::Component {
  public:
    ArrangementPlayhead() { setInterceptsMouseClicks(false, false); }
    void paint(juce::Graphics &g) override { g.fillAll(juce::Colour(0xffffd66b)); }
};
// The Overview is the root's child, not the view's: Live draws it across the track
// headers as well, up to the H and W buttons.
class ArrangementView final : public juce::Component {
  public:
    explicit ArrangementView(AdiRootComponent &);
    void resized() override;
    void paint(juce::Graphics &) override;
    // A track's colour by its place, as Live's auto-assigned colours (the snapshot does
    // not carry a track's own colour yet: window log, N6).
    juce::Colour colourFor(std::size_t position) const;
    void frame(bool changed) noexcept;
    void changed();
    bool command(int);
    void fit(bool selection);
    void wheel(const juce::MouseEvent &, const juce::MouseWheelDetails &);
    // Selects at a point: the clip there, or the insert marker on the grid (§6.10).
    void select(int x, int y, bool snapToGrid = true);
    // Whether a gesture with these keys snaps to the grid.
    bool snaps(const juce::ModifierKeys &) const noexcept;
    // Select one clip by identity: the accessible children's press action.
    void selectClip(std::int64_t track, std::int64_t clip);
    void locate();
    // Live's right-click menu for the track area and the headers.
    void contextMenu(juce::Point<int> where, bool header);
    // The lanes' menu's grid (§6.10): the adaptive and fixed widths by name, the one in use
    // ticked, then Triplet Grid and Snap to Grid.
    void addGridItems(juce::PopupMenu &);
    // Live's Follow (§6.2): brings the playhead at `tick` on screen, by page (it jumps to the
    // left edge once it leaves the lanes) or by scrolling (it stays in the middle). True when
    // the view moved.
    bool follow(std::int64_t tick, bool page) noexcept;
    // Paused by an edit, a sideways scroll or a click on the ruler; a click in the lanes or
    // the scrub area, or playback starting or stopping, starts it again.
    bool followPaused = false;
    const SnapshotReader &reader() const;
    // Live 11's measures at 100 % (2026-10-27): the Overview with its margin, the beat
    // ruler with its numbers along its foot, the locator band, a dark gap, then the
    // lanes; the time ruler under them; a margin left of the time; the track header's
    // name column with the dark rule before it.
    static constexpr int kOverviewHeight = 20, kRulerHeight = 29, kScrubHeight = 24;
    static constexpr int kHeaderWidth = 95, kTimeRulerHeight = 18, kLaneGap = 7, kLeftMargin = 4;
    // One press of Alt/Option + or - on a track's height, in units (PROVISIONAL: Live's step
    // is to be measured).
    static constexpr int kHeightStep = 16;
    // The Overview's row and where the lanes begin in this window: kOverviewHeight and
    // kTopHeight with the Overview shown, without its row when it is hidden (§42.1).
    int overviewHeight() const noexcept;
    int topHeight() const noexcept { return kTopHeight - kOverviewHeight + overviewHeight(); }
    // Where the lanes begin: below the Overview, the ruler and the scrub area.
    static constexpr int kTopHeight = kOverviewHeight + kRulerHeight + kScrubHeight + kLaneGap;
    // The track names' column for a span of names and lanes: Live's width, at most a third.
    static int headerWidth(int span) noexcept { return std::min(kHeaderWidth, span / 3); }
    // With the track controls on the left (the swap, ADR-0080), the names lead: names, a
    // gap of `controls` units the root fills with the track controls, then the lanes.
    void setNamesFirst(bool first, int controls);
    bool namesFirst() const noexcept { return namesFirst_; }
    ArrangementGeometry geometry;
    AdiRootComponent &root;
    ArrangementOverview overview;
    TimelineRuler ruler;
    ScrubArea scrub;
    TrackHeaderList headers;
    ArrangementCanvas canvas;
    ArrangementPlayhead playhead;
    std::uint64_t canvasPaints = 0;

  private:
    bool namesFirst_ = false;
    int controlsGap_ = 0;
};
} // namespace adi::ui
