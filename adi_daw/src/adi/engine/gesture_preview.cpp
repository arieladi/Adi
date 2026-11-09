// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/gesture_preview.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/engine/session.hpp"
#include "adi/gesture_capture_detail.hpp"
#include "adi/history.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace adi::engine {
namespace {
using gesture::Base;
using gesture::OwnerToken;
using gesture::PresentationState;
using gesture::PresentationStatus;
using gesture::Status;
using gesture::Error;
using Tables=std::map<std::string,std::vector<Payload>>;
using TempoFor=std::function<std::vector<rows::TempoEvent>(const rows::Model&)>;
const std::set<std::string> materialTables{
    "project","tracks","track_recording_options","mixer_strip","mixer_pan_settings","group_summing",
    "tempo_map","time_signature_map","tuning_systems","tuning_degrees","key_map","key_map_tunings","key_map_degrees",
    "lanes","clips","clip_properties","clip_playback_details","clip_warp_fade_domains","audio_clips","warp_markers",
    "event_streams","note_expression","automation_lanes","automation_data","automation_segment_ranges",
    "media_files","routing","plugin_refs","device_chains","devices","plugin_params","plugin_state",
    "device_panels","device_panel_params","device_expression_routes","markers","marker_details","remarks",
    "frozen_renders","frozen_groups","return_send_taps","session_state","state_blobs"};
Tables readTables(const Store& store) {
    Tables result;
    SQLite::Statement tables(store.db(),"SELECT name FROM sqlite_master WHERE type IN ('table','view') UNION SELECT name FROM sqlite_temp_master WHERE type IN ('table','view') ORDER BY name");
    while(tables.executeStep()) {
        const auto name=tables.getColumn(0).getString();
        if(!materialTables.contains(name))continue;
        auto& rows=result[name];
        auto& schema=result["@schema/"+name];
        SQLite::Statement columns(store.db(),"PRAGMA table_xinfo(\""+name+"\")");
        while(columns.executeStep())schema.push_back(adi::detail::captureQueryRow(columns));
        std::string queryText="SELECT * FROM \""+name+"\"";
        if(name=="session_state")queryText+=" WHERE key IN ('transport.metronome','transport.loop','cue.controls')";
        if(name=="state_blobs")queryText+=" WHERE hash_blake3 IN (SELECT s.state_hash FROM plugin_state s JOIN devices d ON d.id=s.device_id JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE p.format='internal' AND s.stream_role LIKE 'sample:%')";
        SQLite::Statement query(store.db(),queryText);
        while(query.executeStep())rows.push_back(adi::detail::captureQueryRow(query));
        std::sort(rows.begin(),rows.end(),[](const Payload& a,const Payload& b){return Payload::to_cbor(a)<Payload::to_cbor(b);});
    }
    return result;
}
std::vector<std::uint8_t> encodeTables(const Tables& rows) {return Payload::to_cbor(Payload(rows));}
std::vector<std::uint8_t> imageBytes(const Tables& rows,const TransportSettings& transport,const CueSettings& cue) {
    Payload value=rows;
    value["@transport"]={transport.metronome,transport.loopEnabled,transport.hasLoop,transport.loopStartTicks,transport.loopEndTicks};
    value["@cue"]={cue.cueMode,cue.soloInPlace,cue.referenceOn,cue.referenceFromRole,cue.firstOutputChannel,
        cue.volumeDb,cue.referenceTrack,cue.projectReferenceTrack,cue.selectedTracks};
    return Payload::to_cbor(value);
}
bool sameBase(const Base& a,const Base& b) {
    return a.source.sameConnection(b.source) && a.head==b.head && a.sequence==b.sequence;
}
PresentationStatus refused(std::string message) {return {PresentationState::Refused,std::move(message),{}};}
Status failed(Error code,std::string message) {return {code,std::move(message)};}
struct Image {
    Base base;
    rows::Model model;
    std::shared_ptr<const ClipProject> clips;
    std::shared_ptr<const Snapshot> snapshot;
    TransportSettings transport;
    CueSettings cue;
    Tables tables;
    std::set<std::int64_t> overrides;
    std::vector<std::uint8_t> bytes;
    std::shared_ptr<const std::vector<std::uint8_t>> viewBytes;
};
std::shared_ptr<Image> readImage(const Store& store,const Base& base,const std::set<std::int64_t>& overridden,
                               const TempoFor& tempoFor) {
    std::optional<SQLite::Transaction> read;
    if(sqlite3_get_autocommit(store.db().getHandle())!=0)
        read.emplace(const_cast<SQLite::Database&>(store.db()),SQLite::TransactionBehavior::DEFERRED);
    if(History(const_cast<Store&>(store)).head()!=base.head)throw std::runtime_error("presentation journal base changed during capture");
    auto out=std::make_shared<Image>();out->base=base;out->overrides=overridden;
    out->model=rows::readModel(store);
    out->tables=readTables(store);
    out->transport=readTransportSettings(store);out->cue=readCueSettings(store);
    out->bytes=imageBytes(out->tables,out->transport,out->cue);
    out->viewBytes=std::make_shared<const std::vector<std::uint8_t>>(out->bytes);
    out->snapshot=SnapshotBuilder::fromStore(store,overridden,false);
    auto playback=out->model;
    playback.tempo=tempoFor(playback);
    out->clips=readClipProject(store,playback);
    return out;
}
Tables topology(Tables value) {
    const std::map<std::string,std::set<std::string>> retained{
        {"project",{"sample_rate","ppq"}},
        {"tracks",{"id","parent_id","index_in_parent","kind","time_base","frozen","solo_defeat","freeze_media_id","role","input_ref","monitor_mode"}},
        {"mixer_strip",{"track_id","pan_law","width","phase_invert","delay_samples","vca_group_id"}},
        {"mixer_pan_settings",{"track_id","mode"}},
        {"group_summing",{"track_id","enabled","flavor"}},
        {"routing",{"id","src_kind","src_id","dst_kind","dst_id","kind","ord","pre_fader","enabled","origin"}}};
    const std::set<std::string> whole{"plugin_refs","device_chains","devices","plugin_params","plugin_state",
        "device_expression_routes","track_recording_options","frozen_renders","frozen_groups","return_send_taps"};
    for(auto it=value.begin();it!=value.end();) {
        const auto fields=retained.find(it->first);
        if(fields==retained.end()) {if(whole.contains(it->first)){++it;continue;}it=value.erase(it);continue;}
        for(auto& row:it->second)for(auto cell=row.begin();cell!=row.end();) {
            if(fields->second.contains(cell.key()))++cell;else cell=row.erase(cell);
        }
        // Native absent optional mixer rows mean these same default static
        // values. First scalar insertion must not invent a topology change.
        if(it->first=="mixer_strip" || it->first=="mixer_pan_settings") {
            const Payload defaults=it->first=="mixer_strip"
                ? Payload{{"pan_law",0},{"width",1.0},{"phase_invert",0},{"delay_samples",0},{"vca_group_id",nullptr}}
                : Payload{{"mode","stereo"}};
            for(auto& row:it->second)for(const auto& field:defaults.items())
                if(row.contains(field.key()) && row.at(field.key())==field.value())row.erase(field.key());
            std::erase_if(it->second,[](const Payload& row){return row.size()==1 && row.contains("track_id");});
        }
        std::sort(it->second.begin(),it->second.end(),[](const Payload& a,const Payload& b){return Payload::to_cbor(a)<Payload::to_cbor(b);});
        ++it;
    }
    return value;
}
const std::set<std::string> scalarOps{"mixer.setVolume","mixer.setPan","mixer.setPanLeft","mixer.setPanRight",
    "mixer.setInputGain","routing.setGain","routing.setPan","group.setSummingDrive","track.setMute","track.setSolo"};
struct Patch {std::string operation;std::int64_t id;};
std::vector<Patch> patches(std::span<const OpRequest> requests,gesture::Strategy strategy) {
    std::vector<Patch> result;
    for(const auto& request:requests) {
        const auto* descriptor=OpRegistry::instance().find(request.opType);
        if(!descriptor || (descriptor->engineImpact!=EngineImpact::None && descriptor->engineImpact!=EngineImpact::Snapshot))
            throw std::runtime_error("preview requires a supported None or Snapshot operation");
        if(strategy==gesture::Strategy::LiveValues) {
            if(!scalarOps.contains(request.opType))throw std::runtime_error("unsupported native live-value preview: "+request.opType);
            result.push_back({request.opType,adi::detail::signedInteger(request.payload.at("id"))});
        } else if(request.opType.starts_with("device.") || request.opType=="mixer.setWidth" || request.opType=="mixer.setPhaseInvert")
            throw std::runtime_error("unsupported native structural preview: "+request.opType);
    }
    return result;
}
template<class V,class F> auto* findValue(V& values,std::int64_t id,F key) {
    const auto at=std::find_if(values.begin(),values.end(),[&](const auto& value){return key(value)==id;});
    return at==values.end()?nullptr:&*at;
}
void applyPatch(rows::Model& target,const rows::Model& projected,const Patch& patch) {
    const auto& name=patch.operation;
    if(name.starts_with("mixer.")) {
        auto key=[](const auto& row){return row.trackId;};
        auto* to=findValue(target.strips,patch.id,key);const auto* from=findValue(projected.strips,patch.id,key);
        if(!to || !from)throw std::runtime_error("preview mixer target disappeared");
        if(name=="mixer.setVolume")to->volumeDb=from->volumeDb;
        else if(name=="mixer.setPan")to->pan=from->pan;
        else if(name=="mixer.setPanLeft")to->panLeft=from->panLeft;
        else if(name=="mixer.setPanRight")to->panRight=from->panRight;
        else to->inputGainDb=from->inputGainDb;
    } else if(name.starts_with("track.")) {
        auto key=[](const auto& row){return row.id;};
        auto* to=findValue(target.tracks,patch.id,key);const auto* from=findValue(projected.tracks,patch.id,key);
        if(!to || !from)throw std::runtime_error("preview track target disappeared");
        if(name=="track.setMute")to->muted=from->muted;else to->soloed=from->soloed;
    } else if(name.starts_with("routing.")) {
        auto key=[](const auto& row){return row.id;};
        auto* to=findValue(target.routing,patch.id,key);const auto* from=findValue(projected.routing,patch.id,key);
        if(!to || !from || to->kind!="send")throw std::runtime_error("preview requires an existing send");
        if(name=="routing.setGain")to->gainDb=from->gainDb;else to->pan=from->pan;
    } else {
        auto key=[](const auto& row){return row.trackId;};
        auto* to=findValue(target.summing,patch.id,key);const auto* from=findValue(projected.summing,patch.id,key);
        if(!to || !from || !to->enabled)throw std::runtime_error("preview requires existing native summing");
        to->driveDb=from->driveDb;
    }
}
std::shared_ptr<const Snapshot> scalarSnapshot(const Snapshot& base,const rows::Model& model) {
    auto out=std::make_shared<Snapshot>(base);
    for(auto& track:out->tracks) {
        auto value=std::make_shared<TrackNode>(*track);
        for(const auto& row:model.tracks)if(row.id==value->id){value->muted=row.muted;value->soloed=row.soloed;}
        for(const auto& row:model.strips)if(row.trackId==value->id) {
            value->volumeDb=static_cast<float>(row.volumeDb);value->pan=static_cast<float>(row.pan);
            value->panLeft=row.panLeft;value->panRight=row.panRight;
        }
        track=std::move(value);
    }
    for(auto& send:out->sends)for(const auto& row:model.routing)if(row.id==send->id) {
        auto value=std::make_shared<SendRoute>(*send);value->gainDb=row.gainDb;value->pan=row.pan;send=std::move(value);break;
    }
    return out;
}
std::shared_ptr<const Snapshot> annotateSnapshot(const Snapshot& snapshot,const std::set<std::int64_t>& excluded) {
    auto out=std::make_shared<Snapshot>(snapshot);out->automationOverridden=!excluded.empty();
    for(auto& lane:out->automationLanes)if(lane->overridden!=excluded.contains(lane->id)) {
        auto next=std::make_shared<AutomationLaneNode>(*lane);next->overridden=excluded.contains(lane->id);lane=std::move(next);
    }
    return out;
}
std::set<std::int64_t> manualOverrides(const rows::Model& before,const rows::Model& after,
    std::set<std::int64_t> excluded,const std::set<std::int64_t>& writing,double rate) {
    const auto binding=bindStripAutomation(compileAutomation(before,rate),{});
    auto value=[](const rows::Model& model,std::int64_t id,StripParam parameter)->std::optional<double> {
        const auto* track=findValue(model.tracks,id,[](const auto& row){return row.id;});
        if(!track)return std::nullopt;
        if(parameter==StripParam::Mute)return track->muted?1.0:0.0;
        const auto* strip=findValue(model.strips,id,[](const auto& row){return row.trackId;});
        if(!strip)return parameter==StripParam::PanLeft?-1.0:parameter==StripParam::PanRight?1.0:0.0;
        if(parameter==StripParam::Volume)return strip->volumeDb;
        if(parameter==StripParam::Pan)return strip->pan;
        if(parameter==StripParam::PanLeft)return strip->panLeft;
        return strip->panRight;
    };
    for(const auto& [key,id]:binding->laneFor) {
        if(excluded.contains(id) || writing.contains(id))continue;
        const auto old=value(before,key.first,key.second),next=value(after,key.first,key.second);
        if(old && next && *old!=*next)excluded.insert(id);
    }
    for(const auto& [route,id]:binding->sendLaneFor) {
        if(excluded.contains(id) || writing.contains(id))continue;
        const auto* old=findValue(before.routing,route,[](const auto& row){return row.id;});
        const auto* next=findValue(after.routing,route,[](const auto& row){return row.id;});
        if(old && next && old->gainDb!=next->gainDb)excluded.insert(id);
    }
    return excluded;
}
struct Controls final:GestureControls {
    std::shared_ptr<const MixerControlBatch> mixer;
    std::shared_ptr<const CueControlBatch> cue;
    std::vector<SummingControl> summing;
    std::vector<std::pair<InputGainNode*,double>> input;
    std::shared_ptr<const StripLaneBindings> lanes; // the lanes these controls leave running
    const StripLaneBindings* laneBindings() const noexcept override {return lanes.get();}
    void apply() const noexcept override {
        if(mixer)mixer->apply();
        if(cue)cue->apply();
        for(const auto& value:summing)value.node->setGains(value.pre,value.post);
        for(const auto& [node,db]:input)node->setDb(db);
    }
};
struct PublishedControls final:Sequenced {
    std::shared_ptr<const GestureControls> value;
    std::uint64_t generation=0;
    struct Version {std::uint64_t generation;std::shared_ptr<const GestureControls> value;};
    std::vector<Version> previous;
    const std::shared_ptr<const GestureControls>* forGeneration(std::uint64_t tag)const noexcept {
        if(generation==tag)return &value;
        for(const auto& item:previous)if(item.generation==tag)return &item.value;
        return nullptr;
    }
};
struct Audio final:GestureAudio {
    SnapshotPublisher<PublishedControls> publisher;
    std::uint64_t next=0,current=0; // owner only; callback receives its immutable graph tag
    std::atomic<std::uint64_t> rendered{0};
    const StripLaneBindings* apply(std::uint64_t generation) noexcept override {
        const auto* value=publisher.peek();
        // A graph still rendering the previous generation must not announce a
        // newer overlay that it cannot use: its previous raw lanes stay pinned
        // until the matching generation installs replacement bindings.
        if(!value)return nullptr;
        const auto* controls=value->forGeneration(generation);
        if(!controls)return nullptr;
        publisher.announce(value);
        if(*controls)(*controls)->apply();
        rendered.store(generation,std::memory_order_release);
        // The publisher's announced value keeps these bindings for the block.
        return *controls?(*controls)->laneBindings():nullptr;
    }
    std::uint64_t reserveGeneration() override {
        if(next==std::numeric_limits<std::uint64_t>::max())throw std::runtime_error("preview generation counter exhausted");
        return ++next;
    }
    std::uint64_t currentGeneration()const noexcept override{return current;}
    void publishedGeneration(std::uint64_t value)noexcept override{current=value;}
    void collect() override {publisher.collect();}
    void reset() override {stageReset(current);}
    void stageReset(std::uint64_t tag) override {auto value=std::make_unique<PublishedControls>();value->generation=tag;publish(value);}
    void publish(std::unique_ptr<PublishedControls>& value) {
        // A new graph can be ready while the previous one still renders. Keep
        // the control image for both the observed audio generation and the
        // last published graph. At most two old tags are needed; no retired
        // generation or control object is released by the callback.
        const auto* old=publisher.peek();
        if(value && old) {
            value->previous.reserve(2);
            for(const auto tag:{current,rendered.load(std::memory_order_acquire)}) {
                if(tag==value->generation || std::any_of(value->previous.begin(),value->previous.end(),[tag](const auto& item){return item.generation==tag;}))continue;
                if(const auto* controls=old->forGeneration(tag))value->previous.push_back({tag,*controls});
            }
        }
        publisher.publishPrepared(value);
    }
};
struct Captured final:gesture::CapturedPresentation {
    OwnerToken owner;
    std::shared_ptr<const Image> image;
    std::shared_ptr<const Image> committed;
    gesture::Strategy strategy;
    std::vector<Patch> changes;
    std::uint64_t epoch;
    Captured(OwnerToken token,std::shared_ptr<const Image> projected,std::shared_ptr<const Image> base,
             gesture::Strategy kind,std::vector<Patch> patch,std::uint64_t format)
        :owner(std::move(token)),image(std::move(projected)),committed(std::move(base)),strategy(kind),changes(std::move(patch)),epoch(format){}
};
struct Generation {
    std::shared_ptr<ClipPlayback> clips;
    std::shared_ptr<MidiClips> midi;
    std::shared_ptr<StripAutomation> strips;
    std::shared_ptr<DeviceAutomationBinding> devices;
    std::shared_ptr<const Controls> controls;
    std::unique_ptr<GraphHost::PreparedGraph> ready;
    std::shared_ptr<const GraphTransportState> timing;
    std::shared_ptr<const Image> image;
    device::DeviceHost::RebuildSpec rebuild;
    std::uint64_t tag=0;
};
struct Restore final:gesture::RestorationReceipt {
    PresentationState result=PresentationState::Current;
    std::string diagnostic;
    bool requested=false;
    PresentationState state()const noexcept override{return result;}
    const std::string& message()const noexcept override{return diagnostic;}
};
struct Active {
    OwnerToken owner;
    std::shared_ptr<const Image> image;
    gesture::Strategy strategy;
    std::vector<Patch> changes;
    std::shared_ptr<Restore> restore=std::make_shared<Restore>();
    std::unique_ptr<PublishedControls> restoreControls;
    std::shared_ptr<const Snapshot> restoreSnapshot;
    std::shared_ptr<Generation> restoreGeneration;
    std::shared_ptr<Generation> previewGeneration;
    bool current=true;
    Active(const Captured& value):owner(value.owner),image(value.image),strategy(value.strategy),changes(value.changes){}
    Active(const Active& value):owner(value.owner),image(value.image),strategy(value.strategy),changes(value.changes),
        restore(value.restore),restoreSnapshot(value.restoreSnapshot),restoreGeneration(value.restoreGeneration),
        previewGeneration(value.previewGeneration),current(value.current){}
};
struct Prepared final:gesture::PreparedPresentation {
    std::unique_ptr<const Captured> capture;
    std::vector<std::shared_ptr<Active>> next;
    std::unique_ptr<PublishedControls> controls;
    std::shared_ptr<const Snapshot> snapshot;
    std::shared_ptr<Generation> generation;
};
}
std::shared_ptr<GestureAudio> makeGestureAudio(){return std::make_shared<Audio>();}
std::shared_ptr<const GestureControls> nativeGestureControls(Session& owner) {
    auto result=std::make_shared<Controls>();
    HearingFlags hearing;hearing.soloInPlace=owner.cueSettings_.soloInPlace;
    hearing.soloIsCue=owner.spec_.performanceState && owner.cueSettings_.cueMode;
    hearing.referenceTrack=owner.cueSettings_.projectReferenceTrack;
    result->mixer=owner.strips_.prepareControls(owner.model_,&owner.transport_,owner.stripAutomation_,owner.spec_.delayCompensation,hearing);
    result->summing=owner.summing_.prepareControls(owner.model_);
    result->cue=owner.cueMonitor_.prepareControls(owner.model_,owner.cueSettings_,owner.stripAutomation_,owner.spec_.performanceState);
    for(const auto& [id,node]:owner.inputGains_) {
        const auto* strip=findValue(owner.model_.strips,id,[](const auto& row){return row.trackId;});
        result->input.emplace_back(node.get(),strip?strip->inputGainDb:0.0);
    }
    return result;
}
std::vector<std::uint8_t> capturePresentationImage(const Store& store){
    std::optional<SQLite::Transaction> read;
    if(sqlite3_get_autocommit(store.db().getHandle())!=0)
        read.emplace(const_cast<SQLite::Database&>(store.db()),SQLite::TransactionBehavior::DEFERRED);
    return imageBytes(readTables(store),readTransportSettings(store),readCueSettings(store));
}

