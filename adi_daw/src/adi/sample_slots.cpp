// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sample_slots.hpp"
#include "adi/row_ids.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/store.hpp"
#include "juce/builtin_samples.hpp"
#include "juce/pd_declarations.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
namespace adi {
namespace {
constexpr std::string_view sampleInstrumentUid = "org.adidaw.builtin.oneshot";
constexpr std::int32_t sampleInstrumentSlot = 1;
std::string sampleDeviceUid(Store& store,std::int64_t device) {
    SQLite::Statement selected(store.db(),"SELECT p.format,p.uid FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE d.id=?");selected.bind(1,device);
    if(!selected.executeStep() || selected.getColumn(0).getString()!="internal")throw std::runtime_error("sample assignment needs a supported built-in device");
    return selected.getColumn(1).getString();
}
void checkSlot(std::string_view uid, std::int32_t slot) {
    const auto* info = findBuiltin(uid);
    if(!info||!info->available)throw std::runtime_error("sample device backend is unavailable");
    const auto declarations=device::parsePdDeclarations(builtinDeclarations(uid));
    if(std::none_of(declarations.samples.begin(),declarations.samples.end(),[&](const auto& value){return value.id==slot;}))
        throw std::runtime_error("device did not declare this sample slot");
    (void)device::inspectBuiltinSample(device::emptyBuiltinSample(uid,slot));
}
void requireEditableInstrumentTrack(SQLite::Database& db, std::int64_t track) {
    SQLite::Statement row(db,
        "WITH RECURSIVE enclosing(id,parent_id,frozen,locked) AS ("
        " SELECT id,parent_id,frozen,locked FROM tracks WHERE id=?"
        " UNION SELECT t.id,t.parent_id,t.frozen,t.locked FROM tracks t JOIN enclosing e ON t.id=e.parent_id)"
        " SELECT 1 FROM enclosing WHERE frozen<>0 OR locked<>0 LIMIT 1");
    row.bind(1, track);
    if (row.executeStep()) throw std::runtime_error("A sample cannot replace an instrument on a frozen or locked track.");
}
void addState(PreparedSampleAssignment& prepared,const Payload& state) {
    auto bytes=Payload::to_cbor(state);const auto hash=media::blake3Bytes(std::as_bytes(std::span(bytes)));
    if(!hash)throw std::runtime_error("sample state hashing failed");
    prepared.blobs.push_back({hash.hex,std::move(bytes)});
    OpRequest request;request.opType="device.loadState";request.label="Assign device sample";request.targetKind="device";request.targetId=prepared.deviceId;
    request.payload={{"dev",prepared.deviceId},{"role","sample:"+std::to_string(state.at("slot").get<std::int32_t>())},
        {"hash",hash.hex},{"hint","adi.builtin.sample.cbor.v1"}};
    prepared.requests.push_back(std::move(request));
}
PreparedSampleAssignment prepare(Store& store,std::int64_t deviceId,std::int32_t slot,const std::filesystem::path* file,
    double rate,std::int64_t utc,audio::DecodeCache* cache,std::string_view expectedHash={},std::string_view stagedUid={}) {
    PreparedSampleAssignment result;result.deviceId=deviceId;
    try {
        if(audio::onAudioThread)throw std::runtime_error("sample assignment belongs off the audio thread");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        auto& db=store.db();std::optional<SQLite::Transaction> read;
        if(sqlite3_get_autocommit(db.getHandle())!=0)read.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        const auto uid=stagedUid.empty()?sampleDeviceUid(store,deviceId):std::string(stagedUid);
        checkSlot(uid,slot);result.sourceFile=media::pathUtf8(store.path());
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        Payload state=device::emptyBuiltinSample(uid,slot);
        if(file) {
            RowIds ids(db);
            OpRequest import;const auto imported=media::prepareImport(store,*file,ids.next("media_files"),utc,import,cache);
            if(!imported.ok)throw std::runtime_error(imported.error);
            result.mediaId=imported.mediaId;
            const auto hash=import.opType=="media.import" ? import.payload.at("row").at("hash_blake3").get<std::string>()
                                                          : media::mediaRow(db,result.mediaId).at("hash_blake3").get<std::string>();
            if(!expectedHash.empty() && hash!=expectedHash) {
                result.sourceChanged=true;
                throw std::runtime_error("The selected sample has changed since it was indexed.");
            }
            if(!import.opType.empty())result.requests.push_back(std::move(import));
            const auto source=file->is_absolute()?*file:media::projectFolder(store)/ *file;
            const auto playable=cache?cache->playable(source):source;
            try {state=device::captureBuiltinSample(uid,slot,source,playable,rate);}
            catch(...){if(cache)cache->release(playable);throw;}
            if(cache)cache->release(playable);
            // The import and decode must describe the SAME content even if a
            // worker overwrote the file while preparation was reading it.
            if(state.at("mediaHash")!=hash) {
                result.sourceChanged=true;
                throw std::runtime_error("sample media changed during preparation");
            }
        }
        if (stagedUid.empty()) {
            SQLite::Statement existing(db,"SELECT 1 FROM plugin_state WHERE device_id=? AND stream_role=?");
            existing.bind(1,deviceId);existing.bind(2,"sample:"+std::to_string(slot));
            // The opener lets Undo empty an existing live slot before the
            // final inverse removes its row. Undo removes a staged device.
            if(!existing.executeStep())addState(result,device::emptyBuiltinSample(uid,slot));
        }
        addState(result,state);
        for(auto& request:result.requests) {
            request.label=file?"Assign device sample":"Clear device sample";
            if(request.targetKind.empty()){request.targetKind="media";request.targetId=result.mediaId;}
        }
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
}
PreparedSampleAssignment prepareSampleAssignment(Store& store,std::int64_t device,std::int32_t slot,
    const std::filesystem::path& path,double rate,std::int64_t utc,audio::DecodeCache* cache,std::string_view expectedHash) {
    return prepare(store,device,slot,&path,rate,utc,cache,expectedHash);
}
PreparedSampleAssignment prepareSampleClear(Store& store,std::int64_t device,std::int32_t slot){return prepare(store,device,slot,nullptr,48000,0,nullptr);}
PreparedSampleAssignment prepareSampleInstrument(Store& store, std::int64_t track,
    const std::filesystem::path& file, double rate, std::int64_t utc,
    audio::DecodeCache* cache, std::int64_t retainedDeviceId) {
    try {
        if (audio::onAudioThread) throw std::runtime_error("sample assignment belongs off the audio thread");
        auto& db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        requireEditableInstrumentTrack(db, track);
        auto insertion = prepareBuiltinInsert(store, sampleInstrumentUid, track, {}, retainedDeviceId);
        if (!insertion.ok) throw std::runtime_error(insertion.error);
        auto prepared = prepare(store, insertion.deviceId, sampleInstrumentSlot, &file, rate, utc, cache, {}, sampleInstrumentUid);
        if (!prepared.ok) return prepared;
        prepared.requests.insert(prepared.requests.begin(), std::make_move_iterator(insertion.requests.begin()),
                                  std::make_move_iterator(insertion.requests.end()));
        for (auto& request : prepared.requests) request.label = "Load Sample Instrument";
        return prepared;
    } catch (const std::exception& error) {
        PreparedSampleAssignment refused;
        refused.error = error.what();
        return refused;
    }
}
CommitResult commitSampleAssignment(Store& store,const PreparedSampleAssignment& prepared,Actor actor) {
    CommitResult result;
    if(audio::onAudioThread){result.error="sample assignment belongs off the audio thread";return result;}
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(media::pathUtf8(store.path())!=prepared.sourceFile){result.error="project changed after sample preparation";result.stale=true;return result;}
    if(prepared.options.expectHead && OpJournal(store).headSeq()!=*prepared.options.expectHead){result.error="stale: the undo head moved";result.stale=true;return result;}
    try {
        // Immutable content follows the established ParamOps::writeBlobs
        // convention. A failed journal may retain unreferenced content only.
        SQLite::Transaction content(store.db());
        for(const auto& blob:prepared.blobs) {
            const auto hash=media::blake3Bytes(std::as_bytes(std::span(blob.bytes)));
            if(!hash||hash.hex!=blob.hash)throw std::runtime_error("sample state blob hash differs");
            SQLite::Statement insert(store.db(),"INSERT OR IGNORE INTO state_blobs(hash_blake3,data,size_bytes) VALUES(?,?,?)");
            insert.bind(1,blob.hash);insert.bind(2,blob.bytes.data(),static_cast<int>(blob.bytes.size()));insert.bind(3,static_cast<std::int64_t>(blob.bytes.size()));insert.exec();
            const auto stored=store.getStateBlob(blob.hash);
            if(!stored || *stored!=std::vector<std::byte>(reinterpret_cast<const std::byte*>(blob.bytes.data()),reinterpret_cast<const std::byte*>(blob.bytes.data()+blob.bytes.size())))
                throw std::runtime_error("stored sample state content differs from its hash");
        }
        content.commit();
        auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
        return OpJournal(store).commit(requests,prepared.options);
    }catch(const std::exception& exception){result.error=exception.what();return result;}
}
}
