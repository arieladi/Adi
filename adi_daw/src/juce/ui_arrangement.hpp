// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/arrangement_geometry.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
namespace adi::ui {
class AdiRootComponent;
class ArrangementView;
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
class TrackHeaderList final : public juce::Component, public juce::SettableTooltipClient {
  public:
    explicit TrackHeaderList(ArrangementView &);
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;

  private:
    ArrangementView &view;
    std::int64_t resizeId = 0;
    int initialHeight = 0;
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

  private:
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
    void locate();
    // Live's right-click menu for the track area and the headers.
    void contextMenu(juce::Point<int> where, bool header);
    const SnapshotReader &reader() const;
    static constexpr int kRulerHeight = 28, kHeaderWidth = 128;
    ArrangementGeometry geometry;
    AdiRootComponent &root;
    TimelineRuler ruler;
    TrackHeaderList headers;
    ArrangementCanvas canvas;
    ArrangementPlayhead playhead;
    std::uint64_t canvasPaints = 0;
};
} // namespace adi::ui
