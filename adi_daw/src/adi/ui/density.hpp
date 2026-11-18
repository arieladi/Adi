// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The window's sizes (DF-W24; ADR-0244 d2): one table per density, every measure, corner, line,
// type size and knob size the window draws with, by its role. No drawing code holds a size of its
// own for these: it asks the current density (`look::` in the JUCE layer reads it).
//
// The densities (collab/window/design/README.md, language.png part F):
// - Compact, the default: Live's sizes, as the window was measured and built (look-live11.md);
//   the knob sizes are the language sheet's three (Small, Medium, Large).
// - Open: the same panel with more room. The sheet's compressor gives the step: its panel 270 x 236
//   becomes 316 x 290 (about 1.2), its type 9.5 becomes 11, its knob 11 becomes 16. Every other
//   measure takes the same step of 1.2, rounded to the half point; win's drawing of the full set
//   is asked for (DF-W28).
#pragma once
#include <array>
#include <string_view>
namespace adi::ui {
enum class DensityId { Compact, Open };
// Every measure, with what it sizes. ADI_DENSITY_INT(name) and ADI_DENSITY_FLOAT(name) are applied
// to each in order: Density's members, and the JUCE layer's look:: names, come from this one list.
#define ADI_DENSITY_ROLES(ADI_DENSITY_INT, ADI_DENSITY_FLOAT) \
    /* the window's frame, in points at 100 % */ \
    ADI_DENSITY_INT(controlBarHeight) /* the Control Bar */ \
    ADI_DENSITY_INT(statusBarHeight) /* the Status Bar */ \
    ADI_DENSITY_INT(gap) /* between the window's areas */ \
    ADI_DENSITY_INT(border) /* a panel's dark edge */ \
    ADI_DENSITY_INT(fieldHeight) /* a Control Bar field, a chooser's row */ \
    ADI_DENSITY_INT(detailHeight) /* the Info View and the Device View, with their edges */ \
    /* corners */ \
    ADI_DENSITY_FLOAT(radiusSmall) /* a small mark, a pill's ends at the smallest */ \
    ADI_DENSITY_FLOAT(radiusThumb) /* a scroll bar's thumb */ \
    ADI_DENSITY_FLOAT(radiusBox) /* a value box, a field */ \
    ADI_DENSITY_FLOAT(radiusControl) /* a button, a chooser */ \
    ADI_DENSITY_FLOAT(radiusPanel) /* a panel, a device, the Browser's block */ \
    ADI_DENSITY_FLOAT(radiusGroup) /* a rounded group of buttons (the Control Bar's) */ \
    /* lines */ \
    ADI_DENSITY_FLOAT(hairline) /* a box's edge */ \
    ADI_DENSITY_FLOAT(line) /* a separator, a marker's stem */ \
    ADI_DENSITY_FLOAT(heavyLine) /* a selection's or a drop's frame */ \
    /* type, in points */ \
    ADI_DENSITY_FLOAT(fontMicro) /* the smallest read-out (a meter's scale) */ \
    ADI_DENSITY_FLOAT(fontTiny) /* a knob's label in a device */ \
    ADI_DENSITY_FLOAT(fontSmall) /* a ruler's numbers, a unit */ \
    ADI_DENSITY_FLOAT(fontCaption) /* a caption under a control */ \
    ADI_DENSITY_FLOAT(fontLabel) /* a small label */ \
    ADI_DENSITY_FLOAT(fontBody) /* most text: names, menus, fields */ \
    ADI_DENSITY_FLOAT(fontField) /* a field's value, a track's name */ \
    ADI_DENSITY_FLOAT(fontHeading) /* a panel's title */ \
    ADI_DENSITY_FLOAT(fontTitle) /* a dialog's title */ \
    /* the ring knob, its radius in points (language.png part C) */ \
    ADI_DENSITY_FLOAT(knobSmall) \
    ADI_DENSITY_FLOAT(knobMedium) \
    ADI_DENSITY_FLOAT(knobLarge)
struct Density {
#define ADI_DENSITY_INT_MEMBER(name) int name;
#define ADI_DENSITY_FLOAT_MEMBER(name) float name;
    ADI_DENSITY_ROLES(ADI_DENSITY_INT_MEMBER, ADI_DENSITY_FLOAT_MEMBER)
#undef ADI_DENSITY_INT_MEMBER
#undef ADI_DENSITY_FLOAT_MEMBER
};
inline constexpr std::array<DensityId, 2> kDensities{DensityId::Compact, DensityId::Open};
const Density &densityFor(DensityId);
// The settings' word for each ("compact", "open"), and back; anything unknown reads as Compact.
std::string_view densityName(DensityId);
DensityId densityFromSetting(std::string_view);
} // namespace adi::ui
