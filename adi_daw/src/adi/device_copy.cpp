// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_copy.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/mix_functions.hpp"
#include "adi/op_transaction_resources_detail.hpp"
#include "adi/row_capture.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"
#include "adi/track_image.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <limits>
#include <map>
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
rows::PluginRef pluginReference(SQLite::Database& db, std::int64_t id) {
    SQLite::Statement reference(db,"SELECT id,format,uid,vendor,name,version,subtype,path_hint,is_shell,shell_id FROM plugin_refs WHERE id=?");
    reference.bind(1,id);
    if (!reference.executeStep()) throw std::runtime_error("copied plug-in reference is missing");
    rows::PluginRef value;
    value.id=reference.getColumn(0).getInt64();value.format=reference.getColumn(1).getString();
    value.uid=reference.getColumn(2).getString();value.vendor=reference.getColumn(3).getString();
    value.name=reference.getColumn(4).getString();value.version=reference.getColumn(5).getString();
    value.subtype=reference.getColumn(6).getString();value.pathHint=reference.getColumn(7).getString();
    value.isShell=reference.getColumn(8).getInt()!=0;
    if (!reference.getColumn(9).isNull()) value.shellId=reference.getColumn(9).getInt64();
    return value;
}

std::int64_t integer(const Payload& row, const char* field) {
    return detail::signedInteger(row.at(field));
}

void verifyPluginReference(Store& store, const rows::PluginRef& expected, OpTransactionInputs& guards) {
    const auto row=detail::captureRow(store.db(),"plugin_refs","id",expected.id);
    if (!row) throw std::runtime_error("copied plug-in reference is missing");
    const auto& shell=row->at("shell_id");
    if (row->at("format")!=expected.format || row->at("uid")!=expected.uid ||
        (expected.shellId ? shell!=*expected.shellId : !shell.is_null()))
        throw std::runtime_error("plug-in reference identity changed after copy");
    guards.rowGuards.push_back({"plugin_refs",{{"id",expected.id}},row});
}

bool hasId(const std::set<std::int64_t>& ids, const Payload& row, const char* field) {
    return !row.at(field).is_null() && ids.contains(integer(row,field));
}

const std::set<std::string> deviceChildren{
    "device_expression_routes", "device_panel_params", "device_panels", "plugin_params", "plugin_state"};

Payload rackFragment(Payload image) {
    std::set<std::int64_t> devices, macros, routes;
    for (const auto& entry : image.at("rows")) {
        if (entry.at("table")=="devices" && entry.at("after").is_null())
            devices.insert(integer(entry.at("before"),"id"));
    }
    for (const auto& entry : image.at("rows")) {
        if (!entry.at("after").is_null()) continue;
        const auto& row=entry.at("before");
        if (entry.at("table")=="macros" && hasId(devices,row,"device_id"))
            macros.insert(integer(row,"id"));
        if (entry.at("table")=="routing" && row.at("src_kind")=="device" && row.at("dst_kind")=="device" &&
            hasId(devices,row,"src_id") && hasId(devices,row,"dst_id"))
            routes.insert(integer(row,"id"));
    }
    auto& rows=image.at("rows");
    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const Payload& entry) {
        if (!entry.at("after").is_null()) return true;
        const auto& name=entry.at("table").get_ref<const std::string&>();
        const auto& row=entry.at("before");
        if (name=="devices") return false;
        if (name=="device_chains") return !hasId(devices,row,"parent_device_id");
        if (name=="macros") return !macros.contains(integer(row,"id"));
        if (name=="macro_mappings") return !hasId(macros,row,"macro_id") || !hasId(devices,row,"target_device_id");
        if (name=="routing") return !routes.contains(integer(row,"id"));
        if (name=="routing_key_taps" || name=="routing_send_taps") return !hasId(routes,row,"routing_id");
        return !deviceChildren.contains(name);
    }),rows.end());
    return image;
}

