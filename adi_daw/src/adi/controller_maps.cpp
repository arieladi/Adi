// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/controller_maps.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <limits>

namespace adi {
namespace {
constexpr std::array<Field,14> fields{{
    {"device_name",FieldType::Text}, {"protocol",FieldType::Text},
    {"channel",FieldType::Int,false}, {"msg_type",FieldType::Text},
    {"msg_num",FieldType::Int,false}, {"osc_path",FieldType::Text,false},
    {"target_kind",FieldType::Text}, {"target_id",FieldType::Int,false},
    {"target_param",FieldType::Text}, {"mode",FieldType::Int},
    {"takeover",FieldType::Int}, {"range_min",FieldType::Real},
    {"range_max",FieldType::Real}, {"enabled",FieldType::Bool}
}};
constexpr const char* columns = "device_name,protocol,channel,msg_type,msg_num,osc_path,"
    "target_kind,target_id,target_param,mode,takeover,range_min,range_max,enabled";

Payload row(SQLite::Statement& st,int offset=0) {
    Payload result=Payload::object();
    for(std::size_t i=0;i<fields.size();++i) {
        const auto col=st.getColumn(offset+static_cast<int>(i));
        const std::string key(fields[i].key);
        if(col.isNull()) result[key]=nullptr;
        else if(fields[i].type==FieldType::Text) result[key]=col.getString();
        else if(fields[i].type==FieldType::Real) result[key]=col.getDouble();
        else if(fields[i].type==FieldType::Bool) result[key]=col.getInt()!=0;
        else result[key]=col.getInt64();
    }
    return result;
}
bool validSource(const ControllerCc& cc) {
    return !cc.port.empty() && cc.channel>=1 && cc.channel<=16 && cc.number>=0 && cc.number<=127;
}
bool matches(const Payload& b,const ControllerCc& cc) {
    return b.value("protocol",std::string{})=="midi" && b.value("msg_type",std::string{})=="cc" &&
        b.value("device_name",std::string{})==cc.port && b.at("channel")==cc.channel && b.at("msg_num")==cc.number;
}
bool validateBinding(OpContext& c,const Payload& b,std::string& error) {
    OpDescriptor shape; shape.name="controller binding"; shape.fields=fields;
    const auto issues=validateSubmission(shape,b);
    if(!issues.empty()) { error=issues[0].key+": "+issues[0].problem; return false; }
    for(const auto& field:fields) if(!b.contains(std::string(field.key))) {
        error="binding must name every column, including explicit nulls"; return false;
    }
    const auto protocol=b.at("protocol").get<std::string>();
    const auto type=b.at("msg_type").get<std::string>();
    if(protocol!="midi" && protocol!="osc" && protocol!="mackie" && protocol!="hui") {
        error="unknown controller protocol"; return false;
    }
    if(type!="cc" && type!="note" && type!="pb" && type!="nrpn" && type!="sysex" && type!="osc_path") {
        error="unknown controller message type"; return false;
    }
    const auto within=[&](const char* key,std::int64_t lo,std::int64_t hi,bool required) {
        return b.at(key).is_null() ? !required : b.at(key).get<std::int64_t>()>=lo && b.at(key).get<std::int64_t>()<=hi;
    };
    if(b.at("device_name").get<std::string>().empty() ||
       !within("channel",1,16,type=="cc" || type=="note" || type=="pb" || type=="nrpn") ||
       !within("msg_num",0,type=="nrpn"?16383:127,type=="cc" || type=="note" || type=="nrpn") ||
       !within("mode",0,2,true) || !within("takeover",0,2,true) ||
       b.at("range_min").get<double>()>b.at("range_max").get<double>()) {
        error="invalid controller source, mode, takeover or range"; return false;
    }
    if((protocol=="osc") != (type=="osc_path") ||
       (type=="osc_path" && (b.at("osc_path").is_null() || b.at("osc_path").get<std::string>().empty()))) {
        error="OSC bindings need an OSC path"; return false;
    }
    const auto kind=b.at("target_kind").get<std::string>();
    const char* table=kind=="track"?"tracks":kind=="device"?"devices":kind=="clip"?"clips":kind=="routing"?"routing":nullptr;
    if((!table && kind!="project") || b.at("target_param").get<std::string>().empty()) {
        error="invalid controller target"; return false;
    }
    if(table) {
        if(!within("target_id",1,std::numeric_limits<std::int64_t>::max(),true)) { error="target needs a positive id"; return false; }
        SQLite::Statement target(c.db,std::string("SELECT 1 FROM ")+table+" WHERE id=?");
        target.bind(1,b.at("target_id").get<std::int64_t>());
        if(!target.executeStep()) { error="controller target does not exist"; return false; }
    }
    return true;
}
}

ControllerMode detectControllerMode(std::span<const int> values) noexcept {
    if(values.empty()) return ControllerMode::Absolute;
    bool twos=true,offset=true,movedTwos=false,movedOffset=false;
    for(int v:values) {
        twos=twos && (v==0 || v==1 || v==127); movedTwos=movedTwos || v==1 || v==127;
        offset=offset && v>=63 && v<=65; movedOffset=movedOffset || v==63 || v==65;
    }
    return (twos && movedTwos) || (offset && movedOffset) ? ControllerMode::Relative : ControllerMode::Absolute;
}
std::optional<OpRequest> learnController(const Store& store,std::int64_t id,std::int64_t lane,
    const ControllerCc& source,std::span<const int> values,const ControllerPolicy& policy,std::string& error) {
    SQLite::Statement target(store.db(),"SELECT owner_kind,owner_id,param_ref FROM automation_lanes WHERE id=?");
    target.bind(1,lane);
    if(!target.executeStep()) { error="no automation lane to learn"; return std::nullopt; }
    return learnControllerTarget(id,target.getColumn(0).getString(),target.getColumn(1).getInt64(),target.getColumn(2).getString(),source,values,policy,error);
}
std::optional<OpRequest> learnControllerTarget(std::int64_t id,const std::string& kind,
    std::int64_t targetId,const std::string& param,const ControllerCc& source,std::span<const int> values,
    const ControllerPolicy& policy,std::string& error) {
    error.clear();
    if(!policy.remoteEnabled) { error="MIDI port is not enabled for Remote"; return std::nullopt; }
    if(policy.focusDial && *policy.focusDial==source) { error="Focus Dial is reserved"; return std::nullopt; }
    if(id<=0 || !validSource(source) || values.empty() ||
        std::any_of(values.begin(),values.end(),[](int v){return v<0 || v>127;})) {
        error="invalid Learn CC or binding id"; return std::nullopt;
    }
    const auto mode=detectControllerMode(values);
    Payload binding={{"device_name",source.port},{"protocol","midi"},{"channel",source.channel},
        {"msg_type","cc"},{"msg_num",source.number},{"osc_path",nullptr},
        {"target_kind",kind},{"target_id",targetId},
        {"target_param",param},{"mode",static_cast<int>(mode)},
        {"takeover",static_cast<int>(mode==ControllerMode::Absolute?policy.takeover:ControllerTakeover::Jump)},
        {"range_min",0.0},{"range_max",1.0},{"enabled",true}};
    OpRequest op; op.opType="controller.bind"; op.label="MIDI Learn";
    op.payload={{"id",id},{"binding",std::move(binding)}};
    return op;
}
std::vector<ControllerBinding> readControllerBindings(const Store& store) {
    SQLite::Statement st(store.db(),std::string("SELECT id,")+columns+" FROM controller_maps ORDER BY id");
    std::vector<ControllerBinding> out;
    while(st.executeStep()) out.push_back({st.getColumn(0).getInt64(),row(st,1)});
    return out;
}
bool controllerBindingShadowed(const ControllerBinding& b,const ControllerPolicy& policy) {
    return policy.focusDial && matches(b.binding,*policy.focusDial);
}
ControllerDispatch dispatchController(const ControllerCc& cc,int value,const ControllerPolicy& policy,
    std::span<const ControllerBinding> bindings,const std::function<void(int)>& focus,
    const std::function<void(const ControllerBinding&,int)>& bound) {
    if(!validSource(cc) || value<0 || value>127) return ControllerDispatch::Ignored;
    if(policy.focusDial && *policy.focusDial==cc) {
        if(focus) focus(value);
        return ControllerDispatch::FocusDial;
    }
    if(!policy.remoteEnabled) return ControllerDispatch::Ignored;
    bool fired=false;
    for(const auto& b:bindings) if(b.binding.value("enabled",false) && matches(b.binding,cc) && !controllerBindingShadowed(b,policy)) {
        if(bound) bound(b,value);
        fired=true;
    }
    return fired?ControllerDispatch::Binding:ControllerDispatch::Ignored;
}

bool controllerBindInverse(OpContext& c,const Payload& p,Payload& inverse,std::string& error) {
    try {
        const auto id=p.at("id").get<std::int64_t>();
        if(id<=0) { error="binding id must be positive"; return false; }
        SQLite::Statement st(c.db,std::string("SELECT ")+columns+" FROM controller_maps WHERE id=?");
        st.bind(1,id);
        inverse={{"id",id},{"binding",st.executeStep()?row(st):Payload(nullptr)}};
        return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
bool controllerBindApply(OpContext& c,const Payload& p,std::string& error) {
    try {
        const auto id=p.at("id").get<std::int64_t>();
        if(id<=0 || !p.contains("binding")) { error="bind needs a positive id and explicit binding (or null)"; return false; }
        const auto& b=p.at("binding");
        if(b.is_null()) {
            SQLite::Statement del(c.db,"DELETE FROM controller_maps WHERE id=?"); del.bind(1,id); del.exec(); return true;
        }
        if(!validateBinding(c,b,error)) return false;
        SQLite::Statement st(c.db,std::string("INSERT INTO controller_maps(id,")+columns+") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(id) DO UPDATE SET device_name=excluded.device_name,protocol=excluded.protocol,channel=excluded.channel,"
            "msg_type=excluded.msg_type,msg_num=excluded.msg_num,osc_path=excluded.osc_path,target_kind=excluded.target_kind,"
            "target_id=excluded.target_id,target_param=excluded.target_param,mode=excluded.mode,takeover=excluded.takeover,"
            "range_min=excluded.range_min,range_max=excluded.range_max,enabled=excluded.enabled");
        st.bind(1,id);
        for(std::size_t i=0;i<fields.size();++i) {
            const int index=static_cast<int>(i)+2; const auto& v=b.at(std::string(fields[i].key));
            if(v.is_null()) st.bind(index);
            else if(fields[i].type==FieldType::Text) st.bind(index,v.get<std::string>());
            else if(fields[i].type==FieldType::Real) st.bind(index,v.get<double>());
            else if(fields[i].type==FieldType::Bool) st.bind(index,v.get<bool>()?1:0);
            else st.bind(index,v.get<std::int64_t>());
        }
        st.exec(); return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
bool controllerUnbindInverse(OpContext& c,const Payload& p,Payload& inverse,std::string& error) {
    if(!controllerBindInverse(c,p,inverse,error)) return false;
    if(inverse.at("binding").is_null()) { error="no such controller binding"; return false; }
    return true;
}
bool controllerUnbindApply(OpContext& c,const Payload& p,std::string& error) {
    try {
        SQLite::Statement st(c.db,"DELETE FROM controller_maps WHERE id=?"); st.bind(1,p.at("id").get<std::int64_t>());
        if(st.exec()==0) { error="no such controller binding"; return false; }
        return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
}
