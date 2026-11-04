// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/split_pan_ops.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>

namespace adi {
namespace {
constexpr const char* columns[]={"mode","pan_left","pan_right"};
constexpr const char* keys[]={"mode","pan","pan"};
bool target(OpContext& context,const Payload& payload,std::int64_t& id,std::string& error) {
    id=payload.at("id").get<std::int64_t>();
    SQLite::Statement st(context.db,"SELECT 1 FROM mixer_strip WHERE track_id=?");st.bind(1,id);
    if(!st.executeStep()){error="no such mixer strip";return false;}return true;
}
template<std::size_t Index>
bool apply(OpContext& context,const Payload& payload,std::string& error) {
    try {
        std::int64_t id=0;if(!target(context,payload,id,error))return false;
        if(payload.value("_erase",false)) {
            SQLite::Statement st(context.db,"DELETE FROM mixer_pan_settings WHERE track_id=?");st.bind(1,id);st.exec();return true;
        }
        const auto& value=payload.at(keys[Index]);
        if constexpr(Index==0) {
            if(!value.is_string() || (!payload.value("_restore",false) && value!="stereo" && value!="split")) {
                error="pan mode must be stereo or split";return false;
            }
        } else {
            if(!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < -1 || value.get<double>() > 1) {
                error="split pan must be finite in -1..1";return false;
            }
        }
        SQLite::Statement insert(context.db,"INSERT OR IGNORE INTO mixer_pan_settings(track_id) VALUES(?)");insert.bind(1,id);insert.exec();
        SQLite::Statement update(context.db,std::string("UPDATE mixer_pan_settings SET ")+columns[Index]+"=? WHERE track_id=?");
        if constexpr(Index==0)update.bind(1,value.get<std::string>());
        else update.bind(1,value.get<double>());
        update.bind(2,id);update.exec();return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
template<std::size_t Index>
bool inverse(OpContext& context,const Payload& payload,Payload& undo,std::string& error) {
    try {
        std::int64_t id=0;if(!target(context,payload,id,error))return false;
        SQLite::Statement st(context.db,std::string("SELECT ")+columns[Index]+" FROM mixer_pan_settings WHERE track_id=?");st.bind(1,id);
        undo={{"id",id}};
        if(st.executeStep()) {
            if constexpr(Index==0){undo[keys[Index]]=st.getColumn(0).getString();undo["_restore"]=true;}
            else undo[keys[Index]]=st.getColumn(0).getDouble();
        } else {
            if constexpr(Index==0)undo[keys[Index]]="stereo";
            else undo[keys[Index]]=Index==1?-1.0:1.0;
            undo["_erase"]=true;
        }
        return true;
    } catch(const std::exception& exception){error=exception.what();return false;}
}
constexpr std::array<Field,4> modeFields={{{"id",FieldType::Int},{"mode",FieldType::Text},
                                         {"_erase",FieldType::Bool,false},{"_restore",FieldType::Bool,false}}};
constexpr std::array<Field,3> panFields={{{"id",FieldType::Int},{"pan",FieldType::Real},{"_erase",FieldType::Bool,false}}};
const std::array<OpDescriptor,3> descriptors={{
    {"mixer.setPanMode","Choose stereo or split stereo pan",Scope::Edit,EngineImpact::Snapshot,modeFields,false,false,apply<0>,inverse<0>,""},
    {"mixer.setPanLeft","Pan the left source of a stereo strip",Scope::Edit,EngineImpact::Snapshot,panFields,true,false,apply<1>,inverse<1>,""},
    {"mixer.setPanRight","Pan the right source of a stereo strip",Scope::Edit,EngineImpact::Snapshot,panFields,true,false,apply<2>,inverse<2>,""}
}};
}
std::span<const OpDescriptor> splitPanOps(){return descriptors;}
} // namespace adi
