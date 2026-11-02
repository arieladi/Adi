// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_copy.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace adi {
namespace {
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db) {
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
    }
};
std::string identity(Store& store) {
    SQLite::Statement row(store.db(),"PRAGMA database_list");
    while(row.executeStep())if(row.getColumn(1).getString()=="main")return row.getColumn(2).getString();
    throw std::runtime_error("device clipboard needs a project connection");
}
}
DeviceCopy readDeviceCopy(Store& store,std::int64_t id) {
    DeviceCopy result;
    try {
        if(audio::onAudioThread)throw std::runtime_error("device copy belongs off the audio callback");
        Read read(store.db());result.sourceFile=identity(store);
        const auto* descriptor=OpRegistry::instance().find("device.remove");
        OpContext context{store,store.db()};
        if(!descriptor || !descriptor->buildInverse ||
           !descriptor->buildInverse(context,{{"id",id}},result.insert,result.error))return result;
        if(!result.insert.at("ref").is_null()) {
            SQLite::Statement reference(store.db(),"SELECT id,format,uid,vendor,name,version,subtype,path_hint,is_shell,shell_id FROM plugin_refs WHERE id=?");
            reference.bind(1,result.insert.at("ref").get<std::int64_t>());
            if(!reference.executeStep())throw std::runtime_error("device plug-in reference is missing");
            rows::PluginRef value;
            value.id=reference.getColumn(0).getInt64();value.format=reference.getColumn(1).getString();
            value.uid=reference.getColumn(2).getString();value.vendor=reference.getColumn(3).getString();
            value.name=reference.getColumn(4).getString();value.version=reference.getColumn(5).getString();
            value.subtype=reference.getColumn(6).getString();value.pathHint=reference.getColumn(7).getString();
            value.isShell=reference.getColumn(8).getInt()!=0;
            if(!reference.getColumn(9).isNull())value.shellId=reference.getColumn(9).getInt64();
            result.reference=std::move(value);
        }
        SQLite::Statement nested(store.db(),"SELECT 1 FROM device_chains WHERE parent_device_id=? LIMIT 1");
        nested.bind(1,id);result.hasNestedChains=nested.executeStep();result.ok=true;
    } catch(const std::exception& error){result.error=error.what();}
    return result;
}
PreparedDevicePaste prepareDevicePaste(Store& store,const DeviceCopy& copy,std::int64_t newId,
                                       std::int64_t chain,std::optional<std::int64_t> before) {
    PreparedDevicePaste result;
    try {
        if(audio::onAudioThread)throw std::runtime_error("device paste belongs off the audio callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(!copy.ok)throw std::runtime_error(copy.error);
        if(copy.hasNestedChains)throw std::runtime_error("rack paste needs a nested chain image");
        Read read(store.db());result.sourceFile=identity(store);
        if(copy.sourceFile!=result.sourceFile)throw std::runtime_error("device clipboard belongs to a different project");
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        if(newId==copy.insert.at("id").get<std::int64_t>())throw std::runtime_error("device copy needs a new identity");
        SQLite::Statement occupied(store.db(),"SELECT 1 FROM devices WHERE id=?");occupied.bind(1,newId);
        if(occupied.executeStep())throw std::runtime_error("new device id is occupied");
        SQLite::Statement target(store.db(),"SELECT 1 FROM device_chains WHERE id=?");target.bind(1,chain);
        if(!target.executeStep())throw std::runtime_error("destination chain is missing");
        std::vector<std::pair<std::int64_t,std::int64_t>> order;
        SQLite::Statement existing(store.db(),"SELECT id,ord FROM devices WHERE chain_id=? ORDER BY ord,id");existing.bind(1,chain);
        while(existing.executeStep())order.emplace_back(existing.getColumn(0).getInt64(),existing.getColumn(1).getInt64());
        auto position=order.size();
        if(before) {
            const auto found=std::find_if(order.begin(),order.end(),[&](const auto& item){return item.first==*before;});
            if(found==order.end())throw std::runtime_error("selected device is not in the destination chain");
            position=static_cast<std::size_t>(found-order.begin());
        }
        if(!std::in_range<std::int64_t>(order.size()))throw std::runtime_error("device chain order is exhausted");
        // Canonical integer order also handles legacy ties and ord=INT64_MAX;
        // every original value is restored by the moves' captured inverses.
        for(std::size_t i=0;i<order.size();++i) {
            const auto index=static_cast<std::int64_t>(i+(i>=position?1u:0u));
            if(order[i].second==index)continue;
            OpRequest move;move.opType="device.move";move.payload={{"id",order[i].first},{"chain",chain},{"ord",index}};
            move.label="Paste Device";move.targetKind="device";move.targetId=order[i].first;result.requests.push_back(std::move(move));
        }
        OpRequest insert;insert.opType="device.insert";insert.payload=copy.insert;
        insert.payload["id"]=newId;insert.payload["chain"]=chain;insert.payload["ord"]=static_cast<std::int64_t>(position);
        insert.label="Paste Device";insert.targetKind="device";insert.targetId=newId;result.requests.push_back(std::move(insert));
        result.ok=true;
    } catch(const std::exception& error){result.error=error.what();}
    return result;
}
CommitResult commitDevicePaste(Store& store,const PreparedDevicePaste& prepared,Actor actor) {
    CommitResult result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(prepared.sourceFile!=identity(store)){result.error="project changed after device paste preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}
}
