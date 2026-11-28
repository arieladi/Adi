// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/engine/clip_update_rate.hpp"

#include <array>
#include <atomic>
#include <span>
#include <vector>

namespace adi::engine {
struct LaunchSection;

/// A prepared linear multiplier on the current PCM, which already includes
/// authored clip gain. The current clip, run and launch generation are the
/// literal identities captured by the owner. A later run starts from its
/// prepared baseline. Rate comes from the Clip Update Rate preference.
struct LaunchClipGainEdit {
    std::int64_t track = 0, clip = 0;
    std::uint64_t incarnation = 0, run = 0;
    float multiplier = 1.0f;
    ClipUpdateRate rate = ClipUpdateRate::Sixteenth;
};

struct LaunchClipGainRange {
    std::int32_t beforeFrames = 0;
    float before = 1.0f, after = 1.0f;
};

/// One owner publishes gain edits and one driver applies them. The state
/// survives compatible graph rebuilds; a new incarnation uses its own
/// prepared gain. Workers receive the resulting immutable range gains.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4324) // mailbox counters occupy separate cache lines
#endif
class LaunchClipGainEdits {
public:
    explicit LaunchClipGainEdits(std::size_t maximumTracks);

    [[nodiscard]] bool post(const LaunchClipGainEdit&, std::uint64_t pcmRevision) noexcept;
    // Returns the actual PCM revision for an independently retained run.
    using RetainedRevision = std::uint64_t (*)(const void*,std::int64_t,std::int64_t,std::uint64_t) noexcept;
    void begin(const LaunchBlock&, std::span<const std::int64_t> tracks, std::uint64_t pcmRevision,
               const void* retainedContext = nullptr, RetainedRevision retainedRevision = nullptr) noexcept;
    [[nodiscard]] LaunchClipGainRange range(const LaunchBlock&, const LaunchSection&) noexcept;
    [[nodiscard]] float currentMultiplier(std::int64_t track, std::uint64_t run) noexcept;
    void retargetRun(std::int64_t track, std::int64_t previousClip, std::int64_t clip, std::uint64_t run) noexcept;

private:
    struct Pending {
        LaunchClipGainEdit edit;
        std::optional<ClipUpdateBoundary> boundary;
    };
    struct Track {
        std::int64_t id = 0, clip = 0;
        std::uint64_t run = 0;
        float multiplier = 1.0f;
        std::optional<Pending> pending;
    };
    struct Queued {
        LaunchClipGainEdit edit;
        std::uint64_t pcmRevision = 0;
    };
    [[nodiscard]] Track* findTrack(std::int64_t id) noexcept;

    static constexpr std::size_t capacity = 256;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::array<Queued, capacity> mailbox_{};
    alignas(64) std::atomic<std::uint64_t> written_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::vector<Track> tracks_, scratch_;
    Track* active_ = nullptr;
    Track* spare_ = nullptr;
    std::size_t count_ = 0;
    std::uint64_t incarnation_ = 0;
    std::uint64_t pcmRevision_ = 0;
    std::int64_t nextSample_ = -1;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace adi::engine