Payload rackRootInsert(const Payload& row) {
    return {{"id",row.at("id")},{"chain",row.at("chain_id")},{"ord",row.at("ord")},
            {"ref",row.at("plugin_ref_id")},{"name",row.at("name")},{"enabled",row.at("enabled")!=0},
            {"rack",row.at("is_rack")!=0},{"preset",row.at("preset_name")},{"latency",row.at("latency_samples")},
            {"always",row.at("always_process")!=0},{"missing",row.at("missing")!=0},
            {"rackKind",row.at("rack_kind")},{"remoteHost",row.at("remote_host_id")}};
}

const std::set<std::string> identityTables{"devices", "device_chains", "macros", "macro_mappings", "routing"};
using IdMap = std::map<std::int64_t,std::int64_t>;
using FragmentIds = std::map<std::string,IdMap>;

FragmentIds originalIds(const Payload& contents) {
    FragmentIds ids;
    if (contents.is_null()) return ids;
    for (const auto& entry : contents.at("rows")) {
        const auto& table=entry.at("table").get_ref<const std::string&>();
        if (!identityTables.contains(table)) continue;
        const auto id=integer(entry.at("before"),"id");
        if (!ids[table].emplace(id,0).second) throw std::runtime_error("duplicate rack clipboard identity");
    }
    return ids;
}

std::int64_t nextFragmentId(RowIds& allocator, const std::string& table, const IdMap& origins,
                            const std::set<std::int64_t>& reserved = {}) {
    std::int64_t id;
    do {
        id = allocator.next(table);
    } while (origins.contains(id) || reserved.contains(id));
    return id;
}

void guardNewDeviceRemark(Store& store, std::int64_t id, OpTransactionInputs& guards) {
    const Payload selector = {{"target_kind", "device"}, {"target_id", id}};
    auto rows = detail::captureGuardRows(store, "remarks", selector);
    if (!rows.empty()) throw std::runtime_error("new rack identity is held by a saved remark");
    guards.rowSetGuards.push_back({"remarks", selector, std::move(rows)});
}

void rebind(Payload& entry, const char* field, std::int64_t value) {
    entry.at("before")[field]=value;
    if (entry.at("key").contains(field)) entry.at("key")[field]=value;
}

// reservedDevices: every root identity of the paste, so a fragment of one rack
// never takes another pasted root's ID.
Payload remapRack(Store& store, RowIds& allocator, const Payload& contents, FragmentIds ids, std::int64_t origin,
                  std::int64_t device, std::int64_t chain, std::int64_t order,
                  const std::set<std::int64_t>& reservedDevices, OpTransactionInputs& guards) {
    if (integer(contents,"root")!=origin || contents.at("version")!=1 ||
        !ids.at("devices").contains(origin))
        throw std::runtime_error("rack image does not describe the copied root");
    const std::set<std::int64_t> reservedChains{chain};
    const std::set<std::int64_t> none;
    for (auto& [table, map] : ids) {
        for (auto& [old,fresh] : map) {
            if (table=="devices" && old==origin) { fresh=device; continue; }
            const auto& reserved=table=="devices" ? reservedDevices :
                                 table=="device_chains" ? reservedChains : none;
            fresh=nextFragmentId(allocator,table,map,reserved);
            guards.rowGuards.push_back({table,{{"id",fresh}},std::nullopt});
            if (table == "devices") guardNewDeviceRemark(store, fresh, guards);
        }
    }

    auto rows=contents.at("rows").get<std::vector<Payload>>();
    for (auto& entry : rows) {
        const auto& table=entry.at("table").get_ref<const std::string&>();
        if (!entry.at("after").is_null()) throw std::runtime_error("rack clipboard contains a surviving row");
        const auto bind=[&](const char* foreign, const char* field) {
            rebind(entry,field,ids.at(foreign).at(integer(entry.at("before"),field)));
        };
        if (table=="devices") {
            if (integer(entry.at("before"),"id")==origin) {
                rebind(entry,"chain_id",chain);
                rebind(entry,"ord",order);
            } else bind("device_chains","chain_id");
        } else if (table=="device_chains") bind("devices","parent_device_id");
        else if (table=="macros" || deviceChildren.contains(table)) bind("devices","device_id");
        else if (table=="macro_mappings") {
            bind("macros","macro_id");
            bind("devices","target_device_id");
        } else if (table=="routing") {
            if (entry.at("before").at("src_kind")!="device" || entry.at("before").at("dst_kind")!="device")
                throw std::runtime_error("rack clipboard contains external routing");
            bind("devices","src_id");
            bind("devices","dst_id");
        } else if (table=="routing_key_taps" || table=="routing_send_taps") bind("routing","routing_id");
        else throw std::runtime_error("unsupported rack clipboard table");
        if (identityTables.contains(table)) rebind(entry,"id",ids.at(table).at(integer(entry.at("before"),"id")));
    }
    std::sort(rows.begin(),rows.end(),[](const Payload& a,const Payload& b) {
        const auto& left=a.at("table").get_ref<const std::string&>();
        const auto& right=b.at("table").get_ref<const std::string&>();
        if (left!=right) return left<right;
        return a.at("key").dump()<b.at("key").dump();
    });
    return {{"root",device},{"version",1},{"rows",std::move(rows)}};
}

