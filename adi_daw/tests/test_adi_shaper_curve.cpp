// SPDX-License-Identifier: AGPL-3.0-or-later
#include "juce/adi_shaper_curve.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
using Curve = adi::ui::AdiShaperCurve;
using Gesture = Curve::Gesture;

void check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

struct Event { Curve::Target target; int point; Gesture gesture; float value; };
struct Recorder {
    std::vector<Event> events;
    Curve::Edit callback() {
        return [this](const auto& target, int point, Gesture gesture, float value) {
            events.push_back({target, point, gesture, value});
            return true;
        };
    }
};

Curve::View initial()
{
    Curve::View view;
    view.target = {41, "Pitch", "Low", "Transpose"};
    view.units = "st";
    view.formatNumber = [](float value) { return juce::String(48 * value - 24, 1); };
    view.points = {.125f,.875f,.125f,.375f,.625f,.25f,.75f,.875f};
    return view;
}

juce::Point<float> at(Curve& curve, int tick, float height)
{
    const auto plot = curve.plotBounds();
    return {plot.getX() + plot.getWidth() * static_cast<float>(tick) / 8,
            plot.getBottom() - plot.getHeight() * height};
}

juce::MouseEvent mouse(Curve& curve, juce::Point<float> pos, int mods = juce::ModifierKeys::leftButtonModifier)
{
    return {juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(mods),
            0,0,0,0,0, &curve,&curve, {},pos,{},1,false};
}

void mouseAndBinding()
{
    Recorder log;
    Curve curve;
    auto view = initial();
    curve.bind(view, log.callback());
    check(curve.pointDescription().contains("Pitch, Low, Transpose, point 1 of 8, -18.0 st"),
          "Selected point exposes engineering units and band");
    curve.mouseDown(mouse(curve, {5, 3}));
    curve.mouseDown(mouse(curve, at(curve, 3, .8f), juce::ModifierKeys::rightButtonModifier));
    check(log.events.empty(), "Header and context click cannot change the curve");
    curve.mouseDown(mouse(curve, at(curve, 8, .125f)));
    curve.mouseDrag(mouse(curve, at(curve, 5, .75f)));
    curve.mouseDrag(mouse(curve, at(curve, 2, .875f)));
    auto snapshot = view.points;
    snapshot[0] = .4f; snapshot[4] = .4f;
    curve.refresh(view.target, snapshot);
    check(curve.points()[0] == .875f && curve.points()[4] == .4f, "Snapshot echo preserves the active point");
    curve.mouseUp(mouse(curve, at(curve, 2, .875f)));
    check(log.events.size() == 4 && log.events[0].gesture == Gesture::Begin &&
          log.events[1].gesture == Gesture::Value && log.events[2].gesture == Gesture::Value &&
          log.events[3].gesture == Gesture::End, "A drag has one undo gesture");
    for (const auto& event : log.events)
        check(event.target == view.target && event.point == 0, "Wrap handle belongs to point one throughout drag");

    curve.mouseDown(mouse(curve, at(curve, 1, .875f)));
    curve.mouseDrag(mouse(curve, at(curve, 6, -3)));
    check(curve.points()[1] == 0, "Dragging out of bounds clamps height");
    auto other = initial();
    other.target.device = 85; other.target.band = "High";
    curve.bind(other, log.callback());
    const auto last = log.events.back();
    check(last.target == view.target && last.point == 1 && last.gesture == Gesture::Cancel &&
          last.value == .875f, "Rebind cancels against old device and original value");
    const auto oldSize = log.events.size();
    curve.refresh(view.target, snapshot);
    curve.mouseDrag(mouse(curve, at(curve, 5, .5f)));
    curve.mouseUp(mouse(curve, at(curve, 1, .875f)));
    check(curve.points() == other.points && log.events.size() == oldSize,
          "A stale drag, snapshot or release cannot edit the new device");
    curve.mouseDown(mouse(curve, at(curve, 5, .25f)));
    curve.mouseDrag(mouse(curve, at(curve, 7, .5f)));
    check(curve.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && curve.points()[5] == .25f &&
          log.events.back().gesture == Gesture::Cancel, "Escape restores the point");
}

