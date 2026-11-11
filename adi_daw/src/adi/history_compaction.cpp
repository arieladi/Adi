// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history_compaction.hpp"
#include "adi/digest.hpp"
#include "adi/history_policy.hpp"
#include "adi/history_roots.hpp"
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
void installCheckpoint(Store &target, const Payload &state, bool preserveContentPools) {
    if (state.at("format") != 1 || state.at("schema_major") != target.schemaMajor() ||
        state.at("schema_minor").get<int>() > target.schemaMinor())
        throw std::runtime_error("unsupported checkpoint version");
    const auto allowed = digestedTables(target);
    std::set<std::string> seen;
    target.db().exec("PRAGMA defer_foreign_keys=ON");
    for (const auto &table : allowed)
        if (!preserveContentPools || table != "state_blobs")
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
        const bool mergePool = preserveContentPools && name == "state_blobs";
        SQLite::Statement insert(
            target.db(), std::string(mergePool ? "INSERT OR IGNORE INTO " : "INSERT INTO ") +
                             sqlName(name) + "(" + fields + ") VALUES(" + parameters + ")");
        for (const auto &row : table.at("rows")) {
            if (!row.is_array() || row.size() != names.size())
                throw std::runtime_error("invalid checkpoint row");
            insert.reset();
            insert.clearBindings();
            for (std::size_t i = 0; i < row.size(); ++i)
                bindCell(insert, static_cast<int>(i) + 1, row[i]);
            insert.exec();
            if (mergePool) {
                const auto key = std::find(names.begin(), names.end(), "hash_blake3");
                if (key == names.end())
                    throw std::runtime_error("checkpoint pool lacks identity");
                SQLite::Statement existing(
                    target.db(), "SELECT " + fields + " FROM state_blobs WHERE hash_blake3=?");
                bindCell(existing, 1, row[static_cast<std::size_t>(key - names.begin())]);
                if (!existing.executeStep())
                    throw std::runtime_error("checkpoint pool insertion failed");
                for (std::size_t i = 0; i < row.size(); ++i)
                    if (cell(existing.getColumn(static_cast<int>(i))) != row[i])
                        throw std::runtime_error("checkpoint pool content collision");
            }
        }
    }
    // All tables from the writer's schema are captured, even when empty.
    if (state.at("schema_minor").get<int>() == target.schemaMinor() && seen.size() != allowed.size())
        throw std::runtime_error("checkpoint omits project tables");
    SQLite::Statement foreignKeys(target.db(), "PRAGMA foreign_key_check");
    if (foreignKeys.executeStep())
        throw std::runtime_error("checkpoint has dangling foreign keys");
}
void copyCheckpointPools(const Store &source, Store &target) {
    auto &db = const_cast<SQLite::Database &>(source.db());
    SQLite::Statement rows(db, "SELECT hash_blake3,data,size_bytes FROM state_blobs");
    SQLite::Statement insert(
        target.db(),
        "INSERT OR IGNORE INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
    while (rows.executeStep()) {
        insert.reset();
        insert.clearBindings();
        for (int i = 0; i < 3; ++i)
            bindCell(insert, i + 1, cell(rows.getColumn(i)));
        insert.exec();
        SQLite::Statement check(target.db(),
                                "SELECT data,size_bytes FROM state_blobs WHERE hash_blake3=?");
        check.bind(1, rows.getColumn(0).getString());
        if (!check.executeStep() || cell(check.getColumn(0)) != cell(rows.getColumn(1)) ||
            cell(check.getColumn(1)) != cell(rows.getColumn(2)))
            throw std::runtime_error("replay pool content collision");
    }
}
UndoCompactionResult compactUndo(Store &store, const UndoRetentionPolicy &policy) {
    validate(policy);
    return compactStoredUndo(store, policy);
}
UndoCompactionResult tryCompactUndo(Store& store) {
    auto& db=store.db();UndoCompactionResult result;
    bool active=false;
    try{
        db.exec("SAVEPOINT adi_optional_compaction");active=true;
        result=compactUndo(store,undoPolicy(db));
        if(!result.ok){db.exec("ROLLBACK TO adi_optional_compaction");}
        db.exec("RELEASE adi_optional_compaction");active=false;
    }catch(const std::exception& error){
        if(active){db.exec("ROLLBACK TO adi_optional_compaction");db.exec("RELEASE adi_optional_compaction");}
        result={};result.error=error.what();
    }
    return result;
}
} // namespace adi::history_detail
