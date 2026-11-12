// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/quiet_gesture.hpp"
#include "adi/audio/io_audit.hpp"
#include <stdexcept>
#include <utility>
namespace adi::gesture {
QuietSession::QuietSession(std::unique_ptr<Session> session, std::uint64_t quietMilliseconds)
    : session_(std::move(session)), quiet_(quietMilliseconds), thread_(std::this_thread::get_id()) {
    if (!session_ || !quiet_ || session_->state() != State::Open)
        throw std::invalid_argument(
            "quiet end detector needs a fresh Session and positive interval");
}
QuietSession::~QuietSession() = default;
Status QuietSession::clock(std::uint64_t now) const {
    if (audio::onAudioThread)
        return {Error::WrongThread, {}};
    if (thread_ != std::this_thread::get_id())
        return {Error::WrongThread, "quiet end detector requires its owner thread"};
    if (seen_ && now < *seen_)
        return {Error::InvalidInput, "quiet end clock moved backwards"};
    return {};
}
bool QuietSession::due(std::uint64_t now) const noexcept {
    // Ordered unsigned subtraction avoids overflowing an absolute deadline.
    return accepted_ && now >= *accepted_ && now - *accepted_ >= quiet_;
}
UpdateResult QuietSession::update(std::unique_ptr<const Input> input, std::uint64_t now) {
    auto admitted = clock(now);
    if (!admitted.ok())
        return {std::move(admitted), {}};
    seen_ = now;
    if (session_->state() == State::Committed || session_->state() == State::Cancelled)
        return {{Error::Closed, "quiet gesture has ended"}, {}};
    if (session_->state() == State::SourceLost)
        return {{Error::SourceLost, "quiet gesture source is lost"}, {}};
    if (due(now))
        return {{Error::Busy, "quiet interval elapsed; poll End before a fresh gesture"}, {}};
    auto result = session_->update(std::move(input));
    if (result.status.ok())
        accepted_ = now;
    return result;
}
QuietPoll QuietSession::poll(std::uint64_t now) {
    auto admitted = clock(now);
    if (!admitted.ok())
        return {std::move(admitted), {}};
    seen_ = now;
    if (session_->state() == State::SourceLost)
        return {{Error::SourceLost, "quiet gesture source is lost"}, {}};
    if (session_->state() == State::Committed || session_->state() == State::Cancelled || !due(now))
        return {};
    auto ended = session_->commit();
    auto status = ended.status;
    return {std::move(status), std::move(ended)};
}
CommitOutcome QuietSession::end() { return session_->commit(); }
CancelOutcome QuietSession::cancel() { return session_->cancel(); }
State QuietSession::state() const noexcept { return session_->state(); }
} // namespace adi::gesture
