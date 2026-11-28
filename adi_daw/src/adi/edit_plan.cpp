// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_plan.hpp"
#include "adi/automation_clipboard.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace adi::edit_plan {
struct CapturedDescription {
    Description value;
    std::string sourceFile;
    std::optional<std::optional<std::int64_t>> head;
    std::vector<OpRequest> requests;
};
namespace {
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db) {
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
    }
};
std::string filename(Store& store) {
    SQLite::Statement query(store.db(),"PRAGMA database_list");
    while(query.executeStep())if(query.getColumn(1).getString()=="main")return query.getColumn(2).getString();
    throw std::runtime_error("placement needs a project connection");
}
int clockBase(PlacementClock clock) {
    if(clock==PlacementClock::Ticks)return 0;
    if(clock==PlacementClock::Nanoseconds)return 1;
    throw std::runtime_error("placement needs a declared operation clock");
}
std::int64_t integer(const Payload& value) {return adi::detail::signedInteger(value);}
std::int64_t convert(std::int64_t value,int from,int to,const edit::TimeMap& time) {
    if(value<0 || (from!=0 && from!=1) || (to!=0 && to!=1))throw std::runtime_error("invalid placement coordinate");
    return from==to?value:(to==0?time.ticks(value):time.nanos(value));
}
int nativeBase(const Payload& image) {
    const auto base=integer(image.at("row").at("time_base"));
    if(base!=0 && base!=1)throw std::runtime_error("invalid clip clock");
    return static_cast<int>(base);
}
PlacementSpan nativeSpan(const Payload& image) {
    const auto base=nativeBase(image);const auto& row=image.at("row");
    const auto first=integer(row.at(base==0?"pos_ticks":"pos_ns"));
    const auto& extent=row.at(base==0?"length_ticks":"length_ns");
    if(extent.is_null())throw std::runtime_error("placement needs finite clip extents");
    const auto count=integer(extent);
    if(first<0 || count<0)throw std::runtime_error("placement needs nonnegative clip extents");
    return {first,edit::addTime(first,count)};
}
PlacementSpan interval(const Payload& image,int base,const edit::TimeMap& time) {
    const auto old=nativeSpan(image);const auto native=nativeBase(image);
    PlacementSpan result{convert(old.begin,native,base,time),convert(old.end,native,base,time)};
    if(old.begin<old.end && result.begin>=result.end)throw std::runtime_error("clip extent collapses in the operation clock");
    return result;
}
PlacementLane laneOf(const Payload& image) {
    const auto& row=image.at("row");PlacementLane result{integer(row.at("track_id")),std::nullopt};
    if(!row.at("lane_id").is_null())result.lane=integer(row.at("lane_id"));
    return result;
}
MaterialState state(const Payload& image,int base,const edit::TimeMap& time) {
    MaterialState result;result.id=integer(image.at("id"));result.lane=laneOf(image);
    const auto native=nativeBase(image);result.nativeClock=native==0?PlacementClock::Ticks:PlacementClock::Nanoseconds;
    result.nativeSpan=nativeSpan(image);result.physicalSpan=interval(image,1,time);
    result.span={convert(result.nativeSpan.begin,native,base,time),convert(result.nativeSpan.end,native,base,time)};
    if(image.at("row").contains("name"))result.name=image.at("row").at("name").get<std::string>();
    return result;
}
bool has(Domains domains,Domains flag) {
    return (static_cast<std::uint32_t>(domains)&static_cast<std::uint32_t>(flag))!=0;
}
void validateScope(Store& store,const EditScope& scope,const Options& options) {
    (void)clockBase(scope.clock);
    if(!std::isfinite(options.referenceRate) || options.referenceRate<=0 || options.referenceRate>768000)
        throw std::runtime_error("invalid reference render rate");
    if(scope.targets.empty() || scope.bounds.begin<0 || scope.bounds.end<=scope.bounds.begin)
        throw std::runtime_error("placement scope needs literal targets and ordered bounds");
    const auto supported=static_cast<std::uint32_t>(Domains::Clips|Domains::Automation);
    if(!has(scope.domains,Domains::Clips) || (static_cast<std::uint32_t>(scope.domains)&~supported)!=0)
        throw std::runtime_error("material placement does not yet implement these scope domains");
    const auto& policy=options.policy;
    if(policy.preset!=Preset::Live)throw std::runtime_error("non-Live placement policy is not implemented");
    if(policy.ripple!=Ripple::Off)throw std::runtime_error("ripple placement requires the ED-07 planner");
    if(!policy.deleteOverlaps)throw std::runtime_error("deleteOverlaps=false placement policy is unresolved under ADR-0217");
    if(policy.mutedClipsAsDeleted)throw std::runtime_error("mutedClipsAsDeleted placement policy is not implemented");
    std::set<PlacementLane> unique;
    for(const auto& lane:scope.targets) {
        if(!unique.insert(lane).second)throw std::runtime_error("placement scope has duplicate targets");
        SQLite::Statement track(store.db(),"SELECT id FROM tracks WHERE id=?");track.bind(1,lane.track);
        if(!track.executeStep())throw std::runtime_error("placement target track is missing");
        if(lane.lane) {
            SQLite::Statement take(store.db(),"SELECT 1 FROM lanes WHERE id=? AND track_id=? AND kind='take'");
            take.bind(1,*lane.lane);take.bind(2,lane.track);
            if(!take.executeStep())throw std::runtime_error("placement take lane is missing or belongs to another track");
        }
    }
}
void admitTracks(Store& store,const std::set<std::int64_t>& tracks) {
    const auto groups=freeze::groups(store.db());
    for(const auto id:tracks) {
        SQLite::Statement track(store.db(),"SELECT frozen FROM tracks WHERE id=?");track.bind(1,id);
        if(!track.executeStep())continue; // A clipboard owns its source even after deletion.
        if(track.getColumn(0).getInt()!=0)throw std::runtime_error("frozen placement requires TM-FC-04 policy");
        for(const auto& [root,members]:groups) {
            if(root==id || std::any_of(members.begin(),members.end(),[&](const auto& member){return member.id==id;}))
                throw std::runtime_error("frozen-group placement requires TM-FC-04 policy");
        }
    }
}
bool nearMask(const Payload& image,const std::vector<ClearInterval>& masks,const edit::TimeMap& time) {
    const auto lane=laneOf(image);const auto& row=image.at("row");
    if(std::none_of(masks.begin(),masks.end(),[&](const ClearInterval& mask) {
        return mask.lane==lane && mask.span.begin<mask.span.end;
    }))return false;
    const auto base=nativeBase(image);
    const auto first=integer(row.at(base==0?"pos_ticks":"pos_ns"));
    if(first<0)throw std::runtime_error("placement needs nonnegative clip positions");
    auto physical=[&](std::int64_t value) {
        return base==1?static_cast<long double>(value):std::round(static_cast<long double>(time.seconds(value))*1e9L);
    };
    const auto physicalFirst=physical(first);
    for(const auto& mask:masks) {
        if(mask.lane!=lane || mask.span.begin>=mask.span.end)continue;
        // Same forward rounding as TimeMap::nanos, without narrowing distant
        // endpoints to int64. Inverse ticks() loses precision at large times.
        if(physicalFirst>static_cast<long double>(mask.span.end))continue;
        const auto& length=row.at(base==0?"length_ticks":"length_ns");
        if(length.is_null())return true;
        const auto count=integer(length);
        if(count<0)throw std::runtime_error("placement needs nonnegative clip extents");
        const auto last=edit::addTime(first,count);
        if(physical(last)<static_cast<long double>(mask.span.begin))continue;
        return true;
    }
    return false;
}
std::int64_t directedTick(std::int64_t nanos,PlacementSpan native,bool beginning,const edit::TimeMap& time) {
    auto low=native.begin,high=native.end;
    // TimeMap::ticks truncates, and nanos(ticks) rounds independently. Search
    // the captured finite native range, whose endpoints are already checked.
    while(low<high) {
        const auto distance=high-low;
        const auto middle=low+distance/2+(beginning?0:distance%2);
        if(beginning) {
            if(time.nanos(middle)<nanos)low=middle+1;else high=middle;
        } else {
            if(time.nanos(middle)>nanos)high=middle-1;else low=middle;
        }
    }
    return low;
}
void retain(edit::RowEdits& rows,const Payload& image) {
    const auto id=integer(image.at("id"));auto root=image.at("row");
    // A removed alias source may already have detached this retained row.
    if(auto current=rows.current("clips",id);current && current->at("alias_of").is_null())root["alias_of"]=nullptr;
    rows.set("clips",id,root);detail::retainImageDetails(rows,image);
}
void bindDescription(clip_edit::PreparedEdit& result,Description description) {
    auto captured=std::make_shared<CapturedDescription>();captured->value=std::move(description);
    captured->value.ok=true;captured->sourceFile=result.sourceFile;captured->head=result.options.expectHead;
    captured->requests=result.requests;result.description=std::move(captured);
}
} // namespace

