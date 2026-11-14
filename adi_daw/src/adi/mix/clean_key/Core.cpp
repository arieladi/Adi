// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Core.hpp"
#include "adi/mix/compressor/Core.hpp"
#include "adi/mix/key_eq/Core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>

namespace adi::mix::clean_key {
namespace {
Status status(key_eq::Status value) noexcept {
    switch (value) {
    case key_eq::Status::Ok:
        return Status::Ok;
    case key_eq::Status::NotPrepared:
        return Status::NotPrepared;
    case key_eq::Status::InvalidArgument:
        return Status::InvalidSettings;
    case key_eq::Status::UnsupportedConfiguration:
        return Status::UnsupportedConfiguration;
    case key_eq::Status::AllocationFailed:
        return Status::AllocationFailed;
    case key_eq::Status::InvalidBlock:
        return Status::InvalidBlock;
    }
    return Status::ProcessingFault;
}
Status status(compressor::Status value) noexcept {
    switch (value) {
    case compressor::Status::Ok:
        return Status::Ok;
    case compressor::Status::AllocationFailed:
        return Status::AllocationFailed;
    case compressor::Status::InvalidSpec:
        return Status::InvalidSpec;
    case compressor::Status::InvalidSettings:
        return Status::InvalidSettings;
    case compressor::Status::UnsupportedLayout:
    case compressor::Status::UnsupportedConfiguration:
        return Status::UnsupportedConfiguration;
    case compressor::Status::NotPrepared:
    case compressor::Status::InvalidBlock:
    case compressor::Status::InvalidBuffers:
        return Status::ProcessingFault;
    }
    return Status::ProcessingFault;
}
std::uint64_t prepareIdentity() noexcept {
    // Stopped preparation only. Running callback generations are local.
    static std::atomic<std::uint64_t> serial{1};
    auto result = serial.fetch_add(1, std::memory_order_relaxed);
    if (!result)
        result = serial.fetch_add(1, std::memory_order_relaxed);
    return result;
}
bool validPlanes(const float *left, const float *right, int frames) noexcept {
    if (!left || !right)
        return false;
    const auto l = reinterpret_cast<std::uintptr_t>(left),
               r = reinterpret_cast<std::uintptr_t>(right);
    const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
    const auto limit = std::numeric_limits<std::uintptr_t>::max() - bytes;
    return l % alignof(float) == 0 && r % alignof(float) == 0 && l <= limit && r <= limit &&
           !(l < r + bytes && r < l + bytes);
}
Status validate(const Settings &s) noexcept {
    if (!compressor::validSettings(s.dynamics) || s.movingLane < key_eq::MovingLane::None ||
        s.movingLane > key_eq::MovingLane::Q)
        return Status::InvalidSettings;
    const auto &c = s.dynamics;
    if (c.keySource != compressor::KeySource::Internal || c.stereoLink != 1 ||
        c.linkMode != compressor::LinkMode::Mid || c.autoGain || c.autoRelease ||
        c.holdMs > std::numeric_limits<float>::epsilon() || s.equalizer.audition)
        return Status::UnsupportedConfiguration;
    return Status::Ok;
}
} // namespace
struct Core::Impl {
    compressor::Core clean;
    key_eq::Core key;
    Settings accepted{};
    compressor::Settings childControls{};
    Preparation spec{};
    std::unique_ptr<float[]> scratch;
    compressor::BlockContext cleanContext{};
    key_eq::BlockContext keyContext{};
    std::uint64_t owner = 0, generation = 1, sequence = 0;
    int frames = 0, cursor = 0;
    bool faulted = false;
    Status fault() noexcept {
        faulted = true;
        return Status::ProcessingFault;
    }
    void clear() noexcept {
        key.reset();
        clean.reset();
        if (++generation == 0)
            ++generation;
        sequence = 0;
        frames = cursor = 0;
        faulted = false;
    }
};
Core::Core() noexcept = default;
Core::~Core() = default;
Status Core::prepare(const Preparation &p, const Settings &s) noexcept {
    if (!std::isfinite(p.sampleRate) || p.sampleRate < 44100 || p.sampleRate > 96000 ||
        p.maximumFrames <= 0 || p.maximumFrames > 4096)
        return Status::InvalidSpec;
    if (const auto result = validate(s); result != Status::Ok)
        return result;
    try {
        auto candidate = std::make_unique<Impl>();
        candidate->accepted = s;
        candidate->spec = p;
        candidate->childControls = s.dynamics;
        // This is the already-selected filtered key seam, not a device route.
        candidate->childControls.keySource = compressor::KeySource::External;
        auto result = status(
            candidate->key.prepare({p.sampleRate, p.maximumFrames, 2, s.movingLane}, s.equalizer));
        if (result != Status::Ok)
            return result;
        result = status(
            candidate->clean.prepare({p.sampleRate, p.maximumFrames, 2}, candidate->childControls));
        if (result != Status::Ok)
            return result;
        candidate->scratch =
            std::make_unique<float[]>(static_cast<std::size_t>(p.maximumFrames) * 2);
        candidate->owner = prepareIdentity();
        impl_.swap(candidate);
        return Status::Ok;
    } catch (...) {
        return Status::AllocationFailed;
    }
}
void Core::reset() noexcept {
    if (impl_)
        impl_->clear();
}
bool Core::isPrepared() const noexcept { return static_cast<bool>(impl_); }
const Settings *Core::settings() const noexcept { return impl_ ? &impl_->accepted : nullptr; }
Status Core::target(Target &output) const noexcept {
    if (!impl_)
        return Status::NotPrepared;
    if (impl_->faulted)
        return Status::ProcessingFault;
    float value = 0;
    const auto result = status(impl_->key.target(value));
    if (result != Status::Ok && result != Status::UnsupportedConfiguration)
        return impl_->fault();
    if (result == Status::Ok)
        output = {impl_->accepted.movingLane, value};
    return result;
}
Status Core::beginBlock(int frames, std::optional<Target> targetValue,
                        BlockContext &output) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    auto &s = *impl_;
    if (s.faulted)
        return Status::ProcessingFault;
    if (frames <= 0 || frames > s.spec.maximumFrames || s.cursor != s.frames)
        return Status::InvalidBlock;
    const bool moving = s.accepted.movingLane != key_eq::MovingLane::None;
    if (targetValue && (targetValue->lane < key_eq::MovingLane::FrequencyLog2 ||
                        targetValue->lane > key_eq::MovingLane::Q))
        return Status::InvalidSettings;
    if (targetValue && (!moving || targetValue->lane != s.accepted.movingLane))
        return Status::UnsupportedConfiguration;
    key_eq::BlockContext keyContext;
    if (moving) {
        const key_eq::BlockRequest request{
            frames, targetValue ? std::optional<float>{targetValue->mappedValue} : std::nullopt,
            key_eq::BoundaryAction::None};
        const auto result = status(s.key.beginBlock(request, keyContext));
        if (result != Status::Ok && result != Status::InvalidSettings &&
            result != Status::UnsupportedConfiguration)
            return s.fault();
        if (result != Status::Ok)
            return result;
    }
    // Both children are owned, with matching dimensions and completed cursors.
    // A broken owned-child invariant after a committed key plan is a latched
    // processing fault, never an atomic caller-input refusal.
    const auto cleanContext = s.clean.beginBlock(frames);
    if (!cleanContext.valid())
        return s.fault();
    s.cleanContext = cleanContext;
    s.keyContext = keyContext;
    if (++s.sequence == 0)
        ++s.sequence;
    s.frames = frames;
    s.cursor = 0;
    output = {s.owner, s.generation, s.sequence, frames};
    return Status::Ok;
}
Status Core::process(const BlockContext &context, int offset, int count, float *left,
                     float *right) noexcept {
    if (!impl_)
        return Status::NotPrepared;
    auto &s = *impl_;
    if (s.faulted)
        return Status::ProcessingFault;
    if (context.owner != s.owner || context.generation != s.generation ||
        context.sequence != s.sequence || context.frames != s.frames || offset != s.cursor ||
        offset < 0 || count <= 0 || count > s.frames - offset)
        return Status::InvalidBlock;
    if (!validPlanes(left, right, s.frames))
        return Status::InvalidBuffers;
    for (int i = offset; i < offset + count; ++i)
        if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
            return Status::InvalidBuffers;
    auto *keyLeft = s.scratch.get();
    auto *keyRight = keyLeft + s.spec.maximumFrames;
    std::copy_n(left + offset, count, keyLeft + offset);
    std::copy_n(right + offset, count, keyRight + offset);
    const auto filtered = s.accepted.movingLane == key_eq::MovingLane::None
                              ? s.key.process(keyLeft + offset, keyRight + offset, count)
                              : s.key.process(s.keyContext, offset, count, keyLeft, keyRight);
    if (filtered != key_eq::Status::Ok)
        return s.fault();
    // Finite programme can overflow the copied key's float signal boundaries.
    // Clean requires finite key input; its preflight does not scan samples.
    // Detect that condition before Clean writes the current programme span.
    // Key histories may have advanced, so retry is unsafe until stopped reset.
    for (int i = offset; i < offset + count; ++i)
        if (!std::isfinite(keyLeft[i]) || !std::isfinite(keyRight[i]))
            return s.fault();
    const std::array<float *, 2> main{left, right};
    const std::array<const float *, 2> key{keyLeft, keyRight};
    if (s.clean.process(s.cleanContext, offset, count, main.data(), key.data(), 2,
                        s.childControls) != compressor::Status::Ok)
        return s.fault();
    s.cursor += count;
    return Status::Ok;
}
} // namespace adi::mix::clean_key
