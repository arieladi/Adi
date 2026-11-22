// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/controller_maps.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cmath>
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
    const bool keyboard = protocol == "key";
    if(protocol!="midi" && protocol!="osc" && protocol!="mackie" && protocol!="hui" && !keyboard) {
        error="unknown controller protocol"; return false;
    }
    if(type!="cc" && type!="note" && type!="pb" && type!="nrpn" && type!="sysex" && type!="osc_path" && type!="key") {
        error="unknown controller message type"; return false;
    }
    const auto within=[&](const char* key,std::int64_t lo,std::int64_t hi,bool required) {
        return b.at(key).is_null() ? !required : b.at(key).get<std::int64_t>()>=lo && b.at(key).get<std::int64_t>()<=hi;
    };
    if (keyboard != (type == "key")) {
        error = "keyboard bindings require the key protocol and message type";
        return false;
    }
    const bool deviceNamed = !b.at("device_name").get_ref<const std::string&>().empty();
    if (keyboard) {
        const auto mode = b.at("mode").get<std::int64_t>();
        if (deviceNamed || !b.at("channel").is_null() || !b.at("osc_path").is_null() ||
            !within("msg_num", 1, std::numeric_limits<std::int32_t>::max(), true) ||
            (mode != static_cast<int>(ControllerMode::Absolute) && mode != static_cast<int>(ControllerMode::Toggle))) {
            error = "invalid project keyboard source";
            return false;
        }
    } else if (!deviceNamed ||
               !within("channel",1,16,type=="cc" || type=="note" || type=="pb" || type=="nrpn") ||
               !within("msg_num",0,type=="nrpn"?16383:127,type=="cc" || type=="note" || type=="nrpn")) {
        error = "invalid controller source";
        return false;
    }
    if(!within("mode",0,static_cast<int>(ControllerMode::LinearSignedBitInverted),true) ||
       !within("takeover",0,2,true) || !std::isfinite(b.at("range_min").get<double>()) ||
       !std::isfinite(b.at("range_max").get<double>())) {
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

std::optional<int> decodeRelativeControllerTicks(ControllerMode mode, int value) noexcept {
    if (value < 0 || value > 127) return std::nullopt;
    switch (mode) {
        case ControllerMode::Relative:
        case ControllerMode::LinearTwosComplement:
            return value < 64 ? value : value - 128;
        case ControllerMode::BinaryOffset:
        case ControllerMode::LinearBinaryOffset:
            return value - 64;
        case ControllerMode::SignedBit:
        case ControllerMode::LinearSignedBit:
            return value < 64 ? value : -(value - 64);
        case ControllerMode::SignedBitInverted:
        case ControllerMode::LinearSignedBitInverted:
            return value < 64 ? -value : value - 64;
        case ControllerMode::Absolute:
        case ControllerMode::Toggle:
            break;
    }
    return std::nullopt;
}

std::optional<double> decodeAbsoluteControllerValue(ControllerAbsoluteFormat format, int value) noexcept {
    if (value < 0) return std::nullopt;
    switch (format) {
        case ControllerAbsoluteFormat::SevenBit:
            if (value <= 127) return value / 127.0;
            break;
        case ControllerAbsoluteFormat::FourteenBit:
            if (value <= 16383) return value / 16383.0;
            break;
        case ControllerAbsoluteFormat::PitchBend:
            if (value <= 16383) {
                return value < 8192 ? value / 16384.0 : 0.5 + (value - 8192) / 16382.0;
            }
            break;
    }
    return std::nullopt;
}

bool controllerCcIsDown(int value) noexcept {
    return value >= 64 && value <= 127;
}

bool controllerRangeActive(double value, double begin, double end) noexcept {
    if (!std::isfinite(value) || !std::isfinite(begin) || !std::isfinite(end)) return false;
    if (begin <= end) return value >= begin && value <= end;
    return value < end || value > begin;
}

ControllerMode detectControllerMode(std::span<const int> values, ControllerLearnMotion motion) noexcept {
    if(values.empty()) return ControllerMode::Absolute;
    if (motion == ControllerLearnMotion::SlowLeftTurn) {
        for (const auto mode : {ControllerMode::Relative, ControllerMode::BinaryOffset,
                                ControllerMode::SignedBit, ControllerMode::SignedBitInverted}) {
            bool moved = false;
            const bool consistent = std::all_of(values.begin(), values.end(), [&](int value) {
                const auto ticks = decodeRelativeControllerTicks(mode, value);
                moved = moved || ticks == -1;
                return ticks == -1 || ticks == 0;
            });
            if (consistent && moved) return mode;
        }
        return ControllerMode::Absolute;
    }
    bool twos=true,offset=true,movedTwos=false,movedOffset=false;
    for(int v:values) {
        twos=twos && (v==0 || v==1 || v==127); movedTwos=movedTwos || v==1 || v==127;
        offset=offset && v>=63 && v<=65; movedOffset=movedOffset || v==63 || v==65;
    }
    if (twos && movedTwos) return ControllerMode::Relative;
    if (offset && movedOffset) return ControllerMode::BinaryOffset;
    return ControllerMode::Absolute;
}
std::optional<OpRequest> learnControllerTarget(std::int64_t id,const std::string& kind,
    std::int64_t targetId,const std::string& param,const ControllerCc& source,
    std::span<const int> values,const ControllerPolicy& policy,std::string& error) {
    error.clear();
    if(!policy.remoteEnabled){error="MIDI port is not enabled for Remote";return std::nullopt;}
    if(policy.focusDial && *policy.focusDial==source){error="Focus Dial is reserved";return std::nullopt;}
    if(id<=0 || !validSource(source) || values.empty() ||
        std::any_of(values.begin(),values.end(),[](int value){return value<0 || value>127;})) {
        error="invalid Learn CC or binding id";return std::nullopt;
    }
    if((kind!="track" && kind!="device" && kind!="clip" && kind!="routing" && kind!="project") || targetId<=0 || param.empty()) {
        error="invalid controller target address";return std::nullopt;
    }
    if(policy.takeover!=ControllerTakeover::Jump && policy.takeover!=ControllerTakeover::Pickup && policy.takeover!=ControllerTakeover::Scale) {
        error="invalid controller takeover policy";return std::nullopt;
    }
    const auto mode=detectControllerMode(values,policy.learnMotion);
    Payload binding={{"device_name",source.port},{"protocol","midi"},{"channel",source.channel},
        {"msg_type","cc"},{"msg_num",source.number},{"osc_path",nullptr},
        {"target_kind",kind},{"target_id",targetId},{"target_param",param},
        {"mode",static_cast<int>(mode)},
        {"takeover",static_cast<int>(mode==ControllerMode::Absolute?policy.takeover:ControllerTakeover::Jump)},
        {"range_min",0.0},{"range_max",1.0},{"enabled",true}};
    OpRequest op;op.opType="controller.bind";op.label="MIDI Learn";
    op.payload={{"id",id},{"binding",std::move(binding)}};return op;
}
std::optional<OpRequest> learnController(const Store& store,std::int64_t id,std::int64_t lane,
    const ControllerCc& source,std::span<const int> values,const ControllerPolicy& policy,std::string& error) {
    error.clear();
    SQLite::Statement target(store.db(),"SELECT owner_kind,owner_id,param_ref FROM automation_lanes WHERE id=?");target.bind(1,lane);
    if(!target.executeStep()){error="no automation lane to learn";return std::nullopt;}
    return learnControllerTarget(id,target.getColumn(0).getString(),target.getColumn(1).getInt64(),target.getColumn(2).getString(),source,values,policy,error);
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