EditScope materialScope(std::span<const std::int64_t> tracks,PlacementClock clock,PlacementSpan span) {
    EditScope result;result.clock=clock;result.bounds=span;
    for(const auto track:tracks)result.targets.push_back({track,std::nullopt});
    return result;
}

clip_edit::PreparedEdit detail::prepareCaptured(Store& store,const EditScope& scope,PlacementBatch batch,
    const Options& options,clip_edit::CopyIds& ids,const char* label) {
    clip_edit::PreparedEdit result;
    try {
        Read read(store.db());validateScope(store,scope,options);const auto base=clockBase(scope.clock);
        result.sourceFile=filename(store);result.options.expectHead.emplace(OpJournal(store).headSeq());
        edit::TimeMap time(store.db());OpContext context{store,store.db()};
        // Physical nanoseconds are this resolver request's single declared
        // clock. Public literal scope and descriptions retain their explicit
        // input clock; rounded tick projections cannot hide physical overlap.
        PlacementInput input;input.clock=PlacementClock::Nanoseconds;input.scopeLanes=scope.targets;
        input.scopeSpan={convert(scope.bounds.begin,base,1,time),convert(scope.bounds.end,base,1,time)};
        input.relocated=batch.relocated;
        for(const auto& clear:batch.clear) {
            const PlacementSpan span{convert(clear.span.begin,base,1,time),convert(clear.span.end,base,1,time)};
            if(clear.span.begin<clear.span.end && span.begin>=span.end)throw std::runtime_error("clear mask collapses in physical time");
            input.clear.push_back({clear.lane,span});
        }
        std::map<std::int64_t,Payload> images,incoming;
        std::set<std::int64_t> affected(batch.sourceTracks.begin(),batch.sourceTracks.end());
        const auto touch=[&](const PlacementLane& lane) {affected.insert(lane.track);};
        for(auto& image:batch.incoming) {
            const auto id=integer(image.at("id"));const auto lane=laneOf(image);
            input.incoming.push_back({id,lane,interval(image,1,time)});touch(lane);
            if(!incoming.emplace(id,std::move(image)).second)throw std::runtime_error("duplicate incoming clip identity");
            ids.reserve("clips",id);
            const auto& captured=incoming.at(id);
            for(const auto& child:clip_edit::childTables()) {
                if(std::string_view(child.key)!="id" || !captured.at("children").contains(child.table))continue;
                for(const auto& row:captured.at("children").at(child.table))ids.reserve(child.table,integer(row.at(child.key)));
            }
        }
        auto masks=input.clear;
        for(const auto& placed:input.incoming)masks.push_back({placed.lane,placed.span});
        const std::set<std::int64_t> relocated(batch.relocated.begin(),batch.relocated.end());
        for(const auto& lane:scope.targets) {
            SQLite::Statement query(store.db(),"SELECT id FROM clips WHERE track_id=? AND lane_id IS ? ORDER BY id");
            query.bind(1,lane.track);
            if(lane.lane)query.bind(2,*lane.lane);else query.bind(2);
            while(query.executeStep()) {
                const auto id=query.getColumn(0).getInt64();
                const Payload root={{"row",*edit::row(store.db(),"clips",id)}};
                if(!relocated.contains(id) && !nearMask(root,masks,time))continue;
                auto image=clip_edit::captureImage(context,id);
                input.originals.push_back({id,laneOf(image),interval(image,1,time)});images.emplace(id,std::move(image));
            }
        }
        for(const auto& clear:batch.clear)touch(clear.lane);
        for(const auto id:batch.relocated) {
            const auto found=images.find(id);
            if(found!=images.end())touch(laneOf(found->second));
        }
        admitTracks(store,affected);
        const auto resolved=resolvePlacement(input);
        if(!resolved.ok)throw std::runtime_error(toString(resolved.error));
        // Capture/freeze only changed survivors, and only once per original.
        // The interval resolver has already cleared every mask before this
        // owner adapter stages a single incoming image.
        edit::RowEdits rows(store.db());Description description;description.scope=scope;description.policy=options.policy;
        for(const auto& original:resolved.originals) {
            if(original.relocated)continue;
            auto image=images.at(original.id);
            if(original.survivors.size()==1 && original.survivors.front()==original.original)continue;
            Effect effect;effect.before=state(image,base,time);
            if(original.survivors.empty()) {
                // An alias scheduled for removal needs no detach update, even
                // when its ID sorts before its removed source's ID.
                auto& links=image["alias_links"];
                links.erase(std::remove_if(links.begin(),links.end(),[&](const Payload& link) {
                    return std::binary_search(resolved.removed.begin(),resolved.removed.end(),integer(link));
                }),links.end());
                effect.kind=EffectKind::Removed;clip_edit::deleteImage(rows,image);
            } else {
                effect.kind=original.survivors.size()==1?EffectKind::Trimmed:EffectKind::Split;
                // Retained roots may preserve unknown extension rows, but a
                // clone must not silently omit children this build cannot own.
                if(original.survivors.size()>1)(void)clip_edit::captureDeleted(context,original.id);
                detail::freezeImage(store,image,time,options.referenceRate);
                const auto native=nativeSpan(image);const auto nativeClock=nativeBase(image);
                for(std::size_t index=0;index<original.survivors.size();++index) {
                    const auto span=original.survivors[index];
                    const auto begin=span.begin==original.original.begin?native.begin:
                        (nativeClock==1?span.begin:directedTick(span.begin,native,true,time));
                    const auto end=span.end==original.original.end?native.end:
                        (nativeClock==1?span.end:directedTick(span.end,native,false,time));
                    if(begin>=end)throw std::runtime_error("placement survivor collapses in its native clock");
                    auto survivor=index==0?image:clip_edit::cloneImage(image,ids.next("clips"),ids);
                    detail::trimFrozenImage(survivor,begin,end,time,options.referenceRate);
                    const auto actual=interval(survivor,1,time);
                    // Checked native-clock conversion may leave a harmless gap.
                    // It must never put material back inside a cleared mask.
                    if(actual.begin<span.begin || actual.end>span.end)
                        throw std::runtime_error("placement cut intrudes into a cleared mask after native-clock conversion");
                    effect.after.push_back(state(survivor,base,time));
                    if(index==0)retain(rows,survivor);else clip_edit::createImage(rows,survivor);
                }
            }
            description.effects.push_back(std::move(effect));
        }
        for(const auto& placed:resolved.incoming) {
            auto& image=incoming.at(placed.id);Effect effect;effect.kind=batch.incomingKind;
            const bool moved=std::find(batch.relocated.begin(),batch.relocated.end(),placed.id)!=batch.relocated.end();
            if(moved) {
                effect.before=state(images.at(placed.id),base,time);
                if(images.at(placed.id)==image)continue;
                retain(rows,image);
            } else clip_edit::createImage(rows,image);
            effect.after.push_back(state(image,base,time));description.effects.push_back(std::move(effect));
        }
        if(options.policy.automationFollowsClips && has(scope.domains,Domains::Automation)) {
            // Read all move sources from the original snapshot, then clear
            // their spans before placing any destination, including overlapping moves.
            const auto none=Payload::array();
            if(batch.incomingKind==EffectKind::Moved) {
                for(const auto& placed:resolved.incoming) {
                    const auto& source=images.at(placed.id);
                    const auto from=laneOf(source);
                    const auto sourceSpan=nativeSpan(source);
                    const auto& destination=incoming.at(placed.id);
                    const auto destinationSpan=nativeSpan(destination);
                    if(from.lane || placed.lane.lane || sourceSpan.begin==sourceSpan.end ||
                       (from==placed.lane && sourceSpan==destinationSpan))continue;
                    const auto clock=nativeBase(source);
                    const clip_edit::TimePoint first{clock,sourceSpan.begin},last{clock,sourceSpan.end};
                    auto copied=edit::copyAutomationMaterial(store,time,from.track,first,last,options.referenceRate);
                    edit::placeAutomationMaterial(store,rows,ids,time,from.track,first,last,none,options.referenceRate);
                    batch.envelopes.push_back({placed.lane.track,{clock,destinationSpan.begin},
                                               {clock,destinationSpan.end},std::move(copied)});
                }
            }
            for(const auto& envelope:batch.envelopes)
                edit::placeAutomationMaterial(store,rows,ids,time,envelope.track,envelope.begin,
                                               envelope.end,envelope.envelopes,options.referenceRate);
        }
        const auto changes=rows.changes();std::string error;
        if(!changes.empty() && !edit::validateRows(context,{{"changes",changes}},error))throw std::runtime_error(error);
        if(!changes.empty()) {
            OpRequest request;request.opType="arranger.splice";request.payload={{"id",0},{"changes",changes}};
            request.label=label;request.targetKind="clip";result.requests.push_back(std::move(request));
        }
        result.ok=true;bindDescription(result,std::move(description));
    } catch(const std::exception& error) {result.ok=false;result.error=error.what();result.requests.clear();result.description.reset();}
    return result;
}

