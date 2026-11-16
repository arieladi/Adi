// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace adi::mix::eq_chain {
struct PrepareSpec {
    double sampleRate = 48000.;
    std::int32_t maximumFrames = 512, channels = 2;
};
struct PlanOffer {
    std::uint64_t token = 0, eqToken = 0, focusReadyToken = 0, focusEpoch = 0;
    std::int32_t totalLatency = 0, eqLatency = 0, focusLatency = 0;
    bool eqEnabled = false, carveEnabled = false, focusEnabled = false;
};
struct BlockContext {
    std::uint64_t epoch = 0, generation = 0, block = 0;
};
struct Statistics {
    std::uint64_t eqFrames = 0, carveFrames = 0, focusFrames = 0, keyFrames = 0, eqPrimingFrames = 0;
    std::uint64_t focusPreparations = 0, eqResets = 0;
    bool focusPrepared = false, keyPrepared = false;
};
// Framework-free EQ -> Carve -> Focus. One serialized audio/configuration
// publisher, one owner service thread. No owner work is invoked by audio calls.
// prepare/reset/destruction require stopped processing. An offer is immutable
// until a new physical callback admits its complete latency/enables snapshot.
class Core final {
  public:
    Core();
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    void prepare(PrepareSpec, const Settings &);
    void reset(const Settings &);
    // Stopped owner only, including stopped service and metadata readers. Uses
    // the same EQ object and its authored prepare law, retaining learned
    // histograms unless that law resets them for a learn/relative transition.
    // Other stages, contexts, key history and work versions start a fresh stream.
    // Validation/resource staging failure leaves the old chain intact. If the
    // final retained-EQ prepare throws, the chain fails closed: isPrepared()==
    // false, zero latency/no offer, audio rejects; retry this method or prepare.
    void reprepareStopped(PrepareSpec, const Settings &);
    [[nodiscard]] bool isPrepared() const noexcept;
    // False rejects invalid settings or an unsupported enabled stage layout/rate.
    // Later requests coalesce behind the one immutable candidate being serviced.
    bool requestConfiguration(const Settings &) noexcept;
    bool serviceConfiguration();
    // Lock-free metadata for a third Main/UI observer. Read these versions to
    // signal an owner pump; never call pendingPlan as a third concurrent actor.
    // Separate loads are not a coherent pair snapshot: requested > completed
    // signals outstanding work, and a later Main tick retries the observation.
    // Versions reset on stopped preparation. Lifecycle replacement/destruction
    // still requires metadata readers to have stopped using this instance.
    [[nodiscard]] std::uint64_t requestedWorkVersion() const noexcept;
    [[nodiscard]] std::uint64_t completedWorkVersion() const noexcept;
    // A candidate is advertised only after owner resources and original-key
    // history are ready. Fresh requested-on EQ can prime into private scratch
    // while the previous audible plan runs. It is counted separately from
    // active EQ processing. The stream must continue while warming history.
    // Called by audio or by the same serialized owner that calls service.
    // An arbitrary concurrent UI reader must use an owner-published UI snapshot.
    // Advertised plans have a non-failing read until their audio acknowledgment.
    [[nodiscard]] PlanOffer pendingPlan() const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept;
    [[nodiscard]] BlockContext beginBlock(std::uint64_t physicalBlock, std::int32_t configuredTotalLatency,
                                          const Settings &) noexcept;
    // One segment is at most maximumFrames. Capture precedes in-place EQ.
    // Key layout is independent of main width. Null stereo planes mean zero;
    // a missing bus has zero channels. A connected logical layout (1 or 2)
    // remains fixed until stopped reset/prepare, including disconnect tails.
    // Changing 1 <-> 2 without reset is rejected before changing any samples.
    // The caller also resets/reprepares on a key-source identity change; raw
    // pointers can vary between ordinary segments and do not identify a route.
    // Float/double calls may not alternate within a prepared stream.
    bool process(float *const *main, const float *const *key, std::int32_t keyChannels, std::int32_t frames,
                 const Settings &, BlockContext) noexcept;
    bool process(double *const *main, const double *const *key, std::int32_t keyChannels, std::int32_t frames,
                 const Settings &, BlockContext) noexcept;
    // Audio-owner diagnostics, or after stopping. Not a concurrent UI snapshot.
    [[nodiscard]] Statistics statistics() const noexcept;
    [[nodiscard]] double learnedThreshold(std::size_t band) const noexcept;
    [[nodiscard]] double learnedKnee(std::size_t band) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::eq_chain
