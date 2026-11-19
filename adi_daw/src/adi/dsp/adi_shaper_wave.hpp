// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/dsp/adi_shaper_corner.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <tuple>

namespace adi::dsp {

// A finite pool is shared by the contours in one instance. Each slot has two
// ordinary Pd scalars. Both words round-trip through a float and through a
// normalized host parameter. An empty slot uses a zero address word.
class AdiShaperWaveBank {
public:
    static constexpr int firstId = 2049; // one-based host ID
    static constexpr int slots = 512;
    static constexpr int lastId = firstId + 2 * slots - 1;
    static constexpr int wordMax = 0xffffff;
    static constexpr int curves = 66; // eleven stages, three bands, two lanes
    enum class Shape { Hold, Line, Bend, SourceCorner };
    struct Point { float x, y, bend; Shape shape; };
    struct Node { int key, slot, x; float y, bend; Shape shape; };
    struct CornerRange {
        double start;
        std::array<std::uint16_t,4> controls;
    };
    static_assert(sizeof(CornerRange) == 16);
    struct View {
        std::span<const Node> nodes;
        std::span<const CornerRange> spans;
        std::span<const std::uint16_t> hardJumps;
        bool onlyCorners = false, constant = false;
        mutable AdiShaperCornerSpan::Cursor cursor{};
        mutable AdiShaperCornerSpan polynomial{};
        mutable bool ready = false;
        mutable int cell = -8192;
        mutable std::array<float,4> heights{};
        mutable std::uint8_t limits = 12; // two packed indices, initially [0,3]
        bool empty() const noexcept { return nodes.empty(); }
    };
    static constexpr int key(int stage, int band, int lane) { return (stage * 3 + band) * 2 + lane; }
    static int address(int curve, Point p) noexcept {
        const float scale = p.shape == Shape::SourceCorner ? 8192.f : 8191.f;
        const int x = std::min(8191, static_cast<int>(std::lround(std::clamp(p.x, 0.f, 1.f) * scale)));
        return ((curve + 1) << 15) | (static_cast<int>(p.shape) << 13)
               | x;
    }
    static int value(Point p) noexcept {
        const float bend = std::clamp(p.bend, -1.f, 1.f);
        const int encoded = 128 + static_cast<int>(std::lround(bend * (bend < 0 ? 128.f : 127.f)));
        return (encoded << 16) | static_cast<int>(std::lround(std::clamp(p.y, 0.f, 1.f) * 65535.f));
    }
    static int curveOf(int addressWord) noexcept { return (addressWord >> 15) - 1; }
    static Point unpack(int a, int v) noexcept {
        const int b = (v >> 16) - 128;
        const auto shape = static_cast<Shape>((a >> 13) & 3);
        return {static_cast<float>(a & 8191) / (shape == Shape::SourceCorner ? 8192.f : 8191.f), static_cast<float>(v & 65535) / 65535.f,
                static_cast<float>(b) / (b < 0 ? 128.f : 127.f), shape};
    }
    void set(int hostId, float input) noexcept {
        if (hostId < firstId || hostId > lastId || !std::isfinite(input)) return;
        auto& dest = words_[static_cast<std::size_t>(hostId - firstId)];
        const int next = static_cast<int>(std::lround(std::clamp(input, 0.f, static_cast<float>(wordMax))));
        if (next != dest) { dest = next; dirty_ = true; }
    }
    void prepareBlock() noexcept {
        if (!dirty_) return;
        dirty_ = false;
        starts_.fill(0);
        std::size_t count = 0;
        for (int i = 0; i < slots; ++i) {
            const int a = words_[2 * i], k = curveOf(a);
            if (k < 0 || k >= curves) continue;
            const auto p = unpack(a, words_[2 * i + 1]);
            nodes_[count++] = {k, i, a & 8191, p.y, p.bend, p.shape};
            ++starts_[static_cast<std::size_t>(k + 1)];
        }
        std::sort(nodes_.begin(), nodes_.begin() + count, [](const Node& a, const Node& b) {
            return std::tuple(a.key, sortX(a), a.slot) < std::tuple(b.key, sortX(b), b.slot);
        });
        for (int i = 1; i <= curves; ++i) starts_[i] += starts_[i - 1];
        std::size_t used = 0, jumpCount = 0;
        for (int k = 0; k < curves; ++k) {
            spanStarts_[k] = used;
            jumpStarts_[k] = jumpCount;
            const std::span<const Node> nodes(nodes_.data()+starts_[k], starts_[k+1]-starts_[k]);
            constants_[k] = !nodes.empty() && std::all_of(nodes.begin(), nodes.end(), [&](const Node& n) {
                return n.y == nodes.front().y;
            });
            onlyCorners_[k] = false;
            if (!nodes.empty() && !constants_[k]) {
                used += compileCorners(nodes, spans_.data()+used, onlyCorners_[k]);
                for (std::size_t i=1; i<nodes.size(); ++i) {
                    const auto& a = nodes[i-1];
                    const auto& b = nodes[i];
                    if (a.shape == Shape::SourceCorner && b.shape == Shape::SourceCorner &&
                        a.bend < -.5f && b.bend < -.5f && a.x == b.x && a.y != b.y)
                        jumps_[jumpCount++] = static_cast<std::uint16_t>(a.x);
                }
            }
        }
        spanStarts_[curves] = used;
        jumpStarts_[curves] = jumpCount;
    }
    View view(int k) const noexcept {
        return {{nodes_.data() + starts_[k], starts_[k + 1] - starts_[k]},
                {spans_.data() + spanStarts_[k], spanStarts_[k+1] - spanStarts_[k]},
                {jumps_.data() + jumpStarts_[k], jumpStarts_[k+1] - jumpStarts_[k]}, onlyCorners_[k], constants_[k]};
    }
    bool hasStage(int stage) const noexcept { return starts_[6 * stage] != starts_[6 * stage + 6]; }
    static float at(const View& curve, std::uint64_t phase, bool* moving = nullptr) noexcept {
        const auto nodes = curve.nodes;
        if (nodes.empty()) {
            if (moving) *moving = false;
            return 0;
        }
        if (curve.constant) {
            if (moving) *moving = false;
            return nodes.front().y;
        }
        if (!curve.spans.empty()) {
            const double x = static_cast<double>(phase >> 11) * (8192.0 / 9007199254740992.0);
            if (!curve.onlyCorners) {
                const auto next = std::upper_bound(nodes.begin(), nodes.end(), x,
                    [](double position, const Node& n) { return position < cornerX(n); });
                const auto& left = next == nodes.begin() ? nodes.back() : *(next-1);
                const auto& right = next == nodes.end() ? nodes.front() : *next;
                // Hold and Bend retain their outgoing segment. A Line terminating
                // at a rounded corner participates in the incoming spline.
                if (left.shape != Shape::SourceCorner && (left.shape != Shape::Line || right.shape != Shape::SourceCorner)) {
                    const double from = cornerX(left) - (next == nodes.begin() ? 8192 : 0);
                    const double to = cornerX(right) + (next == nodes.end() ? 8192 : 0);
                    return segment(left,right,x,from,to,moving);
                }
            }
            return sampled(curve,x,moving);
        }
        const double x = static_cast<double>(phase >> 11) * (8191.0 / 9007199254740992.0);
        const auto upper = std::upper_bound(nodes.begin(), nodes.end(), x,
            [](double needle, const Node& node) { return needle < node.x; });
        const auto& left = upper == nodes.begin() ? nodes.back() : *(upper - 1);
        const auto& right = upper == nodes.end() ? nodes.front() : *upper;
        const double x0 = left.x - (upper == nodes.begin() ? 8191 : 0);
        const double x1 = right.x + (upper == nodes.end() ? 8191 : 0);
        return segment(left,right,x,x0,x1,moving);
    }
private:
    static float geometry(const View& curve, double x, bool* moving = nullptr) noexcept {
        const double start = curve.spans.front().start;
        const double at = x < start ? x+8192 : x >= start+8192 ? x-8192 : x;
        auto& cursor = curve.cursor;
        const double end = cursor.span+1 == curve.spans.size() ? start+8192 : curve.spans[cursor.span+1].start;
        const bool sameSpan = curve.ready && at >= curve.spans[cursor.span].start && at < end;
        if (!sameSpan) {
            const auto next = std::upper_bound(curve.spans.begin(),curve.spans.end(),at,
                [](double position, const CornerRange& span) { return position < span.start; });
            cursor.span = next == curve.spans.begin() ? 0 : static_cast<std::size_t>(next-curve.spans.begin()-1);
            const auto& points = curve.spans[cursor.span].controls;
            auto point = [&](std::uint16_t id) -> AdiShaperCornerSpan::XY {
                const auto& n = curve.nodes[id % slots];
                return {cornerX(n)+(static_cast<int>(id)/slots-1)*8192., n.y};
            };
            curve.polynomial = AdiShaperCornerSpan::make(point(points[0]),point(points[1]),
                                                          point(points[2]),point(points[3]));
            curve.ready = true;
            cursor.inverseSpeed = 0;
        }
        if (moving) *moving = !curve.polynomial.flat;
        return curve.polynomial.at(at,cursor,sameSpan);
    }
    static std::uint8_t tapLimits(const View& curve, int cell) noexcept {
        // At a coincident hard pair, crossed taps read the nearest grid
        // sample on their own side of the jump. Before the edge this is
        // the last preceding grid sample, including its incoming slope.
        const auto jumps = curve.hardJumps;
        if (jumps.empty()) return 12;
        auto cut = [&](int grid) { return std::binary_search(jumps.begin(),jumps.end(),grid&8191); };
        const int low = cut(cell) ? 1 : 0;
        const int high = cut(cell+1) ? 1 : cut(cell+2) ? 2 : 3;
        return static_cast<std::uint8_t>(low + 4*high);
    }
    static float sampled(const View& curve, double x, bool* moving) noexcept {
        const int cell = static_cast<int>(x);
        auto& values = curve.heights;
        if (cell != curve.cell) {
            const bool reuse = curve.cell >= 0 && ((curve.cell+1)&8191) == cell;
            curve.limits = tapLimits(curve,cell);
            if (reuse) {
                values[0] = values[1]; values[1] = values[2]; values[2] = values[3];
                values[3] = geometry(curve,(cell+2)&8191);
            } else {
                for (int i=0; i<4; ++i) values[i] = geometry(curve,(cell+i-1)&8191);
            }
            curve.cell = cell;
        }
        const int low = curve.limits & 3, high = curve.limits >> 2;
        const double a = values[low], b = values[1], c = values[std::min(2,high)], d = values[high];
        if (moving) *moving = a != b || b != c || c != d;
        const double t = x-cell;
        const double first = .5*(c-a), second = .5*(d-b);
        return static_cast<float>(b+t*(first+t*(3*(c-b)-2*first-second+t*(2*(b-c)+first+second))));
    }
    static float segment(const Node& left, const Node& right, double x, double x0, double x1, bool* moving) noexcept {
        const bool varies = left.shape != Shape::Hold && left.y != right.y;
        if (moving) *moving = varies;
        if (!varies) return left.y;
        if (x1 <= x0) return right.y;
        float t = static_cast<float>((x - x0) / (x1 - x0));
        if (left.shape == Shape::Bend) t += left.bend * t * (t - 1.f);
        return left.y + (right.y - left.y) * t;
    }
    static int sortX(const Node& n) noexcept {
        return n.x * (n.shape == Shape::SourceCorner ? 8191 : 8192);
    }
    static double cornerX(const Node& n) noexcept {
        return n.shape == Shape::SourceCorner ? n.x : n.x * (8192. / 8191.);
    }
    static std::size_t compileCorners(std::span<const Node> nodes, CornerRange* out, bool& only) noexcept {
        const auto countCorners = std::count_if(nodes.begin(),nodes.end(),[](const Node& n) {
            return n.shape == Shape::SourceCorner;
        });
        if (!countCorners) return 0;
        only = static_cast<std::size_t>(countCorners) == nodes.size();
        std::array<std::uint16_t, 3*slots> sequence;
        int count = 0;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& n = nodes[i];
            const int repeats = n.shape != Shape::SourceCorner || n.bend < -.5f ? 3 : n.bend > .5f ? 1 : 2;
            for (int j = 0; j < repeats; ++j) sequence[count++] = static_cast<std::uint16_t>(i);
        }
        // Four local indices and their wrap describe a span. Cache its cubic
        // coefficients in the per-reader cursor only when playback reaches it.
        auto id = [&](int i) {
            const int cycle = i < 0 ? -1 : i/count;
            return static_cast<std::uint16_t>(sequence[i-cycle*count] + slots*(cycle+1));
        };
        auto x = [&](std::uint16_t encoded) {
            return cornerX(nodes[encoded % slots]) + (static_cast<int>(encoded)/slots-1)*8192.;
        };
        std::size_t used = 0;
        auto p = id(-1), q = id(0), r = id(1);
        double from = (x(p)+4*x(q)+x(r))/6;
        for (int i = 0; i < count; ++i) {
            const auto s = id(i+2);
            const double to = (x(q)+4*x(r)+x(s))/6;
            if (to > from) out[used++] = {from, {p,q,r,s}};
            from = to;
            p = q; q = r; r = s;
        }
        return used;
    }
    std::array<CornerRange, 3*slots> spans_;
    std::array<std::size_t, curves+1> spanStarts_{}, jumpStarts_{};
    std::array<std::uint16_t, slots> jumps_{};
    std::array<bool, curves> onlyCorners_{}, constants_{};
    std::array<int, 2 * slots> words_{};
    std::array<Node, slots> nodes_{};
    std::array<std::size_t, curves + 1> starts_{};
    bool dirty_ = false;
};
} // namespace adi::dsp
