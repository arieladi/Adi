// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <span>
#include <stdexcept>

namespace adi::mix::limiter {

// Prepared channel roles, independent of a plug-in SDK or window framework.
// Roles retain their order: channel grouping and loudness weighting use this
// descriptor only at prepare, never infer a surround layout from its count.
class ChannelLayout {
public:
    enum ChannelType {
        unknown, left, right, centre, LFE,
        leftSurround, rightSurround, leftSurroundSide, rightSurroundSide,
        leftSurroundRear, rightSurroundRear, centreSurround, wideLeft, wideRight,
        topFrontLeft, topFrontRight, topRearLeft, topRearRight,
        topSideLeft, topSideRight, leftCentre, rightCentre
    };
    static constexpr int maximumChannels = 64;

    ChannelLayout() = default;
    explicit ChannelLayout(std::span<const ChannelType> roles) {
        if (roles.size() > roles_.size())
            throw std::invalid_argument("Too many limiter channels");
        for (std::size_t i = 0; i < roles.size(); ++i) {
            if (roles[i] == unknown || std::find(roles.begin(), roles.begin() + static_cast<std::ptrdiff_t>(i), roles[i])
                                         != roles.begin() + static_cast<std::ptrdiff_t>(i))
                throw std::invalid_argument("Unknown or repeated limiter channel role");
            roles_[i] = roles[i];
        }
        count_ = static_cast<int>(roles.size());
    }
    ChannelLayout(std::initializer_list<ChannelType> roles)
        : ChannelLayout(std::span<const ChannelType>(roles.begin(), roles.size())) {}

    [[nodiscard]] static ChannelLayout mono() { return {centre}; }
    [[nodiscard]] static ChannelLayout stereo() { return {left, right}; }
    [[nodiscard]] int size() const noexcept { return count_; }
    [[nodiscard]] ChannelType getTypeOfChannel(int channel) const noexcept {
        return channel >= 0 && channel < count_ ? roles_[static_cast<std::size_t>(channel)] : unknown;
    }
    [[nodiscard]] int getChannelIndexForType(ChannelType type) const noexcept {
        for (int i = 0; i < count_; ++i)
            if (roles_[static_cast<std::size_t>(i)] == type) return i;
        return -1;
    }
    bool operator==(const ChannelLayout&) const = default;

private:
    std::array<ChannelType, maximumChannels> roles_{};
    int count_ = 0;
};
} // namespace adi::mix::limiter
