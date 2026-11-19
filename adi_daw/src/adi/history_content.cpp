// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/history_content.hpp"
#include "adi/history_compaction.hpp"
#include "adi/media/blake3.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
namespace adi::history_detail {
namespace {
std::string contentHash(const std::vector<std::byte>& bytes) {
    const auto hash=media::blake3Bytes(bytes);
    if(!hash)throw std::runtime_error("cannot hash checkpoint content");
    return hash.hex;
}
std::vector<std::byte> objectBytes(SQLite::Database& db,const char* table,const std::string& hash) {
    SQLite::Statement query(db,"SELECT data FROM "+std::string(table)+" WHERE hash=?");query.bind(1,hash);
    if(!query.executeStep())throw std::runtime_error("missing checkpoint content object: "+hash);
    const auto column=query.getColumn(0);std::vector<std::byte> bytes;
    if(column.getBytes()>0){const auto* data=static_cast<const std::byte*>(column.getBlob());bytes.assign(data,data+column.getBytes());}
    if(contentHash(bytes)!=hash)throw std::runtime_error("checkpoint content hash mismatch: "+hash);
    return bytes;
}
std::string intern(SQLite::Database& db,const char* table,const Payload& value) {
    const auto bytes=encodePayload(value);
    if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("checkpoint object exceeds SQLite blob limit");
    const auto hash=contentHash(bytes);
    SQLite::Statement insert(db,"INSERT OR IGNORE INTO "+std::string(table)+"(hash,data) VALUES(?,?)");insert.bind(1,hash);insert.bind(2,bytes.data(),static_cast<int>(bytes.size()));insert.exec();
    // Existing objects are immutable: never accept tampering or a collision.
    if(objectBytes(db,table,hash)!=bytes)throw std::runtime_error("checkpoint content collision");
    return hash;
}
Payload decodeObject(SQLite::Database& db,const char* table,const std::string& hash) {
    std::string error;auto value=decodeFromLog(objectBytes(db,table,hash),error);
    if(!value)throw std::runtime_error("invalid checkpoint content: "+error);
    return std::move(*value);
}
}
std::string storeCheckpointContent(SQLite::Database& db,const Payload& state) {
    if(state.at("format")!=1)throw std::runtime_error("unsupported checkpoint content format");
    Payload manifest={{"format",1},{"schema_major",state.at("schema_major")},
        {"schema_minor",state.at("schema_minor")},{"tables",Payload::array()}};
    for(const auto& table:state.at("tables")){
        std::vector<std::string> hashes;
        for(const auto& row:table.at("rows")){
            if(!row.is_array() || row.size()!=table.at("columns").size())throw std::runtime_error("invalid checkpoint row shape");
            hashes.push_back(intern(db,"history_state_rows",row));
        }
        std::sort(hashes.begin(),hashes.end());
        manifest["tables"].push_back({{"name",table.at("name")},{"columns",table.at("columns")},{"row_hashes",hashes}});
    }
    std::sort(manifest["tables"].begin(),manifest["tables"].end(),[](const auto& a,const auto& b){return a.at("name").template get<std::string>()<b.at("name").template get<std::string>();});
    return intern(db,"history_state_manifests",manifest);
}
Payload readCheckpointContent(SQLite::Database& db,const std::string& hash) {
    const auto manifest=decodeObject(db,"history_state_manifests",hash);
    if(manifest.at("format")!=1)throw std::runtime_error("unsupported checkpoint manifest format");
    Payload result={{"format",1},{"schema_major",manifest.at("schema_major")},
        {"schema_minor",manifest.at("schema_minor")},{"tables",Payload::array()}};
    for(const auto& table:manifest.at("tables")){
        Payload rows=Payload::array();
        for(const auto& rowHash:table.at("row_hashes")){
            auto row=decodeObject(db,"history_state_rows",rowHash.template get<std::string>());
            if(!row.is_array() || row.size()!=table.at("columns").size())throw std::runtime_error("invalid checkpoint content row shape");
            rows.push_back(std::move(row));
        }
        result["tables"].push_back({{"name",table.at("name")},{"columns",table.at("columns")},{"rows",std::move(rows)}});
    }
    return result;
}
void visitCheckpointState(SQLite::Database& db,const std::function<bool(Payload&)>& visitor,bool write) {
    const auto exists=[&](const char* table){SQLite::Statement query(db,"SELECT 1 FROM main.sqlite_master WHERE type='table' AND name=?");query.bind(1,table);return query.executeStep();};
    if (exists("history_roots") &&
        db.execAndGet("SELECT count(*) FROM history_roots").getInt64() != 0) {
        std::map<std::string, std::vector<std::int64_t>> roots;
        {
            SQLite::Statement rows(db, "SELECT seq,state_hash FROM history_roots ORDER BY seq");
            while (rows.executeStep())
                roots[rows.getColumn(1).getString()].push_back(rows.getColumn(0).getInt64());
        }
        for (const auto &[oldHash, seqs] : roots) {
            auto saved = readCheckpointContent(db, oldHash);
            if (!visitor(saved) || !write)
                continue;
            const auto hash = storeCheckpointContent(db, saved);
            const auto payload = encodePayload({{"state_hash", hash}});
            for (const auto seq : seqs) {
                SQLite::Statement mapping(db, "UPDATE history_roots SET state_hash=? WHERE seq=?");
                mapping.bind(1, hash);
                mapping.bind(2, seq);
                mapping.exec();
                SQLite::Statement metadata(
                    db, "UPDATE ops SET payload=? WHERE seq=? AND op_type='history.root'");
                metadata.bind(1, payload.data(), static_cast<int>(payload.size()));
                metadata.bind(2, seq);
                if (metadata.exec() != 1)
                    throw std::runtime_error("missing stored baseline during relocation");
            }
        }
        if (write) {
            SQLite::Statement current(db, "SELECT r.state_hash FROM history_checkpoint c JOIN "
                                          "history_roots r ON r.seq=c.base_seq WHERE c.id=1");
            if (!current.executeStep())
                throw std::runtime_error("missing stored current checkpoint during relocation");
            const auto hash = current.getColumn(0).getString();
            const auto bytes = encodePayload(readCheckpointContent(db, hash));
            if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                throw std::runtime_error("relocated checkpoint exceeds SQLite blob limit");
            SQLite::Statement image(db, "UPDATE history_checkpoint SET state=? WHERE id=1");
            image.bind(1, bytes.data(), static_cast<int>(bytes.size()));
            image.exec();
            SQLite::Statement link(db,
                                   "UPDATE history_checkpoint_content SET state_hash=? WHERE id=1");
            link.bind(1, hash);
            link.exec();
        }
        return;
    }
    if(!exists("history_checkpoint"))return;
    std::optional<Payload> state;
    {SQLite::Statement row(db,"SELECT state FROM history_checkpoint WHERE id=1");if(row.executeStep()){
        const auto column=row.getColumn(0);std::vector<std::byte> bytes;
        if(column.getBytes()>0){const auto* data=static_cast<const std::byte*>(column.getBlob());bytes.assign(data,data+column.getBytes());}
        std::string error;state=decodeFromLog(bytes,error);if(!state)throw std::runtime_error("invalid checkpoint during relocation: "+error);
    }}
    if(!state || !visitor(*state) || !write)return;
    const auto bytes=encodePayload(*state);if(bytes.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::runtime_error("relocated checkpoint exceeds SQLite blob limit");
    SQLite::Statement update(db,"UPDATE history_checkpoint SET state=? WHERE id=1");update.bind(1,bytes.data(),static_cast<int>(bytes.size()));update.exec();
    if(exists("history_checkpoint_content")){
        const auto hash=storeCheckpointContent(db,*state);SQLite::Statement link(db,"INSERT INTO history_checkpoint_content(id,state_hash) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET state_hash=excluded.state_hash");link.bind(1,hash);link.exec();
    }
}
} // namespace adi::history_detail
