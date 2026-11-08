// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/tuning_ops.hpp"
#include "adi/audio/io_audit.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
namespace adi::tunings {
namespace {
using engine::ClipScale;
using engine::TuningDefinition;
constexpr std::size_t maxDegrees=65536,maxText=65536,maxTotalText=8*1024*1024;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::int64_t integer(const Payload& value) {
    require(value.is_number_integer(),"expected an integer");
    if(value.is_number_unsigned())require(value.get<std::uint64_t>()<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),"integer exceeds signed storage");
    return value.get<std::int64_t>();
}
std::int64_t identity(const Payload& value){const auto id=integer(value);require(id>0,"identity must be positive");return id;}
double real(const Payload& value){require(value.is_number(),"expected a real number");const auto result=value.get<double>();require(std::isfinite(result),"expected a finite real number");return result;}
std::string text(const Payload& value){require(value.is_string(),"expected text");const auto& result=value.get_ref<const std::string&>();require(result.size()<=maxText,"text exceeds tuning budget");return result;}
void fields(const Payload& value,std::initializer_list<std::string_view> allowed) {
    require(value.is_object(),"expected an object");
    for(const auto& [key,item]:value.items()) {
        (void)item;bool found=false;for(const auto name:allowed)found=found || key==name;
        require(found,"unknown nested tuning/scale field");
    }
}
void validate(const ClipScale& scale,const std::optional<TuningDefinition>& definition) {
    std::string error;const auto prepared=engine::compileScaleContext(scale,definition,error);
    if(!prepared)throw std::runtime_error(error);
}
TuningDefinition parseDefinition(std::int64_t id,const Payload& value) {
    fields(value,{"name","source","period","description","degrees"});
    TuningDefinition result;result.id=id;result.name=text(value.at("name"));result.source=text(value.at("source"));
    result.description=text(value.at("description"));result.periodCents=real(value.at("period"));
    const auto& degrees=value.at("degrees");require(degrees.is_array() && !degrees.empty() && degrees.size()<=maxDegrees,"invalid tuning degree count");
    std::size_t bytes=result.name.size()+result.source.size()+result.description.size();
    for(const auto& row:degrees) {
        fields(row,{"id","cents","name"});auto name=text(row.at("name"));bytes+=name.size();require(bytes<=maxTotalText,"total tuning text exceeds budget");
        result.degrees.push_back({integer(row.at("id")),real(row.at("cents")),std::move(name)});
    }
    ClipScale scale;scale.tuningId=id;validate(scale,result);return result;
}
TuningDefinition readDefinition(SQLite::Database& db,std::int64_t id) {
    SQLite::Statement query(db,"SELECT name,source,period_cents,description FROM tuning_systems WHERE id=?");query.bind(1,id);
    require(query.executeStep(),"no such tuning");
    Payload value={{"name",query.getColumn(0).getString()},{"source",query.getColumn(1).getString()},
                   {"period",query.getColumn(2).getDouble()},{"description",query.getColumn(3).getString()},{"degrees",Payload::array()}};
    SQLite::Statement rows(db,"SELECT degree,cents,name FROM tuning_degrees WHERE tuning_id=? ORDER BY degree");rows.bind(1,id);
    while(rows.executeStep()) {
        require(value["degrees"].size()<maxDegrees,"stored tuning degree budget exceeded");
        value["degrees"].push_back({{"id",rows.getColumn(0).getInt64()},{"cents",rows.getColumn(1).getDouble()},{"name",rows.getColumn(2).getString()}});
    }
    return parseDefinition(id,value);
}
ClipScale parseScale(SQLite::Database& db,const Payload& value) {
    fields(value,{"root","scale","mask","tuning","degrees"});
    ClipScale result;result.root=integer(value.at("root"));result.id=text(value.at("scale"));result.mask=integer(value.at("mask"));
    // Even an ignored tuned-key mask must remain useful to older readers.
    require(result.mask>=0 && result.mask<=4095,"compatibility scale mask must have twelve bits");
    std::optional<TuningDefinition> definition;
    if(value.contains("tuning")) {
        result.tuningId=identity(value.at("tuning"));definition=readDefinition(db,*result.tuningId);
        const auto& degrees=value.at("degrees");require(degrees.is_array() && degrees.size()<=maxDegrees,"invalid selected degree count");
        for(const auto& degree:degrees)result.degrees.push_back(integer(degree));
    } else require(!value.contains("degrees"),"implicit scale cannot have tuning degrees");
    validate(result,definition);return result;
}
Payload readScale(SQLite::Database& db,std::int64_t id) {
    SQLite::Statement key(db,"SELECT root,scale_id,scale_mask FROM key_map WHERE id=?");key.bind(1,id);require(key.executeStep(),"no such key event");
    Payload value={{"root",key.getColumn(0).getInt64()},{"scale",key.getColumn(1).getString()},{"mask",key.getColumn(2).getInt64()}};
    SQLite::Statement tuning(db,"SELECT tuning_id FROM key_map_tunings WHERE key_map_id=?");tuning.bind(1,id);
    if(tuning.executeStep()) {
        value["tuning"]=tuning.getColumn(0).getInt64();value["degrees"]=Payload::array();
        SQLite::Statement degrees(db,"SELECT degree FROM key_map_degrees WHERE key_map_id=? ORDER BY degree");degrees.bind(1,id);
        while(degrees.executeStep()){require(value["degrees"].size()<maxDegrees,"selected degree budget exceeded");value["degrees"].push_back(degrees.getColumn(0).getInt64());}
    }
    (void)parseScale(db,value);return value;
}
void writeDegrees(SQLite::Database& db,const TuningDefinition& definition) {
    SQLite::Statement row(db,"INSERT INTO tuning_degrees(tuning_id,degree,cents,name) VALUES(?,?,?,?)");
    for(const auto& degree:definition.degrees){row.reset();row.bind(1,definition.id);row.bind(2,degree.id);row.bind(3,degree.cents);row.bind(4,degree.name);row.exec();}
}
void writeScale(SQLite::Database& db,std::int64_t id,const ClipScale& scale) {
    SQLite::Statement clear(db,"DELETE FROM key_map_tunings WHERE key_map_id=?");clear.bind(1,id);clear.exec();
    if(!scale.tuningId)return;
    SQLite::Statement tuning(db,"INSERT INTO key_map_tunings(key_map_id,tuning_id) VALUES(?,?)");tuning.bind(1,id);tuning.bind(2,*scale.tuningId);tuning.exec();
    SQLite::Statement degree(db,"INSERT INTO key_map_degrees(key_map_id,degree) VALUES(?,?)");
    for(const auto member:scale.degrees){degree.reset();degree.bind(1,id);degree.bind(2,member);degree.exec();}
}
template<class Function>bool guarded(std::string& error,Function function) {
    if(audio::onAudioThread){error="callback";return false;}
    try{function();error.clear();return true;}catch(const std::exception& exception){error=exception.what();return false;}
}
bool create(OpContext& context,const Payload& p,std::string& error) {
    return guarded(error,[&]{const auto definition=parseDefinition(identity(p.at("id")),p.at("definition"));
        SQLite::Statement row(context.db,"INSERT INTO tuning_systems(id,name,source,period_cents,description) VALUES(?,?,?,?,?)");
        row.bind(1,definition.id);row.bind(2,definition.name);row.bind(3,definition.source);row.bind(4,definition.periodCents);row.bind(5,definition.description);row.exec();writeDegrees(context.db,definition);});
}
bool inverseCreate(OpContext&,const Payload& p,Payload& inverse,std::string& error) {
    return guarded(error,[&]{inverse={{"id",identity(p.at("id"))}};});
}
bool erase(OpContext& context,const Payload& p,std::string& error) {
    return guarded(error,[&]{const auto id=identity(p.at("id"));
        SQLite::Statement used(context.db,"SELECT 1 FROM key_map_tunings WHERE tuning_id=? LIMIT 1");used.bind(1,id);require(!used.executeStep(),"tuning is used by a key event");
        SQLite::Statement row(context.db,"DELETE FROM tuning_systems WHERE id=?");row.bind(1,id);require(row.exec()==1,"no such tuning");});
}
bool inverseErase(OpContext& context,const Payload& p,Payload& inverse,std::string& error) {
    return guarded(error,[&]{const auto id=identity(p.at("id"));inverse={{"id",id},{"definition",definitionPayload(readDefinition(context.db,id))}};});
}
bool replace(OpContext& context,const Payload& p,std::string& error) {
    return guarded(error,[&]{const auto definition=parseDefinition(identity(p.at("id")),p.at("definition"));
        // Validate every dependent membership before replacing any rows.
        SQLite::Statement keys(context.db,"SELECT key_map_id FROM key_map_tunings WHERE tuning_id=? ORDER BY key_map_id");keys.bind(1,definition.id);
        SQLite::Statement members(context.db,"SELECT degree FROM key_map_degrees WHERE key_map_id=? ORDER BY degree");
        while(keys.executeStep()) {
            ClipScale scale;scale.tuningId=definition.id;members.reset();members.bind(1,keys.getColumn(0).getInt64());
            while(members.executeStep()){require(scale.degrees.size()<maxDegrees,"selected degree budget exceeded");scale.degrees.push_back(members.getColumn(0).getInt64());}
            validate(scale,definition);
        }
        SQLite::Statement row(context.db,"UPDATE tuning_systems SET name=?,source=?,period_cents=?,description=? WHERE id=?");
        row.bind(1,definition.name);row.bind(2,definition.source);row.bind(3,definition.periodCents);row.bind(4,definition.description);row.bind(5,definition.id);require(row.exec()==1,"no such tuning");
        SQLite::Statement clear(context.db,"DELETE FROM tuning_degrees WHERE tuning_id=?");clear.bind(1,definition.id);clear.exec();writeDegrees(context.db,definition);});
}
bool insertKey(OpContext& context,const Payload& p,std::string& error) {
    return guarded(error,[&]{const auto id=identity(p.at("id")),pos=integer(p.at("pos"));require(pos>=0,"key position must be nonnegative");const auto scale=parseScale(context.db,p.at("scale"));
        SQLite::Statement row(context.db,"INSERT INTO key_map(id,pos_ticks,root,scale_id,scale_mask) VALUES(?,?,?,?,?)");
        row.bind(1,id);row.bind(2,pos);row.bind(3,scale.root);row.bind(4,scale.id);row.bind(5,scale.mask);row.exec();writeScale(context.db,id,scale);});
}
bool removeKey(OpContext& context,const Payload& p,std::string& error) {
    return guarded(error,[&]{SQLite::Statement row(context.db,"DELETE FROM key_map WHERE id=?");row.bind(1,identity(p.at("id")));require(row.exec()==1,"no such key event");});
}
bool inverseRemoveKey(OpContext& context,const Payload& p,Payload& inverse,std::string& error) {
    return guarded(error,[&]{const auto id=identity(p.at("id"));const auto scale=readScale(context.db,id);
        SQLite::Statement row(context.db,"SELECT pos_ticks FROM key_map WHERE id=?");row.bind(1,id);require(row.executeStep(),"no such key event");inverse={{"id",id},{"pos",row.getColumn(0).getInt64()},{"scale",scale}};});
}
bool setScale(OpContext& context,const Payload& p,std::string& error) {
    return guarded(error,[&]{const auto id=identity(p.at("id"));const auto scale=parseScale(context.db,p.at("scale"));
        SQLite::Statement row(context.db,"UPDATE key_map SET root=?,scale_id=?,scale_mask=? WHERE id=?");row.bind(1,scale.root);row.bind(2,scale.id);row.bind(3,scale.mask);row.bind(4,id);require(row.exec()==1,"no such key event");writeScale(context.db,id,scale);});
}
bool inverseSetScale(OpContext& context,const Payload& p,Payload& inverse,std::string& error) {
    return guarded(error,[&]{const auto id=identity(p.at("id"));inverse={{"id",id},{"scale",readScale(context.db,id)}};});
}
constexpr Field fId[]={{"id",FieldType::Int}};
constexpr Field fDefinition[]={{"id",FieldType::Int},{"definition",FieldType::Object}};
constexpr Field fKey[]={{"id",FieldType::Int},{"pos",FieldType::Int},{"scale",FieldType::Object}};
constexpr Field fScale[]={{"id",FieldType::Int},{"scale",FieldType::Object}};
const OpDescriptor descriptors[]={
    {"tuning.create","Create an explicit tuning definition",Scope::Edit,EngineImpact::Snapshot,fDefinition,false,false,create,inverseCreate,"tuning.delete"},
    {"tuning.delete","Delete an unreferenced tuning",Scope::Edit,EngineImpact::Snapshot,fId,false,false,erase,inverseErase,"tuning.create"},
    {"tuning.replace","Replace a tuning without invalidating key memberships",Scope::Edit,EngineImpact::Snapshot,fDefinition,false,false,replace,inverseErase,""},
    {"project.insertKeyEvent","Insert an explicit key and scale change",Scope::Edit,EngineImpact::Snapshot,fKey,false,false,insertKey,inverseCreate,"project.removeKeyEvent"},
    {"project.removeKeyEvent","Remove a key and scale change",Scope::Edit,EngineImpact::Snapshot,fId,false,false,removeKey,inverseRemoveKey,"project.insertKeyEvent"},
    {"project.setKeyScale","Set a key event's scale and tuning membership",Scope::Edit,EngineImpact::Snapshot,fScale,false,false,setScale,inverseSetScale,""},
};
} // namespace
Payload definitionPayload(const engine::TuningDefinition& definition) {
    Payload result={{"name",definition.name},{"source",definition.source},{"period",definition.periodCents},{"description",definition.description},{"degrees",Payload::array()}};
    for(const auto& degree:definition.degrees)result["degrees"].push_back({{"id",degree.id},{"cents",degree.cents},{"name",degree.name}});
    return result;
}
Payload scalePayload(const engine::ClipScale& scale) {
    Payload result={{"root",scale.root},{"scale",scale.id},{"mask",scale.mask}};
    if(scale.tuningId){result["tuning"]=*scale.tuningId;result["degrees"]=scale.degrees;}return result;
}
std::span<const OpDescriptor> ops(){return descriptors;}
} // namespace adi::tunings
