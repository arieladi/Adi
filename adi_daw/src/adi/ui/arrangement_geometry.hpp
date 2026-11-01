// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/textproj.hpp"
#include "session_state.hpp"
#include "snapshot_reader.hpp"
#include "track_order.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>
#include <string>
namespace adi::ui {
// What a track's title shows (Live 12 manual 18.2): a name that begins with # shows
// the track's number there, and the number follows the track when it moves; each
// further # adds a leading zero. `position` counts from 1.
inline std::string displayName(const std::string &name, std::size_t position) {
    std::size_t hashes = 0;
    while (hashes < name.size() && name[hashes] == '#')
        ++hashes;
    if (hashes == 0)
        return name;
    auto number = std::to_string(position);
    if (number.size() < hashes)
        number.insert(0, hashes - number.size(), '0');
    return number + name.substr(hashes);
}
// What Live shows for the grid's spacing (manual §6.10), in 4/4 until the snapshot carries
// the meters (collab/window.md, N6): "1/16", "1/8T", "1 Bar", "2 Bars".
inline std::string gridLabel(double quarters, bool triplet) {
    const double plain = triplet ? quarters * 3 / 2 : quarters;
    std::string label;
    if (plain >= 4) {
        const auto bars = std::llround(plain / 4);
        label = std::to_string(bars) + (bars == 1 ? " Bar" : " Bars");
    } else
        label = "1/" + std::to_string(std::llround(4 / plain));
    return triplet ? label + "T" : label;
}
// View-only coordinates and selection. No committed project state is copied here.
//
// Every track the snapshot holds is a row, in the arrangement's order (TrackOrder): each group
// followed by what it holds, Main last.
struct ArrangementGeometry {
    double left = 0, scale = 48; // quarter notes at the left edge; pixels per quarter
    int height = 88, scrollY = 0;
    std::map<std::int64_t, int> heights;
    // Bitwig's Use Large Track Height (Master Reference 10, Figure 33): off, every track shows
    // at half its height, never below the smallest. The heights kept are the large ones.
    bool largeTracks = true;
    static constexpr int kMinTrackHeight = 24, kMaxTrackHeight = 512;
    // The project view (Master Reference 10, ADR-0112 d2): the tracks a view leaves out, sorted.
    // They take no room, so every walk down the rows passes over them; their heights are kept.
    std::vector<std::int64_t> hidden;
    // Live's folded groups (manual 18.3): what they hold, sorted. Out of the lanes, the names and
    // the track controls, as `hidden`; the Mixer View still shows them (UI-ARCHITECTURE §4).
    std::vector<std::int64_t> foldedAway;
    // Whether a track has a row in the lanes.
    bool shown(std::int64_t id) const noexcept {
        return inView(id) && !std::binary_search(foldedAway.begin(), foldedAway.end(), id);
    }
    // Whether the project view shows a track: the Mixer View's columns.
    bool inView(std::int64_t id) const noexcept {
        return !std::binary_search(hidden.begin(), hidden.end(), id);
    }
    // The rows' order, built by the view for each new snapshot.
    TrackOrder order;
    struct Row {
        const engine::TrackNode *track; // never null
        TrackPlace place;
        std::size_t position; // down the rows, from 0: Live's track number less one
    };
    // The tracks in the arrangement's order: `order` when it was built for this snapshot, else
    // the snapshot's own, every track at the top (a geometry used without its view).
    class Rows {
      public:
        Rows(const std::vector<std::shared_ptr<const engine::TrackNode>> &tracks,
             const std::vector<TrackPlace> *places) noexcept
            : tracks_(tracks), places_(places) {}
        std::size_t size() const noexcept { return tracks_.size(); }
        Row operator[](std::size_t i) const noexcept {
            if (places_ != nullptr) {
                const auto &p = (*places_)[i];
                return {tracks_[p.at].get(), p, i};
            }
            TrackPlace p;
            p.at = static_cast<std::uint32_t>(i);
            p.id = tracks_[i]->id;
            return {tracks_[i].get(), p, i};
        }
        class iterator {
          public:
            iterator(const Rows &rows, std::size_t i) noexcept : rows_(&rows), i_(i) {}
            Row operator*() const noexcept { return (*rows_)[i_]; }
            iterator &operator++() noexcept {
                ++i_;
                return *this;
            }
            bool operator!=(const iterator &other) const noexcept { return i_ != other.i_; }

