// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The shared controls (DF-W24 step 3; ADR-0244 d2; collab/window/design/language.png parts B, C
// and D, drawn by collab/window/design/src/lib.js): one painter for each control the window draws,
// used by every surface (the track rows, the Mixer, the Device View, the Clip View, the Browser,
// the dialogs) and by LiveLook for JUCE's own controls. Colours from the theme, sizes from the
// density (adi/ui/theme.hpp, density.hpp, read through look::); a layout gives each its bounds.
#pragma once
#include "ui_look.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include <utility>
namespace adi::ui::controls {
// A button's state, as part B shows them left to right.
enum class ButtonState { Rest, Over, Pressed, On, OnRecord, OnValue, Disabled };
// The state from what JUCE knows: lit (and how), the pointer over it, pressed, enabled.
enum class Lit { Accent, Record, Value };
ButtonState stateOf(bool on, bool over, bool down, bool enabled, Lit lit = Lit::Accent) noexcept;
// A button: a rounded box in its state's colours, its label (or nothing) centred in it.
void button(juce::Graphics &, juce::Rectangle<float>, ButtonState, const juce::String &label = {},
            float fontSize = look::fontBody);
// A round button (the Status Bar's and the Browser's, a selector): the button's colours in a circle.
void roundButton(juce::Graphics &, juce::Rectangle<float>, ButtonState);
// The colour a button's label or glyph takes in a state: the ink on a lit button.
juce::Colour labelColour(ButtonState) noexcept;
// A rounded group (part B: related buttons share one), and the line between two of its buttons.
void group(juce::Graphics &, juce::Rectangle<float>);
void separator(juce::Graphics &, float x, float top, float height);
// A lit cell inside a group (the playing transport, an on switch in the Control Bar).
void groupCell(juce::Graphics &, juce::Rectangle<float>, juce::Colour fill);
// The ring knob (part C): the value is the ring. `radius` from look::knobSmall, knobMedium or
// knobLarge, or the knob's bounds.
struct KnobLook {
    bool fromCentre = false;     // a two-sided knob (pan) fills its ring from the middle
    bool automated = false;      // a red dot at its upper right
    bool disabled = false;       // grey
    bool over = false;           // the pointer over it: a lighter dial
    std::optional<std::pair<float, float>> modulation; // a second arc outside the ring, 0 to 1
    std::optional<juce::Colour> colour;                 // the second colour; the value's otherwise
};
void ringKnob(juce::Graphics &, juce::Point<float> centre, float radius, float value, const KnobLook & = {});
// The ring's width for a radius, as the sheet's.
float ringWidth(float radius) noexcept;
// A value box (part D): a dark well, its value's part filled in the value's colour, its text.
void valueBox(juce::Graphics &, juce::Rectangle<float>, const juce::String &text,
              std::optional<float> fill = std::nullopt, float fontSize = look::fontBody);
// A chooser: a control-coloured box, its text at the left and the arrow at the right; a disabled
// one keeps its text, dim.
void chooser(juce::Graphics &, juce::Rectangle<float>, const juce::String &text, float fontSize = look::fontBody,
             bool enabled = true);
// One of several (part D's one-of-three): a group with the lit one in the accent.
void oneOf(juce::Graphics &, juce::Rectangle<float>, const juce::StringArray &labels, int lit,
           float fontSize = look::fontBody);
// Where each of `count` equal choices sits inside a box (the tabs'); and where each of a one-of's
// labels sits, each as wide as it wants, for hit tests.
juce::Rectangle<float> oneOfCell(juce::Rectangle<float>, int count, int index) noexcept;
juce::Rectangle<float> oneOfCell(juce::Rectangle<float>, const juce::StringArray &labels, int index,
                                 float fontSize = look::fontBody);
// A switch: a pill track, its knob at the right and the track lit when on.
void toggleSwitch(juce::Graphics &, juce::Rectangle<float>, bool on, bool enabled = true);
// Tabs: a ground, the chosen tab raised, the others dim.
void tabs(juce::Graphics &, juce::Rectangle<float>, const juce::StringArray &labels, int current,
          float fontSize = look::fontField);
juce::Rectangle<float> tabCell(juce::Rectangle<float>, int count, int index) noexcept;
} // namespace adi::ui::controls
