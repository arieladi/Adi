// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scale_context.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
namespace adi::engine {
namespace {constexpr long double periodLimit=static_cast<long double>(std::int64_t{1}<<50);}
struct ScaleData {
    ClipScale scale;double period=1200;std::string name,source="implicit",description;
    std::vector<ScaleDegree> degrees;
    std::vector<std::size_t> all,members;
};
std::optional<MidiPitch> encodeMidiPitch(double cents,double maxError) noexcept {
    if(!std::isfinite(cents) || !std::isfinite(maxError) || maxError<0 || cents<-1200 || cents>13900)return {};
    const auto key=std::clamp(std::lround(cents/100),0l,127l);
    const auto offset=cents-static_cast<double>(key)*100;const auto stored=static_cast<float>(offset);
    const auto error=std::abs(static_cast<double>(stored)-offset);if(error>maxError)return {};
    return MidiPitch{static_cast<std::uint8_t>(key),stored,error};
}
std::optional<ScaleContext> compileScaleContext(const ClipScale& scale,const std::optional<TuningDefinition>& tuning,std::string& error) {
    try {
        if(audio::onAudioThread){error="callback";return {};}
        if(scale.root<0 || scale.root>11)throw std::runtime_error("scale root must be a 12-TET pitch class");
        auto data=std::make_shared<ScaleData>();data->scale=scale;
        if(data->scale.tuningId) {
            if(*data->scale.tuningId<=0 || !tuning || tuning->id!=*data->scale.tuningId ||
               (tuning->source!="edo" && tuning->source!="scala") || !std::isfinite(tuning->periodCents) || tuning->periodCents<=0 ||
               tuning->degrees.empty() || tuning->degrees.size()>65536)throw std::runtime_error("missing or invalid tuning definition");
            data->period=tuning->periodCents;data->name=tuning->name;data->source=tuning->source;data->description=tuning->description;
            std::set<std::int64_t> selected;
            for(const auto id:data->scale.degrees)if(id<0 || !selected.insert(id).second)throw std::runtime_error("invalid/duplicate selected tuning degree");
            std::int64_t prior=-1;double previous=-1;
            for(const auto& degree:tuning->degrees) {
                if(degree.id<=prior || !std::isfinite(degree.cents) || degree.cents<0 || degree.cents>=data->period || degree.cents<=previous ||
                   (data->degrees.empty() && (degree.id!=0 || degree.cents!=0)))throw std::runtime_error("tuning degrees must begin at unison and rise within their period");
                ScaleDegree prepared;prepared.id=degree.id;prepared.cents=degree.cents;prepared.name=degree.name;prepared.inScale=selected.contains(degree.id);
                if(prepared.inScale)selected.erase(degree.id);
                data->degrees.push_back(std::move(prepared));prior=degree.id;previous=degree.cents;
            }
            if(!selected.empty())throw std::runtime_error("scale references a degree its tuning does not contain");
        } else {
            if(tuning || !data->scale.degrees.empty() || data->scale.mask<0 || data->scale.mask>4095)throw std::runtime_error("invalid implicit 12-TET scale");
            data->name="12-TET";
            constexpr const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            for(std::int64_t id=0;id<12;++id) {
                ScaleDegree degree;degree.id=id;degree.cents=static_cast<double>(id)*100;degree.name=names[static_cast<std::size_t>((data->scale.root+id)%12)];
                degree.inScale=(data->scale.mask&(std::int64_t{1}<<id))!=0;data->degrees.push_back(std::move(degree));
            }
        }
        data->all.resize(data->degrees.size());std::iota(data->all.begin(),data->all.end(),std::size_t{0});
        for(std::size_t index=0;index<data->degrees.size();++index)if(data->degrees[index].inScale)data->members.push_back(index);
        error.clear();return ScaleContext(std::move(data));
    }catch(const std::exception& exception){error=exception.what();return {};}
}
const ClipScale* ScaleContext::scale()const noexcept{return data_?&data_->scale:nullptr;}
double ScaleContext::periodCents()const noexcept{return data_?data_->period:0;}
std::string_view ScaleContext::tuningName()const noexcept{return data_?std::string_view(data_->name):std::string_view{};}
std::string_view ScaleContext::tuningSource()const noexcept{return data_?std::string_view(data_->source):std::string_view{};}
std::string_view ScaleContext::tuningDescription()const noexcept{return data_?std::string_view(data_->description):std::string_view{};}
std::span<const ScaleDegree> ScaleContext::degrees()const noexcept{return data_?std::span<const ScaleDegree>(data_->degrees):std::span<const ScaleDegree>{};}
bool ScaleContext::includesDegree(std::int64_t id)const noexcept {
    if(!data_)return false;
    const auto& rows=data_->degrees;
    const auto at=std::lower_bound(rows.begin(),rows.end(),id,[](const auto& row,auto target){return row.id<target;});
    return at!=rows.end() && at->id==id && at->inScale;
}
std::optional<ScalePitch> ScaleContext::nearest(double pitch,double tonic,bool scaleOnly)const noexcept {
    if(!data_ || !std::isfinite(pitch) || !std::isfinite(tonic) || std::abs(pitch)>1e9 || std::abs(tonic)>1e9)return {};
    const auto& selected=scaleOnly?data_->members:data_->all;if(selected.empty())return {};
    const auto delta=static_cast<long double>(pitch)-static_cast<long double>(tonic);
    const auto quotient=std::floor(delta/static_cast<long double>(data_->period));
    if(!std::isfinite(quotient) || quotient<-periodLimit || quotient>periodLimit)return {};
    auto period=static_cast<std::int64_t>(quotient);auto local=delta-quotient*static_cast<long double>(data_->period);
    if(local<0){--period;local+=data_->period;}else if(local>=data_->period){++period;local-=data_->period;}
    const auto found=std::lower_bound(selected.begin(),selected.end(),local,[&](auto index,auto value){return static_cast<long double>(data_->degrees[index].cents)<value;});
    const auto highIndex=found==selected.end()?selected.front():*found,lowIndex=found==selected.begin()?selected.back():*(found-1);
    const auto highPeriod=period+(found==selected.end()?1:0),lowPeriod=period-(found==selected.begin()?1:0);
    const auto candidate=[&](std::size_t index,std::int64_t repeat){const auto& row=data_->degrees[index];
        return ScalePitch{row.id,repeat,index,static_cast<double>(static_cast<long double>(tonic)+static_cast<long double>(repeat)*data_->period+row.cents),row.inScale};};
    auto low=candidate(lowIndex,lowPeriod),high=candidate(highIndex,highPeriod);
    const auto lowDistance=found==selected.begin()?local+data_->period-data_->degrees[lowIndex].cents:local-data_->degrees[lowIndex].cents;
    const auto highDistance=found==selected.end()?data_->period-local+data_->degrees[highIndex].cents:data_->degrees[highIndex].cents-local;
    low.distanceCents=static_cast<double>(std::abs(lowDistance));high.distanceCents=static_cast<double>(std::abs(highDistance));
    if(!std::isfinite(low.cents))return std::isfinite(high.cents)?std::optional<ScalePitch>(high):std::nullopt;
    if(!std::isfinite(high.cents))return low;
    return lowDistance<=highDistance?low:high; // ties choose lower pitch
}
std::optional<ScalePitch> ScaleContext::match(double pitch,double tonic,double tolerance)const noexcept {
    if(!std::isfinite(tolerance) || tolerance<0)return {};
    const auto found=nearest(pitch,tonic);return found && found->distanceCents<=tolerance?found:std::nullopt;
}
bool ScaleContext::contains(double pitch,double tonic,double tolerance)const noexcept {const auto found=match(pitch,tonic,tolerance);return found && found->inScale;}
ScaleGrid ScaleContext::grid(double low,double high,double tonic,bool scaleOnly,std::size_t maxRows)const {
    ScaleGrid result;
    try {
        if(audio::onAudioThread){result.error="callback";return result;}
        if(!data_ || !std::isfinite(low) || !std::isfinite(high) || !std::isfinite(tonic) || std::abs(low)>1e9 || std::abs(high)>1e9 || std::abs(tonic)>1e9 || low>high || maxRows==0 || maxRows>65536)throw std::runtime_error("invalid scale ruler range or row budget");
        const auto& selected=scaleOnly?data_->members:data_->all;if(selected.empty())return result;
        const auto first=std::floor((static_cast<long double>(low)-tonic)/data_->period),last=std::floor((static_cast<long double>(high)-tonic)/data_->period);
        if(!std::isfinite(first) || !std::isfinite(last) || first<-periodLimit || last>periodLimit ||
           (last-first+1)*static_cast<long double>(selected.size())>static_cast<long double>(maxRows)+2*static_cast<long double>(selected.size()))
            throw std::runtime_error("scale ruler exceeds its period/row budget");
        for(auto repeat=static_cast<std::int64_t>(first);repeat<=static_cast<std::int64_t>(last);++repeat)for(const auto index:selected) {
            const auto& degree=data_->degrees[index];const auto cents=static_cast<double>(static_cast<long double>(tonic)+static_cast<long double>(repeat)*data_->period+degree.cents);
            if(cents<low || cents>high)continue;
            if(result.rows.size()==maxRows)throw std::runtime_error("scale ruler exceeds its row budget");
            result.rows.push_back({{degree.id,repeat,index,cents,degree.inScale},degree.name.empty()?std::to_string(degree.id):degree.name,encodeMidiPitch(cents)});
        }
    }catch(const std::exception& exception){result.error=exception.what();result.rows.clear();}
    return result;
}
ScaleContextRead readClipScaleContext(const Store& store,std::int64_t clipId) {
    ScaleContextRead result;
    if(audio::onAudioThread){result.error="callback";return result;}
    try {
        auto& db=const_cast<SQLite::Database&>(store.db());std::optional<SQLite::Transaction> transaction;
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        const auto details=readClipDetails(store,clipId,false);if(!details.clip)throw std::runtime_error(details.error);
        const auto& clip=*details.clip;
        if((clip.timeBase!=0 && clip.timeBase!=1) || (clip.timeBase==0 && (!clip.posTicks || *clip.posTicks<0)) ||
           (clip.timeBase==1 && !clip.scale.local && !clip.propertiesProblem.empty()))throw std::runtime_error("clip scale context cannot be derived from its stored clock");
        std::optional<TuningDefinition> definition;
        if(clip.scale.tuningId) {
            SQLite::Statement tuning(db,"SELECT name,source,period_cents,description FROM tuning_systems WHERE id=?");tuning.bind(1,*clip.scale.tuningId);
            if(!tuning.executeStep())throw std::runtime_error("clip scale references a missing tuning");
            definition.emplace();definition->id=*clip.scale.tuningId;definition->name=tuning.getColumn(0).getString();definition->source=tuning.getColumn(1).getString();
            definition->periodCents=tuning.getColumn(2).getDouble();definition->description=tuning.getColumn(3).getString();
            SQLite::Statement degrees(db,"SELECT degree,cents,name FROM tuning_degrees WHERE tuning_id=? ORDER BY degree");degrees.bind(1,*clip.scale.tuningId);
            while(degrees.executeStep()) {
                if(definition->degrees.size()==65536)throw std::runtime_error("tuning degree budget exceeded");
                definition->degrees.push_back({degrees.getColumn(0).getInt64(),degrees.getColumn(1).getDouble(),degrees.getColumn(2).getString()});
            }
        }
        result.context=compileScaleContext(clip.scale,definition,result.error);
        if(transaction)transaction->commit();
    }catch(const std::exception& exception){result.context.reset();result.error=exception.what();}
    return result;
}
} // namespace adi::engine