namespace {
clip_edit::PreparedEdit prepareMoves(Store& store,const EditScope* scope,std::span<const Move> moves,const Options& options) {
    try {
        Read read(store.db());
        if(scope)validateScope(store,*scope,options);
        edit::TimeMap time(store.db());
        OpContext context{store,store.db()};clip_edit::CopyIds ids(store.db());detail::PlacementBatch batch;
        batch.incomingKind=EffectKind::Moved;
        EditScope inferred;inferred.clock=PlacementClock::Nanoseconds;
        inferred.bounds.begin=std::numeric_limits<std::int64_t>::max();
        std::set<PlacementLane> lanes;
        for(const auto& move:moves) {
            auto image=clip_edit::captureImage(context,move.id);const auto native=nativeBase(image);
            if(scope && move.position.base!=clockBase(scope->clock))
                throw std::runtime_error("move position must use the declared operation clock");
            const auto destination=convert(move.position.value,move.position.base,native,time);
            const auto before=nativeSpan(image);const auto source=laneOf(image);
            if((source.lane || move.to.lane) && source!=move.to)
                throw std::runtime_error("change or detach the take lane explicitly before moving it");
            if(!scope) {
                lanes.insert(source);lanes.insert(move.to);
                const auto count=before.end-before.begin;
                inferred.bounds.begin=std::min(inferred.bounds.begin,convert(std::min(before.begin,destination),native,1,time));
                inferred.bounds.end=std::max(inferred.bounds.end,convert(std::max(before.end,edit::addTime(destination,count)),native,1,time));
            }
            if(source!=move.to || destination!=before.begin)detail::freezeImage(store,image,time,options.referenceRate);
            image["row"][native==0?"pos_ticks":"pos_ns"]=destination;image["row"]["track_id"]=move.to.track;
            image["row"]["lane_id"]=move.to.lane?Payload(*move.to.lane):Payload(nullptr);
            batch.relocated.push_back(move.id);batch.incoming.push_back(std::move(image));
        }
        if(!scope) {
            inferred.targets.assign(lanes.begin(),lanes.end());
            if(inferred.bounds.end==inferred.bounds.begin)inferred.bounds.end=edit::addTime(inferred.bounds.end,1);
            scope=&inferred;
        }
        return detail::prepareCaptured(store,*scope,std::move(batch),options,ids,"Move Material");
    } catch(const std::exception& error) {clip_edit::PreparedEdit result;result.error=error.what();return result;}
}
} // namespace

