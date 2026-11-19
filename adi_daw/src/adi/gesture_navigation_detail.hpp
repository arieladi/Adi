// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gesture_session.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace adi::gesture::detail {
struct Domain;
struct ProducerSlot;
struct CaptureAccess;
class PendingCapture;

/// Store-owned domain; weak owner leases cannot keep Store/SQLite alive.
/// Live/suspended/commit-refused gestures block navigation and explicit
/// close/SaveAs. Store destruction invalidates without journal submission.
class Lease {
public:
    Lease();
    ~Lease();
    Lease(Lease&&) noexcept;
    Lease& operator=(Lease&&) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    [[nodiscard]] bool alive() const noexcept;
    void reset() noexcept;
private:
    struct Data;
    std::unique_ptr<Data> data_;
    friend struct CaptureAccess;
};

/// Held for the ENTIRE History navigation, including same-head metadata paths.
/// Normal other-producer OpJournal commits do not acquire this permit.
class NavigationPermit {
public:
    NavigationPermit();
    ~NavigationPermit();
    NavigationPermit(NavigationPermit&&) noexcept;
    NavigationPermit& operator=(NavigationPermit&&) noexcept;
    NavigationPermit(const NavigationPermit&) = delete;
    NavigationPermit& operator=(const NavigationPermit&) = delete;
private:
    struct Data;
    std::unique_ptr<Data> data_;
    friend struct CaptureAccess;
};
struct NavigationResult { Status status; NavigationPermit permit; };
NavigationResult beginNavigation(Store&);
/// Source switching has two phases. While the permit is held, callback input
/// may still reserve AfterNavigation. Finalization atomically closes capture
/// only when no new reservation/fault arrived; refusal keeps the old source.
/// Signals after successful finalization reject Closed and their producer must
/// preserve loss/reconcile evidence before any successor-domain rebind.
NavigationResult beginSourceChange(Store&);
Status finishSourceChange(NavigationPermit&);
void invalidateSource(Store&) noexcept;
bool mutationAllowed(const Store&, std::string& error);

/// Allocated/registered on owner, retained until its callback producer is
/// detached and quiescent. The callback uses a stable raw pointer only.
/// This is the integration object for ParamEditCapture, not a general
/// callable callback or permission to access Store from process().
class ProducerFence {
public:
    enum class Reservation { RejectedClosed, BeforeNavigation, AfterNavigation };
    /// One bounded, lock-free reservation before ring publication. No SQL,
    /// allocation, mutex, shared_ptr transition or retry/CAS loop on producer.
    Reservation reserve() noexcept;
    /// On full ring: set sticky loss before releasing this reservation.
    void publicationFailed() noexcept;
    /// Latch unsupported producer concurrency/overflow without consuming any
    /// reservation; safe even before a reservation has been obtained.
    void captureFault() noexcept;
    /// Owner: install an open gesture lease BEFORE acknowledging a Begin or
    /// implicit Value. Echo/no-op events still release their exact reservation.
    Status acknowledge(const PendingCapture&, std::size_t count);
    /// Trusted owner only: classified echo/no-op input, with no accepted
    /// gesture. Never use this to discard a refused/unknown user Begin/Value.
    Status discardUnchanged(std::size_t count);
    /// Owner only, after explicit cancel/reseed and producer quiescence proof.
    Status acknowledgeLoss();
private:
    std::shared_ptr<ProducerSlot> slot_;
    friend struct CaptureAccess;
};

/// Registration validates aggregate capacities/in-flight bounds against the
/// atomic count width, and refuses unsupported non-lock-free configurations.
/// The fence counts queued AND in-flight events, not ring indices alone.
struct FenceResult { Status status; std::unique_ptr<ProducerFence> fence; };
FenceResult registerProducer(Store&, std::size_t ringCapacity);
FenceResult registerProducer(const AppliedSource&, std::size_t ringCapacity);

/// Separate device state signals, including presets with no ParamEvent.
/// Owner registration is per-instance (current CLAP glue is per-instance).
/// VST3 audioProcessorChanged and CLAP stateSignal call begin before publishing
/// their unmuted epoch and finish afterward; muted host-origin signals skip it.
class StateProducerFence {
public:
    struct Watermarks {
        std::uint64_t reserved = 0, published = 0, consumed = 0;
        bool fault = false, closed = false;
    };
    /// Single checked reservation CAS. Contention, capacity exhaustion or
    /// counter overflow latches CaptureFault; no retry loop or serialization.
    /// false means caller still reports its SDK epoch, but capture/replay stays
    /// faulted until explicit owner recovery. No silently discarded state edit.
    bool beginSignal() noexcept;
    /// Call exactly once ONLY when beginSignal returned true. A false begin
    /// still permits the SDK epoch publication but owns no finish reservation.
    void finishSignal() noexcept;
    void captureFault() noexcept;
    /// Owner-only snapshot; capture bytes only at a complete stable watermark.
    [[nodiscard]] Watermarks watermarks() const noexcept;
    Status acknowledgePrefix(const PendingCapture&, std::uint64_t published);
    /// Requires detached/quiescent producer, explicit cancel/reseed and an
    /// owned recovery entry. It is not an automatic fault-dismissal path.
    Status resetAfterDetach(const PendingCapture&);
private:
    struct Data;
    std::shared_ptr<Data> data_;
    friend struct CaptureAccess;
};
struct StateFenceResult { Status status; std::unique_ptr<StateProducerFence> fence; };
StateFenceResult registerStateProducer(Store&, std::size_t pendingCapacity);
StateFenceResult registerStateProducer(const AppliedSource&, std::size_t pendingCapacity);

/// Internal counters are monotonic reserved/published/consumed watermarks.
/// Snapshot opaque bytes only with reserved==published, stable SDK epoch and
/// stable watermarks before/after capture. Acknowledge only that prefix; never
/// reset a counter and erase a concurrent signal. Transfer reservations into
/// a provisional owner lease before releasing the consumed source count.

} // namespace adi::gesture::detail
