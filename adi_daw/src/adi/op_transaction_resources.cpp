// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/media/blake3.hpp"
#include "adi/engine/gesture_preview.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace adi {
struct OpCapturedTransaction::Data {
    AppliedSource source;
    std::optional<std::int64_t> head;
    OpTransactionInputs inputs;
    std::vector<OpRequest> requests;
    mutable CapturedSubmissionPhase phase = CapturedSubmissionPhase::Offered;
    mutable std::string refusal;
    mutable std::optional<std::int64_t> txn;
};
OpCapturedTransaction::OpCapturedTransaction(std::shared_ptr<const Data> data) : data_(std::move(data)) {}
const AppliedSource& OpCapturedTransaction::source() const noexcept { return data_->source; }
const std::optional<std::int64_t>& OpCapturedTransaction::expectedHead() const noexcept { return data_->head; }
const OpTransactionInputs& OpCapturedTransaction::inputs() const noexcept { return data_->inputs; }
std::span<const OpRequest> OpCapturedTransaction::requests() const noexcept { return data_->requests; }
CapturedSubmissionPhase OpCapturedTransaction::phase() const noexcept { return data_->phase; }
const std::string& OpCapturedTransaction::lastRefusal() const noexcept { return data_->refusal; }
std::optional<std::int64_t> OpCapturedTransaction::durableTxn() const noexcept { return data_->txn; }

std::shared_ptr<const OpCapturedTransaction> CapturedTransactionAccess::seal(AppliedSource source,
    std::optional<std::int64_t> head, std::vector<OpRequest> requests, OpTransactionInputs inputs) {
    auto data = std::make_shared<OpCapturedTransaction::Data>();
    data->source = std::move(source); data->head = head;
    data->inputs = std::move(inputs); data->requests = std::move(requests);
    for (auto& request : data->requests) request.capturedTransaction.reset();
    return std::shared_ptr<const OpCapturedTransaction>(new OpCapturedTransaction(std::move(data)));
}
bool CapturedTransactionAccess::begin(const std::shared_ptr<const OpCapturedTransaction>& token,
    std::span<const OpRequest> requests, const Store& store, const CommitOptions& options, std::string& error) {
    if (!token || token->phase() != CapturedSubmissionPhase::Offered) {
        error = "captured submission is no longer offered"; return false;
    }
    if (!token->source().alive() || !token->source().sameConnection(appliedSource(store))) {
        error = "captured submission source changed"; return false;
    }
    const auto expected = token->requests();
    if (requests.size() != expected.size() || requests.empty()) {
        error = "captured submission group is incomplete"; return false;
    }
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& a = requests[i]; const auto& b = expected[i];
        if (a.capturedTransaction != token || a.opType != b.opType ||
            !detail::sameSqlValue(a.payload, b.payload) || a.targetKind != b.targetKind ||
            a.targetId != b.targetId || a.label != b.label ||
            a.selBefore.has_value() != b.selBefore.has_value() ||
            (a.selBefore && !detail::sameSqlValue(*a.selBefore, *b.selBefore)) ||
            a.runtimeProof != b.runtimeProof || a.actor != requests.front().actor ||
            a.actorDetail != requests.front().actorDetail) {
            error = "captured submission group was changed"; return false;
        }
    }
    token->data_->phase = CapturedSubmissionPhase::Committing;
    if (options.beforeCommit || (options.expectHead && *options.expectHead != token->expectedHead())) {
        error = "captured submission options differ";
        refuse(token, error);
        return false;
    }
    return true;
}
void CapturedTransactionAccess::refuse(const std::shared_ptr<const OpCapturedTransaction>& token,
                                     const std::string& error) {
    if (token && token->data_->phase == CapturedSubmissionPhase::Committing) {
        token->data_->phase = CapturedSubmissionPhase::Refused;
        token->data_->refusal = error;
    }
}
void CapturedTransactionAccess::durable(const std::shared_ptr<const OpCapturedTransaction>& token,
                                      std::int64_t txn) noexcept {
    if (token) { token->data_->txn = txn; token->data_->phase = CapturedSubmissionPhase::Durable; }
}
void CapturedTransactionAccess::supersede(const std::shared_ptr<const OpCapturedTransaction>& token) noexcept {
    if (token && token->phase() != CapturedSubmissionPhase::Durable)
        token->data_->phase = CapturedSubmissionPhase::Superseded;
}
void CapturedTransactionAccess::cancel(const std::shared_ptr<const OpCapturedTransaction>& token) noexcept {
    if (token && token->phase() != CapturedSubmissionPhase::Durable)
        token->data_->phase = CapturedSubmissionPhase::Cancelled;
}