struct GestureRuntime final:SessionRuntimeClient, std::enable_shared_from_this<GestureRuntime> {
    std::shared_ptr<const SessionEndpoint> session;
    std::shared_ptr<const ProjectViewEndpoint> view;
    std::shared_ptr<const Image> committed;
    std::vector<std::shared_ptr<Active>> active;
    std::shared_ptr<Generation> renderedGeneration;
    bool refreshing=false;
    explicit GestureRuntime(Session& owner,ProjectView& project):session(owner.endpoint()),view(project.endpoint()){}
    Session* engine()const noexcept{return session?session->get():nullptr;}
    ProjectView* project()const noexcept{return view?view->get():nullptr;}
    static TempoFor tempoRule(const Session* owner){return [owner](const rows::Model& model){return owner->playbackTempo(model);};}
    std::shared_ptr<Controls> controls(const rows::Model& model,const std::set<std::int64_t>& suspended) {
        auto* owner=engine();if(!owner)throw std::runtime_error("preview Session ended");
        auto playback=model;playback.tempo=owner->playbackTempo(model);
        const auto program=compileAutomation(playback,owner->spec_.sampleRate);
        const auto bound=bindStripAutomation(program,suspended);
        HearingFlags hearing;hearing.soloInPlace=committed->cue.soloInPlace;
        hearing.soloIsCue=owner->spec_.performanceState && committed->cue.cueMode;hearing.referenceTrack=committed->cue.projectReferenceTrack;
        auto out=std::make_shared<Controls>();
        out->mixer=owner->strips_.prepareControls(model,&owner->transport_,bound,owner->spec_.delayCompensation,hearing);
        out->summing=owner->summing_.prepareControls(model);
        out->cue=owner->cueMonitor_.prepareControls(model,committed->cue,bound,owner->spec_.performanceState);
        for(const auto& strip:model.strips) {
            const auto gain=owner->inputGains_.find(strip.trackId);
            if(gain!=owner->inputGains_.end())out->input.emplace_back(gain->second.get(),strip.inputGainDb);
            else if(strip.inputGainDb!=0.0)throw std::runtime_error("input-gain preview requires an existing input-gain node");
        }
        // Link's per-graph bindings outrank a strip's raw lanes, so an overlay
        // or a generation is heard only through bindings of its own.
        auto lanes=std::make_shared<StripLaneBindings>();
        out->mixer->captureLanes(*lanes);out->cue->captureLanes(*lanes);out->lanes=std::move(lanes);
        return out;
    }
    std::set<std::int64_t> suspended(const std::vector<std::shared_ptr<Active>>& owners)const {
        auto* owner=engine();auto result=owner->overridden_;result.insert(owner->writingAutomation_.begin(),owner->writingAutomation_.end());
        const auto program=compileAutomation(committed->model,owner->spec_.sampleRate);
        const auto bound=bindStripAutomation(program,{});
        for(const auto& item:owners)if(item->current)for(const auto& patch:item->changes) {
            if(patch.operation=="routing.setGain") {
                if(const auto found=bound->sendLaneFor.find(patch.id);found!=bound->sendLaneFor.end())result.insert(found->second);
                continue;
            }
            std::optional<StripParam> parameter;
            if(patch.operation=="mixer.setVolume")parameter=StripParam::Volume;
            else if(patch.operation=="mixer.setPan")parameter=StripParam::Pan;
            else if(patch.operation=="mixer.setPanLeft")parameter=StripParam::PanLeft;
            else if(patch.operation=="mixer.setPanRight")parameter=StripParam::PanRight;
            else if(patch.operation=="track.setMute")parameter=StripParam::Mute;
            if(parameter)if(const auto found=bound->laneFor.find({patch.id,*parameter});found!=bound->laneFor.end())result.insert(found->second);
        }
        return result;
    }
    std::pair<std::unique_ptr<PublishedControls>,std::shared_ptr<const Snapshot>> compose(
        const std::vector<std::shared_ptr<Active>>& owners) {
        auto model=committed->model;
        for(const auto& item:owners)if(item->current)for(const auto& patch:item->changes)applyPatch(model,item->image->model,patch);
        auto out=std::make_unique<PublishedControls>();out->value=controls(model,suspended(owners));
        out->generation=engine()->gestureAudio_->currentGeneration();
        return {std::move(out),annotateSnapshot(*scalarSnapshot(*committed->snapshot,model),suspended(owners))};
    }
    std::shared_ptr<Generation> generation(const std::shared_ptr<const Image>& image,
        const std::set<std::int64_t>* committedOverrides=nullptr) {
        auto* owner=engine();if(!owner)throw std::runtime_error("preview Session ended");
        if(!owner->frozenSources_.empty() || !owner->frozenGroupChildren_.empty())
            throw std::runtime_error("structural preview of frozen playback is unsupported");
        auto out=std::make_shared<Generation>();out->image=image;
        auto playback=image->model;
        playback.tempo=owner->playbackTempo(image->model);
        out->clips=std::make_shared<ClipPlayback>(image->clips,owner->transport_,owner->spec_.sampleRate,
            owner->spec_.channels,owner->spec_.maxFrames,owner->spec_.progressiveDecoder);
        out->midi=std::make_shared<MidiClips>(playback,owner->transport_,owner->spec_.sampleRate,owner->midiState_,owner->eventPathFor(image->model));
        const auto program=compileAutomation(playback,owner->spec_.sampleRate);
        auto excluded=committedOverrides?*committedOverrides:owner->overridden_;excluded.insert(owner->writingAutomation_.begin(),owner->writingAutomation_.end());
        out->strips=bindStripAutomation(program,excluded);
        out->devices=bindDeviceAutomation(program,excluded,owner->transport_,owner->spec_.sampleRate);
        out->controls=controls(image->model,excluded);
        auto timing=std::make_shared<Session::TimingView>();timing->session=owner;
        timing->generationControls=out->controls;timing->gestureAudio=owner->gestureAudio_;
        out->tag=owner->gestureAudio_->reserveGeneration();timing->gestureGeneration=out->tag;
        timing->sampleRate=owner->spec_.sampleRate;timing->settings=owner->spec_.performanceState?image->transport:TransportSettings{};
        timing->spec=owner->spec_;timing->sourceReadiness=out->clips->readiness();timing->stripLanes=out->controls->lanes;
        for(const auto& event:playback.tempo)timing->tempo.events.push_back({event.posTicks,event.bpm,static_cast<int>(event.curve)});
        if(timing->tempo.events.empty())timing->tempo.events.push_back({0,120.0,0});
        auto sample=[&](std::int64_t tick){
            const long double value=static_cast<long double>(timing->tempo.ticksToSeconds(tick))*owner->spec_.sampleRate;
            if(!std::isfinite(value) || value<0 || value>static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
                throw std::runtime_error("preview transport time is unrepresentable");
            return static_cast<std::int64_t>(std::llround(value));
        };
        timing->loopBegin=sample(image->transport.loopStartTicks);timing->loopEnd=sample(image->transport.loopEndTicks);
        if(timing->loopEnd<=timing->loopBegin && timing->loopBegin<std::numeric_limits<std::int64_t>::max())timing->loopEnd=timing->loopBegin+1;
        timing->signatures=image->model.signatures;out->timing=timing;
        RealizeOptions options;options.channels=owner->spec_.channels;
        options.devicesFor=[owner](std::int64_t id){return owner->trackChain(id);};
        options.tapFor=[owner](std::int64_t id,Tap tap){return owner->tapFor(id,tap==Tap::PostFader?Tap::PostFx:tap);};
        options.sendFor=[owner](std::int64_t id)->Node*{return owner->strips_.sendFor(id);};
        options.sendTailFor=[owner](std::int64_t id){return owner->summing_.sendTailFor(id);};
        options.feedbackFor=[owner](std::int64_t id){auto* loop=owner->strips_.loopFor(id);return loop?FeedbackNodes{&loop->writer(),&loop->reader()}:FeedbackNodes{};};
        options.sourcesFor=[owner,clips=out->clips,midi=out->midi](std::int64_t id) {
            auto nodes=owner->sources_?owner->sources_(id):std::vector<Node*>{};
            const auto audio=clips->sourcesFor(id);nodes.insert(nodes.end(),audio.begin(),audio.end());
            const auto notes=midi->sourcesFor(id);nodes.insert(nodes.end(),notes.begin(),notes.end());return nodes;
        };
        options.eventSourceFor=[owner,binding=out->devices](const Node& node)->std::optional<EventSourceBinding> {
            for(const auto& entry:owner->entries_)if(&owner->devices_.nodeAt(entry.hostIndex)==&node) {
                const auto found=binding->byDevice.find(entry.deviceId);
                if(entry.retired || found==binding->byDevice.end())return EventSourceBinding{};
                return EventSourceBinding{found->second.get(),binding};
            }
            return std::nullopt;
        };
        options.monitors=owner->cueMonitor_.points([owner](std::int64_t id,Tap tap){return owner->tapFor(id,tap);});
        out->rebuild.model=[image]{return &image->model;};
        out->rebuild.channels=owner->spec_.channels;out->rebuild.sampleRate=owner->spec_.sampleRate;out->rebuild.maxFrames=owner->spec_.maxFrames;
        out->rebuild.devicesFor=options.devicesFor;out->rebuild.sourcesFor=options.sourcesFor;
        out->rebuild.tapFor=options.tapFor;out->rebuild.sendFor=options.sendFor;out->rebuild.sendTailFor=options.sendTailFor;
        out->rebuild.feedbackFor=options.feedbackFor;out->rebuild.eventSourceFor=options.eventSourceFor;
        out->rebuild.monitors=options.monitors;out->rebuild.transportState=timing;
        out->rebuild.nodePreparation=NodePreparePolicy::PreservePrepared;
        auto ready=owner->graph_.prepare(image->model,options,owner->spec_.sampleRate,owner->spec_.maxFrames,{},timing,NodePreparePolicy::PreservePrepared);
        if(!ready.ok())throw std::runtime_error(ready.error);
        out->ready=std::move(ready.graph);return out;
    }
    void viewCommitted(const std::shared_ptr<const Image>& image) {
        if(auto* target=project()) {
            // An ordinary document refresh can already have published this
            // base before a completed private device gesture's receipt is
            // drained. Its native runtime may have been reset meanwhile;
            // compare the actual committed view, not that runtime cache.
            const auto& shown=target->committed_;
            const bool sameOverrides=shown && shown->automationOverridden==image->snapshot->automationOverridden &&
                shown->automationLanes.size()==image->snapshot->automationLanes.size() &&
                std::equal(shown->automationLanes.begin(),shown->automationLanes.end(),image->snapshot->automationLanes.begin(),
                    [](const auto& a,const auto& b){return a->id==b->id && a->overridden==b->overridden;});
            if(sameBase(target->base_,image->base) && target->image_ && image->viewBytes &&
               *target->image_==*image->viewBytes && sameOverrides)return;
            target->adoptCommitted(image->snapshot,image->base,image->viewBytes);
        }
    }
    PresentationStatus adopt(const Store& store,const Base& base,const AppliedBatch*) {
        auto* owner=engine();if(!owner || !project())return {PresentationState::Detached,"preview endpoint ended",{}};
        if(sqlite3_get_autocommit(store.db().getHandle())==0)return refused("committed presentation adoption requires no SQL transaction");
        if(!owner->appliedSource_.sameConnection(base.source))return refused("preview source does not match the loaded Session");
        try {
            auto next=readImage(store,base,owner->overridden_,tempoRule(owner));
            if(committed && sameBase(committed->base,base) && committed->bytes==next->bytes &&
               committed->transport==next->transport && committed->cue==next->cue && committed->overrides==owner->overridden_) {
                if(!owner->presentationReady(base.source,base.sequence))return {PresentationState::Pending,"earlier device delivery or capture is pending",{}};
                // An own receipt may be drained after promotion and ordinary
                // document synchronisation. The identical base already has a
                // view; do not redraw or replace a composed preview on a tick.
                return {};
            }
            const bool topologyChanged=committed && encodeTables(topology(committed->tables))!=encodeTables(topology(next->tables));
            if(topologyChanged || owner->cueSettings_!=next->cue) {
                // Foreign topology is ordinary committed work, never a scratch
                // preview. Withdraw overlays, refresh the real Session, and
                // retain every gesture as suspended against the new base.
                refreshing=true;
                bool loaded=false;
                try {loaded=owner->refresh(store);} catch(...) {refreshing=false;throw;}
                refreshing=false;
                if(!loaded)return refused(owner->error());
                next->overrides=owner->overridden_;next->snapshot=annotateSnapshot(*next->snapshot,next->overrides);
                committed=next;
                for(auto& entry:active){entry->current=false;entry->restoreGeneration.reset();}
                renderedGeneration.reset();viewCommitted(next);
                if(!owner->presentationReady(base.source,base.sequence))return {PresentationState::Pending,"topology realised; earlier device delivery or capture is pending",{}};
                return {PresentationState::Current,"committed topology adopted; gesture targets require revalidation",{}};
            }
            if(!owner->presentationReady(base.source,base.sequence))return {PresentationState::Pending,"earlier device delivery or capture is pending",{}};
            auto nextOverrides=manualOverrides(owner->model_,next->model,owner->overridden_,owner->writingAutomation_,owner->spec_.sampleRate);
            next->snapshot=annotateSnapshot(*next->snapshot,nextOverrides);next->overrides=nextOverrides;
            const auto old=committed;committed=next;
            std::shared_ptr<Generation> baseGeneration;
            std::unique_ptr<PublishedControls> baseControls;
            rows::Model nextModel=next->model,nextPlayback=next->model;
            CueSettings nextCue=next->cue;
            nextPlayback.tempo=owner->playbackTempo(nextPlayback);
            // A foreign clip/tempo/automation edit also has to become audible
            // while a scalar gesture is suspended. Stage the full base once.
            try {
                if(owner->frozenSources_.empty() && owner->frozenGroupChildren_.empty())baseGeneration=generation(next,&nextOverrides);
                baseControls=std::make_unique<PublishedControls>();
                if(!baseGeneration) {baseControls->value=controls(next->model,nextOverrides);baseControls->generation=owner->gestureAudio_->currentGeneration();}
            } catch(...) {committed=old;throw;}
            // Structural generations carry their own controls and a new tag;
            // old overlays cannot cross that boundary. All throwing work
            // precedes the graph exchange.
            auto audio=std::static_pointer_cast<Audio>(owner->gestureAudio_);
            if(!baseGeneration)audio->publish(baseControls);
            if(baseGeneration && !owner->graph_.publish(*baseGeneration->ready)) {committed=old;return refused(owner->graph_.error());}
            if(baseGeneration) {owner->gestureAudio_->publishedGeneration(baseGeneration->tag);owner->stripAutomation_=baseGeneration->strips;owner->deviceAutomation_=baseGeneration->devices;owner->devices_.attachHost(owner->graph_,std::move(baseGeneration->rebuild));renderedGeneration=std::move(baseGeneration);}
            for(auto& entry:active){entry->current=false;entry->restoreGeneration.reset();}
            owner->model_=std::move(nextModel);owner->playbackModel_=std::move(nextPlayback);
            owner->overridden_=std::move(nextOverrides);
            owner->clipProject_=next->clips;owner->transportSettings_=next->transport;owner->cueSettings_=std::move(nextCue);
            owner->appliedSequence_=base.sequence;viewCommitted(next);
            return {};
        } catch(const std::exception& error){return refused(error.what());}
    }
    gesture::CaptureResult capture(const Store& store,const Base& base,const OwnerToken& token,gesture::Strategy strategy,
                                   std::span<const OpRequest> requests) {
        auto* owner=engine();if(!owner || !project())return {failed(Error::SourceLost,"preview endpoint ended"),{}, {}};
        if(!committed || !sameBase(committed->base,base))return {failed(Error::StaleBase,"preview committed base is stale"),{}, {}};
        try {
            auto changes=patches(requests,strategy);
            for(const auto& change:changes) {
                if(change.operation=="mixer.setInputGain" && !owner->inputGains_.contains(change.id))
                    return {failed(Error::UnsupportedPreview,"input-gain preview requires an existing input-gain node"),{}, {}};
                if(change.operation=="group.setSummingDrive" && !owner->summing_.bussOf(change.id))
                    return {failed(Error::UnsupportedPreview,"drive preview requires an existing native summing buss"),{}, {}};
            }
            auto image=readImage(store,base,owner->overridden_,tempoRule(owner));
            if(encodeTables(topology(image->tables))!=encodeTables(topology(committed->tables)))
                return {failed(Error::UnsupportedPreview,"preview changes native topology, device material, width or latency"),{}, {}};
            if(image->cue!=committed->cue)return {failed(Error::UnsupportedPreview,"preview changes cue routing"),{}, {}};
            auto value=std::make_unique<Captured>(token,image,committed,strategy,std::move(changes),owner->runtimeEpoch_);
            if(strategy==gesture::Strategy::LiveValues)image->snapshot=annotateSnapshot(*image->snapshot,suspended({std::make_shared<Active>(*value)}));
            return {{},image->snapshot,std::move(value),OpPresentationImageGuard{committed->bytes}};
        } catch(const std::exception& error){return {failed(Error::UnsupportedPreview,error.what()),{}, {}};}
    }
    gesture::PrepareResult prepare(std::unique_ptr<const gesture::CapturedPresentation> raw) {
        auto* value=dynamic_cast<const Captured*>(raw.get());
        if(!value)return {failed(Error::InvalidInput,"foreign engine preview capture"),{}};
        auto* owner=engine();if(!owner || !project())return {failed(Error::SourceLost,"preview endpoint ended"),{}};
        if(value->committed!=committed || value->epoch!=owner->runtimeEpoch_)return {failed(Error::StaleBase,"preview capture became stale"),{}};
        try {
            auto out=std::make_unique<Prepared>();out->capture.reset(static_cast<const Captured*>(raw.release()));
            for(const auto& existing:active)if(!existing->owner.sameOwner(value->owner))out->next.push_back(std::make_shared<Active>(*existing));
            auto added=std::make_shared<Active>(*value);out->next.push_back(added);
            if(value->strategy==gesture::Strategy::Structural) {
                if(out->next.size()!=1)throw std::runtime_error("structural preview conflicts with existing live controls");
                out->generation=generation(value->image);added->previewGeneration=out->generation;
                added->restoreGeneration=generation(committed);
                out->controls=std::make_unique<PublishedControls>();out->snapshot=value->image->snapshot;
                added->restoreControls=std::make_unique<PublishedControls>();added->restoreSnapshot=committed->snapshot;
            } else {
                for(const auto& item:out->next)if(item->strategy==gesture::Strategy::Structural)
                    throw std::runtime_error("live controls conflict with structural preview");
                auto composed=compose(out->next);out->controls=std::move(composed.first);out->snapshot=std::move(composed.second);
                // Cancellation never has to reconstruct a missing ownership
                // record or allocate controls. Refresh/update replaces every
                // owner's precomputed remaining-owner composition together.
                for(auto& item:out->next) {
                    std::vector<std::shared_ptr<Active>> remaining;
                    for(const auto& other:out->next)if(other!=item)remaining.push_back(other);
                    auto restore=compose(remaining);item->restoreControls=std::move(restore.first);item->restoreSnapshot=std::move(restore.second);
                }
            }
            return {{},std::move(out)};
        } catch(const std::exception& error){return {failed(Error::PreparationFailed,error.what()),{}};}
    }
    PresentationStatus publish(std::unique_ptr<gesture::PreparedPresentation> raw) {
        auto* value=dynamic_cast<Prepared*>(raw.get());auto* owner=engine();
        if(!value)return refused("foreign prepared engine preview");
        if(!owner || !project())return {PresentationState::Detached,"preview endpoint ended",{}};
        if(value->capture->committed!=committed || value->capture->epoch!=owner->runtimeEpoch_ ||
            !owner->appliedSource_.sameConnection(committed->base.source) || committed->base.source.sequence()!=committed->base.sequence)
            return refused("prepared preview source, base or format became stale");
        try {
            // A scalar publication and a structural graph publication are
            // mutually exclusive. Each preserving publisher does all throwing
            // reservation work before its single exchange.
            if(!value->generation)std::static_pointer_cast<Audio>(owner->gestureAudio_)->publish(value->controls);
            if(value->generation && !owner->graph_.publish(*value->generation->ready))return refused(owner->graph_.error());
            active=std::move(value->next);if(value->generation) {
                owner->gestureAudio_->publishedGeneration(value->generation->tag);
                owner->stripAutomation_=value->generation->strips;owner->deviceAutomation_=value->generation->devices;
                owner->devices_.attachHost(owner->graph_,std::move(value->generation->rebuild));renderedGeneration=std::move(value->generation);
            }
            project()->showPreview(std::move(value->snapshot));return {};
        } catch(const std::exception& error){return refused(error.what());}
    }
    PresentationStatus clear(const OwnerToken& token) {
        const auto at=std::find_if(active.begin(),active.end(),[&](const auto& entry){return entry->owner.sameOwner(token);});
        if(at==active.end())return {};
        const auto entry=*at;entry->restore->requested=true;
        auto* owner=engine();if(!owner) {entry->restore->result=PresentationState::Detached;return {PresentationState::Detached,"preview Session ended",entry->restore};}
        if(committed && !owner->presentationReady(committed->base.source,committed->base.sequence)) {
            entry->restore->result=PresentationState::Pending;entry->restore->diagnostic="earlier device delivery or capture prevents current-base restoration";
            return {PresentationState::Pending,entry->restore->diagnostic,entry->restore};
        }
        try {
            std::vector<std::shared_ptr<Active>> next;
            next.reserve(active.size()-1);
            for(const auto& item:active)if(item!=entry)next.push_back(std::make_shared<Active>(*item));
            std::unique_ptr<PublishedControls> nextControls;
            std::shared_ptr<const Snapshot> nextSnapshot;
            if(entry->strategy==gesture::Strategy::Structural) {
                nextControls=std::make_unique<PublishedControls>();nextSnapshot=committed->snapshot;
            } else {
                auto composed=compose(next);nextControls=std::move(composed.first);nextSnapshot=std::move(composed.second);
            }
            if(entry->strategy!=gesture::Strategy::Structural)std::static_pointer_cast<Audio>(owner->gestureAudio_)->publish(nextControls);
            if(entry->current && entry->restoreGeneration && entry->restoreGeneration->ready &&
               !owner->graph_.publish(*entry->restoreGeneration->ready)) {
                entry->restore->result=PresentationState::Pending;entry->restore->diagnostic=owner->graph_.error();
                return {PresentationState::Pending,entry->restore->diagnostic,entry->restore};
            }
            if(entry->current && entry->restoreGeneration) {
                owner->gestureAudio_->publishedGeneration(entry->restoreGeneration->tag);
                owner->stripAutomation_=entry->restoreGeneration->strips;owner->deviceAutomation_=entry->restoreGeneration->devices;
                owner->devices_.attachHost(owner->graph_,std::move(entry->restoreGeneration->rebuild));renderedGeneration=entry->restoreGeneration;
            }
            active=std::move(next);
            if(auto* target=project()) {
                if(active.empty())target->showCommitted();else target->showPreview(std::move(nextSnapshot));
            }
            entry->restore->result=PresentationState::Current;entry->restore->diagnostic.clear();return {};
        } catch(const std::exception& error) {
            entry->restore->result=PresentationState::Pending;entry->restore->diagnostic=error.what();
            return {PresentationState::Pending,entry->restore->diagnostic,entry->restore};
        }
    }
    PresentationStatus promote(const Store& store,const Base& base,const OwnerToken& token,const AppliedBatch& receipt) {
        const auto at=std::find_if(active.begin(),active.end(),[&](const auto& entry){return entry->owner.sameOwner(token);});
        if(at==active.end())return adopt(store,base,&receipt);
        auto* owner=engine();if(!owner || !project())return {PresentationState::Detached,"preview endpoint ended after durable commit",{}};
        try {
            auto actual=readImage(store,base,owner->overridden_,tempoRule(owner));
            const auto entry=*at;
            if(entry->image->bytes!=actual->bytes || entry->image->cue!=actual->cue || entry->image->transport!=actual->transport) {
                const auto status=adopt(store,base,&receipt);
                if(status.state!=PresentationState::Current)return status;
                return clear(token);
            }
            auto nextOverrides=manualOverrides(owner->model_,actual->model,owner->overridden_,owner->writingAutomation_,owner->spec_.sampleRate);
            actual->snapshot=annotateSnapshot(*actual->snapshot,nextOverrides);actual->overrides=nextOverrides;
            rows::Model nextModel=actual->model,nextPlayback=actual->model;
            nextPlayback.tempo=owner->playbackTempo(nextPlayback);
            committed=actual;owner->model_=std::move(nextModel);owner->playbackModel_=std::move(nextPlayback);
            owner->overridden_=std::move(nextOverrides);
            owner->clipProject_=actual->clips;owner->appliedSequence_=base.sequence;
            // SQL's actual typed after-image matched the heard material. Keep
            // its resources; removing ownership does not replay the operation.
            active.erase(at);viewCommitted(actual);
            if(active.empty())project()->showCommitted();
            else {auto composed=compose(active);std::static_pointer_cast<Audio>(owner->gestureAudio_)->publish(composed.first);project()->showPreview(std::move(composed.second));}
            return {};
        } catch(const std::exception& error){return {PresentationState::Pending,error.what(),{}};}
    }
    bool rebuild() {
        auto* owner=engine();if(!owner || !committed)return false;
        try {
            auto basis=committed;
            for(const auto& entry:active)if(entry->current && entry->strategy==gesture::Strategy::Structural)basis=entry->image;
            auto staged=generation(basis);
            std::unique_ptr<PublishedControls> overlay;
            if(std::none_of(active.begin(),active.end(),[](const auto& entry){return entry->strategy==gesture::Strategy::Structural;})) {
                auto value=compose(active);overlay=std::move(value.first);overlay->generation=staged->tag;
                // A tag for an unpublished graph is inert in the old graph.
                std::static_pointer_cast<Audio>(owner->gestureAudio_)->publish(overlay);
            }
            if(!owner->graph_.publish(*staged->ready)) {owner->error_=owner->graph_.error();return false;}
            owner->gestureAudio_->publishedGeneration(staged->tag);
            owner->stripAutomation_=staged->strips;owner->deviceAutomation_=staged->devices;
            owner->devices_.attachHost(owner->graph_,std::move(staged->rebuild));renderedGeneration=std::move(staged);
            owner->error_.clear();return true;
        } catch(const std::exception& error){owner->error_=error.what();return false;}
    }
    void pump(std::int64_t)override {
        for(std::size_t i=0;i<active.size();) {
            if(!active[i]->restore->requested){++i;continue;}
            const auto token=active[i]->owner;const auto before=active.size();(void)clear(token);
            if(active.size()==before)++i;
        }
    }
    bool releasable()const noexcept override{return !engine();}
    void sessionEnded()noexcept override {
        for(const auto& entry:active){entry->restore->result=PresentationState::Detached;}
        active.clear();renderedGeneration.reset();
    }
};
bool isGestureCommittedRefresh(const std::shared_ptr<GestureRuntime>& runtime) noexcept {return runtime && runtime->refreshing;}
bool gestureHoldsExternalTempo(const std::shared_ptr<GestureRuntime>& runtime) noexcept {return runtime && !runtime->active.empty();}
std::optional<bool> refreshGestureRuntime(const std::shared_ptr<GestureRuntime>& runtime,const Store& store) {
    if(!runtime || runtime->refreshing)return std::nullopt;
    if(runtime->active.empty()) {runtime->committed.reset();runtime->renderedGeneration.reset();return std::nullopt;}
    const auto source=adi::appliedSource(store);
    const auto status=runtime->adopt(store,{source,History(const_cast<Store&>(store)).head(),source.sequence()},nullptr);
    return status.state==PresentationState::Current || status.state==PresentationState::Pending;
}
std::optional<bool> rebuildGestureRuntime(const std::shared_ptr<GestureRuntime>& runtime) {
    if(!runtime || runtime->refreshing || runtime->active.empty())return std::nullopt;
    return runtime->rebuild();
}
ClipPlayback* gestureClips(const std::shared_ptr<GestureRuntime>& runtime) noexcept {
    return runtime && runtime->renderedGeneration ? runtime->renderedGeneration->clips.get():nullptr;
}
MidiClips* gestureMidi(const std::shared_ptr<GestureRuntime>& runtime) noexcept {
    return runtime && runtime->renderedGeneration ? runtime->renderedGeneration->midi.get():nullptr;
}
void pumpGestureCoordinator(Session& session,std::int64_t nowMs) {
    const auto coordinator=gesture::detail::coordinatorFor(session.appliedNotifications());
    if(coordinator.status.ok() && coordinator.value)(void)coordinator.value->drain(nowMs);
}
} // namespace adi::engine

