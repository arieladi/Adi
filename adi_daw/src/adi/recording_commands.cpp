// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/recording_commands.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <cmath>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <limits>
#include <stdexcept>
namespace adi::recording {
namespace fs=std::filesystem;
struct RecordingEdit::Files {
    struct Asset {fs::path source,staged,target;std::string hash;bool published=false;};
    std::vector<Asset> assets;
    engine::AudioRecordingResult source;
    fs::path publicationDirectory;
    ~Files(){if(!publicationDirectory.empty()){std::error_code ignored;fs::remove_all(publicationDirectory,ignored);}}
    bool committed=false,keepPrivate=false;
};
namespace {
std::string identity(Store& store) {
    SQLite::Statement query(store.db(),"PRAGMA database_list");
    while(query.executeStep())if(query.getColumn(1).getString()=="main")return query.getColumn(2).getString();
    throw std::runtime_error("recording needs a project connection");
}
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db){if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);}
};
fs::path reservePublication(const fs::path& parent) {
    static std::atomic<std::uint64_t> serial{0};
    for(unsigned attempt=0;attempt<128;++attempt) {
        const auto path=parent/(".publication-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
        std::error_code error;if(fs::create_directory(path,error))return path;
        if(error && error!=std::errc::file_exists)throw fs::filesystem_error("stage recording publication",path,error);
    }
    throw std::runtime_error("cannot reserve recording publication staging");
}
engine::TempoMap tempoOf(Store& store) {
    engine::TempoMap result;SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
    while(points.executeStep()) {
        const auto at=points.getColumn(0).getInt64();const auto bpm=points.getColumn(1).getDouble();
        if(at<0 || !std::isfinite(bpm) || bpm<=0 || bpm>=1000)throw std::runtime_error("recording placement needs a valid tempo map");
        result.events.push_back({at,bpm,points.getColumn(2).getInt()});
    }
    if(result.events.empty() || result.events.front().posTicks!=0)result.events.insert(result.events.begin(),{0,120,0});
    return result;
}
std::int64_t tickAt(const engine::TempoMap& tempo,std::int64_t sample,std::uint32_t rate) {
    if(sample<0 || sample>(std::int64_t{1}<<50))throw std::runtime_error("recording exceeds supported timeline sample precision");
    const auto tick=tempo.secondsToTicks(static_cast<double>(sample)/rate);
    const double replayed=tempo.ticksToSeconds(tick)*rate;
    if(!std::isfinite(replayed) || replayed<0 || replayed>0x1p50 || std::llround(replayed)!=sample)
        throw std::runtime_error("recording boundary cannot preserve its sample on this musical clock");
    return tick;
}
}
RecordingEdit prepareCommit(Store& store,const engine::AudioRecordingResult& recorded,std::span<const ClipIds> ids) {
    RecordingEdit result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        Read read(store.db());result.sourceProject=identity(store);
        if(recorded.sourceProject().empty() || recorded.sourceProject()!=result.sourceProject)throw std::runtime_error("recording belongs to a different project");
        if(recorded.failure!=engine::RecordingFailure::None || recorded.frames==0 || recorded.sampleRate<8000 || recorded.sampleRate>768000 ||
           recorded.startSample<0 || recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-recorded.startSample) ||
           ids.size()!=recorded.tracks.size() || ids.empty() || recorded.directory().empty())throw std::runtime_error("recording is not a completed attachable take");
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        const auto tempo=tempoOf(store);const auto first=tickAt(tempo,recorded.startSample,recorded.sampleRate);
        const auto last=tickAt(tempo,recorded.startSample+static_cast<std::int64_t>(recorded.frames),recorded.sampleRate);
        if(last<=first)throw std::runtime_error("recording has no representable musical extent");
        auto files=std::make_shared<RecordingEdit::Files>();files->source=recorded;result.files_=files;files->publicationDirectory=reservePublication(recorded.directory());
        const auto folder=media::projectFolder(store)/"audio";
        if(fs::is_symlink(fs::symlink_status(folder)))throw std::runtime_error("recording audio directory must not be a symlink");
        fs::create_directories(folder);
        std::set<std::int64_t> clipIds,mediaIds;std::map<std::string,std::int64_t> capturedMedia;
        for(std::size_t index=0;index<ids.size();++index) {
            const auto& target=recorded.tracks[index];const auto& wanted=ids[index];
            if(wanted.clip<=0 || wanted.media<=0 || !clipIds.insert(wanted.clip).second || !mediaIds.insert(wanted.media).second)
                throw std::runtime_error("recording needs distinct positive clip and media allocation ids");
            const auto expected=recorded.directory()/("take-"+std::to_string(index)+".wav");
            if(target.file!=expected || !fs::is_regular_file(fs::symlink_status(expected)))throw std::runtime_error("recording file is not its owned regular take");
            SQLite::Statement track(store.db(),"SELECT kind,locked FROM tracks WHERE id=?");track.bind(1,target.trackId);
            if(!track.executeStep() || (track.getColumn(0).getString()!="audio" && track.getColumn(0).getString()!="midi") || track.getColumn(1).getInt()!=0)
                throw std::runtime_error("recording target is gone, locked or not a regular track");
            {audio::WavReader source(expected);if(source.frames()!=recorded.frames || source.sampleRate()!=recorded.sampleRate || source.channels()!=target.channels)
                throw std::runtime_error("recording dimensions changed before attachment");}
            const auto hash=media::blake3File(expected);if(!hash)throw std::runtime_error("cannot verify recorded audio");
            std::int64_t mediaId=0;
            if(const auto found=capturedMedia.find(hash.hex);found!=capturedMedia.end()) {
                mediaId=found->second;files->assets.push_back({expected,{},{},hash.hex,false});
            }
            else {
                OpRequest request;const auto imported=media::prepareImport(store,expected,wanted.media,0,request);
                if(!imported.ok || !imported.audio)throw std::runtime_error(imported.error.empty()?"cannot capture recorded media":imported.error);
                if(imported.audio->frames!=static_cast<std::int64_t>(recorded.frames) || imported.audio->sampleRate!=recorded.sampleRate || imported.audio->channels!=target.channels)
                    throw std::runtime_error("recording dimensions changed during media capture");
                mediaId=imported.mediaId;
                const auto capturedHash=request.opType=="media.import"?request.payload.at("row").at("hash_blake3"):media::mediaRow(store.db(),mediaId).at("hash_blake3");
                if(capturedHash!=hash.hex)throw std::runtime_error("recorded file changed during media capture");
                if(imported.existing) {
                    std::string pathError;const auto oldPath=media::resolveMedia(media::projectFolder(store),media::mediaRow(store.db(),mediaId),pathError);
                    if(!oldPath.empty() && fs::weakly_canonical(oldPath.parent_path())==fs::weakly_canonical(recorded.directory()))files->keepPrivate=true;
                }
                capturedMedia.emplace(hash.hex,mediaId);
                const auto targetFile=folder/("recording-"+std::to_string(mediaId)+"-"+hash.hex.substr(0,16)+".wav");
                const auto relative=media::pathUtf8(fs::relative(targetFile,media::projectFolder(store)));
                Payload paths={{"rel_path",relative},{"abs_path_hint",nullptr},{"missing",0}};
                if(request.opType=="media.import")request.payload["row"].update(paths);
                else {request.opType="media.relink";request.payload={{"id",mediaId},{"hash",hash.hex},{"paths",paths}};}
                const auto staged=files->publicationDirectory/("asset-"+std::to_string(index)+".wav");fs::copy_file(expected,staged,fs::copy_options::none);
                const auto stagedHash=media::blake3File(staged);if(!stagedHash || stagedHash.hex!=hash.hex)throw std::runtime_error("recording changed while staging publication");
                files->assets.push_back({expected,staged,targetFile,hash.hex,false});request.label="Record Audio";result.requests.push_back(std::move(request));
            }
            OpRequest clip;clip.opType="clip.create";clip.payload={{"id",wanted.clip},{"track",target.trackId},{"kind","audio"},{"name",wanted.name.empty()?"Recording":wanted.name},{"pos",first},{"length",last-first}};
            clip.label="Record Audio";clip.targetKind="clip";clip.targetId=wanted.clip;result.requests.push_back(std::move(clip));
            OpRequest attach;attach.opType="clip.attachAudio";attach.payload={{"id",wanted.clip},{"media",mediaId},{"frames",static_cast<std::int64_t>(recorded.frames)}};
            attach.label="Record Audio";attach.targetKind="clip";attach.targetId=wanted.clip;result.requests.push_back(std::move(attach));
        }
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
CommitResult commit(Store& store,RecordingEdit& edit,Actor actor) {
    CommitResult refused;
    if(!edit.ok){refused.error=edit.error;return refused;}
    try {if(identity(store)!=edit.sourceProject){refused.error="recording belongs to a different project";return refused;}}
    catch(const std::exception& exception){refused.error=exception.what();return refused;}
    const auto files=edit.files_;
    if(!files || files->committed){refused.error="recording edit is absent or already committed";return refused;}
    auto options=edit.options;const auto previous=options.beforeCommit;
    options.beforeCommit=[files,previous](SQLite::Database& db,std::int64_t transaction,std::string& error) {
        if(previous && !previous(db,transaction,error))return false;
        for(auto& asset:files->assets) {
            if(!fs::is_regular_file(fs::symlink_status(asset.source))){error="recorded take is no longer a regular file";return false;}
            const auto hash=media::blake3File(asset.source);if(!hash || hash.hex!=asset.hash){error="recorded file changed after capture";return false;}
            if(asset.target.empty())continue;
            const auto stagedHash=media::blake3File(asset.staged);
            if(!stagedHash || stagedHash.hex!=asset.hash){error="recording publication staging changed after capture";return false;}
            const auto published=media::publishFile(asset.staged,asset.target);
            if(published.status==media::PublishStatus::exists && fs::is_regular_file(fs::symlink_status(asset.target))) {
                const auto existing=media::blake3File(asset.target);if(existing && existing.hex==asset.hash)continue;
            }
            if(published.status!=media::PublishStatus::published){error="Cannot publish recorded audio "+media::pathUtf8(asset.target)+": "+(published.status==media::PublishStatus::exists?"destination contains different content":published.error);return false;}
            asset.published=true;
        }
        return true;
    };
    auto requests=edit.requests;for(auto& request:requests)request.actor=actor;
    auto result=OpJournal(store).commit(requests,options);
    if(result.ok) {
        files->committed=true;
        try{if(!files->keepPrivate)files->source.discard();}catch(...){/* committed assets remain authoritative */}
    }
    else for(auto& asset:files->assets)if(asset.published){std::error_code cleanup;fs::remove(asset.target,cleanup);asset.published=false;}
    return result;
}
}
