// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/process.hpp"
#include <atomic>
#include <limits>
#include <memory>
#include <span>
namespace adi::engine {
/// Live, independent relaxed readouts. Silence/unwarmed windows are -infinity.
/// Integrated loudness and true peak hold until reset; windows update at 10 Hz.
struct MainLoudnessReadout {
    std::atomic<float> momentaryLufs{-std::numeric_limits<float>::infinity()};
    std::atomic<float> shortTermLufs{-std::numeric_limits<float>::infinity()};
    std::atomic<float> integratedLufs{-std::numeric_limits<float>::infinity()};
    std::atomic<float> truePeakDb{-std::numeric_limits<float>::infinity()};
};
static_assert(std::atomic<float>::is_always_lock_free);
class MainLoudness {
public:
    MainLoudness();
    ~MainLoudness();
    MainLoudness(const MainLoudness&)=delete;
    MainLoudness& operator=(const MainLoudness&)=delete;
    /// Message thread. Same format retains history; a new rate/layout starts
    /// fresh on the next callback. Allocation/design/reclamation stay here.
    /// Defaults to full-range channels of weight 1 (mono/stereo BS.1770).
    /// Explicit multichannel roles: supply 0 for LFE, 1.41 for surround, 1
    /// for front. No channel roles are guessed from an output channel number.
    bool prepare(double sampleRate,int channels,std::span<const double> weights={});
    void collect();
    /// Audio thread, once after Main's final output; out64 wins when supplied.
    /// Null channels count as silence. No allocation, lock, I/O or audio writes.
    void process(const AudioIo&) noexcept;
    /// Message/UI thread. Consumed on the next callback, including all filters,
    /// windows, integrated gates and peak history. Not an op.
    void requestReset() const noexcept { reset_.store(true,std::memory_order_release); }
    [[nodiscard]] const MainLoudnessReadout& readout() const noexcept { return readout_; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MainLoudnessReadout readout_;
    mutable std::atomic<bool> reset_{false};
};
}
