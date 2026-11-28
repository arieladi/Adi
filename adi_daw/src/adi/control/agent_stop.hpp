// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace adi::control {

/// A write admitted before Stop stays refused after re-arm. The low bit
/// records whether writes are stopped. Every state change advances the epoch.
class AgentWriteGate {
public:
    [[nodiscard]] std::uint64_t generation() const noexcept { return state_.load(); }

    [[nodiscard]] std::optional<std::uint64_t> admit() const noexcept {
        const auto epoch = generation();
        return (epoch & 1u) == 0 ? std::optional{epoch} : std::nullopt;
    }

    [[nodiscard]] bool stillAdmitted(std::optional<std::uint64_t> epoch) const noexcept {
        return epoch && admit() == epoch;
    }

    [[nodiscard]] bool stopped() const noexcept { return !admit(); }

    void stop() noexcept { setStopped(true); }
    void rearm() noexcept { setStopped(false); }

private:
    void setStopped(bool requested) noexcept {
        auto epoch = state_.load();
        while (((epoch & 1u) != 0) != requested &&
               !state_.compare_exchange_weak(epoch, epoch + 1)) {}
    }

    std::atomic<std::uint64_t> state_{0};
};

}  // namespace adi::control
