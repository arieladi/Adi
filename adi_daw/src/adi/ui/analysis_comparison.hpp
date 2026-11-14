// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ui/shared_analysis.hpp"
#include <optional>
#include <span>

namespace adi::ui {
// Owner-thread spectral pictures, not aligned audio, a null test or a gain-matching
// processor. Shared ownership retains the broker's lifetime; shutdown still revokes
// readiness. All methods and destruction run
// on its owner thread; frame() uses the caller's existing frame clock.
class AnalysisComparison final {
  public:
    struct Endpoint {
        SharedAnalysis::Request request;
        // Nonzero identity supplied by the caller. Change it when the endpoint's
        // document/selection incarnation changes, even if numeric source IDs repeat.
        std::uint64_t ownerToken = 0;
        auto operator<=>(const Endpoint &) const = default;
    };
    struct Metadata {
        Endpoint endpoint;
        std::uint64_t revision = 0, sourceEpoch = 0, generation = 0, windowSerial = 0;
        std::int64_t firstStamp = 0, firstSequence = 0;
        double sampleRate = 0, coherentGain = 0;
        std::int32_t hopSize = 0;
        std::uint32_t outputs = 0;
        std::uint64_t gapCount = 0, overrunCount = 0;
        std::uint64_t historySerial = 0, droppedHistoryColumns = 0;
        std::size_t nonFiniteSamples = 0;
        std::int64_t pendingFrames = 0;
        bool advancing = false, discontinuity = false, budgetLimited = false, historyLimited = false;
    };
    struct Picture {
        const Metadata *metadata = nullptr;
        std::span<const float> binsDbfs;
    };
    enum class Status { Unavailable, Independent, Held, Incompatible };
    struct View {
        Picture a, b;
        Status status = Status::Unavailable;
        bool visible = false, frozen = false, referenceHeld = false;
    };
    struct Difference {
        // No value for unavailable/incompatible/out-of-range or censored bins.
        std::optional<float> db;
        bool aCensored = false, bCensored = false;
    };

    explicit AnalysisComparison(std::shared_ptr<SharedAnalysis> broker) noexcept;
    ~AnalysisComparison() = default;
    AnalysisComparison(const AnalysisComparison &) = delete;
    AnalysisComparison &operator=(const AnalysisComparison &) = delete;

    // Spectrum-only requests. Hop zero is canonicalized to N/4. Invalid settings,
    // unavailable broker capacity and thrown allocation/acquisition failures return false
    // without changing accepted endpoints, pictures, hold/freeze or visibility.
    // An unavailable tap can still have a valid pending lease.
    // This is exception rollback, not recovery from allocator/runtime termination.
    // Activation/resume fences use the tap's generation and written count. An
    // admitted window must end strictly beyond that count: it includes new input,
    // but may overlap the action. This is not a whole-window-after-action promise.
    // Successful B changes clear its reference; A-only changes retain B, but reject
    // an incompatible requested calibration while B is held. Rate is checked on data.
    bool configure(Endpoint a, Endpoint b) noexcept;
    bool setVisible(bool visible) noexcept;
    bool setFrozen(bool frozen) noexcept;
    bool holdReference(std::uint64_t expectedBToken) noexcept;
    // Resume/clear also leave whole-comparison freeze and wait for new A/B data.
    bool resumeReference() noexcept;
    // Clear local pictures/hold/freeze and wait for fresh data. Never resets the broker.
    bool clear() noexcept;
    void frame() noexcept;

    // Views borrow storage. EVERY non-const method, including failed operations,
    // invalidates previously returned views. Copy needed bins/metadata before calling
    // back into the component. Hidden views are unavailable; captured data is retained.
    [[nodiscard]] View view() const noexcept;
    [[nodiscard]] Endpoint endpointA() const noexcept { return a_.endpoint; }
    [[nodiscard]] Endpoint endpointB() const noexcept { return b_.endpoint; }
    [[nodiscard]] std::size_t capturedBinCount() const noexcept;
    // Pure validation of independently supplied pictures; no timestamp equality can
    // upgrade Independent to an aligned status. Exact calibration, no resampling.
    [[nodiscard]] static Status classify(Picture a, Picture b) noexcept;
    [[nodiscard]] static Difference difference(const View &, std::size_t bin) noexcept;

  private:
    struct Snapshot {
        Metadata metadata;
        std::vector<float> bins;
        [[nodiscard]] Picture picture() const noexcept { return {&metadata, bins}; }
    };
    struct Watermark {
        std::uint64_t generation = 0;
        std::int64_t end = 0;
    };
    struct Input {
        Endpoint endpoint;
        SharedAnalysis::Lease lease;
        std::shared_ptr<const SharedAnalysis::Result> live;
        Metadata metadata;
        Watermark watermark;
        [[nodiscard]] Picture picture() const noexcept {
            return live ? Picture{&metadata, live->binsDbfs} : Picture{};
        }
    };
    std::shared_ptr<SharedAnalysis> broker_;
    Input a_, b_;
    std::optional<Snapshot> frozenA_, frozenB_, heldB_;
    bool configured_ = false, visible_ = false, frozen_ = false;

    static bool normalize(Endpoint &) noexcept;
    static bool sameCalibration(const SharedAnalysis::Request &, const SharedAnalysis::Request &) noexcept;
    static bool validPicture(Picture) noexcept;
    static Metadata metadata(const SharedAnalysis::Result &, std::uint64_t) noexcept;
    static Snapshot snapshot(Picture);
    static void fence(Input &) noexcept;
    static void release(Input &) noexcept;
    static void update(Input &) noexcept;
    bool acquire(Input &) const;
    bool restart(bool retainReference) noexcept;
};
} // namespace adi::ui
