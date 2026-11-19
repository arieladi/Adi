// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/applied_batches.hpp"
#include "adi/ops.hpp"
#include "op_transaction_resources.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi {
class Store;
namespace engine { struct Snapshot; }
namespace gesture {
namespace detail { struct CaptureAccess; }

/// ADR-0216's two strategies. LiveValues includes concrete scalar overlays and
/// ParamOps' private owned parameter/state capture, not arbitrary state packets.
enum class Strategy { LiveValues, Structural };
enum class State {
    Open, Previewed, Suspended, Committing, Cancelling, Committed, Cancelled, SourceLost
};
enum class Error {
    None, WrongThread, Closed, Busy, Reentrant, SourceLost, StaleBase,
    TargetConflict, InvalidInput, InvalidOperation, UnsupportedPreview,
    ActiveSqlTransaction, RollbackFailed, CaptureFailed, PreparationFailed,
    PublicationStale, CommitRefused, CaptureOverflow
};
struct Status {
    Error error = Error::None;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return error == Error::None; }
};

/// Literal connection identity plus committed journal head, including NULL.
/// sequence is N47's publication sequence, not a replacement for row guards.
struct Base {
    AppliedSource source;
    std::optional<std::int64_t> head;
    std::uint64_t sequence = 0;
};
/// Callback capture cannot read SQL. An undrained runtime Begin has owned
/// starting values, but unknown journal head/sequence at that instant. Do not
/// mislabel the later owner admission head as the runtime origin's head.
struct OriginBase {
    AppliedSource source;
    std::optional<std::optional<std::int64_t>> headAtCapture;
    std::optional<std::uint64_t> sequenceAtCapture;
};
struct Spec {
    Strategy strategy = Strategy::Structural;
    Actor actor = Actor::User;
    std::string actorDetail;
    std::string label;
    std::optional<Payload> selBefore;
};

/// Trusted producer extension values. Ownership transfers into GestureSession;
/// implementations must own all rows/defaults/bytes and contain no Store,
/// statement, live widget, mutable external selection or audio-owned state.
/// A producer recognises its own concrete classes; no unchecked void casts.
class Origin { public: virtual ~Origin() = default; };
class Input { public: virtual ~Input() = default; };
enum class ClaimKind { Structural, ScalarEndpoint, DeviceInstance };
enum class TargetDomain { Material, Track, Routing, Group, Device };
struct TargetClaim {
    ClaimKind kind;
    TargetDomain domain;
    std::int64_t id = 0;
    std::string endpoint;
    std::weak_ptr<const void> instance;
};

/// Typed transaction resources. These never become op payload fields.
/// Journal and preview validate hash/content, then insert in their transaction
/// BEFORE inverse/apply handlers. Cancel/destruction never writes these blobs.
using StateBlob = OpStateBlob;

/// Mutable construction result only. It is NOT a bound or published preview.
struct Proposal {
    std::vector<OpRequest> requests;
    std::vector<StateBlob> stateBlobs;
    std::vector<OpRowGuard> rowGuards;
    std::vector<OpRowSetGuard> rowSetGuards;
    std::optional<OpPresentationImageGuard> presentationImage{};
};
struct OriginResult { Status status; std::unique_ptr<const Origin> value; };
struct ProposalResult { Status status; Proposal value; };
class Producer {
public:
    virtual ~Producer() = default;
    /// Called exactly once on committed rows after source lease admission.
    virtual OriginResult captureOrigin(const Store&, const Base&) = 0;
    /// Fixed target ownership from the captured origin, never ambient selection.
    /// Coordinator enforces conflicts across all concrete presentations.
    virtual std::vector<TargetClaim> targetClaims(const Origin&) const = 0;
    /// Always origin + latest ABSOLUTE input + CURRENT committed base.
    /// Must validate target identity/old-row guards, including same-head edits.
    /// May preserve unrelated changes or explicitly refuse conflicts; never
    /// derive from a previous preview or merely restamp old expectedHead.
    virtual ProposalResult prepare(const Store&, const Base&, const Origin&,
                                   const Input&) const = 0;
};

