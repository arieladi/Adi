// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "../ChannelLayout.h"
#include <array>
#include <optional>
#include <vector>

namespace adi::mix::limiter
{
struct ChannelGroup
{
    int pair = 0;
    bool shared = true;
    bool stereoPair = false;
};

inline std::optional<std::vector<ChannelGroup>> classifyChannelGroups(const ChannelLayout& layout,
                                                                     bool centerLink, bool lfeLink)
{
    using Channels = ChannelLayout;
    if (layout == Channels::mono()) return std::vector<ChannelGroup>{{0, false}};
    if (layout.size() == 0) return std::nullopt;

    struct Pair
    {
        Channels::ChannelType left, right;
    };
    constexpr std::array pairs{
        Pair{Channels::left, Channels::right},
        Pair{Channels::leftSurround, Channels::rightSurround},
        Pair{Channels::leftSurroundSide, Channels::rightSurroundSide},
        Pair{Channels::leftSurroundRear, Channels::rightSurroundRear},
        Pair{Channels::wideLeft, Channels::wideRight},
        Pair{Channels::topFrontLeft, Channels::topFrontRight},
        Pair{Channels::topRearLeft, Channels::topRearRight},
        Pair{Channels::topSideLeft, Channels::topSideRight},
        Pair{Channels::leftCentre, Channels::rightCentre}
    };
    std::array<bool, pairs.size()> present{};
    bool hasPair = false;
    for (std::size_t index = 0; index < pairs.size(); ++index)
    {
        const auto left = layout.getChannelIndexForType(pairs[index].left) >= 0;
        const auto right = layout.getChannelIndexForType(pairs[index].right) >= 0;
        if (left != right) return std::nullopt;
        present[index] = left;
        hasPair = hasPair || left;
    }
    if (!hasPair) return std::nullopt;

    std::vector<ChannelGroup> result(static_cast<std::size_t>(layout.size()));
    for (int channel = 0; channel < layout.size(); ++channel)
    {
        const auto type = layout.getTypeOfChannel(channel);
        auto& group = result[static_cast<std::size_t>(channel)];
        group = {100 + channel, false};
        if (type == Channels::centre)
        {
            if (centerLink)
            {
                if (present[0]) group.pair = 0;
                group.shared = true;
            }
        }
        else if (type == Channels::centreSurround)
        {
            if (centerLink)
            {
                if (present[1]) group.pair = 1;
                else if (present[3]) group.pair = 3;
                else if (present[2]) group.pair = 2;
                group.shared = true;
            }
        }
        else if (type == Channels::LFE)
        {
            group.shared = lfeLink;
        }
        else
        {
            bool found = false;
            for (std::size_t index = 0; index < pairs.size(); ++index)
                if (type == pairs[index].left || type == pairs[index].right)
                {
                    group = {static_cast<int>(index), true, true};
                    found = true;
                    break;
                }
            if (!found) return std::nullopt;
        }
    }
    return result;
}
}
