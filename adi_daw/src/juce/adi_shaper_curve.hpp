// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cstdint>
#include <functional>

namespace adi::ui {

class AdiShaperCurve final : public juce::Component {
public:
    using Points = std::array<float, 8>;
    struct Target {
        std::int64_t device{};
        juce::String stage, band, contour;
        bool operator==(const Target&) const = default;
    };
    enum class Interpolation { Hold, Linear };
    struct View {
        Target target;
        Points points{};
        Interpolation interpolation = Interpolation::Linear;
        juce::String units = "%";
        // Return the number without its unit. The default displays percent.
        std::function<juce::String(float)> formatNumber;
    };
    enum class Gesture { Begin, Value, End, Cancel };
    // Cancel carries the original value. Close that target's undo gesture,
    // restoring its original value. Begin and Value may be refused.
    using Edit = std::function<bool(const Target&, int point, Gesture, float value)>;

    AdiShaperCurve();
    ~AdiShaperCurve() override;
    void bind(View, Edit);
    void unbind();
    // Snapshot echoes leave an active drag's point under the pointer.
    void refresh(const Target&, const Points&);
    void cancelGesture();
    int selectedPoint() const noexcept { return selected_; }
    const Points& points() const noexcept { return view_.points; }
    juce::Rectangle<float> plotBounds() const;
    juce::String pointDescription() const;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusLost(FocusChangeType) override;
    void enablementChanged() override;
    void visibilityChanged() override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    class AccessibleValue;
    juce::Point<float> knot(int tick, juce::Rectangle<float> plot) const;
    juce::String valueText() const;
    void announce();
    bool begin(int point);
    bool update(float value);
    void finish(Gesture, bool notify = true);
    void setOnce(float value);

    View view_;
    Edit edit_;
    int selected_ = 0, active_ = -1;
    float original_ = 0, grabOffset_ = 0;
    bool bound_ = false;
    std::uint64_t binding_ = 0, gesture_ = 0;
};

} // namespace adi::ui
