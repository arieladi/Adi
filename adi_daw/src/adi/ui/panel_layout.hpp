// SPDX-License-Identifier: GPL-3.0-or-later
//
// `MainSplit`'s model: an ordered panel→slot map with per-panel widths.
// ADR-0080 d1–d5, adopted by ADR-0180 §2.3 (approved 2026-09-29).
//
// NO JUCE. This is the arithmetic of "which panel is where, how wide is it, and
// does this swap fit", which is where a layout is actually wrong. A window is
// not needed to check it and having one would only make it harder.
//
// WHAT ADR-0080 RULES OUT, AND WHY IT IS NOT A `StretchableLayoutManager`.
// That class is slot-indexed and holds the widths itself, which produces the
// `leftWidth`/`rightWidth` shape d2 names as the bug: swap the sides and the
// browser inherits the mixer's width, because the width belonged to the SLOT.
// Here a width belongs to the PANEL and travels with it (d2). A swap is a
// reorder of the map and never a reconstruction (d1, d4) — the same rule
// ADR-0063 d1 states for undocking, arriving for a second reason.
//
// THE REFUSAL IS THE POINT OF d3. A swap that does not fit is not silently
// clamped away: the panels clamp to their own minima, the remainder comes out
// of the arrangement, and if the arrangement would fall below ITS minimum the
// swap is REFUSED and says why. A layout that silently does something other
// than what was asked is how a user learns not to trust the window.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace adi::ui {

/// The panels that can occupy a side slot. The arrangement is not one of them:
/// it is the centre, and it is what gives up the remainder.
enum class Panel { Browser, Mixer };

/// One panel's own geometry. Both numbers belong to the PANEL (d2) and follow
/// it across a swap.
struct PanelGeometry {
    Panel panel = Panel::Browser;
    int width = 0;        ///< logical pixels at 100% zoom, persisted in `ui_view`
    int minWidth = 0;     ///< per panel (d3), never a single shell-wide figure
    bool collapsed = false;
};

/// Why a swap or a resize was refused, in words a UI can show (d3).
struct LayoutRefusal {
    bool refused = false;
    std::string message;
    explicit operator bool() const noexcept { return refused; }
};

/// The ordered map. Slot 0 is the left side, slot 1 the right, and the
/// arrangement fills what is between them.
///
/// The order IS the map: `slots()[0]` is whatever panel is currently on the
/// left. There is deliberately no `bool swapped` (d1) — a boolean beside an
/// order is a second source of truth, and the two disagree the first time a
/// third panel appears.
class PanelLayout {
public:
    /// `arrangementMinWidth` is what the centre may not shrink below. It is the
    /// figure a swap is refused against, so it is required rather than defaulted
    /// to something that would make the refusal silently unreachable.
    PanelLayout(PanelGeometry left, PanelGeometry right, int arrangementMinWidth)
        : slots_{left, right}, arrangementMin_(arrangementMinWidth) {}

    [[nodiscard]] const std::vector<PanelGeometry>& slots() const noexcept { return slots_; }
    [[nodiscard]] int arrangementMinWidth() const noexcept { return arrangementMin_; }

    /// Where a panel currently is, or `npos` if it is not in the map.
    [[nodiscard]] std::size_t slotOf(Panel p) const noexcept {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].panel == p) return i;
        return npos;
    }

    [[nodiscard]] const PanelGeometry* find(Panel p) const noexcept {
        const auto i = slotOf(p);
        return i == npos ? nullptr : &slots_[i];
    }

    /// The width a panel actually occupies: 0 when collapsed, its width
    /// otherwise. Collapsing does not destroy the width, so expanding restores
    /// what the user chose rather than a default.
    [[nodiscard]] int occupied(const PanelGeometry& g) const noexcept {
        return g.collapsed ? 0 : g.width;
    }

    /// What the arrangement gets at this total width, before any clamping.
    [[nodiscard]] int arrangementWidth(int totalWidth) const noexcept {
        int used = 0;
        for (const auto& g : slots_) used += occupied(g);
        return totalWidth - used;
    }

    /// Does the CURRENT layout fit in `totalWidth`, clamping each panel to its
    /// own minimum first? Reports why not (d3).
    [[nodiscard]] LayoutRefusal fits(int totalWidth) const {
        int minimum = 0;
        for (const auto& g : slots_) minimum += g.collapsed ? 0 : g.minWidth;
        const int forArrangement = totalWidth - minimum;
        if (forArrangement < arrangementMin_) {
            return {true, "Not enough width: the panels need at least " +
                              std::to_string(minimum) + " px and the arrangement needs " +
                              std::to_string(arrangementMin_) + " px, but the window is " +
                              std::to_string(totalWidth) + " px."};
        }
        return {};
    }

    /// Swap the two side panels. REORDERS, never reconstructs (d4): the same
    /// `PanelGeometry` values move, so each panel keeps its own width and
    /// minimum (d2).
    ///
    /// Refuses, and changes NOTHING, when the result would not fit (d3). A
    /// refusal that had already half-applied the swap would be worse than no
    /// refusal at all.
    LayoutRefusal swapSides(int totalWidth) {
        auto candidate = *this;
        std::swap(candidate.slots_[0], candidate.slots_[1]);
        if (auto why = candidate.fits(totalWidth)) return why;
        slots_ = std::move(candidate.slots_);
        return {};
    }

    /// Set a panel's width, clamping UP to its own minimum and taking the
    /// remainder from the arrangement. Refuses when the arrangement would fall
    /// below its minimum, leaving the layout untouched.
    LayoutRefusal setWidth(Panel p, int width, int totalWidth) {
        const auto i = slotOf(p);
        if (i == npos) return {true, "That panel is not in the layout."};

        auto candidate = *this;
        candidate.slots_[i].width = width < candidate.slots_[i].minWidth
                                        ? candidate.slots_[i].minWidth
                                        : width;
        if (candidate.arrangementWidth(totalWidth) < arrangementMin_) {
            return {true, "The arrangement would drop below " +
                              std::to_string(arrangementMin_) + " px."};
        }
        slots_ = std::move(candidate.slots_);
        return {};
    }

    /// Collapse or expand a panel. Always allowed in the collapsing direction --
    /// it only ever gives width back -- and refused when expanding would not
    /// fit, for the same reason as `setWidth`.
    LayoutRefusal setCollapsed(Panel p, bool collapsed, int totalWidth) {
        const auto i = slotOf(p);
        if (i == npos) return {true, "That panel is not in the layout."};

        auto candidate = *this;
        candidate.slots_[i].collapsed = collapsed;
        if (!collapsed && candidate.arrangementWidth(totalWidth) < arrangementMin_) {
            return {true, "There is not enough room to show that panel."};
        }
        slots_ = std::move(candidate.slots_);
        return {};
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    std::vector<PanelGeometry> slots_;
    int arrangementMin_ = 0;
};

}  // namespace adi::ui