void keyboardAndAccessibility()
{
    Recorder log;
    Curve curve;
    curve.bind(initial(), log.callback());
    curve.keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
    check(curve.selectedPoint() == 7, "Keyboard selection wraps backwards");
    curve.keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
    curve.keyPressed(juce::KeyPress(juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    check(std::abs(curve.points()[0] - .126f) < .00001f && log.events.size() == 3,
          "Fine adjustment generates one complete gesture");
    curve.keyPressed(juce::KeyPress(juce::KeyPress::endKey));
    check(curve.points()[0] == 1, "End selects the top value");
    curve.keyPressed(juce::KeyPress(juce::KeyPress::homeKey));
    check(curve.points()[0] == 0, "Home selects the bottom value");
    auto handler = curve.createAccessibilityHandler();
    auto* value = handler->getValueInterface();
    check(value && !value->isReadOnly() && value->getRange().isValid(), "Accessible slider provides an editable range");
    value->setValue(75);
    check(std::abs(value->getCurrentValue() - 75) < .00001 &&
          handler->getDescription().contains("12.0 st"), "Accessible edit announces the selected point in units");
    curve.setEnabled(false);
    check(value->isReadOnly(), "Disabled curve is read-only");
    value->setValue(40);
    check(curve.points()[0] == .75f, "Disabled curve refuses accessible writes");
}

void reentrancyAndLifetime()
{
    Recorder log;
    auto curve = std::make_unique<Curve>();
    const auto old = initial();
    auto replacement = initial(); replacement.target.contour = "Grain";
    curve->bind(old, [&](const auto& target, int point, Gesture gesture, float value) {
        log.events.push_back({target, point, gesture, value});
        if (gesture == Gesture::Value) curve->bind(replacement, log.callback());
        return true;
    });
    curve->mouseDown(mouse(*curve, at(*curve, 3, .375f)));
    curve->mouseDrag(mouse(*curve, at(*curve, 3, .1f)));
    check(log.events.size() == 3 && log.events.back().gesture == Gesture::Cancel &&
          curve->points() == replacement.points, "Synchronous replacement cannot apply an old value to new data");
    curve->mouseDown(mouse(*curve, at(*curve, 2, .125f)));
    curve.reset();
    check(log.events.back().gesture == Gesture::Cancel && log.events.back().target == replacement.target,
          "Destruction closes the original gesture");
    Curve refused;
    refused.bind(old, [&](const auto&, int, Gesture gesture, float) { return gesture != Gesture::Value; });
    refused.keyPressed(juce::KeyPress(juce::KeyPress::endKey));
    check(refused.points() == old.points, "A refused value restores its begin value");
}

juce::Image render(Curve& curve)
{
    juce::Image image(juce::Image::ARGB, curve.getWidth(), curve.getHeight(), true);
    juce::Graphics graphics(image);
    curve.paintEntireComponent(graphics, true);
    return image;
}

void rendering()
{
    Curve curve;
    auto view = initial();
    curve.bind(view, {});
    auto linear = render(curve);
    view.interpolation = Curve::Interpolation::Hold;
    curve.bind(view, {});
    auto held = render(curve);
    const auto plot = curve.plotBounds();
    const auto mid = at(curve, 0, .5f).translated(plot.getWidth() / 16, 0).roundToInt();
    const auto low = at(curve, 0, .125f).translated(plot.getWidth() / 16, 0).roundToInt();
    const auto last = at(curve, 8, .5f).translated(-plot.getWidth() / 16, 0).roundToInt();
    check(linear.getPixelAt(mid.x, mid.y) != held.getPixelAt(mid.x, mid.y) &&
          linear.getPixelAt(low.x, low.y) != held.getPixelAt(low.x, low.y) &&
          linear.getPixelAt(last.x, last.y) != held.getPixelAt(last.x, last.y),
          "Linear, hold, and last-to-first segments render at their measured positions");
    view.points[2] = std::numeric_limits<float>::quiet_NaN();
    view.points[6] = 4;
    curve.bind(view, {});
    check(curve.points()[2] == 0 && curve.points()[6] == 1, "Invalid display snapshots are bounded");
    curve.setSize(8, 8);
    check(curve.plotBounds().isEmpty() && render(curve).isValid(), "Tiny editor has a safe empty plot");
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    mouseAndBinding();
    keyboardAndAccessibility();
    reentrancyAndLifetime();
    rendering();
    std::puts("ADi-SHAPER curve tests passed");
}
