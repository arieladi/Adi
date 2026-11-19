// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include "adi/row_ids.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
namespace adi {
namespace {
std::int64_t integer(const Payload& value){return detail::signedInteger(value);}
std::string identity(SQLite::Database& db) {
    SQLite::Statement query(db,"PRAGMA database_list");
    while(query.executeStep())if(query.getColumn(1).getString()=="main")return query.getColumn(2).getString();
    throw std::runtime_error("built-in insertion needs a project");
}
std::int64_t owner(SQLite::Database& db,std::int64_t chain) {
    std::set<std::int64_t> seen;
    for(;;) {
        if(!seen.insert(chain).second)throw std::runtime_error("destination rack contains a cycle");
        SQLite::Statement row(db,"SELECT track_id,parent_device_id FROM device_chains WHERE id=?");row.bind(1,chain);
        if(!row.executeStep())throw std::runtime_error("destination chain is missing");
        if(!row.getColumn(0).isNull())return row.getColumn(0).getInt64();
        if(row.getColumn(1).isNull())throw std::runtime_error("destination chain has no owner");
        SQLite::Statement parent(db,"SELECT chain_id FROM devices WHERE id=?");parent.bind(1,row.getColumn(1).getInt64());
        if(!parent.executeStep())throw std::runtime_error("destination rack owner is missing");
        chain=parent.getColumn(0).getInt64();
    }
}
Payload referenceRow(SQLite::Database& db,const Payload& values) {
    SQLite::Statement ddl(db,"SELECT sql FROM sqlite_master WHERE type='table' AND name='plugin_refs'");
    if(!ddl.executeStep())throw std::runtime_error("plug-in reference schema is missing");
    SQLite::Database scratch(":memory:",SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);scratch.exec(ddl.getColumn(0).getString());
    detail::restoreRow(scratch,"plugin_refs",values);
    return *detail::captureRow(scratch,"plugin_refs","id",integer(values.at("id")));
}
bool registerApply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        if(integer(payload.at("row").at("id"))!=integer(payload.at("id")))throw std::runtime_error("plug-in reference identity differs");
        detail::restoreRow(context.db,"plugin_refs",payload.at("row"));return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool registerInverse(OpContext&,const Payload& payload,Payload& inverse,std::string&) {
    inverse={{"id",payload.at("id")}};return true;
}
bool unregisterApply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        SQLite::Statement erase(context.db,"DELETE FROM plugin_refs WHERE id=?");erase.bind(1,integer(payload.at("id")));
        if(erase.exec()!=1)throw std::runtime_error("plug-in reference is missing");
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
bool unregisterInverse(OpContext& context,const Payload& payload,Payload& inverse,std::string& error) {
    try {
        const auto value=detail::captureRow(context.db,"plugin_refs","id",integer(payload.at("id")));
        if(!value)throw std::runtime_error("plug-in reference is missing");
        inverse={{"id",payload.at("id")},{"row",*value}};return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
constexpr std::array<Field,2> addFields={{{"id",FieldType::Int},{"row",FieldType::Object}}};
constexpr std::array<Field,1> removeFields={{{"id",FieldType::Int}}};
const std::array<OpDescriptor,2> ops={{
 {"pluginRef.register","Register a captured plug-in reference",Scope::Edit,EngineImpact::None,addFields,false,false,registerApply,registerInverse,"pluginRef.unregister"},
 {"pluginRef.unregister","Remove an unused plug-in reference",Scope::Edit,EngineImpact::None,removeFields,false,false,unregisterApply,unregisterInverse,"pluginRef.register"}
}};
}
std::span<const OpDescriptor> builtinReferenceOps(){return ops;}
PreparedBuiltinInsert prepareBuiltinInsert(Store& store,std::string_view uid,std::int64_t track,std::optional<std::int64_t> before) {
    PreparedBuiltinInsert result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        const auto* info=findBuiltin(uid);
        if(!info)throw std::runtime_error("unknown built-in device");
        if(!info->available)throw std::runtime_error(std::string(info->name)+" is not available through this built-in loader");
        auto& db=store.db();RowIds ids(db);std::optional<SQLite::Transaction> read;
        if(sqlite3_get_autocommit(db.getHandle())!=0)read.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        result.sourceFile=identity(db);result.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement target(db,"SELECT 1 FROM tracks WHERE id=?");target.bind(1,track);
        if(!target.executeStep())throw std::runtime_error("destination track is missing");
        const std::string label="Insert "+std::string(info->name);
        auto append=[&](const char* type,Payload payload,const char* kind,std::int64_t id) {
            OpRequest request;request.opType=type;request.payload=std::move(payload);request.label=label;
            request.targetKind=kind;request.targetId=id;result.requests.push_back(std::move(request));
        };
        SQLite::Statement reference(db,"SELECT id FROM plugin_refs WHERE format='internal' AND uid=? AND shell_id IS NULL");
        reference.bind(1,std::string(uid));
        if(reference.executeStep())result.referenceId=reference.getColumn(0).getInt64();
        else {
            result.referenceId=ids.next("plugin_refs");
            auto row=referenceRow(db,{{"id",result.referenceId},{"format","internal"},{"uid",uid},{"vendor","ADI"},
                {"name",info->name},{"version","1"},{"subtype","effect"},{"path_hint",""},{"is_shell",0},{"shell_id",nullptr}});
            append("pluginRef.register",{{"id",result.referenceId},{"row",row}},"plugin",result.referenceId);
        }
        if(before) {
            SQLite::Statement selected(db,"SELECT chain_id FROM devices WHERE id=?");selected.bind(1,*before);
            if(!selected.executeStep())throw std::runtime_error("selected device is missing");
            result.chainId=selected.getColumn(0).getInt64();
            if(owner(db,result.chainId)!=track)throw std::runtime_error("selected device belongs to another track");
        } else {
            SQLite::Statement chain(db,"SELECT id FROM device_chains WHERE track_id=? ORDER BY ord,id LIMIT 1");chain.bind(1,track);
            if(chain.executeStep())result.chainId=chain.getColumn(0).getInt64();
            else {
                result.chainId=ids.next("device_chains");
                append("chain.create",{{"id",result.chainId},{"track",track},{"ord",0},{"name","Devices"}},"chain",result.chainId);
            }
        }
        std::vector<std::pair<std::int64_t,std::int64_t>> order;
        SQLite::Statement devices(db,"SELECT id,ord FROM devices WHERE chain_id=? ORDER BY ord,id");devices.bind(1,result.chainId);
        while(devices.executeStep())order.emplace_back(devices.getColumn(0).getInt64(),devices.getColumn(1).getInt64());
        if(!std::in_range<std::int64_t>(order.size()))throw std::runtime_error("device order is exhausted");
        auto position=order.size();
        if(before)for(std::size_t i=0;i<order.size();++i)if(order[i].first==*before){position=i;break;}
        for(std::size_t i=0;i<order.size();++i) {
            const auto index=static_cast<std::int64_t>(i+(i>=position?1u:0u));
            if(order[i].second!=index)append("device.move",{{"id",order[i].first},{"chain",result.chainId},{"ord",index}},"device",order[i].first);
        }
        result.deviceId=ids.next("devices");
        append("device.insert",{{"id",result.deviceId},{"chain",result.chainId},{"ord",static_cast<std::int64_t>(position)},
            {"ref",result.referenceId},{"name",info->name},{"enabled",true},{"rack",false}},"device",result.deviceId);
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
CommitResult commitBuiltinInsert(Store& store,const PreparedBuiltinInsert& prepared,Actor actor) {
    CommitResult result;if(!prepared.ok){result.error=prepared.error;return result;}
    if(identity(store.db())!=prepared.sourceFile){result.error="project changed after built-in insertion preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}
}
