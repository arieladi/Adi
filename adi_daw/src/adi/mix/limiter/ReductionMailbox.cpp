// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ReductionMailbox.h"
#include <algorithm>
#include <bit>
#include <utility>

namespace adi::mix::limiter {
namespace {
// Two flags and a bounded observed-frame count share one published word.
constexpr auto framesMask = std::uint64_t{0x3fffffff};
constexpr auto availableBit = std::uint64_t{1} << 62;
constexpr auto advancingBit = std::uint64_t{1} << 63;
}

ReductionMailbox::Lease::Lease(std::shared_ptr<ReductionMailbox> source, std::uint64_t epoch) noexcept
    : source_(std::move(source)), epoch_(epoch) {}
ReductionMailbox::Lease::~Lease() { reset(); }
ReductionMailbox::Lease::Lease(Lease&& other) noexcept
    : source_(std::move(other.source_)), epoch_(std::exchange(other.epoch_, 0)) {}
ReductionMailbox::Lease& ReductionMailbox::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        reset();
        source_ = std::move(other.source_);
        epoch_ = std::exchange(other.epoch_, 0);
    }
    return *this;
}
ReductionMailbox::Lease::operator bool() const noexcept {
    return source_ && source_->observationEpoch() == epoch_;
}
std::optional<ReductionFrame> ReductionMailbox::Lease::read() const noexcept {
    return source_ ? source_->read(epoch_) : std::nullopt;
}
void ReductionMailbox::Lease::reset() noexcept {
    if (source_) source_->unobserve(epoch_);
    source_.reset();
    epoch_ = 0;
}

void ReductionMailbox::revokeObservers() noexcept {
    demand_.store(0, std::memory_order_relaxed);
    observers_ = 0;
}
void ReductionMailbox::setPrepared(bool prepared) noexcept {
    revokeObservers();
    prepared_ = prepared;
}
ReductionMailbox::Lease ReductionMailbox::observe(std::shared_ptr<ReductionMailbox> source) noexcept {
    if (!source || !source->prepared_) return {};
    if (source->observers_++ == 0) {
        if (++source->nextEpoch_ == 0) ++source->nextEpoch_;
        source->demand_.store(source->nextEpoch_, std::memory_order_relaxed);
    }
    const auto epoch = source->nextEpoch_;
    return Lease(std::move(source), epoch);
}
void ReductionMailbox::unobserve(std::uint64_t epoch) noexcept {
    if (epoch != 0 && epoch == observationEpoch() && --observers_ == 0) revokeObservers();
}
void ReductionMailbox::publishSegment(std::uint64_t epoch, const ReductionFrame& segment) noexcept {
    if (epoch == 0 || epoch != observationEpoch() || segment.observedFrames == 0) return;
    if (activeEpoch_ != epoch || segment.renderBlock == 0 || aggregate_.renderBlock != segment.renderBlock ||
        aggregate_.transportRevision != segment.transportRevision || aggregate_.advancing != segment.advancing) {
        activeEpoch_ = epoch;
        aggregate_ = segment;
    } else {
        const auto sum = static_cast<std::uint64_t>(aggregate_.observedFrames) + segment.observedFrames;
        aggregate_.observedFrames = static_cast<std::uint32_t>(std::min(sum, framesMask));
        aggregate_.reduction.available &= segment.reduction.available;
        aggregate_.reduction.peakLoss =
            std::max(aggregate_.reduction.peakLoss, segment.reduction.peakLoss);
    }
    const auto loss = aggregate_.reduction.available ? aggregate_.reduction.peakLoss : 0.f;
    const auto value = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(loss)) |
        (std::min(static_cast<std::uint64_t>(aggregate_.observedFrames), framesMask) << 32) |
        (aggregate_.reduction.available ? availableBit : 0) | (aggregate_.advancing ? advancingBit : 0);

    version_.store(++publishVersion_, std::memory_order_relaxed);
    // An observer that sees any new field must also see the preceding odd
    // version or a later version at its final check.
    std::atomic_thread_fence(std::memory_order_release);
    publishedEpoch_.store(epoch, std::memory_order_relaxed);
    block_.store(aggregate_.renderBlock, std::memory_order_relaxed);
    revision_.store(aggregate_.transportRevision, std::memory_order_relaxed);
    firstTimelineSample_.store(aggregate_.firstTimelineSample, std::memory_order_relaxed);
    payload_.store(value, std::memory_order_relaxed);
    version_.store(++publishVersion_, std::memory_order_release);
}
std::optional<ReductionFrame> ReductionMailbox::read(std::uint64_t epoch) const noexcept {
    if (epoch == 0 || epoch != observationEpoch()) return std::nullopt;
    const auto version = version_.load(std::memory_order_acquire);
    if ((version & 1u) || publishedEpoch_.load(std::memory_order_relaxed) != epoch) return std::nullopt;

    ReductionFrame frame;
    frame.renderBlock = block_.load(std::memory_order_relaxed);
    frame.transportRevision = revision_.load(std::memory_order_relaxed);
    frame.firstTimelineSample = firstTimelineSample_.load(std::memory_order_relaxed);
    const auto value = payload_.load(std::memory_order_relaxed);
    frame.observedFrames = static_cast<std::uint32_t>((value >> 32) & framesMask);
    frame.advancing = (value & advancingBit) != 0;
    frame.reduction.available = (value & availableBit) != 0;
    frame.reduction.peakLoss = std::bit_cast<float>(static_cast<std::uint32_t>(value));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (version != version_.load(std::memory_order_relaxed)) return std::nullopt;
    return frame;
}

} // namespace adi::mix::limiter
