// SPDX-License-Identifier: GPL-3.0-or-later
//
// `MainSplit`'s panel→slot map. ADR-0080 d1–d5, adopted by ADR-0180 §2.3.
//
// The load-bearing check is `widthFollowsThePanel`. ADR-0080 d2 exists because
// the obvious implementation -- a layout manager holding leftWidth/rightWidth --
// gives the browser the mixer's width the moment the sides swap. That check
// fails under the implementation this one replaced, which is the only reason
// to trust it.

#include "adi/ui/panel_layout.hpp"

#include <cstdio>
#include <string>

namespace {
using namespace adi::ui;

int g_checks = 0, g_failures = 0;
void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

PanelLayout make() {
    return PanelLayout{{Panel::Browser, 240, 180, false},
                       {Panel::Mixer, 320, 200, false},
                       400};
}

void theOrderIsTheMap() {
    section("ADR-0080 d1: layout is an ordered map, not a bool");
    auto l = make();
    check(l.slots().size() == 2, "two side slots");
    check(l.slots()[0].panel == Panel::Browser, "browser starts on the left");
    check(l.slots()[1].panel == Panel::Mixer, "mixer starts on the right");
    check(l.slotOf(Panel::Browser) == 0, "slotOf reads the order");
    check(l.slotOf(Panel::Mixer) == 1, "for both");
    check(l.find(Panel::Browser) != nullptr, "find resolves a present panel");
}

void widthFollowsThePanel() {
    section("ADR-0080 d2: a width belongs to the PANEL and crosses the swap");
    auto l = make();
    const int browserBefore = l.find(Panel::Browser)->width;
    const int mixerBefore = l.find(Panel::Mixer)->width;
    check(browserBefore == 240 && mixerBefore == 320, "distinct widths to tell apart");

    const auto why = l.swapSides(1400);
    check(!why, "the swap is allowed at 1400 px");
    check(l.slots()[0].panel == Panel::Mixer, "mixer is now on the left");
    check(l.slots()[1].panel == Panel::Browser, "browser is now on the right");

    // THE CHECK. A slot-indexed layout gives the left slot's 240 to whatever is
    // now on the left -- the mixer -- and this fails.
    check(l.find(Panel::Browser)->width == browserBefore, "the browser kept ITS width");
    check(l.find(Panel::Mixer)->width == mixerBefore, "the mixer kept ITS width");
    check(l.find(Panel::Browser)->minWidth == 180, "and its own minimum");
    check(l.find(Panel::Mixer)->minWidth == 200, "and so did the mixer");
}

void aSwapReordersAndNeverReconstructs() {
    section("ADR-0080 d4: a swap reorders -- collapsed state survives it too");
    auto l = make();
    check(!l.setCollapsed(Panel::Browser, true, 1400), "collapse the browser");
    check(l.find(Panel::Browser)->collapsed, "it is collapsed");
    check(l.find(Panel::Browser)->width == 240, "and its width is REMEMBERED, not zeroed");

    check(!l.swapSides(1400), "swap while collapsed");
    check(l.find(Panel::Browser)->collapsed, "still collapsed after the swap");
    check(l.find(Panel::Browser)->width == 240, "and still remembers its width");
    check(!l.setCollapsed(Panel::Browser, false, 1400), "expand it again");
    check(l.occupied(*l.find(Panel::Browser)) == 240, "expanding restores what the user chose");
}

void aSwapThatDoesNotFitRefusesWithAMessage() {
    section("ADR-0080 d3: refuse with a message, and change nothing");
    // Browser 180 min + mixer 200 min + arrangement 400 = 780 needed.
    auto l = make();
    const auto tight = l.fits(700);
    check(tight.refused, "700 px does not fit");
    check(tight.message.find("700") != std::string::npos, "the message says the window width");
    check(tight.message.find("400") != std::string::npos, "and the arrangement's minimum");
    check(!l.fits(800), "800 px does fit");

    // A refused swap must leave the map exactly as it was.
    auto before = l.slots();
    const auto why = l.swapSides(700);
    check(why.refused, "the swap is refused at 700 px");
    check(!why.message.empty(), "with a message, not silently");
    check(l.slots()[0].panel == before[0].panel, "and the order did NOT change");
    check(l.slots()[1].panel == before[1].panel, "on either side");
    check(l.find(Panel::Browser)->width == before[0].width, "nor any width");
}

void theArrangementGivesUpTheRemainder() {
    section("the centre is what yields, and it has a floor");
    auto l = make();
    check(l.arrangementWidth(1400) == 1400 - 240 - 320, "the arrangement gets what is left");

    check(!l.setWidth(Panel::Mixer, 500, 1400), "widening the mixer is allowed while it fits");
    check(l.find(Panel::Mixer)->width == 500, "and it took effect");
    check(l.arrangementWidth(1400) == 1400 - 240 - 500, "out of the arrangement");

    const auto why = l.setWidth(Panel::Mixer, 900, 1400);
    check(why.refused, "but not past the arrangement's floor");
    check(why.message.find("400") != std::string::npos, "and it says what the floor is");
    check(l.find(Panel::Mixer)->width == 500, "and the refused width did not apply");
}

void aWidthClampsUpToItsOwnMinimum() {
    section("ADR-0080 d3: minima are PER PANEL");
    auto l = make();
    check(!l.setWidth(Panel::Browser, 10, 1400), "asking for 10 px is allowed...");
    check(l.find(Panel::Browser)->width == 180, "...and clamps up to the browser's own 180");
    check(!l.setWidth(Panel::Mixer, 10, 1400), "the mixer too");
    check(l.find(Panel::Mixer)->width == 200, "but to ITS 200, not the browser's 180");
}

void collapsingAlwaysGivesWidthBack() {
    section("collapsing is always allowed; expanding can be refused");
    auto l = make();
    check(!l.setCollapsed(Panel::Browser, true, 700),
          "collapsing is allowed even at a width that would not otherwise fit");
    check(l.arrangementWidth(700) == 700 - 320, "the browser's width went to the arrangement");

    const auto why = l.setCollapsed(Panel::Browser, false, 700);
    check(why.refused, "expanding it again at 700 px is refused");
    check(!why.message.empty(), "with a message");
    check(l.find(Panel::Browser)->collapsed, "and it stayed collapsed");
}

void anAbsentPanelIsRefusedNotCrashed() {
    section("a panel that is not in the map");
    PanelLayout only{{Panel::Browser, 240, 180, false}, {Panel::Browser, 240, 180, false}, 400};
    check(only.slotOf(Panel::Mixer) == PanelLayout::npos, "slotOf reports absence");
    check(only.find(Panel::Mixer) == nullptr, "find returns null");
    const auto why = only.setWidth(Panel::Mixer, 300, 1400);
    check(why.refused, "setting its width is refused");
    check(!why.message.empty(), "with a message rather than undefined behaviour");
}

}  // namespace

int main() {
    theOrderIsTheMap();
    widthFollowsThePanel();
    aSwapReordersAndNeverReconstructs();
    aSwapThatDoesNotFitRefusesWithAMessage();
    theArrangementGivesUpTheRemainder();
    aWidthClampsUpToItsOwnMinimum();
    collapsingAlwaysGivesWidthBack();
    anAbsentPanelIsRefusedNotCrashed();
    std::printf("%s -- %d checks, %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
