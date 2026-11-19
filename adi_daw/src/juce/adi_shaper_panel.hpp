// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <span>
#include <vector>

namespace adi::ui {

// Device-owned editor. Bindings are scoped to one live device generation.
// A group lasts from pointer-down to release; all Change values are audible.
class AdiShaperPanel final : public juce::Component, public juce::SettableTooltipClient {
  public:
    enum class Phase { Begin, Change, End, Cancel };
    enum class Shape { Hold, Line, Bend };
    struct Key {
        int stage = 0, band = 1, lane = 0;
        bool operator==(const Key &) const = default;
    };
    struct Point {
        float x{}, y{}, bend{};
        Shape shape = Shape::Line;
    };
    struct Change {
        int id{};
        double value{};
    };
    struct Binding {
        int parameterCount = 415; // core control version, excluding sparse curve storage
        juce::String presetName = "Initial state";
        std::function<void()> choosePreset;
        std::function<double(int)> read; // normalized value of the one-based persisted parameter
        std::function<bool(Phase, std::span<const Change>)> parameters;
        std::function<std::vector<Point>(Key)> wave;
        std::function<bool(Key, Phase, std::span<const Point>)> editWave;
    };
    struct Trace {
        std::span<const float> minimum, maximum;
    };
    AdiShaperPanel();
    ~AdiShaperPanel() override;
    void bind(Binding);
    void setPresetName(juce::String name);
    void refresh(); // parameter/automation publication on the window frame clock
    void setTrace(Trace input, Trace output, float peakLeft, float peakRight, float phase);
    void setSpectrum(std::span<const float> logBandDb);
    void select(Key);
    Key selection() const noexcept { return key_; }
    juce::Rectangle<float> curveBounds() const;
    void paint(juce::Graphics &) override;
    void mouseDown(const juce::MouseEvent &) override;
    void mouseDrag(const juce::MouseEvent &) override;
    void mouseUp(const juce::MouseEvent &) override;
    void mouseDoubleClick(const juce::MouseEvent &) override;
    bool keyPressed(const juce::KeyPress &) override;
    void focusLost(FocusChangeType) override;
    void visibilityChanged() override;
    void enablementChanged() override;
    void cancel();

  private:
    enum class Tool { Edit, Draw };
    enum class Drag { None, Control, Node, Stroke, Bend, Level };
    using Rect = juce::Rectangle<float>;
    using Pos = juce::Point<float>;
    struct Control {
        Rect r;
        int id;
        juce::String title, text;
        int mode = 0;
    };
    struct Map {
        int mix{}, clock{}, cycle{}, rate{}, interpolation{}, points{}, layout{}, low{}, high{}, on{}, type{};
    };
    Map mapping() const;
    double read(int id) const;
    const std::vector<Point> &points() const;
    static float average(const std::vector<Point> &);
    bool freeCurve() const;
    std::vector<Control> controls() const;
    void startNodes(Drag);
    void updateNodes(std::vector<Point>);
    void finish(Phase);
    bool beginParam(int);
    bool send(Phase, std::span<const Change>);
    void setOnce(int, double);
    void stroke(Pos);
    void controlDrag(Pos);
    void chooseShape(Shape);
    void writeShape(int);
    void showStages(Pos);
    int nearestNode(Pos, bool nearOnly) const;
    int segment(Pos) const;
    Pos toPlot(Pos) const;
    Rect graph() const;
    Pos logical(Pos) const;
    void updatePointer(Pos);
    static constexpr float designWidth = 1000, designHeight = 600;
    Binding binding_;
    Key key_{};
    Drag drag_ = Drag::None;
    Tool tool_ = Tool::Edit;
    Shape segmentShape_ = Shape::Line;
    std::vector<Point> original_, working_;
    mutable std::vector<Point> cachedPoints_;
    mutable bool pointsDirty_ = true;
    std::vector<Change> originalParams_, edits_;
    std::array<std::vector<float>, 4> traces_;
    std::vector<float> spectrum_;
    std::array<float, 2> peaks_{};
    float phase_ = -1;
    Pos down_{}, previous_{};
    int selected_ = -1, activeId_ = 0, selectedSegment_ = -1;
    double originalValue_ = 0;
    bool snap_ = true, showInput_ = true, showOutput_ = true, flexible_ = false;
    std::array<bool, 8> tabs_{true, false, false, false, false, false, false, false};
};
} // namespace adi::ui
