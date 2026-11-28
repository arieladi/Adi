// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_copy.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/mix_functions.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <set>
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
detail::DeviceRole copiedDeviceRole(Store& store,const DeviceCopy& copy) {
    std::string subtype;
    const auto& reference=copy.insert.at("ref");
    if(!reference.is_null()) {
        if(!copy.reference || reference!=copy.reference->id)throw std::runtime_error("copied plug-in identity is not captured");
        SQLite::Statement row(store.db(),"SELECT format,uid,shell_id,subtype FROM plugin_refs WHERE id=?");row.bind(1,copy.reference->id);
        if(!row.executeStep())throw std::runtime_error("copied plug-in reference is missing");
        std::optional<std::int64_t> shell;if(!row.getColumn(2).isNull())shell=row.getColumn(2).getInt64();
        if(row.getColumn(0).getString()!=copy.reference->format || row.getColumn(1).getString()!=copy.reference->uid || shell!=copy.reference->shellId)
            throw std::runtime_error("plug-in reference identity changed after copy");
        subtype = row.getColumn(3).getString();
    }
    if (subtype.empty())
        if (const auto rack = copy.insert.find("rackKind"); rack != copy.insert.end() && rack->is_string())
            subtype = rack->get<std::string>();
    if(copy.insert.contains("remoteHost") && !copy.insert.at("remoteHost").is_null()) {
        if(!copy.remoteHost || copy.insert.at("remoteHost")!=copy.remoteHost->id)
            throw std::runtime_error("copied remote host identity is not captured");
        if(remoteReference(store.db(),copy.remoteHost->id)!=*copy.remoteHost)
            throw std::runtime_error("remote host identity changed after copy");
    }
    return detail::deviceRole(subtype);
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
        // A removal undo restores the same identity and causal dependants.
        // A clipboard clone has a NEW identity and copies only the authored
        // device controls/state/panel/expression route below, never old keys,
        // automation or the identity-bound removal image.
        result.insert.erase("contents");
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
std::size_t pastePosition(std::span<const detail::ChainDevice> devices,
                          std::span<const detail::DeviceRole> roles, std::size_t requested) {
    using Role = detail::DeviceRole;
    const auto instruments = std::count(roles.begin(), roles.end(), Role::Instrument);
    if (instruments > 1)
        throw std::runtime_error("Only one instrument can be pasted into a chain.");
    if (instruments == 1 && std::any_of(devices.begin(), devices.end(), [](const auto &device) {
            return device.role == Role::Instrument;
        }))
        throw std::runtime_error("The destination chain already has an instrument.");
    if (!std::is_sorted(roles.begin(), roles.end()))
        throw std::runtime_error("The selected devices cannot be pasted together in their current order.");

    // The last role sets the earliest position and the first role sets the latest.
    const auto first = detail::deviceInsertPosition(devices, roles.back(), 0);
    const auto last = detail::deviceInsertPosition(devices, roles.front(), devices.size());
    if (first > last)
        throw std::runtime_error("There is no position for all selected devices in this chain.");
    return std::clamp(requested, first, last);
}

void requireNativeFunctionsAvailable(SQLite::Database &db, std::int64_t track,
                                     const std::set<std::string> &nativeUids) {
    if (nativeUids.empty())
        return;
    SQLite::Statement exists(db,
        "WITH RECURSIVE chains(id) AS (SELECT id FROM device_chains WHERE track_id=? "
        "UNION SELECT c.id FROM device_chains c JOIN devices owner ON owner.id=c.parent_device_id "
        "JOIN chains parent ON parent.id=owner.chain_id) "
        "SELECT 1 FROM devices d JOIN chains c ON c.id=d.chain_id JOIN plugin_refs p ON p.id=d.plugin_ref_id "
        "WHERE p.format='internal' AND p.uid=? LIMIT 1");
    for (const auto &uid : nativeUids) {
        exists.bind(1, track);
        exists.bind(2, uid);
        if (exists.executeStep())
            throw std::runtime_error("That function is already present on this channel.");
        exists.reset();
    }
}

PreparedDevicePaste preparePaste(Store &store, std::span<const DeviceCopy> copies,
                                 std::span<const std::int64_t> newIds,
                                 std::int64_t chain, std::optional<std::int64_t> before,
                                 std::optional<std::int64_t> track) {
    PreparedDevicePaste result;
    try {
        if (audio::onAudioThread)
            throw std::runtime_error("device paste belongs off the audio callback");
        if (store.readOnly())
            throw std::runtime_error("project is read-only");
        if (copies.empty() || copies.size() != newIds.size())
            throw std::runtime_error("device paste needs one new identity per copied device");
        auto &db = store.db();
        Read read(db);
        result.sourceFile = identity(store);
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        const auto destinationTrack = track ? *track : detail::deviceChainOwner(db, chain);

        OpTransactionInputs guards;
        const auto capture = [&](const char *table, std::int64_t id) {
            guards.rowGuards.push_back({table, {{"id", id}}, detail::captureRow(db, table, "id", id)});
        };
        const char *label = copies.size() == 1 ? "Paste Device" : "Paste Devices";
        const auto append = [&](const char *type, Payload payload, const char *kind, std::int64_t id) {
            OpRequest request;
            request.opType = type;
            request.payload = std::move(payload);
            request.label = label;
            request.targetKind = kind;
            request.targetId = id;
            result.requests.push_back(std::move(request));
        };
        capture("tracks", destinationTrack);

        std::vector<detail::DeviceRole> roles;
        roles.reserve(copies.size());
        std::set<std::int64_t> oldIds, seenIds, references;
        std::set<std::string> nativeUids;
        for (const auto &copy : copies) {
            if (!copy.ok)
                throw std::runtime_error(copy.error);
            if (copy.hasNestedChains)
                throw std::runtime_error("rack paste needs a nested chain image");
            if (copy.sourceFile != result.sourceFile)
                throw std::runtime_error("device clipboard belongs to a different project");
            const auto role = copiedDeviceRole(store, copy);
            detail::requireDeviceInsertTrack(db, destinationTrack, role, copy.insert.value("name", std::string("Device")));
            roles.push_back(role);
            oldIds.insert(detail::signedInteger(copy.insert.at("id")));
            if (copy.reference) {
                references.insert(copy.reference->id);
                if (mix::functionForReference(copy.reference->format, copy.reference->uid) &&
                    !nativeUids.insert(copy.reference->uid).second)
                    throw std::runtime_error("A channel can hold only one copy of each native mix function.");
            }
        }
        requireNativeFunctionsAvailable(db, destinationTrack, nativeUids);
        SQLite::Statement occupied(db, "SELECT 1 FROM devices WHERE id=?");
        for (const auto newId : newIds) {
            if (oldIds.contains(newId) || !seenIds.insert(newId).second)
                throw std::runtime_error("device copy needs a new identity");
            occupied.bind(1, newId);
            if (occupied.executeStep())
                throw std::runtime_error("new device id is occupied");
            occupied.reset();
            guards.rowGuards.push_back({"devices", {{"id", newId}}, std::nullopt});
        }

        bool createChain = false;
        if (track) {
            const Payload selector = {{"track_id", *track}, {"parent_device_id", nullptr}};
            guards.rowSetGuards.push_back(
                {"device_chains", selector, detail::captureGuardRows(store, "device_chains", selector)});
            SQLite::Statement target(db, "SELECT id FROM device_chains WHERE track_id=? AND "
                                         "parent_device_id IS NULL ORDER BY ord,id LIMIT 1");
            target.bind(1, *track);
            if (target.executeStep())
                chain = target.getColumn(0).getInt64();
            else {
                // Same bounded MAX strategy as native built-in insertion; the
                // shared-allocator consolidation remains the M2 follow-up.
                SQLite::Statement last(db, "SELECT coalesce(MAX(id),0) FROM device_chains");
                last.executeStep();
                const auto id = std::max<std::int64_t>(0, last.getColumn(0).getInt64());
                if (id == std::numeric_limits<std::int64_t>::max())
                    throw std::runtime_error("device chain identities are exhausted");
                chain = id + 1;
                createChain = true;
            }
        }
        capture("device_chains", chain);
        const Payload devices = {{"chain_id", chain}};
        auto destination = detail::captureGuardRows(store, "devices", devices);
        for (const auto &row : destination)
            if (!row.at("plugin_ref_id").is_null())
                references.insert(detail::signedInteger(row.at("plugin_ref_id")));
        guards.rowSetGuards.push_back({"devices", devices, std::move(destination)});
        for (const auto id : references)
            capture("plugin_refs", id);

        result.chainId = chain;
        const auto order = detail::deviceChainOrder(db, chain);
        auto requested = order.size();
        if (before) {
            const auto found = std::find_if(order.begin(), order.end(), [&](const auto &item) { return item.id == *before; });
            if (found == order.end())
                throw std::runtime_error("selected device is not in the destination chain");
            requested = static_cast<std::size_t>(found - order.begin());
        }
        if (copies.size() > std::numeric_limits<std::size_t>::max() - order.size() ||
            !std::in_range<std::int64_t>(order.size() + copies.size()))
            throw std::runtime_error("device chain order is exhausted");
        const auto position = pastePosition(order, roles, requested);
        if (createChain)
            append("chain.create", {{"id", chain}, {"track", destinationTrack}, {"ord", 0}, {"name", "Devices"}},
                   "chain", chain);
        // Canonical integer order also handles legacy ties and ord=INT64_MAX;
        // every original value is restored by the moves' captured inverses.
        for (std::size_t i = 0; i < order.size(); ++i) {
            const auto index = static_cast<std::int64_t>(i + (i >= position ? copies.size() : 0));
            if (order[i].ord != index)
                append("device.move", {{"id", order[i].id}, {"chain", chain}, {"ord", index}}, "device", order[i].id);
        }
        for (std::size_t i = 0; i < copies.size(); ++i) {
            auto insert = copies[i].insert;
            insert["id"] = newIds[i];
            insert["chain"] = chain;
            insert["ord"] = static_cast<std::int64_t>(position + i);
            append("device.insert", std::move(insert), "device", newIds[i]);
        }
        const auto packet = CapturedTransactionAccess::seal(appliedSource(store), *result.options.expectHead,
                                                            result.requests, std::move(guards));
        for (auto &request : result.requests)
            request.capturedTransaction = packet;
        result.ok = true;
    } catch (const std::exception &error) {
        result.error = error.what();
    }
    return result;
}
} // namespace
PreparedDevicePaste prepareDevicePaste(Store &store, const DeviceCopy &copy, std::int64_t newId,
                                       std::int64_t chain, std::optional<std::int64_t> before) {
    return prepareDevicePaste(store, std::span(&copy, 1), std::span(&newId, 1), chain, before);
}
PreparedDevicePaste prepareDevicePaste(Store &store, std::span<const DeviceCopy> copies,
                                       std::span<const std::int64_t> newIds, std::int64_t chain,
                                       std::optional<std::int64_t> before) {
    return preparePaste(store, copies, newIds, chain, before, std::nullopt);
}
PreparedDevicePaste prepareDevicePasteForTrack(Store &store, const DeviceCopy &copy,
                                               std::int64_t newId, std::int64_t track) {
    return prepareDevicePasteForTrack(store, std::span(&copy, 1), std::span(&newId, 1), track);
}
PreparedDevicePaste prepareDevicePasteForTrack(Store &store, std::span<const DeviceCopy> copies,
                                               std::span<const std::int64_t> newIds, std::int64_t track) {
    return preparePaste(store, copies, newIds, 0, std::nullopt, track);
}
CommitResult commitDevicePaste(Store& store,const PreparedDevicePaste& prepared,Actor actor) {
    CommitResult result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(prepared.sourceFile!=identity(store)){result.error="project changed after device paste preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}
}
