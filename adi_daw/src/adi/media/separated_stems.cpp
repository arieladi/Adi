// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/media/separated_stems.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <utf8proc.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace adi::media {
namespace fs=std::filesystem;
struct PreparedSeparation::Data {
    struct Asset {fs::path privateFile,target;std::string hash;std::uint64_t bytes=0;};
    fs::path folder,project;
    std::string client,actorDetail,request;
    std::optional<std::int64_t> head;
    std::vector<Asset> assets;
    std::vector<OpRequest> ops;
    SeparationImportOptions options;
    bool committed=false;
    ~Data(){if(!folder.empty()){std::error_code ignored;fs::remove_all(folder,ignored);}}
};
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool text(const std::string& value,std::size_t maximum,bool empty=false) {
    if(value.size()>maximum || (!empty && value.empty()) || value.find('\0')!=value.npos)return false;
    std::size_t offset=0;
    while(offset<value.size()) {
        utf8proc_int32_t code=0;
        const auto size=utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(value.data()+offset),
            static_cast<utf8proc_ssize_t>(value.size()-offset),&code);
        if(size<=0)return false;
        offset+=static_cast<std::size_t>(size);
    }
    return true;
}
bool hash(const std::string& value) {
    if(value.size()!=64)return false;
    for(const char ch:value)if(!((ch>='0' && ch<='9') || (ch>='a' && ch<='f')))return false;
    return true;
}
void tick(const SeparationImportOptions& options) {
    require(!options.stop.stop_requested(),"stem result import cancelled");
    if(options.checkpoint)options.checkpoint();
    require(!options.stop.stop_requested(),"stem result import cancelled");
}
fs::path reserve(const fs::path& parent) {
    static std::atomic<std::uint64_t> serial{0};
    for(unsigned attempt=0;attempt<128;++attempt) {
        const auto path=parent/(".adi-separated-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
            "-"+std::to_string(serial.fetch_add(1,std::memory_order_relaxed)));
        std::error_code error;if(fs::create_directory(path,error))return path;
        if(error && error!=std::errc::file_exists)throw fs::filesystem_error("reserve stem result stage",path,error);
    }
    throw std::runtime_error("cannot reserve stem result stage");
}
void copy(const fs::path& source,const fs::path& target,std::uint64_t expected,const SeparationImportOptions& options) {
    std::ifstream input(source,std::ios::binary);std::ofstream output(target,std::ios::binary|std::ios::trunc);
    require(static_cast<bool>(input) && static_cast<bool>(output),"cannot open stem capture files");
    std::array<char,65536> bytes{};std::uint64_t total=0;
    for(;;) {
        tick(options);input.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        const auto size=input.gcount();
        require(static_cast<std::uint64_t>(size)<=expected-total,"stem changed size during capture");
        total+=static_cast<std::uint64_t>(size);if(size>0)output.write(bytes.data(),size);
        require(static_cast<bool>(output),"stem capture write failed");
        if(input.eof())break;
        require(static_cast<bool>(input),"stem capture read failed");
    }
    output.close();require(static_cast<bool>(output) && total==expected,"stem capture close/size check failed");
}
void validateWave(const fs::path& file,std::int64_t frames,std::int64_t rate,const SeparationImportOptions& options) {
    audio::WavReader reader(file);
    require(reader.frames()==static_cast<std::uint64_t>(frames) && reader.sampleRate()==rate &&
        (reader.channels()==1 || reader.channels()==2),"stem WAV dimensions differ from the source window");
    std::array<float,16384> samples{};
    std::uint64_t read=0;
    for(;;) {
        tick(options);const auto got=reader.read(samples.data(),8192);if(got==0)break;
        for(std::size_t i=0;i<static_cast<std::size_t>(got)*reader.channels();++i)
            require(std::isfinite(samples[i]),"nonfinite stem audio");
        read+=got;
    }
    require(read==reader.frames(),"truncated stem audio");
}
void freeId(SQLite::Database& db,const char* table,std::int64_t id) {
    SQLite::Statement query(db,std::string("SELECT 1 FROM ")+table+" WHERE id=?");query.bind(1,id);
    require(!query.executeStep(),"allocated stem identity is already occupied");
}
void request(std::vector<OpRequest>& ops,const char* type,Payload payload) {
    OpRequest op;op.opType=type;op.payload=std::move(payload);op.label="Separate stems";ops.push_back(std::move(op));
}
} // namespace
bool PreparedSeparation::ready() const noexcept{return data_ && !data_->committed && error_.empty();}
PreparedSeparation prepareSeparatedStems(Store& store,const SeparationResult& result,const SeparationImportOptions& options) {
    // The callback refusal is a prvalue built in the caller's slot: guaranteed
    // elision, not optional NRVO, so it costs exactly one empty result.
    if(audio::onAudioThread)return {};
    PreparedSeparation prepared;
    try {
        require(!store.readOnly(),"stem result project is read-only");
        require(result.sourceMedia>0 && result.sourceBegin>=0 && result.frames>0 && hash(result.sourceHash),"invalid separation source identity/window");
        require(result.stems.size()>0 && result.stems.size()<=1024 && options.stems.size()==result.stems.size(),"stem result and allocated counts must match in 1..1024");
        require(options.group>0 && options.index>=0 && options.positionTicks>=0 && options.createdUtc>=0 &&
            text(options.name,4096,true) && options.maximumBytes>0 && options.maximumBytes<=(std::uint64_t{1}<<40),"invalid stem group placement/budget");
        require(text(result.service,4096) && text(result.model,1024) && text(result.version,1024) && text(result.request,65536,true) &&
            (!result.weightsHash || hash(*result.weightsHash)) && (result.service!="builtin" || result.weightsHash),"invalid separation model/request provenance");
        std::set<std::int64_t> tracks{options.group},clips,medias;std::set<std::string> names;
        for(std::size_t i=0;i<result.stems.size();++i) {
            const auto& stem=result.stems[i];const auto& ids=options.stems[i];
            require(text(stem.file,4096) && text(stem.name,4096) && hash(stem.hash),"invalid returned stem identity/text");
            const auto path=pathFromUtf8(stem.file);
            require(!path.has_root_path() && path==path.filename() && stem.file!="." && stem.file!=".." && names.insert(stem.file).second,"stem file must be a distinct result basename");
            require(ids.track>0 && ids.clip>0 && ids.media>0 && tracks.insert(ids.track).second &&
                clips.insert(ids.clip).second && medias.insert(ids.media).second &&
                (!ids.paletteIndex || (*ids.paletteIndex>=0 && *ids.paletteIndex<=69)),"invalid allocated stem identities/colour");
        }
        auto data=std::make_shared<PreparedSeparation::Data>();data->options=options;data->client=store.clientId();
        data->project=store.path();data->head=OpJournal(store).headSeq();data->request=result.request;
        // This is the existing AI audit contract, not a new project opcode.
        data->actorDetail=Payload{{"kind","stem-separation"},{"version",1},{"service",result.service},{"model",result.model},
            {"model_version",result.version},{"weights_hash",result.weightsHash?Payload(*result.weightsHash):Payload(nullptr)},
            {"source_hash",result.sourceHash},{"source_begin",result.sourceBegin},{"source_frames",result.frames}}.dump();
        require(result.folder.is_absolute() && fs::is_directory(fs::symlink_status(result.folder)),"result folder must be an absolute real directory");
        const auto root=fs::canonical(result.folder);
        // A coherent source/identity read precedes disk work, then the head is
        // rechecked in the later native-payload capture transaction.
        std::int64_t rate=0;
        {
            SQLite::Transaction read(store.db(),SQLite::TransactionBehavior::DEFERRED);
            SQLite::Statement source(store.db(),"SELECT hash_blake3,sample_rate,channels,frames FROM media_files WHERE id=?");source.bind(1,result.sourceMedia);
            require(source.executeStep() && source.getColumn(0).getString()==result.sourceHash && source.getColumn(1).isInteger() &&
                source.getColumn(2).isInteger() && source.getColumn(3).isInteger(),"source media/provenance is missing or changed");
            rate=source.getColumn(1).getInt64();const auto channels=source.getColumn(2).getInt64(),frames=source.getColumn(3).getInt64();
            require(rate>0 && rate<=768000 && (channels==1 || channels==2) && result.sourceBegin<=frames &&
                result.frames<=frames-result.sourceBegin,"source window is outside registered audio");
            freeId(store.db(),"tracks",options.group);
            for(const auto& ids:options.stems){freeId(store.db(),"tracks",ids.track);freeId(store.db(),"clips",ids.clip);freeId(store.db(),"media_files",ids.media);}
            if(options.parent) {
                SQLite::Statement parent(store.db(),"SELECT kind,locked FROM tracks WHERE id=?");parent.bind(1,*options.parent);
                require(parent.executeStep() && parent.getColumn(0).getString()=="group" && parent.getColumn(1).getInt()==0,"stem parent must be an unlocked group");
            }
            read.commit();
        }
        std::uint64_t total=0;std::vector<std::uint64_t> sizes;
        for(const auto& stem:result.stems) {
            const auto path=root/pathFromUtf8(stem.file);
            require(fs::is_regular_file(fs::symlink_status(path)),"stem result must be a real regular file");
            const auto bytes=fs::file_size(path);require(bytes<=options.maximumBytes-total,"stem result exceeds byte budget");total+=bytes;sizes.push_back(bytes);
        }
        tick(options);data->folder=reserve(projectFolder(store));
        for(std::size_t i=0;i<result.stems.size();++i) {
            const auto& stem=result.stems[i];const auto source=root/pathFromUtf8(stem.file);
            const auto path=data->folder/("stem-"+std::to_string(i)+".wav");copy(source,path,sizes[i],options);
            const auto after=blake3File(path,options.stop);require(after && after.hex==stem.hash,"stem changed while capturing");
            validateWave(path,result.frames,rate,options);
            data->assets.push_back({path,projectFolder(store)/"audio"/("separated-"+stem.hash+".wav"),stem.hash,sizes[i]});
        }
        tick(options);
        SQLite::Transaction capture(store.db(),SQLite::TransactionBehavior::DEFERRED);
        require(store.clientId()==data->client && OpJournal(store).headSeq()==data->head,"project changed while capturing stem results");
        const auto snapshot=engine::SnapshotBuilder::fromStore(store);const auto& tempo=*snapshot->tempo;
        const auto start=tempo.ticksToSeconds(options.positionTicks);const auto endSeconds=start+static_cast<double>(result.frames)/static_cast<double>(rate);
        require(std::isfinite(start) && start>=0 && std::isfinite(endSeconds) && endSeconds>start && endSeconds<0x1p40 &&
            tempo.secondsToTicks(start)==options.positionTicks,"stem placement clock cannot represent this source span");
        auto end=tempo.secondsToTicks(endSeconds);
        if(tempo.ticksToSeconds(end)<endSeconds) {
            require(end<std::numeric_limits<std::int64_t>::max(),"stem end tick overflow");++end;
        }
        require(end>options.positionTicks && tempo.ticksToSeconds(end)>=endSeconds,"stem clip window would truncate source audio");
        std::map<std::string,std::int64_t> byHash;std::vector<std::int64_t> mediaIds;
        for(std::size_t i=0;i<data->assets.size();++i) {
            const auto& asset=data->assets[i];
            if(const auto found=byHash.find(asset.hash);found!=byHash.end()){mediaIds.push_back(found->second);continue;}
            OpRequest op;const auto imported=prepareImport(store,asset.privateFile,options.stems[i].media,options.createdUtc,op);
            require(imported.ok && imported.audio && imported.audio->frames==result.frames && imported.audio->sampleRate==rate &&
                (imported.audio->channels==1 || imported.audio->channels==2),"cannot capture native stem media");
            if(imported.existing) {
                const auto row=mediaRow(store.db(),imported.mediaId);
                require(row.at("sample_rate")==rate && row.at("frames")==result.frames && row.at("channels")==imported.audio->channels,"existing stem media has inconsistent audio metadata");
            }
            Payload paths={{"rel_path",pathUtf8(fs::relative(asset.target,projectFolder(store)))},{"abs_path_hint",nullptr},{"missing",0}};
            if(op.opType=="media.import") {
                op.payload["row"].update(paths);op.payload["row"]["orig_name"]=result.stems[i].file;
            } else {op.opType="media.relink";op.payload={{"id",imported.mediaId},{"hash",asset.hash},{"paths",paths}};}
            data->ops.push_back(std::move(op));byHash.emplace(asset.hash,imported.mediaId);mediaIds.push_back(imported.mediaId);
        }
        request(data->ops,"track.create",{{"id",options.group},{"kind","group"},{"name",options.name.empty()?"Separated stems":options.name},
            {"parent",options.parent?Payload(*options.parent):Payload(nullptr)},{"index",options.index}});
        for(std::size_t i=0;i<result.stems.size();++i) {
            const auto& ids=options.stems[i];const auto& stem=result.stems[i];
            request(data->ops,"track.create",{{"id",ids.track},{"kind","audio"},{"name",stem.name},{"parent",options.group},{"index",i}});
            if(ids.paletteIndex)request(data->ops,"track.setColor",{{"id",ids.track},{"color",*ids.paletteIndex}});
            request(data->ops,"clip.create",{{"id",ids.clip},{"track",ids.track},{"kind","audio"},{"name",stem.name},{"pos",options.positionTicks},{"length",end-options.positionTicks}});
            request(data->ops,"clip.attachAudio",{{"id",ids.clip},{"media",mediaIds[i]},{"frames",result.frames}});
        }
        for(auto& op:data->ops){op.actor=Actor::Import;op.actorDetail=data->actorDetail;op.label="Separate stems";op.targetKind="track";op.targetId=options.group;}
        if(options.selectionBefore)data->ops.front().selBefore=options.selectionBefore;
        capture.commit();prepared.data_=std::move(data);
    } catch(const std::exception& error){prepared.error_=error.what();}
    return prepared;
}
CommitResult commitSeparatedStems(Store& store,PreparedSeparation& prepared,std::uint64_t generation,Actor actor,const CommitOptions& extra) {
    // The callback refusal is a prvalue built in the caller's slot. A named result
    // is moved out, not elided (other returns name `result`), which on MSVC Debug
    // allocates a second set of container proxies on the audio thread.
    if(audio::onAudioThread)return {};
    CommitResult refused;
    if(!prepared.ready()){refused.error="stem result is absent or already committed";return refused;}
    const auto data=prepared.data_;
    try {
        require(store.clientId()==data->client && store.path()==data->project && generation==data->options.documentGeneration,"stem result belongs to another document/client/generation");
        require(!store.readOnly(),"stem result project is read-only");
        require(!extra.expectHead || *extra.expectHead==data->head,"caller head policy differs from captured stem result");
        tick(data->options);CommitOptions options;options.expectHead.emplace(data->head);
        options.beforeCommit=[data,extra](SQLite::Database& db,std::int64_t txn,std::string& error) {
            try {
                const auto folder=data->folder.parent_path()/"audio";
                require(!fs::is_symlink(fs::symlink_status(folder)),"stem audio folder must not be a symlink");fs::create_directories(folder);
                for(std::size_t i=0;i<data->assets.size();++i) {
                    tick(data->options);const auto& asset=data->assets[i];
                    const auto verified=blake3File(asset.privateFile,data->options.stop);
                    require(verified && verified.hex==asset.hash,"captured private stem changed before publication");
                    const auto publication=data->folder/("publish-"+std::to_string(i)+".wav");std::error_code ignored;fs::remove(publication,ignored);
                    copy(asset.privateFile,publication,asset.bytes,data->options);
                    const auto copied=blake3File(publication,data->options.stop);
                    require(copied && copied.hex==asset.hash,"captured stem changed during publication");
                    const auto published=publishFile(publication,asset.target);
                    if(published.status==PublishStatus::exists && fs::is_regular_file(fs::symlink_status(asset.target))) {
                        const auto existing=blake3File(asset.target,data->options.stop);
                        require(existing && existing.hex==asset.hash,"owned stem target contains different audio");
                    } else require(published.status==PublishStatus::published,"cannot publish owned stem audio");
                }
                tick(data->options);
                SQLite::Statement audit(db,"INSERT INTO agent_requests(txn_id,request,actor_detail,created_utc) VALUES(?,?,?,?)");
                audit.bind(1,txn);audit.bind(2,data->request);audit.bind(3,data->actorDetail);audit.bind(4,data->options.createdUtc);audit.exec();
                return !extra.beforeCommit || extra.beforeCommit(db,txn,error);
            } catch(const std::exception& failure){error=failure.what();return false;}
        };
        auto ops=data->ops;for(auto& op:ops)op.actor=actor;
        auto result=OpJournal(store).commit(ops,options);if(result.ok)data->committed=true;return result;
    } catch(const std::exception& error){refused.error=error.what();return refused;}
}
} // namespace adi::media
