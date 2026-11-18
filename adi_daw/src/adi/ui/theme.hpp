// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The window's colours (DF-W24; ADR-0244 d2): one table per theme, every colour the window draws
// with named by its role. No drawing code holds a colour of its own: it asks the current theme
// (`look::` in the JUCE layer reads it). Plain ARGB words, so the table needs no JUCE and the
// headless tests read it as the window does.
//
// The themes (collab/window/design/README.md, language.png part G):
// - Dark, the default: Live 12's dark, as the sheets draw it (`lib.js`'s `C`; the Arrangement
//   sheet's ruler, grid and selection).
// - Mid grey: the window as built before the sheets, value for value, so a render in it matches
//   the goldens drawn before them.
// - Light and Mix View (darker, cooler): the sheet gives six swatches of each (window, panel,
//   control, text, accent, value). Every other grey is placed on the same ramp as Dark's (the
//   rule is in theme.cpp); they wait for win's drawing of the full set (DF-W28).
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
namespace adi::ui {
enum class ThemeId { Dark, MidGrey, Light, MixView };
// Every role, with what it colours. ADI_THEME_ROLE(name) is applied to each in order: Theme's
// members, and the JUCE layer's look:: names, come from this one list.
#define ADI_THEME_ROLES(ADI_THEME_ROLE) \
    /* surfaces */ \
    ADI_THEME_ROLE(frame) /* the window, its gaps, the Control Bar's ground */ \
    ADI_THEME_ROLE(panel) /* the Browser, the track headers, the Info View, devices */ \
    ADI_THEME_ROLE(field) /* the Control Bar's fields, title bars, a header's top */ \
    ADI_THEME_ROLE(chooser) /* a chooser, a switch that is off */ \
    ADI_THEME_ROLE(columnHead) /* the Browser's "Name" header */ \
    ADI_THEME_ROLE(band) /* the rulers, the strip at the outer edge */ \
    ADI_THEME_ROLE(lane) /* an Arrangement lane */ \
    ADI_THEME_ROLE(laneAlt) /* every other bar group of a lane */ \
    ADI_THEME_ROLE(laneSelected) /* a selected track's lane */ \
    ADI_THEME_ROLE(laneSelectedAlt) /* its other bar groups */ \
    ADI_THEME_ROLE(gridLine) /* the grid over the lanes */ \
    ADI_THEME_ROLE(separator) /* between tracks */ \
    ADI_THEME_ROLE(button) /* a button that is off (S, Arm, In) */ \
    ADI_THEME_ROLE(display) /* a device's display, a meter's well */ \
    ADI_THEME_ROLE(outline) /* a box's 1 pt edge */ \
    ADI_THEME_ROLE(browserEdge) /* the Browser's dark block behind its panes */ \
    ADI_THEME_ROLE(windowBack) /* the desktop window's own ground, behind everything */ \
    ADI_THEME_ROLE(editorBack) /* a plug-in editor's window, behind the editor */ \
    /* text */ \
    ADI_THEME_ROLE(text) /* text on the theme's surfaces */ \
    ADI_THEME_ROLE(textDim) /* a label, a unit, a quiet read-out */ \
    ADI_THEME_ROLE(textFaint) /* a search field's hint */ \
    ADI_THEME_ROLE(textOnBand) /* a ruler's numbers and a band's icons */ \
    ADI_THEME_ROLE(textBright) /* a band's icon under the pointer, a lit marker */ \
    ADI_THEME_ROLE(rulerLabel) /* the second ruler's time labels, its dots */ \
    ADI_THEME_ROLE(ink) /* text and marks on a colour: a clip, an on switch, a track's title */ \
    /* states */ \
    ADI_THEME_ROLE(accent) /* an on switch, the activator */ \
    ADI_THEME_ROLE(value) /* a value box's fill, a value knob's ring */ \
    ADI_THEME_ROLE(selection) /* a selected row, a selected marker */ \
    ADI_THEME_ROLE(record) /* Arm, a clip held over 0 dB, a red LED */ \
    ADI_THEME_ROLE(recordLed) /* the automation LED, the Control Bar's record dot */ \
    ADI_THEME_ROLE(error) /* a message that something failed */ \
    ADI_THEME_ROLE(favourite) /* the Browser's favourite mark */ \
    ADI_THEME_ROLE(focus) /* the Browser's preview ring */ \
    ADI_THEME_ROLE(lock) /* the Arrangement's lock button when on */ \
    ADI_THEME_ROLE(dropTarget) /* where a dragged track lands */ \
    ADI_THEME_ROLE(mutedClip) /* a deactivated clip, an off note */ \
    ADI_THEME_ROLE(handle) /* a fade's handle */ \
    ADI_THEME_ROLE(handleChosen) /* a chosen fade handle, the insert marker's line */ \
    ADI_THEME_ROLE(neutral) /* a mark with no colour of its own: the project's node, another clip's notes */ \
    ADI_THEME_ROLE(overviewFrame) /* the Overview's frame round what the lanes show */ \
    ADI_THEME_ROLE(inputBack) /* a text field being typed in */ \
    /* the rulers' marks */ \
    ADI_THEME_ROLE(marker) /* a locator, a time signature marker */ \
    ADI_THEME_ROLE(markerText) /* its label and its notch */ \
    ADI_THEME_ROLE(loopOn) /* the loop brace when the loop is on */ \
    ADI_THEME_ROLE(loopOff) /* and when it is off */ \
    ADI_THEME_ROLE(loopBand) /* the loop's band over the ruler */ \
    /* meters */ \
    ADI_THEME_ROLE(meterLevel) /* a meter's level */ \
    ADI_THEME_ROLE(meterPeak) /* its peak line */ \
    /* controls */ \
    ADI_THEME_ROLE(selectorOff) /* the side strip's selector when off */ \
    ADI_THEME_ROLE(selectorEdge) /* its frame */ \
    ADI_THEME_ROLE(knobFace) /* a rotary's face */ \
    ADI_THEME_ROLE(knobTrack) /* a rotary's track, a scroll bar's thumb */ \
    ADI_THEME_ROLE(knobRim) /* a rotary's rim */ \
    ADI_THEME_ROLE(tooltip) /* a tooltip's ground */ \
    /* menus */ \
    ADI_THEME_ROLE(menuBack) \
    ADI_THEME_ROLE(menuEdge) \
    ADI_THEME_ROLE(menuRowCurrent) \
    ADI_THEME_ROLE(menuTextCurrent) \
    ADI_THEME_ROLE(menuRowHover) \
    ADI_THEME_ROLE(menuText) \
    /* the Clip View's piano */ \
    ADI_THEME_ROLE(pianoWhite) /* a white key */ \
    ADI_THEME_ROLE(pianoBlack) /* a black key */ \
    ADI_THEME_ROLE(pianoLine) /* the line where two white keys meet */ \
    ADI_THEME_ROLE(noteOff) /* a deactivated note's velocity stem */ \
    ADI_THEME_ROLE(noteInactive) /* a note of another clip, or deactivated, in the editor */ \
    ADI_THEME_ROLE(noteOutOfScale) /* a note outside the scale, coloured by degree */
struct Theme {
#define ADI_THEME_MEMBER(name) std::uint32_t name;
    ADI_THEME_ROLES(ADI_THEME_MEMBER)
#undef ADI_THEME_MEMBER
};
// The four, in the order a chooser offers them.
inline constexpr std::array<ThemeId, 4> kThemes{ThemeId::Dark, ThemeId::MidGrey, ThemeId::Light, ThemeId::MixView};
const Theme &themeFor(ThemeId);
// The settings' word for each ("dark", "midgrey", "light", "mixview"), and back. Today's registry
// offers "os", "dark" and "light" for `lookfeel.theme`: "os" and anything unknown read as Dark.
std::string_view themeName(ThemeId);
ThemeId themeFromSetting(std::string_view);
} // namespace adi::ui
