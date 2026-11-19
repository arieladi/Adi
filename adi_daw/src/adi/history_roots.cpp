// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history_roots.hpp"
#include "adi/history_compaction.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <limits>
#include <stdexcept>
namespace adi {
std::span<const OpDescriptor> historyRootOps() {
    static constexpr Field fields[] = {{"state_hash", FieldType::Text, true}};
    static constexpr OpDescriptor descriptors[] = {
        {"history.root", "Stored history baseline (internal metadata)", Scope::Read,
         EngineImpact::None, fields, false, false, nullptr, nullptr, ""}};
    return descriptors;
}
namespace history_detail {
bool hasStoredRoots(SQLite::Database &db) {
    return db.execAndGet("SELECT EXISTS(SELECT 1 FROM history_roots)").getInt() != 0;
}
std::optional<std::string> storedRootHash(SQLite::Database &db, std::int64_t seq) {
    SQLite::Statement row(db, "SELECT state_hash FROM history_roots WHERE seq=?");
    row.bind(1, seq);
    if (row.executeStep())
        return row.getColumn(0).getString();
    return {};
}
std::optional<std::int64_t> originRoot(SQLite::Database &db) {
    SQLite::Statement row(db, "SELECT root_seq FROM history_origin_root WHERE id=1");
    if (row.executeStep())
        return row.getColumn(0).getInt64();
    return {};
}
std::int64_t rootForPoint(SQLite::Database &db, std::optional<std::int64_t> seq) {
    if (!hasStoredRoots(db))
        return 0;
    if (!seq) {
        const auto origin = originRoot(db);
        if (!origin)
            throw std::runtime_error("stored history has no origin");
        return *origin;
    }
    // UNION deduplicates cycles. The single terminal must be a mapped root;
    // missing parents and cycles without a terminal cannot look like a baseline.
    SQLite::Statement query(db,
                            "WITH RECURSIVE chain(seq,parent) AS ("
                            "SELECT seq,parent_seq FROM ops WHERE seq=? UNION SELECT "
                            "o.seq,o.parent_seq "
                            "FROM ops o JOIN chain c ON o.seq=c.parent) "
                            "SELECT c.seq,c.parent,r.state_hash,o.op_type,o.payload,o.inverse "
                            "FROM chain c JOIN ops o ON o.seq=c.seq LEFT JOIN history_roots r ON "
                            "r.seq=c.seq "
                            "WHERE c.parent IS NULL OR r.seq IS NOT NULL");
    query.bind(1, *seq);
    if (!query.executeStep() || !query.getColumn(1).isNull() || query.getColumn(2).isNull())
        throw std::runtime_error("history line has no valid stored baseline");
    const auto root = query.getColumn(0).getInt64();
    const auto col = query.getColumn(4);
    const auto *data = static_cast<const std::byte *>(col.getBlob());
    std::string error;
    const auto payload = decodeFromLog(std::vector<std::byte>(data, data + col.getBytes()), error);
    if (query.getColumn(3).getString() != "history.root" || !query.getColumn(5).isNull() ||
        !payload || *payload != Payload{{"state_hash", query.getColumn(2).getString()}})
        throw std::runtime_error("invalid stored baseline metadata");
    if (query.executeStep())
        throw std::runtime_error("history line crosses multiple stored baselines");
    return root;
}
std::int64_t materializeRoot(Store &store, std::optional<std::int64_t> point,
                             const std::string &hash) {
    if (point && *point <= 0)
        throw std::runtime_error("invalid stored baseline sequence");
    auto &db = store.db();
    if (point) {
        if (const auto existing = storedRootHash(db, *point)) {
            if (*existing != hash)
                throw std::runtime_error("stored baseline identity changed");
            return *point;
        }
        SQLite::Statement check(db, "SELECT op_type FROM ops WHERE seq=?");
        check.bind(1, *point);
        if (!check.executeStep() || check.getColumn(0).getString() == "history.root")
            throw std::runtime_error("invalid baseline boundary");
    }
    const auto bytes = encodePayload({{"state_hash", hash}});
    std::int64_t seq = point.value_or(0);
    if (point) {
        SQLite::Statement update(db,
                                 "UPDATE ops SET "
                                 "parent_seq=NULL,inverse=NULL,sel_before=NULL,op_type='history."
                                 "root',payload=?,tags='history-root' WHERE seq=?");
        update.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        update.bind(2, seq);
        update.exec();
    } else {
        const auto maximum = db.execAndGet("SELECT COALESCE(MAX(txn_id),0) FROM ops").getInt64();
        if (maximum == std::numeric_limits<std::int64_t>::max())
            throw std::runtime_error("history transaction identity exhausted");
        SQLite::Statement insert(
            db, "INSERT INTO "
                "ops(txn_id,ts_utc,actor,actor_detail,op_type,payload,label,tags) "
                "VALUES(?,0,'migration','stored baseline','history.root',?,'Stored "
                "history state','history-root')");
        insert.bind(1, maximum + 1);
        insert.bind(2, bytes.data(), static_cast<int>(bytes.size()));
        insert.exec();
        seq = db.getLastInsertRowid();
        SQLite::Statement client(db,
                                 "INSERT OR IGNORE INTO op_clients(client_id,label,first_seen_utc) "
                                 "VALUES(?,?,0)");
        client.bind(1, store.clientId());
        client.bind(2, store.clientLabel());
        client.exec();
        const auto clock = db.execAndGet("SELECT MAX(COALESCE((SELECT MAX(lamport) FROM "
                                         "op_clocks),0),COALESCE((SELECT MAX(seq) FROM ops),0))")
                               .getInt64();
        if (clock == std::numeric_limits<std::int64_t>::max())
            throw std::runtime_error("history clock exhausted");
        SQLite::Statement stamp(db, "INSERT INTO op_clocks(seq,client_id,lamport) VALUES(?,?,?)");
        stamp.bind(1, seq);
        stamp.bind(2, store.clientId());
        stamp.bind(3, clock + 1);
        stamp.exec();
    }
    SQLite::Statement root(db, "INSERT INTO history_roots(seq,state_hash) VALUES(?,?)");
    root.bind(1, seq);
    root.bind(2, hash);
    root.exec();
    return seq;
}
void syncCurrentRoot(Store &store) {
    auto &db = store.db();
    if (!hasStoredRoots(db))
        return;
    const auto seq = rootForPoint(db, OpJournal(store).headSeq());
    const auto hash = storedRootHash(db, seq);
    if (!hash)
        throw std::runtime_error("missing current stored baseline");
    {
        SQLite::Statement current(db,
                                  "SELECT c.base_seq,h.state_hash FROM history_checkpoint c LEFT "
                                  "JOIN history_checkpoint_content h ON h.id=c.id WHERE c.id=1");
        if (current.executeStep() && current.getColumn(0).getInt64() == seq &&
            !current.getColumn(1).isNull() && current.getColumn(1).getString() == *hash)
            return;
    }
    const auto bytes = encodePayload(readCheckpointContent(db, *hash));
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("stored baseline exceeds SQLite blob limit");
    SQLite::Statement legacy(db, "INSERT INTO history_checkpoint(id,base_seq,state) "
                                 "VALUES(1,?,?) ON CONFLICT(id) DO UPDATE SET "
                                 "base_seq=excluded.base_seq,state=excluded.state");
    legacy.bind(1, seq);
    legacy.bind(2, bytes.data(), static_cast<int>(bytes.size()));
    legacy.exec();
    SQLite::Statement content(db,
                              "INSERT INTO history_checkpoint_content(id,state_hash) VALUES(1,?) "
                              "ON CONFLICT(id) DO UPDATE SET state_hash=excluded.state_hash");
    content.bind(1, *hash);
    content.exec();
}
} // namespace history_detail
} // namespace adi
