// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/material_commands.hpp"
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
    SQLite::Database output(target.string(),SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
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
    MaterialEdit result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(options.warp)throw std::runtime_error("Warp on import needs tempo detection and the warp renderer");
        if(!std::isfinite(referenceRate) || referenceRate<=0 || referenceRate>768000)
            throw std::runtime_error("invalid reference render rate");
        SQLite::Transaction read(store.db(),SQLite::TransactionBehavior::DEFERRED);
        result.edit.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement identity(store.db(),"PRAGMA database_list");
        while(identity.executeStep())if(identity.getColumn(1).getString()=="main")result.edit.sourceFile=identity.getColumn(2).getString();
        auto source=options.source.is_absolute()?options.source:media::projectFolder(store)/options.source;
        auto files=stage(media::projectFolder(store));result.files=files;
        if(options.copyIn) {
            files->staged=files->directory/source.filename();
            fs::copy_file(source,files->staged,fs::copy_options::none);source=files->staged;
        }
        OpRequest mediaRequest;
        const auto imported=media::prepareImport(store,source,options.mediaId,options.importedUtc,mediaRequest);
        if(!imported.ok)throw std::runtime_error(imported.error);
        if(!imported.audio)throw std::runtime_error("import prompt needs a supported audio file; video playback is not built");
        const auto& audio=*imported.audio;
        if(options.copyIn && !mediaRequest.opType.empty()) {
            const auto folder=media::projectFolder(store)/"audio";
            if(fs::is_symlink(fs::symlink_status(folder)))throw std::runtime_error("audio directory must not be a symlink");
            fs::create_directories(folder);
            auto extension=options.source.extension().string();
            if(extension.size()>12 || !std::all_of(extension.begin(),extension.end(),[](unsigned char c){return std::isalnum(c)!=0 || c=='.';}))extension=".bin";
            const auto hash=media::blake3File(source);
            if(!hash)throw std::runtime_error("cannot verify copied import");
            files->hash=hash.hex;files->target=folder/("import-"+std::to_string(imported.mediaId)+"-"+hash.hex.substr(0,16)+extension);
            paths(mediaRequest,store,files->target);
        }
        if(!mediaRequest.opType.empty())result.edit.requests.push_back(std::move(mediaRequest));
        edit::TimeMap time(store.db());
        if((options.at.base!=0 && options.at.base!=1) || options.at.value<0)throw std::runtime_error("invalid import placement");
        const auto beginNs=options.at.base==0?time.nanos(options.at.value):options.at.value;
        const auto duration=edit::nanosFromSeconds(static_cast<double>(audio.frames)/static_cast<double>(audio.sampleRate));
        const auto beginTicks=options.at.base==0?options.at.value:time.ticks(beginNs);
        const auto tickSpan=time.ticks(edit::addTime(beginNs,duration))-beginTicks;
        Payload values={{"id",options.clipId},{"track_id",options.track},{"kind","audio"},
                        {"name",media::pathUtf8(options.source.stem())},{"time_base",options.at.base}};
        if(options.at.base==0){values["pos_ticks"]=options.at.value;values["length_ticks"]=tickSpan;}
        else {values["pos_ns"]=beginNs;values["length_ns"]=duration;}
        if(options.edgeFades) {
            const auto fade=std::min(tickSpan/2,time.ticks(edit::addTime(beginNs,std::min<std::int64_t>(duration/2,5000000)))-beginTicks);
            values["fade_in_ticks"]=fade;values["fade_out_ticks"]=fade;
        }
        auto root=edit::defaultRow(store.db(),"clips",values);
        Payload image={{"id",options.clipId},{"row",root},{"children",Payload::object()},{"alias_links",Payload::array()}};
        image["children"]["audio_clips"]=Payload::array({edit::defaultRow(store.db(),"audio_clips",
            {{"clip_id",options.clipId},{"media_id",imported.mediaId},{"src_len_frames",audio.frames}})});
        image["children"]["clip_playback_details"]=Payload::array({edit::defaultRow(store.db(),"clip_playback_details",
            {{"clip_id",options.clipId},{"content_offset_ns",0},{"source_end_ns",duration}})});
        edit::RowEdits plan(store.db());createImage(plan,image);
        OpRequest request;request.opType="clip.duplicate";request.payload={{"id",options.clipId},{"changes",plan.changes()}};
        request.label="Import Audio";request.targetKind="clip";request.targetId=options.clipId;result.edit.requests.push_back(std::move(request));
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
        auto destination=options.destination;destination.source=rendered.output;destination.copyIn=true;
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
    if(files && !files->target.empty()) {
        const auto previous=prepared.options.beforeCommit;
        prepared.options.beforeCommit=[files,previous](SQLite::Database& db,std::int64_t transaction,std::string& error) {
            if(previous && !previous(db,transaction,error))return false;
            if(files->stop.stop_requested()){error="bounce cancelled";return false;}
            const auto hash=media::blake3File(files->staged);
            if(!hash || hash.hex!=files->hash){error="staged media changed after capture";return false;}
            const auto published=media::publishFile(files->staged,files->target);
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