void captureFragmentReferences(Store& store, DeviceCopy& copy) {
    for (const auto& entry : copy.rackContents.at("rows")) {
        if (entry.at("table")!="devices") continue;
        const auto& row=entry.at("before");
        if (!row.at("plugin_ref_id").is_null()) {
            const auto id=integer(row,"plugin_ref_id");
            if (!copy.rackReferences.contains(id))
                copy.rackReferences.emplace(id,pluginReference(store.db(),id));
        }
        if (!row.at("remote_host_id").is_null()) {
            const auto id=integer(row,"remote_host_id");
            if (!copy.rackRemoteHosts.contains(id))
                copy.rackRemoteHosts.emplace(id,remoteReference(store.db(),id));
        }
    }
}

// DF-E55: the role a pasted device plays on its destination track (an
// instrument or a MIDI effect needs a MIDI track). A rack plays its kind's.
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

void verifyReferences(Store& store, const DeviceCopy& copy, OpTransactionInputs& guards) {
    const auto verifyRemote = [&](const DeviceRemoteReference& expected) {
        if (remoteReference(store.db(),expected.id)!=expected)
            throw std::runtime_error("remote host identity changed after copy");
    };
    if (copy.rackContents.is_null()) {
        const auto& reference=copy.insert.at("ref");
        if (!reference.is_null()) {
            if (!copy.reference || integer(copy.insert,"ref")!=copy.reference->id)
                throw std::runtime_error("copied plug-in identity is not captured");
            verifyPluginReference(store,*copy.reference,guards);
        }
        if (copy.insert.contains("remoteHost") && !copy.insert.at("remoteHost").is_null()) {
            if (!copy.remoteHost || integer(copy.insert,"remoteHost")!=copy.remoteHost->id)
                throw std::runtime_error("copied remote host identity is not captured");
            verifyRemote(*copy.remoteHost);
        }
        return;
    }
    std::set<std::int64_t> plugins, remotes;
    for (const auto& entry : copy.rackContents.at("rows")) {
        if (entry.at("table")!="devices") continue;
        const auto& row=entry.at("before");
        if (!row.at("plugin_ref_id").is_null()) plugins.insert(integer(row,"plugin_ref_id"));
        if (!row.at("remote_host_id").is_null()) remotes.insert(integer(row,"remote_host_id"));
    }
    for (const auto id : plugins) {
        const auto entry=copy.rackReferences.find(id);
        if (entry==copy.rackReferences.end() || entry->second.id!=id)
            throw std::runtime_error("copied rack plug-in identity is not captured");
        verifyPluginReference(store,entry->second,guards);
    }
    for (const auto id : remotes) {
        const auto entry=copy.rackRemoteHosts.find(id);
        if (entry==copy.rackRemoteHosts.end() || entry->second.id!=id)
            throw std::runtime_error("copied rack remote host identity is not captured");
        verifyRemote(entry->second);
        guards.rowGuards.push_back(
            {"remote_hosts",{{"id",id}},detail::captureRow(store.db(),"remote_hosts","id",id)});
    }
}

