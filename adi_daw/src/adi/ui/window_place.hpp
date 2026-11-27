// SPDX-License-Identifier: GPL-3.0-or-later
//
// WIN-02 (docs/PLAN.md; SPEC §8.4; ADR-0063 d2, ADR-0180 d3 and d7): one rule for where every window
// opens again. JUCE-free, so the rule is tested without a desktop.
//  - sanitize what a record holds;
//  - find the monitor a place was saved on: its id, then its area, then its origin, then the one monitor
//    of the same size and DPI, then the main display;
//  - restore: as it was on the same monitor, proportionally when that monitor's size changed, at the
//    same offset on the main display when it is gone, and there wholly on the screen;
//  - clamp the size to the monitor's usable area, and keep a grab of the title bar on it;
//  - cascade a window that would open at another's corner.
#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace adi::ui::window_place {
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int right() const noexcept { return x + w; }
    int bottom() const noexcept { return y + h; }
    bool empty() const noexcept { return w <= 0 || h <= 0; }
    friend bool operator==(const Rect &, const Rect &) = default;
};
inline Rect intersection(const Rect &a, const Rect &b) noexcept {
    const int x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    const int r = std::min(a.right(), b.right()), t = std::min(a.bottom(), b.bottom());
    return r > x && t > y ? Rect{x, y, r - x, t - y} : Rect{};
}
inline std::int64_t overlap(const Rect &a, const Rect &b) noexcept {
    const auto i = intersection(a, b);
    return static_cast<std::int64_t>(i.w) * i.h;
}
// A display as the desktop lists it: its whole area, the part windows may use (no task bar or menu
// bar), its DPI, and whether it is the main display.
struct Monitor {
    std::string id;
    Rect area, usable;
    int dpi = 96;
    bool main = false;
};
// Where a window was: the display it was on, that display's area and DPI then, its rectangle, and
// whether it was full screen.
struct Place {
    std::string display;
    Rect area;
    int dpi = 0;
    Rect rect;
    bool fullScreen = false;
};
// How much of the title bar stays on a screen, whatever else is off it: 40 units across, 18 down
// (WI1's Grab).
struct Grab {
    int height = 18, width = 40;
};
inline constexpr int kLimit = 32768, kCascade = 22;
// A record's rectangle made one the rule can place: its size within [min, kLimit], its corner
// within ±kLimit.
inline Rect sanitize(Rect r, int minW, int minH) noexcept {
    r.w = std::clamp(r.w, std::max(1, minW), kLimit);
    r.h = std::clamp(r.h, std::max(1, minH), kLimit);
    r.x = std::clamp(r.x, -kLimit, kLimit);
    r.y = std::clamp(r.y, -kLimit, kLimit);
    return r;
}
// The main display: the one marked so, else the first; -1 with none.
inline int mainOf(const std::vector<Monitor> &ms) noexcept {
    for (std::size_t i = 0; i < ms.size(); ++i)
        if (ms[i].main)
            return static_cast<int>(i);
    return ms.empty() ? -1 : 0;
}
// The monitor showing most of `r`; the main display when none shows any of it.
inline int monitorOf(const Rect &r, const std::vector<Monitor> &ms) noexcept {
    int best = -1;
    std::int64_t most = 0;
    for (std::size_t i = 0; i < ms.size(); ++i)
        if (const auto o = overlap(r, ms[i].area); o > most) {
            most = o;
            best = static_cast<int>(i);
        }
    return best >= 0 ? best : mainOf(ms);
}
struct Found {
    int index = -1;
    bool gone = false; // no monitor answers the place: the main display stands in
};
inline Found identify(const Place &p, const std::vector<Monitor> &ms) {
    const auto first = [&](auto &&match) -> int {
        for (std::size_t i = 0; i < ms.size(); ++i)
            if (match(ms[i]))
                return static_cast<int>(i);
        return -1;
    };
    if (int i = first([&](const Monitor &m) { return !p.display.empty() && m.id == p.display; }); i >= 0)
        return {i, false};
    if (!p.area.empty()) {
        if (int i = first([&](const Monitor &m) { return m.area == p.area; }); i >= 0)
            return {i, false};
        if (int i = first([&](const Monitor &m) { return m.area.x == p.area.x && m.area.y == p.area.y; }); i >= 0)
            return {i, false};
        int same = -1, count = 0;
        for (std::size_t i = 0; i < ms.size(); ++i)
            if (ms[i].area.w == p.area.w && ms[i].area.h == p.area.h && (p.dpi == 0 || ms[i].dpi == p.dpi)) {
                same = static_cast<int>(i);
                ++count;
            }
        if (count == 1)
            return {same, false};
    }
    return {mainOf(ms), true};
}
// The size within [min, usable] (the usable size wins when it is the smaller), then the whole
// rectangle on the usable area.
inline Rect clampOnto(Rect r, const Rect &usable, int minW, int minH) noexcept {
    r.w = std::max(1, std::min(std::max(r.w, minW), usable.w));
    r.h = std::max(1, std::min(std::max(r.h, minH), usable.h));
    r.x = std::clamp(r.x, usable.x, usable.right() - r.w);
    r.y = std::clamp(r.y, usable.y, usable.bottom() - r.h);
    return r;
}
// SC-428: a window of a size it does not choose (a plug-in's) moved, not resized, to lie wholly on the
// monitor it stands on most, as far as it fits: its left and top edges first when it is larger.
inline Rect wholly(Rect r, const std::vector<Monitor> &ms) noexcept {
    if (ms.empty())
        return r;
    const auto &usable = ms[static_cast<std::size_t>(monitorOf(r, ms))].usable;
    r.x = r.w >= usable.w ? usable.x : std::clamp(r.x, usable.x, usable.right() - r.w);
    r.y = r.h >= usable.h ? usable.y : std::clamp(r.y, usable.y, usable.bottom() - r.h);
    return r;
}
// True when a monitor's usable area holds a grab of the title bar: never above the screen's top,
// a grab's height of it down and a grab's width across on one monitor.
inline bool reachable(const Rect &r, const std::vector<Monitor> &ms, Grab g = {}) noexcept {
    const Rect bar{r.x, r.y, r.w, g.height};
    for (const auto &m : ms) {
        const auto i = intersection(bar, m.usable);
        if (r.y >= m.usable.y && i.h >= g.height && i.w >= std::min(g.width, r.w))
            return true;
    }
    return false;
}
// Moved the least that keeps a grab of the title bar on `usable`: its top on the screen, at least a
// grab's width of it across.
inline Rect keptReachable(Rect r, const Rect &usable, Grab g = {}) noexcept {
    const int grab = std::min(g.width, r.w);
    r.y = std::clamp(r.y, usable.y, std::max(usable.y, usable.bottom() - g.height));
    r.x = std::clamp(r.x, usable.x - r.w + grab, std::max(usable.x - r.w + grab, usable.right() - grab));
    return r;
}
// Where a saved place opens on today's monitors.
inline Rect restore(const Place &p, const std::vector<Monitor> &ms, int minW, int minH, Grab g = {}) {
    auto r = sanitize(p.rect, minW, minH);
    if (ms.empty())
        return r;
    const auto found = identify(p, ms);
    const auto &m = ms[static_cast<std::size_t>(found.index)];
    if (found.gone) {
        // On the main display at the same offset from its corner, and wholly on it.
        if (!p.area.empty()) {
            r.x = m.area.x + (r.x - p.area.x);
            r.y = m.area.y + (r.y - p.area.y);
        }
        return clampOnto(r, m.usable, minW, minH);
    }
    if (!p.area.empty() && (p.area.w != m.area.w || p.area.h != m.area.h)) {
        // The same monitor at another size or scale: where it stood, in proportion.
        const auto scale = [](int v, int from, int to) {
            return static_cast<int>(static_cast<std::int64_t>(v) * to / std::max(1, from));
        };
        r = {m.area.x + scale(r.x - p.area.x, p.area.w, m.area.w), m.area.y + scale(r.y - p.area.y, p.area.h, m.area.h),
             scale(r.w, p.area.w, m.area.w), scale(r.h, p.area.h, m.area.h)};
    } else if (!p.area.empty() && (p.area.x != m.area.x || p.area.y != m.area.y)) {
        // The same monitor moved on the desktop: the window with it.
        r.x += m.area.x - p.area.x;
        r.y += m.area.y - p.area.y;
    }
    r.w = std::max(1, std::min(std::max(r.w, minW), m.usable.w));
    r.h = std::max(1, std::min(std::max(r.h, minH), m.usable.h));
    return keptReachable(r, m.usable, g);
}
// A window that would open at another's top left steps down and right until it does not, back to
// the usable area's corner when a step would take it past the edge.
inline Rect cascade(Rect r, const std::vector<Rect> &others, const Rect &usable) {
    for (std::size_t tries = 0; tries <= others.size(); ++tries) {
        const bool taken = std::any_of(others.begin(), others.end(),
                                       [&](const Rect &o) { return o.x == r.x && o.y == r.y; });
        if (!taken)
            break;
        r.x += kCascade;
        r.y += kCascade;
        if (r.right() > usable.right() || r.bottom() > usable.bottom()) {
            r.x = usable.x;
            r.y = usable.y;
        }
    }
    return r;
}
// The place to keep for a window standing at `r`: the monitor showing most of it.
inline Place placeOf(const Rect &r, bool fullScreen, const std::vector<Monitor> &ms) {
    Place p;
    p.rect = r;
    p.fullScreen = fullScreen;
    if (const int i = monitorOf(r, ms); i >= 0) {
        p.display = ms[static_cast<std::size_t>(i)].id;
        p.area = ms[static_cast<std::size_t>(i)].area;
        p.dpi = ms[static_cast<std::size_t>(i)].dpi;
    }
    return p;
}
// A place as window.places holds it: {display, area: [x, y, w, h], dpi, rect: [x, y, w, h], fullScreen}.
inline nlohmann::json toJson(const Place &p) {
    const auto rect = [](const Rect &r) { return nlohmann::json::array({r.x, r.y, r.w, r.h}); };
    return {{"display", p.display}, {"area", rect(p.area)}, {"dpi", p.dpi}, {"rect", rect(p.rect)}, {"fullScreen", p.fullScreen}};
}
// None for a record the rule cannot read; what it can read of one, otherwise (a missing area or DPI
// reads as unknown).
inline std::optional<Place> fromJson(const nlohmann::json &j) {
    if (!j.is_object())
        return std::nullopt;
    const auto rect = [](const nlohmann::json &a) -> std::optional<Rect> {
        if (!a.is_array() || a.size() != 4 || !std::all_of(a.begin(), a.end(), [](const auto &v) { return v.is_number(); }))
            return std::nullopt;
        const auto at = [&](std::size_t i) {
            return static_cast<int>(std::clamp(a[i].template get<double>(), -2.0 * kLimit, 2.0 * kLimit));
        };
        return Rect{at(0), at(1), at(2), at(3)};
    };
    Place p;
    const auto r = j.contains("rect") ? rect(j["rect"]) : std::nullopt;
    if (!r || r->empty())
        return std::nullopt;
    p.rect = *r;
    if (j.contains("area"))
        if (const auto a = rect(j["area"]))
            p.area = *a;
    if (const auto d = j.find("display"); d != j.end() && d->is_string())
        p.display = d->get<std::string>();
    if (const auto d = j.find("dpi"); d != j.end() && d->is_number_integer())
        p.dpi = d->get<int>();
    if (const auto f = j.find("fullScreen"); f != j.end() && f->is_boolean())
        p.fullScreen = f->get<bool>();
    return p;
}
} // namespace adi::ui::window_place
