// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/session_ops.hpp"

#include "adi/clip_image.hpp"
#include "adi/edit_rows.hpp"
#include "adi/engine/launch_data.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace adi {
namespace {
using detail::signedInteger;
namespace launch = engine::launch;

enum class Table { Scene, Slot, Launch, Marker };
struct Key {
    Table table;
    std::int64_t id, scene = 0;
    auto operator<=>(const Key&) const = default;
};

const char* tableName(Table table) {
    switch (table) {
    case Table::Scene: return "scenes";
    case Table::Slot: return "clip_slots";
    case Table::Launch: return "clip_launch";
    case Table::Marker: return "extensions";
    }
    throw std::runtime_error("invalid Session table");
}

Payload primaryKey(Key key) {
    if (key.table == Table::Scene || key.table == Table::Marker) return {{"id",key.id}};
    if (key.table == Table::Launch) return {{"clip_id",key.id}};
    return {{"track_id",key.id},{"scene_id",key.scene}};
}

std::int64_t identity(const Payload& value) {
    const auto id = signedInteger(value);
    if (id <= 0) throw std::runtime_error("Session identities must be positive");
    return id;
}

Key slotKey(const Payload& payload) {
    return {Table::Slot,identity(payload.at("track")),identity(payload.at("scene"))};
}

Key keyFrom(const Payload& change) {
    const auto table = signedInteger(change.at("table"));
    if (table == static_cast<std::int64_t>(Table::Marker))
        return {Table::Marker,signedInteger(change.at("id"))};
    const auto id = identity(change.at("id"));
    switch (table) {
    case static_cast<std::int64_t>(Table::Scene): return {Table::Scene,id};
    case static_cast<std::int64_t>(Table::Slot): return {Table::Slot,id,identity(change.at("scene"))};
    case static_cast<std::int64_t>(Table::Launch): return {Table::Launch,id};
    default: throw std::runtime_error("invalid captured Session table");
    }
}

class Patch {
public:
    explicit Patch(SQLite::Database& db) : clips(db), db_(db) {}

    [[nodiscard]] std::optional<Payload> current(Key key) const {
        if (const auto it = rows_.find(key); it != rows_.end()) return it->second.after;
        return detail::captureRowKey(db_,tableName(key.table),primaryKey(key));
    }

    [[nodiscard]] Payload require(Key key) const {
        auto row = current(key);
        if (!row) throw std::runtime_error(std::string("missing ") + tableName(key.table) + " row");
        return std::move(*row);
    }

    void set(Key key, std::optional<Payload> row) {
        if (auto it = rows_.find(key); it != rows_.end()) {
            it->second.after = std::move(row);
            return;
        }
        rows_.emplace(key,Change{current(key),std::move(row)});
    }

    [[nodiscard]] Payload packet() const {
        auto rows = Payload::array();
        for (const auto& [key, change] : rows_) {
            if (!change.before && !change.after) continue;
            rows.push_back({{"table",static_cast<std::int64_t>(key.table)}, {"id",key.id}, {"scene",key.scene},
                {"before",change.before.value_or(nullptr)}, {"after",change.after.value_or(nullptr)}});
        }
        return {{"session",std::move(rows)},{"clips",clips.changes()}};
    }

