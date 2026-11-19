// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
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
    enum class Shape { Hold, Line, Bend };
    struct Point { float x, y, bend; Shape shape; };
    struct Node { int key, slot, x; float y, bend; Shape shape; };
    using View = std::span<const Node>;
    static constexpr int key(int stage, int band, int lane) { return (stage * 3 + band) * 2 + lane; }
    static int address(int curve, Point p) noexcept {
        return ((curve + 1) << 15) | (static_cast<int>(p.shape) << 13)
               | static_cast<int>(std::lround(std::clamp(p.x, 0.f, 1.f) * 8191.f));
    }
    static int value(Point p) noexcept {
        const float bend = std::clamp(p.bend, -1.f, 1.f);
        const int encoded = 128 + static_cast<int>(std::lround(bend * (bend < 0 ? 128.f : 127.f)));
        return (encoded << 16) | static_cast<int>(std::lround(std::clamp(p.y, 0.f, 1.f) * 65535.f));
    }
    static int curveOf(int addressWord) noexcept { return (addressWord >> 15) - 1; }
    static Point unpack(int a, int v) noexcept {
        const int b = (v >> 16) - 128;
        return {static_cast<float>(a & 8191) / 8191.f, static_cast<float>(v & 65535) / 65535.f,
                static_cast<float>(b) / (b < 0 ? 128.f : 127.f), static_cast<Shape>((a >> 13) & 3)};
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
            if (k < 0 || k >= curves || ((a >> 13) & 3) > 2) continue;
            const auto p = unpack(a, words_[2 * i + 1]);
            nodes_[count++] = {k, i, a & 8191, p.y, p.bend, p.shape};
            ++starts_[static_cast<std::size_t>(k + 1)];
        }
        std::sort(nodes_.begin(), nodes_.begin() + count, [](const Node& a, const Node& b) {
            return std::tie(a.key, a.x, a.slot) < std::tie(b.key, b.x, b.slot);
        });
        for (int i = 1; i <= curves; ++i) starts_[i] += starts_[i - 1];
    }
    View view(int k) const noexcept {
        return {nodes_.data() + starts_[k], starts_[k + 1] - starts_[k]};
    }
    bool hasStage(int stage) const noexcept { return starts_[6 * stage] != starts_[6 * stage + 6]; }
    static float at(View nodes, std::uint64_t phase) noexcept {
        const double x = static_cast<double>(phase >> 11) * (8191.0 / 9007199254740992.0);
        const auto upper = std::upper_bound(nodes.begin(), nodes.end(), x,
            [](double needle, const Node& node) { return needle < node.x; });
        const auto& left = upper == nodes.begin() ? nodes.back() : *(upper - 1);
        const auto& right = upper == nodes.end() ? nodes.front() : *upper;
        if (left.shape == Shape::Hold || left.y == right.y) return left.y;
        const double x0 = left.x - (upper == nodes.begin() ? 8191 : 0);
        const double x1 = right.x + (upper == nodes.end() ? 8191 : 0);
        if (x1 <= x0) return right.y;
        float t = static_cast<float>((x - x0) / (x1 - x0));
        if (left.shape == Shape::Bend) t += left.bend * t * (t - 1.f);
        return left.y + (right.y - left.y) * t;
    }
private:
    std::array<int, 2 * slots> words_{};
    std::array<Node, slots> nodes_{};
    std::array<std::size_t, curves + 1> starts_{};
    bool dirty_ = false;
};
} // namespace adi::dsp