namespace detail {
bool sameSqlValue(const Payload& a, const Payload& b) noexcept {
    try {
        if (a.type() != b.type()) return false;
        if (a.is_number_float())
            return std::bit_cast<std::uint64_t>(a.get<double>()) == std::bit_cast<std::uint64_t>(b.get<double>());
        if (a.is_object()) {
            if (a.size() != b.size()) return false;
            for (auto it = a.begin(); it != a.end(); ++it)
                if (!b.contains(it.key()) || !sameSqlValue(it.value(), b.at(it.key()))) return false;
            return true;
        }
        if (a.is_array()) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) if (!sameSqlValue(a[i], b[i])) return false;
            return true;
        }
        return a == b;
    } catch (...) { return false; }
}
namespace {
std::string quote(const std::string& identifier) {
    std::string result = "\"";
    for (const char ch : identifier) { result += ch; if (ch == '"') result += ch; }
    return result + '"';
}
struct Shape { std::set<std::string> columns; std::vector<std::string> primary; };
Shape shape(const Store& store, const std::string& table) {
    static const std::set<std::string> tables{
        "project", "tracks", "mixer_strip", "mixer_pan_settings", "routing", "group_summing", "clips",
        "audio_data", "note_data", "note_expression", "clip_takes", "clip_fades",
        "clip_warp", "clip_warp_markers", "automation_lanes", "automation_data", "automation_segment_ranges",
        "devices", "device_chains", "plugin_refs", "plugin_params", "plugin_state",
        "tempo_map", "time_signature_map", "track_recording_options", "return_send_taps",
        "clip_properties", "clip_loop", "clip_loops", "midi_data", "takes", "layers"};
    if (!tables.contains(table)) throw std::runtime_error("unsupported transaction guard table: " + table);
    auto& db = const_cast<SQLite::Database&>(store.db());
    SQLite::Statement query(db, "PRAGMA table_info(" + quote(table) + ")");
    Shape result; std::map<int, std::string> primary;
    while (query.executeStep()) {
        const auto name = query.getColumn(1).getString(); result.columns.insert(name);
        const int ordinal = query.getColumn(5).getInt();
        if (ordinal > 0) primary.emplace(ordinal, name);
    }
    if (result.columns.empty() || primary.empty()) throw std::runtime_error("guard table has no supported primary key");
    for (const auto& [ordinal, name] : primary) { (void)ordinal; result.primary.push_back(name); }
    return result;
}
bool cell(const Payload& value) {
    if (value.is_number_integer()) { (void)signedInteger(value); return true; }
    return value.is_null() || value.is_string() || value.is_binary() ||
        (value.is_number_float() && !std::isnan(value.get<double>()));
}
void validObject(const Payload& object, const Shape& table, bool full) {
    if (!object.is_object() || (full && object.size() != table.columns.size()))
        throw std::runtime_error("invalid typed guard row");
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!table.columns.contains(it.key()) || !cell(it.value()))
            throw std::runtime_error("invalid typed guard cell");
}
void bind(SQLite::Statement& query, int index, const Payload& value) {
    if (value.is_null()) query.bind(index);
    else if (value.is_number_integer()) query.bind(index, signedInteger(value));
    else if (value.is_number_float()) query.bind(index, value.get<double>());
    else if (value.is_string()) query.bind(index, value.get<std::string>());
    else {
        const auto& bytes = value.get_binary(); static const char empty = 0;
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("guard blob exceeds SQLite length");
        query.bind(index, bytes.empty() ? static_cast<const void*>(&empty) : bytes.data(), static_cast<int>(bytes.size()));
    }
}
std::vector<Payload> readRows(const Store& store, const std::string& name,
                             const Payload& equal, const Shape& table, std::size_t limit) {
    validObject(equal, table, false);
    std::string sql = "SELECT * FROM " + quote(name);
    if (!equal.empty()) {
        sql += " WHERE "; bool first = true;
        for (auto it = equal.begin(); it != equal.end(); ++it) {
            if (!first) sql += " AND ";
            first = false;
            sql += quote(it.key()) + " IS ?";
        }
    }
    sql += " ORDER BY ";
    for (std::size_t i = 0; i < table.primary.size(); ++i) { if (i != 0) sql += ','; sql += quote(table.primary[i]); }
    SQLite::Statement query(const_cast<SQLite::Database&>(store.db()), sql);
    int index = 0; for (const auto& value : equal) bind(query, ++index, value);
    std::vector<Payload> result;
    while (query.executeStep()) {
        if (result.size() >= limit) throw std::runtime_error("guard row set exceeds capture limit");
        result.push_back(captureQueryRow(query));
    }
    return result;
}
} // namespace
std::vector<Payload> captureGuardRows(const Store& store, const std::string& table,
                                    const Payload& equal, std::size_t limit) {
    return readRows(store, table, equal, shape(store, table), limit);
}
bool validateOpRowGuards(const Store& store, std::span<const OpRowGuard> guards, std::string& error) {
    try {
        for (const auto& guard : guards) {
            const auto table = shape(store, guard.table); validObject(guard.primaryKey, table, false);
            if (guard.primaryKey.size() != table.primary.size()) throw std::runtime_error("guard needs complete primary key");
            for (const auto& key : table.primary)
                if (!guard.primaryKey.contains(key) || guard.primaryKey.at(key).is_null())
                    throw std::runtime_error("guard primary key is missing");
            if (guard.expectedRow) validObject(*guard.expectedRow, table, true);
            const auto actual = readRows(store, guard.table, guard.primaryKey, table, 2);
            if ((guard.expectedRow && (actual.size() != 1 || !sameSqlValue(actual.front(), *guard.expectedRow))) ||
                (!guard.expectedRow && !actual.empty())) throw std::runtime_error("stale target row: " + guard.table);
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool validateOpRowSetGuards(const Store& store, std::span<const OpRowSetGuard> guards, std::string& error) {
    try {
        for (const auto& guard : guards) {
            const auto table = shape(store, guard.table);
            for (const auto& row : guard.expectedRows) validObject(row, table, true);
            const auto actual = readRows(store, guard.table, guard.equalColumns, table, guard.expectedRows.size() + 1);
            if (actual.size() != guard.expectedRows.size()) throw std::runtime_error("stale target set: " + guard.table);
            for (std::size_t i = 0; i < actual.size(); ++i)
                if (!sameSqlValue(actual[i], guard.expectedRows[i])) throw std::runtime_error("stale target set: " + guard.table);
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool validateOpPresentationImage(const Store& store, const std::optional<OpPresentationImageGuard>& guard,
                                 std::string& error) {
    if (!guard) return true;
    try {
        if (guard->image != engine::capturePresentationImage(store)) {
            error = "stale presentation material or schema";
            return false;
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool installOpResources(Store& store, std::span<const OpRequest> requests,
                        const OpTransactionInputs& inputs, std::string& error) {
    try {
        std::set<std::string> referenced, seen;
        std::vector<const OpStateBlob*> missing;
        for (const auto& request : requests)
            if (request.opType == "device.loadState" && request.payload.contains("hash") && request.payload.at("hash").is_string())
                referenced.insert(request.payload.at("hash").get<std::string>());
        for (const auto& blob : inputs.stateBlobs) {
            if (blob.hash.size() != 64 || !std::all_of(blob.hash.begin(), blob.hash.end(), [](char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }) ||
                !seen.insert(blob.hash).second || !referenced.contains(blob.hash) ||
                blob.bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("invalid or unreferenced captured state resource");
            const auto hash = media::blake3Bytes(blob.bytes);
            if (!hash || hash.hex != blob.hash) throw std::runtime_error("captured state resource hash mismatch");
            SQLite::Statement query(store.db(), "SELECT data,size_bytes FROM state_blobs WHERE hash_blake3=?");
            query.bind(1, blob.hash);
            if (query.executeStep()) {
                const auto value = query.getColumn(0);
                const auto size = static_cast<std::size_t>(value.getBytes());
                if (!value.isBlob() || size != blob.bytes.size() || query.getColumn(1).getInt64() != static_cast<std::int64_t>(size) ||
                    (size != 0 && !std::equal(blob.bytes.begin(), blob.bytes.end(), static_cast<const std::byte*>(value.getBlob()))))
                    throw std::runtime_error("existing state resource differs from captured bytes");
            } else missing.push_back(&blob);
        }
        // Validate the complete bundle before its first write, so malformed
        // later resources cannot produce even rolled-back partial admission.
        for (const auto* blob : missing) {
            SQLite::Statement insert(store.db(), "INSERT INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
            static const char empty = 0; insert.bind(1, blob->hash);
            insert.bind(2, blob->bytes.empty() ? static_cast<const void*>(&empty) : blob->bytes.data(), static_cast<int>(blob->bytes.size()));
            insert.bind(3, static_cast<std::int64_t>(blob->bytes.size())); insert.exec();
        }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
} // namespace detail
} // namespace adi
