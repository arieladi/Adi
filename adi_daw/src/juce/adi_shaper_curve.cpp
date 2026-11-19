// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi_shaper_curve.hpp"
#include "ui_look.hpp"

#include <algorithm>
#include <cmath>

namespace adi::ui {
namespace {
float normal(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}
}

AdiShaperCurve::AdiShaperCurve()
{
    setWantsKeyboardFocus(true);
    setHelpText("Left and right select a point. Up and down change its height. "
                "Hold Shift for fine adjustment. Home and End set the limits. Escape cancels a drag.");
    setSize(340, 132);
}

AdiShaperCurve::~AdiShaperCurve() { finish(Gesture::Cancel, false); }

void AdiShaperCurve::bind(View view, Edit edit)
{
    const auto generation = ++binding_;
    SafePointer<AdiShaperCurve> alive(this);
    cancelGesture();
    if (!alive || binding_ != generation) return;
    view_ = std::move(view);
    for (auto& value : view_.points) value = normal(value);
    edit_ = std::move(edit);
    selected_ = 0;
    bound_ = true;
    announce();
}

void AdiShaperCurve::unbind()
{
    const auto generation = ++binding_;
    SafePointer<AdiShaperCurve> alive(this);
    cancelGesture();
    if (!alive || binding_ != generation) return;
    bound_ = false;
    edit_ = {};
    view_ = {};
    selected_ = 0;
    announce();
}

void AdiShaperCurve::refresh(const Target& target, const Points& values)
{
    if (!bound_ || !(target == view_.target)) return;
    auto next = view_.points;
    for (std::size_t i = 0; i < next.size(); ++i)
        if (static_cast<int>(i) != active_) next[i] = normal(values[i]);
    if (next == view_.points) return;
    view_.points = next;
    announce();
}

juce::Rectangle<float> AdiShaperCurve::plotBounds() const
{
    if (getWidth() < 32 || getHeight() < 42) return {};
    return {9.0f, 27.0f, static_cast<float>(getWidth() - 18),
                          static_cast<float>(getHeight() - 36)};
}

juce::Point<float> AdiShaperCurve::knot(int tick, juce::Rectangle<float> plot) const
{
    const auto index = static_cast<std::size_t>(tick % 8);
    return {plot.getX() + plot.getWidth() * static_cast<float>(tick) / 8,
            plot.getBottom() - plot.getHeight() * view_.points[index]};
}

juce::String AdiShaperCurve::valueText() const
{
    const float value = view_.points[static_cast<std::size_t>(selected_)];
    const auto number = view_.formatNumber ? view_.formatNumber(value) : juce::String(value * 100, 1);
    return view_.units.isEmpty() ? number : number + " " + view_.units;
}

juce::String AdiShaperCurve::pointDescription() const
{
    if (!bound_) return "No curve selected";
    return view_.target.stage + ", " + view_.target.band + ", " + view_.target.contour
         + ", point " + juce::String(selected_ + 1) + " of 8, " + valueText()
         + (view_.interpolation == Interpolation::Hold ? ", hold" : ", linear");
}

void AdiShaperCurve::announce()
{
    repaint();
    setTitle(bound_ ? view_.target.stage + " " + view_.target.contour + " curve" : "Curve");
    setDescription(pointDescription());
    if (auto* handler = getAccessibilityHandler())
        handler->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
}

void AdiShaperCurve::paint(juce::Graphics& g)
{
    g.fillAll(look::display);
    if (!bound_) return;
    const auto plot = plotBounds();
    auto heading = getLocalBounds().reduced(8, 0).removeFromTop(22);
    g.setFont(look::fontSmall);
    g.setColour(look::text);
    const auto readout = juce::String(selected_ + 1) + "/8   " + valueText();
    const auto readoutBounds = heading.removeFromRight(std::min(112, heading.getWidth() / 3));
    g.drawText(readout, readoutBounds, juce::Justification::centredRight);
    g.drawText(view_.target.stage + " / " + view_.target.band + " / " + view_.target.contour,
               heading, juce::Justification::centredLeft);
    if (plot.isEmpty()) return;
    g.setColour(look::gridLine);
    for (int i = 0; i <= 8; ++i) {
        const float x = knot(i, plot).x;
        g.drawLine(x, plot.getY(), x, plot.getBottom());
    }
    for (int i = 0; i <= 4; ++i) {
        const float y = plot.getY() + plot.getHeight() * static_cast<float>(i) / 4;
        g.drawLine(plot.getX(), y, plot.getRight(), y);
    }
    juce::Path path;
    auto previous = knot(0, plot);
    path.startNewSubPath(previous);
    for (int i = 1; i <= 8; ++i) {
        const auto point = knot(i, plot);
        if (view_.interpolation == Interpolation::Hold) path.lineTo(point.x, previous.y);
        path.lineTo(point);
        previous = point;
    }
    g.setColour(isEnabled() ? look::value : look::text.withAlpha(0.5f));
    g.strokePath(path, juce::PathStrokeType(1.8f));
    for (int i = 0; i <= 8; ++i) {
        const auto p = knot(i, plot);
        const bool selected = i % 8 == selected_;
        const auto dot = juce::Rectangle<float>(7, 7).withCentre(p);
        g.setColour(selected ? look::accent : look::text);
        if (i == 8) g.drawEllipse(dot, 1.5f);
        else g.fillEllipse(dot);
        if (selected && hasKeyboardFocus(false)) g.drawEllipse(dot.expanded(2.0f), 1.0f);
    }
}

bool AdiShaperCurve::begin(int point)
{
    if (!bound_ || !isEnabled() || !edit_ || active_ >= 0) return false;
    selected_ = active_ = point;
    original_ = view_.points[static_cast<std::size_t>(point)];
    const auto number = ++gesture_;
    const auto target = view_.target;
    const auto send = edit_;
    SafePointer<AdiShaperCurve> alive(this);
    const bool accepted = send(target, point, Gesture::Begin, original_);
    if (!alive || number != gesture_) return false;
    if (!accepted) active_ = -1;
    announce();
    return accepted;
}

bool AdiShaperCurve::update(float value)
{
    if (active_ < 0) return false;
    value = normal(value);
    auto& current = view_.points[static_cast<std::size_t>(active_)];
    if (current == value) return true;
    const auto number = gesture_;
    const auto target = view_.target;
    const auto send = edit_;
    SafePointer<AdiShaperCurve> alive(this);
    const bool accepted = send(target, active_, Gesture::Value, value);
    if (!alive || number != gesture_) return false;
    if (!accepted) { cancelGesture(); return false; }
    current = value;
    announce();
    return true;
}

void AdiShaperCurve::finish(Gesture event, bool notify)
{
    if (active_ < 0) return;
    const int point = active_;
    const auto target = view_.target;
    const auto send = edit_;
    auto& value = view_.points[static_cast<std::size_t>(point)];
    if (event == Gesture::Cancel) value = original_;
    const float finalValue = value;
    active_ = -1;
    ++gesture_;
    if (notify) announce();
    send(target, point, event, finalValue);
}

void AdiShaperCurve::cancelGesture() { finish(Gesture::Cancel); }

void AdiShaperCurve::setOnce(float value)
{
    if (normal(value) == view_.points[static_cast<std::size_t>(selected_)]) return;
    if (begin(selected_) && update(value)) finish(Gesture::End);
}

void AdiShaperCurve::mouseDown(const juce::MouseEvent& event)
{
    const auto plot = plotBounds();
    if (!bound_ || !isEnabled() || !event.mods.isLeftButtonDown() || event.mods.isPopupMenu()
        || plot.isEmpty() || !plot.expanded(4).contains(event.position)) return;
    const int tick = std::clamp(juce::roundToInt(8 * (event.position.x - plot.getX()) / plot.getWidth()), 0, 8);
    const auto point = knot(tick, plot);
    grabOffset_ = point.getDistanceFrom(event.position) <= 5 ? point.y - event.position.y : 0.0f;
    SafePointer<AdiShaperCurve> alive(this);
    const auto generation = binding_;
    if (isShowing()) grabKeyboardFocus();
    if (!alive || generation != binding_) return;
    if (begin(tick % 8)) update((plot.getBottom() - event.position.y - grabOffset_) / plot.getHeight());
}

void AdiShaperCurve::mouseDrag(const juce::MouseEvent& event)
{
    const auto plot = plotBounds();
    if (active_ >= 0 && !plot.isEmpty())
        update((plot.getBottom() - event.position.y - grabOffset_) / plot.getHeight());
}

void AdiShaperCurve::mouseUp(const juce::MouseEvent&) { finish(Gesture::End); }
void AdiShaperCurve::focusLost(FocusChangeType) { repaint(); cancelGesture(); }
void AdiShaperCurve::enablementChanged() { repaint(); if (!isEnabled()) cancelGesture(); }
void AdiShaperCurve::visibilityChanged() { if (!isVisible()) cancelGesture(); }

bool AdiShaperCurve::keyPressed(const juce::KeyPress& key)
{
    if (!bound_ || !isEnabled()) return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey && active_ >= 0) { cancelGesture(); return true; }
    if (active_ >= 0 || key.getModifiers().isCommandDown() || key.getModifiers().isAltDown()) return false;
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
        selected_ = (selected_ + (code == juce::KeyPress::leftKey ? 7 : 1)) % 8;
        announce();
        return true;
    }
    const float value = view_.points[static_cast<std::size_t>(selected_)];
    const float step = key.getModifiers().isShiftDown() ? .001f : .01f;
    if (code == juce::KeyPress::upKey) setOnce(value + step);
    else if (code == juce::KeyPress::downKey) setOnce(value - step);
    else if (code == juce::KeyPress::homeKey) setOnce(0);
    else if (code == juce::KeyPress::endKey) setOnce(1);
    else return false;
    return true;
}

class AdiShaperCurve::AccessibleValue final : public juce::AccessibilityRangedNumericValueInterface {
public:
    explicit AccessibleValue(AdiShaperCurve& owner) : curve(owner) {}
    bool isReadOnly() const override { return !curve.bound_ || !curve.edit_ || !curve.isEnabled(); }
    double getCurrentValue() const override { return 100.0 * curve.points()[static_cast<std::size_t>(curve.selected_)]; }
    void setValue(double percent) override {
        if (!isReadOnly() && std::isfinite(percent)) curve.setOnce(static_cast<float>(percent / 100));
    }
    AccessibleValueRange getRange() const override { return {{0, 100}, 0.1}; }
private:
    AdiShaperCurve& curve;
};

std::unique_ptr<juce::AccessibilityHandler> AdiShaperCurve::createAccessibilityHandler()
{
    juce::AccessibilityHandler::Interfaces interfaces(std::make_unique<AccessibleValue>(*this));
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::slider,
                                                       juce::AccessibilityActions{}, std::move(interfaces));
}
} // namespace adi::ui
