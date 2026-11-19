// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace adi {
class AppliedSource;
struct OpStateBlob {
    std::string hash;
    std::vector<std::byte> bytes;
};
/// Complete primary-key address plus exact captured row or absence. SQL cells
/// use row_capture's native Payload representation (integer/real/text/binary/
/// null), not stringified SQL or tagged wrappers. The full current row, including
/// future columns/storage classes, must compare exactly inside the transaction.
/// Identifier/table/complete-primary-key admission is validated centrally;
/// values are bound, never concatenated into executable SQL.
struct OpRowGuard {
    std::string table;
    Payload primaryKey;
    std::optional<Payload> expectedRow;
};
/// Complete bounded equality-selected set, including an empty set. Required
/// whenever preparation read a set (for example plugin_params by device or
/// automation_data by lane/clip): per-PK guards cannot detect inserted rows.
/// Only centrally admitted project tables/equality-column selectors are allowed;
/// no SQL text, predicates or callbacks. An empty selector means the explicitly
/// admitted whole table (for fixed-topology definitions). Expected rows are full
/// native typed rows in primary-key order; duplicates/malformed rows refuse.
/// During validation, one extra actual row already proves mismatch, bounding
/// materialization by expected size+1. Full schema/row shape also compares.
struct OpRowSetGuard {
    std::string table;
    Payload equalColumns;
    std::vector<Payload> expectedRows;
};
/// Native presentation's owned typed committed material/schema image. The
/// fixed engine capture routine defines coverage; no caller SQL or callback.
struct OpPresentationImageGuard { std::vector<std::uint8_t> image; };
struct OpTransactionInputs {
    std::vector<OpStateBlob> stateBlobs;
    std::vector<OpRowGuard> rowGuards;
    std::vector<OpRowSetGuard> rowSetGuards;
    std::optional<OpPresentationImageGuard> presentationImage{};
};

/// Sealed transient transaction admission carried by OpRequest::capturedTransaction.
/// This field is never payload/CBOR/SQL/history and is empty on History replay.
enum class CapturedSubmissionPhase {
    Offered, Committing, Refused, Durable, Superseded, Cancelled, Invalidated
};
class OpCapturedTransaction {
public:
    [[nodiscard]] const AppliedSource& source() const noexcept;
    [[nodiscard]] const std::optional<std::int64_t>& expectedHead() const noexcept;
    [[nodiscard]] const OpTransactionInputs& inputs() const noexcept;
    /// Stored packet excludes its own capturedTransaction field to avoid cycles.
    [[nodiscard]] std::span<const OpRequest> requests() const noexcept;
    [[nodiscard]] CapturedSubmissionPhase phase() const noexcept;
    [[nodiscard]] const std::string& lastRefusal() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> durableTxn() const noexcept;
private:
    struct Data;
    explicit OpCapturedTransaction(std::shared_ptr<const Data>);
    std::shared_ptr<const Data> data_;
    friend struct CapturedTransactionAccess;
};

/// Ordinary OpJournal::commit sees a capturedTransaction on ANY request and
/// requires ONE identical nonempty capability on EVERY request, exact count/
/// order/type/payload/target/label/selection, and consistent permitted actor/
/// detail stamps (legacy OpSubmitter may supply these). No mixed raw suffixes,
/// partial groups, deduplication or silent capture downgrade. It delegates the
/// same transactional guards/resources path as commitCaptured below.
///
/// The capability holds owned fixed core submission state, never
/// arbitrary code/SQL callbacks. Journal admission sets that state Committing
/// for own-receipt suppression; valid rollback becomes Refused; durable success is
/// recorded without allocating after COMMIT, independently of observer outcome.
/// Successful packet cannot be retried as a second mutation. Presentation work
/// remains outside journal/dispatch. Source coordinator retains pending input.
/// Offered may be reoffered if a caller dropped the vector; Refused requires
/// explicit retry/reprepare. Rebase supersedes the old capability; stale external
/// copies refuse. Invalid/tampered submission must not poison an authentic offer.

/// Existing commit overloads retain compatibility. commitCaptured requires
/// options.expectHead to be engaged (inner NULL is a valid root). Source and
/// head and ALL row guards are checked INSIDE its transaction before resource/
/// control writes. Mismatched guard is a named stale-target refusal. A same-head
/// external row edit between prepare and journal therefore cannot be overwritten.
/// Empty requests still check both and finish without journal/client/fork/
/// resource writes or an N47 event. Empty requests with state blobs refuse;
/// row/set guards on empty requests are still required and evaluated.
///
/// Before mutation: validate closed schemas; reject duplicate hashes, malformed
/// hashes, mismatched BLAKE3 content, unrepresentable SQLite byte lengths, or
/// resources not referenced by a submitted nonclear device.loadState packet.
/// Inside transaction before inverse/apply: existing blobs must have exact data
/// and size; missing blobs are inserted. A later failure rolls insertion back.
/// Row equality compares SQLite storage kind and exact typed contents (including
/// REAL representation), never nlohmann's cross-kind numeric equality alone.
/// No arbitrary callback can be added to a sealed group: beforeCommit refuses
/// that authentic offer. Ordinary unsealed commits retain their existing hook.
namespace detail {
/// Shared by real journal and rollback preview while caller owns transaction.
/// Performs resource validation/insertion only; no journal/notification writes.
bool installOpResources(Store&, std::span<const OpRequest>,
                        const OpTransactionInputs&, std::string& error);
bool validateOpRowGuards(const Store&, std::span<const OpRowGuard>, std::string& error);
bool validateOpRowSetGuards(const Store&, std::span<const OpRowSetGuard>, std::string& error);
bool validateOpPresentationImage(const Store&, const std::optional<OpPresentationImageGuard>&,
                                 std::string& error);
}
} // namespace adi
