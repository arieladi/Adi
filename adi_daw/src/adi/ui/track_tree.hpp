// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>
namespace adi::ui {
// One track as Bitwig's project view sees it (Master Reference 10, Figure 32; ADR-0112 d2):
// what it sits in, whether it is a group or Main, its colour.
struct TrackRow {
    std::int64_t id = 0, parent = 0; // parent: the group it sits in; 0 at the top
    bool group = false, main = false;
    std::uint32_t colour = 0; // 0xRRGGBB; 0 when the project gives none
    std::string name;
};
// The project's tracks as a tree of groups, in the arrangement's order. The window builds it
// from the snapshot, which does not carry a track's group, kind or colour yet (collab/window.md,
// N6): until it does, every track stands alone and there is no group to choose.
class TrackTree {
  public:
    TrackTree() = default;
    explicit TrackTree(std::vector<TrackRow> rows) : rows_(std::move(rows)) {}
    struct Group {
        std::int64_t id = 0;
        int depth = 0; // 0 at the top, one more for each group it sits in
        const TrackRow *row = nullptr;
    };
    // Every group in the arrangement's order, each with how deep it sits: Bitwig's menu.
    std::vector<Group> groups() const {
        std::vector<Group> out;
        for (const auto &r : rows_)
            if (r.group)
                out.push_back({r.id, depth(r), &r});
        return out;
    }
    bool isGroup(std::int64_t id) const noexcept {
        const auto *r = find(id);
        return r != nullptr && r->group;
    }
    // Whether a view of `group` shows `r`: the group, everything inside it at any depth, and
    // Main, which every view keeps (the director, 28 October 2026). 0 is the whole project.
    bool shows(std::int64_t group, const TrackRow &r) const noexcept {
        return group == 0 || r.main || within(r, group);
    }
    std::set<std::int64_t> shows(std::int64_t group) const {
        std::set<std::int64_t> out;
        for (const auto &r : rows_)
            if (shows(group, r))
                out.insert(r.id);
        return out;
    }
    const std::vector<TrackRow> &rows() const noexcept { return rows_; }
    // Refilled in place by the window on a new generation, so the same rows allocate nothing.
    std::vector<TrackRow> &rows() noexcept { return rows_; }

  private:
    const TrackRow *find(std::int64_t id) const noexcept {
        for (const auto &r : rows_)
            if (r.id == id)
                return &r;
        return nullptr;
    }
    // Whether `r` is `group` or sits in it at any depth. A parent that loops back, which the
    // project should never hold, ends the walk rather than the window.
    bool within(const TrackRow &r, std::int64_t group) const noexcept {
        const TrackRow *at = &r;
        for (std::size_t steps = 0; at != nullptr && steps <= rows_.size(); ++steps) {
            if (at->id == group)
                return true;
            at = at->parent == 0 ? nullptr : find(at->parent);
        }
        return false;
    }
    int depth(const TrackRow &r) const noexcept {
        int d = 0;
        const TrackRow *at = r.parent == 0 ? nullptr : find(r.parent);
        for (; at != nullptr && static_cast<std::size_t>(d) <= rows_.size(); ++d)
            at = at->parent == 0 ? nullptr : find(at->parent);
        return d;
    }
    std::vector<TrackRow> rows_;
};
} // namespace adi::ui
