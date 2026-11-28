// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/textproj.hpp"
#include "meters.hpp"
#include "session_state.hpp"
#include "snapshot_reader.hpp"
#include "track_order.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <map>
#include <set>
#include <span>
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
// displayName written into `out` in place: no temporary, so a string that already holds a name as long
// allocates nothing (the Mix View's rows refill theirs each publication, walk-B B13).
inline void assignDisplayName(std::string &out, const std::string &name, std::size_t position) {
    std::size_t hashes = 0;
    while (hashes < name.size() && name[hashes] == '#')
        ++hashes;
    if (hashes == 0) {
        if (out != name)
            out.assign(name);
        return;
    }
    char digits[24];
    std::size_t n = 0;
    do {
        digits[n++] = static_cast<char>('0' + position % 10);
        position /= 10;
    } while (position != 0 && n < sizeof digits);
    out.clear();
    for (std::size_t pad = n; pad < hashes; ++pad)
        out.push_back('0');
    while (n != 0)
        out.push_back(digits[--n]);
    out.append(name, hashes, std::string::npos);
}
// Track `id`'s name as its header shows it (displayName at its Live number), for what is named after a
// track (a recorded take, Live 12 manual 19.2). Builds the order once: for commands, not for painting.
inline std::string shownTrackName(const SnapshotReader &reader, std::int64_t id) {
    std::vector<TrackRow> rows;
    rowsFromSnapshot(reader, rows);
    TrackOrder order;
    order.build(reader, rows);
    for (const auto &place : order.places())
        if (place.id == id)
            return displayName(reader.tracks()[place.at]->name, place.number);
    return {};
}
// Every track's name as its header shows it, by id (walk-B B13): the order built once, for what lists
// the tracks on a change, not for painting.
inline std::map<std::int64_t, std::string> shownTrackNames(const SnapshotReader &reader) {
    std::vector<TrackRow> rows;
    rowsFromSnapshot(reader, rows);
    TrackOrder order;
    order.build(reader, rows);
    std::map<std::int64_t, std::string> out;
    for (const auto &place : order.places())
        out.emplace(place.id, displayName(reader.tracks()[place.at]->name, place.number));
    return out;
}
// What Live shows for the grid's spacing (manual §6.10): "1/16", "1/8T", "1 Bar", "2 Bars". A bar
// here is four quarters: the ruler and the positions count bars in the project's meters (#299),
// but the grid's widths stay in note values until it snaps by bars of the meter in force
// (parity-7.3 row 11).
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
// followed by what it holds, then the returns, then Main. The returns and Main are Live's foot:
// pinned at the bottom of the lanes, whatever the scroll, with the drop area between them and the
// tracks above (Live 12 manual 6.1, its demo arrangement; parity-7.3 row 17).
struct ArrangementGeometry {
    double left = 0, scale = 48; // quarter notes at the left edge; pixels per quarter
    int height = 88, scrollY = 0;
    // The lanes' height as the view shows them, which the foot stands at the bottom of. Zero for a
    // geometry used without its view: the foot then follows the tracks.
    int lanesHeight = 0;
    std::map<std::int64_t, int> heights;
    // Total height of this track's separate automation lanes, after its clip row.
    std::map<std::int64_t, int> automationLaneHeights;
    // Folded ordinary tracks keep their row and their expanded height.
    std::set<std::int64_t> foldedTracks;
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
        std::size_t position; // down the rows, from 0
        // The number a # in its name and its activator show: Live's track number, which skips the
        // returns (one may stand in a group, DF-W33); a return or Main keeps its place's.
        std::size_t number() const noexcept { return place.number != 0 ? place.number : position + 1; }
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
        if (foldedTracks.contains(id))
            return kMinTrackHeight;
        const auto i = heights.find(id);
        const int h = i == heights.end() ? height : i->second;
        return largeTracks ? h : std::max(kMinTrackHeight, h / 2);
    }
    int stackHeightFor(std::int64_t id) const noexcept {
        const int base = heightFor(id);
        if (base == 0 || automationLaneHeights.empty() || foldedTracks.contains(id))
            return base;
        const auto it = automationLaneHeights.find(id);
        return it == automationLaneHeights.end() ? base : base + std::max(0, it->second);
    }
    // `next` is a height as shown; while the tracks are small it is kept at twice that, so
    // the track comes back at the size it was given.
    void resizeTrack(std::int64_t id, int next, bool all = false) {
        if (!all && foldedTracks.contains(id)) {
            if (next <= kMinTrackHeight)
                return; // A drag that leaves the row folded keeps its expanded height.
            foldedTracks.erase(id);
        }
        next = std::clamp(next, kMinTrackHeight, kMaxTrackHeight);
        if (!largeTracks)
            next = std::min(kMaxTrackHeight, next * 2);
        if (all) {
            height = next;
            heights.clear();
        } else if (id)
            heights[id] = next;
    }
    // Whether the selection covers a track: a time selection across tracks covers every track from
    // `selection.track` to `selection.toTrack` in the lanes' order (Live 12 manual 6.9); otherwise
    // just the selected track.
    // Whether a clip is selected: the selected one, or one picked besides it with Ctrl/Cmd+click.
    bool clipSelected(std::int64_t id) const noexcept {
        return id != 0 && (id == selection.clip || std::find(selection.pickedClips.begin(), selection.pickedClips.end(),
                                                             id) != selection.pickedClips.end());
    }
    // The selected clips: the selected one first, then those picked besides it.
    std::vector<std::int64_t> selectedClips() const {
        std::vector<std::int64_t> out;
        if (selection.clip != 0)
            out.push_back(selection.clip);
        for (const auto id : selection.pickedClips)
            if (id != selection.clip)
                out.push_back(id);
        return out;
    }
    bool inSelection(const SnapshotReader &reader, std::int64_t id) const noexcept {
        if (id == 0)
            return false;
        if (std::find(selection.picked.begin(), selection.picked.end(), id) != selection.picked.end())
            return true;
        if (selection.toTrack == 0 || selection.toTrack == selection.track)
            return id == selection.track;
        const auto all = rows(reader);
        const auto from = all.find(selection.track), to = all.find(selection.toTrack), at = all.find(id);
        if (from == all.size() || to == all.size() || at == all.size())
            return id == selection.track;
        return at >= std::min(from, to) && at <= std::max(from, to);
    }
    // The tracks the selection covers, in the lanes' order.
    std::vector<std::int64_t> selectedTracks(const SnapshotReader &reader) const {
        std::vector<std::int64_t> out;
        for (const auto row : rows(reader))
            if (inSelection(reader, row.track->id))
                out.push_back(row.track->id);
        return out;
    }
    // Alt+wheel (Live 12 manual 6.2): the track under the mouse, or, with a time
    // selection, every selected track with selected content.
    void wheelHeight(const SnapshotReader &reader, std::int64_t under, int delta) {
        bool changed = false;
        if (selection.start != selection.end) {
            const auto a = std::min(selection.start, selection.end),
                       b = std::max(selection.start, selection.end);
            for (const auto &t : reader.tracks())
                for (const auto &c : t->clips)
                    if (shown(t->id) && inSelection(reader, t->id) && c->posTicks < b &&
                        c->posTicks + c->lengthTicks > a) {
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
    // The step between grid lines, in quarter notes. The default adaptive width
    // aims for at least 14 pixels between lines until it reaches a thirty-second.
    double gridQuarters() const noexcept {
        return grid.triplet ? plainGridQuarters() * 2 / 3 : plainGridQuarters();
    }
    // The same step before triplets.
    double plainGridQuarters() const noexcept {
        const double adaptive = std::exp2(std::ceil(std::log2(14. / scale)) - grid.narrow);
        return std::clamp(grid.fixed ? grid.fixedQuarters : adaptive, kFinestGrid, kWidestGrid);
    }
    // The project's meters (#299), for a grid in the meter in force: set by the view for each
    // snapshot. Null, or 4/4 throughout, keeps the grid a plain multiple of its step from the start.
    const std::vector<textproj::Meter> *meters = nullptr;
    // Whether the meters shape the grid: any signature but a lone 4/4.
    bool metered() const noexcept {
        if (meters == nullptr || meters->empty())
            return false;
        return meters->size() > 1 || meters->front().numerator != 4 || meters->front().denominator != 4;
    }
    // The grid line at or before `tick`, or at or after it with `up`. In the project's meters (Live
    // 12 manual 6.10; 6.5: markers' "positioning is only constrained by the editing grid"), a step
    // under a bar runs from each bar line, and a bar's own lines among them; the bar widths count
    // the meter's bars from bar 1, a triplet two thirds of them. In 4/4 throughout both are plain
    // multiples of the step from the start, as without meters.
    std::int64_t snap(std::int64_t tick, bool up) const noexcept {
        if (!metered() || tick < 0) {
            const double step = gridQuarters() * textproj::kPPQ;
            return pickLine(tick, static_cast<double>(tick) / step, up,
                            [step](std::int64_t k) { return std::llround(static_cast<double>(k) * step); });
        }
        const auto bar = barHolding(tick);
        const double plain = plainGridQuarters();
        if (plain < 4) {
            const double step = gridQuarters() * textproj::kPPQ;
            const auto line = pickLine(tick, static_cast<double>(tick - bar.start) / step, up, [&bar, step](std::int64_t k) {
                return bar.start + std::llround(static_cast<double>(k) * step);
            });
            return up ? std::min<std::int64_t>(line, bar.end) : line;
        }
        const double unit = plain / 4 * (grid.triplet ? 2. / 3 : 1.);
        const double at = static_cast<double>(bar.index) +
                          static_cast<double>(tick - bar.start) / static_cast<double>(std::max<std::int64_t>(1, bar.end - bar.start));
        return pickLine(tick, at / unit, up,
                        [this, unit](std::int64_t k) { return tickOfBars(static_cast<double>(k) * unit); });
    }
    // The line at or before `tick` (or, `up`, at or after it) among `lineOf(k)` for whole k, lines in
    // order. `cells` only says where to start looking: the line is chosen by comparing ticks, so a line
    // a tick away is never taken for the tick itself, however long the bar or wide the grid (DF-W30 a:
    // a bar past 22 quarters on the 8 Bars grid put one tick inside a fixed 1e-9 of a cell).
    template <class LineOf>
    static std::int64_t pickLine(std::int64_t tick, double cells, bool up, LineOf lineOf) noexcept {
        auto k = static_cast<std::int64_t>(std::floor(cells));
        while (lineOf(k) > tick)
            --k;
        while (lineOf(k + 1) <= tick)
            ++k;
        const auto at = lineOf(k);
        return !up || at == tick ? at : lineOf(k + 1);
    }
    // Every grid line from `from` to `to`, in order.
    template <class Visit>
    void forEachGridLine(std::int64_t from, std::int64_t to, Visit &&visit) const {
        for (auto line = snap(std::max<std::int64_t>(0, from), true); line <= to;) {
            visit(line);
            const auto next = snap(line + 1, true);
            if (next <= line)
                return;
            line = next;
        }
    }
    // Sub-beat grid lines get a short ruler mark. Label the eighths and sixteenths
    // the grid lands on when there is room for a bar.beat.sixteenth label.
    static constexpr double kRulerLabelSpacing = 56.;
    template <class Visit>
    void forEachRulerSubdivision(std::int64_t from, std::int64_t to,
                                 const std::vector<textproj::Meter> &songMeters, Visit &&visit) const {
        const double step = gridQuarters();
        if (step >= 1. || step * scale < 4.)
            return;
        constexpr auto sixteenth = textproj::kWhole / 16;
        const auto labelStep = scale / 4 >= kRulerLabelSpacing ? sixteenth :
                               scale / 2 >= kRulerLabelSpacing ? 2 * sixteenth : 0;
        forEachBar(from - meterAt(from, songMeters).barTicks(), to, songMeters,
                   [&](std::int64_t start, std::int64_t bar) {
            const auto &meter = meterAt(start, songMeters);
            const auto next = std::upper_bound(songMeters.begin(), songMeters.end(), start,
                                               [](auto at, const auto &m) { return at < m.start_ticks; });
            const auto end = next == songMeters.end() ? start + meter.barTicks() :
                                                        std::min(start + meter.barTicks(), next->start_ticks);
            const auto beat = textproj::kWhole / std::max<std::int64_t>(1, meter.denominator);
            forEachGridLine(std::max(from, start), std::min(to, end - 1), [&](std::int64_t line) {
                const auto intoBar = line - start;
                const auto intoBeat = intoBar % beat;
                if (intoBeat == 0)
                    return;
                std::optional<BarPosition> label;
                if (labelStep != 0 && intoBeat % labelStep == 0 &&
                    x(std::min(end, line + beat - intoBeat)) - x(line) >= kRulerLabelSpacing)
                    label = BarPosition{bar, intoBar / beat + 1, intoBeat / sixteenth + 1};
                visit(line, label);
            });
        });
    }
    // The grid line after `tick`, or the one before it: where an arrow key takes the insert
    // marker, a clip or a selection's edge (online §41.4.1.3). Never before the start.
    std::int64_t nextGridLine(std::int64_t tick, bool forward) const noexcept {
        return forward ? snap(tick + 1, true) : std::max<std::int64_t>(0, snap(tick - 1, false));
    }
    // The bar holding `tick` (from 0): its number from 0, its start and its end, cut short by a
    // change that falls inside it.
    struct BarSpan {
        std::int64_t index = 0, start = 0, end = 0;
    };
    BarSpan barHolding(std::int64_t tick) const noexcept {
        const auto &m = *meters;
        std::int64_t start = 0, before = 0;
        const textproj::Meter *meter = &m.front();
        std::size_t next = 1;
        for (; next < m.size() && m[next].start_ticks <= tick; ++next) {
            const auto bt = std::max<std::int64_t>(1, meter->barTicks());
            const auto span = m[next].start_ticks - start;
            before += span / bt + (span % bt != 0 ? 1 : 0);
            start = m[next].start_ticks;
            meter = &m[next];
        }
        const auto bt = std::max<std::int64_t>(1, meter->barTicks());
        const auto k = (tick - start) / bt;
        auto end = start + (k + 1) * bt;
        if (next < m.size())
            end = std::min(end, m[next].start_ticks);
        return {before + k, start + k * bt, end};
    }
    // Where a count of bars from the start falls: whole bars at their lines, a part of one through it.
    std::int64_t tickOfBars(double bars) const noexcept {
        const auto whole = static_cast<std::int64_t>(std::floor(bars + 1e-9));
        const auto start = tickOf({whole + 1, 1, 1}, *meters).value_or(0);
        const double part = bars - static_cast<double>(whole);
        if (part < 1e-9)
            return start;
        const auto end = tickOf({whole + 2, 1, 1}, *meters).value_or(start);
        return start + std::llround(part * static_cast<double>(end - start));
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
    void fit(std::int64_t start, std::int64_t end, int width, bool remember = true, double minimumQuarters = 1.) noexcept {
        if (remember) {
            if (historySize == history.size()) {
                std::move(history.begin() + 1, history.end(), history.begin());
                --historySize;
            }
            history[historySize++] = {left, scale};
        }
        left = static_cast<double>(start) / textproj::kPPQ;
        scale = std::clamp(static_cast<double>(std::max(width, 1)) /
                               std::max(minimumQuarters, static_cast<double>(end - start) / textproj::kPPQ),
                           1., 4096.);
    }
    void back() noexcept {
        if (historySize) {
            const auto v = history[--historySize];
            left = v.left;
            scale = v.scale;
        }
    }
    // The first row of the foot: the returns and Main at the end of the rows, or the rows' size
    // when there are none (or no order, as for a geometry used without its view).
    std::size_t footStart(const Rows &all) const noexcept {
        auto i = all.size();
        while (i > 0 && all[i - 1].place.foot())
            --i;
        return i;
    }
    static constexpr int takeLaneHeight = 40;
    std::set<std::int64_t> shownTakeLanes;
    std::span<const engine::TakeLaneNode> takeLanes(const engine::TrackNode &track) const noexcept {
        if (shown(track.id) && shownTakeLanes.contains(track.id) && !foldedTracks.contains(track.id))
            return track.takeLanes;
        return {};
    }
    int rowHeight(const engine::TrackNode &track) const noexcept {
        return stackHeightFor(track.id) + takeLaneHeight * static_cast<int>(takeLanes(track).size());
    }
    struct TakeLaneHit {
        const engine::TrackNode *track;
        const engine::TakeLaneNode *lane;
    };
    std::optional<TakeLaneHit> takeLaneAt(const SnapshotReader &r, int y) const noexcept {
        int top = 0;
        const auto position = rowAt(r, y, &top);
        const auto all = rows(r);
        if (position >= all.size())
            return {};
        const auto *track = all[position].track;
        const auto offset = y - top - stackHeightFor(track->id);
        const auto visible = takeLanes(*track);
        if (offset < 0 || static_cast<std::size_t>(offset / takeLaneHeight) >= visible.size())
            return {};
        return TakeLaneHit{track, &visible[static_cast<std::size_t>(offset / takeLaneHeight)]};
    }
    // The foot's height, and the tracks' above it, as shown.
    int footHeight(const SnapshotReader &r) const noexcept {
        const auto all = rows(r);
        int total = 0;
        for (auto i = footStart(all); i < all.size(); ++i)
            total += rowHeight(*all[i].track);
        return total;
    }
    int tracksHeight(const SnapshotReader &r) const noexcept {
        const auto all = rows(r);
        const auto foot = footStart(all);
        int total = 0;
        for (std::size_t i = 0; i < foot; ++i)
            total += rowHeight(*all[i].track);
        return total;
    }
    // Where the foot begins in the lanes: at their bottom, whatever the scroll; right after the
    // tracks for a geometry without its view. A foot taller than the lanes leaves the tracks one
    // row of the smallest height, and is cut off at the bottom.
    int footTop(const SnapshotReader &r) const noexcept {
        if (lanesHeight <= 0)
            return tracksHeight(r) - scrollY;
        return std::max(std::min(kMinTrackHeight, lanesHeight), lanesHeight - footHeight(r));
    }
    // Where a track's row begins in the lanes; -1 when it has none. A walk down the rows does the
    // same: from -scrollY, and at the foot's first row, from footTop.
    int rowTop(const SnapshotReader &r, std::int64_t id) const noexcept {
        const auto all = rows(r);
        const auto foot = footStart(all);
        int y = -scrollY;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (i == foot)
                y = footTop(r);
            if (all[i].track->id == id)
                return heightFor(id) > 0 ? y : -1;
            y += rowHeight(*all[i].track);
        }
        return -1;
    }
    // The most the tracks scroll: the foot never scrolls, and the last track keeps a sliver in
    // sight.
    int maxScroll(const SnapshotReader &r) const noexcept { return std::max(0, tracksHeight(r) - kMinTrackHeight); }
    // Scrolls to show track `id` above the foot. Pointer selection can leave a partly visible row in place.
    // The foot is pinned and a geometry without its view has nothing to scroll. True when the scroll moved.
    bool reveal(const SnapshotReader &r, std::int64_t id, bool fully = true) noexcept {
        const auto all = rows(r);
        const auto i = all.find(id);
        const int h = stackHeightFor(id);
        if (lanesHeight <= 0 || i >= footStart(all) || h <= 0)
            return false;
        int top = 0;
        for (std::size_t k = 0; k < i; ++k)
            top += rowHeight(*all[k].track);
        const int before = scrollY;
        const int footY = footTop(r);
        if (!fully && top + h > scrollY && top < scrollY + footY)
            return false;
        if (top + h - scrollY > footY)
            scrollY = top + h - footY;
        if (top < scrollY)
            scrollY = top;
        scrollY = std::clamp(scrollY, 0, maxScroll(r));
        return scrollY != before;
    }
    // Clip rows alone are hit targets for existing clip and mixer interactions.
    const engine::TrackNode *trackAt(const SnapshotReader &r, int y) const noexcept {
        const auto all = rows(r);
        int top = 0;
        const auto i = rowAt(r, y, &top);
        if (i >= all.size())
            return nullptr;
        const auto *track = all[i].track;
        return y - top < heightFor(track->id) ? track : nullptr;
    }
    // The track stack at `y`, including separate automation lanes, or the rows'
    // size when there is none; `top` gets the start of its clip row.
    // The foot stands over the tracks scrolled under it, so a point there is the foot's.
    std::size_t rowAt(const SnapshotReader &r, int y, int *top = nullptr) const noexcept {
        const auto all = rows(r);
        if (y < 0)
            return all.size();
        const auto foot = footStart(all);
        const int footY = footTop(r);
        const bool inFoot = foot < all.size() && y >= footY;
        int at = inFoot ? footY : -scrollY;
        for (std::size_t i = inFoot ? foot : 0; i < (inFoot ? all.size() : foot); ++i) {
            const int h = rowHeight(*all[i].track);
            if (y >= at && y < at + h) {
                if (top != nullptr)
                    *top = at;
                return i;
            }
            at += h;
        }
        return all.size();
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
// Track `id`'s name as its header shows it, from the geometry's rows (what the window draws by): a
// stored "# Audio" at 3 reads "3 Audio" (walk-B B13). Empty for a track the rows do not hold.
inline std::string shownName(const ArrangementGeometry &geometry, const SnapshotReader &reader, std::int64_t id) {
    const auto rows = geometry.rows(reader);
    const auto at = rows.find(id);
    return at < rows.size() ? displayName(rows[at].track->name, rows[at].number()) : std::string();
}
} // namespace adi::ui
