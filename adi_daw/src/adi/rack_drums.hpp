// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/audio/io_audit.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace adi::rack {

struct DrumChainMapping {
    std::int64_t chain = 0;
    std::optional<std::uint8_t> receive; ///< All Notes if absent
    std::optional<std::uint8_t> play; ///< retain the incoming key if absent
    std::optional<std::uint8_t> choke; ///< 1 to 16, when configured
};
struct DrumDestination {
    std::int64_t chain = 0;
    std::uint8_t key = 0;
    std::optional<std::uint8_t> choke;
    bool operator==(const DrumDestination &) const = default;
};

/// Compiled on the owner and retained by one rendering generation. Only the
/// explicit pad and the All Notes group are read for a played key.
class DrumPadMap {
public:
    enum class Status { Ready, InvalidKey, InsufficientSpace };
    struct Result {
        Status status = Status::Ready;
        std::size_t size = 0;
    };
    [[nodiscard]] static std::optional<DrumPadMap>
    compile(std::span<const DrumChainMapping> chains, std::string &error);

    /// Writes an entire note fan-out in authored chain order, or leaves
    /// output untouched. Choke is a destination tag for the owning router.
    [[nodiscard]] Result route(int key, std::span<DrumDestination> output) const noexcept {
        if (key < 0 || key >= static_cast<int>(pads_.size()))
            return {Status::InvalidKey, 0};
        const auto &pad = pads_[static_cast<std::size_t>(key)];
        const auto count = all_.size() + pad.size();
        if (output.size() < count)
            return {Status::InsufficientSpace, count};
        std::size_t shared = 0, dedicated = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const bool chooseShared = shared < all_.size() &&
                (dedicated == pad.size() || all_[shared].order < pad[dedicated].order);
            const auto &chain = chooseShared ? all_[shared++].mapping : pad[dedicated++].mapping;
            output[i] = {chain.chain, chain.play.value_or(static_cast<std::uint8_t>(key)),
                         chain.choke};
        }
        return {Status::Ready, count};
    }

private:
    struct Chain {
        std::size_t order;
        DrumChainMapping mapping;
    };
    std::array<std::vector<Chain>, 128> pads_;
    std::vector<Chain> all_;
};

inline std::optional<DrumPadMap> DrumPadMap::compile(std::span<const DrumChainMapping> chains,
                                                    std::string &error) {
    if (audio::onAudioThread) {
        error = "callback";
        return {};
    }
    DrumPadMap result;
    std::set<std::int64_t> identities;
    for (std::size_t i = 0; i < chains.size(); ++i) {
        const auto &chain = chains[i];
        if ((chain.receive && *chain.receive > 127) || (chain.play && *chain.play > 127) ||
            (chain.choke && (*chain.choke == 0 || *chain.choke > 16))) {
            error = "Drum chain mapping has an invalid key or choke group";
            return {};
        }
        if (!identities.insert(chain.chain).second) {
            error = "Drum chain mapping repeats a chain identity";
            return {};
        }
        auto &target = chain.receive ? result.pads_[*chain.receive] : result.all_;
        target.push_back({i, chain});
    }
    error.clear();
    return result;
}

} // namespace adi::rack
