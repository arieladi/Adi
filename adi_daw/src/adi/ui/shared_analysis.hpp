// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/dsp/spectrum.hpp"
#include "adi/dsp/stereo_envelope_meter.hpp"
#include "adi/dsp/triggered_scope.hpp"
#include "adi/engine/scope.hpp"
#include <array>
#include <compare>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace adi::ui {
struct FocusScopeRange;
class FocusScopeBarProjection;
struct FocusLagRequest;
struct FocusLagPreview;
// Owner-thread demand, immutable worker publications. No audio processor, callback lock,
// repaint timer or independent analysis clock: visible windows request a bounded pass on
// their frame clock. All documents share one process worker. Compatible output modes use
// one cursor, overlap window, plan and FFT. Source/history callbacks run only on the owner.
class SharedAnalysis final {
    struct State;
    struct Domain;
    struct CumulativeHistory;
    struct CaptureDomain;
    struct BarScopeDomain;
    struct FocusLagJob;
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
    enum Output : std::uint32_t {
        Spectrum = 1, Scope = 2, Meters = 4, Phase = 8, Spectrogram = 16, Average1s = 32,
        RealtimeMaximum = 64, CumulativeMaximum = 128
    };
    struct StereoMeterProfile {
        bool midSide = false;
        dsp::StereoEnvelopeMeter::Weighting weighting = dsp::StereoEnvelopeMeter::Weighting::Standard;
        bool linkedDynamic = false;
        auto operator<=>(const StereoMeterProfile &) const = default;
    };
    struct Request {
        std::int64_t track = 0;
        engine::ScopePoint point = engine::ScopePoint::PostFader;
        std::int32_t fftSize = 1024;
        Channel channel = Channel::Mid;
        dsp::Window window = dsp::Window::Hann;
        std::int64_t device = 0;
        std::int32_t hopSize = 0; // zero resolves to N/4
        std::uint32_t outputs = Output::Spectrum;
        // A separate contiguous measurement incompatible with a windowed analysis domain.
        // Meter profiles on the same source still share the physical tap and worker.
        std::optional<StereoMeterProfile> stereoMeter = std::nullopt;
        auto operator<=>(const Request &) const = default;
        engine::ScopeSource source() const noexcept { return {track, point, device}; }
    };
    struct Column {
        std::uint64_t serial = 0;
        std::int64_t firstStamp = 0;
        std::shared_ptr<const std::vector<float>> binsDbfs;
        std::int64_t firstSequence = 0;
    };
    struct Result {
        Request request;
        std::uint64_t revision = 0, sourceEpoch = 0, generation = 0, windowSerial = 0;
        // The source transport epoch, shared by taps. Domain epochs are local to one source.
        std::uint64_t transportRevision = 0;
        std::int64_t firstStamp = 0, firstSequence = 0;
        double sampleRate = 0, coherentGain = 0;
        std::int32_t hopSize = 0;
        std::uint32_t outputs = 0;
        // Raw per-channel sample peak/RMS, not a true-peak or loudness meter. Spectrum uses
        // dsp::Spectrum's coherent sine calibration; Scope/Phase keep unwindowed samples.
        std::array<float, 2> peak{}, rms{};
        std::size_t clippedSamples = 0; // Meters: finite samples at or above full scale across both channels.
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
    enum class AverageStatus { Waiting, Ready, InvalidInput, Unavailable };
    // Optional sidecar keeps the Raw Result layout/allocation path unchanged.
    // The fixed profile is a one-second 20 dB linear-magnitude decay, not LUFS.
    struct AverageResult {
        std::shared_ptr<const Result> window;
        std::shared_ptr<const std::vector<float>> binsDbfs;
        AverageStatus status = AverageStatus::Waiting;
        std::uint64_t epoch = 0, processedWindows = 0, windowSerial = 0;
    };
    struct MaximumResult {
        std::shared_ptr<const Result> window;
        std::vector<float> binsDbfs;
    };
    struct StereoMeterResult {
        dsp::StereoEnvelopeMeter::Reading reading;
        double sampleRate = 0;
        std::uint64_t generation = 0, epoch = 0;
    };
    struct Stats {
        std::size_t consumers = 0, domains = 0, taps = 0;
        std::uint64_t transforms = 0, publications = 0, frameRequests = 0, workerPasses = 0;
        std::uint64_t cursorReads = 0, analysisWindows = 0;
        std::size_t spectralPlans = 0, historyBytes = 0, historyPeakBytes = 0;
        std::uint64_t droppedHistoryColumns = 0;
        // Successful insertions into this broker's shared scheduler queue.
        std::uint64_t successfulEnqueues = 0;
    };
    class Lease {
      public:
        struct SourcePosition {
            std::uint64_t generation = 0;
            std::int64_t written = 0;
        };
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
        // Only Average1s leases: same immutable sidecar/window for compatible
        // consumers. Missing/refused publication returns null; inspect status.
        std::shared_ptr<const AverageResult> readAverage() const noexcept;
        // Cumulative demand shares the FFT and owns its own hold interval.
        // A combined lease can select the maximum output explicitly.
        std::shared_ptr<const MaximumResult> readMaximum() const noexcept;
        std::shared_ptr<const MaximumResult> readMaximum(Output) const noexcept;
        bool restartCumulativeMaximum() noexcept;
        std::shared_ptr<const StereoMeterResult> readStereoMeter() const noexcept;
        bool restartStereoMeter() noexcept;
        // Owner-only activation barrier, not a paint/audio query. A nonzero
        // generation identifies the currently usable tap; written is its completed
        // sample count, independent of analysis backlog or publication revisions.
        SourcePosition sourcePosition() const noexcept;
      private:
        Lease(std::weak_ptr<State>, std::shared_ptr<Domain>, std::uint32_t, std::shared_ptr<CumulativeHistory>);
        std::weak_ptr<State> state_;
        std::shared_ptr<Domain> domain_;
        std::shared_ptr<CumulativeHistory> cumulative_;
        std::uint32_t outputs_ = 0;
        friend class SharedAnalysis;
    };
    // A continuous acquisition has its own sequence cursor. The physical tap
    // supplies sampleRate. Equal profiles on one lane share both sweeps.
    struct CaptureRequest {
        engine::ScopeSource source;
        Channel channel = Channel::Left;
        dsp::TriggeredScope::Settings settings;
        bool operator==(const CaptureRequest &) const = default;
    };
    struct CaptureResult {
        std::vector<dsp::TriggeredScope::Column> current, previous;
        double sampleRate = 0;
        std::uint64_t revision = 0, sourceEpoch = 0, generation = 0, transportRevision = 0;
        std::uint64_t currentSerial = 0, previousSerial = 0, gapCount = 0, overrunCount = 0;
        std::int64_t currentTriggerSequence = -1, previousTriggerSequence = -1;
        std::int64_t pendingFrames = 0;
    };
    class CaptureLease {
      public:
        CaptureLease() = default;
        ~CaptureLease();
        CaptureLease(CaptureLease &&) noexcept;
        CaptureLease &operator=(CaptureLease &&) noexcept;
        CaptureLease(const CaptureLease &) = delete;
        CaptureLease &operator=(const CaptureLease &) = delete;
        void reset() noexcept;
        explicit operator bool() const noexcept;
        bool available() const noexcept;
        std::shared_ptr<const CaptureResult> read() const noexcept;
        Lease::SourcePosition sourcePosition() const noexcept;
      private:
        CaptureLease(std::weak_ptr<State>, std::shared_ptr<CaptureDomain>);
        std::weak_ptr<State> state_;
        std::shared_ptr<CaptureDomain> domain_;
        friend class SharedAnalysis;
    };
    enum class BarScopeStatus { Idle, Pending, Ready, Missed };
    struct BarScopeResult {
        std::int64_t firstStamp = 0;
        std::size_t frames = 0;
        double sampleRate = 0.;
        std::uint64_t transportRevision = 0;
        std::shared_ptr<const FocusScopeBarProjection> projection;
    };
    class BarScopeLease {
      public:
        BarScopeLease() = default;
        ~BarScopeLease();
        BarScopeLease(BarScopeLease &&) noexcept;
        BarScopeLease &operator=(BarScopeLease &&) noexcept;
        BarScopeLease(const BarScopeLease &) = delete;
        BarScopeLease &operator=(const BarScopeLease &) = delete;
        explicit operator bool() const noexcept;
        bool request(const FocusScopeRange &, std::uint64_t expectedRevision = 0) noexcept;
        // Use the same sources. Mid starts with a full unread window and can be dropped mid-capture.
        void setMidRequested(bool) noexcept;
        double sampleRate() const noexcept;
        BarScopeStatus status() const noexcept;
        std::shared_ptr<const BarScopeResult> read() const noexcept;
        void reset() noexcept;
      private:
        BarScopeLease(std::weak_ptr<State>, std::shared_ptr<BarScopeDomain>);
        std::weak_ptr<State> state_;
        std::shared_ptr<BarScopeDomain> domain_;
        friend class SharedAnalysis;
    };
    enum class FocusLagStatus { Idle, Pending, Ready, Refused };
    class FocusLagLease {
      public:
        FocusLagLease() = default;
        ~FocusLagLease();
        FocusLagLease(FocusLagLease &&) noexcept;
        FocusLagLease &operator=(FocusLagLease &&) noexcept;
        FocusLagLease(const FocusLagLease &) = delete;
        FocusLagLease &operator=(const FocusLagLease &) = delete;
        explicit operator bool() const noexcept;
        FocusLagStatus status() const noexcept;
        std::shared_ptr<const FocusLagPreview> read() const noexcept;
        void reset() noexcept;
      private:
        FocusLagLease(std::weak_ptr<State>, std::shared_ptr<FocusLagJob>);
        std::weak_ptr<State> state_;
        std::shared_ptr<FocusLagJob> job_;
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
    Lease acquireStereoMeter(std::int64_t track, StereoMeterProfile);
    CaptureLease acquireCapture(const CaptureRequest &);
    BarScopeLease acquireBarScope(std::span<const engine::ScopeSource>, std::size_t columns);
    // One owner-requested calculation on existing source demand. An empty
    // lease means the broker is busy or the requested bindings are unavailable.
    FocusLagLease requestFocusLag(const FocusLagRequest &);
    void shutdown() noexcept;
    // Owner-only lifecycle query, including consumers which released all leases
    // to hold local pictures. False after explicit shutdown.
    bool active() const noexcept;
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
