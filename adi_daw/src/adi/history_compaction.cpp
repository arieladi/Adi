// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history_compaction.hpp"
#include "adi/digest.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::history_detail {
namespace {
std::string sqlName(const std::string &name) {
    std::string out = "\"";
    for (char ch : name) {
        out += ch;
        if (ch == '"')
            out += '"';
    }
    return out + '"';
}
void validate(const UndoRetentionPolicy &policy) {
    if (policy.maxTransactions < 1 || policy.maxTransactions > 2000 || policy.maxBytes < 1 ||
        policy.maxBytes > 1000000000 || (policy.uploadedThroughSeq && *policy.uploadedThroughSeq < 0))
        throw std::runtime_error("invalid undo retention budget or upload acknowledgement");
}
std::optional<std::string> meta(SQLite::Database &db, const char *key) {
    SQLite::Statement row(db, "SELECT value FROM adi_meta WHERE key=?");
    row.bind(1, key);
    if (row.executeStep() && !row.getColumn(0).isNull())
        return row.getColumn(0).getString();
    return {};
}
std::int64_t integer(const std::string &text) {
    std::size_t used = 0;
    const auto value = std::stoll(text, &used);
    if (used != text.size())
        throw std::runtime_error("invalid undo policy integer");
    return value;
}
void setMeta(SQLite::Database &db, const char *key, const std::optional<std::string> &value) {
    SQLite::Statement row(
        db,
        "INSERT INTO adi_meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    row.bind(1, key);
    if (value)
        row.bind(2, *value);
    else
        row.bind(2);
    row.exec();
}
std::vector<std::string> columns(SQLite::Database &db, const std::string &table) {
    SQLite::Statement query(db, "PRAGMA table_info(" + sqlName(table) + ")");
    std::vector<std::string> result;
    while (query.executeStep())
        result.push_back(query.getColumn(1).getString());
    return result;
}
Payload cell(const SQLite::Column &value) {
    if (value.isNull())
        return Payload::array({"null", nullptr});
    if (value.isInteger())
        return Payload::array({"int", value.getInt64()});
    if (value.isFloat())
        return Payload::array({"real", value.getDouble()});
    if (value.isText())
        return Payload::array({"text", value.getString()});
    if (value.isBlob()) {
        std::vector<std::uint8_t> bytes;
        if (value.getBytes() > 0) {
            const auto *data = static_cast<const std::uint8_t *>(value.getBlob());
            bytes.assign(data, data + value.getBytes());
        }
        return Payload::array({"blob", Payload::binary(bytes)});
    }
    throw std::runtime_error("unsupported checkpoint cell");
}
void bindCell(SQLite::Statement &insert, int index, const Payload &value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_string())
        throw std::runtime_error("invalid checkpoint cell");
    const auto type = value[0].get<std::string>();
    const auto &data = value[1];
    if (type == "null" && data.is_null())
        insert.bind(index);
    else if (type == "int" && data.is_number_integer()) {
        if (data.is_number_unsigned() &&
            data.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("checkpoint integer overflow");
        insert.bind(index, data.get<std::int64_t>());
    } else if (type == "real" && data.is_number_float())
        insert.bind(index, data.get<double>());
    else if (type == "text" && data.is_string())
        insert.bind(index, data.get<std::string>());
    else if (type == "blob" && data.is_binary()) {
        const auto &bytes = data.get_binary();
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("checkpoint blob is too large");
        // A non-null address preserves a zero-length BLOB rather than SQL NULL.
        const std::uint8_t empty = 0;
        insert.bind(index, bytes.empty() ? &empty : bytes.data(), static_cast<int>(bytes.size()));
    } else
        throw std::runtime_error("invalid checkpoint cell type");
}
} // namespace
UndoRetentionPolicy undoPolicy(SQLite::Database &db) {
    UndoRetentionPolicy policy;
    if (const auto value = meta(db, "history.undo.max_transactions"))
        policy.maxTransactions = integer(*value);
    if (const auto value = meta(db, "history.undo.max_bytes"))
        policy.maxBytes = integer(*value);
    if (const auto value = meta(db, "history.undo.collaboration")) {
        if (*value != "0" && *value != "1")
            throw std::runtime_error("invalid collaboration retention mode");
        policy.collaboration = *value == "1";
    }
    if (const auto value = meta(db, "history.undo.uploaded_through"))
        policy.uploadedThroughSeq = integer(*value);
    validate(policy);
    return policy;
}
void writeUndoPolicy(SQLite::Database &db, const UndoRetentionPolicy &policy) {
    validate(policy);
    setMeta(db, "history.undo.max_transactions", std::to_string(policy.maxTransactions));
    setMeta(db, "history.undo.max_bytes", std::to_string(policy.maxBytes));
    setMeta(db, "history.undo.collaboration", policy.collaboration ? "1" : "0");
    setMeta(db, "history.undo.uploaded_through",
            policy.uploadedThroughSeq ? std::optional<std::string>(std::to_string(*policy.uploadedThroughSeq))
                                      : std::nullopt);
}
Payload checkpointState(Store &store) {
    Payload result = {{"format", 1},
                      {"schema_major", store.schemaMajor()},
                      {"schema_minor", store.schemaMinor()},
                      {"tables", Payload::array()}};
    for (const auto &table : digestedTables(store)) {
        const auto names = columns(store.db(), table);
        Payload rows = Payload::array();
        SQLite::Statement query(store.db(), "SELECT * FROM " + sqlName(table));
        while (query.executeStep()) {
            Payload values = Payload::array();
            for (std::size_t i = 0; i < names.size(); ++i)
                values.push_back(cell(query.getColumn(static_cast<int>(i))));
            rows.push_back(std::move(values));
        }
        result["tables"].push_back({{"name", table}, {"columns", names}, {"rows", std::move(rows)}});
    }
    return result;
}
void installCheckpoint(Store &target, const Payload &state) {
    if (state.at("format") != 1 || state.at("schema_major") != target.schemaMajor() ||
        state.at("schema_minor").get<int>() > target.schemaMinor())
        throw std::runtime_error("unsupported checkpoint version");
    const auto allowed = digestedTables(target);
    std::set<std::string> seen;
    target.db().exec("PRAGMA defer_foreign_keys=ON");
    for (const auto &table : allowed)
        target.db().exec("DELETE FROM " + sqlName(table));
    for (const auto &table : state.at("tables")) {
        const auto name = table.at("name").get<std::string>();
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end() || !seen.insert(name).second)
            throw std::runtime_error("invalid checkpoint table");
        const auto names = table.at("columns").get<std::vector<std::string>>();
        const auto current = columns(target.db(), name);
        std::set<std::string> used;
        std::string fields, parameters;
        for (const auto &field : names) {
            if (std::find(current.begin(), current.end(), field) == current.end() ||
                !used.insert(field).second)
                throw std::runtime_error("invalid checkpoint column");
            if (!fields.empty()) {
                fields += ',';
                parameters += ',';
            }
            fields += sqlName(field);
            parameters += '?';
        }
        if (fields.empty())
            throw std::runtime_error("checkpoint table has no columns");
        SQLite::Statement insert(target.db(), "INSERT INTO " + sqlName(name) + "(" + fields + ") VALUES(" +
                                                  parameters + ")");
        for (const auto &row : table.at("rows")) {
            if (!row.is_array() || row.size() != names.size())
                throw std::runtime_error("invalid checkpoint row");
            insert.reset();
            insert.clearBindings();
            for (std::size_t i = 0; i < row.size(); ++i)
                bindCell(insert, static_cast<int>(i) + 1, row[i]);
            insert.exec();
        }
    }
    // All tables from the writer's schema are captured, even when empty.
    if (state.at("schema_minor").get<int>() == target.schemaMinor() && seen.size() != allowed.size())
        throw std::runtime_error("checkpoint omits project tables");
    SQLite::Statement foreignKeys(target.db(), "PRAGMA foreign_key_check");
    if (foreignKeys.executeStep())
        throw std::runtime_error("checkpoint has dangling foreign keys");
}
UndoCompactionResult compactUndo(Store &store, const UndoRetentionPolicy &policy) {
    validate(policy);
    UndoCompactionResult result;
    auto &db = store.db();
    std::int64_t base = 0;
    {
        SQLite::Statement query(db, "SELECT base_seq FROM history_checkpoint WHERE id=1");
        if (query.executeStep())
            base = query.getColumn(0).getInt64();
    }
    result.baseSeq = base;
    struct Row {
        std::int64_t seq, txn, bytes;
        std::optional<std::int64_t> parent;
    };
    const std::string byteSize =
        "COALESCE(length(payload),0)+COALESCE(length(inverse),0)+COALESCE(length(sel_before),0)+length(CAST("
        "actor AS BLOB))+length(CAST(actor_detail AS BLOB))+length(CAST(op_type AS "
        "BLOB))+length(CAST(target_kind AS BLOB))+length(CAST(label AS BLOB))+length(CAST(tags AS BLOB))+80";
    SQLite::Statement size(db, "SELECT COUNT(DISTINCT txn_id),COALESCE(SUM(" + byteSize +
                                   "),0) FROM ops WHERE tags<>'ephemeral' AND seq>?");
    size.bind(1, base);
    size.executeStep();
    result.transactions = size.getColumn(0).getInt64();
    result.bytes = size.getColumn(1).getInt64();
    const auto over = [&] {
        return result.transactions > policy.maxTransactions || result.bytes > policy.maxBytes;
    };
    if (!over()) {
        result.ok = true;
        return result;
    }
    // Old 1.0 files can still contain embedded media. Current schema triggers
    // deliberately forbid reinserting those rows, so preserve their log until
    // the separate extraction path has removed the legacy content.
    if (db.execAndGet("SELECT COUNT(*) FROM media_files WHERE embedded<>0").getInt64() != 0 ||
        db.execAndGet("SELECT COUNT(*) FROM media_blobs").getInt64() != 0) {
        result.ok = true;
        result.protectedHistory = true;
        return result;
    }
    std::map<std::int64_t, Row> rows;
    SQLite::Statement query(db, "SELECT seq,txn_id,parent_seq," + byteSize +
                                    " FROM ops WHERE tags<>'ephemeral' AND seq>? ORDER BY seq");
    query.bind(1, base);
    while (query.executeStep()) {
        Row row{
            query.getColumn(0).getInt64(), query.getColumn(1).getInt64(), query.getColumn(3).getInt64(), {}};
        if (!query.getColumn(2).isNull())
            row.parent = query.getColumn(2).getInt64();
        rows.emplace(row.seq, row);
    }
    const auto head = OpJournal(store).headSeq();
    std::int64_t protection = std::numeric_limits<std::int64_t>::max();
    SQLite::Statement anchors(db, "SELECT op_seq FROM history_snapshots UNION ALL SELECT head_seq FROM "
                                  "op_branches UNION ALL SELECT parent_seq FROM ops WHERE tags<>'ephemeral' "
                                  "AND parent_seq IS NOT NULL GROUP BY parent_seq HAVING COUNT(*)>1");
    while (anchors.executeStep())
        protection = std::min(protection, anchors.getColumn(0).isNull() ? std::int64_t(0)
                                                                        : anchors.getColumn(0).getInt64());
    if (policy.collaboration)
        protection = std::min(protection, policy.uploadedThroughSeq.value_or(0));
    std::vector<Row> chain;
    auto cursor = head;
    std::set<std::int64_t> visited;
    while (cursor && *cursor > base) {
        if (!visited.insert(*cursor).second || !rows.contains(*cursor))
            throw std::runtime_error("invalid undo parent chain");
        const auto &row = rows.at(*cursor);
        chain.push_back(row);
        cursor = row.parent;
    }
    if (base && cursor != std::optional<std::int64_t>(base))
        throw std::runtime_error("undo chain does not reach its checkpoint");
    std::reverse(chain.begin(), chain.end());
    std::int64_t candidate = base;
    for (std::size_t i = 0; i < chain.size() && over();) {
        const auto txn = chain[i].txn;
        std::int64_t bytes = 0;
        std::size_t end = i;
        while (end < chain.size() && chain[end].txn == txn) {
            bytes += chain[end].bytes;
            ++end;
        }
        const auto seq = chain[end - 1].seq;
        // Keep the newest complete transaction, and never cross any anchor.
        if (seq > protection || end == chain.size())
            break;
        candidate = seq;
        --result.transactions;
        result.bytes -= bytes;
        i = end;
    }
    result.protectedHistory = over();
    if (candidate == base) {
        result.ok = true;
        return result;
    }
    const auto state = encodePayload(checkpointAt(store, candidate));
    if (state.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("checkpoint exceeds SQLite blob limit");
    SQLite::Statement checkpoint(
        db, "INSERT INTO history_checkpoint(id,base_seq,state) VALUES(1,?,?) ON CONFLICT(id) DO UPDATE SET "
            "base_seq=excluded.base_seq,state=excluded.state");
    checkpoint.bind(1, candidate);
    checkpoint.bind(2, state.data(), static_cast<int>(state.size()));
    checkpoint.exec();
    SQLite::Statement root(db, "UPDATE ops SET parent_seq=NULL,inverse=NULL,sel_before=NULL WHERE seq=?");
    root.bind(1, candidate);
    root.exec();
    SQLite::Statement prune(db, "DELETE FROM ops WHERE seq<?");
    prune.bind(1, candidate);
    result.removedOps = prune.exec();
    db.exec("DELETE FROM agent_requests WHERE NOT EXISTS(SELECT 1 FROM ops WHERE "
            "ops.txn_id=agent_requests.txn_id)");
    result.baseSeq = candidate;
    result.ok = true;
    return result;
}
} // namespace adi::history_detail
