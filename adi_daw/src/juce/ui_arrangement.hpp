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
// One row per track, on the RIGHT of the clips as in Live's Arrangement View.
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

  private:
    void announceSelection();
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
class ArrangementView final : public juce::Component {
  public:
    explicit ArrangementView(AdiRootComponent &);
    void resized() override;
    void frame(bool changed) noexcept;
    void changed();
    bool command(int);
    void fit(bool selection);
    void wheel(const juce::MouseEvent &, const juce::MouseWheelDetails &);
    void select(int x, int y);
    // Select one clip by identity: the accessible children's press action.
    void selectClip(std::int64_t track, std::int64_t clip);
    void locate();
    // Live's right-click menu for the track area and the headers.
    void contextMenu(juce::Point<int> where, bool header);
    const SnapshotReader &reader() const;
    static constexpr int kOverviewHeight = 16, kRulerHeight = 28, kHeaderWidth = 128;
    static constexpr int kTopHeight = kOverviewHeight + kRulerHeight; // where the lanes begin
    ArrangementGeometry geometry;
    AdiRootComponent &root;
    ArrangementOverview overview;
    TimelineRuler ruler;
    TrackHeaderList headers;
    ArrangementCanvas canvas;
    ArrangementPlayhead playhead;
    std::uint64_t canvasPaints = 0;
};
} // namespace adi::ui
