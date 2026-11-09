// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/gesture_capture_detail.hpp"
#include "adi/applied_batches_detail.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/freeze_state.hpp"
#include "adi/engine/session_runtime.hpp"
#include "adi/store.hpp"
#include "adi/audio/io_audit.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

namespace adi::gesture {
namespace detail {
struct Entry;
struct Domain : std::enable_shared_from_this<Domain> {
    Store* store = nullptr;
    AppliedSource source;
    std::thread::id thread;
    std::atomic<std::uint64_t> fence{0}, faultEpoch{0};
    std::atomic<bool> hardFault{false};
    std::vector<std::shared_ptr<std::atomic<bool>>> producerFaults;
    std::uint64_t capacity = 0, leases = 0, nextOwner = 1;
    bool busy = false, journalAdmission = false, draining = false, closed = false;
    std::shared_ptr<Coordinator> coordinator;
    std::shared_ptr<Presentation> presentation;
    std::vector<std::shared_ptr<Entry>> entries;
    std::vector<std::shared_ptr<const AppliedBatch>> notices;
    std::vector<std::weak_ptr<const engine::SessionEndpoint>> sessions;
    std::vector<std::weak_ptr<const engine::ProjectViewEndpoint>> views;
    std::shared_ptr<AppliedObserver> observer;
    AppliedSubscription subscription;
};
struct ProducerSlot {
    std::shared_ptr<Domain> domain;
    std::shared_ptr<std::atomic<bool>> fault;
    std::atomic<std::uint64_t> outstanding{0};
};
struct DomainAccess {
    static std::shared_ptr<Domain>& stored(Store& store) { return store.gestureDomain_; }
    static const std::shared_ptr<Domain>& stored(const Store& store) { return store.gestureDomain_; }
};
constexpr std::uint64_t navigationBit = std::uint64_t{1} << 63;
constexpr std::uint64_t closedBit = std::uint64_t{1} << 62;
constexpr std::uint64_t faultBit = std::uint64_t{1} << 61;
constexpr std::uint64_t countMask = faultBit - 1;
} // namespace detail

struct OwnerToken::Data { std::weak_ptr<detail::Domain> domain; std::uint64_t identity = 0; };
OwnerToken::OwnerToken(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
bool OwnerToken::sameOwner(const OwnerToken& other) const noexcept { return data_ && data_ == other.data_; }
struct Preview::Data {
    Base base;
    std::uint64_t revision = 0;
    Proposal proposal;
    std::shared_ptr<const engine::Snapshot> snapshot;
};
Preview::Preview(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
const Base& Preview::base() const noexcept { return data_->base; }
std::uint64_t Preview::revision() const noexcept { return data_->revision; }
std::span<const OpRequest> Preview::requests() const noexcept { return data_->proposal.requests; }
std::shared_ptr<const engine::Snapshot> Preview::snapshot() const noexcept { return data_->snapshot; }

namespace detail {
struct Lease::Data { std::weak_ptr<Domain> domain; bool held = false; };
struct NavigationPermit::Data { std::shared_ptr<Domain> domain; bool acquired = false; };
struct StateProducerFence::Data {
    std::shared_ptr<Domain> domain;
    std::shared_ptr<std::atomic<bool>> fault;
    std::uint64_t capacity = 0;
    std::atomic<std::uint64_t> reserved{0}, published{0}, consumed{0};
};
struct Entry {
    explicit Entry(OwnerToken token) : owner(std::move(token)) {}
    OwnerToken owner;
    std::weak_ptr<Domain> domain;
    Lease lease;
    Spec spec;
    OriginBase originBase;
    Base base;
    std::unique_ptr<Producer> producer;
    std::unique_ptr<const Origin> origin;
    std::unique_ptr<const Input> input;
    std::vector<TargetClaim> claims;
    std::shared_ptr<Presentation> presentation;
    std::shared_ptr<const Preview> preview;
    std::shared_ptr<const OpCapturedTransaction> offered;
    State state = State::Open;
    Status status;
    PresentationStatus presentationStatus;
    std::uint64_t revision = 0;
    bool dirty = false, abandoned = false, callbackOrigin = false, provisional = false;
};
struct PendingCapture::Data { std::shared_ptr<Entry> entry; };
} // namespace detail
struct Session::Data { std::shared_ptr<detail::Entry> entry; };
struct Coordinator::Data { std::weak_ptr<detail::Domain> domain; };

namespace detail {
struct CaptureAccess {
    static std::shared_ptr<Domain> domain(Coordinator& coordinator) { return coordinator.data_->domain.lock(); }
    static std::shared_ptr<Entry> entry(Session& session) { return session.data_->entry; }
    static std::shared_ptr<Entry> entry(const PendingCapture& capture) { return capture.data_->entry; }
    static OwnerToken owner(const std::shared_ptr<Domain>& domain) {
        if (domain->nextOwner == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("gesture owner identity exhausted");
        auto data = std::make_shared<OwnerToken::Data>(); data->domain = domain; data->identity = domain->nextOwner++;
        return OwnerToken(std::move(data));
    }
    static Lease lease(const std::shared_ptr<Domain>& domain) {
        Lease result; result.data_ = std::make_unique<Lease::Data>(); result.data_->domain = domain;
        result.data_->held = true; ++domain->leases; return result;
    }
    static std::unique_ptr<Session> session(std::shared_ptr<Entry> entry) {
        auto data = std::make_unique<Session::Data>(); data->entry = std::move(entry);
        return std::unique_ptr<Session>(new Session(std::move(data)));
    }
    static std::shared_ptr<Coordinator> coordinator(const std::shared_ptr<Domain>& domain) {
        auto data = std::make_unique<Coordinator::Data>(); data->domain = domain;
        return std::shared_ptr<Coordinator>(new Coordinator(std::move(data)));
    }
    static std::unique_ptr<PendingCapture> pending(std::shared_ptr<Entry> entry) {
        auto data = std::make_shared<PendingCapture::Data>(); data->entry = std::move(entry);
        return std::unique_ptr<PendingCapture>(new PendingCapture(std::move(data)));
    }
    static NavigationPermit navigation(const std::shared_ptr<Domain>& domain) {
        NavigationPermit result; result.data_ = std::make_unique<NavigationPermit::Data>(); result.data_->domain = domain; return result;
    }
    static void acquired(NavigationPermit& permit) { permit.data_->acquired = true; }
    static std::shared_ptr<Domain> permitDomain(NavigationPermit& permit) {
        return permit.data_ && permit.data_->acquired ? permit.data_->domain : nullptr;
    }
    static std::unique_ptr<ProducerFence> producer(const std::shared_ptr<Domain>& domain) {
        auto result = std::make_unique<ProducerFence>(); result->slot_ = std::make_shared<ProducerSlot>(); result->slot_->domain = domain;
        result->slot_->fault = std::make_shared<std::atomic<bool>>(false); domain->producerFaults.push_back(result->slot_->fault); return result;
    }
    static std::unique_ptr<StateProducerFence> stateProducer(const std::shared_ptr<Domain>& domain, std::uint64_t capacity) {
        auto result = std::make_unique<StateProducerFence>(); result->data_ = std::make_shared<StateProducerFence::Data>();
        result->data_->domain = domain; result->data_->capacity = capacity;
        result->data_->fault = std::make_shared<std::atomic<bool>>(false); domain->producerFaults.push_back(result->data_->fault); return result;
    }
    static std::shared_ptr<const Preview> preview(Base base, std::uint64_t revision, Proposal proposal,
                                                 std::shared_ptr<const engine::Snapshot> snapshot) {
        auto data = std::make_shared<Preview::Data>(); data->base = std::move(base); data->revision = revision;
        data->proposal = std::move(proposal); data->snapshot = std::move(snapshot);
        return std::shared_ptr<const Preview>(new Preview(std::move(data)));
    }
    static const Proposal& proposal(const Preview& preview) { return preview.data_->proposal; }
};
namespace {
Status fail(Error code, std::string message) { return {code, std::move(message)}; }
Status usable(const std::shared_ptr<Domain>& domain, bool allowBusy = false) {
    if (audio::onAudioThread) return {Error::WrongThread, {}};
    if (!domain || domain->closed || !domain->source.alive() || !domain->store) return fail(Error::SourceLost, "gesture source is closed");
    if (domain->thread != std::this_thread::get_id()) return fail(Error::WrongThread, "gesture operation requires source owner thread");
    if (adi::detail::appliedDispatchActive(*domain->store)) return fail(Error::Reentrant, "gesture operation during applied-batch notification");
    if (domain->busy && !allowBusy) return fail(Error::Reentrant, "reentrant gesture operation");
    if (domain->store->readOnly()) return fail(Error::Closed, "gesture source is read-only");
    return {};
}
void fault(const std::shared_ptr<Domain>& domain, const std::shared_ptr<std::atomic<bool>>& producer = {}) noexcept {
    if (!domain) return;
    domain->faultEpoch.fetch_add(1, std::memory_order_relaxed);
    if (producer) producer->store(true, std::memory_order_release);
    else domain->hardFault.store(true, std::memory_order_release);
    domain->fence.fetch_or(faultBit, std::memory_order_release);
}
void recoverFault(const std::shared_ptr<Domain>& domain, const std::shared_ptr<std::atomic<bool>>& producer) noexcept {
    const auto epoch = domain->faultEpoch.load(std::memory_order_acquire);
    producer->store(false, std::memory_order_release);
    if (domain->hardFault.load(std::memory_order_acquire)) return;
    for (const auto& pending : domain->producerFaults)
        if (pending->load(std::memory_order_acquire)) return;
    domain->fence.fetch_and(~faultBit, std::memory_order_acq_rel);
    if (domain->faultEpoch.load(std::memory_order_acquire) != epoch) domain->fence.fetch_or(faultBit, std::memory_order_release);
}
ProducerFence::Reservation reserve(const std::shared_ptr<Domain>& domain) noexcept {
    if (!domain) return ProducerFence::Reservation::RejectedClosed;
    const auto before = domain->fence.fetch_add(1, std::memory_order_acq_rel);
    if ((before & (closedBit | faultBit)) != 0 || (before & countMask) == countMask) {
        if ((before & countMask) == countMask) fault(domain);
        domain->fence.fetch_sub(1, std::memory_order_release);
        return ProducerFence::Reservation::RejectedClosed;
    }
    return (before & navigationBit) != 0 ? ProducerFence::Reservation::AfterNavigation : ProducerFence::Reservation::BeforeNavigation;
}
void release(const std::shared_ptr<Domain>& domain, std::uint64_t count) noexcept {
    domain->fence.fetch_sub(count, std::memory_order_release);
}
bool terminal(State state) { return state == State::Committed || state == State::Cancelled || state == State::SourceLost; }
void sweepDurable(const std::shared_ptr<Domain>& domain) {
    for (const auto& entry : domain->entries)
        if (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable) entry->lease.reset();
}
std::shared_ptr<Domain> domainFor(Store& store) {
    auto& domain = DomainAccess::stored(store);
    if (!domain) {
        domain = std::make_shared<Domain>(); domain->store = &store; domain->source = appliedSource(store);
        domain->thread = std::this_thread::get_id();
        if (!domain->fence.is_lock_free()) throw std::runtime_error("gesture capture fence is not lock-free");
    }
    return domain;
}
Base currentBase(const std::shared_ptr<Domain>& domain) {
    return {domain->source, OpJournal(*domain->store).headSeq(), domain->source.sequence()};
}
bool sameBase(const Base& a, const Base& b) {
    return a.source.sameConnection(b.source) && a.head == b.head && a.sequence == b.sequence;
}
bool matchingLease(const PendingCapture& capture, const std::shared_ptr<Domain>& domain) {
    const auto entry = CaptureAccess::entry(capture);
    return entry && entry->domain.lock() == domain && entry->lease.alive() && !terminal(entry->state);
}
CancelOutcome cancelEntry(const std::shared_ptr<Entry>& entry);
void invalidateDomain(const std::shared_ptr<Domain>& domain) noexcept;
} // namespace

Lease::Lease() = default;
Lease::~Lease() { reset(); }
Lease::Lease(Lease&& other) noexcept : data_(std::move(other.data_)) {}
Lease& Lease::operator=(Lease&& other) noexcept { if (this != &other) { reset(); data_ = std::move(other.data_); } return *this; }
bool Lease::alive() const noexcept {
    const auto domain = data_ ? data_->domain.lock() : nullptr;
    return data_ && data_->held && domain && !domain->closed;
}
void Lease::reset() noexcept {
    if (data_ && data_->held) {
        if (const auto domain = data_->domain.lock(); domain && domain->leases != 0) --domain->leases;
        data_->held = false;
    }
}
NavigationPermit::NavigationPermit() = default;
NavigationPermit::~NavigationPermit() { if (data_ && data_->acquired) data_->domain->fence.fetch_and(~navigationBit, std::memory_order_release); }
NavigationPermit::NavigationPermit(NavigationPermit&& other) noexcept : data_(std::move(other.data_)) {}
NavigationPermit& NavigationPermit::operator=(NavigationPermit&& other) noexcept {
    if (this != &other) {
        if (data_ && data_->acquired) data_->domain->fence.fetch_and(~navigationBit, std::memory_order_release);
        data_ = std::move(other.data_);
    }
    return *this;
}
NavigationResult beginNavigation(Store& store) {
    if (audio::onAudioThread) return {{Error::WrongThread, {}}, {}};
    NavigationResult result;
    try {
        const auto domain = domainFor(store); result.status = usable(domain);
        if (!result.status.ok()) return result;
        sweepDurable(domain);
        if (domain->leases != 0) { result.status = fail(Error::Busy, "history navigation refused while a gesture is open"); return result; }
        auto permit = CaptureAccess::navigation(domain);
        std::uint64_t expected = 0;
        if (!domain->fence.compare_exchange_strong(expected, navigationBit, std::memory_order_acq_rel)) {
            // An unacquired permit must not clear someone else's gate.
            permit = NavigationPermit{};
            result.status = fail((expected & faultBit) != 0 ? Error::CaptureOverflow : Error::Busy,
                "history navigation refused while gesture capture is pending"); return result;
        }
        CaptureAccess::acquired(permit); result.permit = std::move(permit); return result;
    } catch (const std::exception& exception) { result.status = fail(Error::Busy, exception.what()); return result; }
}
NavigationResult beginSourceChange(Store& store) {
    if (audio::onAudioThread) return {{Error::WrongThread, {}}, {}};
    NavigationResult result;
    try {
        const auto domain = domainFor(store);
        if (domain->closed) return result; // forced destructor has already invalidated capture
        if (domain->thread != std::this_thread::get_id() || domain->busy || adi::detail::appliedDispatchActive(store)) {
            result.status = fail(Error::Reentrant, "source change requires idle source owner"); return result;
        }
        sweepDurable(domain);
        if (domain->leases != 0) { result.status = fail(Error::Busy, "source change refused while gesture is open"); return result; }
        auto permit = CaptureAccess::navigation(domain); std::uint64_t expected = 0;
        if (!domain->fence.compare_exchange_strong(expected, navigationBit, std::memory_order_acq_rel)) {
            result.status = fail(Error::Busy, "source change refused while capture is pending"); return result;
        }
        CaptureAccess::acquired(permit); result.permit = std::move(permit); return result;
    } catch (const std::exception& exception) { result.status = fail(Error::Busy, exception.what()); return result; }
}
Status finishSourceChange(NavigationPermit& permit) {
    const auto domain = CaptureAccess::permitDomain(permit);
    if (!domain) return {};
    std::uint64_t expected = navigationBit;
    if (!domain->fence.compare_exchange_strong(expected, navigationBit | closedBit, std::memory_order_acq_rel))
        return fail(Error::Busy, "source change refused: new producer input arrived before switching");
    return {};
}
bool mutationAllowed(const Store& store, std::string& error) {
    if (audio::onAudioThread) return false;
    const auto domain = DomainAccess::stored(store);
    if (!domain) return true;
    if (domain->closed || (domain->busy && !domain->journalAdmission) || domain->thread != std::this_thread::get_id()) {
        error = "journal mutation refused during gesture evaluation or source teardown"; return false;
    }
    return true;
}
void invalidateSource(Store& store) noexcept { invalidateDomain(DomainAccess::stored(store)); }

ProducerFence::Reservation ProducerFence::reserve() noexcept {
    if (!slot_) return Reservation::RejectedClosed;
    const auto result = detail::reserve(slot_->domain);
    if (result != Reservation::RejectedClosed) slot_->outstanding.fetch_add(1, std::memory_order_release);
    return result;
}
void ProducerFence::captureFault() noexcept { if (slot_) fault(slot_->domain, slot_->fault); }
void ProducerFence::publicationFailed() noexcept {
    if (!slot_) return;
    captureFault(); const auto before = slot_->outstanding.fetch_sub(1, std::memory_order_acq_rel);
    if (before != 0) release(slot_->domain, 1);
    else slot_->outstanding.fetch_add(1, std::memory_order_relaxed);
}
Status ProducerFence::acknowledge(const PendingCapture& capture, std::size_t count) {
    if (!slot_ || !matchingLease(capture, slot_->domain)) return fail(Error::TargetConflict, "capture acknowledgement needs its provisional source lease");
    return discardUnchanged(count);
}
Status ProducerFence::discardUnchanged(std::size_t count) {
    if (!slot_ || slot_->domain->thread != std::this_thread::get_id()) return fail(Error::WrongThread, "capture acknowledgement requires owner thread");
    const auto amount = static_cast<std::uint64_t>(count);
    if (amount > slot_->outstanding.load(std::memory_order_acquire)) return fail(Error::InvalidInput, "capture acknowledgement exceeds reservation");
    slot_->outstanding.fetch_sub(amount, std::memory_order_acq_rel); release(slot_->domain, amount); return {};
}
Status ProducerFence::acknowledgeLoss() {
    if (!slot_ || slot_->outstanding.load(std::memory_order_acquire) != 0) return fail(Error::Busy, "capture loss still has outstanding input");
    const auto domain = slot_->domain;
    if (domain->thread != std::this_thread::get_id()) return fail(Error::WrongThread, "capture recovery requires owner thread");
    recoverFault(domain, slot_->fault);
    return {};
}
FenceResult registerProducer(Store& store, std::size_t capacity) {
    if (audio::onAudioThread) return {{Error::WrongThread, {}}, {}};
    FenceResult result;
    try {
        const auto domain = domainFor(store); result.status = usable(domain);
        if (!result.status.ok()) return result;
        if (capacity >= countMask || domain->capacity > countMask - static_cast<std::uint64_t>(capacity) - 1) {
            result.status = fail(Error::CaptureOverflow, "producer capacities exceed fence representation"); return result;
        }
        result.fence = CaptureAccess::producer(domain); domain->capacity += static_cast<std::uint64_t>(capacity) + 1;
    } catch (const std::exception& exception) { result.status = fail(Error::CaptureOverflow, exception.what()); }
    return result;
}
FenceResult registerProducer(const AppliedSource& source, std::size_t capacity) {
    if (auto* store = adi::detail::appliedSourceStore(source)) return registerProducer(*store, capacity);
    return {fail(source.alive() ? Error::WrongThread : Error::SourceLost, "producer source is unavailable"), {}};
}
bool StateProducerFence::beginSignal() noexcept {
    if (!data_ || reserve(data_->domain) == ProducerFence::Reservation::RejectedClosed) return false;
    auto before = data_->reserved.load(std::memory_order_acquire);
    const auto consumed = data_->consumed.load(std::memory_order_acquire);
    if (before == std::numeric_limits<std::uint64_t>::max() || before - consumed >= data_->capacity ||
        !data_->reserved.compare_exchange_strong(before, before + 1, std::memory_order_acq_rel)) {
        fault(data_->domain, data_->fault); release(data_->domain, 1); return false;
    }
    return true;
}
void StateProducerFence::finishSignal() noexcept { if (data_) data_->published.fetch_add(1, std::memory_order_release); }
void StateProducerFence::captureFault() noexcept { if (data_) fault(data_->domain, data_->fault); }
StateProducerFence::Watermarks StateProducerFence::watermarks() const noexcept {
    if (!data_) return {0, 0, 0, false, true};
    const auto word = data_->domain->fence.load(std::memory_order_acquire);
    return {data_->reserved.load(std::memory_order_acquire), data_->published.load(std::memory_order_acquire),
        data_->consumed.load(std::memory_order_acquire), (word & faultBit) != 0, (word & closedBit) != 0};
}
Status StateProducerFence::acknowledgePrefix(const PendingCapture& capture, std::uint64_t prefix) {
    if (!data_ || !matchingLease(capture, data_->domain)) return fail(Error::TargetConflict, "state acknowledgement needs source lease");
    if (data_->domain->thread != std::this_thread::get_id()) return fail(Error::WrongThread, "state acknowledgement requires owner thread");
    const auto consumed = data_->consumed.load(std::memory_order_acquire);
    if (prefix < consumed || prefix > data_->published.load(std::memory_order_acquire)) return fail(Error::InvalidInput, "state prefix is not published");
    data_->consumed.store(prefix, std::memory_order_release); release(data_->domain, prefix - consumed); return {};
}
Status StateProducerFence::resetAfterDetach(const PendingCapture& capture) {
    const auto values = watermarks();
    if (values.reserved != values.published) return fail(Error::Busy, "state producer is not quiescent");
    const auto acknowledged = acknowledgePrefix(capture, values.published);
    if (!acknowledged.ok()) return acknowledged;
    recoverFault(data_->domain, data_->fault);
    return {};
}
StateFenceResult registerStateProducer(Store& store, std::size_t capacity) {
    if (audio::onAudioThread) return {{Error::WrongThread, {}}, {}};
    StateFenceResult result;
    try {
        const auto domain = domainFor(store); result.status = usable(domain);
        if (!result.status.ok()) return result;
        if (capacity == 0 || capacity >= countMask || domain->capacity > countMask - static_cast<std::uint64_t>(capacity)) {
            result.status = fail(Error::CaptureOverflow, "state producer capacity is invalid"); return result;
        }
        result.fence = CaptureAccess::stateProducer(domain, static_cast<std::uint64_t>(capacity)); domain->capacity += static_cast<std::uint64_t>(capacity);
    } catch (const std::exception& exception) { result.status = fail(Error::CaptureOverflow, exception.what()); }
    return result;
}
StateFenceResult registerStateProducer(const AppliedSource& source, std::size_t capacity) {
    if (auto* store = adi::detail::appliedSourceStore(source)) return registerStateProducer(*store, capacity);
    return {fail(source.alive() ? Error::WrongThread : Error::SourceLost, "state producer source is unavailable"), {}};
}

namespace {
class RollbackScope {
public:
    explicit RollbackScope(Store& store) : db_(store.db()) {
        if (sqlite3_get_autocommit(db_.getHandle()) == 0) throw std::runtime_error("gesture needs its own SQL transaction");
        db_.exec("BEGIN"); active_ = true;
    }
    ~RollbackScope() { if (active_) { try { db_.exec("ROLLBACK"); } catch (...) {} } }
    void rollback() { if (active_) { db_.exec("ROLLBACK"); active_ = false; } }
private:
    SQLite::Database& db_; bool active_ = false;
};
struct BusyScope {
    Domain& domain;
    explicit BusyScope(Domain& value) : domain(value) { domain.busy = true; }
    ~BusyScope() { domain.busy = false; domain.journalAdmission = false; }
};
bool sameIdentity(const std::weak_ptr<const void>& a, const std::weak_ptr<const void>& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}
bool equalClaim(const TargetClaim& a, const TargetClaim& b) {
    return a.kind == b.kind && a.domain == b.domain && a.id == b.id && a.endpoint == b.endpoint && sameIdentity(a.instance, b.instance);
}
bool conflicts(const TargetClaim& a, const TargetClaim& b) {
    if (a.kind == ClaimKind::Structural && b.kind == ClaimKind::Structural) return true;
    if (a.domain != b.domain || a.id != b.id) return false;
    if (a.domain == TargetDomain::Device && !sameIdentity(a.instance, b.instance)) return false;
    return a.kind == ClaimKind::DeviceInstance || b.kind == ClaimKind::DeviceInstance || a.endpoint == b.endpoint;
}
bool ownsConflict(const std::shared_ptr<Domain>& domain, const std::shared_ptr<Entry>& entry) {
    for (const auto& other : domain->entries) {
        // A later captured input retains its lease, but cannot deadlock the
        // earlier reserved owner whose completion lets this FIFO advance.
        if (other == entry) break;
        if (other == entry || terminal(other->state) ||
            (other->offered && other->offered->phase() == CapturedSubmissionPhase::Durable)) continue;
        for (const auto& ours : entry->claims)
            for (const auto& theirs : other->claims) if (conflicts(ours, theirs)) return true;
    }
    return false;
}
Status validateClaims(const std::vector<TargetClaim>& claims) {
    for (std::size_t i = 0; i < claims.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (equalClaim(claims[i], claims[j])) return fail(Error::InvalidInput, "duplicate gesture target claim");
    return {};
}
OpTransactionInputs inputsFor(const Proposal& proposal) {
    return {proposal.stateBlobs, proposal.rowGuards, proposal.rowSetGuards, proposal.presentationImage};
}
void stamp(Proposal& proposal, const Spec& spec) {
    for (std::size_t i = 0; i < proposal.requests.size(); ++i) {
        auto& request = proposal.requests[i]; request.actor = spec.actor; request.actorDetail = spec.actorDetail;
        request.label = spec.label; request.selBefore = i == 0 ? spec.selBefore : std::nullopt;
        request.capturedTransaction.reset();
    }
}
Status guardCurrent(const std::shared_ptr<Domain>& domain, const Base& base, const Proposal& proposal) {
    if (!base.source.sameConnection(domain->source) || !sameBase(base, currentBase(domain)))
        return fail(Error::StaleBase, "gesture committed base changed");
    std::string error;
    if (!adi::detail::validateOpPresentationImage(*domain->store, proposal.presentationImage, error) ||
        !adi::detail::validateOpRowGuards(*domain->store, proposal.rowGuards, error) ||
        !adi::detail::validateOpRowSetGuards(*domain->store, proposal.rowSetGuards, error))
        return fail(Error::StaleBase, std::move(error));
    return {};
}
bool acceptedStillCurrent(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Domain>& domain) {
    if (!entry->preview || sqlite3_get_autocommit(domain->store->db().getHandle()) == 0) return false;
    try {
        RollbackScope read(*domain->store);
        const auto status = guardCurrent(domain, entry->preview->base(), CaptureAccess::proposal(*entry->preview));
        read.rollback(); return status.ok();
    } catch (...) { return false; }
}
Status presentationFailure(const PresentationStatus& status) {
    return fail(status.state == PresentationState::Pending ? Error::Busy : Error::PreparationFailed,
                status.message.empty() ? "gesture presentation is not ready" : status.message);
}
UpdateResult reject(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Domain>& domain, Status status) {
    entry->status = std::move(status);
    if (entry->preview && !acceptedStillCurrent(entry, domain)) {
        entry->state = State::Suspended; entry->preview.reset();
        CapturedTransactionAccess::supersede(entry->offered); entry->offered.reset();
        try {
            entry->base = currentBase(domain);
            entry->presentationStatus = entry->presentation->adoptCommitted(*domain->store, entry->base, nullptr);
            if (entry->presentationStatus.state == PresentationState::Current)
                entry->presentationStatus = entry->presentation->clear(entry->owner);
        } catch (const std::exception& exception) {
            entry->presentationStatus = {PresentationState::Refused, exception.what(), {}};
        }
    }
    return {entry->status, entry->preview};
}
Status screen(const Entry& entry, const Proposal& proposal) {
    static const std::set<std::string> live{
        "mixer.setVolume", "mixer.setPan", "mixer.setPanLeft", "mixer.setPanRight", "mixer.setPanMode",
        "mixer.setInputGain", "routing.setGain", "routing.setPan", "group.setSummingDrive", "track.setMute", "track.setSolo",
        "device.setParam", "device.loadState"};
    for (const auto& request : proposal.requests) {
        const auto* descriptor = OpRegistry::instance().find(request.opType);
        if (!descriptor || !descriptor->apply || !descriptor->buildInverse || descriptor->ephemeral)
            return fail(Error::InvalidOperation, "gesture operation has no reversible descriptor: " + request.opType);
        if (!validateSubmission(*descriptor, request.payload).empty()) return fail(Error::InvalidOperation, "gesture operation payload is invalid: " + request.opType);
        if (descriptor->scope != Scope::Edit || (descriptor->engineImpact != EngineImpact::None && descriptor->engineImpact != EngineImpact::Snapshot))
            return fail(Error::UnsupportedPreview, "gesture operation requires unsupported external/topology effects: " + request.opType);
        const bool device = request.opType == "device.setParam" || request.opType == "device.loadState";
        if (device && (entry.spec.strategy != Strategy::LiveValues || !entry.presentation->supportsDeviceValues()))
            return fail(Error::UnsupportedPreview, "device gesture needs its concrete ParamOps presentation");
        if (entry.spec.strategy == Strategy::LiveValues && !live.contains(request.opType))
            return fail(Error::UnsupportedPreview, "operation has no live value presentation: " + request.opType);
    }
    return {};
}
Status initializeOrigin(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Domain>& domain) {
    if (!entry->presentation || !entry->producer) return fail(Error::InvalidInput, "gesture requires a producer and presentation");
    if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0) return fail(Error::ActiveSqlTransaction, "gesture cannot capture inside caller transaction");
    try {
        entry->base = currentBase(domain);
        entry->presentationStatus = entry->presentation->adoptCommitted(*domain->store, entry->base, nullptr);
        if (entry->presentationStatus.state != PresentationState::Current) return presentationFailure(entry->presentationStatus);
        if (!entry->origin) {
            RollbackScope read(*domain->store); entry->base = currentBase(domain);
            auto origin = entry->producer->captureOrigin(*domain->store, entry->base);
            read.rollback();
            if (!origin.status.ok() || !origin.value) return origin.status.ok() ? fail(Error::CaptureFailed, "producer returned no immutable origin") : origin.status;
            auto claims = entry->producer->targetClaims(*origin.value);
            const auto valid = validateClaims(claims); if (!valid.ok()) return valid;
            if (entry->provisional) {
                if (claims.size() != entry->claims.size()) return fail(Error::TargetConflict, "pending capture target claims changed");
                for (const auto& candidate : claims)
                    if (std::none_of(entry->claims.begin(), entry->claims.end(), [&](const auto& reserved) { return equalClaim(candidate, reserved); }))
                        return fail(Error::TargetConflict, "pending capture target claims changed");
            } else entry->claims = std::move(claims);
            entry->origin = std::move(origin.value);
            entry->originBase.source = domain->source;
            if (!entry->callbackOrigin) { entry->originBase.headAtCapture.emplace(entry->base.head); entry->originBase.sequenceAtCapture = entry->base.sequence; }
        }
        if (ownsConflict(domain, entry)) return fail(Error::TargetConflict, "gesture target is already owned by another gesture");
        return {};
    } catch (const std::exception& exception) { return fail(Error::CaptureFailed, exception.what()); }
}
UpdateResult evaluate(const std::shared_ptr<Entry>& entry, const Input& input) {
    const auto domain = entry->domain.lock(); const auto admission = usable(domain);
    if (!admission.ok()) return {admission, entry->preview};
    if (terminal(entry->state) || entry->state == State::Cancelling ||
        (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable))
        return {fail(Error::Closed, "gesture is no longer editable"), entry->preview};
    if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0)
        return {fail(Error::ActiveSqlTransaction, "gesture cannot preview inside caller transaction"), entry->preview};
    BusyScope busy(*domain);
    try {
        if (!entry->origin) {
            const auto initialized = initializeOrigin(entry, domain);
            if (!initialized.ok()) { entry->state = State::Suspended; return reject(entry, domain, initialized); }
        }
        const Base base = currentBase(domain);
        if (!sameBase(entry->base, base) || entry->dirty || !entry->preview) {
            entry->presentationStatus = entry->presentation->adoptCommitted(*domain->store, base, nullptr);
            if (entry->presentationStatus.state != PresentationState::Current)
                return reject(entry, domain, presentationFailure(entry->presentationStatus));
        }
        if (ownsConflict(domain, entry)) return reject(entry, domain, fail(Error::TargetConflict, "gesture target is already owned"));
        auto prepared = entry->producer->prepare(*domain->store, base, *entry->origin, input);
        if (!prepared.status.ok()) return reject(entry, domain, prepared.status);
        stamp(prepared.value, entry->spec);
        const auto screened = screen(*entry, prepared.value);
        if (!screened.ok()) return reject(entry, domain, screened);
        auto resources = inputsFor(prepared.value);
        CaptureResult capture;
        {
            RollbackScope scratch(*domain->store);
            const auto guarded = guardCurrent(domain, base, prepared.value);
            if (!guarded.ok()) { scratch.rollback(); return reject(entry, domain, guarded); }
            std::string error;
            if (!adi::detail::installOpResources(*domain->store, prepared.value.requests, resources, error)) {
                scratch.rollback(); return reject(entry, domain, fail(Error::InvalidInput, std::move(error)));
            }
            OpContext context{*domain->store, domain->store->db()};
            for (const auto& request : prepared.value.requests) {
                const auto* descriptor = OpRegistry::instance().find(request.opType); Payload inverse;
                if (!descriptor->buildInverse(context, request.payload, inverse, error) || !descriptor->apply(context, request.payload, error)) {
                    scratch.rollback(); return reject(entry, domain, fail(Error::InvalidOperation, request.opType + ": " + error));
                }
            }
            freeze::validate(*domain->store);
            capture = entry->presentation->capture(*domain->store, base, entry->owner, entry->spec.strategy, prepared.value.requests);
            scratch.rollback();
        }
        if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0)
            return reject(entry, domain, fail(Error::RollbackFailed, "gesture scratch rollback did not complete"));
        if (!capture.status.ok() || !capture.value)
            return reject(entry, domain, capture.status.ok() ? fail(Error::CaptureFailed, "presentation returned no owned capture") : capture.status);
        if (capture.presentationImage) prepared.value.presentationImage = std::move(capture.presentationImage);
        auto ready = entry->presentation->prepare(std::move(capture.value));
        if (!ready.status.ok() || !ready.value)
            return reject(entry, domain, ready.status.ok() ? fail(Error::PreparationFailed, "presentation returned no prepared value") : ready.status);
        {
            RollbackScope read(*domain->store);
            const auto guarded = guardCurrent(domain, base, prepared.value);
            read.rollback(); if (!guarded.ok()) return reject(entry, domain, guarded);
        }
        if (entry->revision == std::numeric_limits<std::uint64_t>::max()) return reject(entry, domain, fail(Error::Busy, "gesture preview revision exhausted"));
        auto frame = CaptureAccess::preview(base, entry->revision + 1, std::move(prepared.value), std::move(capture.snapshot));
        entry->presentationStatus = entry->presentation->publish(std::move(ready.value));
        if (entry->presentationStatus.state != PresentationState::Current)
            return reject(entry, domain, presentationFailure(entry->presentationStatus));
        CapturedTransactionAccess::supersede(entry->offered); entry->offered.reset();
        entry->preview = std::move(frame); entry->base = base; ++entry->revision;
        entry->state = State::Previewed; entry->dirty = false; entry->status = {};
        return {{}, entry->preview};
    } catch (const std::exception& exception) {
        const auto code = sqlite3_get_autocommit(domain->store->db().getHandle()) == 0 ? Error::RollbackFailed : Error::PreparationFailed;
        return reject(entry, domain, fail(code, exception.what()));
    }
}
CancelOutcome cancelEntry(const std::shared_ptr<Entry>& entry) {
    const auto domain = entry->domain.lock();
    if (entry->state == State::Cancelled) return {{}, entry->presentationStatus};
    if (entry->state == State::SourceLost) return {fail(Error::SourceLost, "gesture source is closed"), entry->presentationStatus};
    if (entry->state == State::Committed || (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable))
        return {fail(Error::Closed, "a durable gesture cannot be cancelled"), entry->presentationStatus};
    const auto admission = usable(domain);
    if (!admission.ok()) return {admission, entry->presentationStatus};
    if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0)
        return {fail(Error::ActiveSqlTransaction, "gesture cancel requires committed SQL"), entry->presentationStatus};
    BusyScope busy(*domain); entry->state = State::Cancelling;
    CapturedTransactionAccess::cancel(entry->offered);
    try {
        entry->base = currentBase(domain);
        entry->presentationStatus = entry->presentation->adoptCommitted(*domain->store, entry->base, nullptr);
        if (entry->presentationStatus.state == PresentationState::Current)
            entry->presentationStatus = entry->presentation->clear(entry->owner);
        if (entry->presentationStatus.state == PresentationState::Current || entry->presentationStatus.state == PresentationState::Detached) {
            entry->state = State::Cancelled; entry->lease.reset(); entry->preview.reset(); entry->status = {};
        } else entry->status = presentationFailure(entry->presentationStatus);
    } catch (const std::exception& exception) {
        entry->status = fail(Error::PreparationFailed, exception.what()); entry->presentationStatus = {PresentationState::Refused, exception.what(), {}};
    }
    return {entry->status, entry->presentationStatus};
}
void abandon(const std::shared_ptr<Entry>& entry) noexcept {
    if (!entry || terminal(entry->state)) return;
    if (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable) return;
    entry->abandoned = true;
    try {
        const auto domain = entry->domain.lock();
        if (usable(domain).ok()) { (void)cancelEntry(entry); return; }
        entry->state = State::Cancelling;
    } catch (...) { entry->state = State::Cancelling; }
}
void invalidateDomain(const std::shared_ptr<Domain>& domain) noexcept {
    if (!domain || domain->closed) return;
    domain->closed = true; domain->fence.fetch_or(closedBit, std::memory_order_acq_rel);
    for (const auto& entry : domain->entries) {
        if (!terminal(entry->state)) {
            try { if (entry->presentation) entry->presentationStatus = entry->presentation->clear(entry->owner); }
            catch (...) { entry->presentationStatus.state = PresentationState::Refused; }
            CapturedTransactionAccess::cancel(entry->offered);
            entry->state = State::SourceLost; entry->preview.reset(); entry->lease.reset();
        }
    }
    domain->store = nullptr; domain->subscription.reset();
}
struct DomainObserver final : AppliedObserver {
    explicit DomainObserver(std::weak_ptr<Domain> domain) : domain_(std::move(domain)) {}
    void committed(std::shared_ptr<const AppliedBatch> batch) override {
        const auto domain = domain_.lock(); if (!domain || domain->closed) return;
        for (const auto& entry : domain->entries) {
            if (terminal(entry->state)) continue;
            const bool own = entry->offered && entry->offered->durableTxn() && !batch->operations().empty() &&
                *entry->offered->durableTxn() == batch->operations().front().txnId;
            if (!own) entry->dirty = true;
        }
        domain->notices.push_back(std::move(batch));
    }
    void sourceInvalidated(const AppliedSource&, AppliedSourceInvalidation) override {
        const auto domain = domain_.lock(); if (!domain || domain->closed) return;
        // Store lifecycle invokes invalidateSource before this notification.
        // Defensive fallback stays queue-only: no SQL/SDK/presentation work.
        domain->closed = true; domain->store = nullptr; domain->fence.fetch_or(closedBit, std::memory_order_release);
        for (const auto& entry : domain->entries) { entry->state = State::SourceLost; entry->lease.reset(); }
    }
private: std::weak_ptr<Domain> domain_;
};
void ensureCoordinator(const std::shared_ptr<Domain>& domain) {
    if (!domain->coordinator) domain->coordinator = CaptureAccess::coordinator(domain);
    if (!domain->observer) {
        domain->observer = std::make_shared<DomainObserver>(domain);
        auto subscribed = domain->source.observe(domain->observer);
        if (!subscribed.ok) throw std::runtime_error(subscribed.error);
        domain->subscription = std::move(subscribed.subscription);
    }
}
std::shared_ptr<Presentation> defaultPresentation(const std::shared_ptr<Domain>& domain) {
    if (domain->presentation) return domain->presentation;
    engine::Session* session = nullptr; engine::ProjectView* view = nullptr;
    for (const auto& weak : domain->sessions) {
        const auto endpoint = weak.lock(); if (!endpoint || !endpoint->get()) continue;
        if (session && session != endpoint->get()) return {};
        session = endpoint->get();
    }
    for (const auto& weak : domain->views) {
        const auto endpoint = weak.lock(); if (!endpoint || !endpoint->get()) continue;
        if (view && view != endpoint->get()) return {};
        view = endpoint->get();
    }
    if (!session || !view) return {};
    return std::make_shared<EnginePresentation>(*view, *session);
}
std::shared_ptr<Entry> makeEntry(const std::shared_ptr<Domain>& domain, Spec spec,
                                std::unique_ptr<Producer> producer, std::shared_ptr<Presentation> presentation) {
    auto entry = std::make_shared<Entry>(CaptureAccess::owner(domain)); entry->domain = domain;
    entry->spec = std::move(spec); entry->producer = std::move(producer); entry->presentation = std::move(presentation);
    entry->lease = CaptureAccess::lease(domain); entry->base = currentBase(domain); entry->originBase.source = domain->source;
    domain->entries.push_back(entry); return entry;
}
std::shared_ptr<const AppliedBatch> receipt(const std::shared_ptr<Domain>& domain, const Entry& entry) {
    if (!entry.offered || !entry.offered->durableTxn()) return {};
    for (auto it = domain->notices.rbegin(); it != domain->notices.rend(); ++it)
        if (!(*it)->operations().empty() && (*it)->operations().front().txnId == *entry.offered->durableTxn()) return *it;
    return {};
}
void finishDurable(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Domain>& domain) {
    entry->state = State::Committed; entry->lease.reset(); entry->dirty = false;
    try {
        entry->base = currentBase(domain);
        const auto applied = receipt(domain, *entry);
        entry->presentationStatus = applied ? entry->presentation->promote(*domain->store, entry->base, entry->owner, *applied)
                                           : entry->presentation->adoptCommitted(*domain->store, entry->base, nullptr);
        if (!applied && entry->presentationStatus.state == PresentationState::Current)
            entry->presentationStatus = entry->presentation->clear(entry->owner);
        entry->preview.reset(); entry->status = {};
    } catch (const std::exception& exception) {
        entry->presentationStatus = {PresentationState::Refused, exception.what(), {}};
        entry->status = {};
    }
}
} // namespace

PendingCapture::PendingCapture(std::shared_ptr<Data> data) : data_(std::move(data)) {}
PendingCapture::~PendingCapture() { if (data_ && data_->entry->provisional) abandon(data_->entry); }
PendingCapture::PendingCapture(PendingCapture&& other) noexcept : data_(std::move(other.data_)) {}
PendingCapture& PendingCapture::operator=(PendingCapture&& other) noexcept {
    if (this != &other) { if (data_ && data_->entry->provisional) abandon(data_->entry); data_ = std::move(other.data_); } return *this;
}
const OwnerToken& PendingCapture::owner() const noexcept { return data_->entry->owner; }
const AppliedSource& PendingCapture::source() const noexcept { return data_->entry->originBase.source; }
const Status& PendingCapture::status() const noexcept { return data_->entry->status; }
CoordinatorResult coordinatorFor(const AppliedSource& source) {
    if (audio::onAudioThread) return {{Error::WrongThread, {}}, {}};
    try {
        auto* store = adi::detail::appliedSourceStore(source);
        if (!store) return {fail(source.alive() ? Error::WrongThread : Error::SourceLost, "gesture source is unavailable"), {}};
        const auto domain = domainFor(*store); const auto admission = usable(domain);
        if (!admission.ok()) return {admission, {}};
        ensureCoordinator(domain); return {{}, domain->coordinator};
    } catch (const std::exception& exception) { return {fail(Error::CaptureFailed, exception.what()), {}}; }
}
Status registerSessionEndpoint(const AppliedSource& source, std::shared_ptr<const engine::SessionEndpoint> endpoint) {
    const auto coordinator = coordinatorFor(source); if (!coordinator.status.ok()) return coordinator.status;
    const auto domain = CaptureAccess::domain(*coordinator.value);
    std::erase_if(domain->sessions, [](const auto& weak) { const auto value = weak.lock(); return !value || !value->get(); });
    for (const auto& weak : domain->sessions) if (weak.lock() == endpoint) return {};
    if (!endpoint || !endpoint->get()) return fail(Error::Closed, "Session endpoint is revoked");
    domain->sessions.push_back(std::move(endpoint)); return {};
}
Status registerProjectViewEndpoint(const AppliedSource& source, std::shared_ptr<const engine::ProjectViewEndpoint> endpoint) {
    const auto coordinator = coordinatorFor(source); if (!coordinator.status.ok()) return coordinator.status;
    const auto domain = CaptureAccess::domain(*coordinator.value);
    std::erase_if(domain->views, [](const auto& weak) { const auto value = weak.lock(); return !value || !value->get(); });
    for (const auto& weak : domain->views) if (weak.lock() == endpoint) return {};
    if (!endpoint || !endpoint->get()) return fail(Error::Closed, "ProjectView endpoint is revoked");
    domain->views.push_back(std::move(endpoint)); return {};
}
PendingCaptureResult reservePending(Coordinator& coordinator, std::vector<TargetClaim> claims,
    std::shared_ptr<Presentation> presentation, Spec spec, std::unique_ptr<Producer> producer,
    std::unique_ptr<const Input> input) {
    const auto domain = CaptureAccess::domain(coordinator); const auto admission = usable(domain);
    if (!admission.ok()) return {admission, {}};
    const auto valid = validateClaims(claims); if (!valid.ok()) return {valid, {}};
    if (!presentation || !producer || !input) return {fail(Error::InvalidInput, "pending capture requires complete owned input"), {}};
    try {
        auto entry = makeEntry(domain, std::move(spec), std::move(producer), std::move(presentation));
        entry->claims = std::move(claims); entry->input = std::move(input); entry->provisional = true; entry->callbackOrigin = true;
        if (ownsConflict(domain, entry)) { entry->state = State::Suspended; entry->status = fail(Error::TargetConflict, "pending capture target is already owned"); }
        return {entry->status, CaptureAccess::pending(entry)};
    } catch (const std::exception& exception) { return {fail(Error::CaptureFailed, exception.what()), {}}; }
}
BeginResult adoptPending(Coordinator& coordinator, PendingCapture& capture) {
    const auto domain = CaptureAccess::domain(coordinator); const auto admission = usable(domain);
    if (!admission.ok()) return {admission, {}};
    const auto entry = CaptureAccess::entry(capture);
    if (entry->domain.lock() != domain || !entry->provisional || !entry->lease.alive()) return {fail(Error::TargetConflict, "pending capture does not belong to source"), {}};
    {
        BusyScope busy(*domain); entry->status = initializeOrigin(entry, domain);
    }
    if (!entry->status.ok()) { entry->state = State::Suspended; return {entry->status, {}}; }
    auto session = CaptureAccess::session(entry);
    entry->provisional = false;
    auto updated = evaluate(entry, *entry->input);
    return {updated.status, std::move(session)};
}
CapturedOffer offerCaptured(Session& session) {
    const auto entry = CaptureAccess::entry(session); const auto domain = entry->domain.lock(); const auto admission = usable(domain);
    if (!admission.ok()) return {admission, {}, {}};
    if (!entry->preview || !entry->input || terminal(entry->state) || entry->state == State::Cancelling)
        return {fail(Error::Closed, "gesture has no accepted offer"), {}, {}};
    if (entry->offered) {
        if (entry->offered->phase() != CapturedSubmissionPhase::Offered)
            return {fail(Error::CommitRefused, "captured submission requires explicit retry"), {}, entry->offered};
        if (!acceptedStillCurrent(entry, domain)) return {fail(Error::StaleBase, "captured offer base changed; retry is required"), {}, entry->offered};
    } else {
        if (!acceptedStillCurrent(entry, domain)) return {fail(Error::StaleBase, "gesture preview base changed"), {}, {}};
        const auto& proposal = CaptureAccess::proposal(*entry->preview);
        if (proposal.requests.empty()) return {{}, {}, {}};
        entry->offered = CapturedTransactionAccess::seal(domain->source, entry->preview->base().head, proposal.requests, inputsFor(proposal));
    }
    std::vector<OpRequest> requests(entry->offered->requests().begin(), entry->offered->requests().end());
    for (auto& request : requests) request.capturedTransaction = entry->offered;
    return {{}, std::move(requests), entry->offered};
}
CapturedOffer retryCaptured(Session& session) {
    const auto entry = CaptureAccess::entry(session); const auto domain = entry->domain.lock(); const auto admission = usable(domain);
    if (!admission.ok()) return {admission, {}, {}};
    if (!entry->input || terminal(entry->state) || entry->state == State::Cancelling ||
        (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable)) return {fail(Error::Closed, "gesture cannot be retried"), {}, entry->offered};
    CapturedTransactionAccess::supersede(entry->offered); entry->offered.reset();
    const auto updated = evaluate(entry, *entry->input);
    if (!updated.status.ok()) return {updated.status, {}, {}};
    return offerCaptured(session);
}
} // namespace detail

Session::Session(std::unique_ptr<Data> data) : data_(std::move(data)) {}
Session::~Session() { if (data_) detail::abandon(data_->entry); }
State Session::state() const noexcept {
    if (data_->entry->offered && data_->entry->offered->phase() == CapturedSubmissionPhase::Durable) return State::Committed;
    return data_->entry->state;
}
const OriginBase& Session::originBase() const noexcept { return data_->entry->originBase; }
const Base& Session::currentBase() const noexcept { return data_->entry->base; }
std::shared_ptr<const Preview> Session::preview() const noexcept { return data_->entry->preview; }
const Status& Session::lastStatus() const noexcept { return data_->entry->status; }
UpdateResult Session::update(std::unique_ptr<const Input> input) {
    if (!input) {
        const auto entry = data_->entry; const auto domain = entry->domain.lock();
        const auto admission = detail::usable(domain);
        if (!admission.ok()) return {admission, entry->preview};
        if (detail::terminal(entry->state) || entry->state == State::Cancelling)
            return {{Error::Closed, "gesture is no longer editable"}, entry->preview};
        if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0)
            return {{Error::ActiveSqlTransaction, "gesture cannot inspect caller transaction"}, entry->preview};
        detail::BusyScope busy(*domain);
        return detail::reject(entry, domain, {Error::InvalidInput, "gesture input is empty"});
    }
    auto result = detail::evaluate(data_->entry, *input);
    if (result.status.ok()) data_->entry->input = std::move(input);
    return result;
}
UpdateResult Session::reapply() {
    if (!data_->entry->input) return {{Error::InvalidInput, "gesture has no accepted input"}, data_->entry->preview};
    return detail::evaluate(data_->entry, *data_->entry->input);
}
CancelOutcome Session::cancel() { return detail::cancelEntry(data_->entry); }
CancelOutcome Session::retryCancel() {
    if (data_->entry->state != State::Cancelling) return {{Error::Closed, "gesture is not cancelling"}, data_->entry->presentationStatus};
    return detail::cancelEntry(data_->entry);
}
CommitOutcome Session::commit() {
    CommitOutcome result; const auto entry = data_->entry; const auto domain = entry->domain.lock();
    result.status = detail::usable(domain); if (!result.status.ok()) return result;
    if (detail::terminal(entry->state) || entry->state == State::Cancelling ||
        (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable)) {
        result.status = {Error::Closed, "gesture cannot be committed again"}; return result;
    }
    if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0) {
        result.status = {Error::ActiveSqlTransaction, "gesture commit needs committed SQL"}; return result;
    }
    if (entry->input && !detail::acceptedStillCurrent(entry, domain)) {
        const auto updated = detail::evaluate(entry, *entry->input);
        if (!updated.status.ok()) { result.status = updated.status; return result; }
    }
    if (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Refused) {
        auto offered = detail::retryCaptured(*this);
        if (!offered.status.ok()) { result.status = offered.status; return result; }
    }
    try {
        CommitOptions options; options.expectHead.emplace(entry->preview ? entry->preview->base().head : entry->base.head);
        if (!entry->preview || entry->preview->requests().empty()) {
            const OpTransactionInputs inputs = entry->preview ? detail::inputsFor(detail::CaptureAccess::proposal(*entry->preview)) : OpTransactionInputs{};
            detail::BusyScope busy(*domain); domain->journalAdmission = true;
            result.durable = OpJournal(*domain->store).commitCaptured({}, domain->source, options, inputs);
            domain->journalAdmission = false;
            if (result.durable->ok) {
                result.semanticNoOp = true; entry->state = State::Committed; entry->lease.reset();
                entry->presentationStatus = entry->presentation->clear(entry->owner); entry->preview.reset();
            }
        } else {
            auto offered = detail::offerCaptured(*this);
            if (!offered.status.ok()) { result.status = offered.status; return result; }
            detail::BusyScope busy(*domain); domain->journalAdmission = true; entry->state = State::Committing;
            result.durable = OpJournal(*domain->store).commit(offered.requests);
            domain->journalAdmission = false;
            if (result.durable->ok) detail::finishDurable(entry, domain);
            else entry->state = State::Suspended;
        }
        if (result.durable && !result.durable->ok) result.status = {Error::CommitRefused, result.durable->error};
        result.presentation = entry->presentationStatus; entry->status = result.status; return result;
    } catch (const std::exception& exception) {
        if (result.durable && result.durable->ok) {
            entry->state = State::Committed; entry->lease.reset();
            result.presentation = {PresentationState::Refused, exception.what(), {}};
        } else { result.status = {Error::CommitRefused, exception.what()}; entry->state = State::Suspended; }
        return result;
    }
}