          private:
            const Rows *rows_;
            std::size_t i_;
        };
        iterator begin() const noexcept { return {*this, 0}; }
        iterator end() const noexcept { return {*this, size()}; }
        // A track's row, or size() when it has none.
        std::size_t find(std::int64_t id) const noexcept {
            for (std::size_t i = 0; i < size(); ++i)
                if ((*this)[i].track->id == id)
                    return i;
            return size();
        }

      private:
        const std::vector<std::shared_ptr<const engine::TrackNode>> &tracks_;
        const std::vector<TrackPlace> *places_;
    };
    Rows rows(const SnapshotReader &r) const noexcept {
        return {r.tracks(), order.fits(r) ? &order.places() : nullptr};
    }
    int heightFor(std::int64_t id) const noexcept {
        if (!shown(id))
            return 0;
        const auto i = heights.find(id);
        const int h = i == heights.end() ? height : i->second;
        return largeTracks ? h : std::max(kMinTrackHeight, h / 2);
    }
    // `next` is a height as shown; while the tracks are small it is kept at twice that, so
    // the track comes back at the size it was given.
    void resizeTrack(std::int64_t id, int next, bool all = false) {
        next = std::clamp(next, kMinTrackHeight, kMaxTrackHeight);
        if (!largeTracks)
            next = std::min(kMaxTrackHeight, next * 2);
        if (all) {
            height = next;
            heights.clear();
        } else if (id)
            heights[id] = next;
    }
    // Alt+wheel (Live 12 manual 6.2): the track under the mouse, or, with a time
    // selection, every track with selected content.
    void wheelHeight(const SnapshotReader &reader, std::int64_t under, int delta) {
        bool changed = false;
        if (selection.start != selection.end) {
            const auto a = std::min(selection.start, selection.end),
                       b = std::max(selection.start, selection.end);
            for (const auto &t : reader.tracks())
                for (const auto &c : t->clips)
                    if (shown(t->id) && c->posTicks < b && c->posTicks + c->lengthTicks > a) {
                        resizeTrack(t->id, heightFor(t->id) + delta);
                        changed = true;
                        break;
                    }
        }
        if (!changed)
            resizeTrack(under, heightFor(under) + delta);
    }
    // Live's editing grid (manual §6.10): the cursor snaps to its lines, which are
    // zoom-adaptive or fixed; narrowing doubles their density, widening halves it, triplets
    // take two thirds of the step. A view setting, kept per window.
    struct Grid {
        bool snap = true, fixed = false, triplet = false;
        int narrow = 0;              // the adaptive grid's steps narrower (+) or wider (-)
        double fixedQuarters = .25;  // the fixed grid's step: a sixteenth
        bool operator==(const Grid &) const = default;
    } grid;
    // The lanes' menu names the widths (§6.10): the fixed grid's from 8 Bars to 1/32, and the
    // adaptive grid's five, widest first, a step apart. Ctrl/Cmd+1 and 2 stay within them.
    // PROVISIONAL: the names are Live's as the window recalls them, and which of them is the
    // default (Medium here) is to be measured; the online manual names neither.
    static constexpr double kFinestGrid = 1. / 8, kWidestGrid = 32; // a 1/32 note to 8 bars
    static constexpr std::array<const char *, 5> kAdaptiveNames{"Widest", "Wide", "Medium", "Narrow",
                                                                "Narrowest"};
    static constexpr int kGridNarrowest = 2;
    // The step between grid lines, in quarter notes. The adaptive grid at its default keeps
    // lines at least 14 pixels apart and never finer than a quarter, as it is drawn.
    double gridQuarters() const noexcept {
        return grid.triplet ? plainGridQuarters() * 2 / 3 : plainGridQuarters();
    }
    // The same step before triplets.
    double plainGridQuarters() const noexcept {
        const double adaptive = std::max(1., std::pow(2., std::ceil(std::log2(14. / scale)))) *
                                std::pow(2., -grid.narrow);
        return std::clamp(grid.fixed ? grid.fixedQuarters : adaptive, kFinestGrid, kWidestGrid);
    }
    // The grid line at or before `tick`, or at or after it with `up`.
    std::int64_t snap(std::int64_t tick, bool up) const noexcept {
        const double step = gridQuarters() * textproj::kPPQ;
        const double cells = static_cast<double>(tick) / step;
        return std::llround((up ? std::ceil(cells - 1e-9) : std::floor(cells + 1e-9)) * step);
    }
    // The grid line after `tick`, or the one before it: where an arrow key takes the insert
    // marker, a clip or a selection's edge (online §41.4.1.3). Never before the start.
    std::int64_t nextGridLine(std::int64_t tick, bool forward) const noexcept {
        return forward ? snap(tick + 1, true) : std::max<std::int64_t>(0, snap(tick - 1, false));
    }
    // This window's presented copy of the session's selection (ADR-0021): read at the
    // top of each frame, written back through the root. Never a second source of truth.
    Selection selection;
    struct View {
        double left, scale;
    };
    std::array<View, 32> history{};
    std::size_t historySize = 0;
    double x(std::int64_t tick) const noexcept {
        return (static_cast<double>(tick) / textproj::kPPQ - left) * scale;
    }
    std::int64_t tick(double pixel) const noexcept {
        return static_cast<std::int64_t>(std::clamp(left + pixel / scale, 0., 1e9) *
                                         textproj::kPPQ);
    }
    // What + and - zoom around (Live 12 manual 6.2, ADR-0129 deviation 2): the
    // time selection's middle, else the insert marker, else the playhead.
    std::int64_t anchor(std::int64_t playhead) const noexcept {
        if (selection.start != selection.end)
            return (selection.start + selection.end) / 2;
        return selection.marked ? selection.insert : playhead;
    }
    void pan(double pixels) noexcept { left = std::clamp(left + pixels / scale, 0., 1e9); }
    void zoom(double factor, double pixel) noexcept {
        const double at = left + pixel / scale;
        scale = std::clamp(scale * factor, 1., 4096.);
        left = std::max(0., at - pixel / scale);
    }
    void zoomAround(double factor, std::int64_t at) noexcept { zoom(factor, x(at)); }
    void fit(std::int64_t start, std::int64_t end, int width, bool remember = true) noexcept {
        if (remember) {
            if (historySize == history.size()) {
                std::move(history.begin() + 1, history.end(), history.begin());
                --historySize;
            }
            history[historySize++] = {left, scale};
        }
        left = static_cast<double>(start) / textproj::kPPQ;
        scale = std::clamp(static_cast<double>(std::max(width, 1)) /
                               std::max(1., static_cast<double>(end - start) / textproj::kPPQ),
                           1., 4096.);
    }
    void back() noexcept {
        if (historySize) {
            const auto v = history[--historySize];
            left = v.left;
            scale = v.scale;
        }
    }
    const engine::TrackNode *trackAt(const SnapshotReader &r, int y) const noexcept {
        const auto all = rows(r);
        const auto i = rowAt(r, y);
        return i < all.size() ? all[i].track : nullptr;
    }
    // The row at `y`, or the rows' size when there is none; `top` gets where it begins.
    std::size_t rowAt(const SnapshotReader &r, int y, int *top = nullptr) const noexcept {
        const auto all = rows(r);
        if (y < 0)
            return all.size();
        int at = -scrollY;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const int h = heightFor(all[i].track->id);
            if (y >= at && y < at + h) {
                if (top != nullptr)
                    *top = at;
                return i;
            }
            at += h;
        }
        return all.size();
    }
    int contentHeight(const SnapshotReader &r) const noexcept {
        int total = 0;
        for (const auto &t : r.tracks())
            total += heightFor(t->id);
        return total;
    }
    const engine::ClipNode *hit(const engine::TrackNode &t, std::int64_t at) const noexcept {
        // SnapshotBuilder orders by position/id. Upper bound excludes future clips;
        // reverse scan handles overlapping clips, matching last-painted precedence.
        auto end = std::upper_bound(t.clips.begin(), t.clips.end(), at,
                                    [](auto value, const auto &c) { return value < c->posTicks; });
        while (end != t.clips.begin()) {
            const auto &c = *--end;
            if (at >= c->posTicks && at - c->posTicks < c->lengthTicks)
                return c.get();
        }
        return nullptr;
    }
    std::int64_t end(const SnapshotReader &r) const noexcept {
        std::int64_t result = 16 * textproj::kPPQ;
        for (const auto &t : r.tracks())
            for (const auto &c : t->clips)
                result = std::max(result, c->posTicks + c->lengthTicks);
        return result;
    }
};
} // namespace adi::ui