std::int64_t rackDestinationTrack(Store& store, std::int64_t chain, OpTransactionInputs& guards) {
    std::set<std::int64_t> seen;
    while (seen.insert(chain).second) {
        const auto row=detail::captureRow(store.db(),"device_chains","id",chain);
        if (!row) throw std::runtime_error("destination chain is missing");
        guards.rowGuards.push_back({"device_chains",{{"id",chain}},row});
        if (!row->at("track_id").is_null()) return integer(*row,"track_id");

        const auto device=integer(*row,"parent_device_id");
        const auto parent=detail::captureRow(store.db(),"devices","id",device);
        if (!parent) throw std::runtime_error("destination rack owner is missing");
        if (parent->at("is_rack") == 0) throw std::runtime_error("destination chain owner is not a rack");
        guards.rowGuards.push_back({"devices",{{"id",device}},parent});
        chain=integer(*parent,"chain_id");
    }
    throw std::runtime_error("destination rack contains a cycle");
}

void guardRackMixFunctions(Store& store, const DeviceCopy& copy, std::int64_t chain,
                           std::optional<std::int64_t> track, OpTransactionInputs& guards) {
    std::set<mix::Function> copied;
    for (const auto& entry : copy.rackContents.at("rows")) {
        if (entry.at("table")!="devices") continue;
        const auto& row=entry.at("before");
        if (row.at("plugin_ref_id").is_null()) continue;
        const auto& ref=copy.rackReferences.at(integer(row,"plugin_ref_id"));
        if (const auto* function=mix::functionForReference(ref.format,ref.uid))
            if (!copied.insert(function->kind).second)
                throw std::runtime_error("a rack copy contains more than one of the same Mix View function");
    }
    const auto owner=track ? *track : rackDestinationTrack(store,chain,guards);
    if (copied.empty()) return;
    const auto captureSet=[&](const char* table, const Payload& selector) {
        auto rows=detail::captureGuardRows(store,table,selector);
        guards.rowSetGuards.push_back({table,selector,rows});
        return rows;
    };
    std::vector<std::int64_t> chains;
    for (const auto& row : captureSet("device_chains",{{"track_id",owner},{"parent_device_id",nullptr}}))
        chains.push_back(integer(row,"id"));
    std::set<std::int64_t> seen;
    std::set<std::int64_t> refs;
    for (std::size_t i=0;i<chains.size();++i) {
        if (!seen.insert(chains[i]).second) throw std::runtime_error("destination rack contains a cycle");
        for (const auto& row : captureSet("devices",{{"chain_id",chains[i]}})) {
            for (const auto& child : captureSet("device_chains",{{"parent_device_id",integer(row,"id")}}))
                chains.push_back(integer(child,"id"));
            if (row.at("plugin_ref_id").is_null()) continue;
            const auto id=integer(row,"plugin_ref_id");
            if (!refs.insert(id).second) continue;
            const auto ref=detail::captureRow(store.db(),"plugin_refs","id",id);
            if (!ref) throw std::runtime_error("destination plug-in reference is missing");
            const auto* function=mix::functionForReference(ref->at("format").get_ref<const std::string&>(),
                                                          ref->at("uid").get_ref<const std::string&>());
            if (function && copied.contains(function->kind))
                throw std::runtime_error("that Mix View function is already present on the destination channel");
            guards.rowGuards.push_back({"plugin_refs",{{"id",id}},ref});
        }
    }
}

