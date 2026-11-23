// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/scale_timeline.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/timeline_clips.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
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
ScaleTimelineRead compileScaleTimeline(const rows::Model &model) {
    ScaleTimelineRead result;
    if (audio::onAudioThread) {
        result.error = "callback";
        return result;
    }
    try {
        require(model.keyEvents.size() <= eventLimit && model.keyDegrees.size() <= degreeLimit,
                "scale timeline event or membership budget exceeded");
        std::vector<ScaleKeyChange> changes;
        changes.reserve(model.keyEvents.size());
        std::map<std::int64_t, std::size_t> index;
        std::size_t text = 0;
        for (const auto &row : model.keyEvents) {
            require(index.emplace(row.id, changes.size()).second, "duplicate key identity");
            budget(text, row.scaleId.size(), textLimit, "scale timeline text budget exceeded");
            ClipScale scale;
            scale.root = row.root;
            scale.id = row.scaleId;
            scale.mask = row.mask;
            changes.push_back({row.id, row.posTicks, std::move(scale)});
        }
        std::set<std::int64_t> referencedTunings;
        for (const auto &row : model.keyTunings) {
            const auto found = index.find(row.keyId);
            require(found != index.end(), "orphan key tuning reference");
            auto &scale = changes[found->second].scale;
            require(!scale.tuningId, "duplicate key tuning reference");
            scale.tuningId = row.tuningId;
            referencedTunings.insert(row.tuningId);
            require(referencedTunings.size() <= tuningLimit, "scale timeline tuning budget exceeded");
        }
        for (const auto &row : model.keyDegrees) {
            const auto found = index.find(row.keyId);
            require(found != index.end(), "orphan key membership");
            auto &scale = changes[found->second].scale;
            require(scale.tuningId.has_value(), "membership without a tuning");
            scale.degrees.push_back(row.degree);
        }
        std::vector<TuningDefinition> tunings;
        tunings.reserve(referencedTunings.size());
        index.clear();
        for (const auto &row : model.tunings) {
            if (!referencedTunings.contains(row.id))
                continue;
            require(index.emplace(row.id, tunings.size()).second, "duplicate tuning identity");
            budget(text, row.name.size(), textLimit, "scale timeline text budget exceeded");
            budget(text, row.source.size(), textLimit, "scale timeline text budget exceeded");
            budget(text, row.description.size(), textLimit, "scale timeline text budget exceeded");
            tunings.push_back({row.id, row.name, row.source, row.description, row.periodCents, {}});
        }
        std::size_t totalDegrees = 0;
        for (const auto &row : model.tuningDegrees) {
            const auto found = index.find(row.tuningId);
            if (found == index.end())
                continue;
            auto &degrees = tunings[found->second].degrees;
            require(degrees.size() < eventLimit, "tuning degree budget exceeded");
            budget(totalDegrees, 1, degreeLimit, "scale timeline degree budget exceeded");
            budget(text, row.name.size(), textLimit, "scale timeline text budget exceeded");
            degrees.push_back({row.degree, row.cents, row.name});
        }
        result.timeline = compileScaleTimeline(changes, tunings, result.error);
    } catch (const std::exception &exception) {
        result.error = exception.what();
    }
    return result;
}
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

DeviceScalePosition TrackScaleTimeline::at(std::int64_t track, std::int64_t sample) const noexcept {
    const auto entries = tracks_.find(track);
    if (entries == tracks_.end())
        return {};
    const auto &spans = entries->second;
    const auto next = std::upper_bound(spans.begin(), spans.end(), sample,
                                       [](auto at, const auto &span) { return at < span.begin; });
    if (next != spans.begin()) {
        const auto &prior = *std::prev(next);
        if (sample < prior.end)
            return {&prior.context, prior.end};
    }
    return next == spans.end() ? DeviceScalePosition{} : DeviceScalePosition{nullptr, next->begin};
}

TrackScaleTimeline compileTrackScaleTimeline(const rows::Model &model, const TempoMap &tempo,
                                              double rate, std::string &error) {
    if (audio::onAudioThread) {
        error = "callback";
        return {};
    }
    struct Candidate {
        MidiClipPlacement placement;
        std::optional<ScaleContext> local;
    };
    struct Edge {
        std::int64_t sample;
        std::size_t clip;
        bool starts;
    };
    try {
        std::set<std::int64_t> localTracks;
        const auto eligible = [](const rows::Clip &clip) {
            return clip.kind == "midi" && !clip.muted && !clip.aliasOf;
        };
        for (const auto &clip : timelineClips(model)) {
            if (eligible(clip) && clip.scaleRoot) {
                localTracks.insert(clip.trackId);
                require(localTracks.size() <= eventLimit, "track scale budget exceeded");
            }
        }
        std::map<std::int64_t, std::vector<Candidate>> candidates;
        std::size_t clipCount = 0, text = 0;
        for (const auto &clip : timelineClips(model)) {
            if (!eligible(clip) || !localTracks.contains(clip.trackId))
                continue;
            MidiClipPlacement placement;
            try {
                placement = projectMidiClipPlacement(clip, tempo, rate);
            } catch (const std::runtime_error &) {
                continue; // Playback publishes the malformed clip's diagnostic.
            }
            if (placement.beginSample >= placement.endSample)
                continue;
            budget(clipCount, 1, eventLimit, "track scale clip budget exceeded");
            std::optional<ScaleContext> local;
            if (clip.scaleRoot) {
                require(clip.scaleMask.has_value() && clip.scaleId && !clip.scaleId->empty(),
                        "incomplete clip scale");
                budget(text, clip.scaleId->size(), textLimit, "track scale text budget exceeded");
                ClipScale scale;
                scale.root = *clip.scaleRoot;
                scale.mask = *clip.scaleMask;
                scale.id = *clip.scaleId;
                scale.local = true;
                local = compileScaleContext(scale, {}, error);
                require(local.has_value(), error.c_str());
            }
            candidates[clip.trackId].push_back({placement, std::move(local)});
        }
        TrackScaleTimeline result;
        for (const auto &[track, clips] : candidates) {
            std::vector<Edge> edges;
            edges.reserve(clips.size() * 2);
            for (std::size_t i = 0; i < clips.size(); ++i) {
                edges.push_back({clips[i].placement.beginSample, i, true});
                edges.push_back({clips[i].placement.endSample, i, false});
            }
            std::sort(edges.begin(), edges.end(), [](const auto &a, const auto &b) {
                return a.sample < b.sample;
            });
            std::set<std::size_t> active;
            for (auto cursor = edges.begin(); cursor != edges.end();) {
                const auto sample = cursor->sample;
                do {
                    if (cursor->starts)
                        active.insert(cursor->clip);
                    else
                        active.erase(cursor->clip);
                    ++cursor;
                } while (cursor != edges.end() && cursor->sample == sample);
                if (cursor == edges.end() || active.size() != 1)
                    continue;
                const auto &local = clips[*active.begin()].local;
                if (local)
                    result.tracks_[track].push_back({sample, cursor->sample, *local});
            }
        }
        error.clear();
        return result;
    } catch (const std::exception &exception) {
        error = exception.what();
        return {};
    }
}
} // namespace adi::engine
