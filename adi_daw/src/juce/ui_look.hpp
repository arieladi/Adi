// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Live's look (ADR-0203 d1, d5: "the desktop is built purely as Live"). The palette
// and the measures are read off the director's Live 11.2.7 on his Mac, default theme,
// 100 % zoom, 2026-10-27 (collab/window/look-live11.md has the captures); every
// value here is a measured one unless its comment says otherwise.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace adi::ui::look {
// --- the palette ------------------------------------------------------------------------
inline const juce::Colour frame{0xff707070};     // the window, the control bar, gaps
inline const juce::Colour panel{0xff8f8f8f};     // browser, headers, Info View, devices
inline const juce::Colour field{0xffaaaaaa};     // control bar fields, title bars
inline const juce::Colour chooser{0xffdcdcdc};   // choosers and off switches
inline const juce::Colour columnHead{0xff808080}; // the browser's "Name" header
inline const juce::Colour band{0xff505050};      // the rulers, the right-hand strip
inline const juce::Colour lane{0xff858585};      // an arrangement lane
inline const juce::Colour laneAlt{0xff8f8f8f};   // every other bar group
inline const juce::Colour laneSelected{0xff9f9f9f};
inline const juce::Colour laneSelectedAlt{0xffaaaaaa};
inline const juce::Colour gridLine{0xff6e6e6e};
inline const juce::Colour separator{0xff505050}; // between tracks, 2 pt
inline const juce::Colour button{0xffb7b7b7};    // a switch that is off (S, arm, In)
inline const juce::Colour display{0xff191919};   // a device's display
inline const juce::Colour text{0xff000000};
inline const juce::Colour textDim{0xff4d4d4d};
inline const juce::Colour textOnBand{0xffc8c8c8}; // ruler numbers
inline const juce::Colour accent{0xfff4b43e};     // an on switch, the activator
inline const juce::Colour value{0xff7ddbf3};      // a value box's fill
inline const juce::Colour selection{0xffc7e8fd};  // a selected browser row
inline const juce::Colour outline{0xff444444};   // every box's 1 pt edge
// Live's colours for new tracks, in the order its default set shows them, then its
// palette's first row. Measured from the four default tracks; the rest from Live's
// palette, to be read again off Live (AWAITING).
juce::Colour trackColour(std::size_t position);
inline const juce::Colour returnA{0xffc49b40}, returnB{0xfff2a948}, mainTrack{0xffe5742e};
// --- the measures, in points at 100 % ---------------------------------------------------
inline constexpr int controlBarHeight = 35, gap = 8, statusBarHeight = 38;
inline constexpr int fieldHeight = 17;
inline constexpr int detailHeight = 196; // Info View and Device View with their 4 pt edges
inline constexpr int border = 4;         // a panel's dark edge
// --- type ---------------------------------------------------------------------------------
juce::Font font(float height = 11.5f, bool bold = false);
// --- drawing helpers ------------------------------------------------------------------
void box(juce::Graphics &, juce::Rectangle<float>, juce::Colour fill, bool outlined = true);
void triangle(juce::Graphics &, juce::Rectangle<float>, float angle, juce::Colour);
// Live's flat look for every JUCE control, installed once for the application.
class LiveLook final : public juce::LookAndFeel_V4 {
  public:
    LiveLook();
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
