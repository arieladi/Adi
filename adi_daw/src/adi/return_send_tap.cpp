// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/return_send_tap.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <set>
#include <stdexcept>
namespace adi {
namespace {
bool hasPolicies(SQLite::Database& db) {
    return db.execAndGet("SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name='return_send_taps') OR EXISTS(SELECT 1 FROM sqlite_temp_master WHERE type='table' AND name='return_send_taps')").getInt()!=0;
}
void target(SQLite::Database& db,std::int64_t id) {
    if(id<=0)throw std::runtime_error("return tap needs a positive track id");
    SQLite::Statement track(db,"SELECT kind FROM tracks WHERE id=?");track.bind(1,id);
    if(!track.executeStep() || track.getColumn(0).getString()!="return")throw std::runtime_error("send tap belongs to a return track");
}
Payload state(SQLite::Database& db,std::int64_t id) {
    Payload result={{"choice",nullptr},{"routes",Payload::array()}};
    SQLite::Statement preference(db,"SELECT pre_fader FROM return_send_taps WHERE track_id=?");preference.bind(1,id);
    if(preference.executeStep())result["choice"]=preference.getColumn(0).getInt()!=0;
    SQLite::Statement edges(db,"SELECT id,pre_fader FROM routing WHERE kind='send' AND dst_kind='track' AND dst_id=? ORDER BY id");edges.bind(1,id);
    while(edges.executeStep())result["routes"].push_back({{"id",edges.getColumn(0).getInt64()},{"pre",edges.getColumn(1).getInt()!=0}});
    return result;
}
std::set<std::int64_t> edgeIds(const Payload& image) {
    if(!image.is_object() || image.size()!=2 || (!image.at("choice").is_null() && !image.at("choice").is_boolean()) || !image.at("routes").is_array())throw std::runtime_error("invalid return tap image");
    std::set<std::int64_t> ids;std::optional<std::int64_t> previous;
    for(const auto& edge:image.at("routes")) {
        if(!edge.is_object() || edge.size()!=2 || !edge.at("pre").is_boolean())throw std::runtime_error("invalid return tap edge image");
        const auto id=detail::signedInteger(edge.at("id"));
        if(previous && id<=*previous)throw std::runtime_error("return tap edge image must be ordered and unique");
        previous=id;ids.insert(id);
    }
    return ids;
}
void validateRestore(SQLite::Database& db,std::int64_t id,const Payload& restore) {
    if(!restore.is_object() || restore.size()!=2 || edgeIds(restore.at("current"))!=edgeIds(restore.at("target")))throw std::runtime_error("return tap restore changes its edge set");
    if(state(db,id)!=restore.at("current"))throw std::runtime_error("return tap or incoming edge set changed before restore");
}
void write(SQLite::Database& db,std::int64_t id,const Payload& image) {
    if(image.at("choice").is_null()) {
        SQLite::Statement erase(db,"DELETE FROM return_send_taps WHERE track_id=?");erase.bind(1,id);erase.exec();
    }else {
        SQLite::Statement preference(db,"INSERT INTO return_send_taps(track_id,pre_fader) VALUES(?,?) ON CONFLICT(track_id) DO UPDATE SET pre_fader=excluded.pre_fader");preference.bind(1,id);preference.bind(2,image.at("choice").get<bool>()?1:0);preference.exec();
    }
    for(const auto& edge:image.at("routes")) {
        SQLite::Statement update(db,"UPDATE routing SET pre_fader=? WHERE id=?");update.bind(1,edge.at("pre").get<bool>()?1:0);update.bind(2,detail::signedInteger(edge.at("id")));
        if(update.exec()!=1)throw std::runtime_error("incoming return edge disappeared");
    }
}
Payload desired(SQLite::Database& db,std::int64_t id,const Payload& payload) {
    if(payload.contains("restore")) {
        if(payload.contains("pre"))throw std::runtime_error("return tap takes pre or restore, not both");
        validateRestore(db,id,payload.at("restore"));return payload.at("restore").at("target");
    }
    if(!payload.contains("pre") || !payload.at("pre").is_boolean())throw std::runtime_error("return tap needs a boolean choice");
    auto after=state(db,id);after["choice"]=payload.at("pre");for(auto& edge:after["routes"])edge["pre"]=payload.at("pre");return after;
}
bool apply(OpContext& context,const Payload& payload,std::string& error) {
    try {const auto id=detail::signedInteger(payload.at("id"));target(context.db,id);const auto after=desired(context.db,id,payload);write(context.db,id,after);return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
bool inverse(OpContext& context,const Payload& payload,Payload& result,std::string& error) {
    try {const auto id=detail::signedInteger(payload.at("id"));target(context.db,id);const auto before=state(context.db,id);const auto after=desired(context.db,id,payload);result={{"id",id},{"restore",{{"current",after},{"target",before}}}};return true;}
    catch(const std::exception& exception){error=exception.what();return false;}
}
std::string fileIdentity(SQLite::Database& db) {
    const auto* filename=sqlite3_db_filename(db.getHandle(),"main");return filename?filename:"";
}
}
std::optional<bool> explicitReturnSendTap(SQLite::Database& db,std::int64_t id) {
    if(!hasPolicies(db))return {};
    SQLite::Statement query(db,"SELECT p.pre_fader FROM return_send_taps p JOIN tracks t ON t.id=p.track_id WHERE p.track_id=? AND t.kind='return'");query.bind(1,id);
    if(query.executeStep())return query.getColumn(0).getInt()!=0;
    return {};
}
std::map<std::int64_t,ReturnSendTap> readReturnSendTaps(const Store& store) {
    std::map<std::int64_t,ReturnSendTap> result;auto& db=const_cast<SQLite::Database&>(store.db());
    SQLite::Statement tracks(db,"SELECT id FROM tracks WHERE kind='return'");while(tracks.executeStep())result.emplace(tracks.getColumn(0).getInt64(),ReturnSendTap{});
    std::map<std::int64_t,std::pair<bool,bool>> seen;
    SQLite::Statement edges(db,"SELECT dst_id,pre_fader FROM routing WHERE kind='send' AND dst_kind='track'");
    while(edges.executeStep()) {
        const auto id=edges.getColumn(0).getInt64();if(!result.contains(id))continue;
        auto& flags=seen[id];if(edges.getColumn(1).getInt()!=0)flags.first=true;else flags.second=true;
    }
    for(auto& [id,tap]:result) {
        const auto flags=seen[id];tap.preFader=flags.first && !flags.second;tap.legacyMixed=flags.first && flags.second;
    }
    if(hasPolicies(db)) {
        SQLite::Statement policies(db,"SELECT track_id,pre_fader FROM return_send_taps");
        while(policies.executeStep()) {
            const auto found=result.find(policies.getColumn(0).getInt64());if(found==result.end())continue;
            found->second={policies.getColumn(1).getInt()!=0,true,false};
        }
    } // An older schema retains legacy routing.
    return result;
}
bool allowSendEdgeTapEdit(SQLite::Database& db,std::int64_t id,bool pre,std::string& error) {
    SQLite::Statement edge(db,"SELECT dst_id FROM routing WHERE id=? AND kind='send' AND dst_kind='track'");edge.bind(1,id);
    if(edge.executeStep())if(const auto preference=explicitReturnSendTap(db,edge.getColumn(0).getInt64());preference && *preference!=pre){error="Pre/Post belongs to the return; use return.setSendTap";return false;}
    return true;
}
std::span<const OpDescriptor> returnSendTapOps() {
    static const Field fields[]={{"id",FieldType::Int,true},{"pre",FieldType::Bool,false},{"restore",FieldType::Object,false}};
    static const OpDescriptor ops[]={{"return.setSendTap","Choose Pre/Post on a return",Scope::Edit,EngineImpact::GraphRebuild,fields,false,false,apply,inverse,""}};
    return ops;
}
PreparedReturnSend prepareSendToReturn(Store& store,std::int64_t routingId,std::int64_t sourceTrack,std::int64_t returnTrack) {
    PreparedReturnSend result;
    try {
        if(store.readOnly() || routingId<=0 || sourceTrack<=0)throw std::runtime_error("invalid new send or read-only project");
        auto& db=store.db();std::optional<SQLite::Transaction> transaction;if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        target(db,returnTrack);SQLite::Statement source(db,"SELECT 1 FROM tracks WHERE id=?");source.bind(1,sourceTrack);if(!source.executeStep())throw std::runtime_error("send source track missing");
        const auto tap=readReturnSendTaps(store).at(returnTrack);result.sourceFile=fileIdentity(db);result.options.expectHead.emplace(OpJournal(store).headSeq());
        OpRequest connect;connect.opType="routing.connect";connect.label="Create Send";connect.payload={{"id",routingId},{"srcKind","track"},{"src",sourceTrack},{"dstKind","track"},{"dst",returnTrack},{"kind","send"}};result.requests.push_back(connect);
        if(!tap.explicitChoice && tap.preFader){OpRequest inherit;inherit.opType="routing.setPreFader";inherit.label=connect.label;inherit.payload={{"id",routingId},{"pre",true}};result.requests.push_back(std::move(inherit));}
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
CommitResult commitSendToReturn(Store& store,const PreparedReturnSend& prepared,Actor actor) {
    CommitResult result;if(!prepared.ok){result.error=prepared.error;return result;}
    if(fileIdentity(store.db())!=prepared.sourceFile){result.error="project changed after send preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;return OpJournal(store).commit(requests,prepared.options);
}
}
