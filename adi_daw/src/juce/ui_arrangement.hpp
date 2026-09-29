// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/arrangement_geometry.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
namespace adi::ui {
class AdiRootComponent;
class ArrangementView;
class TimelineRuler final : public juce::Component {
  public:
    explicit TimelineRuler(ArrangementView &v) : view(v) {};
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;

  private:
    ArrangementView &view;
    juce::Point<float> last;
};
class TrackHeaderList final : public juce::Component {
  public:
    explicit TrackHeaderList(ArrangementView &v) : view(v) {};
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
class ArrangementCanvas final : public juce::Component, public juce::FileDragAndDropTarget {
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
    const SnapshotReader &reader() const;
    ArrangementGeometry geometry;
    AdiRootComponent &root;
    TimelineRuler ruler;
    TrackHeaderList headers;
    ArrangementCanvas canvas;
    ArrangementPlayhead playhead;
    juce::TextButton add{"+ Audio"}, navigation{"View"};
    std::function<void()> addTrack;
    std::function<bool(const juce::String &, std::int64_t, std::int64_t)> drop;
    std::uint64_t canvasPaints = 0;
};
} // namespace adi::ui
