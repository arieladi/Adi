// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sample_slots.hpp"
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
#include <limits>
#include <stdexcept>
namespace adi {
namespace {
std::string checkedDevice(Store& store,std::int64_t device,std::int32_t slot) {
    if(store.readOnly())throw std::runtime_error("project is read-only");
    if(audio::onAudioThread)throw std::runtime_error("sample assignment belongs off the audio thread");
    SQLite::Statement selected(store.db(),"SELECT p.format,p.uid FROM devices d JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE d.id=?");selected.bind(1,device);
    if(!selected.executeStep() || selected.getColumn(0).getString()!="internal")throw std::runtime_error("sample assignment needs a supported built-in device");
    const auto uid=selected.getColumn(1).getString();const auto* info=findBuiltin(uid);
    if(!info||!info->available)throw std::runtime_error("sample device backend is unavailable");
    const auto declarations=device::parsePdDeclarations(builtinCanvas(uid));
    if(std::none_of(declarations.samples.begin(),declarations.samples.end(),[&](const auto& value){return value.id==slot;}))
        throw std::runtime_error("device did not declare this sample slot");
    (void)device::inspectBuiltinSample(device::emptyBuiltinSample(uid,slot));return uid;
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
    double rate,std::int64_t utc,audio::DecodeCache* cache) {
    PreparedSampleAssignment result;result.deviceId=deviceId;
    try {
        if(audio::onAudioThread)throw std::runtime_error("sample assignment belongs off the audio thread");
        auto& db=store.db();std::optional<SQLite::Transaction> read;
        if(sqlite3_get_autocommit(db.getHandle())!=0)read.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        const auto uid=checkedDevice(store,deviceId,slot);result.sourceFile=media::pathUtf8(store.path());
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        Payload state=device::emptyBuiltinSample(uid,slot);
        if(file) {
            const auto highest=db.execAndGet("SELECT MAX(0,IFNULL(MAX(id),0)) FROM media_files").getInt64();
            if(highest==std::numeric_limits<std::int64_t>::max())throw std::runtime_error("media identities are exhausted");
            OpRequest import;const auto imported=media::prepareImport(store,*file,highest+1,utc,import,cache);
            if(!imported.ok)throw std::runtime_error(imported.error);
            result.mediaId=imported.mediaId;
            if(!import.opType.empty())result.requests.push_back(std::move(import));
            const auto source=file->is_absolute()?*file:media::projectFolder(store)/ *file;
            const auto playable=cache?cache->playable(source):source;
            try {state=device::captureBuiltinSample(uid,slot,source,playable,rate);}
            catch(...){if(cache)cache->release(playable);throw;}
            if(cache)cache->release(playable);
            // The import and decode must describe the SAME content even if a
            // worker overwrote the file while preparation was reading it.
            std::string expected;
            if(!result.requests.empty() && result.requests.front().opType=="media.import")expected=result.requests.front().payload.at("row").at("hash_blake3").get<std::string>();
            else expected=media::mediaRow(db,result.mediaId).at("hash_blake3").get<std::string>();
            if(state.at("mediaHash")!=expected)throw std::runtime_error("sample media changed during preparation");
        }
        SQLite::Statement existing(db,"SELECT 1 FROM plugin_state WHERE device_id=? AND stream_role=?");existing.bind(1,deviceId);existing.bind(2,"sample:"+std::to_string(slot));
        // The opener is necessary for the existing live ParamOps inverse: it
        // loads the empty slot before the final inverse removes the absent row.
        if(!existing.executeStep())addState(result,device::emptyBuiltinSample(uid,slot));
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
    const std::filesystem::path& path,double rate,std::int64_t utc,audio::DecodeCache* cache){return prepare(store,device,slot,&path,rate,utc,cache);}
PreparedSampleAssignment prepareSampleClear(Store& store,std::int64_t device,std::int32_t slot){return prepare(store,device,slot,nullptr,48000,0,nullptr);}
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
