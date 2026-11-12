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
// One document's visible analysis demand. It owns no audio processor and never changes audio.
// Full and compact views acquire the same request and read the same immutable publication.
// Creation, acquire, refreshSources, Lease destruction/reset and broker destruction belong to
// the document owner thread. Only pump/the optional single worker run analysis. Session callbacks
// are invoked on the owner thread, never by that worker. A lease must not outlive its document
// owner thread; it may outlive the broker, in which case it is inert.
//
// This adapts the existing newest-window ScopeTap API. It is NOT MX-30's future gapless cursor,
// device-tap, history-negotiation or epoch-reclamation implementation. Every acquired tap must
// have kTapSeconds of history; a too-small tap is unavailable rather than silently shortened.
class SharedAnalysis final {
    struct State;
    struct Domain;
  public:
    static constexpr double kTapSeconds = 10.0;
    static constexpr std::size_t kMaxDomains = 128;
    static constexpr std::int32_t kMaxFftSize = 65536;
    enum class Channel : std::uint8_t { Mid, Side, Left, Right };
    enum class Mode { Worker, Manual };
    struct Request {
        std::int64_t track = 0;
        engine::ScopePoint point = engine::ScopePoint::PostFader;
        std::int32_t fftSize = 1024;
        Channel channel = Channel::Mid;
        dsp::Window window = dsp::Window::Hann;
        auto operator<=>(const Request &) const = default;
    };
    struct Result {
        Request request;
        std::uint64_t revision = 0, sourceEpoch = 0;
        // Tap's heard-timeline stamp of the first sample in its newest window. No claim of a
        // gapless stream or precise correspondence with an independently loaded written counter.
        std::int64_t firstStamp = 0;
        double sampleRate = 0;
        std::array<float, 2> peak{}, rms{};
        std::vector<float> binsDbfs;
    };
    struct Stats {
        std::size_t consumers = 0, domains = 0, taps = 0;
        std::uint64_t transforms = 0, publications = 0;
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
        Lease(std::weak_ptr<State>, std::shared_ptr<Domain>);
        std::weak_ptr<State> state_;
        std::shared_ptr<Domain> domain_;
        friend class SharedAnalysis;
    };
    // AcquireTap must ensure the track/point tap is attached with kTapSeconds history. It is
    // idempotent: refreshSources may call it again while attached. Null means unavailable.
    // ReleaseTap is called exactly once at last demand, including after an unavailable refresh
    // if that key had previously attached. Callbacks must not recursively enter this broker.
    using AcquireTap = std::function<std::shared_ptr<const engine::ScopeTap>(std::int64_t, engine::ScopePoint)>;
    using ReleaseTap = std::function<void(std::int64_t, engine::ScopePoint)>;
    SharedAnalysis(AcquireTap, ReleaseTap, Mode = Mode::Worker);
    ~SharedAnalysis();
    SharedAnalysis(const SharedAnalysis &) = delete;
    SharedAnalysis &operator=(const SharedAnalysis &) = delete;
    Lease acquire(const Request &);
    // Permanent owner-thread revocation. Join the worker and detach taps before Session goes
    // away even when views still retain this broker. Idempotent; all later leases are inert.
    void shutdown() noexcept;
    // Owner calls after source/rate/graph replacement. forceEpoch is for an explicit transport
    // seek: invalidate the old publication and wait for newly written samples. Existing leases
    // remain valid. Repeated refresh of the same source does not reset it.
    void refreshSources(bool forceEpoch = false);
    // Deterministic worker pass, useful in headless tests. Returns the transforms completed.
    // Newest-window analysis only; repeated calls without new samples do no FFT/publication.
    // First demand on a newly attached tap waits for a write after acquisition, so reopening
    // a retained Session tap never republishes audio from before the view was hidden.
    std::size_t pump();
    Stats stats() const;
  private:
    std::shared_ptr<State> state_;
    struct Worker;
    std::unique_ptr<Worker> worker_;
};
} // namespace adi::ui