    edit::RowEdits clips;

private:
    struct Change { std::optional<Payload> before, after; };
    SQLite::Database& db_;
    std::map<Key,Change> rows_;
};

Payload sceneDefaults(std::int64_t id, const Payload& request) {
    const auto name = request.value("name",Payload(nullptr));
    return {{"id",id}, {"ord",request.at("ord")}, {"name",name.is_null() ? Payload("") : name},
        {"color",request.value("color",Payload(nullptr))}, {"tempo_bpm",launch::defaultDormantSceneTempo},
        {"tempo_enabled",0}, {"signature_numerator",launch::defaultDormantSceneSignature},
        {"signature_denominator",launch::defaultDormantSceneSignature}, {"signature_enabled",0},
        {"follow_enabled",0}, {"follow_duration_ticks",launch::defaultFollowDurationTicks},
        {"follow_a",0}, {"follow_a_scene_id",nullptr}, {"follow_b",0}, {"follow_b_scene_id",nullptr},
        {"follow_b_chance",0.0}};
}

Payload launchDefaults(std::int64_t id) {
    return {{"clip_id",id}, {"launch_mode",0}, {"launch_quant",0}, {"legato",0},
        {"velocity_amount",launch::defaultVelocityAmount}, {"follow_enabled",0}, {"follow_linked",1},
        {"follow_loop_count",1}, {"follow_duration_ticks",launch::defaultFollowDurationTicks},
        {"follow_a",0}, {"follow_a_scene_id",nullptr}, {"follow_b",0}, {"follow_b_scene_id",nullptr},
        {"follow_b_chance",0.0}, {"clip_fade",launch::defaultClipFade ? 1 : 0},
        {"ram_mode",launch::defaultClipRamMode ? 1 : 0}};
}

Payload slotDefaults(Key key) {
    return {{"track_id",key.id}, {"scene_id",key.scene}, {"clip_id",nullptr},
        {"has_stop",launch::defaultEmptySlotStop ? 1 : 0}};
}

void setSlot(Patch& patch, Key key, Payload row) {
    if (row.at("clip_id").is_null() && row.at("has_stop") == (launch::defaultEmptySlotStop ? 1 : 0))
        patch.set(key,std::nullopt);
    else
        patch.set(key,std::move(row));
}

std::optional<Key> findSlot(SQLite::Database& db, std::int64_t clip) {
    SQLite::Statement query(db,"SELECT track_id,scene_id FROM clip_slots WHERE clip_id=?");
    query.bind(1,clip);
    if (!query.executeStep()) return std::nullopt;
    return Key{Table::Slot,query.getColumn(0).getInt64(),query.getColumn(1).getInt64()};
}

void requireSlotAddress(OpContext& context, const Patch& patch, Key key) {
    (void)patch.require({Table::Scene,key.scene});
    SQLite::Statement query(context.db,"SELECT kind FROM tracks WHERE id=?");
    query.bind(1,key.id);
    if (!query.executeStep()) throw std::runtime_error("slot track does not exist");
    const auto kind = query.getColumn(0).getString();
    if (kind != "audio" && kind != "midi" && kind != "instrument")
        throw std::runtime_error("slot needs an audio, MIDI or instrument track");
}

constexpr std::string_view sessionNamespace = "org.adidaw.session";
constexpr std::string_view sessionMarker = "clip_slots";

bool featureMarker(const Payload& row) {
    return row.at("ns") == std::string(sessionNamespace) && row.at("key") == std::string(sessionMarker) &&
        row.at("scope_kind") == "project" && row.at("scope_id").is_null();
}

void markOccupiedSlots(OpContext& context, Patch& patch) {
    SQLite::Statement query(context.db,
        "SELECT id FROM extensions WHERE ns=? AND key=? AND scope_kind='project' AND scope_id IS NULL");
    query.bind(1,std::string(sessionNamespace));
    query.bind(2,std::string(sessionMarker));
    if (query.executeStep()) {
        const Key key{Table::Marker,query.getColumn(0).getInt64()};
        auto row = patch.require(key);
        if (row.at("criticality") == "essential" && row.at("min_reader") >= kSessionSchemaUserVersion) return;
        row["criticality"] = "essential";
        row["min_reader"] = std::max<std::int64_t>(detail::signedInteger(row.at("min_reader")),kSessionSchemaUserVersion);
        patch.set(key,std::move(row));
        return;
    }
    // This derived marker has one project-level natural identity. Choose a
    // free physical rowid and capture it in the inverse alongside the slot.
    SQLite::Statement ids(context.db,"SELECT id FROM extensions WHERE id>0 ORDER BY id");
    auto id = std::int64_t{1};
    while (ids.executeStep() && ids.getColumn(0).getInt64() == id) {
        if (id == std::numeric_limits<std::int64_t>::max()) throw std::runtime_error("no free extension row ID");
        ++id;
    }
    patch.set({Table::Marker,id},Payload{{"id",id},{"ns",sessionNamespace},{"key",sessionMarker},
        {"scope_kind","project"},{"scope_id",nullptr},{"criticality","essential"},
        {"min_reader",kSessionSchemaUserVersion},{"mime","application/octet-stream"},{"data",nullptr}});
}

void deleteClips(OpContext& context, Patch& patch, const std::set<std::int64_t>& ids) {
    for (const auto clip : ids) {
        patch.set({Table::Launch,clip},std::nullopt);
        auto image = clip_edit::captureImage(context,clip);
        auto& links = image.at("alias_links");
        links.erase(std::remove_if(links.begin(),links.end(),[&](const Payload& id) {
            return ids.contains(signedInteger(id));
        }),links.end());
        clip_edit::deleteImage(patch.clips,image);
    }
}

void clearSceneReferences(OpContext& context, Patch& patch, std::int64_t scene) {
    for (const auto table : {Table::Scene, Table::Launch}) {
        const auto* key = table == Table::Scene ? "id" : "clip_id";
        SQLite::Statement query(context.db,std::string("SELECT ") + key + " FROM " + tableName(table) +
            " WHERE follow_a_scene_id=? OR follow_b_scene_id=?");
        query.bind(1,scene);
        query.bind(2,scene);
        while (query.executeStep()) {
            const Key address{table,query.getColumn(0).getInt64()};
            auto row = patch.current(address);
            if (!row) continue;
            for (const auto* side : {"follow_a","follow_b"}) {
                const auto jump = std::string(side) + "_scene_id";
                if (row->at(jump) != scene) continue;
                (*row)[side] = static_cast<std::int64_t>(engine::FollowAction::None);
                (*row)[jump] = nullptr;
            }
            patch.set(address,std::move(row));
        }
    }
}

void deleteScene(OpContext& context, Patch& patch, Key key) {
    (void)patch.require(key);
    std::set<std::int64_t> removedClips;
    SQLite::Statement query(context.db,"SELECT track_id,clip_id FROM clip_slots WHERE scene_id=? ORDER BY track_id");
    query.bind(1,key.id);
    while (query.executeStep()) {
        patch.set({Table::Slot,query.getColumn(0).getInt64(),key.id},std::nullopt);
        if (!query.isColumnNull(1)) removedClips.insert(query.getColumn(1).getInt64());
    }
    deleteClips(context,patch,removedClips);
    patch.set(key,std::nullopt);
    clearSceneReferences(context,patch,key.id);
}

void reorderScenes(OpContext& context, Patch& patch, const Payload& payload) {
    std::set<std::int64_t> known;
    SQLite::Statement query(context.db,"SELECT id FROM scenes");
    while (query.executeStep()) known.insert(query.getColumn(0).getInt64());
    auto ordinal = std::int64_t{0};
    for (const auto& value : payload.at("scenes")) {
        const Key key{Table::Scene,identity(value)};
        if (known.erase(key.id) != 1) throw std::runtime_error("invalid or duplicate reordered scene");
        auto row = patch.require(key);
        row["ord"] = ordinal++;
        patch.set(key,std::move(row));
    }
    if (!known.empty()) throw std::runtime_error("reorder must name every scene");
}

void editSlotClip(OpContext& context, Patch& patch, Key key, const Payload& payload, bool assign) {
    requireSlotAddress(context,patch,key);
    auto cell = patch.current(key).value_or(slotDefaults(key));
    auto& destination = cell.at("clip_id");
    const auto newClip = assign ? identity(payload.at("clip")) : 0;
    if (!destination.is_null() && destination == newClip) return;
    if (!destination.is_null()) deleteClips(context,patch,{identity(destination)});
    destination = nullptr;

    if (assign) {
        markOccupiedSlots(context,patch);
        auto clip = patch.clips.current("clips",newClip);
        if (!clip || clip->at("track_id") != key.id ||
            (clip->at("kind") != "audio" && clip->at("kind") != "midi"))
            throw std::runtime_error("slot needs an audio or MIDI clip on its own track");
        if (const auto old = findSlot(context.db,newClip)) {
            auto previous = patch.require(*old);
            previous["clip_id"] = nullptr;
            setSlot(patch,*old,std::move(previous));
            const Key settings{Table::Launch,newClip};
            patch.set(settings,patch.current(settings));
        }
        const auto musical = clip->at("time_base") == 0;
        (*clip)["lane_id"] = nullptr;
        (*clip)["pos_ticks"] = musical ? Payload(0) : Payload(nullptr);
        (*clip)["pos_ns"] = musical ? Payload(nullptr) : Payload(0);
        patch.clips.set("clips",newClip,std::move(clip));
        destination = newClip;
    }
    setSlot(patch,key,std::move(cell));
}

bool present(const Payload& payload, const char* key) {
    const auto value = payload.find(key);
    return value != payload.end() && !value->is_null();
}

void setFlag(Payload& row, const Payload& payload, const char* key, const char* field) {
    if (present(payload,key)) row[field] = payload.at(key).get<bool>() ? 1 : 0;
}

void editFollow(Patch& patch, Payload& row, const Payload& payload, bool clip) {
    row["follow_enabled"] = payload.at("enabled").get<bool>() ? 1 : 0;
    row["follow_duration_ticks"] = payload.at("duration");
    row["follow_b_chance"] = payload.at("chanceB");
    for (const auto& [request, target, column] : std::array{
            std::array{"actionA","jumpA","follow_a"}, std::array{"actionB","jumpB","follow_b"}}) {
        const auto action = signedInteger(payload.at(request));
        const auto jump = present(payload,target);
        if ((action == static_cast<std::int64_t>(engine::FollowAction::Jump)) != jump)
            throw std::runtime_error("Jump needs exactly one literal scene target");
        if (jump) (void)patch.require({Table::Scene,identity(payload.at(target))});
        row[column] = action;
        row[std::string(column) + "_scene_id"] = jump ? payload.at(target) : Payload(nullptr);
    }
    if (clip) {
        setFlag(row,payload,"linked","follow_linked");
        if (present(payload,"loops")) row["follow_loop_count"] = payload.at("loops");
    } else if (present(payload,"linked") || present(payload,"loops")) {
        throw std::runtime_error("scene follows have an unlinked duration");
    }
}

enum class Action { Create, Delete, Reorder, Rename, Tempo, Signature, Enable,
                    Assign, Clear, Stop, Mode, Quantization, Settings, Follow };

Payload plan(OpContext& context, const Payload& payload, Action action) {
    Patch patch(context.db);
    switch (action) {
    case Action::Reorder:
        reorderScenes(context,patch,payload);
        break;
    case Action::Assign:
    case Action::Clear:
        editSlotClip(context,patch,slotKey(payload),payload,action == Action::Assign);
        break;
    case Action::Stop: {
        const auto key = slotKey(payload);
        requireSlotAddress(context,patch,key);
        auto row = patch.current(key).value_or(slotDefaults(key));
        setFlag(row,payload,"on","has_stop");
        setSlot(patch,key,std::move(row));
        break;
    }
    case Action::Mode:
    case Action::Quantization:
    case Action::Settings:
    case Action::Follow: {
        const auto clip = present(payload,"clip");
        const auto scene = present(payload,"scene");
        if (clip == scene) throw std::runtime_error("address one clip or one scene");
        const Key key{clip ? Table::Launch : Table::Scene,identity(payload.at(clip ? "clip" : "scene"))};
        if (clip && !findSlot(context.db,key.id)) throw std::runtime_error("launch settings require a slotted clip");
        auto row = clip ? patch.current(key).value_or(launchDefaults(key.id)) : patch.require(key);
        if (action == Action::Mode) row["launch_mode"] = payload.at("mode");
        if (action == Action::Quantization) row["launch_quant"] = payload.at("quant");
        if (action == Action::Settings) {
            if (!present(payload,"legato") && !present(payload,"velocity") &&
                !present(payload,"fade") && !present(payload,"ram"))
                throw std::runtime_error("no launch setting supplied");
            setFlag(row,payload,"legato","legato");
            setFlag(row,payload,"fade","clip_fade");
            setFlag(row,payload,"ram","ram_mode");
            if (present(payload,"velocity")) row["velocity_amount"] = payload.at("velocity");
        }
        if (action == Action::Follow) editFollow(patch,row,payload,clip);
        patch.set(key,std::move(row));
        break;
    }
    default: {
        const Key key{Table::Scene,identity(payload.at("id"))};
        if (action == Action::Create) {
            if (patch.current(key)) throw std::runtime_error("scene identity is occupied");
            patch.set(key,sceneDefaults(key.id,payload));
        } else if (action == Action::Delete) {
            deleteScene(context,patch,key);
        } else {
            auto row = patch.require(key);
            if (action == Action::Rename) row["name"] = payload.at("name");
            if (action == Action::Tempo) row["tempo_bpm"] = payload.at("bpm");
            if (action == Action::Signature) {
                row["signature_numerator"] = payload.at("numerator");
                row["signature_denominator"] = payload.at("denominator");
            }
            if (action == Action::Enable) {
                if (!present(payload,"tempo") && !present(payload,"signature"))
                    throw std::runtime_error("no scene enable supplied");
                setFlag(row,payload,"tempo","tempo_enabled");
                setFlag(row,payload,"signature","signature_enabled");
            }
            patch.set(key,std::move(row));
        }
        break;
    }
    }
    return patch.packet();
}

struct RowChange {
    Key address;
    const Payload& before;
    const Payload& after;
};

std::vector<RowChange> validatePatch(SQLite::Database& db, const Payload& packet) {
    std::vector<RowChange> rows;
    std::set<Key> keys;
    if (!packet.at("session").is_array()) throw std::runtime_error("captured Session rows must be an array");
    for (const auto& change : packet.at("session")) {
        const auto address = keyFrom(change);
        if (!keys.insert(address).second) throw std::runtime_error("duplicate captured Session row");
        const auto key = primaryKey(address);
        const auto& before = change.at("before");
        const auto& after = change.at("after");
        for (const auto* row : {&before,&after}) {
            if (row->is_null()) continue;
            for (auto column = key.begin(); column != key.end(); ++column) {
                if (row->at(column.key()) != column.value())
                    throw std::runtime_error("captured Session identity differs");
            }
            if (address.table == Table::Marker && !featureMarker(*row))
                throw std::runtime_error("unexpected captured Session extension");
        }
        const auto current = detail::captureRowKey(db,tableName(address.table),key);
        if (current.value_or(nullptr) != before)
            throw std::runtime_error("captured Session row changed");
        rows.push_back({address,before,after});
    }
    return rows;
}

void stageSceneOrdinals(SQLite::Database& db, const std::vector<RowChange>& rows) {
    const auto changed = [](const RowChange& row) {
        return row.address.table == Table::Scene && !row.before.is_null() && !row.after.is_null() &&
            row.before.at("ord") != row.after.at("ord");
    };
    if (std::none_of(rows.begin(),rows.end(),changed)) return;
    std::set<std::int64_t> occupied;
    SQLite::Statement query(db,"SELECT ord FROM scenes");
    while (query.executeStep()) occupied.insert(query.getColumn(0).getInt64());
    for (const auto& row : rows) {
        if (row.address.table == Table::Scene && !row.after.is_null())
            occupied.insert(signedInteger(row.after.at("ord")));
    }
    auto spare = std::numeric_limits<std::int64_t>::max();
    SQLite::Statement update(db,"UPDATE scenes SET ord=? WHERE id=?");
    for (const auto& row : rows) {
        if (!changed(row)) continue;
        while (occupied.contains(spare)) --spare;
        update.bind(1,spare--);
        update.bind(2,row.address.id);
        update.exec();
        update.reset();
    }
}

void put(SQLite::Database& db, const RowChange& row, bool retained) {
    const auto* table = tableName(row.address.table);
    const auto key = primaryKey(row.address);
    if (!row.after.is_null()) {
        if (retained) detail::updateRowKey(db,table,row.after,key);
        else detail::restoreRow(db,table,row.after);
    } else if (retained) {
        (void)detail::eraseRowKey(db,table,key);
    }
}

bool detach(const RowChange& row) {
    if (row.before.is_null()) return false;
    if (row.address.table == Table::Launch) return true;
    return row.address.table == Table::Slot && (row.after.is_null() || row.before.at("clip_id") != row.after.at("clip_id"));
}

bool applyPatch(OpContext& context, const Payload& packet, std::string& error) {
    auto& db = context.db;
    const auto rows = validatePatch(db,packet);
    // Slot ownership constrains clip placement. Release changed bindings before
    // the shared clip edit, then establish the new ones after restoring roots.
    for (const auto table : {Table::Launch,Table::Slot}) {
        for (const auto& row : rows) {
            if (row.address.table == table && detach(row))
                (void)detail::eraseRowKey(db,tableName(table),primaryKey(row.address));
        }
    }
    const auto& clips = packet.at("clips");
    if (!clips.is_array()) throw std::runtime_error("captured clip rows must be an array");
    for (const auto& change : clips) {
        const auto& table = change.at("table");
        const auto known = std::any_of(clip_edit::childTables().begin(),clip_edit::childTables().end(),
            [&](const auto& child) { return table == child.table; });
        if (table != "clips" && !known) throw std::runtime_error("unexpected captured clip table");
    }
    if (!clips.empty() && !edit::applyRows(context,{{"changes",clips}},error)) return false;
    stageSceneOrdinals(db,rows);
    // Deferred scene jump references allow a restored scene and its Jump
    // sources to arrive in either stable ID order inside this transaction.
    for (const auto table : {Table::Scene,Table::Slot,Table::Launch,Table::Marker}) {
        for (const auto& row : rows) {
            if (row.address.table != table) continue;
            const bool retained = !row.before.is_null() && !detach(row);
            put(db,row,retained);
        }
    }
    return true;
}

template<Action action>
bool apply(OpContext& context, const Payload& payload, std::string& error) {
    try {
        if (!payload.contains("_session")) return applyPatch(context,plan(context,payload,action),error);
        if (!context.replay || !context.replay->asInverse)
            throw std::runtime_error("captured Session state is only used by Undo");
        return applyPatch(context,payload.at("_session"),error);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

template<Action action>
bool capture(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try {
        auto packet = plan(context,payload,action);
        for (auto& change : packet.at("session")) std::swap(change.at("before"),change.at("after"));
        if (!packet.at("clips").empty()) {
            Payload other;
            if (!edit::inverseRows(context,{{"changes",packet.at("clips")}},other,error)) return false;
            packet["clips"] = std::move(other.at("changes"));
        }
        inverse = {{"_session",std::move(packet)}};
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

constexpr Field sceneId{"id",FieldType::Int,true,"Scene identity",{},1};
constexpr Field trackId{"track",FieldType::Int,true,"Track identity",{},1};
constexpr Field slotScene{"scene",FieldType::Int,true,"Scene identity",{},1};
constexpr Field clipId{"clip",FieldType::Int,true,"Clip identity",{},1};
constexpr Field sceneCreateFields[]{sceneId,{"ord",FieldType::Int,true,"Scene order",{},0},
    {"name",FieldType::Text,false},{"color",FieldType::Int,false,"Palette index",{},0,69}};
constexpr Field sceneIdFields[]{sceneId};
constexpr Field sceneOrderFields[]{{"scenes",FieldType::Array,true,"All scene identities in the desired order"}};
constexpr Field sceneNameFields[]{sceneId,{"name",FieldType::Text}};
constexpr Field sceneTempoFields[]{sceneId,{"bpm",FieldType::Real,true,"Stored scene tempo",{},20,999}};
constexpr Field signatureFields[]{sceneId,{"numerator",FieldType::Int,true,"Meter numerator",{},1,99},
    {"denominator",FieldType::Int,true,"Power of two from 1 to 16",{},1,16}};
constexpr Field sceneEnableFields[]{sceneId,{"tempo",FieldType::Bool,false},{"signature",FieldType::Bool,false}};
constexpr Field slotAssignFields[]{trackId,slotScene,clipId};
constexpr Field slotAddressFields[]{trackId,slotScene};
constexpr Field stopButtonFields[]{trackId,slotScene,{"on",FieldType::Bool}};
constexpr Field launchModeFields[]{clipId,{"mode",FieldType::Int,true,"Trigger, Gate, Toggle or Repeat",{},0,3}};
constexpr Field launchQuantFields[]{clipId,{"quant",FieldType::Int,true,"Clip quantization code, 0 is Global",{},0,14}};
constexpr Field launchSettingsFields[]{clipId,{"legato",FieldType::Bool,false},
    {"velocity",FieldType::Real,false,"Velocity amount",{},0,1},
    {"fade",FieldType::Bool,false},{"ram",FieldType::Bool,false}};
constexpr Field followFields[]{{"clip",FieldType::Int,false,"Clip target identity",{},1},
    {"scene",FieldType::Int,false,"Scene target identity",{},1}, {"enabled",FieldType::Bool},
    {"duration",FieldType::Int,true,"Unlinked duration in ticks",{},1},
    {"actionA",FieldType::Int,true,"First Follow Action code",{},0,9},
    {"jumpA",FieldType::Int,false,"First action's Jump scene identity",{},1},
    {"actionB",FieldType::Int,true,"Second Follow Action code",{},0,9},
    {"jumpB",FieldType::Int,false,"Second action's Jump scene identity",{},1},
    {"chanceB",FieldType::Real,true,"Second action's chance in percent",{},0,100},
    {"linked",FieldType::Bool,false}, {"loops",FieldType::Int,false,"Linked loop count",{},1}};

template<Action action>
OpDescriptor edit(std::string_view name, std::string_view summary, std::span<const Field> fields) {
    return {name,summary,Scope::Edit,EngineImpact::GraphRebuild,fields,false,false,
        apply<action>,capture<action>,""};
}

const OpDescriptor descriptors[]{
    edit<Action::Create>("scene.create","Create a Session scene",sceneCreateFields),
    edit<Action::Delete>("scene.delete","Delete a Session scene and its clips",sceneIdFields),
    edit<Action::Reorder>("scene.reorder","Set the complete Session scene order",sceneOrderFields),
    edit<Action::Rename>("scene.rename","Set a scene name",sceneNameFields),
    edit<Action::Tempo>("scene.setTempo","Set a scene's stored tempo",sceneTempoFields),
    edit<Action::Signature>("scene.setSignature","Set a scene's stored signature",signatureFields),
    edit<Action::Enable>("scene.setEnabled","Set scene tempo or signature enable flags",sceneEnableFields),
    edit<Action::Assign>("session.setSlotClip","Move a same-track clip into a Session slot",slotAssignFields),
    edit<Action::Clear>("session.clearSlot","Delete the clip in a Session slot",slotAddressFields),
    edit<Action::Stop>("session.setStopButton","Set a Session slot Stop button",stopButtonFields),
    edit<Action::Mode>("session.setLaunchMode","Set a clip's launch mode",launchModeFields),
    edit<Action::Quantization>("session.setLaunchQuant","Set a clip's launch quantization",launchQuantFields),
    edit<Action::Settings>("session.setLaunchSettings","Set a clip's Legato, velocity, Fade or RAM options",launchSettingsFields),
    edit<Action::Follow>("session.setFollowAction","Set a clip or scene Follow Action",followFields),
};
} // namespace

std::span<const OpDescriptor> sessionEditOps() { return descriptors; }
} // namespace adi