Coordinator::Coordinator(std::unique_ptr<Data> data) : data_(std::move(data)) {}
Coordinator::~Coordinator() = default;
CoordinatorResult Coordinator::attach(Store& store, std::shared_ptr<Presentation> presentation) {
    auto result = detail::coordinatorFor(appliedSource(store));
    if (result.status.ok() && presentation) detail::CaptureAccess::domain(*result.value)->presentation = std::move(presentation);
    return result;
}
BeginResult Coordinator::begin(Spec spec, std::unique_ptr<Producer> producer, std::shared_ptr<Presentation> presentation) {
    const auto domain = data_->domain.lock(); const auto admission = detail::usable(domain);
    if (!admission.ok()) return {admission, {}};
    if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0) return {{Error::ActiveSqlTransaction, "gesture begin needs committed SQL"}, {}};
    if (!presentation) presentation = detail::defaultPresentation(domain);
    if (!presentation || !producer) return {{Error::InvalidInput, "gesture needs unambiguous live presentation and producer"}, {}};
    try {
        detail::BusyScope busy(*domain); auto entry = detail::makeEntry(domain, std::move(spec), std::move(producer), std::move(presentation));
        entry->status = detail::initializeOrigin(entry, domain);
        if (!entry->status.ok()) {
            const auto status = entry->status; entry->state = State::Cancelled; entry->lease.reset(); return {status, {}};
        }
        return {{}, detail::CaptureAccess::session(std::move(entry))};
    } catch (const std::exception& exception) { return {{Error::CaptureFailed, exception.what()}, {}}; }
}
std::size_t Coordinator::pendingRestorations() const noexcept {
    const auto domain = data_->domain.lock(); if (!domain) return 0;
    return static_cast<std::size_t>(std::count_if(domain->entries.begin(), domain->entries.end(), [](const auto& entry) { return entry->state == State::Cancelling; }));
}
DrainOutcome Coordinator::retryRestorations(std::int64_t nowMs) {
    (void)nowMs; DrainOutcome result; const auto domain = data_->domain.lock(); result.status = detail::usable(domain);
    if (!result.status.ok()) return result;
    for (const auto& entry : domain->entries) if (entry->state == State::Cancelling) {
        const auto cancelled = detail::cancelEntry(entry); if (!cancelled.status.ok()) result.status = cancelled.status;
    }
    result.pendingRestorations = pendingRestorations(); return result;
}
DrainOutcome Coordinator::drain(std::int64_t nowMs) {
    DrainOutcome result; const auto domain = data_->domain.lock(); result.status = detail::usable(domain);
    if (!result.status.ok()) return result;
    if (sqlite3_get_autocommit(domain->store->db().getHandle()) == 0) {
        result.status = {Error::ActiveSqlTransaction, "gesture drain requires committed SQL"}; return result;
    }
    if (domain->draining) { result.status = {Error::Reentrant, "recursive gesture coordinator drain"}; return result; }
    struct DrainScope { detail::Domain& domain; ~DrainScope() { domain.draining = false; } } scope{*domain}; domain->draining = true;
    (void)nowMs;
    for (const auto& entry : domain->entries) {
        if (entry->state == State::Cancelling) { (void)detail::cancelEntry(entry); continue; }
        if (detail::terminal(entry->state)) continue;
        if (entry->offered && entry->offered->phase() == CapturedSubmissionPhase::Durable) {
            detail::BusyScope busy(*domain); detail::finishDurable(entry, domain); continue;
        }
        if (entry->provisional || !entry->input) continue;
        if (entry->dirty || !entry->preview || !detail::acceptedStillCurrent(entry, domain)) {
            const auto updated = detail::evaluate(entry, *entry->input);
            if (updated.status.ok()) ++result.reapplied; else result.status = updated.status;
        }
    }
    bool keepNotices = false;
    // A completed owner no longer receives reapplication, but its concrete
    // native presentation still has to hear subsequent Undo/Redo and foreign
    // commits. Use the registered/default presentation, not a finished private
    // device presentation: ParamOps' receipt FIFO owns SDK delivery separately.
    if (!domain->notices.empty() && std::none_of(domain->entries.begin(), domain->entries.end(),
            [](const auto& entry) { return detail::terminal(entry->state) == false; })) {
        try {
            const auto presentation = detail::defaultPresentation(domain);
            if (presentation) {
                detail::BusyScope busy(*domain);
                const auto adopted = presentation->adoptCommitted(*domain->store, detail::currentBase(domain), domain->notices.back().get());
                if (adopted.state != PresentationState::Current && adopted.state != PresentationState::Detached) {
                    keepNotices = true;
                    result.status = detail::presentationFailure(adopted);
                }
            }
        } catch (const std::exception& exception) {
            keepNotices = true;
            result.status = {Error::PreparationFailed, exception.what()};
        }
    }
    if (!keepNotices) domain->notices.clear();
    std::erase_if(domain->entries, [](const auto& entry) { return detail::terminal(entry->state); });
    result.pendingRestorations = pendingRestorations(); return result;
}
} // namespace adi::gesture
