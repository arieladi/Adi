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
#include "adi/edit_plan.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <atomic>
#include <array>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <map>
#include <stdexcept>

namespace adi::clip_edit {
namespace fs=std::filesystem;
namespace {
void copyImport(const fs::path& source,const fs::path& target,std::stop_token stop) {
    const auto expected=fs::file_size(source);std::uintmax_t total=0;
    std::ifstream input(source,std::ios::binary);std::ofstream output(target,std::ios::binary);
    if(!input || !output)throw std::runtime_error("cannot open import copy");
    std::array<char,65536> bytes{};
    for(;;) {
        if(stop.stop_requested())throw std::runtime_error("import cancelled");
        input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));const auto got=input.gcount();
        if(static_cast<std::uintmax_t>(got)>expected-total)throw std::runtime_error("import source changed size during copy");
        total+=static_cast<std::uintmax_t>(got);output.write(bytes.data(),got);
        if(!output)throw std::runtime_error("import copy write failed");
        if(input.eof())break;
        if(!input)throw std::runtime_error("import copy read failed");
    }
    output.close();
    if(!output || total!=expected)throw std::runtime_error("import copy close/size check failed");
}
}
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
    // Keep both locations against the private backup's new base so a project
    // moved by hand bounces the same file that plays in the live session.
    const auto sourceFolder=media::projectFolder(store),captureFolder=media::projectFolder(output);
    SQLite::Statement rows(output,"SELECT id,rel_path FROM media_files WHERE rel_path IS NOT NULL");
    SQLite::Statement update(output,"UPDATE media_files SET rel_path=?,abs_path_hint=COALESCE(?,abs_path_hint) WHERE id=?");
    while(rows.executeStep()) {
        const auto original=media::pathFromUtf8(rows.getColumn(1).getString());
        const auto source=sourceFolder/original;
        const auto relative=source.lexically_normal().lexically_relative(captureFolder);
        if(relative.empty() || original.empty() || *original.begin()!="..") {
            update.bind(1);update.bind(2,media::pathUtf8(source));
        } else {
            update.bind(1,media::pathUtf8(relative));update.bind(2);
        }
        update.bind(3,rows.getColumn(0).getInt64());update.exec();update.reset();
    }
}
}
namespace {
using ImportedHashes = std::map<std::string, std::int64_t>;
struct ImportedPlacement {
    edit_plan::ClearInterval range;
    std::int64_t endTicks = 0;
};
MaterialEdit prepareAudio(Store& store, const ImportOptions& options, double referenceRate,
                          library::Index* index, ImportedHashes* hashes = nullptr,
                          ImportedPlacement* placement = nullptr) {
    // The callback refusal is a prvalue built in the caller's slot: guaranteed
    // elision, not optional NRVO, so it costs exactly one empty result.
    if(audio::onAudioThread)return {};
    MaterialEdit result;
    if(audio::onAudioThread)return result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(options.stop.stop_requested())throw std::runtime_error("import cancelled");
        if(!std::isfinite(referenceRate) || referenceRate<=0 || referenceRate>768000)
            throw std::runtime_error("invalid reference render rate");
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(store.db().getHandle()) != 0)
            read.emplace(store.db(), SQLite::TransactionBehavior::DEFERRED);
        result.edit.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement identity(store.db(),"PRAGMA database_list");
        while(identity.executeStep())if(identity.getColumn(1).getString()=="main")result.edit.sourceFile=identity.getColumn(2).getString();
        auto source=options.source.is_absolute()?options.source:media::projectFolder(store)/options.source;
        auto files=stage(media::projectFolder(store));files->stop=options.stop;result.files.push_back(files);
        if(options.copyIn) {
            files->staged=files->directory/source.filename();
            copyImport(source,files->staged,options.stop);source=files->staged;
        }
        OpRequest mediaRequest;
        const auto imported=media::prepareImport(store,source,options.mediaId,options.importedUtc,mediaRequest,nullptr,options.stop,!options.warp);
        if(!imported.ok)throw std::runtime_error(imported.error);
        if(!imported.audio)throw std::runtime_error("import prompt needs a supported audio file; video playback is not built");
        const auto& info=*imported.audio;
        const auto saved=index?loadSampleDefaults(store,*index,imported,mediaRequest):std::nullopt;
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
        auto mediaId = imported.mediaId;
        bool firstUse = true;
        if (hashes) {
            const auto [owner, inserted] = hashes->try_emplace(sourceHash.hex, mediaId);
            mediaId = owner->second;
            firstUse = inserted;
            if (!firstUse) mediaRequest = {};
        }
        if(options.copyIn && firstUse) {
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
        Payload sourceValues={{"clip_id",options.clipId},{"media_id",mediaId},{"src_len_frames",info.frames}};
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
        if (options.arrangementPlacement || placement) {
            const auto& placed = image.at("row");
            const int clock = placed.at("time_base").get<int>();
            const auto first = placed.at(clock == 0 ? "pos_ticks" : "pos_ns").get<std::int64_t>();
            const auto length = placed.at(clock == 0 ? "length_ticks" : "length_ns").get<std::int64_t>();
            const auto last = edit::addTime(first, length);
            if (placement) {
                placement->range = {{options.track, std::nullopt},
                                    {clock == 0 ? time.nanos(first) : first, clock == 0 ? time.nanos(last) : last}};
                placement->endTicks = clock == 0 ? last : time.ticks(last);
            }
            if (options.arrangementPlacement) {
                auto cleared = prepareAudioDropReplacement(store, options.track, {clock, first}, {clock, last},
                    {referenceRate, *options.arrangementPlacement}, options.clipId);
                if (!cleared.ok)
                    throw std::runtime_error(cleared.error);
                for (auto& request : cleared.requests) {
                    request.label = "Import Audio";
                    result.edit.requests.push_back(std::move(request));
                }
            }
        }
        edit::RowEdits plan(store.db());createImage(plan,image);
        OpRequest request;request.opType="clip.duplicate";request.payload={{"id",options.clipId},{"changes",plan.changes()}};
        request.label="Import Audio";request.targetKind="clip";request.targetId=options.clipId;result.edit.requests.push_back(std::move(request));
        if(options.stop.stop_requested())throw std::runtime_error("import cancelled");
        result.edit.ok=true;
    } catch(const std::exception& exception){result.edit.error=exception.what();}
    return result;
}
// One splice for all affected original rows. Separate per-file splices would
// each capture the same old clip when a consecutive drop cuts through it twice.
PreparedEdit prepareBatchReplacement(Store& store, std::span<const edit_plan::ClearInterval> ranges,
                                     const edit_plan::Options& options, std::int64_t lastIncomingId) {
    edit::TimeMap time(store.db());
    edit_plan::EditScope scope;
    scope.clock = edit_plan::PlacementClock::Nanoseconds;
    scope.bounds = {std::numeric_limits<std::int64_t>::max(), 0};
    edit_plan::detail::PlacementBatch material;
    std::set<edit_plan::PlacementLane> tracks;
    SQLite::Statement clips(store.db(),
        "SELECT time_base,COALESCE(pos_ticks,pos_ns),COALESCE(length_ticks,length_ns) "
        "FROM clips WHERE track_id=? AND lane_id IS NULL");
    for (const auto& range : ranges) {
        if (range.span.begin >= range.span.end)
            continue;
        clips.bind(1, range.lane.track);
        bool covered = false;
        while (clips.executeStep()) {
            const auto clock = clips.getColumn(0).getInt();
            const auto start = clips.getColumn(1).getInt64();
            const auto physical = [&](std::int64_t value) { return clock == 0 ? time.nanos(value) : value; };
            if (physical(start) >= range.span.end)
                continue;
            const auto length = clips.getColumn(2);
            if (length.isNull() || (length.getInt64() > 0 &&
                physical(edit::addTime(start, length.getInt64())) > range.span.begin)) {
                covered = true;
                break;
            }
        }
        clips.reset();
        if (!covered)
            continue;
        tracks.insert(range.lane);
        scope.bounds.begin = std::min(scope.bounds.begin, range.span.begin);
        scope.bounds.end = std::max(scope.bounds.end, range.span.end);
        material.clear.push_back(range);
        if (options.policy.automationFollowsClips)
            material.envelopes.push_back({range.lane.track, {1, range.span.begin}, {1, range.span.end}, Payload::array()});
    }
    if (tracks.empty()) {
        PreparedEdit ready;
        ready.ok = true;
        return ready;
    }
    scope.targets.assign(tracks.begin(), tracks.end());
    CopyIds ids(store.db());
    ids.reserve("clips", lastIncomingId);
    return edit_plan::detail::prepareCaptured(store, scope, std::move(material), options, ids, "Import Audio");
}

} // namespace
MaterialEdit prepareImportAudio(Store& store,const ImportOptions& options,double referenceRate) {
    return prepareImportAudio(store,options,referenceRate,nullptr);
}
MaterialEdit prepareImportAudio(Store& store,const ImportOptions& options,double referenceRate,library::Index* index) {
    return prepareAudio(store, options, referenceRate, index);
}
MaterialEdit prepareImportAudioBatch(Store& store, const ImportBatch& batch, double referenceRate,
                                     library::Index* sampleDefaults) {
    if (audio::onAudioThread) return {};
    MaterialEdit material;
    try {
        if (batch.imports.empty()) throw std::runtime_error("Drop an audio file.");
        SQLite::Transaction read(store.db(), SQLite::TransactionBehavior::DEFERRED);
        if (OpJournal(store).headSeq() != batch.expectedHead)
            throw std::runtime_error("The project changed before the drop could be prepared. Drop it again.");
        material.edit.options.expectHead.emplace(batch.expectedHead);
        material.edit.requests = batch.createTracks;
        ImportedHashes hashes;
        auto next = batch.imports.front().at;
        const bool replace = batch.placementPolicy && batch.placementPolicy->deleteOverlaps;
        std::vector<edit_plan::ClearInterval> covered;
        std::int64_t lastIncomingId = 0;
        for (std::size_t i = 0; i < batch.imports.size(); ++i) {
            auto options = batch.imports[i];
            if (batch.consecutive && i != 0) options.at = next;
            if (batch.placementPolicy)
                options.arrangementPlacement.reset();
            ImportedPlacement placement;
            auto file = prepareAudio(store, options, referenceRate, sampleDefaults, &hashes, &placement);
            if (!file.edit.ok) {
                material.edit.error = std::move(file.edit.error);
                material.refusedImport = i;
                return material;
            }
            next = {0, placement.endTicks};
            lastIncomingId = std::max(lastIncomingId, options.clipId);
            if (replace)
                covered.push_back(placement.range);
            if (i == 0) material.edit.sourceFile = std::move(file.edit.sourceFile);
            for (auto &request : file.edit.requests)
                material.edit.requests.push_back(std::move(request));
            material.files.push_back(std::move(file.files.front()));
        }
        if (replace) {
            auto cleared = prepareBatchReplacement(store, covered, {referenceRate, *batch.placementPolicy}, lastIncomingId);
            if (!cleared.ok)
                throw std::runtime_error(cleared.error);
            const auto afterTracks = material.edit.requests.begin() + static_cast<std::ptrdiff_t>(batch.createTracks.size());
            material.edit.requests.insert(afterTracks, std::make_move_iterator(cleared.requests.begin()),
                                          std::make_move_iterator(cleared.requests.end()));
        }
        material.edit.ok = true;
    } catch (const std::exception& error) {
        material.edit.error = error.what();
    }
    return material;
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
        result.files.front()->stop=control.stop;
        if(options.replace) {
            edit_plan::Options placement{job.session.sampleRate,{}};
            placement.policy.automationFollowsClips=false; // Replace material while leaving its mixer controls live.
            const auto remove=prepareDeleteRange(store,*options.replace,placement,options.destination.clipId);
            if(!remove.ok)throw std::runtime_error(remove.error);
            result.edit.requests.insert(result.edit.requests.begin(),remove.requests.begin(),remove.requests.end());
        }
        for(auto& request:result.edit.requests)request.label="Bounce";
    } catch(const std::exception& exception){result.edit.ok=false;result.edit.error=exception.what();}
    return result;
}
namespace {
bool publishMaterialFile(MaterialFiles& files, std::string& error) {
    if (files.stop.stop_requested()) { error = "operation cancelled"; return false; }
    const auto hash = media::blake3File(files.staged, files.stop);
    if (!hash || hash.hex != files.hash) { error = "staged media changed after capture"; return false; }
    if (files.target.empty()) return true;
    // publishFile moves its source. Retain the capture to allow a retry after
    // a publication failure or a deferred constraint rejecting SQL COMMIT.
    const auto publication = files.directory / "publication.tmp";
    std::error_code cleanup;
    fs::remove(publication, cleanup);
    fs::copy_file(files.staged, publication, fs::copy_options::none);
    const auto copied = media::blake3File(publication, files.stop);
    if (!copied || copied.hex != files.hash) { error = "material changed while staging publication"; return false; }
    const auto published = media::publishFile(publication, files.target, {}, media::PublishPolicy::AtomicOnly);
    if (published.status == media::PublishStatus::exists) {
        // Undo retains owned media for redo. Reuse an identical regular file;
        // this commit must never remove pre-existing bytes on failure.
        std::error_code statusError;
        const auto status = fs::symlink_status(files.target, statusError);
        if (!statusError && fs::is_regular_file(status)) {
            const auto existing = media::blake3File(files.target, files.stop);
            if (existing && existing.hex == files.hash) return true;
        }
    }
    if (published.status != media::PublishStatus::published) {
        error = "publish imported media " + media::pathUtf8(files.target) + ": " +
            (published.status == media::PublishStatus::exists ? "file exists" : published.error);
        return false;
    }
    files.published = true;
    return true;
}
} // namespace
CommitResult commitMaterial(Store& store,MaterialEdit& material,Actor actor) {
    auto prepared = material.edit;
    for (const auto& files : material.files) {
        CommitResult refused;
        if (files->committed) refused.error = "material edit was already committed";
        else if (files->stop.stop_requested()) refused.error = "operation cancelled";
        if (!refused.error.empty()) return refused;
    }
    if (!material.files.empty()) {
        const auto previous = prepared.options.beforeCommit;
        prepared.options.beforeCommit = [files = material.files, previous](SQLite::Database& db,
                                                std::int64_t transaction, std::string& error) {
            if (previous && !previous(db, transaction, error)) return false;
            return std::all_of(files.begin(), files.end(), [&](const auto& file) {
                return publishMaterialFile(*file, error);
            });
        };
    }
    auto result = commitPrepared(store, prepared, actor);
    for (const auto& files : material.files) {
        if (result.ok) files->committed = true;
        else if (files->published && !files->committed) {
            std::error_code error;
            fs::remove(files->target, error);
            if (!error) files->published = false;
        }
    }
    return result;
}
} // namespace adi::clip_edit
