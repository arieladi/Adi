// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Settings.hpp"
#include <cstdint>
#include <memory>

namespace adi::mix::deesser {
enum class Status {
    Ok,
    NotPrepared,
    InvalidSpec,
    InvalidSettings,
    UnsupportedLayout,
    UnsupportedConfiguration,
    InvalidBlock,
    InvalidBuffers,
    AllocationFailed
};
struct PrepareSpec {
    double sampleRate = 48000;
    std::int32_t maximumFrames = 512;
    std::int32_t mainChannels = 2;
};
struct BlockContext {
    std::uint64_t epoch = 0, sequence = 0;
    std::int32_t frames = 0;
    [[nodiscard]] bool valid() const noexcept { return epoch != 0 && sequence != 0 && frames > 0; }
};

// Serialized audio owner. Preparation/reset/destruction require stopped audio.
// Float signal boundaries, double filter/envelope history; double I/O narrows to
// float and widens its output. It is not a double-precision signal engine.
// No callback allocation/free, lock, SDK, UI, worker or coefficient construction.
class Core final {
  public:
    Core() noexcept;
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;
    Core(Core &&) = delete;
    Core &operator=(Core &&) = delete;

    // Fresh stream; failure preserves the entire previous stream. Stereo Wide,
    // base rate, 44.1..96 kHz only. Settings are fixed until fresh preparation.
    // Threshold accepts finite [-180,0] dB and Range [0,24] dB. SingleVocal link
    // >100% is unsupported. Accepted rates form a range; tested rates are
    // 44.1/48/96 kHz. Fractional-rate reference equality is not established.
    [[nodiscard]] Status prepare(const PrepareSpec &, const Settings & = {}) noexcept;
    // Same float-rounded rate/layout/settings only; capacity may change. Clears
    // histories but preserves the two Vocal classifier Schmitt states/channel.
    // Failure preserves the old stream. Use prepare for a genuinely fresh stream.
    [[nodiscard]] Status reprepare(const PrepareSpec &) noexcept;
    // Clears filter/hold/envelope history, retaining classifier Schmitt states.
    void reset() noexcept;
    // Identical settings restore retains all DSP history; changes are refused.
    [[nodiscard]] Status restore(const Settings &) noexcept;
    [[nodiscard]] bool isPrepared() const noexcept;
    [[nodiscard]] std::int32_t latencySamples() const noexcept { return 0; }
    [[nodiscard]] BlockContext beginBlock(std::int32_t frames) noexcept;

    // Pointers address the physical block start. Segments must be contiguous and
    // complete it before another beginBlock. Foreign/stale contexts are refused.
    // Main has two distinct complete planes. Explicit key has zero or two planes;
    // zero means temporary silence on the same External route. A mono key or
    // channel/source migration requires a different future capability, not an
    // inferred pointer convention. Internal key follows input gain/pan; External
    // key does not. Exact/crossed key-main aliases work; partial overlap refuses
    // the whole segment before writes or DSP/context advancement.
    // Samples must be finite and within float's finite range (ordinary doubles
    // round on entry). There is no extra full-buffer sample-validation scan.
    // Any changed setting is refused, including within a physical callback.
    [[nodiscard]] Status process(const BlockContext &, std::int32_t offset, std::int32_t frames,
                                 float *const *main, const float *const *key, std::int32_t keyChannels,
                                 const Settings &) noexcept;
    [[nodiscard]] Status process(const BlockContext &, std::int32_t offset, std::int32_t frames,
                                 double *const *main, const double *const *key, std::int32_t keyChannels,
                                 const Settings &) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::mix::deesser
