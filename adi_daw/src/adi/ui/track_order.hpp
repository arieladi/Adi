// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "snapshot_reader.hpp"
#include "track_tree.hpp"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
namespace adi::ui {
// One row of the arrangement: which track, how deep it sits, whether it is a group or Main.
struct TrackPlace {
    std::uint32_t at = 0;            // the track's index in the snapshot
    std::uint32_t depth = 0;         // how many groups it sits in
    std::int64_t id = 0, parent = 0; // parent: the group it sits in; 0 at the top
    bool group = false, main = false;
};
// The one order every panel reads (UI-ARCHITECTURE §4; Live 12 manual 18.3): each group followed
// by what it holds, in their order in it (index_in_parent, then id), at any depth; Main last, as
// Live draws it. A track the rows do not describe, or whose group is not among the tracks, stands
// at the top; so does one in a loop of parents, which the project should never hold.
//
// The snapshot's own order is by index_in_parent alone (engine/snapshot.cpp), which mixes what
// one group holds with what another does; until it carries the tree (collab/window.md, N6) the
// rows come from the project file (track_rows.hpp). Built again in place for each new snapshot,
// so the same tracks allocate nothing.
class TrackOrder {
  public:
    void build(const SnapshotReader &reader, const std::vector<TrackRow> &rows) {
        const auto &tracks = reader.tracks();
        const auto n = static_cast<std::uint32_t>(tracks.size());
        for_ = reader.get();
        // Each row by its track's id, and each track by its id, to look either up.
        sortedIndices(rowsById_, static_cast<std::uint32_t>(rows.size()),
                      [&rows](std::uint32_t i) { return rows[i].id; });
        sortedIndices(tracksById_, n, [&tracks](std::uint32_t i) { return tracks[i]->id; });
        info_.resize(n);
        index_.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            auto &place = info_[i];
            place = {};
            place.at = i;
            place.id = tracks[i]->id;
            index_[i] = 0;
            const auto r = find(rowsById_, place.id, [&rows](std::uint32_t k) { return rows[k].id; });
            if (r == kNone)
                continue;
            const auto &row = rows[r];
            place.group = row.group;
            place.main = row.main;
            index_[i] = row.index;
            if (row.parent != place.id &&
                find(tracksById_, row.parent, [&tracks](std::uint32_t k) { return tracks[k]->id; }) != kNone)
                place.parent = row.parent;
        }
        // What each parent holds, in its order: by parent, Main after the rest, then the index,
        // then the snapshot's order (by id within an index; the snapshot's own without rows).
        children_.resize(n);
        for (std::uint32_t i = 0; i < n; ++i)
            children_[i] = i;
        std::sort(children_.begin(), children_.end(), [this](std::uint32_t a, std::uint32_t b) {
            const auto &x = info_[a], &y = info_[b];
            if (x.parent != y.parent)
                return x.parent < y.parent;
            if (x.main != y.main)
                return y.main;
            if (index_[a] != index_[b])
                return index_[a] < index_[b];
            return a < b;
        });
        // Down the tree from the top, each group followed by what it holds.
        places_.clear();
        placed_.assign(n, 0);
        stack_.clear();
        stack_.push_back(held(0));
        while (!stack_.empty()) {
            auto &range = stack_.back();
            if (range.first == range.second) {
                stack_.pop_back();
                continue;
            }
            const auto c = children_[range.first++];
            if (placed_[c] != 0)
                continue; // a track with id 0 would hold the top again
            auto place = info_[c];
            place.depth = static_cast<std::uint32_t>(stack_.size() - 1);
            places_.push_back(place);
            placed_[c] = 1;
            stack_.push_back(held(place.id));
        }
        // What the walk never reached: a loop of parents. At the top, in the snapshot's order.
        for (std::uint32_t i = 0; i < n; ++i)
            if (placed_[i] == 0) {
                auto place = info_[i];
                place.parent = 0;
                place.depth = 0;
                places_.push_back(place);
            }
    }
    const std::vector<TrackPlace> &places() const noexcept { return places_; }
    // Whether this order was built for `reader`'s snapshot. Compared, never followed.
    bool fits(const SnapshotReader &reader) const noexcept {
        return for_ != nullptr && for_ == reader.get() && places_.size() == reader.tracks().size();
    }

  private:
    static constexpr std::uint32_t kNone = 0xffffffffu;
    template <class Id>
    static void sortedIndices(std::vector<std::uint32_t> &out, std::uint32_t n, Id id) {
        out.resize(n);
        for (std::uint32_t i = 0; i < n; ++i)
            out[i] = i;
        std::sort(out.begin(), out.end(), [&id](std::uint32_t a, std::uint32_t b) { return id(a) < id(b); });
    }
    template <class Id>
    static std::uint32_t find(const std::vector<std::uint32_t> &sorted, std::int64_t value, Id id) {
        const auto at = std::lower_bound(sorted.begin(), sorted.end(), value,
                                         [&id](std::uint32_t k, std::int64_t v) { return id(k) < v; });
        return at != sorted.end() && id(*at) == value ? *at : kNone;
    }
    // The run of `children_` that `parent` holds.
    std::pair<std::uint32_t, std::uint32_t> held(std::int64_t parent) const {
        const auto [from, to] = std::equal_range(
            children_.begin(), children_.end(), parent, Parent{info_});
        return {static_cast<std::uint32_t>(from - children_.begin()),
                static_cast<std::uint32_t>(to - children_.begin())};
    }
    struct Parent {
        const std::vector<TrackPlace> &info;
        bool operator()(std::uint32_t a, std::int64_t p) const noexcept { return info[a].parent < p; }
        bool operator()(std::int64_t p, std::uint32_t a) const noexcept { return p < info[a].parent; }
    };
    const engine::Snapshot *for_ = nullptr;
    std::vector<TrackPlace> places_, info_; // in the arrangement's order; by snapshot index
    std::vector<std::int64_t> index_;
    std::vector<std::uint32_t> rowsById_, tracksById_, children_;
    std::vector<std::uint8_t> placed_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stack_;
};
} // namespace adi::ui
