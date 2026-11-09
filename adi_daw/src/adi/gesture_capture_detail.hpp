// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gesture_navigation_detail.hpp"
#include "gesture_presentation.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace adi::engine { class SessionEndpoint; class ProjectViewEndpoint; }
namespace adi::gesture::detail {

/// Live owner-thread lookup through AppliedSource's weak connection domain.
/// Domain remembers Store* only while live, set by Store::open and invalidated
/// before destruction/swap. No SQLite lifetime extension or callback SQL access.
/// All entry methods recheck connection/thread before dereferencing Store.
CoordinatorResult coordinatorFor(const AppliedSource&);

/// Handle to PREALLOCATED coordinator-owned pending entry + provisional lease.
/// A target conflict may make it Suspended but cannot discard captured external
/// input. Destruction abandons the handle, not the owned restoration task/lease.
class PendingCapture {
public:
    ~PendingCapture();
    PendingCapture(PendingCapture&&) noexcept;
    PendingCapture& operator=(PendingCapture&&) noexcept;
    PendingCapture(const PendingCapture&) = delete;
    PendingCapture& operator=(const PendingCapture&) = delete;
    [[nodiscard]] const OwnerToken& owner() const noexcept;
    [[nodiscard]] const AppliedSource& source() const noexcept;
    [[nodiscard]] const Status& status() const noexcept;
private:
    struct Data;
    explicit PendingCapture(std::shared_ptr<Data>);
    std::shared_ptr<Data> data_;
    friend struct CaptureAccess;
};
struct PendingCaptureResult {
    Status status;
    std::unique_ptr<PendingCapture> capture;
};
/// Call after copying/classifying callback input, BEFORE acknowledging its
/// source reservations. Failure leaves original reservations held. Claims may
/// conflict: retain a suspended entry and owned input for explicit resolution.
PendingCaptureResult reservePending(Coordinator&, std::vector<TargetClaim>,
    std::shared_ptr<Presentation>, Spec, std::unique_ptr<Producer>,
    std::unique_ptr<const Input> absoluteInput);

/// Origin/Input/Producer OWN the complete Inbox, including pre-drain live origin,
/// parameter rows, opaque bytes and exact IDs. Admission captures current SQL
/// separately; originBase head/sequence stay unknown for callback-origin data.
/// This transfers the existing provisional lease, never creates a guard gap.
/// Arguments transferred at reservePending remain owned even if replay/adoption
/// refuses. Producer-derived claims must match the reserved literal claims.
BeginResult adoptPending(Coordinator&, PendingCapture&);

struct CapturedOffer {
    Status status;
    std::vector<OpRequest> requests;
    std::shared_ptr<const OpCapturedTransaction> transaction;
};
/// Core seals exact requests, guards, resources and source/head from the latest
/// valid owned entry; every exported request carries the same typed capability.
/// At most one completed gesture per legacy vector. Never releases the entry.
CapturedOffer offerCaptured(Session&);
/// Explicit retry reprepares retained origin/intent on actual current base and
/// supersedes any older external capability. Durable entries refuse retry.
CapturedOffer retryCaptured(Session&);

/// Engine-owned automatic presentation registration through existing calls.
/// Concrete revocable endpoint wrappers are owned by Session/ProjectView and
/// return null after destructor-entry revocation even if wrappers are retained.
/// Registry is weak; no raw owner may be recovered after revocation.
Status registerSessionEndpoint(const AppliedSource&,
                               std::shared_ptr<const engine::SessionEndpoint>);
Status registerProjectViewEndpoint(const AppliedSource&,
                                   std::shared_ptr<const engine::ProjectViewEndpoint>);
/// Unique compatible live endpoints supply the default EnginePresentation.
/// Ambiguous automatic pairing refuses; an explicitly constructed presentation
/// remains available. ParamOps binds its known Session/private presentation;
/// runtime-only fixtures do not require a ProjectView.

} // namespace adi::gesture::detail
