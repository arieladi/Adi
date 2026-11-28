// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/builtin_catalog.hpp"
#include "adi/engine/mpe_output.hpp"
#include "adi/mix_functions.hpp"
#include "adi/op_transaction_resources.hpp"
#include "adi/row_ids.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>
namespace adi {
namespace {
std::int64_t chainOwner(const SQLite::Database& db, std::int64_t chain,
                       const std::set<std::int64_t>* moving = nullptr) {
    std::set<std::int64_t> seen;
    for (;;) {
        if (!seen.insert(chain).second) throw std::runtime_error("destination rack contains a cycle");
        SQLite::Statement row(db, "SELECT track_id,parent_device_id FROM device_chains WHERE id=?");
        row.bind(1, chain);
        if (!row.executeStep()) throw std::runtime_error("destination chain is missing");
        if (!row.getColumn(0).isNull()) return row.getColumn(0).getInt64();
        if (row.getColumn(1).isNull()) throw std::runtime_error("destination chain has no owner");
        const auto parentDevice = row.getColumn(1).getInt64();
        if (moving && moving->contains(parentDevice))
            throw std::runtime_error("A rack cannot be moved into one of its own chains.");
        SQLite::Statement parent(db, "SELECT chain_id FROM devices WHERE id=?");
        parent.bind(1, parentDevice);
        if (!parent.executeStep()) throw std::runtime_error("destination rack owner is missing");
        chain = parent.getColumn(0).getInt64();
    }
}
}
namespace detail {
std::int64_t deviceChainOwner(const SQLite::Database& db, std::int64_t chain) {
    return chainOwner(db, chain);
}
void requireDeviceInsertTrack(const SQLite::Database& db, std::int64_t track, DeviceRole role,
                              std::string_view deviceName) {
    SQLite::Statement target(db, "SELECT kind FROM tracks WHERE id=?");
    target.bind(1, track);
    if (!target.executeStep()) throw std::runtime_error("destination track is missing");
    const auto kind = target.getColumn(0).getString();
    if (kind == "vca") throw std::runtime_error("devices cannot be inserted on a VCA track");
    if (role == DeviceRole::AudioEffect || kind == "midi" || kind == "instrument") return;
    if (role == DeviceRole::Instrument)
        throw std::runtime_error(std::string(deviceName) + " is an instrument: load it onto a MIDI track");
    throw std::runtime_error(std::string(deviceName) + " is a MIDI effect: load it onto a MIDI track");
}
DeviceRole deviceRole(std::string_view subtype) noexcept {
    if (subtype == "midi_effect") return DeviceRole::MidiEffect;
    if (subtype == "instrument") return DeviceRole::Instrument;
    return DeviceRole::AudioEffect;
}
std::vector<ChainDevice> deviceChainOrder(const SQLite::Database& db, std::int64_t chain) {
    std::vector<ChainDevice> order;
    SQLite::Statement devices(db, "SELECT devices.id,devices.ord,"
        "COALESCE(NULLIF(plugin_refs.subtype,''),devices.rack_kind,''),devices.is_rack "
        "FROM devices LEFT JOIN plugin_refs ON plugin_refs.id=devices.plugin_ref_id "
        "WHERE devices.chain_id=? ORDER BY devices.ord,devices.id");
    devices.bind(1, chain);
    while (devices.executeStep())
        order.push_back({devices.getColumn(0).getInt64(), devices.getColumn(1).getInt64(),
                         deviceRole(devices.getColumn(2).getString()), devices.getColumn(3).getInt() != 0});
    return order;
}
std::size_t deviceInsertPosition(std::span<const ChainDevice> devices, DeviceRole kind, std::size_t requested) {
    std::size_t firstNonMidi = devices.size(), firstAudio = devices.size();
    std::size_t afterMidi = 0, afterNonAudio = 0;
    for (std::size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].role == DeviceRole::MidiEffect) afterMidi = i + 1;
        else firstNonMidi = std::min(firstNonMidi, i);
        if (devices[i].role == DeviceRole::AudioEffect) firstAudio = std::min(firstAudio, i);
        else afterNonAudio = i + 1;
    }
    if (kind == DeviceRole::MidiEffect) return std::min(requested, firstNonMidi);
    if (kind == DeviceRole::AudioEffect) return std::max(requested, afterNonAudio);
    if (afterMidi > firstAudio) throw std::runtime_error("no instrument position between MIDI and audio effects");
    return std::clamp(requested, afterMidi, firstAudio);
}
}
namespace {
std::int64_t integer(const Payload& value){return detail::signedInteger(value);}
// Live 12 Mixing §18.1 describes first-instrument arming under Exclusive Arm. The R16 observation
// will confirm what happens to the previously armed track in the same gesture.
constexpr bool kAutoArmFirstInstrumentWithExclusiveArm = true;

bool midiTrackNeedsFirstInstrument(SQLite::Database& db, std::int64_t track) {
    SQLite::Statement eligible(db, "SELECT 1 FROM tracks t WHERE t.id=? AND t.kind IN ('midi','instrument') "
        "AND NOT EXISTS (SELECT 1 FROM device_chains c JOIN devices d ON d.chain_id=c.id "
        "LEFT JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE c.track_id=t.id "
        "AND COALESCE(NULLIF(p.subtype,''),d.rack_kind,'')='instrument')");
    eligible.bind(1, track);
    return eligible.executeStep();
}
std::string editableTrackKind(SQLite::Database& db, std::int64_t track, std::string_view action = "moved") {
    std::set<std::int64_t> seen;
    std::string kind;
    for (auto current = track;;) {
        if (!seen.insert(current).second)
            throw std::runtime_error("The track's group hierarchy contains a cycle.");
        SQLite::Statement row(db, "SELECT kind,frozen,locked,parent_id FROM tracks WHERE id=?");
        row.bind(1, current);
        if (!row.executeStep()) throw std::runtime_error("A track for the device edit is missing.");
        if (row.getColumn(1).getInt() != 0 || row.getColumn(2).getInt() != 0)
            throw std::runtime_error("Devices cannot be " + std::string(action) + " on a frozen or locked track.");
        if (current == track) kind = row.getColumn(0).getString();
        const auto parent = row.getColumn(3);
        if (parent.isNull()) return kind;
        current = parent.getInt64();
    }
}

std::string identity(SQLite::Database& db) {
    SQLite::Statement query(db,"PRAGMA database_list");
    while(query.executeStep())if(query.getColumn(1).getString()=="main")return query.getColumn(2).getString();
    throw std::runtime_error("device editing needs a project");
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
static PreparedBuiltinInsert prepareInsert(Store& store,const rows::PluginRef& ref,std::int64_t track,
    std::optional<std::int64_t> before,std::int64_t retainedDeviceId,std::span<const OpRequest> newTrack,bool exclusiveArm,
    const PluginInsertOptions* plugin = nullptr, std::optional<std::int64_t> replace = {}) {
    PreparedBuiltinInsert result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        auto& db=store.db();RowIds ids(db);std::optional<SQLite::Transaction> read;
        if(sqlite3_get_autocommit(db.getHandle())!=0)read.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        result.sourceFile=identity(db);result.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement reference(db,"SELECT id,subtype FROM plugin_refs WHERE format=? AND uid=? AND shell_id IS ?");
        reference.bind(1,ref.format);reference.bind(2,ref.uid);
        if(ref.shellId)reference.bind(3,*ref.shellId);else reference.bind(3);
        const bool registered = reference.executeStep();
        // The project's existing reference determines the resulting role.
        const auto insertionRole = detail::deviceRole(replace && registered ? reference.getColumn(1).getString() : ref.subtype);
        // Built-in instruments and Browser plug-in loads can replace an
        // existing instrument. Paste uses the same order without replacement.
        const bool replaceInstrument = plugin ? plugin->replaceInstrument : insertionRole == detail::DeviceRole::Instrument;
        bool enabled = true;
        if (replace) {
            SQLite::Statement target(db, "SELECT d.chain_id,d.is_rack,d.enabled,p.format,p.uid,p.subtype "
                "FROM devices d LEFT JOIN plugin_refs p ON p.id=d.plugin_ref_id WHERE d.id=?");
            target.bind(1, *replace);
            if (!target.executeStep()) throw std::runtime_error("The device to replace is missing.");
            if (target.getColumn(1).getInt() != 0 || target.getColumn(3).isNull() ||
                mix::functionForReference(target.getColumn(3).getString(), target.getColumn(4).getString()))
                throw std::runtime_error("This device cannot be hot-swapped.");
            if (detail::deviceRole(target.getColumn(5).getString()) != insertionRole)
                throw std::runtime_error("Choose a replacement with the same device role.");
            result.chainId = target.getColumn(0).getInt64();
            enabled = target.getColumn(2).getInt() != 0;
            track = detail::deviceChainOwner(db, result.chainId);
            (void)editableTrackKind(db, track, "hot-swapped");
        }
        const std::string label = replace ? "Hot-Swap Device" : "Insert " + ref.name;
        if (newTrack.empty()) {
            detail::requireDeviceInsertTrack(db, track, insertionRole, ref.name);
        } else {
            const auto& create = newTrack.front();
            if (before || create.opType != "track.create" || create.payload.at("id") != track ||
                create.payload.at("kind") != (insertionRole == detail::DeviceRole::AudioEffect ? "audio" : "midi"))
                throw std::runtime_error("The new track does not match the device drop.");
            result.newTrack = track;
            result.requests.assign(newTrack.begin(), newTrack.end());
            for (auto& request : result.requests) request.label = label;
        }
        auto append=[&](const char* type,Payload payload,const char* kind,std::int64_t id) {
            OpRequest request;request.opType=type;request.payload=std::move(payload);request.label=label;
            request.targetKind=kind;request.targetId=id;result.requests.push_back(std::move(request));
        };
        if(registered)result.referenceId=reference.getColumn(0).getInt64();
        else {
            result.referenceId=ids.next("plugin_refs");
            auto row=referenceRow(db,{{"id",result.referenceId},{"format",ref.format},{"uid",ref.uid},{"vendor",ref.vendor},
                {"name",ref.name},{"version",ref.version},{"subtype",ref.subtype},{"path_hint",ref.pathHint},{"is_shell",ref.isShell?1:0},
                {"shell_id",ref.shellId ? Payload(*ref.shellId) : Payload(nullptr)}});
            append("pluginRef.register",{{"id",result.referenceId},{"row",row}},"plugin",result.referenceId);
        }
        if (!replace && before) {
            SQLite::Statement selected(db,"SELECT chain_id FROM devices WHERE id=?");selected.bind(1,*before);
            if(!selected.executeStep())throw std::runtime_error("selected device is missing");
            result.chainId=selected.getColumn(0).getInt64();
            if (detail::deviceChainOwner(db, result.chainId) != track)
                throw std::runtime_error("selected device belongs to another track");
        } else if (!replace) {
            SQLite::Statement chain(db,"SELECT id FROM device_chains WHERE track_id=? ORDER BY ord,id LIMIT 1");chain.bind(1,track);
            if(chain.executeStep())result.chainId=chain.getColumn(0).getInt64();
            else {
                result.chainId=ids.next("device_chains");
                append("chain.create",{{"id",result.chainId},{"track",track},{"ord",0},{"name","Devices"}},"chain",result.chainId);
            }
        }
        auto order = detail::deviceChainOrder(db, result.chainId);
        if(!std::in_range<std::int64_t>(order.size()))throw std::runtime_error("device order is exhausted");
        const auto anchor = replace ? replace : before;
        auto position = order.size();
        if(anchor)for(std::size_t i=0;i<order.size();++i)if(order[i].id==*anchor){position=i;break;}
        auto replacement = order.end();
        if (replace) {
            if (position == order.size()) throw std::runtime_error("The device to replace left its chain.");
            replacement = order.begin() + static_cast<std::ptrdiff_t>(position);
        } else if (replaceInstrument) {
            if (position < order.size() && order[position].role == detail::DeviceRole::Instrument) {
                replacement = order.begin() + static_cast<std::ptrdiff_t>(position);
            } else {
                for (auto device = order.begin(); device != order.end(); ++device) {
                    if (device->role != detail::DeviceRole::Instrument) continue;
                    if (replacement != order.end())
                        throw std::runtime_error("select which instrument to replace");
                    replacement = device;
                }
            }
        }
        std::optional<std::int64_t> replacementOrd;
        if (replacement != order.end()) {
            if (replacement->rack)
                throw std::runtime_error("select an instrument inside the rack to replace");
            position = static_cast<std::size_t>(replacement - order.begin());
            // A new ID sorts after all peers at the same ord. Only renumber
            // an exact replacement if that would move it past a tied peer.
            const auto next = std::next(replacement);
            if (replace && (next == order.end() || next->ord != replacement->ord))
                replacementOrd = replacement->ord;
            append("device.remove",{{"id",replacement->id}},"device",replacement->id);
            order.erase(replacement);
        } else {
            position = detail::deviceInsertPosition(order, insertionRole, position);
        }
        if (!replacementOrd) {
            for(std::size_t i=0;i<order.size();++i) {
                const auto index=static_cast<std::int64_t>(i+(i>=position?1u:0u));
                if(order[i].ord!=index)append("device.move",{{"id",order[i].id},{"chain",result.chainId},{"ord",index}},"device",order[i].id);
            }
        }
        ids.reserve("devices",retainedDeviceId);
        result.deviceId=ids.next("devices");
        append("device.insert",{{"id",result.deviceId},{"chain",result.chainId},
            {"ord",replacementOrd.value_or(static_cast<std::int64_t>(position))},
            {"ref",result.referenceId},{"name",ref.name},{"enabled",enabled},{"rack",false}},"device",result.deviceId);
        if (plugin && plugin->expressionRoute && *plugin->expressionRoute != engine::RouteChoice::Auto)
            append("device.setExpressionRoute",{{"dev",result.deviceId},{"route",engine::routeChoiceName(*plugin->expressionRoute)}},
                   "device",result.deviceId);
        if (kAutoArmFirstInstrumentWithExclusiveArm && exclusiveArm && !replace &&
            insertionRole == detail::DeviceRole::Instrument && (result.newTrack || midiTrackNeedsFirstInstrument(db, track))) {
            SQLite::Statement arms(db, "SELECT id,record_armed FROM tracks WHERE kind IN ('audio','midi','instrument') "
                "AND (id=? OR record_armed<>0) ORDER BY id");
            arms.bind(1, track);
            while (arms.executeStep()) {
                const auto id = arms.getColumn(0).getInt64();
                const bool arm = id == track;
                if (arm != (arms.getColumn(1).getInt() != 0))
                    append("track.setArm", {{"id", id}, {"armed", arm}}, "track", id);
            }
            if (result.newTrack)
                append("track.setArm", {{"id", track}, {"armed", true}}, "track", track);
        }
        result.ok=true;
    }catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
static PreparedBuiltinInsert prepareBuiltin(Store& store,std::string_view uid,std::int64_t track,std::optional<std::int64_t> before,
    std::int64_t retainedDeviceId,std::span<const OpRequest> newTrack,bool exclusiveArm,std::optional<std::int64_t> replace = {}) {
    const auto* info=findBuiltin(uid);
    if(!info || !info->available) {
        PreparedBuiltinInsert result;
        result.error=info?std::string(info->name)+" is not available through this built-in loader":"unknown built-in device";
        return result;
    }
    rows::PluginRef ref;
    ref.format="internal";ref.uid=uid;ref.name=info->name;ref.vendor="ADI";ref.version="1";
    ref.subtype=info->instrument?"instrument":"effect";
    return prepareInsert(store,ref,track,before,retainedDeviceId,newTrack,exclusiveArm,nullptr,replace);
}
static PreparedBuiltinInsert preparePlugin(Store& store,const rows::PluginRef& ref,std::int64_t track,
    std::optional<std::int64_t> before,const PluginInsertOptions& options,std::int64_t retainedDeviceId,
    std::span<const OpRequest> newTrack,std::optional<std::int64_t> replace = {}) {
    if((ref.format!="vst3" && ref.format!="clap") || ref.uid.empty() || ref.name.empty()) {
        PreparedBuiltinInsert result;result.error="Select a VST3 or CLAP plug-in with a valid identity";return result;
    }
    if (options.replaceInstrument && detail::deviceRole(ref.subtype) != detail::DeviceRole::Instrument) {
        PreparedBuiltinInsert result;result.error="only an instrument can replace the current instrument";return result;
    }
    if (options.expressionRoute && *options.expressionRoute != engine::RouteChoice::Auto &&
        *engine::routeChoiceName(*options.expressionRoute) == '\0') {
        PreparedBuiltinInsert result;result.error="unknown plug-in expression route";return result;
    }
    return prepareInsert(store,ref,track,before,retainedDeviceId,newTrack,options.exclusiveArm,&options,replace);
}
PreparedBuiltinInsert prepareBuiltinInsert(Store& store,std::string_view uid,std::int64_t track,std::optional<std::int64_t> before,
    std::int64_t retainedDeviceId,std::span<const OpRequest> newTrack,bool exclusiveArm) {
    return prepareBuiltin(store,uid,track,before,retainedDeviceId,newTrack,exclusiveArm);
}
PreparedBuiltinInsert preparePluginInsert(Store& store,const rows::PluginRef& ref,std::int64_t track,
    std::optional<std::int64_t> before,const PluginInsertOptions& options,std::int64_t retainedDeviceId,
    std::span<const OpRequest> newTrack) {
    return preparePlugin(store,ref,track,before,options,retainedDeviceId,newTrack);
}
PreparedBuiltinInsert prepareBuiltinReplacement(Store& store, std::string_view uid, std::int64_t targetDevice,
    std::int64_t retainedDeviceId) {
    return prepareBuiltin(store,uid,0,{},retainedDeviceId,{},false,targetDevice);
}
PreparedBuiltinInsert preparePluginReplacement(Store& store, const rows::PluginRef& ref, std::int64_t targetDevice,
    std::int64_t retainedDeviceId) {
    return preparePlugin(store,ref,0,{},{},retainedDeviceId,{},targetDevice);
}
PreparedDeviceMove prepareDeviceMove(Store& store, DeviceMoveSource source, std::int64_t track,
                                    std::optional<std::int64_t> before) {
    return prepareDeviceMove(store, std::span(&source, 1), track, before);
}
PreparedDeviceMove prepareDeviceMove(Store& store, std::span<const DeviceMoveSource> sources, std::int64_t track,
                                    std::optional<std::int64_t> before) {
    PreparedDeviceMove result;
    try {
        if (store.readOnly()) throw std::runtime_error("project is read-only");
        if (sources.empty()) throw std::runtime_error("Select at least one device to move.");
        const auto& source = sources.front();
        std::set<std::int64_t> selected;
        for (const auto& member : sources) {
            if (member.track != source.track || member.chain != source.chain || !selected.insert(member.device).second)
                throw std::runtime_error("Select distinct devices in one chain to move together.");
        }

        auto& db = store.db();
        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            read.emplace(db, SQLite::TransactionBehavior::DEFERRED);
        result.source = appliedSource(store);
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        const auto sourceKind = editableTrackKind(db, source.track);
        const auto targetKind = track == source.track ? sourceKind : editableTrackKind(db, track);
        detail::requireDeviceInsertTrack(db, track);
        auto from = detail::deviceChainOrder(db, source.chain);
        const auto isSelected = [&](const auto& device) { return selected.contains(device.id); };
        const auto first = std::find_if(from.begin(), from.end(), isSelected);
        const auto last = std::find_if_not(first, from.end(), isSelected);
        if (!std::cmp_equal(last - first, selected.size()) || chainOwner(db, source.chain) != source.track)
            throw std::runtime_error("The selected devices no longer form one contiguous range in their chain.");
        if (before && track == source.track && selected.contains(*before)) {
            result.ok = true;
            return result;
        }
        const std::vector<detail::ChainDevice> moved(first, last);
        if (source.track != track && targetKind != "midi" && targetKind != "instrument" &&
            std::ranges::any_of(moved, [](const auto& device) { return device.role != detail::DeviceRole::AudioEffect; }))
            throw std::runtime_error("MIDI effects and instruments need a MIDI track.");

        std::optional<std::int64_t> destination;
        if (before) {
            SQLite::Statement anchor(db, "SELECT chain_id FROM devices WHERE id=?");
            anchor.bind(1, *before);
            if (!anchor.executeStep()) throw std::runtime_error("The destination device changed before the drop.");
            destination = anchor.getColumn(0).getInt64();
            if (chainOwner(db, *destination, &selected) != track)
                throw std::runtime_error("The destination device belongs to another track.");
        } else {
            SQLite::Statement root(db, "SELECT id FROM device_chains WHERE track_id=? ORDER BY ord DESC,id DESC LIMIT 1");
            root.bind(1, track);
            if (root.executeStep()) destination = root.getColumn(0).getInt64();
        }
        const auto originalPosition = static_cast<std::size_t>(first - from.begin());
        from.erase(first, last);
        const bool sameChain = destination == source.chain;
        auto to = sameChain ? std::move(from) : destination ? detail::deviceChainOrder(db, *destination)
                                                          : std::vector<detail::ChainDevice>{};
        const auto isInstrument = [](const auto& device) { return device.role == detail::DeviceRole::Instrument; };
        if (!sameChain && std::ranges::any_of(moved, isInstrument)) {
            if (std::ranges::any_of(to, isInstrument))
                throw std::runtime_error("The destination chain already has an instrument.");
            if (std::ranges::count_if(moved, isInstrument) > 1)
                throw std::runtime_error("Only one instrument can move to a different chain.");
        }
        const auto anchor = before ? std::find_if(to.begin(), to.end(), [&](const auto& device) { return device.id == *before; }) : to.end();
        if (before && anchor == to.end()) throw std::runtime_error("The destination device changed before the drop.");
        const auto requested = static_cast<std::size_t>(anchor - to.begin());
        if (!std::ranges::is_sorted(moved, {}, &detail::ChainDevice::role))
            throw std::runtime_error("The selected devices cannot move together in their current order.");
        // The last role sets the earliest position, and the first role sets
        // the latest. The whole selection must fit at one position.
        const auto earliest = detail::deviceInsertPosition(to, moved.back().role, 0);
        const auto latest = detail::deviceInsertPosition(to, moved.front().role, to.size());
        if (earliest > latest) throw std::runtime_error("There is no position for all selected devices in this chain.");
        const auto position = std::clamp(requested, earliest, latest);
        if (sameChain && position == originalPosition) {
            result.ok = true;
            return result;
        }

        const auto append = [&](const char* op, Payload payload, const char* kind, std::int64_t id) {
            OpRequest request;
            request.opType = op;
            request.payload = std::move(payload);
            request.label = moved.size() == 1 ? "Move Device" : "Move Devices";
            request.targetKind = kind;
            request.targetId = id;
            result.requests.push_back(std::move(request));
        };
        if (!destination) {
            destination = RowIds(db).next("device_chains");
            append("chain.create", {{"id", *destination}, {"track", track}, {"ord", 0}, {"name", "Devices"}},
                   "chain", *destination);
        }
        to.insert(to.begin() + static_cast<std::ptrdiff_t>(position), moved.begin(), moved.end());
        const auto renumber = [&](const std::vector<detail::ChainDevice>& order, std::int64_t chain) {
            if (!std::in_range<std::int64_t>(order.size())) throw std::runtime_error("device order is exhausted");
            for (std::size_t i = 0; i < order.size(); ++i) {
                const auto ordinal = static_cast<std::int64_t>(i);
                if (order[i].ord != ordinal || (chain != source.chain && isSelected(order[i])))
                    append("device.move", {{"id", order[i].id}, {"chain", chain}, {"ord", ordinal}},
                           "device", order[i].id);
            }
        };
        if (!sameChain) renumber(from, source.chain);
        renumber(to, *destination);
        result.ok = true;
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
CommitResult commitDeviceMove(Store& store, const PreparedDeviceMove& prepared, Actor actor) {
    CommitResult result;
    if (!prepared.ok) { result.error = prepared.error; return result; }
    if (!prepared.source.alive() || !prepared.source.sameConnection(appliedSource(store))) {
        result.error = "project changed after the device move was prepared";
        result.stale = true;
        return result;
    }
    auto requests = prepared.requests;
    for (auto& request : requests) request.actor = actor;
    return OpJournal(store).commitCaptured(requests, prepared.source, prepared.options, {});
}
CommitResult commitBuiltinInsert(Store& store,const PreparedBuiltinInsert& prepared,Actor actor) {
    CommitResult result;if(!prepared.ok){result.error=prepared.error;return result;}
    if(identity(store.db())!=prepared.sourceFile){result.error="project changed after built-in insertion preparation";result.stale=true;return result;}
    auto requests=prepared.requests;for(auto& request:requests)request.actor=actor;
    return OpJournal(store).commit(requests,prepared.options);
}
}