const Payload& rackRoot(const Payload& image, std::int64_t id) {
    for (const auto& entry : image.at("rows"))
        if (entry.at("table")=="devices" && integer(entry.at("before"),"id")==id)
            return entry.at("before");
    throw std::runtime_error("rack clipboard is missing its root");
}

}
bool DeviceCopy::hasOriginalDeviceId(std::int64_t id) const {
    if (insert.contains("id") && insert.at("id")==id) return true;
    if (rackContents.is_null()) return false;
    const auto& rows=rackContents.at("rows");
    return std::any_of(rows.begin(),rows.end(),[id](const Payload& row) {
        return row.at("table")=="devices" && row.at("before").at("id")==id;
    });
}

DeviceCopy readDeviceCopy(Store& store,std::int64_t id) {
    DeviceCopy result;
    try {
        if (audio::onAudioThread) throw std::runtime_error("device copy belongs off the audio callback");
        Read read(store.db());
        result.sourceFile=identity(store);
        const auto root=detail::captureRow(store.db(),"devices","id",id);
        if (!root) throw std::runtime_error("no such device to capture");
        SQLite::Statement nested(store.db(),"SELECT 1 FROM device_chains WHERE parent_device_id=? LIMIT 1");
        nested.bind(1,id);
        result.hasNestedChains=nested.executeStep();
        if (result.hasNestedChains || root->at("is_rack")!=0) {
            result.rackContents=rackFragment(detail::captureDevice(store.db(),id));
            result.insert=rackRootInsert(*root);
            captureFragmentReferences(store,result);
        } else {
            const auto* descriptor=OpRegistry::instance().find("device.remove");
            OpContext context{store,store.db()};
            if (!descriptor || !descriptor->buildInverse ||
                !descriptor->buildInverse(context,{{"id",id}},result.insert,result.error)) return result;
            // A flat clone keeps the ordinary device payload and drops the
            // same-identity undo image and its external causal dependants.
            result.insert.erase("contents");
        }
        if (!result.insert.at("ref").is_null())
            result.reference=pluginReference(store.db(),integer(result.insert,"ref"));
        if (!result.insert.at("remoteHost").is_null())
            result.remoteHost=remoteReference(store.db(),integer(result.insert,"remoteHost"));
        result.ok=true;
    } catch (const std::exception& error) {result.error=error.what();}
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

// requestedIds empty: allocate each root's identity together with the copies'
// rack fragments, skipping every original ID and saved remark (DEV-06).
PreparedDevicePaste preparePaste(Store &store, std::span<const DeviceCopy> copies,
                                 std::span<const std::int64_t> requestedIds,
                                 std::int64_t chain, std::optional<std::int64_t> before,
                                 std::optional<std::int64_t> track) {
    PreparedDevicePaste result;
    try {
        if (audio::onAudioThread)
            throw std::runtime_error("device paste belongs off the audio callback");
        if (store.readOnly())
            throw std::runtime_error("project is read-only");
        if (copies.empty() || (!requestedIds.empty() && copies.size() != requestedIds.size()))
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
        std::set<std::int64_t> references;
        std::set<std::string> nativeUids;
        const auto addNative = [&](const rows::PluginRef& ref) {
            if (mix::functionForReference(ref.format, ref.uid) && !nativeUids.insert(ref.uid).second)
                throw std::runtime_error("A channel can hold only one copy of each native mix function.");
        };
        for (const auto &copy : copies) {
            if (!copy.ok)
                throw std::runtime_error(copy.error);
            if (copy.hasNestedChains && copy.rackContents.is_null())
                throw std::runtime_error("rack clipboard is missing its nested chain image");
            if (copy.sourceFile != result.sourceFile)
                throw std::runtime_error("device clipboard belongs to a different project");
            verifyReferences(store, copy, guards);
            const auto role = copiedDeviceRole(store, copy);
            detail::requireDeviceInsertTrack(db, destinationTrack, role, copy.insert.value("name", std::string("Device")));
            roles.push_back(role);
            if (copy.reference) addNative(*copy.reference);
            for (const auto& [id, ref] : copy.rackReferences) {
                (void)id;
                addNative(ref);
            }
        }
        requireNativeFunctionsAvailable(db, destinationTrack, nativeUids);
        // verifyReferences guarded the copied references; the destination's are added below.
        std::set<std::int64_t> guardedReferences;
        for (const auto& guard : guards.rowGuards)
            if (guard.table == "plugin_refs") guardedReferences.insert(detail::signedInteger(guard.primaryKey.at("id")));

        RowIds allocator(db);
        const auto originalId = [&](std::int64_t id) {
            return std::any_of(copies.begin(), copies.end(),
                               [id](const DeviceCopy& copy) { return copy.hasOriginalDeviceId(id); });
        };
        std::vector<std::int64_t> newIds;
        newIds.reserve(copies.size());
        std::set<std::int64_t> seenIds;
        for (std::size_t i = 0; i < copies.size(); ++i) {
            auto newId = std::int64_t{0};
            if (!requestedIds.empty()) {
                newId = requestedIds[i];
            } else {
                do newId = allocator.next("devices");
                while (originalId(newId) || seenIds.contains(newId));
            }
            if (originalId(newId) || !seenIds.insert(newId).second)
                throw std::runtime_error("device copy needs a new identity");
            if (detail::captureRow(db, "devices", "id", newId))
                throw std::runtime_error("new device id is occupied");
            guards.rowGuards.push_back({"devices", {{"id", newId}}, std::nullopt});
            if (!copies[i].rackContents.is_null()) guardNewDeviceRemark(store, newId, guards);
            newIds.push_back(newId);
        }

        bool createChain = false;
        if (track) {
            const Payload selector = {{"track_id", *track}, {"parent_device_id", nullptr}};
            guards.rowSetGuards.push_back(
                {"device_chains", selector, detail::captureGuardRows(store, "device_chains", selector)});
            SQLite::Statement target(db, "SELECT id FROM device_chains WHERE track_id=? AND "
                                         "parent_device_id IS NULL ORDER BY ord,id LIMIT 1");
            target.bind(1, *track);
            if (target.executeStep()) {
                chain = target.getColumn(0).getInt64();
            } else {
                // DEV-06: one allocator for the chain and the copies' rack
                // fragments, skipping every identity a fragment held.
                std::set<std::int64_t> fragmentChains;
                for (const auto& copy : copies) {
                    const auto origins = originalIds(copy.rackContents);
                    if (const auto old = origins.find("device_chains"); old != origins.end())
                        for (const auto& [id, fresh] : old->second) {
                            (void)fresh;
                            fragmentChains.insert(id);
                        }
                }
                do chain = allocator.next("device_chains");
                while (fragmentChains.contains(chain));
                createChain = true;
            }
        }
        const auto destinationChain = detail::captureRow(db, "device_chains", "id", chain);
        if (!destinationChain && !createChain) throw std::runtime_error("destination chain is missing");
        guards.rowGuards.push_back({"device_chains", {{"id", chain}}, destinationChain});
        const Payload devices = {{"chain_id", chain}};
        auto destination = detail::captureGuardRows(store, "devices", devices);
        for (const auto &row : destination)
            if (!row.at("plugin_ref_id").is_null())
                references.insert(detail::signedInteger(row.at("plugin_ref_id")));
        guards.rowSetGuards.push_back({"devices", devices, std::move(destination)});
        for (const auto id : references)
            if (!guardedReferences.contains(id)) capture("plugin_refs", id);

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
        const std::set<std::int64_t> reservedDevices(newIds.begin(), newIds.end());
        for (std::size_t i = 0; i < copies.size(); ++i) {
            const auto& copy = copies[i];
            const auto ord = static_cast<std::int64_t>(position + i);
            auto insert = copy.insert;
            if (!copy.rackContents.is_null()) {
                // DEV-06: a rack's owned rows come along under fresh identities.
                guardRackMixFunctions(store, copy, chain, track, guards);
                auto mapped = remapRack(store, allocator, copy.rackContents, originalIds(copy.rackContents),
                                        integer(copy.insert, "id"), newIds[i], chain, ord, reservedDevices, guards);
                insert = rackRootInsert(rackRoot(mapped, newIds[i]));
                insert["contents"] = std::move(mapped);
            }
            insert["id"] = newIds[i];
            insert["chain"] = chain;
            insert["ord"] = ord;
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
PreparedDevicePaste prepareDevicePasteWithFreshId(Store& store, const DeviceCopy& copy, std::int64_t chain,
                                                  std::optional<std::int64_t> before) {
    return preparePaste(store, std::span(&copy, 1), {}, chain, before, std::nullopt);
}
PreparedDevicePaste prepareDevicePasteForTrackWithFreshId(Store& store, const DeviceCopy& copy, std::int64_t track) {
    return preparePaste(store, std::span(&copy, 1), {}, 0, std::nullopt, track);
}
CommitResult commitDevicePaste(Store& store,const PreparedDevicePaste& prepared,Actor actor) {
    CommitResult result;
    if(!prepared.ok){result.error=prepared.error;return result;}
    if(prepared.sourceFile!=identity(store)){result.error="project changed after device paste preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}

namespace {
std::vector<Payload> captureRows(Store& store, OpTransactionInputs& guards, const char* table, const Payload& selector) {
    auto rows = detail::captureGuardRows(store, table, selector);
    guards.rowSetGuards.push_back({table, selector, rows});
    return rows;
}

std::vector<Payload> orderedDevices(Store& store, OpTransactionInputs& guards, std::int64_t chain) {
    auto rows = captureRows(store, guards, "devices", {{"chain_id", chain}});
    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        const auto ordering = [](const auto& row) { return std::pair{integer(row, "ord"), integer(row, "id")}; };
        return ordering(a) < ordering(b);
    });
    if (!std::in_range<std::int64_t>(rows.size())) throw std::runtime_error("device chain order is exhausted");
    return rows;
}

void appendRackEdit(std::vector<OpRequest>& requests, std::string_view label, const char* type,
                    const char* kind, std::int64_t id, Payload fields) {
    auto& request = requests.emplace_back();
    request.opType = type;
    request.label = label;
    request.targetKind = kind;
    request.targetId = id;
    fields["id"] = id;
    request.payload = std::move(fields);
}

void moveRackDevice(std::vector<OpRequest>& requests, std::string_view label, const Payload& row,
                    std::int64_t chain, std::int64_t order) {
    if (integer(row, "chain_id") == chain && integer(row, "ord") == order) return;
    appendRackEdit(requests, label, "device.move", "device", integer(row, "id"), {{"chain", chain}, {"ord", order}});
}

template<class Prepare>
CommitResult editRack(Store& store, Actor actor, const Prepare& prepare) {
    try {
        if (audio::onAudioThread || store.readOnly()) throw std::runtime_error("rack editing requires a writable owner connection");
        std::vector<OpRequest> requests;
        {
            Read read(store.db());
            const auto head = OpJournal(store).headSeq();
            OpTransactionInputs guards;
            prepare(requests, guards);
            const auto packet = CapturedTransactionAccess::seal(appliedSource(store), head, requests, std::move(guards));
            for (auto& request : requests) {
                request.actor = actor;
                request.capturedTransaction = packet;
            }
        }
        return OpJournal(store).commit(requests);
    } catch (const std::exception& error) {
        CommitResult result;
        result.error = error.what();
        return result;
    }
}
}

CommitResult groupDevicesInRack(Store& store, std::span<const std::int64_t> deviceIds, std::string_view rackKind, Actor actor) {
    return editRack(store, actor, [&](std::vector<OpRequest>& requests, OpTransactionInputs& guards) {
        constexpr std::string_view label = "Group Devices";
        if (rackKind != "audio_effect" && rackKind != "instrument" && rackKind != "midi_effect" && rackKind != "drum")
            throw std::runtime_error("choose an audio effect, MIDI effect, instrument or drum rack");
        const std::set selected(deviceIds.begin(), deviceIds.end());
        if (selected.empty() || selected.size() != deviceIds.size()) throw std::runtime_error("select distinct devices to group");
        const auto first = detail::captureRow(store.db(), "devices", "id", *selected.begin());
        if (!first) throw std::runtime_error("a selected device is missing");
        const auto chain = integer(*first, "chain_id");
        (void)rackDestinationTrack(store, chain, guards);
        const auto order = orderedDevices(store, guards, chain);
        const auto selectedRow = [&](const auto& row) { return selected.contains(integer(row, "id")); };
        const auto begin = std::ranges::find_if(order, selectedRow);
        const auto end = std::find_if_not(begin, order.end(), selectedRow);
        if (static_cast<std::size_t>(end - begin) != selected.size())
            throw std::runtime_error("grouping requires one contiguous serial selection");

        RowIds ids(store.db());
        const auto rack = ids.next("devices");
        const auto child = ids.next("device_chains");
        guards.rowGuards.push_back({"devices", {{"id", rack}}, std::nullopt});
        guardNewDeviceRemark(store, rack, guards);
        guards.rowGuards.push_back({"device_chains", {{"id", child}}, std::nullopt});
        const auto at = static_cast<std::int64_t>(begin - order.begin());
        appendRackEdit(requests, label, "device.insert", "device", rack,
                       {{"chain", chain}, {"ord", at}, {"name", "Rack"}, {"rack", true}, {"rackKind", rackKind}});
        appendRackEdit(requests, label, "chain.create", "chain", child, {{"device", rack}, {"name", "Chain"}});
        std::int64_t childOrder = 0;
        for (auto it = begin; it != end; ++it) moveRackDevice(requests, label, *it, child, childOrder++);
        std::int64_t parentOrder = 0;
        for (auto it = order.begin(); it != begin; ++it) moveRackDevice(requests, label, *it, chain, parentOrder++);
        ++parentOrder;
        for (auto it = end; it != order.end(); ++it) moveRackDevice(requests, label, *it, chain, parentOrder++);
    });
}

CommitResult ungroupDeviceRack(Store& store, std::int64_t rack, Actor actor) {
    return editRack(store, actor, [&](std::vector<OpRequest>& requests, OpTransactionInputs& guards) {
        constexpr std::string_view label = "Ungroup Devices";
        const auto row = detail::captureRow(store.db(), "devices", "id", rack);
        if (!row || row->at("is_rack") == 0) throw std::runtime_error("the selected device is not a rack");
        const auto parent = integer(*row, "chain_id");
        (void)rackDestinationTrack(store, parent, guards);
        const auto children = captureRows(store, guards, "device_chains", {{"parent_device_id", rack}});
        if (children.size() != 1 || row->at("enabled") == 0)
            throw std::runtime_error("ungroup requires one enabled serial rack chain");
        const auto& child = children.front();
        if (child.at("muted") != 0 || !child.at("key_zone").is_null() || !child.at("vel_zone").is_null() ||
            !child.at("chain_zone").is_null())
            throw std::runtime_error("ungroup requires an audible chain with no zone restrictions");
        const auto members = orderedDevices(store, guards, integer(child, "id"));
        const auto siblings = orderedDevices(store, guards, parent);
        if (!std::in_range<std::int64_t>(siblings.size() - 1 + members.size()))
            throw std::runtime_error("device chain order is exhausted");
        std::int64_t position = 0;
        for (const auto& sibling : siblings) {
            if (integer(sibling, "id") == rack) {
                for (const auto& member : members) moveRackDevice(requests, label, member, parent, position++);
            } else {
                moveRackDevice(requests, label, sibling, parent, position++);
            }
        }
        appendRackEdit(requests, label, "device.remove", "device", rack, Payload::object());
    });
}
}