clip_edit::PreparedEdit prepareMove(Store& store,const EditScope& scope,std::span<const Move> moves,const Options& options) {
    return prepareMoves(store,&scope,moves,options);
}
clip_edit::PreparedEdit prepareMove(Store& store,std::span<const Move> moves,const Options& options) {
    return prepareMoves(store,nullptr,moves,options);
}
clip_edit::PreparedEdit prepareResizeEnd(Store& store,const EditScope& scope,ResizeEnd resize,const Options& options) {
    try {
        Read read(store.db());validateScope(store,scope,options);edit::TimeMap time(store.db());
        OpContext context{store,store.db()};auto image=clip_edit::captureImage(context,resize.id);
        if(resize.end.base!=clockBase(scope.clock))throw std::runtime_error("resize end must use the declared operation clock");
        const auto end=convert(resize.end.value,resize.end.base,nativeBase(image),time);
        if(end!=nativeSpan(image).end) {
            detail::freezeImage(store,image,time,options.referenceRate);
            detail::resizeFrozenEnd(image,end,time,options.referenceRate);
        }
        clip_edit::CopyIds ids(store.db());detail::PlacementBatch batch;batch.incomingKind=EffectKind::Resized;
        batch.relocated.push_back(resize.id);batch.incoming.push_back(std::move(image));
        return detail::prepareCaptured(store,scope,std::move(batch),options,ids,"Resize Material End");
    } catch(const std::exception& error) {clip_edit::PreparedEdit result;result.error=error.what();return result;}
}
clip_edit::PreparedEdit prepareTrim(Store& store,const EditScope& scope,Trim trim,const Options& options) {
    try {
        Read read(store.db());validateScope(store,scope,options);edit::TimeMap time(store.db());
        OpContext context{store,store.db()};auto image=clip_edit::captureImage(context,trim.id);
        if(trim.begin.base!=clockBase(scope.clock))throw std::runtime_error("trim begin must use the declared operation clock");
        const auto begin=convert(trim.begin.value,trim.begin.base,nativeBase(image),time);
        const auto first=nativeSpan(image).begin;
        if(begin!=first) {
            // A source-zero clamp can turn an extension into a no-op. Retain
            // absent optional clock rows and opaque original bytes in that case.
            auto original=image;
            detail::freezeImage(store,image,time,options.referenceRate);
            detail::trimFrozenStart(image,begin,time,options.referenceRate);
            if(nativeSpan(image).begin==first)image=std::move(original);
        }
        clip_edit::CopyIds ids(store.db());detail::PlacementBatch batch;
        batch.incomingKind=EffectKind::Trimmed;batch.relocated.push_back(trim.id);
        batch.incoming.push_back(std::move(image));
        return detail::prepareCaptured(store,scope,std::move(batch),options,ids,"Trim Material Start");
    } catch(const std::exception& error) {clip_edit::PreparedEdit result;result.error=error.what();return result;}
}
clip_edit::PreparedEdit prepareDelete(Store& store,const EditScope& scope,std::span<const std::int64_t> clips,
                                       const Options& options) {
    clip_edit::PreparedEdit result;
    try {
        Read read(store.db());
        validateScope(store,scope,options);
        const auto base=clockBase(scope.clock);
        edit::TimeMap time(store.db());
        OpContext context{store,store.db()};
        const PlacementSpan bounds{convert(scope.bounds.begin,base,1,time),convert(scope.bounds.end,base,1,time)};
        std::set<std::int64_t> seen,tracks;
        edit::RowEdits rows(store.db());clip_edit::CopyIds ids(store.db());
        Description description;description.scope=scope;description.policy=options.policy;
        result.sourceFile=filename(store);
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        for(const auto id:clips) {
            if(!seen.insert(id).second)throw std::runtime_error("duplicate delete clip identity");
            const auto image=clip_edit::captureImage(context,id);
            const auto lane=laneOf(image);
            const auto span=interval(image,1,time);
            if(std::find(scope.targets.begin(),scope.targets.end(),lane)==scope.targets.end() ||
               span.begin<bounds.begin || span.end>bounds.end)
                throw std::runtime_error("delete clip lies outside the literal scope");
            tracks.insert(lane.track);
            if(!lane.lane && options.policy.automationFollowsClips && has(scope.domains,Domains::Automation)) {
                const auto native=nativeBase(image);
                const auto extent=nativeSpan(image);
                edit::placeAutomationMaterial(store,rows,ids,time,lane.track,
                    {native,extent.begin},{native,extent.end},Payload::array(),options.referenceRate);
            }
            OpRequest request;request.opType="clip.delete";request.payload={{"id",id}};
            request.label="Delete Material";request.targetKind="clip";request.targetId=id;
            result.requests.push_back(std::move(request));
            description.effects.push_back({EffectKind::Removed,state(image,base,time),{}});
        }
        admitTracks(store,tracks);
        const auto changes=rows.changes();
        if(!changes.empty()) {
            OpRequest request;request.opType="arranger.splice";request.payload={{"id",0},{"changes",changes}};
            request.label="Delete Material";request.targetKind="clip";
            result.requests.push_back(std::move(request));
        }
        result.ok=true;
        bindDescription(result,std::move(description));
    } catch(const std::exception& error) {
        result={};
        result.error=error.what();
    }
    return result;
}
clip_edit::PreparedEdit prepareDelete(Store& store,std::span<const std::int64_t> clips,const Options& options) {
    if(clips.empty()){clip_edit::PreparedEdit result;result.error="no clips to delete";return result;}
    try {
        Read read(store.db());edit::TimeMap time(store.db());OpContext context{store,store.db()};
        EditScope scope;scope.clock=PlacementClock::Nanoseconds;
        scope.bounds.begin=std::numeric_limits<std::int64_t>::max();
        std::set<PlacementLane> lanes;
        for(const auto id:clips) {
            const auto image=clip_edit::captureImage(context,id);
            const auto span=interval(image,1,time);
            scope.bounds.begin=std::min(scope.bounds.begin,span.begin);
            scope.bounds.end=std::max(scope.bounds.end,span.end);
            lanes.insert(laneOf(image));
        }
        if(scope.bounds.end==scope.bounds.begin)scope.bounds.end=edit::addTime(scope.bounds.end,1);
        scope.targets.assign(lanes.begin(),lanes.end());
        return prepareDelete(store,scope,clips,options);
    } catch(const std::exception& error) {clip_edit::PreparedEdit result;result.error=error.what();return result;}
}
clip_edit::PreparedEdit prepareCompTake(Store& store, const EditScope& scope,
                                        std::int64_t takeClip, const Options& options) {
    try {
        Read read(store.db());
        if (scope.domains != Domains::Clips)
            throw std::runtime_error("comp scope can change only clips");
        auto compOptions = options;
        compOptions.policy.automationFollowsClips = false;
        validateScope(store, scope, compOptions);
        const auto base = clockBase(scope.clock);
        edit::TimeMap time(store.db());
        OpContext context{store, store.db()};
        auto image = clip_edit::captureDeleted(context, takeClip);
        const auto source = laneOf(image);
        const PlacementLane main{source.track, std::nullopt};
        if (!source.lane || scope.targets.size() != 2 ||
            std::find(scope.targets.begin(), scope.targets.end(), source) == scope.targets.end() ||
            std::find(scope.targets.begin(), scope.targets.end(), main) == scope.targets.end())
            throw std::runtime_error("comp requires the owning take and main lane of one track");
        const auto& row = image.at("row");
        if ((row.at("kind") != "audio" && row.at("kind") != "midi") || !row.at("alias_of").is_null())
            throw std::runtime_error("comp requires an ordinary audio or MIDI take");
        const PlacementSpan selection{convert(scope.bounds.begin, base, 1, time),
                                      convert(scope.bounds.end, base, 1, time)};
        const auto physical = interval(image, 1, time);
        if (selection.begin < physical.begin || selection.end > physical.end || selection.begin >= selection.end)
            throw std::runtime_error("comp range must be fully covered by this take");

        const auto native = nativeSpan(image);
        const auto begin = nativeBase(image) == 1 ? selection.begin :
            directedTick(selection.begin, native, true, time);
        const auto end = nativeBase(image) == 1 ? selection.end :
            directedTick(selection.end, native, false, time);
        detail::freezeImage(store, image, time, options.referenceRate);
        detail::trimFrozenImage(image, begin, end, time, options.referenceRate);
        if (interval(image, 1, time) != selection)
            throw std::runtime_error("comp range must align to the take's native clock");

        clip_edit::CopyIds ids(store.db());
        auto copy = clip_edit::cloneImage(image, ids.next("clips"), ids);
        copy["row"]["lane_id"] = nullptr;
        detail::PlacementBatch batch;
        batch.incoming.push_back(std::move(copy));
        return detail::prepareCaptured(store, scope, std::move(batch), compOptions, ids, "Comp Take");
    } catch (const std::exception& error) {
        clip_edit::PreparedEdit result;
        result.error = error.what();
        return result;
    }
}

Description describe(const clip_edit::PreparedEdit& prepared) {
    Description result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(!prepared.description){result.error="this prepared edit has no captured placement description";return result;}
    const auto& captured=*prepared.description;
    if(prepared.sourceFile!=captured.sourceFile || prepared.options.expectHead!=captured.head || prepared.requests.size()<captured.requests.size()) {
        result.error="prepared placement changed after its description was captured";return result;
    }
    for(std::size_t index=0;index<captured.requests.size();++index) {
        const auto& before=captured.requests[index];const auto& after=prepared.requests[index];
        if(before.opType!=after.opType || before.payload!=after.payload || before.targetKind!=after.targetKind || before.targetId!=after.targetId) {
            result.error="prepared placement request changed after its description was captured";return result;
        }
    }
    result=captured.value;
    for(std::size_t index=captured.requests.size();index<prepared.requests.size();++index) {
        const auto& request=prepared.requests[index];result.appended.push_back({request.opType,request.label,request.payload});
    }
    return result;
}
} // namespace adi::edit_plan
