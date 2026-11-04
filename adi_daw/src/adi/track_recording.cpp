// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/track_recording.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <cmath>
#include <stdexcept>
namespace adi {
namespace {
constexpr const char* table="track_recording_options";
bool gridValid(std::string_view grid){return grid=="1/4"||grid=="1/8"||grid=="1/16"||grid=="1/32";}
std::int64_t trackId(const Payload& payload){return detail::signedInteger(payload.at("id"));}
void ensureTrack(SQLite::Database& db,std::int64_t id) {
    SQLite::Statement found(db,"SELECT 1 FROM tracks WHERE id=?");found.bind(1,id);
    if(!found.executeStep())throw std::runtime_error("recording options need an existing track");
}
bool inverse(OpContext& context,const Payload& payload,Payload& result,std::string& error) {
    try {
        const auto id=trackId(payload);ensureTrack(context.db,id);result={{"id",id}};
        const auto row=detail::captureRow(context.db,table,"track_id",id);if(row)result["row"]=*row;
        return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool restore(OpContext& context,const Payload& payload,std::string& error) {
    try {
        const auto id=trackId(payload);ensureTrack(context.db,id);
        if(!payload.contains("row")||payload.at("row").is_null()) {
            SQLite::Statement erase(context.db,"DELETE FROM track_recording_options WHERE track_id=?");erase.bind(1,id);erase.exec();return true;
        }
        const auto& row=payload.at("row");if(detail::signedInteger(row.at("track_id"))!=id)throw std::runtime_error("recording options identity differs");
        const bool exists=detail::captureRow(context.db,table,"track_id",id).has_value();
        detail::restoreRow(context.db,table,row,exists?"track_id":"");return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool update(OpContext& context,const Payload& payload,std::string& error,int field) {
    try {
        const auto id=trackId(payload);ensureTrack(context.db,id);
        // Validate before creating even the optional defaults row.
        std::optional<std::string> grid;
        if(field<2 && payload.contains("grid")&&!payload.at("grid").is_null()) {
            grid=payload.at("grid").get<std::string>();if(!gridValid(*grid))throw std::runtime_error("quantize grid must be 1/4, 1/8, 1/16 or 1/32");
        }
        std::optional<double> forgiveness;
        if(field==1 && payload.contains("forgivenessMs")&&!payload.at("forgivenessMs").is_null()) {
            forgiveness=payload.at("forgivenessMs").get<double>();
            if(!std::isfinite(*forgiveness)||*forgiveness<0||*forgiveness>1000)throw std::runtime_error("Play-Q forgiveness must be 0 through 1000 ms");
        }
        SQLite::Statement add(context.db,"INSERT OR IGNORE INTO track_recording_options(track_id) VALUES(?)");add.bind(1,id);add.exec();
        const auto& on=payload.at("on");if(!on.is_boolean())throw std::runtime_error("recording switch must be boolean");
        const char* column=field==0?"record_enabled":field==1?"play_enabled":"keep_monitoring_latency";
        SQLite::Statement toggle(context.db,std::string("UPDATE track_recording_options SET ")+column+"=? WHERE track_id=?");
        toggle.bind(1,on.get<bool>()?1:0);toggle.bind(2,id);toggle.exec();
        if(grid) {
            SQLite::Statement value(context.db,std::string("UPDATE track_recording_options SET ")+(field==0?"record_grid":"play_grid")+"=? WHERE track_id=?");
            value.bind(1,*grid);value.bind(2,id);value.exec();
        }
        if(forgiveness) {
            SQLite::Statement value(context.db,"UPDATE track_recording_options SET play_forgiveness_ms=? WHERE track_id=?");value.bind(1,*forgiveness);value.bind(2,id);value.exec();
        }
        return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool record(OpContext& context,const Payload& payload,std::string& error){return update(context,payload,error,0);}
bool play(OpContext& context,const Payload& payload,std::string& error){return update(context,payload,error,1);}
bool monitor(OpContext& context,const Payload& payload,std::string& error){return update(context,payload,error,2);}
constexpr std::array<Field,3> recordFields={{{"id",FieldType::Int},{"on",FieldType::Bool},{"grid",FieldType::Text,false}}};
constexpr std::array<Field,4> playFields={{{"id",FieldType::Int},{"on",FieldType::Bool},{"grid",FieldType::Text,false},{"forgivenessMs",FieldType::Real,false}}};
constexpr std::array<Field,2> monitorFields={{{"id",FieldType::Int},{"on",FieldType::Bool}}};
constexpr std::array<Field,2> restoreFields={{{"id",FieldType::Int},{"row",FieldType::Object,false}}};
const std::array<OpDescriptor,4> ops={{
 {"track.setRecordQuantize","Set a track's record quantization",Scope::Edit,EngineImpact::Snapshot,recordFields,false,false,record,inverse,"track.restoreRecording"},
 {"track.setPlayQuantize","Set a track's live quantization",Scope::Edit,EngineImpact::Snapshot,playFields,false,false,play,inverse,"track.restoreRecording"},
 {"track.setKeepMonitoringLatency","Keep monitoring latency in this track's recording",Scope::Edit,EngineImpact::Snapshot,monitorFields,false,false,monitor,inverse,"track.restoreRecording"},
 {"track.restoreRecording","Restore captured track recording options",Scope::Edit,EngineImpact::Snapshot,restoreFields,false,false,restore,inverse,{}}
}};
}
std::span<const OpDescriptor> trackRecordingOps(){return ops;}
std::map<std::int64_t,TrackRecordingOptions> readTrackRecordingOptions(const Store& store) {
    auto& db=const_cast<SQLite::Database&>(store.db());std::map<std::int64_t,TrackRecordingOptions> result;
    if(db.execAndGet("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='track_recording_options'").getInt()==0)return result;
    SQLite::Statement rows(db,"SELECT track_id,record_enabled,record_grid,play_enabled,play_grid,play_forgiveness_ms,keep_monitoring_latency FROM track_recording_options ORDER BY track_id");
    while(rows.executeStep()) {
        TrackRecordingOptions options;options.recordEnabled=rows.getColumn(1).getInt()!=0;options.recordGrid=rows.getColumn(2).getString();
        options.playEnabled=rows.getColumn(3).getInt()!=0;options.playGrid=rows.getColumn(4).getString();options.playForgivenessMs=rows.getColumn(5).getDouble();
        options.keepMonitoringLatency=rows.getColumn(6).getInt()!=0;result.emplace(rows.getColumn(0).getInt64(),std::move(options));
    }
    return result;
}
std::vector<OpRequest> trackRecordingDefaults(std::int64_t id,std::string_view choice,bool keep) {
    if(choice!="off"&&!gridValid(choice))throw std::runtime_error("unknown record quantize default");
    std::vector<OpRequest> requests;
    OpRequest record;record.opType="track.setRecordQuantize";record.payload={{"id",id},{"on",choice!="off"}};
    if(choice!="off")record.payload["grid"]=choice;
    record.label="Add track";record.targetKind="track";record.targetId=id;requests.push_back(std::move(record));
    OpRequest monitor;monitor.opType="track.setKeepMonitoringLatency";monitor.payload={{"id",id},{"on",keep}};
    monitor.label="Add track";monitor.targetKind="track";monitor.targetId=id;requests.push_back(std::move(monitor));return requests;
}
}
