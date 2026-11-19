// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_copy.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <sqlite3.h>
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
DeviceRemoteReference remoteReference(SQLite::Database& db,std::int64_t id) {
    SQLite::Statement row(db,"SELECT host,port,protocol FROM remote_hosts WHERE id=?");row.bind(1,id);
    if(!row.executeStep())throw std::runtime_error("copied remote host reference is missing");
    return {id,row.getColumn(1).getInt64(),row.getColumn(0).getString(),row.getColumn(2).getString()};
}
void verifyReferences(Store& store,const DeviceCopy& copy) {
    const auto& reference=copy.insert.at("ref");
    if(!reference.is_null()) {
        if(!copy.reference || reference!=copy.reference->id)throw std::runtime_error("copied plug-in identity is not captured");
        SQLite::Statement row(store.db(),"SELECT format,uid,shell_id FROM plugin_refs WHERE id=?");row.bind(1,copy.reference->id);
        if(!row.executeStep())throw std::runtime_error("copied plug-in reference is missing");
        std::optional<std::int64_t> shell;if(!row.getColumn(2).isNull())shell=row.getColumn(2).getInt64();
        if(row.getColumn(0).getString()!=copy.reference->format || row.getColumn(1).getString()!=copy.reference->uid || shell!=copy.reference->shellId)
            throw std::runtime_error("plug-in reference identity changed after copy");
    }
    if(copy.insert.contains("remoteHost") && !copy.insert.at("remoteHost").is_null()) {
        if(!copy.remoteHost || copy.insert.at("remoteHost")!=copy.remoteHost->id)
            throw std::runtime_error("copied remote host identity is not captured");
        if(remoteReference(store.db(),copy.remoteHost->id)!=*copy.remoteHost)
            throw std::runtime_error("remote host identity changed after copy");
    }
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
        if(result.insert.contains("remoteHost") && !result.insert.at("remoteHost").is_null())
            result.remoteHost=remoteReference(store.db(),result.insert.at("remoteHost").get<std::int64_t>());
        SQLite::Statement nested(store.db(),"SELECT 1 FROM device_chains WHERE parent_device_id=? LIMIT 1");
        nested.bind(1,id);result.hasNestedChains=nested.executeStep();result.ok=true;
    } catch(const std::exception& error){result.error=error.what();}
    return result;
}
namespace {
PreparedDevicePaste preparePaste(Store &store, const DeviceCopy &copy, std::int64_t newId,
                                 std::int64_t chain, std::optional<std::int64_t> before,
                                 std::optional<std::int64_t> track) {
    PreparedDevicePaste result;
    try {
        if(audio::onAudioThread)throw std::runtime_error("device paste belongs off the audio callback");
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(!copy.ok)throw std::runtime_error(copy.error);
        if(copy.hasNestedChains)throw std::runtime_error("rack paste needs a nested chain image");
        Read read(store.db());result.sourceFile=identity(store);
        if(copy.sourceFile!=result.sourceFile)throw std::runtime_error("device clipboard belongs to a different project");
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        verifyReferences(store,copy);
        if(newId==copy.insert.at("id").get<std::int64_t>())throw std::runtime_error("device copy needs a new identity");
        SQLite::Statement occupied(store.db(),"SELECT 1 FROM devices WHERE id=?");occupied.bind(1,newId);
        if(occupied.executeStep())throw std::runtime_error("new device id is occupied");
        OpTransactionInputs guards;
        if (track) {
            const auto row = detail::captureRow(store.db(), "tracks", "id", *track);
            if (!row)
                throw std::runtime_error("destination track is missing");
            guards.rowGuards.push_back({"tracks", {{"id", *track}}, row});
            const Payload selector = {{"track_id", *track}, {"parent_device_id", nullptr}};
            guards.rowSetGuards.push_back(
                {"device_chains", selector,
                 detail::captureGuardRows(store, "device_chains", selector)});
            SQLite::Statement target(store.db(),
                                     "SELECT id FROM device_chains WHERE track_id=? AND "
                                     "parent_device_id IS NULL ORDER BY ord,id LIMIT 1");
            target.bind(1, *track);
            if (target.executeStep())
                chain = target.getColumn(0).getInt64();
            else {
                // Same bounded MAX strategy as native built-in insertion; the
                // shared-allocator consolidation remains the M2 follow-up.
                SQLite::Statement last(store.db(), "SELECT coalesce(MAX(id),0) FROM device_chains");
                last.executeStep();
                const auto id = std::max<std::int64_t>(0, last.getColumn(0).getInt64());
                if (id == std::numeric_limits<std::int64_t>::max())
                    throw std::runtime_error("device chain identities are exhausted");
                chain = id + 1;
                guards.rowGuards.push_back({"device_chains", {{"id", chain}}, std::nullopt});
                OpRequest create;
                create.opType = "chain.create";
                create.payload = {
                    {"id", chain}, {"track", *track}, {"ord", 0}, {"name", "Devices"}};
                create.label = "Paste Device";
                create.targetKind = "chain";
                create.targetId = chain;
                result.requests.push_back(std::move(create));
            }
            guards.rowGuards.push_back({"devices", {{"id", newId}}, std::nullopt});
            const Payload devices = {{"chain_id", chain}};
            guards.rowSetGuards.push_back(
                {"devices", devices, detail::captureGuardRows(store, "devices", devices)});
            if (copy.reference)
                guards.rowGuards.push_back(
                    {"plugin_refs",
                     {{"id", copy.reference->id}},
                     detail::captureRow(store.db(), "plugin_refs", "id", copy.reference->id)});
        } else {
            SQLite::Statement target(store.db(), "SELECT 1 FROM device_chains WHERE id=?");
            target.bind(1, chain);
            if (!target.executeStep())
                throw std::runtime_error("destination chain is missing");
        }
        result.chainId = chain;
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
        if (track) {
            auto packet =
                CapturedTransactionAccess::seal(appliedSource(store), *result.options.expectHead,
                                                result.requests, std::move(guards));
            for (auto &request : result.requests)
                request.capturedTransaction = packet;
        }
        result.ok=true;
    } catch(const std::exception& error){result.error=error.what();}
    return result;
}
} // namespace
PreparedDevicePaste prepareDevicePaste(Store &store, const DeviceCopy &copy, std::int64_t newId,
                                       std::int64_t chain, std::optional<std::int64_t> before) {
    return preparePaste(store, copy, newId, chain, before, std::nullopt);
}
PreparedDevicePaste prepareDevicePasteForTrack(Store &store, const DeviceCopy &copy,
                                               std::int64_t newId, std::int64_t track) {
    return preparePaste(store, copy, newId, 0, std::nullopt, track);
}
CommitResult commitDevicePaste(Store& store,const PreparedDevicePaste& prepared,Actor actor) {
    CommitResult result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(prepared.sourceFile!=identity(store)){result.error="project changed after device paste preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}
}