namespace adi::gesture {
struct EnginePresentation::Data {std::shared_ptr<engine::GestureRuntime> runtime;};
EnginePresentation::EnginePresentation(engine::ProjectView& view,engine::Session& session):data_(std::make_unique<Data>()) {
    if(session.gestureRuntime_ && !session.gestureRuntime_->project() && session.gestureRuntime_->active.empty())
        session.gestureRuntime_->view=view.endpoint();
    if(session.gestureRuntime_ && session.gestureRuntime_->project()!=&view)
        throw std::runtime_error("Session already has a different live ProjectView presentation");
    if(!session.gestureRuntime_) {
        session.gestureRuntime_=std::make_shared<engine::GestureRuntime>(session,view);
        session.retainRuntimeClient(session.gestureRuntime_);
    }
    data_->runtime=session.gestureRuntime_;
}
EnginePresentation::~EnginePresentation()=default;
PresentationStatus EnginePresentation::adoptCommitted(const Store& store,const Base& base,const AppliedBatch* receipt){return data_->runtime->adopt(store,base,receipt);}
CaptureResult EnginePresentation::capture(const Store& store,const Base& base,const OwnerToken& owner,Strategy strategy,std::span<const OpRequest> requests){return data_->runtime->capture(store,base,owner,strategy,requests);}
PrepareResult EnginePresentation::prepare(std::unique_ptr<const CapturedPresentation> value){return data_->runtime->prepare(std::move(value));}
PresentationStatus EnginePresentation::publish(std::unique_ptr<PreparedPresentation> value){return data_->runtime->publish(std::move(value));}
PresentationStatus EnginePresentation::promote(const Store& store,const Base& base,const OwnerToken& owner,const AppliedBatch& receipt){return data_->runtime->promote(store,base,owner,receipt);}
PresentationStatus EnginePresentation::clear(const OwnerToken& owner){return data_->runtime->clear(owner);}
} // namespace adi::gesture
