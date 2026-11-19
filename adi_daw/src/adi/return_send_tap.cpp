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
bool hasTable(const SQLite::Database& db, const char* name) {
    SQLite::Statement query(db,
        "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 "
        "UNION ALL SELECT 1 FROM sqlite_temp_master WHERE type='table' AND name=?1)");
    query.bind(1, name);
    return query.executeStep() && query.getColumn(0).getInt() != 0;
}
bool hasPolicies(SQLite::Database& db) {
    return hasTable(db, "return_send_taps");
}
std::optional<IndividualSendTap> parseIndividualTap(std::string_view name) {
    if (name == "pre_fx") return IndividualSendTap::PreFx;
    if (name == "post_fx") return IndividualSendTap::PostFx;
    if (name == "post_mixer") return IndividualSendTap::PostMixer;
    return std::nullopt;
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
bool setIndividualTap(OpContext& context, const Payload& payload, std::string& error) {
    try {
        const auto id = detail::signedInteger(payload.at("id"));
        SQLite::Statement route(context.db, "SELECT 1 FROM routing WHERE id=? AND kind='send'");
        route.bind(1, id);
        if (!route.executeStep()) throw std::runtime_error("send tap requires an existing send");
        if (const auto found = payload.find("tap"); found != payload.end()) {
            const auto name = found->get<std::string>();
            if (!parseIndividualTap(name)) throw std::runtime_error("unknown send tap");
            SQLite::Statement update(context.db,
                "INSERT INTO routing_send_taps(routing_id,tap) VALUES(?,?) "
                "ON CONFLICT(routing_id) DO UPDATE SET tap=excluded.tap");
            update.bind(1, id);
            update.bind(2, name);
            update.exec();
        } else {
            SQLite::Statement clear(context.db, "DELETE FROM routing_send_taps WHERE routing_id=?");
            clear.bind(1, id);
            clear.exec();
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
bool captureIndividualTap(OpContext& context, const Payload& payload, Payload& result, std::string& error) {
    try {
        const auto id = detail::signedInteger(payload.at("id"));
        result = {{"id", id}};
        SQLite::Statement query(context.db, "SELECT tap FROM routing_send_taps WHERE routing_id=?");
        query.bind(1, id);
        if (query.executeStep()) result["tap"] = query.getColumn(0).getString();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
}
std::string_view individualSendTapName(IndividualSendTap tap) noexcept {
    switch (tap) {
        case IndividualSendTap::PreFx: return "pre_fx";
        case IndividualSendTap::PostFx: return "post_fx";
        case IndividualSendTap::PostMixer: return "post_mixer";
    }
    return {};
}

std::map<std::int64_t, IndividualSendTap> readIndividualSendTaps(const SQLite::Database& db) {
    std::map<std::int64_t, IndividualSendTap> result;
    if (!hasTable(db, "routing_send_taps")) return result;
    SQLite::Statement query(db, "SELECT routing_id, tap FROM routing_send_taps");
    while (query.executeStep())
        if (const auto tap = parseIndividualTap(query.getColumn(1).getString()))
            result.emplace(query.getColumn(0).getInt64(), *tap);
    return result;
}

bool allowedSendToGroupedReturn(const SQLite::Database& db, std::int64_t source,
                                std::int64_t destination, std::optional<std::int64_t> movedTrack,
                                std::optional<std::int64_t> newParent) {
    SQLite::Statement check(db,
        "WITH RECURSIVE tree(id,kind,parent_id) AS ("
        " SELECT id,kind,CASE WHEN id=?3 THEN ?4 ELSE parent_id END FROM tracks), ancestors(id) AS ("
        " SELECT parent_id FROM tree WHERE id=?1 AND parent_id IS NOT NULL"
        " UNION SELECT t.parent_id FROM tree t JOIN ancestors a ON t.id=a.id"
        " WHERE t.parent_id IS NOT NULL)"
        " SELECT NOT EXISTS(SELECT 1 FROM tree d JOIN tree g ON g.id=d.parent_id"
        " WHERE d.id=?2 AND d.kind='return' AND g.kind='group' AND g.id NOT IN (SELECT id FROM ancestors))");
    check.bind(1, source);
    check.bind(2, destination);
    if (movedTrack) check.bind(3, *movedTrack);
    else check.bind(3);
    if (newParent) check.bind(4, *newParent);
    else check.bind(4);
    return check.executeStep() && check.getColumn(0).getInt() != 0;
}

std::vector<OpRequest> copyGroupSendRequests(const Store& store,
    const std::map<std::int64_t, std::int64_t>& trackIds,
    const std::map<std::int64_t, std::int64_t>& routingIds) {
    auto& db = const_cast<SQLite::Database&>(store.db());
    const auto taps = readIndividualSendTaps(db);
    std::vector<OpRequest> requests;
    for (const auto& [sourceId, copiedId] : routingIds) {
        auto route = detail::captureRow(db, "routing", "id", sourceId);
        if (!route || route->at("kind") != "send" || route->at("src_kind") != "track" ||
            route->at("dst_kind") != "track" || copiedId <= 0 || sourceId == copiedId)
            throw std::runtime_error("invalid routing ID for a copied send");
        const auto from = trackIds.find(detail::signedInteger(route->at("src_id")));
        if (from == trackIds.end() || from->second <= 0)
            throw std::runtime_error("the copied send needs its copied source track");
        auto destination = detail::signedInteger(route->at("dst_id"));
        if (const auto to = trackIds.find(destination); to != trackIds.end())
            destination = to->second;
        route->at("id") = copiedId;
        route->at("src_id") = from->second;
        route->at("dst_id") = destination;
        OpRequest request;
        request.opType = "routing.connect";
        request.payload = {{"id", copiedId}, {"srcKind", "track"}, {"src", from->second},
                           {"dstKind", "track"}, {"dst", destination}, {"kind", "send"}, {"row", *route}};
        if (taps.contains(sourceId)) {
            auto tap = detail::captureRow(db, "routing_send_taps", "routing_id", sourceId);
            tap->at("routing_id") = copiedId;
            request.payload["sendTapRow"] = std::move(*tap);
        }
        requests.push_back(std::move(request));
    }
    return requests;
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
    if (hasPolicies(db)) {
        SQLite::Statement policies(db, "SELECT track_id,pre_fader FROM return_send_taps");
        while (policies.executeStep()) {
            const auto found = result.find(policies.getColumn(0).getInt64());
            if (found != result.end()) found->second = {policies.getColumn(1).getInt() != 0, true, false};
        }
    }
    const auto individual = readIndividualSendTaps(db);
    std::map<std::int64_t,std::pair<bool,bool>> seen;
    std::map<std::int64_t, std::set<IndividualSendTap>> effective;
    SQLite::Statement edges(db,"SELECT dst_id,pre_fader,id FROM routing WHERE kind='send' AND dst_kind='track'");
    while(edges.executeStep()) {
        const auto id = edges.getColumn(0).getInt64();
        const auto policy = result.find(id);
        if (policy == result.end()) continue;
        const bool rawPre = edges.getColumn(1).getInt() != 0;
        auto& flags = seen[id];
        if (rawPre) flags.first = true;
        else flags.second = true;
        const bool pre = policy->second.explicitChoice ? policy->second.preFader : rawPre;
        const auto tap = individual.find(edges.getColumn(2).getInt64());
        effective[id].insert(tap != individual.end() ? tap->second
                             : pre ? IndividualSendTap::PostFx : IndividualSendTap::PostMixer);
    }
    for(auto& [id,tap]:result) {
        const auto flags = seen[id];
        if (!tap.explicitChoice) tap.preFader = flags.first && !flags.second;
        tap.legacyMixed = effective[id].size() > 1;
    }
    return result;
}
bool allowSendEdgeTapEdit(SQLite::Database& db,std::int64_t id,bool pre,std::string& error) {
    SQLite::Statement edge(db,"SELECT dst_id FROM routing WHERE id=? AND kind='send' AND dst_kind='track'");edge.bind(1,id);
    if(edge.executeStep())if(const auto preference=explicitReturnSendTap(db,edge.getColumn(0).getInt64());preference && *preference!=pre){error="Pre/Post belongs to the return; use return.setSendTap";return false;}
    return true;
}
std::span<const OpDescriptor> returnSendTapOps() {
    static const Field fields[]={{"id",FieldType::Int,true},{"pre",FieldType::Bool,false},{"restore",FieldType::Object,false}};
    static const Field individualFields[] = {
        {"id", FieldType::Int, true},
        {"tap", FieldType::Text, false},
    };
    static const OpDescriptor ops[] = {
        {"return.setSendTap","Choose Pre/Post on a return",Scope::Edit,EngineImpact::GraphRebuild,fields,false,false,apply,inverse,""},
        {"routing.setSendTap", "Choose the tap for one send", Scope::Edit, EngineImpact::GraphRebuild,
         individualFields, false, false, setIndividualTap, captureIndividualTap, ""},
    };
    return ops;
}
PreparedReturnSend prepareSendToReturn(Store& store,std::int64_t routingId,std::int64_t sourceTrack,std::int64_t returnTrack) {
    PreparedReturnSend result;
    try {
        if(store.readOnly() || routingId<=0 || sourceTrack<=0)throw std::runtime_error("invalid new send or read-only project");
        auto& db=store.db();std::optional<SQLite::Transaction> transaction;if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        target(db,returnTrack);SQLite::Statement source(db,"SELECT 1 FROM tracks WHERE id=?");source.bind(1,sourceTrack);if(!source.executeStep())throw std::runtime_error("send source track missing");
        if (!allowedSendToGroupedReturn(db, sourceTrack, returnTrack))
            throw std::runtime_error("the source must be inside the group that owns this return");
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
