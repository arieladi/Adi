// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/edit_plan.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
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
        if(lane.lane)throw std::runtime_error("ordinary placement currently supports main lanes; take-lane editing requires CT-01");
    }
}
bool relevantAutomation(Store& store,std::int64_t track) {
    // Authored lane existence is significant, even without a stream or any
    // interior points. UNION terminates malformed recursive rack cycles.
    SQLite::Statement query(store.db(),
        "WITH RECURSIVE owned_devices(id) AS ("
        "SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id WHERE c.track_id=? "
        "UNION SELECT d.id FROM devices d JOIN device_chains c ON c.id=d.chain_id "
        "JOIN owned_devices p ON p.id=c.parent_device_id) "
        "SELECT 1 FROM automation_lanes a WHERE (a.owner_kind='track' AND a.owner_id=?) "
        "OR (a.owner_kind='device' AND a.owner_id IN (SELECT id FROM owned_devices)) "
        "OR (a.owner_kind='routing' AND a.owner_id IN "
        "(SELECT id FROM routing WHERE src_kind='track' AND src_id=? AND kind='send')) LIMIT 1");
    query.bind(1,track);query.bind(2,track);query.bind(3,track);return query.executeStep();
}
void admitTracks(Store& store,const std::set<std::int64_t>& tracks,const EditScope& scope,const EditPolicy& policy) {
    const auto groups=freeze::groups(store.db());
    for(const auto id:tracks) {
        SQLite::Statement track(store.db(),"SELECT frozen FROM tracks WHERE id=?");track.bind(1,id);
        if(!track.executeStep()) {
            if(policy.automationFollowsClips)throw std::runtime_error("copied source automation owner is no longer available; AU-05 needs captured envelopes");
            continue; // Literal destination targets were already validated.
        }
        if(track.getColumn(0).getInt()!=0)throw std::runtime_error("frozen placement requires TM-FC-04 policy");
        for(const auto& [root,members]:groups) {
            if(root==id || std::any_of(members.begin(),members.end(),[&](const auto& member){return member.id==id;}))
                throw std::runtime_error("frozen-group placement requires TM-FC-04 policy");
        }
        if(policy.automationFollowsClips && relevantAutomation(store,id)) {
            if(!has(scope.domains,Domains::Automation))throw std::runtime_error("unlocked placement must explicitly include automation in its scope");
            throw std::runtime_error("unlocked source/destination automation placement requires AU-05 guarded replacement");
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
        for(auto& image:batch.incoming) {
            const auto id=integer(image.at("id"));const auto lane=laneOf(image);
            input.incoming.push_back({id,lane,interval(image,1,time)});affected.insert(lane.track);
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
            SQLite::Statement query(store.db(),"SELECT id FROM clips WHERE track_id=? AND lane_id IS NULL ORDER BY id");query.bind(1,lane.track);
            while(query.executeStep()) {
                const auto id=query.getColumn(0).getInt64();
                const Payload root={{"row",*edit::row(store.db(),"clips",id)}};
                if(!relocated.contains(id) && !nearMask(root,masks,time))continue;
                auto image=clip_edit::captureImage(context,id);
                input.originals.push_back({id,laneOf(image),interval(image,1,time)});images.emplace(id,std::move(image));
            }
        }
        for(const auto& clear:batch.clear)affected.insert(clear.lane.track);
        for(const auto id:batch.relocated) {
            const auto found=images.find(id);
            if(found!=images.end())affected.insert(laneOf(found->second).track);
        }
        admitTracks(store,affected,scope,options.policy);
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

clip_edit::PreparedEdit prepareMove(Store& store,const EditScope& scope,std::span<const Move> moves,const Options& options) {
    try {
        Read read(store.db());validateScope(store,scope,options);edit::TimeMap time(store.db());
        OpContext context{store,store.db()};clip_edit::CopyIds ids(store.db());detail::PlacementBatch batch;
        batch.incomingKind=EffectKind::Moved;
        for(const auto& move:moves) {
            auto image=clip_edit::captureImage(context,move.id);const auto native=nativeBase(image);
            if(move.position.base!=clockBase(scope.clock))throw std::runtime_error("move position must use the declared operation clock");
            const auto destination=convert(move.position.value,move.position.base,native,time);
            const auto before=nativeSpan(image);const auto source=laneOf(image);
            if(source!=move.to || destination!=before.begin)detail::freezeImage(store,image,time,options.referenceRate);
            image["row"][native==0?"pos_ticks":"pos_ns"]=destination;image["row"]["track_id"]=move.to.track;
            image["row"]["lane_id"]=move.to.lane?Payload(*move.to.lane):Payload(nullptr);
            batch.relocated.push_back(move.id);batch.incoming.push_back(std::move(image));
        }
        return detail::prepareCaptured(store,scope,std::move(batch),options,ids,"Move Material");
    } catch(const std::exception& error) {clip_edit::PreparedEdit result;result.error=error.what();return result;}
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
