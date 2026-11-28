// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace adi::backup {

class Schedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto interval = std::chrono::minutes(10);

    struct Revision {
        std::int64_t changes = 0;
        std::int64_t dataVersion = 0;
        bool operator==(const Revision &) const = default;
    };

    void arm(Clock::time_point now, bool enabled) noexcept {
        lastBackup_.reset();
        if (enabled)
            next_ = now;
        else
            next_.reset();
    }

    [[nodiscard]] bool due(Clock::time_point now) const noexcept {
        return next_ && now >= *next_;
    }

    [[nodiscard]] bool claim(Clock::time_point now, Revision revision) noexcept {
        if (!due(now))
            return false;
        next_ = now + interval;
        return lastBackup_ != revision;
    }

    void recordSuccess(Revision revision) noexcept {
        lastBackup_ = revision;
    }

private:
    std::optional<Clock::time_point> next_;
    std::optional<Revision> lastBackup_;
};

} // namespace adi::backup
