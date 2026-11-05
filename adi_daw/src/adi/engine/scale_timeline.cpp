// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scale_timeline.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
namespace adi::engine {
namespace {
constexpr std::size_t eventLimit=65536,tuningLimit=1024,degreeLimit=1048576,textLimit=8*1024*1024;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void budget(std::size_t& total,std::size_t amount,std::size_t limit,const char* message){require(amount<=limit-total,message);total+=amount;}
void tuningText(std::size_t& total,const TuningDefinition& tuning) {
    budget(total,tuning.name.size(),textLimit,"scale timeline text budget exceeded");
    budget(total,tuning.source.size(),textLimit,"scale timeline text budget exceeded");
    budget(total,tuning.description.size(),textLimit,"scale timeline text budget exceeded");
    for(const auto& degree:tuning.degrees)budget(total,degree.name.size(),textLimit,"scale timeline text budget exceeded");
}
} // namespace
struct ScaleTimelineData {
    explicit ScaleTimelineData(ScaleContext context):fallback(std::move(context)){}
    ScaleContext fallback;
    std::vector<ScaleTimelineEvent> changes;
};
std::optional<ScaleTimeline> compileScaleTimeline(std::span<const ScaleKeyChange> changes,std::span<const TuningDefinition> definitions,std::string& error) {
    if(audio::onAudioThread){error="callback";return {};}
    try {
        require(changes.size()<=eventLimit && definitions.size()<=tuningLimit,"scale timeline event/tuning budget exceeded");
        std::map<std::int64_t,const TuningDefinition*> available;
        for(const auto& definition:definitions)require(definition.id>0 && available.emplace(definition.id,&definition).second,"invalid/duplicate tuning identity");
        std::set<std::int64_t> identities;std::int64_t previous=-1;std::size_t degrees=12,members=0,text=0;
        // Preflight every repeated definition, not just its first reference,
        // before allocating compiled degree/name copies for any event.
        for(const auto& change:changes) {
            require(change.id>0 && identities.insert(change.id).second && change.posTicks>=0 && change.posTicks>previous,"invalid, duplicate or unordered scale key event");previous=change.posTicks;
            require(!change.scale.local,"a global key event cannot be a clip-local override");
            budget(text,change.scale.id.size(),textLimit,"scale timeline text budget exceeded");
            budget(members,change.scale.degrees.size(),degreeLimit,"scale timeline membership budget exceeded");
            if(change.scale.tuningId) {
                const auto found=available.find(*change.scale.tuningId);require(found!=available.end(),"key event references a missing tuning");
                budget(degrees,found->second->degrees.size(),degreeLimit,"scale timeline degree budget exceeded");tuningText(text,*found->second);
            } else budget(degrees,12,degreeLimit,"scale timeline degree budget exceeded");
        }
        const auto fallback=compileScaleContext(ClipScale{},{},error);if(!fallback)throw std::runtime_error(error);
        auto data=std::make_shared<ScaleTimelineData>(*fallback);data->changes.reserve(changes.size());
        for(const auto& change:changes) {
            std::optional<TuningDefinition> definition;
            if(change.scale.tuningId)definition=*available.at(*change.scale.tuningId);
            const auto context=compileScaleContext(change.scale,definition,error);if(!context)throw std::runtime_error(error);
            data->changes.push_back({change.id,change.posTicks,*context});
        }
        error.clear();return ScaleTimeline(std::move(data));
    }catch(const std::exception& exception){error=exception.what();return {};}
}
const ScaleContext* ScaleTimeline::atTicks(std::int64_t ticks) const noexcept {
    if(!data_)return nullptr;
    const auto& changes=data_->changes;const auto found=std::upper_bound(changes.begin(),changes.end(),ticks,[](auto tick,const auto& row){return tick<row.posTicks;});
    return found==changes.begin()?&data_->fallback:&(found-1)->context;
}
std::optional<std::int64_t> ScaleTimeline::nextChangeAfterTicks(std::int64_t ticks) const noexcept {
    if(!data_)return {};
    const auto& changes=data_->changes;const auto found=std::upper_bound(changes.begin(),changes.end(),ticks,[](auto tick,const auto& row){return tick<row.posTicks;});
    return found==changes.end()?std::nullopt:std::optional<std::int64_t>(found->posTicks);
}
std::span<const ScaleTimelineEvent> ScaleTimeline::events() const noexcept{return data_?std::span<const ScaleTimelineEvent>(data_->changes):std::span<const ScaleTimelineEvent>{};}
ScaleTimelineRead readScaleTimeline(const Store& store) {
    ScaleTimelineRead result;if(audio::onAudioThread){result.error="callback";return result;}
    try {
        auto& db=const_cast<SQLite::Database&>(store.db());std::optional<SQLite::Transaction> transaction;
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        std::vector<ScaleKeyChange> changes;std::map<std::int64_t,std::size_t> index;std::size_t text=0;
        {
            SQLite::Statement query(db,"SELECT id,pos_ticks,root,scale_id,scale_mask FROM key_map ORDER BY pos_ticks");
            while(query.executeStep()) {
                require(changes.size()<eventLimit,"scale timeline event budget exceeded");ScaleKeyChange change;
                change.id=query.getColumn(0).getInt64();change.posTicks=query.getColumn(1).getInt64();change.scale.root=query.getColumn(2).getInt64();change.scale.id=query.getColumn(3).getString();change.scale.mask=query.getColumn(4).getInt64();
                budget(text,change.scale.id.size(),textLimit,"scale timeline text budget exceeded");require(index.emplace(change.id,changes.size()).second,"duplicate key identity");changes.push_back(std::move(change));
            }
        }
        std::set<std::int64_t> tuningIds;
        {
            SQLite::Statement query(db,"SELECT key_map_id,tuning_id FROM key_map_tunings ORDER BY key_map_id");
            while(query.executeStep()) {
                const auto found=index.find(query.getColumn(0).getInt64());require(found!=index.end(),"orphan key tuning reference");auto& scale=changes[found->second].scale;
                require(!scale.tuningId,"duplicate key tuning reference");scale.tuningId=query.getColumn(1).getInt64();tuningIds.insert(*scale.tuningId);require(tuningIds.size()<=tuningLimit,"scale timeline tuning budget exceeded");
            }
        }
        std::size_t members=0;
        {
            SQLite::Statement query(db,"SELECT key_map_id,degree FROM key_map_degrees ORDER BY key_map_id,degree");
            while(query.executeStep()) {
                budget(members,1,degreeLimit,"scale timeline membership budget exceeded");const auto found=index.find(query.getColumn(0).getInt64());require(found!=index.end(),"orphan key membership");
                auto& scale=changes[found->second].scale;require(scale.tuningId.has_value(),"membership without a tuning");scale.degrees.push_back(query.getColumn(1).getInt64());
            }
        }
        std::vector<TuningDefinition> definitions;std::size_t degrees=0;
        for(const auto id:tuningIds) {
            SQLite::Statement query(db,"SELECT name,source,period_cents,description FROM tuning_systems WHERE id=?");query.bind(1,id);require(query.executeStep(),"missing referenced tuning definition");
            TuningDefinition definition;definition.id=id;definition.name=query.getColumn(0).getString();definition.source=query.getColumn(1).getString();definition.periodCents=query.getColumn(2).getDouble();definition.description=query.getColumn(3).getString();
            // Bound strings as rows arrive, before retaining a whole table.
            tuningText(text,definition);
            SQLite::Statement rows(db,"SELECT degree,cents,name FROM tuning_degrees WHERE tuning_id=? ORDER BY degree");rows.bind(1,id);
            while(rows.executeStep()) {
                require(definition.degrees.size()<eventLimit,"tuning degree budget exceeded");budget(degrees,1,degreeLimit,"scale timeline degree budget exceeded");
                auto name=rows.getColumn(2).getString();budget(text,name.size(),textLimit,"scale timeline text budget exceeded");definition.degrees.push_back({rows.getColumn(0).getInt64(),rows.getColumn(1).getDouble(),std::move(name)});
            }
            definitions.push_back(std::move(definition));
        }
        result.timeline=compileScaleTimeline(changes,definitions,result.error);if(transaction)transaction->commit();
    }catch(const std::exception& exception){result.timeline.reset();result.error=exception.what();}
    return result;
}
} // namespace adi::engine