class Preview;
class Presentation;
class Session;
class Coordinator;
class RestorationReceipt;
struct BeginResult { Status status; std::unique_ptr<Session> session; };
struct UpdateResult { Status status; std::shared_ptr<const Preview> preview; };
enum class PresentationState { Current, Pending, Refused, Detached };
struct PresentationStatus {
    PresentationState state = PresentationState::Current;
    std::string message;
    /// Preallocated runtime-owned task. Retaining status does not retain Store.
    /// Nonterminal restoration remains discoverable even if public Session dies.
    std::shared_ptr<const RestorationReceipt> restoration;
};
class RestorationReceipt {
public:
    virtual ~RestorationReceipt() = default;
    [[nodiscard]] virtual PresentationState state() const noexcept = 0;
    [[nodiscard]] virtual const std::string& message() const noexcept = 0;
protected:
    /// Trusted concrete runtime task supplies this observation interface.
    /// It is diagnostic state, not a forgeable publication/admission capability.
    RestorationReceipt() = default;
};
struct CommitOutcome {
    Status status;
    /// If present and ok, SQL COMMIT succeeded. A later presentation failure
    /// NEVER changes this outcome or causes the requests to be submitted again.
    std::optional<CommitResult> durable;
    bool semanticNoOp = false;
    PresentationStatus presentation;
};
struct CancelOutcome { Status status; PresentationStatus presentation; };

/// Immutable public observation. No public constructor or mutable request list.
/// Requests/resources/base/projection come from the same sealed evaluation.
class Preview {
public:
    [[nodiscard]] const Base& base() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::span<const OpRequest> requests() const noexcept;
    [[nodiscard]] std::shared_ptr<const engine::Snapshot> snapshot() const noexcept;
private:
    struct Data;
    explicit Preview(std::shared_ptr<const Data>);
    std::shared_ptr<const Data> data_;
    friend class Session;
    friend struct detail::CaptureAccess;
};

/// Owner-thread handle for a coordinator-owned entry. One source-domain lease
/// lasts from successful begin until durable success or completed cancel.
/// Failed submission/cancel retains exact intent, resources and rollback data.
/// Destructor marks its preallocated entry abandoned and requests cancel;
/// refusal transfers no allocation and does not release the entry/lease.
class Session {
public:
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    UpdateResult update(std::unique_ptr<const Input> absoluteInput);
    /// Owner retry for dirty foreign base or presentation failure. Never
    /// appends a transaction and does not recapture Origin.
    UpdateResult reapply();
    CommitOutcome commit();
    CancelOutcome cancel();
    /// Cancelling is sticky: update/reapply/commit refuse and cannot reinstate
    /// the old intent. retryCancel only advances restoration to Cancelled.
    CancelOutcome retryCancel();
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] const OriginBase& originBase() const noexcept;
    [[nodiscard]] const Base& currentBase() const noexcept;
    [[nodiscard]] std::shared_ptr<const Preview> preview() const noexcept;
    [[nodiscard]] const Status& lastStatus() const noexcept;
private:
    struct Data;
    explicit Session(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
    friend class Coordinator;
    friend struct detail::CaptureAccess;
};

struct CoordinatorResult { Status status; std::shared_ptr<Coordinator> value; };
struct DrainOutcome {
    Status status;
    std::size_t reapplied = 0;
    std::size_t pendingRestorations = 0;
};
/// Source Store retains this coordinator while open; the concrete engine also
/// retains its restoration registry after source destruction. Neither the
/// registry nor entries may extend SQLite lifetime. No ownership cycle.
class Coordinator {
public:
    ~Coordinator();
    static CoordinatorResult attach(Store&, std::shared_ptr<Presentation>);
    BeginResult begin(Spec, std::unique_ptr<Producer>,
                      std::shared_ptr<Presentation> perGesturePresentation = {});
    /// N47 callbacks ONLY enqueue/mark dirty. Call after notification dispatch;
    /// ParamOps/Session pass their existing supplied monotonic owner clock.
    DrainOutcome drain(std::int64_t nowMs);
    DrainOutcome retryRestorations(std::int64_t nowMs);
    [[nodiscard]] std::size_t pendingRestorations() const noexcept;
private:
    struct Data;
    explicit Coordinator(std::unique_ptr<Data>);
    std::unique_ptr<Data> data_;
    friend struct detail::CaptureAccess;
};

} // namespace gesture
/// ADR-0216's public spelling; distinct from playback engine::Session.
using GestureSession = gesture::Session;
using GestureCoordinator = gesture::Coordinator;
} // namespace adi
