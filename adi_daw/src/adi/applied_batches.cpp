// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/applied_batches_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <thread>

namespace adi::detail {

struct AppliedIdentity {
    std::string file;
    std::weak_ptr<const AppliedIdentity> predecessor;
};

struct AppliedObserverSlot {
    std::weak_ptr<AppliedObserver> observer;
    AppliedSubscriptionState state = AppliedSubscriptionState::Active;
    std::uint64_t delivered = 0;
    std::uint64_t failures = 0;
    std::string error;
};

struct AppliedDomain {
    Store* store = nullptr;
    std::thread::id ownerThread;
    std::shared_ptr<const AppliedIdentity> identity;
    std::vector<std::weak_ptr<AppliedObserverSlot>> slots;
    std::uint64_t sequence = 0;
    bool live = true;
    bool dispatching = false;
};

struct AppliedBatchData {
    AppliedBatchKind kind = AppliedBatchKind::Commit;
    AppliedSource source;
    std::uint64_t sequence = 0;
    std::optional<std::int64_t> before, after;
    std::vector<AppliedOperation> operations;
    std::vector<AppliedStateBlob> blobs;
    std::vector<AppliedDeviceTarget> targets;
    std::vector<AppliedDeviceParameters> parameters;
};

struct AppliedSourceAccess {
    static std::shared_ptr<AppliedDomain> domain(const AppliedSource& source) noexcept {
        return source.domain_.lock();
    }
    static const std::shared_ptr<AppliedDomain>& domain(const Store& store) noexcept {
        return store.appliedDomain_;
    }
    static AppliedSource source(const std::shared_ptr<AppliedDomain>& domain) {
        AppliedSource result;
        if (domain) {
            result.identity_ = domain->identity;
            result.domain_ = domain;
        }
        return result;
    }
    static AppliedSubscription subscription(std::shared_ptr<AppliedObserverSlot> slot) {
        AppliedSubscription result;
        result.slot_ = std::move(slot);
        return result;
    }
};

struct AppliedBatchAccess {
    static std::shared_ptr<const AppliedBatch> seal(AppliedBatchData data) {
        return std::shared_ptr<const AppliedBatch>(
            new AppliedBatch(std::make_shared<const AppliedBatchData>(std::move(data))));
    }
};

namespace {

void fault(AppliedObserverSlot& slot, const char* message) noexcept {
    slot.state = AppliedSubscriptionState::ObserverFailed;
    if (slot.failures != std::numeric_limits<std::uint64_t>::max()) {
        ++slot.failures;
    }
    // Capacity is reserved at subscription. Failure reporting after COMMIT
    // must not allocate, propagate or change the durable transaction result.
    std::size_t size = 0;
    while (size < slot.error.capacity() && message[size] != '\0') {
        ++size;
    }
    try {
        slot.error.assign(message, size);
    } catch (...) {
        slot.error.clear();
    }
}

std::int64_t deviceId(const Payload& payload) {
    if (!payload.is_object() || !payload.contains("dev")) {
        throw std::runtime_error("applied device packet has no device identity");
    }
    return signedInteger(payload.at("dev"));
}

} // namespace

std::shared_ptr<AppliedDomain> makeAppliedDomain(Store& store) {
    auto identity = std::make_shared<AppliedIdentity>();
    const auto file = store.path().u8string();
    identity->file.assign(reinterpret_cast<const char*>(file.data()), file.size());
    auto result = std::make_shared<AppliedDomain>();
    result->identity = std::move(identity);
    result->store = &store;
    result->ownerThread = std::this_thread::get_id();
    return result;
}

Store* appliedSourceStore(const AppliedSource& source) noexcept {
    const auto domain = AppliedSourceAccess::domain(source);
    return domain && domain->live && domain->ownerThread == std::this_thread::get_id() ? domain->store : nullptr;
}
void rebindAppliedDomain(Store& store) noexcept {
    const auto& domain = AppliedSourceAccess::domain(store);
    if (domain) domain->store = &store;
}

void prepareAppliedReplacement(Store& replacement, const Store& previous) {
    const auto& next = AppliedSourceAccess::domain(replacement);
    const auto& before = AppliedSourceAccess::domain(previous);
    if (!next || !before || !next->live || !before->live || next == before) {
        throw std::runtime_error("invalid applied source replacement");
    }
    next->identity = std::make_shared<const AppliedIdentity>(
        AppliedIdentity{next->identity->file, before->identity});
}

bool appliedDispatchActive(const Store& store) noexcept {
    const auto& domain = AppliedSourceAccess::domain(store);
    return domain && domain->dispatching;
}

void invalidateAppliedDomain(Store& store, AppliedSourceInvalidation reason) noexcept {
    const auto domain = AppliedSourceAccess::domain(store);
    if (!domain || !domain->live) {
        return;
    }
    domain->live = false;
    domain->store = nullptr;
    domain->dispatching = true;
    const auto source = AppliedSourceAccess::source(domain);
    // Observe is refused once live is false. reset only marks/removes the
    // subscriber's own strong slot, so this weak-slot vector stays stable.
    for (const auto& weak : domain->slots) {
        const auto slot = weak.lock();
        if (!slot || slot->state == AppliedSubscriptionState::Inactive
            || slot->state == AppliedSubscriptionState::SourceInvalidated) {
            continue;
        }
        const auto observer = slot->observer.lock();
        slot->state = AppliedSubscriptionState::SourceInvalidated;
        if (!observer) {
            continue;
        }
        try {
            observer->sourceInvalidated(source, reason);
        } catch (const std::exception& exception) {
            fault(*slot, exception.what());
            slot->state = AppliedSubscriptionState::SourceInvalidated;
        } catch (...) {
            fault(*slot, "applied observer threw during source invalidation");
            slot->state = AppliedSubscriptionState::SourceInvalidated;
        }
    }
    domain->dispatching = false;
}

struct PreparedAppliedDelivery::Data {
    std::shared_ptr<AppliedDomain> domain;
    std::shared_ptr<const AppliedBatch> batch;
    std::vector<std::shared_ptr<AppliedObserverSlot>> slots;
    bool published = false;
};

PreparedAppliedDelivery::PreparedAppliedDelivery() = default;
PreparedAppliedDelivery::~PreparedAppliedDelivery() = default;
PreparedAppliedDelivery::PreparedAppliedDelivery(PreparedAppliedDelivery&&) noexcept = default;
PreparedAppliedDelivery& PreparedAppliedDelivery::operator=(PreparedAppliedDelivery&&) noexcept = default;

void PreparedAppliedDelivery::publish() noexcept {
    if (!data_ || data_->published) {
        return;
    }
    data_->published = true;
    const auto& domain = data_->domain;
    if (!domain || !domain->live || domain->dispatching) {
        return;
    }
    domain->sequence = data_->batch->sequence();
    domain->dispatching = true;
    for (const auto& slot : data_->slots) {
        if (slot->state != AppliedSubscriptionState::Active) {
            continue;
        }
        const auto observer = slot->observer.lock();
        if (!observer) {
            slot->state = AppliedSubscriptionState::ObserverExpired;
            continue;
        }
        try {
            observer->committed(data_->batch);
            // Self-revocation is respected; last delivery remains observable
            // only while the caller retains its still-live subscription slot.
            slot->delivered = data_->batch->sequence();
        } catch (const std::exception& exception) {
            if (slot->state != AppliedSubscriptionState::Inactive) {
                fault(*slot, exception.what());
            }
        } catch (...) {
            if (slot->state != AppliedSubscriptionState::Inactive) {
                fault(*slot, "applied observer threw");
            }
        }
    }
    domain->dispatching = false;
}

struct AppliedBatchBuilder::Data {
    std::shared_ptr<AppliedDomain> domain;
    AppliedBatchData batch;
    std::map<std::string, std::vector<std::byte>> blobs;
    std::set<std::int64_t> targets, stateTargets;
    bool finished = false;
};

AppliedBatchBuilder::AppliedBatchBuilder(const Store& store, AppliedBatchKind kind,
                                       std::optional<std::int64_t> beforeHead)
    : data_(std::make_unique<Data>()) {
    data_->domain = AppliedSourceAccess::domain(store);
    if (!data_->domain || !data_->domain->live || data_->domain->dispatching) {
        throw std::runtime_error("applied batch source is closed or delivering observers");
    }
    data_->batch.kind = kind;
    data_->batch.source = AppliedSourceAccess::source(data_->domain);
    data_->batch.before = beforeHead;
}

AppliedBatchBuilder::~AppliedBatchBuilder() = default;

bool AppliedBatchBuilder::append(const Store& store, std::string_view recordedType,
                                std::string_view dispatchedType, const Payload& payload,
                                std::int64_t seq, std::int64_t txnId, bool inverse,
                                std::string& error, std::shared_ptr<const void> runtimeProof) {
    error.clear();
    try {
        if (data_->finished || AppliedSourceAccess::domain(store) != data_->domain
            || !data_->domain->live) {
            throw std::runtime_error("applied batch source changed during collection");
        }
        if (seq <= 0 || txnId <= 0 || recordedType.empty() || dispatchedType.empty()) {
            throw std::runtime_error("applied operation has invalid original log context");
        }
        if (dispatchedType == "device.setParam" || dispatchedType == "device.loadState") {
            const auto id = deviceId(payload);
            data_->targets.insert(id);
            if (dispatchedType == "device.loadState") {
                data_->stateTargets.insert(id);
                if (payload.contains("hash") && !payload.at("hash").is_null()) {
                    if (!payload.at("hash").is_string()) {
                        throw std::runtime_error("applied state hash is not text");
                    }
                    const auto& hash = payload.at("hash").get_ref<const std::string&>();
                    if (!data_->blobs.contains(hash)) {
                        auto bytes = store.getStateBlob(hash);
                        if (!bytes) {
                            throw std::runtime_error("cannot retain applied state blob " + hash);
                        }
                        data_->blobs.emplace(hash, std::move(*bytes));
                    }
                }
            }
        }
        AppliedOperation operation;
        operation.recordedType = recordedType;
        operation.dispatchedType = dispatchedType;
        operation.payload = payload;
        operation.seq = seq;
        operation.txnId = txnId;
        operation.inverse = inverse;
        if (data_->batch.kind == AppliedBatchKind::Commit && !inverse) {
            operation.runtimeProof = std::move(runtimeProof);
        }
        data_->batch.operations.push_back(std::move(operation));
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::optional<PreparedAppliedDelivery> AppliedBatchBuilder::finish(
    const Store& store, std::optional<std::int64_t> afterHead, std::string& error) {
    error.clear();
    try {
        if (data_->finished || AppliedSourceAccess::domain(store) != data_->domain
            || !data_->domain->live || data_->domain->dispatching) {
            throw std::runtime_error("applied batch source changed before commit");
        }
        if (data_->domain->sequence == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("applied batch sequence exhausted");
        }
        auto& db = const_cast<SQLite::Database&>(store.db());
        for (const auto id : data_->targets) {
            AppliedDeviceTarget target;
            target.deviceId = id;
            target.device = captureRow(db, "devices", "id", id);
            if (target.device && target.device->contains("plugin_ref_id")
                && !target.device->at("plugin_ref_id").is_null()) {
                target.pluginRef = captureRow(db, "plugin_refs", "id",
                                             signedInteger(target.device->at("plugin_ref_id")));
            }
            data_->batch.targets.push_back(std::move(target));
        }
        for (const auto id : data_->stateTargets) {
            AppliedDeviceParameters parameters;
            parameters.deviceId = id;
            const auto target = std::lower_bound(data_->batch.targets.begin(), data_->batch.targets.end(), id,
                [](const AppliedDeviceTarget& value, std::int64_t wanted) {
                    return value.deviceId < wanted;
                });
            parameters.devicePresent = target != data_->batch.targets.end()
                                       && target->deviceId == id && target->device.has_value();
            SQLite::Statement rows(db, "SELECT * FROM plugin_params WHERE device_id=? ORDER BY param_id");
            rows.bind(1, id);
            while (rows.executeStep()) {
                parameters.rows.push_back(captureQueryRow(rows));
            }
            data_->batch.parameters.push_back(std::move(parameters));
        }
        for (auto& [hash, bytes] : data_->blobs) {
            data_->batch.blobs.push_back({hash, std::move(bytes)});
        }
        data_->batch.after = afterHead;
        data_->batch.sequence = data_->domain->sequence + 1;
        PreparedAppliedDelivery result;
        result.data_ = std::make_unique<PreparedAppliedDelivery::Data>();
        result.data_->domain = data_->domain;
        for (const auto& weak : data_->domain->slots) {
            auto slot = weak.lock();
            if (slot && slot->state == AppliedSubscriptionState::Active) {
                result.data_->slots.push_back(std::move(slot));
            }
        }
        result.data_->batch = AppliedBatchAccess::seal(std::move(data_->batch));
        data_->finished = true;
        return result;
    } catch (const std::exception& exception) {
        error = exception.what();
        return std::nullopt;
    }
}

} // namespace adi::detail

namespace adi {
namespace {
const std::string& emptyAppliedText() noexcept {
    static const std::string empty;
    return empty;
}
} // namespace

AppliedSource appliedSource(const Store& store) {
    return detail::AppliedSourceAccess::source(detail::AppliedSourceAccess::domain(store));
}

bool AppliedSource::alive() const noexcept {
    const auto domain = domain_.lock();
    return domain && domain->live;
}
bool AppliedSource::sameConnection(const AppliedSource& other) const noexcept {
    return identity_ && identity_ == other.identity_;
}
bool AppliedSource::isReplacementOf(const AppliedSource& previous) const noexcept {
    return identity_ && previous.identity_ && identity_ != previous.identity_
        && identity_->predecessor.lock() == previous.identity_;
}
const std::string& AppliedSource::sourceFile() const noexcept {
    return identity_ ? identity_->file : emptyAppliedText();
}
std::uint64_t AppliedSource::sequence() const noexcept {
    const auto domain = domain_.lock();
    return domain ? domain->sequence : 0;
}

AppliedObserveResult AppliedSource::observe(const std::shared_ptr<AppliedObserver>& observer) const {
    AppliedObserveResult result;
    const auto domain = domain_.lock();
    if (!domain || !domain->live || !observer) {
        result.error = "cannot observe an expired source or empty observer";
        return result;
    }
    auto slot = std::make_shared<detail::AppliedObserverSlot>();
    slot->observer = observer;
    slot->error.reserve(1024);
    std::erase_if(domain->slots, [](const auto& weak) { return weak.expired(); });
    domain->slots.push_back(slot);
    result.subscription = detail::AppliedSourceAccess::subscription(std::move(slot));
    result.ok = true;
    return result;
}

AppliedBatch::AppliedBatch(std::shared_ptr<const detail::AppliedBatchData> data) noexcept
    : data_(std::move(data)) {}
AppliedBatchKind AppliedBatch::kind() const noexcept { return data_->kind; }
const AppliedSource& AppliedBatch::source() const noexcept { return data_->source; }
std::uint64_t AppliedBatch::sequence() const noexcept { return data_->sequence; }
std::optional<std::int64_t> AppliedBatch::beforeHead() const noexcept { return data_->before; }
std::optional<std::int64_t> AppliedBatch::afterHead() const noexcept { return data_->after; }
std::span<const AppliedOperation> AppliedBatch::operations() const noexcept { return data_->operations; }
std::span<const AppliedStateBlob> AppliedBatch::stateBlobs() const noexcept { return data_->blobs; }
const AppliedStateBlob* AppliedBatch::stateBlob(std::string_view hash) const noexcept {
    const auto found = std::lower_bound(data_->blobs.begin(), data_->blobs.end(), hash,
        [](const AppliedStateBlob& blob, std::string_view wanted) { return blob.hash < wanted; });
    return found != data_->blobs.end() && found->hash == hash ? &*found : nullptr;
}
std::span<const AppliedDeviceTarget> AppliedBatch::deviceTargets() const noexcept { return data_->targets; }
std::span<const AppliedDeviceParameters> AppliedBatch::finalParameters() const noexcept { return data_->parameters; }

AppliedSubscription::~AppliedSubscription() { reset(); }
AppliedSubscription::AppliedSubscription(AppliedSubscription&& other) noexcept
    : slot_(std::move(other.slot_)) {}
AppliedSubscription& AppliedSubscription::operator=(AppliedSubscription&& other) noexcept {
    if (this != &other) {
        reset();
        slot_ = std::move(other.slot_);
    }
    return *this;
}
void AppliedSubscription::reset() noexcept {
    if (slot_) {
        slot_->state = AppliedSubscriptionState::Inactive;
        slot_.reset();
    }
}
AppliedSubscriptionState AppliedSubscription::state() const noexcept {
    return slot_ ? slot_->state : AppliedSubscriptionState::Inactive;
}
bool AppliedSubscription::active() const noexcept { return state() == AppliedSubscriptionState::Active; }
std::uint64_t AppliedSubscription::lastDeliveredSequence() const noexcept { return slot_ ? slot_->delivered : 0; }
std::uint64_t AppliedSubscription::failureCount() const noexcept { return slot_ ? slot_->failures : 0; }
const std::string& AppliedSubscription::error() const noexcept { return slot_ ? slot_->error : emptyAppliedText(); }

} // namespace adi
