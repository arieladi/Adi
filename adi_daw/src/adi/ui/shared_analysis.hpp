// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/dsp/spectrum.hpp"
#include "adi/engine/scope.hpp"
#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace adi::ui {
// Owner-thread demand, immutable worker publications. No audio processor, callback lock,
// repaint timer or independent analysis clock: visible windows request a bounded pass on
// their frame clock. All documents share one process worker. Compatible output modes use
// one cursor, overlap window, plan and FFT. Source/history callbacks run only on the owner.
class SharedAnalysis final {
    struct State;
    struct Domain;
    struct Scheduler;
  public:
    static constexpr double kTapSeconds = 10.0; // legacy adapter's history contract
    static constexpr std::size_t kMaxDomains = 128;
    static constexpr std::int32_t kMaxFftSize = 65536;
    static constexpr std::size_t kMaxWindowsPerFrame = 16;
    static constexpr std::size_t kMaxSamplesPerFrame = 262144;
    static constexpr std::size_t kMaxReadsPerFrame = 256;
    // Immutable spectrogram-bin payload, including buffers retained by frozen
    // snapshots. This is not a limit on FFT/tap/allocator or client metadata.
    static constexpr std::size_t kMaxHistoryBytes = 64u * 1024u * 1024u;
    enum class Channel : std::uint8_t { Mid, Side, Left, Right, StereoPower };
    enum class Mode { Worker, Manual };
    enum Output : std::uint32_t { Spectrum = 1, Scope = 2, Meters = 4, Phase = 8, Spectrogram = 16 };
    struct Request {
        std::int64_t track = 0;
        engine::ScopePoint point = engine::ScopePoint::PostFader;
        std::int32_t fftSize = 1024;
        Channel channel = Channel::Mid;
        dsp::Window window = dsp::Window::Hann;
        std::int64_t device = 0;
        std::int32_t hopSize = 0; // zero resolves to N/4
        std::uint32_t outputs = Output::Spectrum;
        auto operator<=>(const Request &) const = default;
        engine::ScopeSource source() const noexcept { return {track, point, device}; }
    };
    struct Column {
        std::uint64_t serial = 0;
        std::int64_t firstStamp = 0;
        std::shared_ptr<const std::vector<float>> binsDbfs;
    };
    struct Result {
        Request request;
        std::uint64_t revision = 0, sourceEpoch = 0, generation = 0, windowSerial = 0;
        std::int64_t firstStamp = 0, firstSequence = 0;
        double sampleRate = 0, coherentGain = 0;
        std::int32_t hopSize = 0;
        std::uint32_t outputs = 0;
        // Raw per-channel sample peak/RMS, not a true-peak or loudness meter. Spectrum uses
        // dsp::Spectrum's coherent sine calibration; Scope/Phase keep unwindowed samples.
        std::array<float, 2> peak{}, rms{};
        double correlation = 0;
        std::vector<float> binsDbfs, scopeLeft, scopeRight;
        // Bounded newest columns. Serial and sourceEpoch expose missed or reset history.
        std::vector<Column> columns;
        std::uint64_t gapCount = 0, overrunCount = 0;
        std::uint64_t historySerial = 0, droppedHistoryColumns = 0;
        std::size_t nonFiniteSamples = 0;
        std::int64_t pendingFrames = 0;
        bool advancing = false, discontinuity = false, budgetLimited = false, historyLimited = false;
    };
    struct Stats {
        std::size_t consumers = 0, domains = 0, taps = 0;
        std::uint64_t transforms = 0, publications = 0, frameRequests = 0, workerPasses = 0;
        std::uint64_t cursorReads = 0, analysisWindows = 0;
        std::size_t spectralPlans = 0, historyBytes = 0, historyPeakBytes = 0;
        std::uint64_t droppedHistoryColumns = 0;
    };
    class Lease {
      public:
        Lease() = default;
        ~Lease();
        Lease(Lease &&) noexcept;
        Lease &operator=(Lease &&) noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        void reset() noexcept;
        explicit operator bool() const noexcept;
        bool available() const noexcept;
        std::shared_ptr<const Result> read() const noexcept;
      private:
        Lease(std::weak_ptr<State>, std::shared_ptr<Domain>, std::uint32_t);
        std::weak_ptr<State> state_;
        std::shared_ptr<Domain> domain_;
        std::uint32_t outputs_ = 0;
        friend class SharedAnalysis;
    };
    using AcquireSourceTap = std::function<std::shared_ptr<const engine::ScopeTap>(engine::ScopeSource, double)>;
    // Acquire can establish a pending lease whose tap is currently null. Last demand calls
    // Release once whenever Acquire was invoked, so unavailable sources cannot leak demand.
    using ReleaseSourceTap = std::function<void(engine::ScopeSource)>;
    using AcquireTap = std::function<std::shared_ptr<const engine::ScopeTap>(std::int64_t, engine::ScopePoint)>;
    using ReleaseTap = std::function<void(std::int64_t, engine::ScopePoint)>;
    SharedAnalysis(AcquireSourceTap, ReleaseSourceTap, Mode = Mode::Worker,
                   std::size_t historyByteLimit = kMaxHistoryBytes);
    SharedAnalysis(AcquireTap, ReleaseTap, Mode = Mode::Worker,
                   std::size_t historyByteLimit = kMaxHistoryBytes);
    ~SharedAnalysis();
    SharedAnalysis(const SharedAnalysis &) = delete;
    SharedAnalysis &operator=(const SharedAnalysis &) = delete;
    Lease acquire(const Request &);
    void shutdown() noexcept;
    void refreshSources(bool forceEpoch = false);
    // Owner/window-frame driven and coalesced. Manual brokers deliberately do not schedule.
    void requestFrame() noexcept;
    // Explicit deterministic bounded pass for headless/component tests. Counts actual FFTs.
    std::size_t pump();
    Stats stats() const;
  private:
    std::shared_ptr<State> state_;
    Mode mode_;
};
} // namespace adi::ui
