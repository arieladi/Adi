// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adi::engine {
struct AudioMemorySnapshot {
    std::int64_t inputId=0, firstSample=0;
    std::uint64_t epoch=0;
    std::uint32_t sampleRate=0, channels=0;
    // Interleaved physical-input float samples, bit-preserved, ready for the
    // existing off-callback Float32 WavWriter. No normalization or dither.
    std::vector<float> samples;
    [[nodiscard]] std::size_t frames() const noexcept { return channels?samples.size()/channels:0; }
};

// ADR-0132 d9: one retrospective RAM memory per armed OR monitored input,
// default 30 seconds. Physical driver samples are float; this does not narrow
// an internal double graph bus. Construct/destroy on the owner with producer
// and snapshot calls quiescent. One producer serializes gates and push(); one
// other owner calls snapshot(). Queue UI gate changes to the producer.
//
// push() consumes an explicit nonnegative, monotonic driver stream sample
// clock, independent of the transport. A discontinuity or rejected segment
// begins a new epoch: snapshots contain only the latest contiguous suffix.
// A listening on/off transition clears history. Nothing is retained while neither gate is
// on. Ring storage is preallocated and capped at 256 MiB per instance.
//
// Chunk ownership protects ordinary sample storage from concurrent writes.
// A producer never waits for a snapshot reader: a pinned chunk is dropped and
// counted, and history restarts after that segment. The owner likewise fails
// rather than returning partially overwritten data. It may retry later.
class AudioMemory {
public:
    AudioMemory(std::int64_t inputId,std::uint32_t sampleRate,std::uint32_t channels,
                double seconds=30.0);
    void setListening(bool armed,bool monitored) noexcept;
    [[nodiscard]] bool listening() const noexcept { return listening_.load(std::memory_order_relaxed); }
    // Planar physical input, at most 1,048,576 frames per call. Null channels
    // (including a null channel array) mean digital silence. Zero frames is a
    // no-op; invalid timestamps/counts reject the entire block and clear history.
    // Returns frames copied, which can be less than requested under contention.
    std::uint32_t push(const float* const* input,std::int32_t frames,std::int64_t firstSample) noexcept;
    // Owner only; allocates. On failure output is empty and error explains why.
    // Success is an immutable copy at a published block boundary, at most the
    // configured duration. It need not include a producer block still in flight.
    bool snapshot(AudioMemorySnapshot& output,std::string& error) const;
    [[nodiscard]] std::uint64_t capacityFrames() const noexcept { return capacity_; }
    struct Stats { std::uint64_t copiedFrames=0,droppedFrames=0,ignoredFrames=0,invalidBlocks=0; };
    [[nodiscard]] Stats stats() const noexcept;
private:
    static constexpr std::uint64_t chunkFrames=256;
    enum : std::uint8_t { Vacant, Ready, Writing, Reading };
    struct Slot {
        mutable std::atomic<std::uint8_t> state{Vacant};
        std::uint64_t epoch=0,base=0;
        std::uint32_t begin=0,end=0;
    };
    void reset(std::uint64_t at) noexcept;
    void publish() noexcept;
    const std::int64_t inputId_;
    const std::uint32_t rate_,channels_;
    std::uint64_t capacity_=0,slotCount_=0;
    std::unique_ptr<Slot[]> slots_;
    std::vector<float> samples_;
    std::atomic<bool> listening_{false};
    // Producer-owned metadata, atomically published once per block/gate change.
    std::uint64_t epoch_=1,begin_=0,end_=0;
    bool haveClock_=false;
    std::atomic<std::uint64_t> revision_{0},publishedEpoch_{1},publishedBegin_{0},publishedEnd_{0};
    std::atomic<std::uint64_t> copied_{0},dropped_{0},ignored_{0},invalid_{0};
};
} // namespace adi::engine
