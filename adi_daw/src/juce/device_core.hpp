// SPDX-License-Identifier: GPL-3.0-or-later
//
// Driving a BlockProcessor from an audio device — the half that is not JUCE.
//
// This file lives under `src/juce/` because devices are that lane's
// responsibility, and it includes **no JUCE at all**. That is deliberate and it
// is the same trick ADR-0036 used for the snapshot handoff: the interesting
// logic — what gets prepared with which size, what happens when the block size
// changes mid-session, what the stream clock does across a device restart — has
// nothing to do with JUCE, and a test that needs a sound card is a test that
// does not run. CI has no sound card.
//
// So `DeviceCore` is tested on every ABI in the main suite, and `DeviceBridge`
// (which does include JUCE) stays thin enough that "it compiles and a callback
// arrives" is the whole of what the JUCE job has to prove.
//
// ADR-0049 is the rule this type exists to make structural: **the granted size
// is the only size that exists.** Asking CoreAudio for 8192 returns 4096 and
// says nothing, so every allocation here is sized from what the driver returned
// and never from what was asked for.

#pragma once

#include "adi/engine/process.hpp"

#include <cstdint>

namespace adi::device {

/// What happened when a device was opened or reconfigured.
///
/// `requested` is kept alongside `granted` so that a mismatch can be *reported*
/// rather than silently corrected. Running at a quarter of the requested size
/// while the preference still reads 4096 is how somebody spends an afternoon on
/// a dropout with an obvious cause.
struct OpenResult {
    bool          opened     = false;
    double        sampleRate = 0.0;
    std::int32_t  requested  = 0;   ///< what the user asked for
    std::int32_t  granted    = 0;   ///< what the driver actually gave
    const char*   error      = nullptr;

    [[nodiscard]] bool sizeMismatch() const {
        return opened && requested > 0 && granted != requested;
    }
};

/// Drives one `engine::BlockProcessor` from whatever a device hands over.
///
/// Thread discipline, which is the whole point of the type:
///   * `open`, `changeBlockSize`, `close`, `setProcessor` — message thread.
///   * `process` — audio thread, `noexcept`, no allocation, no locks.
///
/// Not copyable: it holds a processor reference and a stream clock, and a
/// second one of either is a bug rather than a use case.
class DeviceCore {
public:
    DeviceCore() = default;
    DeviceCore(const DeviceCore&) = delete;
    DeviceCore& operator=(const DeviceCore&) = delete;

    /// Prepare `proc` for a stream. `granted` is what the driver returned;
    /// `requested` is recorded only so the caller can see they differ.
    ///
    /// A granted size of zero or less is refused rather than prepared: a
    /// processor prepared for no frames would be asked to fill a buffer it was
    /// never sized for.
    OpenResult open(engine::BlockProcessor& proc, double sampleRate,
                    std::int32_t requested, std::int32_t granted);

    /// ADR-0042 decision 5: change block size **without reloading the project**.
    ///
    /// At 4096 the monitoring round trip makes overdubbing impossible, so anyone
    /// mixing dense playback with recording moves between sizes during a
    /// session. Re-prepares the *same* processor — the project, its graph and
    /// its undo history are untouched, because none of them live here.
    ///
    /// The stream clock is CONTINUOUS across the change. A device restart makes
    /// the driver's own frame counter begin again at zero, and a transport that
    /// followed it would jump backwards mid-session; the accumulated offset is
    /// what stops that, and it is the part most likely to be got wrong.
    OpenResult changeBlockSize(double sampleRate, std::int32_t requested,
                               std::int32_t granted);

    /// Release the processor. Safe to call when nothing was ever opened —
    /// `process.hpp` says `release` may be called without a preceding
    /// `prepare`, and a device that fails to open takes exactly that path.
    void close();

    /// Swap in a different processor while the stream keeps running.
    ///
    /// This is how `SilenceProcessor` gets installed while a project loads or
    /// after a graph fails to build. The new processor must already be prepared
    /// for the current granted size — this does not prepare it, because
    /// preparing allocates and the caller is on the message thread where that
    /// is allowed.
    ///
    /// Returns false, and changes nothing, if there is no open stream to swap
    /// within.
    bool setProcessor(engine::BlockProcessor& next);

    /// AUDIO THREAD. Fills `out` by driving the processor.
    ///
    /// `frames` may be anything from 1 to the granted size, and routinely is:
    /// a driver may hand over fewer than the maximum. **Frames beyond the
    /// prepared size are refused** — the processor was sized for `granted_` and
    /// writing past it is the overrun ADR-0049 exists to prevent. The refusal
    /// outputs silence and counts, because a callback that returns without
    /// touching the buffer hands the driver's uninitialised memory to somebody's
    /// monitors.
    void process(float* const* out, std::int32_t numOut,
                 const float* const* in, std::int32_t numIn,
                 std::int32_t frames) noexcept;

    // --- observable state, so tests assert on what happened ----------------

    [[nodiscard]] bool          isOpen()      const { return open_; }
    [[nodiscard]] double        sampleRate()  const { return sampleRate_; }
    [[nodiscard]] std::int32_t  granted()     const { return granted_; }
    [[nodiscard]] std::int32_t  requested()   const { return requested_; }
    [[nodiscard]] std::int64_t  streamTime()  const { return streamTime_; }
    [[nodiscard]] std::int64_t  callbacks()   const { return callbacks_; }

    /// Callbacks refused because `frames` exceeded the prepared size. Non-zero
    /// here means a driver changed its block size without telling us, which is
    /// a reportable fault rather than something to absorb quietly.
    [[nodiscard]] std::int64_t  oversizeRefusals() const { return oversize_; }

    /// How many times the block size has been changed on a live stream.
    [[nodiscard]] int           reconfigurations() const { return reconfigs_; }

private:
    engine::BlockProcessor* proc_ = nullptr;
    bool         open_       = false;
    double       sampleRate_ = 0.0;
    std::int32_t granted_    = 0;
    std::int32_t requested_  = 0;
    std::int64_t streamTime_ = 0;   ///< continuous across reconfiguration
    std::int64_t callbacks_  = 0;
    std::int64_t oversize_   = 0;
    int          reconfigs_  = 0;
};

}  // namespace adi::device
