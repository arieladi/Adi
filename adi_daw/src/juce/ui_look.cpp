// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_look.hpp"
#include <array>
namespace adi::ui::look {
juce::Colour trackColour(std::size_t position) {
    static const std::array<juce::uint32, 14> palette{
        0xfff099a7, 0xffeb4cce, 0xffeb4a41, 0xffd45d9e, // Live 11's default set
        0xffffa529, 0xfff7f47c, 0xffbffb00, 0xff1aff2f, 0xff25ffa8,
        0xff5cffe8, 0xff8bc5ff, 0xff5480e4, 0xff92a7ff, 0xffd86ce4};
    return juce::Colour(palette[position % palette.size()]);
}
juce::Font font(float height, bool bold) {
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}
void box(juce::Graphics &g, juce::Rectangle<float> r, juce::Colour fill, bool outlined) {
    g.setColour(fill);
    g.fillRect(r);
    if (outlined) {
        g.setColour(outline);
        g.drawRect(r, 1.f);
    }
}
void triangle(juce::Graphics &g, juce::Rectangle<float> r, float angle, juce::Colour c) {
    juce::Path p;
    p.addTriangle(r.getX(), r.getY(), r.getRight(), r.getCentreY(), r.getX(), r.getBottom());
    g.setColour(c);
    g.fillPath(p, juce::AffineTransform::rotation(angle, r.getCentreX(), r.getCentreY()));
}
LiveLook::LiveLook() {
    setColour(juce::ResizableWindow::backgroundColourId, frame);
    setColour(juce::TextButton::buttonColourId, chooser);
    setColour(juce::TextButton::buttonOnColourId, accent);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, text);
    setColour(juce::ComboBox::backgroundColourId, chooser);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::outlineColourId, outline);
    setColour(juce::ComboBox::arrowColourId, text);
    setColour(juce::Label::textColourId, text);
    setColour(juce::TextEditor::backgroundColourId, panel);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::highlightColourId, selection);
    setColour(juce::TextEditor::outlineColourId, outline);
    setColour(juce::TextEditor::focusedOutlineColourId, accent);
    setColour(juce::Slider::textBoxTextColourId, text);
    setColour(juce::Slider::textBoxBackgroundColourId, chooser);
    setColour(juce::Slider::textBoxOutlineColourId, outline);
    setColour(juce::Slider::trackColourId, value);
    setColour(juce::Slider::backgroundColourId, chooser);
    setColour(juce::Slider::thumbColourId, text);
    setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff1a1a1a));
    setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff6a6a6a));
    setColour(juce::PopupMenu::backgroundColourId, field);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, selection);
    setColour(juce::PopupMenu::highlightedTextColourId, text);
    setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff6a6a6a));
    setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xfff5f5d0));
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, outline);
    setColour(juce::ToggleButton::textColourId, text);
    setColour(juce::ToggleButton::tickColourId, text);
    setColour(juce::ToggleButton::tickDisabledColourId, textDim);
}
void LiveLook::drawButtonBackground(juce::Graphics &g, juce::Button &b, const juce::Colour &bg,
                                    bool highlighted, bool down) {
    // Live's buttons are flat boxes: light when off, orange when on, a hair darker
    // under the mouse (ADR-0203 d5, hover as Live).
    auto fill = b.getToggleState() ? b.findColour(juce::TextButton::buttonOnColourId) : bg;
    // A transparent button is a bar's text, as a device's title: no box at all.
    if (fill.isTransparent())
        return;
    if (down)
        fill = fill.darker(0.15f);
    else if (highlighted)
        fill = fill.brighter(0.08f);
    box(g, b.getLocalBounds().toFloat().reduced(0.5f), fill);
}
void LiveLook::drawButtonText(juce::Graphics &g, juce::TextButton &b, bool, bool) {
    g.setFont(getTextButtonFont(b, b.getHeight()));
    g.setColour(!b.isEnabled() ? textDim
                : b.getToggleState() ? b.findColour(juce::TextButton::textColourOnId)
                                     : b.findColour(juce::TextButton::textColourOffId));
    const bool left = b.getProperties()["live.left"];
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(2, 0),
                     left ? juce::Justification::centredLeft : juce::Justification::centred, 1, 1.f);
}
juce::Font LiveLook::getTextButtonFont(juce::TextButton &b, int buttonHeight) {
    // Live's box text: 12 pt in a 13 pt box, smaller only in smaller boxes.
    return font(std::min(12.f, static_cast<float>(buttonHeight) - 1) -
                (b.getProperties()["live.small"] ? 1.f : 0.f));
}
void LiveLook::drawToggleButton(juce::Graphics &g, juce::ToggleButton &b, bool highlighted,
                                bool down) {
    // A switch: a small square that turns blue, as Live's device switches do.
    const auto r = b.getLocalBounds().toFloat();
    const float s = std::min(r.getHeight(), 14.f);
    const juce::Rectangle<float> sq(r.getX() + 1, r.getCentreY() - s / 2, s, s);
    box(g, sq, b.getToggleState() ? value : chooser);
    if (highlighted || down)
        g.fillAll(juce::Colours::white.withAlpha(0.04f));
    if (b.getButtonText().isNotEmpty()) {
        g.setColour(text);
        g.setFont(font());
        g.drawText(b.getButtonText(), r.withTrimmedLeft(s + 5), juce::Justification::centredLeft);
    }
}
void LiveLook::drawComboBox(juce::Graphics &g, int w, int h, bool, int, int, int, int,
                            juce::ComboBox &c) {
    box(g, juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(w) - 1, static_cast<float>(h) - 1),
        c.findColour(juce::ComboBox::backgroundColourId));
    triangle(g, {static_cast<float>(w) - 11, static_cast<float>(h) / 2 - 3, 7, 6},
             juce::MathConstants<float>::halfPi, text);
}
juce::Font LiveLook::getComboBoxFont(juce::ComboBox &) { return font(); }
void LiveLook::positionComboBoxText(juce::ComboBox &c, juce::Label &l) {
    l.setBounds(2, 0, c.getWidth() - 14, c.getHeight());
    l.setFont(font());
}
void LiveLook::drawLinearSlider(juce::Graphics &g, int x, int y, int w, int h, float pos,
                                float, float, juce::Slider::SliderStyle style, juce::Slider &s) {
    const juce::Rectangle<float> r(static_cast<float>(x), static_cast<float>(y),
                                   static_cast<float>(w), static_cast<float>(h));
    if (style == juce::Slider::LinearBar || style == juce::Slider::LinearBarVertical) {
        // Live's value box: light, filled in blue up to the value.
        box(g, r.reduced(0.5f), chooser);
        g.setColour(value);
        const auto inner = r.reduced(1.f);
        if (style == juce::Slider::LinearBarVertical)
            g.fillRect(inner.withTop(pos));
        else if (s.getProperties()["live.bipolar"]) {
            // A two-sided value, as pan: filled from the middle to the value.
            const float mid = inner.getCentreX();
            g.fillRect(juce::Rectangle<float>::leftTopRightBottom(std::min(mid, pos), inner.getY(),
                                                                  std::max(mid, pos), inner.getBottom()));
        } else
            g.fillRect(inner.withWidth(std::max(0.f, pos - r.getX() - 1)));
        // Live redraws the edge over the fill.
        g.setColour(outline);
        g.drawRect(r.reduced(0.5f), 1.f);
        return;
    }
    if (s.isHorizontal()) {
        const float cy = r.getCentreY();
        g.setColour(juce::Colour(0xff6a6a6a));
        g.fillRect(r.getX(), cy - 1, r.getWidth(), 2.f);
        g.setColour(text);
        g.fillRect(r.getX(), cy - 1, pos - r.getX(), 2.f);
        g.fillRect(pos - 1, cy - 5, 2.f, 10.f);
    } else {
        // A fader: a thin slot and a flat cap, as Live's mixer.
        const float cx = r.getCentreX();
        g.setColour(juce::Colour(0xff3a3a3a));
        g.fillRect(cx - 1, r.getY(), 2.f, r.getHeight());
        g.setColour(text);
        juce::Path cap;
        cap.addTriangle(cx - 7, pos - 5, cx + 7, pos, cx - 7, pos + 5);
        g.fillPath(cap);
    }
}
void LiveLook::drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h, float pos,
                                float start, float end, juce::Slider &s) {
    // Live's knob: a thick dark ring open at the foot, blue from its start to the
    // value, and a dark needle from the centre that stops short of the ring.
    const auto b = juce::Rectangle<int>(x, y, w, h).toFloat();
    const float r = std::min(b.getWidth(), b.getHeight()) / 2 - 3.f;
    const float cx = b.getCentreX(), cy = b.getCentreY();
    const juce::PathStrokeType stroke(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
    juce::Path ring;
    ring.addCentredArc(cx, cy, r, r, 0, start, end, true);
    g.setColour(s.findColour(juce::Slider::rotarySliderFillColourId));
    g.strokePath(ring, stroke);
    const float a = start + pos * (end - start);
    if (pos > 0.005f) {
        juce::Path fill;
        fill.addCentredArc(cx, cy, r, r, 0, start, a, true);
        g.setColour(value);
        g.strokePath(fill, stroke);
    }
    g.setColour(s.findColour(juce::Slider::rotarySliderFillColourId));
    juce::Path needle;
    needle.startNewSubPath(cx, cy);
    needle.lineTo(cx + 0.72f * r * std::sin(a), cy - 0.72f * r * std::cos(a));
    g.strokePath(needle, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}
juce::Label *LiveLook::createSliderTextBox(juce::Slider &s) {
    auto *l = juce::LookAndFeel_V4::createSliderTextBox(s);
    l->setFont(font());
    l->setJustificationType(juce::Justification::centred);
    l->setColour(juce::Label::textColourId, text);
    l->setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    return l;
}
juce::Font LiveLook::getLabelFont(juce::Label &l) {
    // A value box's number is Live's box text, whatever the slider set.
    if (dynamic_cast<juce::Slider *>(l.getParentComponent()) != nullptr)
        return font(12.f);
    return l.getFont().getHeight() > 0 ? l.getFont() : font();
}
void LiveLook::fillTextEditorBackground(juce::Graphics &g, int w, int h, juce::TextEditor &e) {
    g.setColour(e.findColour(juce::TextEditor::backgroundColourId));
    g.fillRect(0, 0, w, h);
}
void LiveLook::drawTextEditorOutline(juce::Graphics &g, int w, int h, juce::TextEditor &e) {
    g.setColour(e.hasKeyboardFocus(true) ? accent : outline);
    g.drawRect(0, 0, w, h, 1);
}
juce::Font LiveLook::getPopupMenuFont() { return font(12.f); }
void LiveLook::drawPopupMenuBackground(juce::Graphics &g, int w, int h) {
    g.fillAll(field);
    g.setColour(outline);
    g.drawRect(0, 0, w, h, 1);
}
void LiveLook::drawScrollbar(juce::Graphics &g, juce::ScrollBar &, int x, int y, int w, int h,
                             bool vertical, int thumbStart, int thumbSize, bool, bool) {
    g.setColour(juce::Colour(0xff6a6a6a));
    if (vertical)
        g.fillRoundedRectangle(static_cast<float>(x + 2), static_cast<float>(thumbStart),
                               static_cast<float>(w - 4), static_cast<float>(thumbSize), 3.f);
    else
        g.fillRoundedRectangle(static_cast<float>(thumbStart), static_cast<float>(y + 2),
                               static_cast<float>(thumbSize), static_cast<float>(h - 4), 3.f);
}
LiveLook &liveLook() {
    static LiveLook look;
    return look;
}
} // namespace adi::ui::look
