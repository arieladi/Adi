// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gesture_session.hpp"
#include <cstdint>
#include <memory>
#include <span>

namespace adi::engine { class Session; class ProjectView; }
namespace adi::gesture {
namespace detail { struct CaptureAccess; }

/// Issued by the coordinator, not numeric track/device identity. Definitions
/// and constructors are internal; external callers cannot forge ownership.
class OwnerToken {
public:
    [[nodiscard]] bool sameOwner(const OwnerToken&) const noexcept;
private:
    struct Data;
    explicit OwnerToken(std::shared_ptr<const Data>);
    std::shared_ptr<const Data> data_;
    friend class Session;
    friend class Coordinator;
    friend struct detail::CaptureAccess;
};
/// Trusted presentation implementations derive owned capture/preparation
/// values. These complete base classes make return-value destruction safe in
/// callers without exposing concrete engine internals or SQL references.
class CapturedPresentation {
public:
    virtual ~CapturedPresentation() = default;
protected:
    CapturedPresentation() = default;
};
class PreparedPresentation {
public:
    virtual ~PreparedPresentation() = default;
protected:
    PreparedPresentation() = default;
};

struct CaptureResult {
    Status status;
    std::shared_ptr<const engine::Snapshot> snapshot;
    std::unique_ptr<const CapturedPresentation> value;
    /// Previous COMMITTED image, never the scratch projection being captured.
    std::optional<OpPresentationImageGuard> presentationImage{};
};
struct PrepareResult {
    Status status;
    std::unique_ptr<PreparedPresentation> value;
};

/// Core trusted seam. Concrete EnginePresentation below is required for HI-01;
/// a fake implementation is useful for owner tests but does not close N44.
class Presentation {
public:
    virtual ~Presentation() = default;
    /// Trusted concrete ParamOps presentation only. General structural engine
    /// presentation leaves this false and cannot stage arbitrary plugin state.
    virtual bool supportsDeviceValues() const noexcept { return false; }
    /// Outside scratch, before begin/rebase evaluation: capture/prepare current
    /// committed restoration state without erasing an audible preview. No stale
    /// Session model may masquerade as the newly observed committed SQL base.
    virtual PresentationStatus adoptCommitted(const Store&, const Base&,
                                              const AppliedBatch* provenance) = 0;
    /// Scratch is visible here. Copy rows/ClipProject/settings/Snapshot only.
    /// No Session::refresh, plugin calls, file opens, source workers, graph
    /// preparation/publication, freeze cache remember, or live node mutation.
    virtual CaptureResult capture(const Store& scratch, const Base&,
        const OwnerToken&, Strategy, std::span<const OpRequest>) = 0;
    /// Called only after explicit successful SQL rollback. Heavy engine/source
    /// preparation may occur here, with zero mutation of the current generation.
    virtual PrepareResult prepare(std::unique_ptr<const CapturedPresentation>) = 0;
    /// Validate source/owner/current-base/Session/format epochs again. Publish
    /// owned view and engine handles only if all admissions still match.
    /// Reserve retirement/bookkeeping storage BEFORE the first graph exchange;
    /// after exchange, no throw/refusal may misreport audible publication.
    virtual PresentationStatus publish(std::unique_ptr<PreparedPresentation>) = 0;
    /// After own durable commit, outside observer dispatch: compare actual
    /// committed projection/base/receipt, then promote matching current preview
    /// without replaying heard state; otherwise adopt the actual committed base.
    virtual PresentationStatus promote(const Store&, const Base&,
                                       const OwnerToken&, const AppliedBatch&) = 0;
    /// Remove only this owner; latest committed truth survives. A failed or
    /// delayed restoration remains in the runtime registry with a receipt.
    /// clear marks a task allocated at capture/publish, never creates ownership
    /// in a destructor. Refused clear keeps the live source coordinator lease.
    virtual PresentationStatus clear(const OwnerToken&) = 0;
};

/// Concrete engine adapter; captures source, ProjectView and Session weak
/// lifetime tokens, not
/// SQL pointers. ProjectView must retain committed and composed preview state
/// separately. Session hosts structural generation + disjoint scalar overlays.
class EnginePresentation final : public Presentation {
public:
    EnginePresentation(engine::ProjectView&, engine::Session&);
    ~EnginePresentation() override;
    PresentationStatus adoptCommitted(const Store&, const Base&,
                                      const AppliedBatch*) override;
    CaptureResult capture(const Store&, const Base&, const OwnerToken&, Strategy,
                          std::span<const OpRequest>) override;
    PrepareResult prepare(std::unique_ptr<const CapturedPresentation>) override;
    PresentationStatus publish(std::unique_ptr<PreparedPresentation>) override;
    PresentationStatus promote(const Store&, const Base&, const OwnerToken&,
                               const AppliedBatch&) override;
    PresentationStatus clear(const OwnerToken&) override;
private:
    struct Data;
    std::unique_ptr<Data> data_;
};

} // namespace adi::gesture
