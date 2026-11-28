// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/process.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
namespace adi::engine {

enum class MainLoudnessField : std::uint32_t {
    Momentary = 1U << 0, ShortTerm = 1U << 1, Integrated = 1U << 2, TruePeak = 1U << 3,
    Range = 1U << 4, MaxMomentary = 1U << 5, MaxShortTerm = 1U << 6,
};

struct MainLoudnessSnapshot {
    static constexpr float silence = -std::numeric_limits<float>::infinity();
    float momentaryLufs = silence;
    float shortTermLufs = silence;
    float integratedLufs = silence;
    float truePeakDb = silence;
    float rangeLu = 0.0f;
    float maxMomentaryLufs = silence;
    float maxShortTermLufs = silence;
    std::uint64_t formatSequence = 0;
    std::uint32_t validFields = 0;
    bool running = true;

    [[nodiscard]] bool valid(MainLoudnessField field) const noexcept {
        return (validFields & static_cast<std::uint32_t>(field)) != 0;
    }
};

/// Independent numeric readouts for existing clients and an LD7 snapshot.
struct MainLoudnessReadout {
    std::atomic<float> momentaryLufs{-std::numeric_limits<float>::infinity()};
    std::atomic<float> shortTermLufs{-std::numeric_limits<float>::infinity()};
    std::atomic<float> integratedLufs{-std::numeric_limits<float>::infinity()};
    std::atomic<float> truePeakDb{-std::numeric_limits<float>::infinity()};
    // Audio-published format identity. Zero is unprepared or in transition.
    std::atomic<std::uint64_t> formatSequence{0};
    [[nodiscard]] std::optional<MainLoudnessSnapshot> snapshot() const noexcept;

private:
    friend class MainLoudness;
    void publish(const MainLoudnessSnapshot&) noexcept;
    std::atomic<std::uint64_t> publication_{0};
    std::atomic<float> publishedPeakDb_{MainLoudnessSnapshot::silence};
    std::atomic<float> rangeLu_{0.0f};
    std::atomic<float> maxMomentaryLufs_{MainLoudnessSnapshot::silence};
    std::atomic<float> maxShortTermLufs_{MainLoudnessSnapshot::silence};
    std::atomic<std::uint32_t> validFields_{0};
    std::atomic<bool> running_{true};
};
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
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
    void setRunning(bool running) const noexcept { running_.store(running, std::memory_order_release); }
    [[nodiscard]] const MainLoudnessReadout& readout() const noexcept { return readout_; }
    /// Main only, serialized with prepare/collect/destruction. A view compares
    /// this identity with its snapshot's formatSequence to reject a pending
    /// or superseded format without borrowing the measurement implementation.
    [[nodiscard]] std::uint64_t preparedFormatSequence() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MainLoudnessReadout readout_;
    mutable std::atomic<bool> reset_{false};
    mutable std::atomic<bool> running_{true};
};
}
