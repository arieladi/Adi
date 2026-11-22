// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/recording_commands.hpp"
#include "adi/engine/audio_memory.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/media/published_rollback.hpp"
#include "adi/store.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_time.hpp"
#include "adi/take_lane_ops.hpp"
#include <algorithm>
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
    struct Asset {fs::path source,staged,target;std::string hash;};
    std::vector<Asset> assets;
    engine::AudioRecordingResult source;
    fs::path publicationDirectory;
    ~Files(){if(!publicationDirectory.empty()){std::error_code ignored;fs::remove_all(publicationDirectory,ignored);}}
    bool committed=false,keepPrivate=false;
};
struct RecordingEdit::PreparedSource {
    std::int64_t mediaId;
    std::optional<OpRequest> mediaEdit;
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
        const auto path=parent/(".adi-media-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(serial.fetch_add(1)));
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
engine::AudioRecordingResult captureMemory(Store& store,const engine::AudioMemorySnapshot& memory,
    std::int64_t track,std::int64_t timelineStartSample,const fs::path& workFolder) {
    try {
        if(audio::onAudioThread)throw std::runtime_error("capture audio memory off callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        return engine::AudioRecordingResult::fromMemory(memory,{workFolder,identity(store),track,timelineStartSample});
    }catch(const std::exception& exception) {
        engine::AudioRecordingResult result;result.failure=engine::RecordingFailure::InvalidCapture;result.error=exception.what();return result;
    }
}
std::vector<RecordingEdit::PreparedSource> RecordingEdit::prepareSources(Store& store,const engine::AudioRecordingResult& recorded,
    std::span<const ClipIds> ids,RecordingEdit& result) {
    if(store.readOnly())throw std::runtime_error("project is read-only");
    if(audio::onAudioThread)throw std::runtime_error("prepare recording off callback");
    result.sourceProject=identity(store);result.owner_=&store;result.client_=store.clientId();
    if(recorded.sourceProject().empty() || recorded.sourceProject()!=result.sourceProject)throw std::runtime_error("recording belongs to a different project");
    if(!recorded.hasAttachableAudio() || recorded.sampleRate<8000 || recorded.sampleRate>768000 ||
       recorded.startSample<0 || recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-recorded.startSample) ||
       ids.size()!=recorded.tracks.size() || ids.empty() || recorded.directory().empty())throw std::runtime_error("recording is not a completed attachable take");
    result.options.expectHead.emplace(OpJournal(store).headSeq());
    const auto folder=media::projectFolder(store)/"audio";
    if(fs::is_symlink(fs::symlink_status(folder)))throw std::runtime_error("recording audio directory must not be a symlink");
    fs::create_directories(folder);
    auto files=std::make_shared<RecordingEdit::Files>();files->source=recorded;result.files_=files;files->publicationDirectory=reservePublication(folder);
    std::set<std::int64_t> clipIds,mediaIds;std::map<std::string,std::int64_t> capturedMedia;
    std::vector<PreparedSource> sources;
    sources.reserve(ids.size());
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
        std::optional<OpRequest> mediaEdit;
        if(const auto found=capturedMedia.find(hash.hex);found!=capturedMedia.end()) {
            mediaId=found->second;files->assets.push_back({expected,{},{},hash.hex});
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
            files->assets.push_back({expected,staged,targetFile,hash.hex});
            request.label="Record Audio";
            mediaEdit=std::move(request);
        }
        sources.push_back({mediaId,std::move(mediaEdit)});
    }
    return sources;
}
RecordingEdit prepareCommit(Store& store,const engine::AudioRecordingResult& recorded,std::span<const ClipIds> ids) {
    RecordingEdit result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(audio::onAudioThread)throw std::runtime_error("prepare recording off callback");
        if(recorded.loopSpec())throw std::runtime_error("loop recording needs take-pass publication; linear attachment is not permitted");
        Read read(store.db());
        const auto tempo=tempoOf(store);
        if(recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
           recorded.startSample<0 || recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-recorded.startSample) ||
           recorded.sampleRate<8000 || recorded.sampleRate>768000)
            throw std::runtime_error("recording is not a completed attachable take");
        const auto first=tickAt(tempo,recorded.startSample,recorded.sampleRate);
        const auto last=tickAt(tempo,recorded.startSample+static_cast<std::int64_t>(recorded.frames),recorded.sampleRate);
        if(last<=first)throw std::runtime_error("recording has no representable musical extent");
        auto sources=RecordingEdit::prepareSources(store,recorded,ids,result);
        for(std::size_t index=0;index<ids.size();++index) {
            const auto& target=recorded.tracks[index];const auto& wanted=ids[index];
            auto& source=sources[index];
            if(source.mediaEdit)result.requests.push_back(std::move(*source.mediaEdit));
            OpRequest clip;clip.opType="clip.create";clip.payload={{"id",wanted.clip},{"track",target.trackId},{"kind","audio"},{"name",wanted.name.empty()?"Recording":wanted.name},{"pos",first},{"length",last-first}};
            clip.label="Record Audio";clip.targetKind="clip";clip.targetId=wanted.clip;result.requests.push_back(std::move(clip));
            OpRequest attach;attach.opType="clip.attachAudio";attach.payload={{"id",wanted.clip},{"media",source.mediaId},{"frames",static_cast<std::int64_t>(recorded.frames)}};
            attach.label="Record Audio";attach.targetKind="clip";attach.targetId=wanted.clip;result.requests.push_back(std::move(attach));
        }
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
namespace {
// Existing lanes are considered in visible order, once per capture. A lane
// whose native extent cannot prove it is wholly before the pass is not reused.
struct ReusableLane {
    std::int64_t id=0,tickEnd=0,nanoEnd=0;
    bool valid=true,hasTicks=false,hasNanos=false,used=false;
};
ReusableLane inspectReusableLane(SQLite::Database& db,std::int64_t id,std::int64_t track) {
    ReusableLane lane;lane.id=id;
    SQLite::Statement clips(db,"SELECT track_id,time_base,pos_ticks,length_ticks,pos_ns,length_ns FROM clips WHERE lane_id=?");
    clips.bind(1,id);
    while(clips.executeStep()) {
        if(!clips.getColumn(0).isInteger() || clips.getColumn(0).getInt64()!=track || !clips.getColumn(1).isInteger()) {
            lane.valid=false;return lane;
        }
        const auto base=clips.getColumn(1).getInt64();
        if(base!=0 && base!=1){lane.valid=false;return lane;}
        const int position=base==0?2:4,length=position+1;
        if(!clips.getColumn(position).isInteger() || !clips.getColumn(length).isInteger()) {lane.valid=false;return lane;}
        const auto begin=clips.getColumn(position).getInt64(),count=clips.getColumn(length).getInt64();
        if(begin<0 || count<0 || begin>std::numeric_limits<std::int64_t>::max()-count) {lane.valid=false;return lane;}
        const auto end=edit::addTime(begin,count);
        if(base==0){lane.hasTicks=true;lane.tickEnd=std::max(lane.tickEnd,end);}
        else {lane.hasNanos=true;lane.nanoEnd=std::max(lane.nanoEnd,end);}
    }
    return lane;
}
std::optional<std::int64_t> captureStartNanos(std::int64_t sample,std::uint32_t rate) {
    // The clip's tick placement round-trips to this sample. Use the original
    // sample clock for physical extents, whose exact boundary can lie between ticks.
    constexpr std::int64_t billion=1000000000;
    const auto seconds=sample/rate,remainder=sample%rate;
    const auto fraction=remainder*billion/rate;
    if(seconds>(std::numeric_limits<std::int64_t>::max()-fraction)/billion)return {};
    return seconds*billion+fraction;
}
std::optional<std::int64_t> takeReusableLane(std::vector<ReusableLane>& lanes,
    std::int64_t tick,std::optional<std::int64_t> nanos) {
    for(auto& lane:lanes) {
        if(lane.used || !lane.valid || (lane.hasTicks && lane.tickEnd>tick) ||
            (lane.hasNanos && (!nanos || lane.nanoEnd>*nanos)))continue;
        lane.used=true;return lane.id;
    }
    return {};
}
struct RecordedPiece {
    std::int64_t id=0,begin=0,end=0;
    std::uint64_t source=0;
    std::string name;
};
Payload recordedImage(Store& store,const engine::TempoMap& tempo,const RecordedPiece& piece,
    std::int64_t track,std::int64_t media,std::uint32_t rate,std::optional<std::int64_t> lane={}) {
    const auto first=tickAt(tempo,piece.begin,rate),last=tickAt(tempo,piece.end,rate);
    if(last<=first)throw std::runtime_error("recorded pass has no representable musical extent");
    auto row=edit::defaultRow(store.db(),"clips",{{"id",piece.id},{"track_id",track},{"kind","audio"},
        {"name",piece.name},{"time_base",0},{"pos_ticks",first},{"length_ticks",last-first}});
    row["lane_id"]=lane?Payload(*lane):Payload{};
    auto audio=edit::defaultRow(store.db(),"audio_clips",{{"clip_id",piece.id},{"media_id",media},
        {"src_start_frames",static_cast<std::int64_t>(piece.source)},{"src_len_frames",piece.end-piece.begin}});
    return {{"id",piece.id},{"row",std::move(row)},
        {"children",{{"audio_clips",Payload::array({std::move(audio)})}}},{"alias_links",Payload::array()}};
}
// Both packets are complete changes from the same unchanged read snapshot.
// Derive a sequential native splice, without applying anything while preparing.
Payload passDelta(const Payload& previous,const Payload& next) {
    using Key=std::pair<std::string,std::string>;
    std::map<Key,std::pair<const Payload*,const Payload*>> rows;
    for(const auto& change:previous)rows[{change.at("table").get<std::string>(),change.at("id").dump()}].first=&change;
    for(const auto& change:next)rows[{change.at("table").get<std::string>(),change.at("id").dump()}].second=&change;
    Payload result=Payload::array();
    for(const auto& [key,sides]:rows) {
        (void)key;const auto* old=sides.first;const auto* now=sides.second;
        if(old && now && old->at("before")!=now->at("before"))throw std::runtime_error("recording placement snapshots differ");
        const auto& before=old?old->at("after"):now->at("before");
        const auto& after=now?now->at("after"):old->at("before");
        if(before==after)continue;
        const auto& identity=now?*now:*old;
        result.push_back({{"table",identity.at("table")},{"id",identity.at("id")},{"before",before},{"after",after}});
    }
    return result;
}
void replaceRecordedSpan(std::vector<RecordedPiece>& pieces,RecordedPiece incoming,clip_edit::CopyIds& ids) {
    std::vector<RecordedPiece> after;
    for(const auto& piece:pieces) {
        if(piece.end<=incoming.begin || piece.begin>=incoming.end){after.push_back(piece);continue;}
        if(piece.begin<incoming.begin){auto left=piece;left.end=incoming.begin;after.push_back(std::move(left));}
        if(piece.end>incoming.end) {
            auto right=piece;right.source+=static_cast<std::uint64_t>(incoming.end-piece.begin);right.begin=incoming.end;
            if(piece.begin<incoming.begin)right.id=ids.next("clips");
            after.push_back(std::move(right));
        }
    }
    after.push_back(std::move(incoming));pieces=std::move(after);
}
}
RecordingEdit prepareLoopCommit(Store& store,const engine::AudioRecordingResult& recorded,
    std::span<const LoopTrackIds> ids,std::span<const std::optional<Payload>> selections) {
    RecordingEdit result;
    try {
        if(audio::onAudioThread)throw std::runtime_error("prepare recording off callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        Read read(store.db());const auto& loop=recorded.loopSpec();const auto passes=recorded.passes();
        if(recorded.sampleRate<8000 || recorded.sampleRate>768000 || !loop || passes.empty() || passes.size()>loop->maxPasses || loop->beginSample<0 || loop->endSample<=loop->beginSample ||
           ids.empty() || ids.size()!=recorded.tracks.size() || (!selections.empty() && selections.size()!=passes.size()))
            throw std::runtime_error("loop recording needs its complete pass log, target ids and optional per-pass selection");
        std::uint64_t consumed=0;
        for(std::size_t pass=0;pass<passes.size();++pass) {
            const auto& p=passes[pass];
            if(p.frames==0 || p.sourceStartFrame!=consumed || consumed>recorded.frames || p.frames>recorded.frames-consumed ||
               p.timelineStartSample<0 || p.timelineEndSample<=p.timelineStartSample || p.timelineEndSample>loop->endSample ||
               static_cast<std::uint64_t>(p.timelineEndSample-p.timelineStartSample)!=p.frames ||
               (pass==0?p.timelineStartSample!=recorded.startSample:
                   (p.timelineStartSample!=loop->beginSample || passes[pass-1].timelineEndSample!=loop->endSample)))
                throw std::runtime_error("loop pass log is not a contiguous capture with exact wrapped intervals");
            consumed+=p.frames;
        }
        if(consumed!=recorded.frames || recorded.frames>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("loop pass log does not cover its complete source");
        clip_edit::CopyIds allocated(store.db());
        std::set<std::int64_t> lanes,clips,tracks;
        std::vector<ClipIds> sources;std::vector<std::int64_t> trackIds;
        std::vector<std::vector<std::int64_t>> laneOrders(ids.size());
        std::vector<std::vector<ReusableLane>> reusable(ids.size());
        std::vector<std::size_t> newLaneCounts(ids.size());
        for(std::size_t track=0;track<ids.size();++track) {
            const auto& wanted=ids[track];const auto trackId=recorded.tracks[track].trackId;
            if(wanted.passes.size()!=passes.size() || !tracks.insert(trackId).second)
                throw std::runtime_error("loop materialization needs distinct targets and one fallback lane/new clip per pass");
            for(const auto& pass:wanted.passes) {
                if(pass.lane<=0 || pass.clip<=0 || !lanes.insert(pass.lane).second || !clips.insert(pass.clip).second ||
                   edit::row(store.db(),"lanes",pass.lane) || edit::row(store.db(),"clips",pass.clip))
                    throw std::runtime_error("loop materialization needs unoccupied distinct positive lane and clip ids");
                allocated.reserve("clips",pass.clip);
            }
            sources.push_back({wanted.passes.front().clip,wanted.media,{}});trackIds.push_back(trackId);
            SQLite::Statement existing(store.db(),"SELECT id FROM lanes WHERE track_id=? AND kind='take' ORDER BY ord,id");
            existing.bind(1,trackId);
            while(existing.executeStep()) {
                const auto id=existing.getColumn(0).getInt64();
                laneOrders[track].push_back(id);
                reusable[track].push_back(inspectReusableLane(store.db(),id,trackId));
            }
        }
        const auto tempo=tempoOf(store);
        for(const auto& pass:passes) {
            (void)tickAt(tempo,pass.timelineStartSample,recorded.sampleRate);
            (void)tickAt(tempo,pass.timelineEndSample,recorded.sampleRate);
        }
        auto sourceEdits=RecordingEdit::prepareSources(store,recorded,sources,result);
        for(auto& source:sourceEdits)
            if(source.mediaEdit)result.requests.push_back(std::move(*source.mediaEdit));
        std::vector<std::vector<RecordedPiece>> main(ids.size());Payload previous=Payload::array();
        auto earliest=passes.front().timelineStartSample;auto latest=passes.front().timelineEndSample;
        edit_plan::Options placement{static_cast<double>(recorded.sampleRate),{}};
        placement.policy.automationFollowsClips=false; // Audio recording does not rewrite authored automation.
        for(std::size_t pass=0;pass<passes.size();++pass) {
            const auto groupStart=pass==0?std::size_t{0}:result.requests.size();
            const auto& captured=passes[pass];earliest=std::min(earliest,captured.timelineStartSample);latest=std::max(latest,captured.timelineEndSample);
            edit_plan::detail::PlacementBatch batch;
            const auto first=tickAt(tempo,earliest,recorded.sampleRate),last=tickAt(tempo,latest,recorded.sampleRate);
            auto scope=edit_plan::materialScope(trackIds,edit_plan::PlacementClock::Ticks,{first,last});
            scope.domains=edit_plan::Domains::Clips;
            const auto startTick=tickAt(tempo,captured.timelineStartSample,recorded.sampleRate);
            const auto startNanos=captureStartNanos(captured.timelineStartSample,recorded.sampleRate);
            for(std::size_t track=0;track<ids.size();++track) {
                const auto& wanted=ids[track].passes[pass];const auto trackId=recorded.tracks[track].trackId;
                const auto name=wanted.name.empty()?(pass==0?std::string("Audio"):"Audio "+std::to_string(pass+1)):wanted.name;
                const auto reused=takeReusableLane(reusable[track],startTick,startNanos);
                const auto laneId=reused.value_or(wanted.lane);
                if(!reused) {
                    result.requests.push_back(createTakeLane(store,laneId,trackId));
                    auto& lanesInOrder=laneOrders[track];auto& created=newLaneCounts[track];
                    lanesInOrder.insert(lanesInOrder.begin()+static_cast<std::ptrdiff_t>(created),laneId);
                    if(created!=0) {
                        OpRequest order;order.opType="lane.reorder";
                        order.payload={{"track",trackId},{"lanes",lanesInOrder}};
                        result.requests.push_back(std::move(order));
                    }
                    ++created;
                }
                RecordedPiece take{wanted.clip,captured.timelineStartSample,captured.timelineEndSample,captured.sourceStartFrame,name};
                edit::RowEdits takeRows(store.db());
                clip_edit::createImage(takeRows,recordedImage(store,tempo,take,trackId,sourceEdits[track].mediaId,recorded.sampleRate,laneId));
                OpRequest clip;clip.opType="clip.duplicate";clip.payload={{"id",wanted.clip},{"changes",takeRows.changes()}};
                clip.targetKind="clip";clip.targetId=wanted.clip;result.requests.push_back(std::move(clip));
                take.id=allocated.next("clips");replaceRecordedSpan(main[track],std::move(take),allocated);
                for(const auto& piece:main[track])batch.incoming.push_back(recordedImage(store,tempo,piece,trackId,sourceEdits[track].mediaId,recorded.sampleRate));
                batch.clear.push_back({{trackId,std::nullopt},{first,last}});
            }
            auto prepared=edit_plan::detail::prepareCaptured(store,scope,std::move(batch),placement,allocated,"Record");
            if(!prepared.ok)throw std::runtime_error(prepared.error);
            Payload cumulative=Payload::array();
            for(const auto& request:prepared.requests) {
                if(request.opType!="arranger.splice" || request.payload.contains("loop"))throw std::runtime_error("unexpected recording placement operation");
                for(const auto& change:request.payload.at("changes"))cumulative.push_back(change);
            }
            auto delta=passDelta(previous,cumulative);
            if(delta.empty())throw std::runtime_error("recorded pass has no audible placement");
            OpRequest splice;splice.opType="arranger.splice";splice.payload={{"id",0},{"changes",std::move(delta)}};
            result.requests.push_back(std::move(splice));previous=std::move(cumulative);
            for(auto index=groupStart;index<result.requests.size();++index)result.requests[index].label="Record";
            if(!selections.empty())result.requests[groupStart].selBefore=selections[pass];
            result.groupEnds_.push_back(result.requests.size());
        }
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();result.requests.clear();result.groupEnds_.clear();}
    return result;
}

CommitResult commit(Store& store,RecordingEdit& edit,Actor actor) {
    CommitResult refused;
    if(!edit.ok){refused.error=edit.error;return refused;}
    if(audio::onAudioThread || edit.owner_!=&store || edit.client_!=store.clientId()) {
        refused.error="recording edit belongs to another open document or callback";return refused;
    }
    try {if(identity(store)!=edit.sourceProject){refused.error="recording belongs to a different project";return refused;}}
    catch(const std::exception& exception){refused.error=exception.what();return refused;}
    const auto files=edit.files_;
    if(!files || files->committed){refused.error="recording edit is absent or already committed";return refused;}
    auto rollback=std::make_shared<media::detail::PublishedRollback>();
    rollback->files.reserve(files->assets.size());
    auto options=edit.options;const auto previous=options.beforeCommit;
    options.beforeCommit=[files,previous,rollback](SQLite::Database& db,std::int64_t transaction,std::string& error) {
        if(previous && !previous(db,transaction,error))return false;
        for(const auto& asset:files->assets) {
            if(!fs::is_regular_file(fs::symlink_status(asset.source))){error="recorded take is no longer a regular file";return false;}
            const auto hash=media::blake3File(asset.source);if(!hash || hash.hex!=asset.hash){error="recorded file changed after capture";return false;}
            if(asset.target.empty())continue;
            const auto stagedHash=media::blake3File(asset.staged);
            if(!stagedHash || stagedHash.hex!=asset.hash){error="recording publication staging changed after capture";return false;}
            rollback->files.push_back({asset.target,{}});
            const auto published = media::publishFile(asset.staged, asset.target, {},
                                                      media::PublishPolicy::AtomicOnly);
            if(published.status==media::PublishStatus::published) {
                rollback->files.back().identity=published.identity;
                continue;
            }
            rollback->files.pop_back();
            if(published.status==media::PublishStatus::exists && fs::is_regular_file(fs::symlink_status(asset.target))) {
                const auto existing=media::blake3File(asset.target);if(existing && existing.hex==asset.hash)continue;
            }
            error="Cannot publish recorded audio "+media::pathUtf8(asset.target)+": "+(published.status==media::PublishStatus::exists?"destination contains different content":published.error);
            return false;
        }
        return true;
    };
    auto requests=edit.requests;for(auto& request:requests)request.actor=actor;
    auto result=edit.groupEnds_.empty()?OpJournal(store).commit(requests,options):
        OpJournal(store).commitGrouped(requests,edit.groupEnds_,options);
    if(result.ok) {
        rollback->committed=true;
        files->committed=true;
        try{if(!files->keepPrivate)files->source.discard();}catch(...){/* committed assets remain authoritative */}
    } else if(auto warning=rollback->rollback();!warning.empty()) {
        if(!result.error.empty())result.error+='\n';
        result.error+=warning;
    }
    return result;
}
}
