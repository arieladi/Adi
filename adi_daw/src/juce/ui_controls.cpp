// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_controls.hpp"
#include <algorithm>
#include <cmath>
namespace adi::ui::controls {
namespace {
// The sheet's arc: degrees from twelve o'clock, clockwise, as JUCE's radians.
float radians(float degrees) noexcept { return degrees * juce::MathConstants<float>::pi / 180.f; }
juce::Point<float> polar(juce::Point<float> c, float r, float degrees) noexcept {
    const float a = radians(degrees);
    return {c.x + r * std::sin(a), c.y - r * std::cos(a)};
}
void stroke(juce::Graphics &g, juce::Point<float> c, float r, float from, float to, float width) {
    if (std::abs(to - from) < 0.01f)
        return;
    juce::Path arc;
    arc.addCentredArc(c.x, c.y, r, r, 0.f, radians(std::min(from, to)), radians(std::max(from, to)), true);
    g.strokePath(arc, juce::PathStrokeType(width, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
}
void label(juce::Graphics &g, juce::Rectangle<float> r, const juce::String &text, juce::Colour colour, float size,
           bool strong, juce::Justification where = juce::Justification::centred, bool cut = false) {
    if (text.isEmpty())
        return;
    g.setColour(colour);
    g.setFont(look::font(size, strong));
    if (cut) {
        // Cut at the box's edge, as Live cuts "All Channe" (Live 12 manual, Figure 6.1): no ellipsis.
        juce::Graphics::ScopedSaveState keep(g);
        g.reduceClipRegion(r.toNearestInt());
        g.drawText(text, r, where, false);
        return;
    }
    g.drawFittedText(text, r.toNearestInt(), where, 1, 1.f);
}
constexpr float kKnobStart = -135.f, kKnobEnd = 135.f;
} // namespace
ButtonState stateOf(bool on, bool over, bool down, bool enabled, Lit lit) noexcept {
    if (!enabled)
        return on ? ButtonState::DisabledOn : ButtonState::Disabled;
    if (on)
        return lit == Lit::Record ? ButtonState::OnRecord : lit == Lit::Value ? ButtonState::OnValue : ButtonState::On;
    if (down)
        return ButtonState::Pressed;
    return over ? ButtonState::Over : ButtonState::Rest;
}
juce::Colour labelColour(ButtonState state) noexcept {
    switch (state) {
    case ButtonState::On:
    case ButtonState::OnRecord:
    case ButtonState::OnValue:
        return look::ink;
    case ButtonState::Disabled:
        return look::textFaint;
    case ButtonState::DisabledOn:
        return look::ink.withMultipliedAlpha(0.6f);
    case ButtonState::Rest:
    case ButtonState::Over:
    case ButtonState::Pressed:
        break;
    }
    return look::text;
}
void button(juce::Graphics &g, juce::Rectangle<float> r, ButtonState state, const juce::String &text, float fontSize) {
    juce::Colour fill = look::buttonRest, edge = look::buttonEdge;
    bool edged = true;
    switch (state) {
    case ButtonState::Over:
        fill = look::buttonHover;
        edge = look::buttonHoverEdge;
        break;
    case ButtonState::Pressed:
        fill = look::buttonPressed;
        edge = look::buttonPressedEdge;
        break;
    case ButtonState::On:
        fill = look::accent;
        edged = false;
        break;
    case ButtonState::OnRecord:
        fill = look::record;
        edged = false;
        break;
    case ButtonState::OnValue:
        fill = look::value;
        edged = false;
        break;
    case ButtonState::Disabled:
        fill = look::buttonDisabled;
        edge = look::buttonDisabledEdge;
        break;
    case ButtonState::DisabledOn:
        fill = look::accent.withMultipliedAlpha(0.45f);
        edged = false;
        break;
    case ButtonState::Rest:
        break;
    }
    const float radius = std::min(look::radiusControl, r.getHeight() / 2);
    g.setColour(fill);
    g.fillRoundedRectangle(r, radius);
    if (edged) {
        g.setColour(edge);
        g.drawRoundedRectangle(r.reduced(look::hairline / 2), radius, look::hairline);
    }
    label(g, r.reduced(2.f, 0.f), text, labelColour(state), fontSize, true);
}
void roundButton(juce::Graphics &g, juce::Rectangle<float> r, ButtonState state) {
    // The button's fill and edge for the state, on a circle (part B's states, round).
    juce::Colour fill = look::buttonRest, edge = look::buttonEdge;
    switch (state) {
    case ButtonState::Over:
        fill = look::buttonHover;
        edge = look::buttonHoverEdge;
        break;
    case ButtonState::Pressed:
        fill = look::buttonPressed;
        edge = look::buttonPressedEdge;
        break;
    case ButtonState::On:
        fill = edge = look::accent;
        break;
    case ButtonState::OnRecord:
        fill = edge = look::record;
        break;
    case ButtonState::OnValue:
        fill = edge = look::value;
        break;
    case ButtonState::Disabled:
        fill = look::buttonDisabled;
        edge = look::buttonDisabledEdge;
        break;
    case ButtonState::DisabledOn:
        fill = edge = look::accent.withMultipliedAlpha(0.45f);
        break;
    case ButtonState::Rest:
        break;
    }
    g.setColour(fill);
    g.fillEllipse(r);
    g.setColour(edge);
    g.drawEllipse(r.reduced(look::hairline / 2), look::hairline);
}
void group(juce::Graphics &g, juce::Rectangle<float> r) {
    const float radius = std::min(look::radiusGroup, r.getHeight() / 2);
    g.setColour(look::group);
    g.fillRoundedRectangle(r, radius);
    g.setColour(look::groupEdge);
    g.drawRoundedRectangle(r.reduced(look::hairline / 2), radius, look::hairline);
}
void separator(juce::Graphics &g, float x, float top, float height) {
    g.setColour(look::groupSeparator);
    const float inset = std::min(4.f, height / 4);
    g.drawLine(x, top + inset, x, top + height - inset, look::hairline);
}
void groupCell(juce::Graphics &g, juce::Rectangle<float> r, juce::Colour fill) {
    g.setColour(fill);
    g.fillRoundedRectangle(r.reduced(2.f), std::min(look::radiusPanel, r.getHeight() / 2 - 2.f));
}
void frozenTrackBadge(juce::Graphics &g, juce::Rectangle<float> bounds) {
    g.setColour(look::value);
    g.fillEllipse(bounds);
    g.setColour(look::ink);
    const auto centre = bounds.getCentre();
    const auto radius = bounds.getWidth() * .31f;
    for (const float angle : {0.f, 60.f, 120.f})
        g.drawLine({polar(centre, radius, angle), polar(centre, radius, angle + 180.f)}, 1.f);
}
float ringWidth(float radius) noexcept { return std::max(2.2f, radius * 0.17f); }
void ringKnob(juce::Graphics &g, juce::Point<float> c, float r, float value, const KnobLook &k) {
    value = std::clamp(value, 0.f, 1.f);
    const float rw = ringWidth(r), at = kKnobStart + (kKnobEnd - kKnobStart) * value;
    const auto colour = k.colour.value_or(look::value);
    // The ring, then the value along it: from the start, or from the middle for a two-sided knob.
    g.setColour(look::knobRing);
    stroke(g, c, r, kKnobStart, kKnobEnd, rw);
    g.setColour(k.disabled ? colour.withMultipliedAlpha(0.35f) : colour);
    if (k.fromCentre)
        stroke(g, c, r, std::min(0.f, at), std::max(0.f, at) + 0.01f, rw);
    else
        stroke(g, c, r, kKnobStart, at + 0.01f, rw);
    // A modulated knob: a second, thin arc just outside the ring.
    if (k.modulation) {
        g.setColour(look::modulation);
        const float m0 = kKnobStart + (kKnobEnd - kKnobStart) * std::clamp(k.modulation->first, 0.f, 1.f),
                    m1 = kKnobStart + (kKnobEnd - kKnobStart) * std::clamp(k.modulation->second, 0.f, 1.f);
        stroke(g, c, r + rw + 1.5f, m0, m1, 1.6f);
    }
    // The dial, lit from the upper left, lighter under the pointer.
    const float dial = r - rw - 1.5f;
    if (dial > 1.f) {
        const auto high = k.over ? look::knobFaceHoverHigh : look::knobFaceHigh;
        const auto low = k.over ? look::knobFaceHoverLow : look::knobFaceLow;
        juce::ColourGradient face(high, c.x - dial * 0.3f, c.y - dial * 0.44f, low, c.x + dial * 0.9f, c.y + dial * 0.9f,
                                  true);
        g.setGradientFill(face);
        g.fillEllipse(juce::Rectangle<float>(dial * 2, dial * 2).withCentre(c));
        g.setColour(look::knobRing);
        g.drawEllipse(juce::Rectangle<float>(dial * 2, dial * 2).withCentre(c), look::hairline);
    }
    // The needle, from near the centre to short of the ring.
    const float reach = std::max(1.f, r - rw - 3.f);
    g.setColour(k.disabled ? look::knobNeedleOff : look::knobNeedle);
    juce::Path needle;
    needle.startNewSubPath(polar(c, reach * 0.3f, at));
    needle.lineTo(polar(c, reach, at));
    g.strokePath(needle, juce::PathStrokeType(std::max(1.4f, r * 0.09f), juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    // An automated knob: a red dot at its upper right.
    if (k.automated) {
        g.setColour(look::recordLed);
        g.fillEllipse(juce::Rectangle<float>(5.2f, 5.2f).withCentre({c.x + r, c.y - r}));
    }
}
void valueBox(juce::Graphics &g, juce::Rectangle<float> r, const juce::String &text, std::optional<float> fill,
              float fontSize) {
    g.setColour(look::boxFill);
    g.fillRoundedRectangle(r, look::radiusBox);
    g.setColour(look::boxEdge);
    g.drawRoundedRectangle(r.reduced(look::hairline / 2), look::radiusBox, look::hairline);
    if (fill) {
        const auto inner = r.reduced(1.f);
        g.setColour(look::value.withAlpha(0.28f));
        g.fillRoundedRectangle(inner.withWidth(inner.getWidth() * std::clamp(*fill, 0.f, 1.f)),
                               std::max(0.f, look::radiusBox - 1.f));
    }
    label(g, r.reduced(3.f, 0.f), text, look::text, fontSize, false);
}
void chooser(juce::Graphics &g, juce::Rectangle<float> r, const juce::String &text, float fontSize, bool enabled) {
    g.setColour(look::buttonRest);
    g.fillRoundedRectangle(r, look::radiusBox);
    g.setColour(look::buttonEdge);
    g.drawRoundedRectangle(r.reduced(look::hairline / 2), look::radiusBox, look::hairline);
    label(g, r.withTrimmedLeft(6.f).withTrimmedRight(16.f), text, enabled ? look::text : look::textDim, fontSize, false,
          juce::Justification::centredLeft, true);
    juce::Path arrow;
    const float ax = r.getRight() - 12.f, ay = r.getCentreY() - 2.f;
    arrow.startNewSubPath(ax, ay);
    arrow.lineTo(ax + 3.5f, ay + 3.5f);
    arrow.lineTo(ax + 7.f, ay);
    g.setColour(look::textDim);
    g.strokePath(arrow, juce::PathStrokeType(look::line, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
juce::Rectangle<float> oneOfCell(juce::Rectangle<float> r, int count, int index) noexcept {
    const float w = r.getWidth() / static_cast<float>(std::max(1, count));
    return {r.getX() + w * static_cast<float>(index), r.getY(), w, r.getHeight()};
}
juce::Rectangle<float> oneOfCell(juce::Rectangle<float> r, const juce::StringArray &labels, int index, float fontSize) {
    // Each choice as wide as its label wants, the box shared in that proportion, so a long one
    // ("Auto") is not cut where a short one ("In") has room to spare.
    const auto font = look::font(fontSize, true);
    float total = 0, before = 0, own = 0;
    for (int i = 0; i < labels.size(); ++i) {
        const float want = juce::GlyphArrangement::getStringWidth(font, labels[i]) + 2 * look::hairline;
        if (i < index)
            before += want;
        if (i == index)
            own = want;
        total += want;
    }
    if (total <= 0)
        return oneOfCell(r, labels.size(), index);
    const float scale = r.getWidth() / total;
    return {r.getX() + before * scale, r.getY(), own * scale, r.getHeight()};
}
void oneOf(juce::Graphics &g, juce::Rectangle<float> r, const juce::StringArray &labels, int lit, float fontSize) {
    const float radius = std::min(look::radiusPanel, r.getHeight() / 2);
    g.setColour(look::group);
    g.fillRoundedRectangle(r, radius);
    g.setColour(look::groupEdge);
    g.drawRoundedRectangle(r.reduced(look::hairline / 2), radius, look::hairline);
    for (int i = 0; i < labels.size(); ++i) {
        const auto cell = oneOfCell(r, labels, i, fontSize);
        if (i == lit) {
            const float inset = std::min(2.f, cell.getHeight() / 8);
            g.setColour(look::accent);
            g.fillRoundedRectangle(cell.reduced(inset), std::min(look::radiusBox, cell.getHeight() / 2 - inset));
        }
        label(g, cell, labels[i], i == lit ? look::ink : look::textDim, fontSize, true);
    }
}
void toggleSwitch(juce::Graphics &g, juce::Rectangle<float> r, bool on, bool enabled) {
    const float radius = r.getHeight() / 2, knob = std::max(1.f, radius - 2.f);
    g.setColour(on ? look::accent : look::switchOff);
    g.fillRoundedRectangle(r, radius);
    if (!on) {
        g.setColour(look::boxEdge);
        g.drawRoundedRectangle(r.reduced(look::hairline / 2), radius, look::hairline);
    }
    const float cx = on ? r.getRight() - radius : r.getX() + radius;
    g.setColour(on ? look::ink : look::textDim);
    g.fillEllipse(juce::Rectangle<float>(knob * 2, knob * 2).withCentre({cx, r.getCentreY()}));
    if (!enabled) {
        g.setColour(look::buttonDisabled.withAlpha(0.6f));
        g.fillRoundedRectangle(r, radius);
    }
}
juce::Rectangle<float> tabCell(juce::Rectangle<float> r, int count, int index) noexcept {
    return oneOfCell(r.reduced(3.f), count, index);
}
void tabs(juce::Graphics &g, juce::Rectangle<float> r, const juce::StringArray &labels, int current, float fontSize) {
    g.setColour(look::tabsBack);
    g.fillRoundedRectangle(r, std::min(look::radiusPanel + 1.f, r.getHeight() / 2));
    for (int i = 0; i < labels.size(); ++i) {
        const auto cell = tabCell(r, labels.size(), i);
        if (i == current) {
            g.setColour(look::tabOn);
            g.fillRoundedRectangle(cell, look::radiusControl);
            g.setColour(look::tabOnEdge);
            g.drawRoundedRectangle(cell.reduced(look::hairline / 2), look::radiusControl, look::hairline);
        }
        label(g, cell, labels[i], i == current ? look::text : look::textDim, fontSize, i == current);
    }
}
} // namespace adi::ui::controls
