// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/automation_lanes.hpp"
#include "adi/blob.hpp"
#include "adi/automation_range_rows.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <climits>
#include <cstring>
#include <stdexcept>

namespace adi::automation {
namespace {
constexpr std::array columns{"id", "owner_kind", "owner_id", "param_ref", "param_name", "time_base",
    "value_domain", "unit", "default_value", "min_value", "max_value", "enabled", "visible", "height"};
std::int64_t number(const Payload& p) {
    if (!p.is_number_integer() || (p.is_number_unsigned() && p.get<std::uint64_t>() > INT64_MAX))
        throw std::runtime_error("automation: expected int64");
    return p.get<std::int64_t>();
}
void ownerExists(OpContext& c, const Payload& row) {
    const auto kind = row.at("owner_kind").get<std::string>();
    const char* table = kind=="track" ? "tracks" : kind=="device" ? "devices" : kind=="clip" ? "clips" :
        kind=="project" ? "project" : kind=="routing" ? "routing" : nullptr;
    if (!table) throw std::runtime_error("automation: unknown owner kind");
    SQLite::Statement q(c.db,std::string("SELECT 1 FROM ")+table+" WHERE id=?");
    q.bind(1,number(row.at("owner_id")));
    if (!q.executeStep()) throw std::runtime_error("automation: missing owner");
}
void validateRow(OpContext& c, const Payload& row) {
    if (!row.is_object() || row.size()!=columns.size()) throw std::runtime_error("automation: complete lane row required");
    for (const auto* col:columns) if (!row.contains(col)) throw std::runtime_error("automation: missing lane column");
    if (number(row.at("id"))<=0 || number(row.at("owner_id"))<=0 ||
        (number(row.at("time_base"))!=0 && number(row.at("time_base"))!=1) ||
        number(row.at("height"))<0) throw std::runtime_error("automation: invalid lane IDs, unit or height");
    for (const auto* col:{"param_ref","param_name","unit","value_domain"})
        if (!row.at(col).is_string()) throw std::runtime_error("automation: lane text required");
    if (row.at("param_ref").get_ref<const std::string&>().empty()) throw std::runtime_error("automation: empty parameter");
    const auto& domain=row.at("value_domain").get_ref<const std::string&>();
    if (domain!="normalized" && domain!="real" && domain!="enum") throw std::runtime_error("automation: unknown value domain");
    for (const auto* col:{"enabled","visible"})
        if (number(row.at(col))!=0 && number(row.at(col))!=1) throw std::runtime_error("automation: invalid lane switch");
    for (const auto* col:{"default_value","min_value","max_value"}) {
        const auto& v=row.at(col);
        if (v.is_null() && std::string_view(col)!="default_value") continue;
        if (!v.is_number() || !std::isfinite(v.get<double>())) throw std::runtime_error("automation: finite lane values required");
    }
    const double value=row.at("default_value").get<double>();
    if ((!row.at("min_value").is_null() && value<row.at("min_value").get<double>()) ||
        (!row.at("max_value").is_null() && value>row.at("max_value").get<double>()))
        throw std::runtime_error("automation: default outside lane bounds");
    ownerExists(c,row);
}
Payload rowAt(OpContext& c, std::int64_t id) {
    SQLite::Statement q(c.db,"SELECT * FROM automation_lanes WHERE id=?"); q.bind(1,id);
    if (!q.executeStep()) return nullptr;
    Payload row=Payload::object(); int index=0;
    for (const auto* col:columns) {
        const auto value=q.getColumn(index++);
        if (value.isNull()) row[col]=nullptr;
        else if (value.isText()) row[col]=value.getString();
        else if (value.isFloat()) row[col]=value.getDouble();
        else row[col]=value.getInt64();
    }
    return row;
}
void insertRow(OpContext& c,const Payload& row) {
    SQLite::Statement q(c.db,"INSERT INTO automation_lanes VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    int i=0;
    for (const auto* col:columns) {
        const auto& v=row.at(col); ++i;
        if (v.is_null()) q.bind(i);
        else if (v.is_string()) q.bind(i,v.get_ref<const std::string&>());
        else if (v.is_number_float()) q.bind(i,v.get<double>());
        else q.bind(i,number(v));
    }
    q.exec();
}
bool capture(OpContext& c,const Payload& p,Payload& inv,std::string& err) {
    try {
        const auto id=number(p.at("id"));
        if (id<=0) throw std::runtime_error("automation: positive lane ID required");
        inv={{"id",id},{"state",{{"row",rowAt(c,id)},{"data",Payload::array()}}}};
        SQLite::Statement q(c.db,"SELECT id,clip_id,data FROM automation_data WHERE lane_id=? ORDER BY id"); q.bind(1,id);
        while (q.executeStep()) {
            const auto col=q.getColumn(2); std::vector<std::uint8_t> b(static_cast<std::size_t>(col.getBytes()));
            if (!b.empty()) std::memcpy(b.data(),col.getBlob(),b.size());
            inv["state"]["data"].push_back({{"id",q.getColumn(0).getInt64()},
                {"clip",q.getColumn(1).isNull()?Payload(nullptr):Payload(q.getColumn(1).getInt64())},{"bytes",Payload::binary(std::move(b))},
                {"ranges",range_rows::capture(c.db,q.getColumn(0).getInt64())}});
        }
        return true;
    } catch (const std::exception& e) { err=e.what(); return false; }
}
bool restore(OpContext& c,const Payload& p,std::string& err) {
    try {
        const auto id=number(p.at("id")); const auto& state=p.at("state"); const auto& row=state.at("row");
        if (!row.is_null() && number(row.at("id"))!=id) throw std::runtime_error("automation: restore ID mismatch");
        // Restoration preserves every stored scalar, even legacy metadata whose
        // domain/range a new creation would reject. SQLite still enforces types.
        SQLite::Statement remove(c.db,"DELETE FROM automation_lanes WHERE id=?"); remove.bind(1,id); remove.exec();
        if (row.is_null()) {
            if (!state.at("data").empty()) throw std::runtime_error("automation: absent lane with data");
            return true;
        }
        insertRow(c,row);
        for (const auto& data:state.at("data")) {
            SQLite::Statement q(c.db,"INSERT INTO automation_data(id,lane_id,clip_id,data) VALUES(?,?,?,?)");
            q.bind(1,number(data.at("id"))); q.bind(2,id);
            if (data.at("clip").is_null()) q.bind(3); else q.bind(3,number(data.at("clip")));
            const auto& b=data.at("bytes").get_binary();
            if (b.size()>INT_MAX) throw std::runtime_error("automation: stream too large");
            q.bind(4,b.empty()?static_cast<const void*>(""):b.data(),static_cast<int>(b.size())); q.exec();
            range_rows::replace(c.db,number(data.at("id")),data.at("clip"),data.value("ranges",Payload::array()));
        }
        return true;
    } catch (const std::exception& e) { err=e.what(); return false; }
}
bool create(OpContext& c,const Payload& p,std::string& err) {
    try {
        const auto& row=p.at("row"); validateRow(c,row);
        if (number(row.at("id"))!=number(p.at("id"))) throw std::runtime_error("automation: creation ID mismatch");
        insertRow(c,row); return true;
    } catch (const std::exception& e) { err=e.what(); return false; }
}
bool removeLane(OpContext& c,const Payload& p,std::string& err) {
    try {
        SQLite::Statement q(c.db,"DELETE FROM automation_lanes WHERE id=?"); q.bind(1,number(p.at("id")));
        if (q.exec()!=1) throw std::runtime_error("automation: missing lane");
        return true;
    } catch (const std::exception& e) { err=e.what(); return false; }
}
bool clear(OpContext& c,const Payload& p,std::string& err) {
    try {
        const auto id=number(p.at("id")); if (rowAt(c,id).is_null()) throw std::runtime_error("automation: missing lane");
        SQLite::Statement q(c.db,"SELECT id,data FROM automation_data WHERE lane_id=?"); q.bind(1,id);
        while(q.executeStep()) {
            const auto col=q.getColumn(1); const auto* b=static_cast<const std::byte*>(col.getBlob());
            const std::span<const std::byte> bytes(b,static_cast<std::size_t>(col.getBytes()));
            const StreamReader<AutomationPoint> reader(bytes,FourCC::Automation);
            if (!reader.ok()) throw std::runtime_error("automation: unreadable stream cannot be cleared");
            auto h=reader.header(); const auto tail=sizeof h+static_cast<std::size_t>(h.count)*h.rec_size; h.count=0;
            std::vector<std::byte> out(sizeof h); std::memcpy(out.data(),&h,sizeof h);
            out.insert(out.end(),bytes.begin()+static_cast<std::ptrdiff_t>(tail),bytes.end());
            SQLite::Statement update(c.db,"UPDATE automation_data SET data=? WHERE id=?");
            update.bind(1,out.data(),static_cast<int>(out.size())); update.bind(2,q.getColumn(0).getInt64()); update.exec();
            range_rows::replace(c.db,q.getColumn(0).getInt64(),nullptr,Payload::array());
        }
        return true;
    } catch(const std::exception& e) { err=e.what(); return false; }
}
template<int field> bool scalar(OpContext& c,const Payload& p,std::string& err) {
    try {
        const auto id=number(p.at("id")); auto row=rowAt(c,id);
        if (row.is_null()) throw std::runtime_error("automation: missing lane");
        const auto* col=field==0?"enabled":field==1?"visible":"default_value";
        const double value=field==2?p.at("value").get<double>(): (p.at("value").get<bool>()?1.0:0.0);
        if (!std::isfinite(value) || (field==2 && ((!row.at("min_value").is_null() && value<row.at("min_value").get<double>()) ||
            (!row.at("max_value").is_null() && value>row.at("max_value").get<double>()))))
            throw std::runtime_error("automation: default outside lane bounds");
        SQLite::Statement q(c.db,std::string("UPDATE automation_lanes SET ")+col+"=? WHERE id=?");
        if constexpr(field==2) q.bind(1,value); else q.bind(1,value==0?0:1);
        q.bind(2,id); q.exec(); return true;
    } catch(const std::exception& e) { err=e.what(); return false; }
}
constexpr Field idFields[]={{"id",FieldType::Int}};
constexpr Field rowFields[]={{"id",FieldType::Int},{"row",FieldType::Object}};
constexpr Field stateFields[]={{"id",FieldType::Int},{"state",FieldType::Object}};
constexpr Field boolFields[]={{"id",FieldType::Int},{"value",FieldType::Bool}};
constexpr Field realFields[]={{"id",FieldType::Int},{"value",FieldType::Real}};
const OpDescriptor ops[]={
    {"automation.createLane","Create an explicitly addressed lane",Scope::Edit,EngineImpact::Snapshot,rowFields,false,false,create,capture,"automation.restoreLane"},
    {"automation.deleteLane","Delete a lane and its streams",Scope::Edit,EngineImpact::Snapshot,idFields,false,false,removeLane,capture,"automation.restoreLane"},
    {"automation.clearLane","Clear all arrangement and clip points",Scope::Edit,EngineImpact::Snapshot,idFields,false,false,clear,capture,"automation.restoreLane"},
    {"automation.setLaneEnabled","Enable lane playback",Scope::Edit,EngineImpact::Snapshot,boolFields,false,false,scalar<0>,capture,"automation.restoreLane"},
    {"automation.setLaneVisible","Show a lane",Scope::Edit,EngineImpact::None,boolFields,false,false,scalar<1>,capture,"automation.restoreLane"},
    {"automation.setDefaultValue","Set the lane default",Scope::Edit,EngineImpact::Snapshot,realFields,false,false,scalar<2>,capture,"automation.restoreLane"},
    {"automation.restoreLane","Restore captured lane and streams",Scope::Edit,EngineImpact::Snapshot,stateFields,false,false,restore,capture,""},
};
OpRequest request(const char* name,std::int64_t id) { OpRequest r; r.opType=name; r.payload={{"id",id}}; return r; }
} // namespace
OpRequest createLane(const LaneSpec& s) {
    auto r=request("automation.createLane",s.id);
    r.payload["row"]={{"id",s.id},{"owner_kind",s.ownerKind},{"owner_id",s.ownerId},{"param_ref",s.paramRef},
        {"param_name",s.paramName},{"time_base",s.timeBase},{"value_domain",s.valueDomain},{"unit",s.unit},
        {"default_value",s.defaultValue},{"min_value",s.minValue?Payload(*s.minValue):Payload(nullptr)},
        {"max_value",s.maxValue?Payload(*s.maxValue):Payload(nullptr)},{"enabled",s.enabled?1:0},{"visible",s.visible?1:0},{"height",s.height}};
    return r;
}
OpRequest deleteLane(std::int64_t id) { return request("automation.deleteLane",id); }
OpRequest clearLane(std::int64_t id) { return request("automation.clearLane",id); }
OpRequest setLaneEnabled(std::int64_t id,bool value) { auto r=request("automation.setLaneEnabled",id); r.payload["value"]=value; return r; }
OpRequest setLaneVisible(std::int64_t id,bool value) { auto r=request("automation.setLaneVisible",id); r.payload["value"]=value; return r; }
OpRequest setDefaultValue(std::int64_t id,double value) { auto r=request("automation.setDefaultValue",id); r.payload["value"]=value; return r; }
std::span<const OpDescriptor> laneDescriptors() { return ops; }
} // namespace adi::automation
