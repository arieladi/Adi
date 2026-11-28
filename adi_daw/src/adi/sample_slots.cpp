// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sample_slots.hpp"
#include "adi/row_ids.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/store.hpp"
#include "juce/builtin_samples.hpp"
#include "juce/pd_declarations.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <bit>
#include <cmath>
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
    double rate,std::int64_t utc,audio::DecodeCache* cache,std::string_view stagedUid={}) {
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
    const std::filesystem::path& path,double rate,std::int64_t utc,audio::DecodeCache* cache){return prepare(store,device,slot,&path,rate,utc,cache);}
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
        auto prepared = prepare(store, insertion.deviceId, sampleInstrumentSlot, &file, rate, utc, cache, sampleInstrumentUid);
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
namespace {
// SC-482: Reverse and Crop (Simpler's context menu), as changed copies.
enum class SampleProcess { Reverse, Crop };
Payload currentSampleState(Store& store, std::int64_t device, std::int32_t slot) {
    SQLite::Statement row(store.db(), "SELECT state_hash FROM plugin_state WHERE device_id=? AND stream_role=?");
    row.bind(1, device);
    row.bind(2, "sample:" + std::to_string(slot));
    if (!row.executeStep()) throw std::runtime_error("the sample slot holds no sample");
    const auto blob = store.getStateBlob(row.getColumn(0).getString());
    if (!blob) throw std::runtime_error("the sample slot's state is missing");
    std::vector<std::uint8_t> bytes(blob->size());
    std::transform(blob->begin(), blob->end(), bytes.begin(),
                   [](std::byte b) { return static_cast<std::uint8_t>(b); });
    return Payload::from_cbor(bytes);
}
// A declared parameter's stored value, normalized, or its default.
double storedNormal(SQLite::Database& db, std::int64_t device, const device::PdParamDecl& decl) {
    SQLite::Statement row(db, "SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id=?");
    row.bind(1, device);
    row.bind(2, std::to_string(decl.id));
    if (row.executeStep()) return std::clamp(row.getColumn(0).getDouble(), 0., 1.);
    return decl.max > decl.min ? (decl.def - decl.min) / (decl.max - decl.min) : 0.;
}
const device::PdParamDecl& declared(const device::PdDeclarations& declarations, std::string_view name) {
    for (const auto& decl : declarations.params)
        if (decl.name == name) return decl;
    throw std::runtime_error("OneShot does not declare " + std::string(name));
}
OpRequest setParamRequest(std::int64_t device, const device::PdParamDecl& decl, double norm) {
    OpRequest request;
    request.opType = "device.setParam";
    request.targetKind = "device";
    request.targetId = device;
    request.payload = {{"dev", device}, {"param", std::to_string(decl.id)}, {"norm", norm},
                       {"real", decl.min + norm * (decl.max - decl.min)}};
    return request;
}
PreparedSampleAssignment prepareProcessed(Store& store, std::int64_t deviceId, std::int32_t slot,
                                          SampleProcess what, double rate, std::int64_t utc) {
    std::filesystem::path written;
    try {
        if (audio::onAudioThread) throw std::runtime_error("sample processing belongs off the audio thread");
        if (store.readOnly()) throw std::runtime_error("project is read-only");
        auto& db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0) read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        if (sampleDeviceUid(store, deviceId) != sampleInstrumentUid)
            throw std::runtime_error("Reverse and Crop act on OneShot's sample");
        const auto state = currentSampleState(store, deviceId, slot);
        const auto info = device::inspectBuiltinSample(state);
        if (info.empty) throw std::runtime_error("the sample slot holds no sample");
        const auto channels = state.at("channels").get<int>();
        const auto sourceRate = state.at("sourceRate").get<double>();
        const auto& bytes = state.at("source_f32le").get_binary();
        const auto stride = static_cast<std::size_t>(channels);
        if (channels < 1 || channels > 2 || !(sourceRate >= 1 && sourceRate <= 768000) || bytes.empty() ||
            bytes.size() % (4 * stride) != 0)
            throw std::runtime_error("the sample slot's audio is invalid");
        const std::size_t frames = bytes.size() / (4 * stride);
        std::vector<float> samples(bytes.size() / 4);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            std::uint32_t bits = 0;
            for (unsigned k = 0; k < 4; ++k) bits |= static_cast<std::uint32_t>(bytes[i * 4 + k]) << (8 * k);
            samples[i] = std::bit_cast<float>(bits);
        }
        const auto declarations = device::parsePdDeclarations(builtinDeclarations(sampleInstrumentUid));
        const auto& start = declared(declarations, "Start");
        const auto& end = declared(declarations, "End");
        std::size_t first = 0, last = frames;
        if (what == SampleProcess::Crop) {
            // The region a note plays (OneShot::noteOn): from Start's frame to
            // End's, at least one frame; partial frames are kept whole.
            const auto count = static_cast<double>(frames);
            const double from = std::min(count - 1, count * storedNormal(db, deviceId, start));
            const double to = std::max(from + 1, count * storedNormal(db, deviceId, end));
            first = static_cast<std::size_t>(std::floor(from));
            last = std::clamp(static_cast<std::size_t>(std::ceil(to)), first + 1, frames);
        }
        std::vector<float> changed(samples.begin() + static_cast<std::ptrdiff_t>(first * stride),
                                   samples.begin() + static_cast<std::ptrdiff_t>(last * stride));
        if (what == SampleProcess::Reverse)
            for (std::size_t a = 0, b = last - first - 1; a < b; ++a, --b)
                for (std::size_t c = 0; c < stride; ++c) std::swap(changed[a * stride + c], changed[b * stride + c]);
        // The copy's name: the original's, then the process, numbered if taken.
        std::string name = "Sample";
        {
            SQLite::Statement original(db, "SELECT orig_name FROM media_files WHERE hash_blake3=? ORDER BY id LIMIT 1");
            original.bind(1, info.mediaHash);
            if (original.executeStep()) {
                const auto stem = media::pathFromUtf8(original.getColumn(0).getString()).stem();
                if (!stem.empty()) name = media::pathUtf8(stem);
            }
        }
        const std::string process = what == SampleProcess::Reverse ? "Reverse" : "Crop";
        const auto folder = media::projectFolder(store) / "Samples" / "Processed" / process;
        std::filesystem::create_directories(folder);
        for (int n = 1; n < 1000 && written.empty(); ++n) {
            const auto candidate =
                folder / media::pathFromUtf8(name + " " + process + (n > 1 ? " " + std::to_string(n) : "") + ".wav");
            if (!std::filesystem::exists(candidate)) written = candidate;
        }
        if (written.empty()) throw std::runtime_error("no free name for the processed sample");
        {
            audio::WavWriter writer(written, static_cast<std::uint32_t>(std::lround(sourceRate)),
                                    static_cast<std::uint16_t>(channels), audio::WavFormat::Float32);
            writer.write(changed.data(), static_cast<std::uint32_t>(last - first));
            writer.close();
        }
        auto prepared = prepare(store, deviceId, slot, &written, rate, utc, nullptr);
        if (!prepared.ok) throw std::runtime_error(prepared.error);
        if (what == SampleProcess::Crop) {
            prepared.requests.push_back(setParamRequest(deviceId, start, 0));
            prepared.requests.push_back(setParamRequest(deviceId, end, 1));
        }
        for (auto& request : prepared.requests) request.label = process + " Sample";
        return prepared;
    } catch (const std::exception& error) {
        std::error_code ignored;
        if (!written.empty()) std::filesystem::remove(written, ignored);
        PreparedSampleAssignment refused;
        refused.error = error.what();
        return refused;
    }
}
} // namespace
PreparedSampleAssignment prepareSampleReverse(Store& store, std::int64_t device, std::int32_t slot,
                                              double rate, std::int64_t utc) {
    return prepareProcessed(store, device, slot, SampleProcess::Reverse, rate, utc);
}
PreparedSampleAssignment prepareSampleCrop(Store& store, std::int64_t device, std::int32_t slot,
                                           double rate, std::int64_t utc) {
    return prepareProcessed(store, device, slot, SampleProcess::Crop, rate, utc);
}
PreparedSampleAssignment prepareSampleSlices(Store& store, std::int64_t deviceId, std::int32_t slot,
                                             std::span<const std::size_t> frames) {
    PreparedSampleAssignment result;
    result.deviceId = deviceId;
    try {
        if (audio::onAudioThread) throw std::runtime_error("slice editing belongs off the audio thread");
        if (store.readOnly()) throw std::runtime_error("project is read-only");
        auto& db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0) read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        if (sampleDeviceUid(store, deviceId) != sampleInstrumentUid)
            throw std::runtime_error("slices by hand are OneShot's");
        auto state = currentSampleState(store, deviceId, slot);
        if (device::inspectBuiltinSample(state).empty) throw std::runtime_error("the sample slot holds no sample");
        const auto channels = state.at("channels").get<int>();
        const auto& bytes = state.at("source_f32le").get_binary();
        if (channels < 1 || channels > 2 || bytes.size() % (4 * static_cast<std::size_t>(channels)) != 0)
            throw std::runtime_error("the sample slot's audio is invalid");
        const std::size_t sampleFrames = bytes.size() / (4 * static_cast<std::size_t>(channels));
        std::vector<std::size_t> slices(frames.begin(), frames.end());
        std::sort(slices.begin(), slices.end());
        slices.erase(std::unique(slices.begin(), slices.end()), slices.end());
        std::erase(slices, std::size_t{0});
        if (slices.size() > 63) throw std::runtime_error("a sample holds at most 64 slices");
        if (!slices.empty() && slices.back() >= sampleFrames) throw std::runtime_error("a slice lies past the sample's end");
        if (slices.empty())
            state.erase("slices");
        else
            state["slices"] = slices;
        result.sourceFile = media::pathUtf8(store.path());
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        addState(result, state);
        for (auto& request : result.requests) request.label = "Edit Slices";
        result.ok = true;
    } catch (const std::exception& error) {
        result = {};
        result.error = error.what();
    }
    return result;
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
