// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gesture_session.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

namespace adi::gesture {
struct QuietPoll {
    Status status;
    std::optional<CommitOutcome> ended;
};
/// Owner-side end detector for one source without a release event. Adopt a fresh
/// common Session and an explicit nonzero quiet interval. Call update/poll with
/// monotonic milliseconds in one source clock; only accepted values renew it.
/// At/after the interval, poll submits the ordinary End exactly once. A late
/// value refuses Busy until that End, then the caller begins a fresh gesture.
/// No background timer, op merging, Undo/recommit or implicit automation write.
class QuietSession final {
  public:
    QuietSession(std::unique_ptr<Session>, std::uint64_t quietMilliseconds);
    ~QuietSession();
    QuietSession(const QuietSession&) = delete;
    QuietSession& operator=(const QuietSession&) = delete;
    UpdateResult update(std::unique_ptr<const Input>, std::uint64_t nowMilliseconds);
    QuietPoll poll(std::uint64_t nowMilliseconds);
    CommitOutcome end();
    CancelOutcome cancel();
    [[nodiscard]] State state() const noexcept;

  private:
    Status clock(std::uint64_t) const;
    bool due(std::uint64_t) const noexcept;
    std::unique_ptr<Session> session_;
    const std::uint64_t quiet_;
    const std::thread::id thread_;
    std::optional<std::uint64_t> seen_, accepted_;
};
} // namespace adi::gesture
