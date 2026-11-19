// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Live's look (ADR-0203 d1, d5: "the desktop is built purely as Live"). The palette
// and the measures are read off the director's Live 11.2.7 on his Mac, default theme,
// 100 % zoom, 2026-10-27 (collab/window/look-live11.md has the captures); every
// value here is a measured one unless its comment says otherwise.
#pragma once
#include "adi/ui/density.hpp"
#include "adi/ui/theme.hpp"
#include <juce_gui_basics/juce_gui_basics.h>
namespace adi::ui::look {
using adi::ui::DensityId;
using adi::ui::ThemeId;
using adi::ui::densityFor;
using adi::ui::themeFor;
// --- the theme and the density (DF-W24) --------------------------------------------------
// The window draws only with these. Each role of the current theme (adi/ui/theme.hpp) is a
// look:: colour of the same name, and each measure of the current density (adi/ui/density.hpp) a
// look:: number of the same name. `apply` sets them all, and the JUCE controls' colours, before a
// window is drawn and whenever the settings change; until then they hold Dark and Compact.
#define ADI_LOOK_COLOUR(name) inline juce::Colour name{themeFor(ThemeId::Dark).name};
ADI_THEME_ROLES(ADI_LOOK_COLOUR)
#undef ADI_LOOK_COLOUR
#define ADI_LOOK_INT(name) inline int name = densityFor(DensityId::Compact).name;
#define ADI_LOOK_FLOAT(name) inline float name = densityFor(DensityId::Compact).name;
ADI_DENSITY_ROLES(ADI_LOOK_INT, ADI_LOOK_FLOAT)
#undef ADI_LOOK_INT
#undef ADI_LOOK_FLOAT
// The current pair, and setting it. True when anything changed (every window then repaints).
ThemeId themeId() noexcept;
DensityId densityId() noexcept;
bool apply(ThemeId, DensityId);
// The text on a switch: the theme's text when it is off, the ink on its lit colour when it is on.
inline juce::Colour textOn(bool lit) { return lit ? ink : text; }
// Live's colours for new tracks, in the order its default set shows them, then its
// palette's first row. Measured from the four default tracks; the rest from Live's
// palette, to be read again off Live (AWAITING).
juce::Colour trackColour(std::size_t position);
// A colour of Live's palette by its index, 0 to 69, row-major as Live's chooser lays it out
// (ADR-0211 d1, d3). PROVISIONAL: until the seventy are measured from Live 12's chooser, the
// fourteen above stand in, index % 14.
juce::Colour paletteColour(std::int64_t index);
inline const juce::Colour returnA{0xffc49b40}, returnB{0xfff2a948}, mainTrack{0xffe5742e};
// --- type ---------------------------------------------------------------------------------
juce::Font font(float height = fontBody, bool bold = false);
// --- drawing helpers ------------------------------------------------------------------
void box(juce::Graphics &, juce::Rectangle<float>, juce::Colour fill, bool outlined = true);
void triangle(juce::Graphics &, juce::Rectangle<float>, float angle, juce::Colour);
// Live's flat look for every JUCE control, installed once for the application.
class LiveLook final : public juce::LookAndFeel_V4 {
  public:
    LiveLook();
    // The JUCE controls' colours, from the current theme (again after `apply`).
    void setColours();
    void drawButtonBackground(juce::Graphics &, juce::Button &, const juce::Colour &,
                              bool highlighted, bool down) override;
    void drawButtonText(juce::Graphics &, juce::TextButton &, bool highlighted,
                        bool down) override;
    juce::Font getTextButtonFont(juce::TextButton &, int buttonHeight) override;
    void drawToggleButton(juce::Graphics &, juce::ToggleButton &, bool highlighted,
                          bool down) override;
    void drawComboBox(juce::Graphics &, int width, int height, bool down, int bx, int by,
                      int bw, int bh, juce::ComboBox &) override;
    juce::Font getComboBoxFont(juce::ComboBox &) override;
    void positionComboBoxText(juce::ComboBox &, juce::Label &) override;
    void drawLinearSlider(juce::Graphics &, int x, int y, int w, int h, float pos, float min,
                          float max, juce::Slider::SliderStyle, juce::Slider &) override;
    void drawRotarySlider(juce::Graphics &, int x, int y, int w, int h, float pos, float start,
                          float end, juce::Slider &) override;
    juce::Label *createSliderTextBox(juce::Slider &) override;
    juce::Font getLabelFont(juce::Label &) override;
    void fillTextEditorBackground(juce::Graphics &, int w, int h, juce::TextEditor &) override;
    void drawTextEditorOutline(juce::Graphics &, int w, int h, juce::TextEditor &) override;
    juce::Font getPopupMenuFont() override;
    void drawPopupMenuBackground(juce::Graphics &, int w, int h) override;
    void drawScrollbar(juce::Graphics &, juce::ScrollBar &, int x, int y, int w, int h,
                       bool vertical, int thumbStart, int thumbSize, bool over,
                       bool down) override;
    int getDefaultScrollbarWidth() override { return 8; }
    // Live's mixer fader runs the meter's whole height: no inset for a thumb.
    int getSliderThumbRadius(juce::Slider &) override;
};
// The one instance. Each window's root sets it on itself, and so on all it holds,
// and clears it when it goes, so nothing outlives a look still in use.
LiveLook &liveLook();
} // namespace adi::ui::look
