// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace adi::control {

/// ADR-0203 d20: the rolling rate cap for Apply commits, per client grant.
class ApplyRateLimit {
    struct Charge {
        std::size_t ops;
        std::int64_t committedMs = 0;
    };

    using Charges = std::list<Charge>;

    struct ClientBudget {
        Charges pending;
        Charges committed;
        std::size_t usedOps = 0;
    };

    struct State {
        std::mutex mutex;
        std::unordered_map<std::string, ClientBudget> clients;
    };

public:
    ApplyRateLimit() = default;
    ApplyRateLimit(const ApplyRateLimit&) = delete;
    ApplyRateLimit& operator=(const ApplyRateLimit&) = delete;

    class Reservation {
    public:
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&&) noexcept = default;
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                settle(std::nullopt);
                state_ = std::move(other.state_);
                client_ = other.client_;
                charge_ = std::move(other.charge_);
            }
            return *this;
        }
        ~Reservation() { settle(std::nullopt); }

        /// Keep the charge for 60 seconds from a successful commit.
        void commit(std::int64_t nowMs) noexcept { settle(nowMs); }

    private:
        friend class ApplyRateLimit;

        Reservation(std::shared_ptr<State> state, ClientBudget& client, Charges::iterator charge)
            : state_(std::move(state)), client_(&client), charge_(charge) {}

        void settle(std::optional<std::int64_t> committedMs) noexcept {
            if (!state_) return;
            auto state = std::move(state_);
            std::lock_guard lock(state->mutex);
            const auto charge = std::exchange(charge_, Charges::iterator{});
            if (!committedMs) {
                client_->usedOps -= charge->ops;
                client_->pending.erase(charge);
                return;
            }
            charge->committedMs = *committedMs;
            // Preserve timestamp order if concurrent callers reach the lock
            // in a different order than they sampled the clock.
            auto position = client_->committed.end();
            while (position != client_->committed.begin()) {
                const auto previous = std::prev(position);
                if (previous->committedMs <= *committedMs) break;
                position = previous;
            }
            client_->committed.splice(position, client_->pending, charge);
        }

        std::shared_ptr<State> state_;
        ClientBudget* client_;
        Charges::iterator charge_;
    };

    /// Reserve before writing. A failed write releases the reservation when
    /// it goes out of scope. A replay returns before reserving a second time.
    [[nodiscard]] std::optional<Reservation> reserve(const std::string& credential, std::size_t ops,
                                                      std::size_t maxOps, std::int64_t nowMs) {
        if (ops == 0 || ops > maxOps) return std::nullopt;
        std::lock_guard lock(state_->mutex);
        prune(nowMs);
        auto& client = state_->clients[credential];
        if (client.usedOps > maxOps - ops) return std::nullopt;
        client.pending.push_back({ops});
        client.usedOps += ops;
        return Reservation(state_, client, std::prev(client.pending.end()));
    }

private:
    void prune(std::int64_t nowMs) {
        constexpr std::int64_t windowMs = 60'000;
        if (nowMs < std::numeric_limits<std::int64_t>::min() + windowMs) return;
        const auto cutoff = nowMs - windowMs;
        for (auto client = state_->clients.begin(); client != state_->clients.end();) {
            auto& budget = client->second;
            while (!budget.committed.empty() && budget.committed.front().committedMs <= cutoff) {
                budget.usedOps -= budget.committed.front().ops;
                budget.committed.pop_front();
            }
            if (budget.usedOps == 0)
                client = state_->clients.erase(client);
            else
                ++client;
        }
    }

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

}  // namespace adi::control
