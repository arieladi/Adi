// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/textproj.hpp"
#include "snapshot_reader.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
namespace adi::ui {
// View-only coordinates and selection. No committed project state is copied here.
struct ArrangementGeometry {
    double left = 0, scale = 48;
    int height = 64, scrollY = 0;
    std::map<std::int64_t, int> heights;
    int heightFor(std::int64_t id) const noexcept {
        const auto i = heights.find(id);
        return i == heights.end() ? height : i->second;
    }
    void resizeTrack(std::int64_t id, int next, bool all = false) {
        next = std::clamp(next, 24, 512);
        if (all) {
            height = next;
            heights.clear();
        } else if (id)
            heights[id] = next;
    }
    void wheelHeight(const SnapshotReader &reader, std::int64_t under, int delta) {
        bool changed = false;
        if (selectionStart != selectionEnd) {
            const auto a = std::min(selectionStart, selectionEnd),
                       b = std::max(selectionStart, selectionEnd);
            for (const auto &t : reader.tracks())
                for (const auto &c : t->clips)
                    if (c->posTicks < b && c->posTicks + c->lengthTicks > a) {
                        resizeTrack(t->id, heightFor(t->id) + delta);
                        changed = true;
                        break;
                    }
        }
        if (!changed)
            resizeTrack(under, heightFor(under) + delta);
    }
    std::int64_t selectedTrack = 0, selectedClip = 0, selectionStart = 0, selectionEnd = 0,
                 insert = 0;
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
    void pan(double pixels) noexcept { left = std::clamp(left + pixels / scale, 0., 1e9); }
    void zoom(double factor, double pixel) noexcept {
        const double at = left + pixel / scale;
        scale = std::clamp(scale * factor, 1., 4096.);
        left = std::max(0., at - pixel / scale);
    }
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
        if (y < 0)
            return nullptr;
        int at = -scrollY;
        for (const auto &t : r.tracks())
            if (t->kind != "master") {
                const int h = heightFor(t->id);
                if (y >= at && y < at + h)
                    return t.get();
                at += h;
            }
        return nullptr;
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
