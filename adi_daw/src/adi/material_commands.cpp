// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/material_commands.hpp"
#include "adi/sample_defaults.hpp"
#include "adi/library/index.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/transients.hpp"
#include "adi/audio/decode.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace adi::clip_edit {
namespace fs=std::filesystem;
struct MaterialFiles {
    fs::path directory,staged,target;
    std::string hash;
    bool published=false,committed=false;
    std::stop_token stop;
    ~MaterialFiles() {
        std::error_code ec;
        if(published && !committed)fs::remove(target,ec);
        fs::remove_all(directory,ec);
    }
};
namespace {
std::shared_ptr<MaterialFiles> stage(const fs::path& parent) {
    auto result=std::make_shared<MaterialFiles>();
    static std::atomic<std::uint64_t> serial{0};
    for(unsigned attempt=0;attempt<128;++attempt) {
        auto path=parent/(".adi-edit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
                          "-"+std::to_string(serial.fetch_add(1)));
        std::error_code ec;
        if(fs::create_directory(path,ec)){result->directory=std::move(path);return result;}
        if(ec && ec!=std::errc::file_exists)throw fs::filesystem_error("stage edit",path,ec);
    }
    throw std::runtime_error("cannot reserve material stage");
}
void paths(OpRequest& request,Store& store,const fs::path& target) {
    const auto relative=fs::relative(target,media::projectFolder(store));
    Payload captured={{"rel_path",media::pathUtf8(relative)},{"abs_path_hint",nullptr},{"missing",0}};
    if(request.opType=="media.import")request.payload["row"].update(captured);
    else if(request.opType=="media.relink")request.payload["paths"]=std::move(captured);
}
void snapshot(Store& store,const fs::path& target) {
    SQLite::Database output(media::pathUtf8(target),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
    auto* backup=sqlite3_backup_init(output.getHandle(),"main",store.db().getHandle(),"main");
    if(!backup)throw std::runtime_error("cannot start bounce snapshot");
    const int copied=sqlite3_backup_step(backup,-1),finished=sqlite3_backup_finish(backup);
    if(copied!=SQLITE_DONE || finished!=SQLITE_OK)throw std::runtime_error("cannot capture bounce snapshot");
    // The backup lives in a private subdirectory. Its media retain the live
    // project's original base; no project or history paths are rewritten.
    SQLite::Statement rows(output,"SELECT id,rel_path FROM media_files WHERE rel_path IS NOT NULL");
    while(rows.executeStep()) {
        const auto source=media::projectFolder(store)/media::pathFromUtf8(rows.getColumn(1).getString());
        SQLite::Statement update(output,"UPDATE media_files SET rel_path=NULL,abs_path_hint=? WHERE id=?");
        update.bind(1,media::pathUtf8(fs::absolute(source)));update.bind(2,rows.getColumn(0).getInt64());update.exec();
    }
}
}
MaterialEdit prepareImportAudio(Store& store,const ImportOptions& options,double referenceRate) {
    return prepareImportAudio(store,options,referenceRate,nullptr);
}
MaterialEdit prepareImportAudio(Store& store,const ImportOptions& options,double referenceRate,library::Index* index) {
    MaterialEdit result;
    if(audio::onAudioThread)return result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(options.stop.stop_requested())throw std::runtime_error("import cancelled");
        if(!std::isfinite(referenceRate) || referenceRate<=0 || referenceRate>768000)
            throw std::runtime_error("invalid reference render rate");
        SQLite::Transaction read(store.db(),SQLite::TransactionBehavior::DEFERRED);
        result.edit.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement identity(store.db(),"PRAGMA database_list");
        while(identity.executeStep())if(identity.getColumn(1).getString()=="main")result.edit.sourceFile=identity.getColumn(2).getString();
        auto source=options.source.is_absolute()?options.source:media::projectFolder(store)/options.source;
        auto files=stage(media::projectFolder(store));files->stop=options.stop;result.files=files;
        if(options.copyIn) {
            files->staged=files->directory/source.filename();
            fs::copy_file(source,files->staged,fs::copy_options::none);source=files->staged;
        }
        OpRequest mediaRequest;
        const auto imported=media::prepareImport(store,source,options.mediaId,options.importedUtc,mediaRequest);
        if(!imported.ok)throw std::runtime_error(imported.error);
        if(!imported.audio)throw std::runtime_error("import prompt needs a supported audio file; video playback is not built");
        const auto& info=*imported.audio;
        std::optional<library::SampleSettings> saved;
        if(index) {
            std::string hash;
            if(mediaRequest.opType=="media.import")hash=mediaRequest.payload.at("row").at("hash_blake3").get<std::string>();
            else {SQLite::Statement key(store.db(),"SELECT hash_blake3 FROM media_files WHERE id=?");key.bind(1,imported.mediaId);if(!key.executeStep())throw std::runtime_error("imported source identity is missing");hash=key.getColumn(0).getString();}
            const auto loaded=index->sampleSettings(hash);
            if(loaded) {
                if(loaded.data->sampleRate!=info.sampleRate || loaded.data->channels!=info.channels || loaded.data->frames!=static_cast<std::uint64_t>(info.frames))throw std::runtime_error("saved sample defaults do not match source dimensions");
                saved=*loaded.data;
            }else if(loaded.problem!=library::SampleSettingsProblem::Missing)throw std::runtime_error("cannot recall authored sample defaults: "+loaded.error);
        }
        std::optional<audio::AutoWarpResult> warped;
        std::optional<audio::WarpMapWrite> markers;
        if(options.warp && !saved) {
            if(referenceRate<44100 || referenceRate!=std::floor(referenceRate) || info.sampleRate<8000 ||
               info.sampleRate>768000 || info.channels<1 || info.channels>64 || info.frames<=0 || info.frames>0x7fffffff ||
               !audio::warpModeName(options.warpMode))throw std::runtime_error("audio source is outside supported warp bounds");
            struct Decoded {
                std::filesystem::path file;bool pinned=false;
                explicit Decoded(const std::filesystem::path& input):file(audio::playableFile(input)),pinned(file!=input){}
                ~Decoded(){if(pinned)try{audio::defaultDecodeCache().release(file);}catch(...){}}
            } decoded(source);
            audio::WavReader reader(decoded.file);
            if(reader.frames()!=static_cast<std::uint64_t>(info.frames) || reader.sampleRate()!=static_cast<std::uint32_t>(info.sampleRate) || reader.channels()!=static_cast<std::uint16_t>(info.channels))
                throw std::runtime_error("source dimensions changed during import");
            const auto onsets=audio::detectTransients(reader,0,static_cast<std::uint64_t>(info.frames),50,options.stop);
            if(options.stop.stop_requested())throw std::runtime_error("import cancelled");
            if(!onsets)throw std::runtime_error("cannot analyze the source audio");
            warped=audio::autoWarpFromOnsets(onsets.frames,static_cast<std::uint64_t>(info.frames),static_cast<std::uint32_t>(info.sampleRate),options.warpAnalysis,options.stop);
            if(options.stop.stop_requested())throw std::runtime_error("import cancelled");
            if(!*warped) {
                if(warped->error==audio::AutoWarpError::NoTempo)throw std::runtime_error("Warp needs a detected beat or an explicit source tempo");
                if(warped->error==audio::AutoWarpError::InvalidOptions)throw std::runtime_error("invalid source tempo or meter for Warp");
                if(warped->error==audio::AutoWarpError::TooManyMarkers)throw std::runtime_error("source needs too many warp markers");
                throw std::runtime_error("detected source timing cannot be represented by warp markers");
            }
            markers=audio::writeWarpMap(warped->markers,static_cast<std::uint64_t>(info.frames)-warped->trimFrames);
            if(!*markers)throw std::runtime_error("cannot capture the detected warp map");
        }
        const auto sourceHash=media::blake3File(source,options.stop);
        const auto capturedHash=mediaRequest.opType=="media.import"?mediaRequest.payload.at("row").at("hash_blake3"):media::mediaRow(store.db(),imported.mediaId).at("hash_blake3");
        if(!sourceHash || capturedHash!=sourceHash.hex)throw std::runtime_error("source changed during material capture");
        files->staged=source;files->hash=sourceHash.hex;
        if(options.copyIn) {
            const auto folder=media::projectFolder(store)/"audio";
            if(fs::is_symlink(fs::symlink_status(folder)))throw std::runtime_error("audio directory must not be a symlink");
            fs::create_directories(folder);
            auto extension=options.source.extension().string();
            if(extension.size()>12 || !std::all_of(extension.begin(),extension.end(),[](unsigned char c){return std::isalnum(c)!=0 || c=='.';}))extension=".bin";
            const auto hash=media::blake3File(source);
            if(!hash)throw std::runtime_error("cannot verify copied import");
            files->hash=hash.hex;files->target=folder/("import-"+std::to_string(imported.mediaId)+"-"+hash.hex.substr(0,16)+extension);
            if(imported.existing) {
                mediaRequest.opType="media.relink";
                mediaRequest.payload={{"id",imported.mediaId},{"hash",files->hash},{"paths",Payload::object()}};
            }
            paths(mediaRequest,store,files->target);
        }
        if(!mediaRequest.opType.empty())result.edit.requests.push_back(std::move(mediaRequest));
        edit::TimeMap time(store.db());
        if((options.at.base!=0 && options.at.base!=1) || options.at.value<0)throw std::runtime_error("invalid import placement");
        const auto beginNs=options.at.base==0?time.nanos(options.at.value):options.at.value;
        const auto duration=edit::nanosFromSeconds(static_cast<double>(info.frames)/static_cast<double>(info.sampleRate));
        const auto beginTicks=options.at.base==0?options.at.value:time.ticks(beginNs);
        const auto tickSpan=warped?warped->markers.back().tick:time.ticks(edit::addTime(beginNs,duration))-beginTicks;
        const int base=warped?0:options.at.base;
        if(warped) {
            engine::TempoMap tempo;
            SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
            while(points.executeStep())tempo.events.push_back({points.getColumn(0).getInt64(),points.getColumn(1).getDouble(),points.getColumn(2).getInt()});
            if(tempo.events.empty())tempo.events.push_back({0,120,0});
            const auto projected=engine::projectWarpAnchors(warped->markers,tempo,beginTicks,referenceRate);
            if(projected.back().outputFrame>0x7fffffffULL)throw std::runtime_error("warped clip exceeds supported render duration");
        }
        Payload values={{"id",options.clipId},{"track_id",options.track},{"kind","audio"},
                        {"name",media::pathUtf8(options.source.stem())},{"time_base",base}};
        if(base==0){values["pos_ticks"]=beginTicks;values["length_ticks"]=tickSpan;}
        else {values["pos_ns"]=beginNs;values["length_ns"]=duration;}
        if(options.edgeFades) {
            const auto placedDuration=warped?time.duration(beginTicks,tickSpan):duration;
            const auto fade=std::min(tickSpan/2,time.ticks(edit::addTime(beginNs,std::min<std::int64_t>(placedDuration/2,5000000)))-beginTicks);
            values["fade_in_ticks"]=fade;values["fade_out_ticks"]=fade;
        }
        auto root=edit::defaultRow(store.db(),"clips",values);
        Payload image={{"id",options.clipId},{"row",root},{"children",Payload::object()},{"alias_links",Payload::array()}};
        Payload sourceValues={{"clip_id",options.clipId},{"media_id",imported.mediaId},{"src_len_frames",info.frames}};
        Payload playbackValues={{"clip_id",options.clipId}};
        if(warped) {
            sourceValues["src_start_frames"]=static_cast<std::int64_t>(warped->trimFrames);sourceValues["src_len_frames"]=info.frames-static_cast<std::int64_t>(warped->trimFrames);
            sourceValues["warp_enabled"]=1;sourceValues["warp_mode"]=audio::warpModeName(options.warpMode);
            std::vector<std::uint8_t> bytes(markers->bytes.size());
            if(!bytes.empty())std::memcpy(bytes.data(),markers->bytes.data(),bytes.size());
            sourceValues["warp_markers"]=Payload::binary(std::move(bytes));
        } else {playbackValues["content_offset_ns"]=0;playbackValues["source_end_ns"]=duration;}
        image["children"]["audio_clips"]=Payload::array({edit::defaultRow(store.db(),"audio_clips",sourceValues)});
        image["children"]["clip_playback_details"]=Payload::array({edit::defaultRow(store.db(),"clip_playback_details",playbackValues)});
        if(saved)applySampleDefaults(store,image,*saved,referenceRate);
        edit::RowEdits plan(store.db());createImage(plan,image);
        OpRequest request;request.opType="clip.duplicate";request.payload={{"id",options.clipId},{"changes",plan.changes()}};
        request.label="Import Audio";request.targetKind="clip";request.targetId=options.clipId;result.edit.requests.push_back(std::move(request));
        if(options.stop.stop_requested())throw std::runtime_error("import cancelled");
        result.edit.ok=true;
    } catch(const std::exception& exception){result.edit.error=exception.what();}
    return result;
}
MaterialEdit prepareBounce(Store& store,const BounceOptions& options,engine::DeviceLoader loader,render::Control control) {
    MaterialEdit result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        const auto expected=OpJournal(store).headSeq();
        auto files=stage(media::projectFolder(store));
        {
            SQLite::Transaction read(store.db(),SQLite::TransactionBehavior::DEFERRED);
            if(OpJournal(store).headSeq()!=expected)throw std::runtime_error("project changed before bounce capture");
            snapshot(store,files->directory/"capture.adi");
        }
        auto job=options.job;job.project=files->directory/"capture.adi";job.directory=files->directory;
        job.nameTemplate="bounce";job.regionId.reset();
        const auto rendered=render::renderWav(job,std::move(loader),control);
        if(rendered.state!=render::JobState::Completed)throw std::runtime_error(rendered.error.empty()?"bounce cancelled":rendered.error);
        if(control.stop.stop_requested())throw std::runtime_error("bounce cancelled");
        auto destination=options.destination;destination.source=rendered.output;destination.copyIn=true;destination.warp=false;destination.stop=control.stop;
        result=prepareImportAudio(store,destination,job.session.sampleRate);
        if(!result.edit.ok)return result;
        result.edit.options.expectHead.emplace(expected);
        result.files->stop=control.stop;
        if(options.replace) {
            const auto remove=prepareDeleteRange(store,*options.replace,job.session.sampleRate,options.destination.clipId);
            if(!remove.ok)throw std::runtime_error(remove.error);
            result.edit.requests.insert(result.edit.requests.begin(),remove.requests.begin(),remove.requests.end());
        }
        for(auto& request:result.edit.requests)request.label="Bounce";
    } catch(const std::exception& exception){result.edit.ok=false;result.edit.error=exception.what();}
    return result;
}
CommitResult commitMaterial(Store& store,MaterialEdit& material,Actor actor) {
    auto prepared=material.edit;
    const auto files=material.files;
    if(files && files->committed){CommitResult result;result.error="material edit was already committed";return result;}
    if(files && files->stop.stop_requested()){CommitResult result;result.error="operation cancelled";return result;}
    if(files) {
        const auto previous=prepared.options.beforeCommit;
        prepared.options.beforeCommit=[files,previous](SQLite::Database& db,std::int64_t transaction,std::string& error) {
            if(previous && !previous(db,transaction,error))return false;
            if(files->stop.stop_requested()){error="operation cancelled";return false;}
            const auto hash=media::blake3File(files->staged,files->stop);
            if(!hash || hash.hex!=files->hash){error="staged media changed after capture";return false;}
            if(files->target.empty())return true;
            // publishFile moves its source. Retain the only captured render for
            // recovery when a deferred SQL constraint rejects COMMIT afterward.
            const auto publication=files->directory/"publication.tmp";
            std::error_code cleanup;fs::remove(publication,cleanup);
            fs::copy_file(files->staged,publication,fs::copy_options::none);
            const auto copied=media::blake3File(publication,files->stop);
            if(!copied || copied.hex!=files->hash){error="material changed while staging publication";return false;}
            const auto published=media::publishFile(publication,files->target);
            if(published.status==media::PublishStatus::exists) {
                // Undo retains owned media for redo. A new import with the
                // same content/id may safely reference that existing regular
                // file; no bytes are replaced and failure must not remove it.
                std::error_code statusError;const auto status=fs::symlink_status(files->target,statusError);
                if(!statusError && fs::is_regular_file(status)) {
                    const auto existing=media::blake3File(files->target);
                    if(existing && existing.hex==files->hash)return true;
                }
            }
            if(published.status!=media::PublishStatus::published){error="publish imported media "+media::pathUtf8(files->target)+": "+(published.status==media::PublishStatus::exists?"file exists":published.error);return false;}
            files->published=true;return true;
        };
    }
    auto result=commitPrepared(store,prepared,actor);
    if(files) {
        if(result.ok)files->committed=true;
        else if(files->published && !files->committed) {std::error_code ec;fs::remove(files->target,ec);files->published=false;}
    }
    return result;
}
} // namespace adi::clip_edit
