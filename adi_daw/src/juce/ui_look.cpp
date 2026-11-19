// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ui_look.hpp"
#include "ui_controls.hpp"
#include <array>
namespace adi::ui::look {
juce::Colour trackColour(std::size_t position) {
    static const std::array<juce::uint32, 14> palette{
        0xfff099a7, 0xffeb4cce, 0xffeb4a41, 0xffd45d9e, // Live 11's default set
        0xffffa529, 0xfff7f47c, 0xffbffb00, 0xff1aff2f, 0xff25ffa8,
        0xff5cffe8, 0xff8bc5ff, 0xff5480e4, 0xff92a7ff, 0xffd86ce4};
    return juce::Colour(palette[position % palette.size()]);
}
juce::Colour paletteColour(std::int64_t index) {
    return trackColour(static_cast<std::size_t>(index < 0 ? 0 : index));
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
LiveLook::LiveLook() { setColours(); }
void LiveLook::setColours() {
    setColour(juce::ResizableWindow::backgroundColourId, frame);
    setColour(juce::TextButton::buttonColourId, chooser);
    setColour(juce::TextButton::buttonOnColourId, accent);
    setColour(juce::TextButton::textColourOffId, text);
    setColour(juce::TextButton::textColourOnId, ink);
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
    setColour(juce::Slider::rotarySliderFillColourId, knobRing);
    setColour(juce::Slider::rotarySliderOutlineColourId, knobNeedle);
    setColour(juce::PopupMenu::backgroundColourId, field);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, selection);
    setColour(juce::PopupMenu::highlightedTextColourId, text);
    setColour(juce::ScrollBar::thumbColourId, knobTrack);
    setColour(juce::TooltipWindow::backgroundColourId, tooltip);
    setColour(juce::TooltipWindow::textColourId, text);
    setColour(juce::TooltipWindow::outlineColourId, outline);
    setColour(juce::ToggleButton::textColourId, text);
    setColour(juce::ToggleButton::tickColourId, text);
    setColour(juce::ToggleButton::tickDisabledColourId, textDim);
}
namespace {
// How a lit button is lit: in the record's red, the value's blue, or the accent. A button says so
// itself (`adi.lit`: "record" or "value"), so a theme changed after it was made does not change its
// kind; a button that says nothing is read from the on colour it was given, and is the accent
// otherwise.
controls::Lit litOf(juce::Button &b) {
    const auto kind = b.getProperties()["adi.lit"].toString();
    if (kind == "record")
        return controls::Lit::Record;
    if (kind == "value")
        return controls::Lit::Value;
    const auto on = b.findColour(juce::TextButton::buttonOnColourId);
    return on == record ? controls::Lit::Record : on == value ? controls::Lit::Value : controls::Lit::Accent;
}
juce::Colour litColour(controls::Lit lit) noexcept {
    return lit == controls::Lit::Record ? record : lit == controls::Lit::Value ? value : accent;
}
controls::ButtonState stateOf(juce::Button &b, bool highlighted, bool down) {
    return controls::stateOf(b.getToggleState(), highlighted, down, b.isEnabled(), litOf(b));
}
} // namespace
void LiveLook::drawButtonBackground(juce::Graphics &g, juce::Button &b, const juce::Colour &bg,
                                    bool highlighted, bool down) {
    // The shared button (language.png part B): rest, pointer over, pressed, on, on for record, on
    // for a value, disabled (lit or not). A transparent button is a bar's text, as a device's title:
    // no box.
    const bool on = b.getToggleState();
    if ((on ? b.findColour(juce::TextButton::buttonOnColourId) : bg).isTransparent())
        return;
    const auto r = b.getLocalBounds().toFloat();
    controls::button(g, r, stateOf(b, highlighted, down));
    // Live's half-coloured Solo on a group holding a soloed track (Live 12 manual 18.3): the box cut
    // corner to corner, its lower right half lit.
    if (!on && b.getProperties()["live.half"]) {
        const auto inner = r.reduced(1.5f);
        juce::Path half;
        half.addTriangle(inner.getRight(), inner.getY(), inner.getRight(), inner.getBottom(), inner.getX(),
                         inner.getBottom());
        g.setColour(litColour(litOf(b)));
        g.fillPath(half);
    }
    // Live 12's focus ring, on a switch that only the keyboard can focus.
    if (b.getProperties()["live.focusRing"] && b.hasKeyboardFocus(false)) {
        g.setColour(accent);
        g.drawRoundedRectangle(r.reduced(hairline / 2), std::min(radiusControl, r.getHeight() / 2), hairline);
    }
}
void LiveLook::drawButtonText(juce::Graphics &g, juce::TextButton &b, bool highlighted, bool down) {
    g.setFont(getTextButtonFont(b, b.getHeight()));
    const auto state = stateOf(b, highlighted, down);
    // The label in the state's colour (the ink on a lit button), unless the button chose its own.
    const bool disabled = state == controls::ButtonState::Disabled || state == controls::ButtonState::DisabledOn;
    if (disabled)
        g.setColour(controls::labelColour(state));
    else if (b.getToggleState())
        g.setColour(b.isColourSpecified(juce::TextButton::textColourOnId)
                        ? b.findColour(juce::TextButton::textColourOnId)
                        : controls::labelColour(state));
    else
        g.setColour(b.findColour(juce::TextButton::textColourOffId));
    const bool left = b.getProperties()["live.left"];
    g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(2, 0),
                     left ? juce::Justification::centredLeft : juce::Justification::centred, 1, 1.f);
}
juce::Font LiveLook::getTextButtonFont(juce::TextButton &b, int buttonHeight) {
    // Live's box text: 12 pt in a 13 pt box, smaller only in smaller boxes.
    if (const auto *size = b.getProperties().getVarPointer("live.font"))
        return font(static_cast<float>(static_cast<double>(*size)));
    return font(std::min(fontField, static_cast<float>(buttonHeight) - 1) -
                (b.getProperties()["live.small"] ? 1.f : 0.f));
}
void LiveLook::drawToggleButton(juce::Graphics &g, juce::ToggleButton &b, bool, bool) {
    // The shared switch (language.png part D): a pill, lit when on, its label at its right.
    const auto r = b.getLocalBounds().toFloat();
    // Small enough that the label starts about where the layouts placed it (beside a 14 pt square).
    const float h = std::min(r.getHeight() - 2.f, static_cast<float>(fieldHeight) * 0.6f), w = h * 1.78f;
    const juce::Rectangle<float> pill(r.getX() + 1, r.getCentreY() - h / 2, w, h);
    controls::toggleSwitch(g, pill, b.getToggleState(), b.isEnabled());
    if (b.getButtonText().isNotEmpty()) {
        g.setColour(b.isEnabled() ? text : textFaint);
        g.setFont(font());
        g.drawText(b.getButtonText(), r.withTrimmedLeft(w + 5), juce::Justification::centredLeft);
    }
}
void LiveLook::drawComboBox(juce::Graphics &g, int w, int h, bool, int, int, int, int, juce::ComboBox &) {
    // The shared chooser (language.png part D); its text is the ComboBox's own label.
    controls::chooser(g, juce::Rectangle<float>(0.f, 0.f, static_cast<float>(w), static_cast<float>(h)), {});
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
        // The shared value box (language.png part D): a dark well, filled in the value's colour up
        // to the value; JUCE's own label draws the number over it.
        g.setColour(boxFill);
        g.fillRoundedRectangle(r, radiusBox);
        // A value field dragged up or down (the Track Delay's, manual 18.7): the number alone in
        // the box, as Live's figure shows it; JUCE's vertical bar gives it Live's drag axis.
        if (!s.getProperties()["live.valueField"]) {
            g.setColour(value.withAlpha(0.28f));
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
        }
        g.setColour(boxEdge);
        g.drawRoundedRectangle(r.reduced(hairline / 2), radiusBox, hairline);
        return;
    }
    if (s.isHorizontal()) {
        const float cy = r.getCentreY();
        g.setColour(knobTrack);
        g.fillRect(r.getX(), cy - 1, r.getWidth(), 2.f);
        g.setColour(text);
        g.fillRect(r.getX(), cy - 1, pos - r.getX(), 2.f);
        g.fillRect(pos - 1, cy - 5, 2.f, 10.f);
    } else if (s.getProperties()["live.meterFader"]) {
        // Live's mixer fader: an outlined ◁ beside the meter, at the level.
        const float tx = r.getX() + static_cast<float>(static_cast<int>(s.getProperties()["live.faderX"]));
        juce::Path cap;
        cap.addTriangle(tx, pos, tx + 8, pos - 5, tx + 8, pos + 5);
        g.setColour(field);
        g.fillPath(cap);
        g.setColour(text);
        g.strokePath(cap, juce::PathStrokeType(1.2f));
    } else {
        // A fader: a thin slot and a flat cap, as Live's mixer.
        const float cx = r.getCentreX();
        g.setColour(knobRim);
        g.fillRect(cx - 1, r.getY(), 2.f, r.getHeight());
        g.setColour(text);
        juce::Path cap;
        cap.addTriangle(cx - 7, pos - 5, cx + 7, pos, cx - 7, pos + 5);
        g.fillPath(cap);
    }
}
void LiveLook::drawRotarySlider(juce::Graphics &g, int x, int y, int w, int h, float pos, float, float,
                                juce::Slider &s) {
    // The shared ring knob (language.png part C): the value is the ring. A two-sided knob (pan)
    // fills from its middle; an automated one carries a red dot; the pointer over it lights its dial.
    const auto b = juce::Rectangle<int>(x, y, w, h).toFloat();
    controls::KnobLook k;
    k.fromCentre = s.getProperties()["live.bipolar"];
    k.automated = s.getProperties()["adi.automated"];
    k.disabled = !s.isEnabled();
    k.over = s.isMouseOverOrDragging();
    if (const auto *second = s.getProperties().getVarPointer("adi.secondColour"))
        k.colour = juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(*second)));
    const float radius = std::min(b.getWidth(), b.getHeight()) / 2 - 3.f;
    controls::ringKnob(g, b.getCentre(), radius, pos, k);
}
int LiveLook::getSliderThumbRadius(juce::Slider &s) {
    return s.getProperties()["live.meterFader"] ? 0 : juce::LookAndFeel_V4::getSliderThumbRadius(s);
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
        return font(fontField);
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
juce::Font LiveLook::getPopupMenuFont() { return font(fontField); }
void LiveLook::drawPopupMenuBackground(juce::Graphics &g, int w, int h) {
    g.fillAll(field);
    g.setColour(outline);
    g.drawRect(0, 0, w, h, 1);
}
void LiveLook::drawScrollbar(juce::Graphics &g, juce::ScrollBar &, int x, int y, int w, int h,
                             bool vertical, int thumbStart, int thumbSize, bool, bool) {
    g.setColour(knobTrack);
    if (vertical)
        g.fillRoundedRectangle(static_cast<float>(x + 2), static_cast<float>(thumbStart),
                               static_cast<float>(w - 4), static_cast<float>(thumbSize), radiusThumb);
    else
        g.fillRoundedRectangle(static_cast<float>(thumbStart), static_cast<float>(y + 2),
                               static_cast<float>(thumbSize), static_cast<float>(h - 4), radiusThumb);
}
LiveLook &liveLook() {
    static LiveLook look;
    return look;
}
namespace {
ThemeId currentTheme = ThemeId::Dark;
DensityId currentDensity = DensityId::Compact;
} // namespace
ThemeId themeId() noexcept { return currentTheme; }
DensityId densityId() noexcept { return currentDensity; }
bool apply(ThemeId theme, DensityId density) {
    const bool changed = theme != currentTheme || density != currentDensity;
    currentTheme = theme;
    currentDensity = density;
    const auto &t = themeFor(theme);
#define ADI_LOOK_SET_COLOUR(name) name = juce::Colour(t.name);
    ADI_THEME_ROLES(ADI_LOOK_SET_COLOUR)
#undef ADI_LOOK_SET_COLOUR
    const auto &d = densityFor(density);
#define ADI_LOOK_SET_SIZE(name) name = d.name;
    ADI_DENSITY_ROLES(ADI_LOOK_SET_SIZE, ADI_LOOK_SET_SIZE)
#undef ADI_LOOK_SET_SIZE
    liveLook().setColours();
    return changed;
}
} // namespace adi::ui::look
