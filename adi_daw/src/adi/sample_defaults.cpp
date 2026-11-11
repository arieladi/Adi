// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/sample_defaults.hpp"
#include "adi/library/index.hpp"
#include "adi/clip_commands.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
namespace adi::clip_edit {
namespace {
using library::SampleSettings;
std::int64_t integer(const Payload& value){if(!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>()>static_cast<std::uint64_t>(INT64_MAX)))throw std::runtime_error("invalid native sample-setting integer");return value.get<std::int64_t>();}
std::optional<std::int64_t> optionalInteger(const Payload& row,const char* key){if(!row.contains(key) || row.at(key).is_null())return {};return integer(row.at(key));}
Payload optional(const std::optional<std::int64_t>& value){return value?Payload(*value):Payload(nullptr);}
Payload optional(const std::optional<double>& value){return value?Payload(*value):Payload(nullptr);}
bool boolean(const Payload& value){const auto v=integer(value);if(v!=0 && v!=1)throw std::runtime_error("unsupported native sample-setting flag");return v!=0;}
void known(const Payload& row,std::initializer_list<const char*> keys) {
    const std::set<std::string> fields(keys.begin(),keys.end());
    for(const auto& [key,value]:row.items()){(void)value;if(!fields.contains(key))throw std::runtime_error("sample defaults cannot discard unknown native fields: "+key);}
}
const Payload* child(const Payload& image,const char* table) {
    if(!image.at("children").contains(table) || image.at("children").at(table).empty())return nullptr;
    if(image.at("children").at(table).size()!=1)throw std::runtime_error("sample defaults need one audio attachment");
    return &image.at("children").at(table)[0];
}
std::int64_t nanos(double seconds,double rate){
    const auto frames=seconds*rate;if(!std::isfinite(frames) || frames<0 || frames>0x1p50)throw std::runtime_error("sample source clock exceeds native bounds");
    return edit::nanosFromSeconds(static_cast<double>(std::llround(frames))/rate);
}
std::int64_t initialSpan(std::int64_t offset,std::int64_t end,const std::optional<std::int64_t>& loopBegin,const std::optional<std::int64_t>& loopEnd) {
    if(loopBegin && loopEnd) {
        if(*loopEnd<=*loopBegin || offset<0)throw std::runtime_error("invalid saved sample loop");
        if(offset>=*loopBegin)offset=*loopBegin+(offset-*loopBegin)%(*loopEnd-*loopBegin);
        end=*loopEnd;
    }
    if(offset<0 || end<=offset)throw std::runtime_error("saved sample has no initial playable extent");
    return end-offset;
}
}
SampleDefaultsCapture captureSampleDefaults(Store& store,std::int64_t clipId,double referenceRate) {
    // The callback refusal is a prvalue built in the caller's slot: guaranteed
    // elision, not optional NRVO, so it costs exactly one empty result.
    if(audio::onAudioThread)return {};
    SampleDefaultsCapture result;
    try {
        std::optional<SQLite::Transaction> read;if(sqlite3_get_autocommit(store.db().getHandle())!=0)read.emplace(store.db(),SQLite::TransactionBehavior::DEFERRED);
        const auto copied=copyClip(store,clipId,referenceRate);if(!copied.ok)throw std::runtime_error(copied.error);
        const auto& image=copied.image;const auto& root=image.at("row");if(root.at("kind")!="audio")throw std::runtime_error("sample defaults need an audio clip");
        known(root,{"id","track_id","lane_id","kind","name","color","time_base","pos_ticks","pos_ns","length_ticks","length_ns","loop_enabled","loop_start_ticks","loop_len_ticks","content_offset_ticks","muted","gain_db","fade_in_ticks","fade_out_ticks","fade_in_curve","fade_out_curve","alias_of"});
        const auto* source=child(image,"audio_clips");if(!source)throw std::runtime_error("audio clip has no source attachment");
        known(*source,{"clip_id","media_id","src_start_frames","src_len_frames","warp_enabled","warp_mode","warp_markers","transpose_semis","formant_shift","reverse","channel_mode"});
        // Project/device automation is not portable per-file metadata. Refuse
        // instead of presenting a partial authored save as a complete default.
        for(const auto* table:{"automation_data","automation_segment_ranges"})if(image.at("children").contains(table) && !image.at("children").at(table).empty())throw std::runtime_error("clip automation cannot be saved as sample defaults");
        SQLite::Statement media(store.db(),"SELECT hash_blake3,sample_rate,channels,frames FROM media_files WHERE id=?");media.bind(1,integer(source->at("media_id")));
        if(!media.executeStep() || media.getColumn(0).isNull() || media.getColumn(1).isNull() || media.getColumn(2).isNull() || media.getColumn(3).isNull())throw std::runtime_error("sample source identity/dimensions are missing");
        const auto rate=media.getColumn(1).getInt64(),channels=media.getColumn(2).getInt64(),frames=media.getColumn(3).getInt64();if(rate<1 || rate>768000 || channels<1 || channels>64 || frames<1)throw std::runtime_error("invalid source dimensions");
        SampleSettings settings;settings.sampleRate=static_cast<std::uint32_t>(rate);settings.channels=static_cast<std::uint32_t>(channels);settings.frames=static_cast<std::uint64_t>(frames);
        const auto first=integer(source->at("src_start_frames")),length=integer(source->at("src_len_frames"));if(first<0 || length<1)throw std::runtime_error("invalid source frame window");settings.sourceStart=static_cast<std::uint64_t>(first);settings.sourceLength=static_cast<std::uint64_t>(length);
        settings.warpEnabled=boolean(source->at("warp_enabled"));settings.warpMode=source->at("warp_mode").get<std::string>();settings.transposeSemis=source->at("transpose_semis").get<double>();settings.formantShift=source->at("formant_shift").get<double>();settings.reverse=boolean(source->at("reverse"));settings.channelMode=integer(source->at("channel_mode"));
        if(!source->at("warp_markers").is_null()) {
            if(!source->at("warp_markers").is_binary())throw std::runtime_error("native Warp map is not binary");
            const auto& bytes=source->at("warp_markers").get_binary();if(bytes.size()>library::kMaxSampleWarpBytes)throw std::runtime_error("native Warp map exceeds library bounds");
            settings.warpMarkers.emplace(bytes.size());if(!bytes.empty())std::memcpy(settings.warpMarkers->data(),bytes.data(),bytes.size());
        }
        settings.gainDb=root.at("gain_db").get<double>();settings.muted=boolean(root.at("muted"));settings.loopEnabled=boolean(root.at("loop_enabled"));settings.contentOffsetTicks=integer(root.at("content_offset_ticks"));settings.loopStartTicks=integer(root.at("loop_start_ticks"));settings.loopLengthTicks=optionalInteger(root,"loop_len_ticks");
        settings.fadeInTicks=integer(root.at("fade_in_ticks"));settings.fadeOutTicks=integer(root.at("fade_out_ticks"));settings.fadeInCurve=integer(root.at("fade_in_curve"));settings.fadeOutCurve=integer(root.at("fade_out_curve"));
        if(const auto* playback=child(image,"clip_playback_details")) {
            known(*playback,{"clip_id","in_tension","out_tension","content_offset_ns","loop_begin_ns","loop_end_ns","source_end_ns","in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"});
            if(!playback->at("in_tension").is_null())settings.fadeInTension=playback->at("in_tension").get<double>();
            if(!playback->at("out_tension").is_null())settings.fadeOutTension=playback->at("out_tension").get<double>();
            settings.contentOffsetNs=optionalInteger(*playback,"content_offset_ns");settings.loopBeginNs=optionalInteger(*playback,"loop_begin_ns");settings.loopEndNs=optionalInteger(*playback,"loop_end_ns");settings.sourceEndNs=optionalInteger(*playback,"source_end_ns");
            constexpr std::array<const char*,4> names{"in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"};for(std::size_t i=0;i<4;++i)settings.fadeClockNs[i]=optionalInteger(*playback,names[i]);
        }
        if(const auto* fades=child(image,"clip_warp_fade_domains")) {
            known(*fades,{"clip_id","in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"});constexpr std::array<const char*,4> names{"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"};for(std::size_t i=0;i<4;++i)settings.fadeClockTicks[i]=optionalInteger(*fades,names[i]);
        }
        if(const auto* properties=child(image,"clip_properties")) {
            known(*properties,{"clip_id","end_marker_ticks","signature_numerator","signature_denominator","scale_root","scale_id","scale_mask"});
            settings.endMarkerTicks=optionalInteger(*properties,"end_marker_ticks");settings.signatureNumerator=optionalInteger(*properties,"signature_numerator");settings.signatureDenominator=optionalInteger(*properties,"signature_denominator");
        }
        if(!settings.warpEnabled && settings.endMarkerTicks) {
            // copyClip already freezes an absent legacy End clock. A present
            // pin is independent of the current placement tempo (SPEC 9.2).
            if(!settings.sourceEndNs)throw std::runtime_error("captured unwarped End has no source clock");
            settings.endMarkerTicks.reset();
        }
        if(settings.contentOffsetTicks<0 && !settings.warpEnabled && settings.contentOffsetNs) {
            // Native left trim can derive signed display ticks after a tempo
            // relocation. Portable defaults use a nonnegative legacy fallback;
            // the unchanged physical pin remains the playback authority.
            edit::TimeMap time(store.db());const auto origin=root.at("time_base")==0?integer(root.at("pos_ticks")):time.ticks(integer(root.at("pos_ns")));
            settings.contentOffsetTicks=time.ticks(edit::addTime(time.nanos(origin),*settings.contentOffsetNs))-origin;
        }
        std::string error;if(library::validateSampleSettings(settings,error)!=library::SampleSettingsProblem::None)throw std::runtime_error(error);
        result.hash=media.getColumn(0).getString();if(result.hash.size()!=64 || result.hash.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("invalid source content hash");result.settings=std::move(settings);result.ok=true;
    }catch(const std::exception& e){result.error=e.what();}
    return result;
}
library::SampleSettingsProblem saveSampleDefaults(Store& store,std::int64_t clipId,double referenceRate,library::Index& index,std::string& error) {
    if(audio::onAudioThread)return library::SampleSettingsProblem::Realtime;
    const auto captured=captureSampleDefaults(store,clipId,referenceRate);if(!captured.ok){error=captured.error;return library::SampleSettingsProblem::Invalid;}
    return index.saveSampleSettings(captured.hash,*captured.settings,error);
}
void applySampleDefaults(Store& store,Payload& image,const SampleSettings& s,double referenceRate) {
    if(audio::onAudioThread)throw std::runtime_error("sample defaults cannot be applied on the callback");
    std::string error;if(library::validateSampleSettings(s,error)!=library::SampleSettingsProblem::None)throw std::runtime_error(error);
    auto& root=image.at("row");auto& source=image.at("children").at("audio_clips").at(0);edit::TimeMap time(store.db());
    const auto origin=root.at("time_base")==0?integer(root.at("pos_ticks")):time.ticks(integer(root.at("pos_ns")));const auto beginNs=root.at("time_base")==0?time.nanos(origin):integer(root.at("pos_ns"));
    const auto naturalNs=edit::nanosFromSeconds(static_cast<double>(s.sourceLength)/s.sampleRate);
    std::int64_t durationNs=naturalNs,tickSpan=0;
    auto endMarker=s.endMarkerTicks;
    if(s.warpEnabled) {
        if(s.sourceLength>0x7fffffff || referenceRate<44100 || referenceRate!=std::floor(referenceRate))throw std::runtime_error("saved Warp is outside native preparation bounds");
        const auto map=audio::readWarpMap(*s.warpMarkers,s.sourceLength);if(!map)throw std::runtime_error("invalid saved Warp map");
        const auto last=map.markers.back().tick;std::optional<std::int64_t> loopEnd;
        if(s.loopEnabled){loopEnd=edit::addTime(s.loopStartTicks,*s.loopLengthTicks);if(*loopEnd>last)throw std::runtime_error("saved Warp loop extends beyond its source map");}
        tickSpan=initialSpan(s.contentOffsetTicks,std::min(last,s.endMarkerTicks.value_or(last)),s.loopEnabled?std::optional(s.loopStartTicks):std::nullopt,loopEnd);
        engine::TempoMap tempo;SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");while(points.executeStep())tempo.events.push_back({points.getColumn(0).getInt64(),points.getColumn(1).getDouble(),points.getColumn(2).getInt()});if(tempo.events.empty())tempo.events.push_back({0,120,0});
        const auto anchors=engine::projectWarpAnchors(map.markers,tempo,origin-s.contentOffsetTicks,referenceRate);if(anchors.empty() || !anchors.back().outputFrame || anchors.back().outputFrame>0x7fffffffULL)throw std::runtime_error("saved Warp output exceeds native bounds");
        root["time_base"]=0;root["pos_ticks"]=origin;root["pos_ns"]=nullptr;root["length_ticks"]=tickSpan;root["length_ns"]=nullptr;
    }else {
        const auto offset=(s.contentOffsetNs?*s.contentOffsetNs:nanos(time.durationSeconds(origin,s.contentOffsetTicks),referenceRate));
        auto end=std::min(naturalNs,s.sourceEndNs.value_or(naturalNs));if(endMarker && !s.sourceEndNs)end=std::min(end,nanos(time.durationSeconds(origin,*endMarker),referenceRate));
        std::optional<std::int64_t> loopBegin,loopEnd;
        if(s.loopEnabled){loopBegin=(s.loopBeginNs?*s.loopBeginNs:nanos(time.durationSeconds(origin,s.loopStartTicks),referenceRate));loopEnd=(s.loopEndNs?*s.loopEndNs:nanos(time.durationSeconds(origin,edit::addTime(s.loopStartTicks,*s.loopLengthTicks)),referenceRate));if(*loopEnd>naturalNs)throw std::runtime_error("saved loop extends beyond its source window");}
        durationNs=initialSpan(offset,end,loopBegin,loopEnd);tickSpan=time.ticks(edit::addTime(beginNs,durationNs))-origin;
        if(root.at("time_base")==0)root["length_ticks"]=tickSpan;else root["length_ns"]=durationNs;
        if(s.sourceEndNs)endMarker.reset();
    }
    root["muted"]=s.muted?1:0;root["gain_db"]=s.gainDb;root["loop_enabled"]=s.loopEnabled?1:0;root["content_offset_ticks"]=s.contentOffsetTicks;root["loop_start_ticks"]=s.loopStartTicks;root["loop_len_ticks"]=optional(s.loopLengthTicks);root["fade_in_ticks"]=s.fadeInTicks;root["fade_out_ticks"]=s.fadeOutTicks;root["fade_in_curve"]=s.fadeInCurve;root["fade_out_curve"]=s.fadeOutCurve;
    if(!s.warpEnabled) {
        // Pinned source fades own their actual shape. Re-express the legacy
        // handle lengths in the destination tempo so its unused fallback
        // cannot fail at a different tempo or before project tick zero.
        if(s.fadeClockNs[0])root["fade_in_ticks"]=std::min(tickSpan,time.ticks(edit::addTime(beginNs,*s.fadeClockNs[1]-*s.fadeClockNs[0]))-origin);
        if(s.fadeClockNs[2]) {
            const auto fade=std::min(durationNs,*s.fadeClockNs[3]-*s.fadeClockNs[2]);const auto end=edit::addTime(beginNs,durationNs);root["fade_out_ticks"]=time.ticks(end)-time.ticks(end-fade);
        }
        if(s.loopEnabled && !s.loopLengthTicks) {
            const auto begin=time.ticks(edit::addTime(beginNs,*s.loopBeginNs))-origin;const auto end=time.ticks(edit::addTime(beginNs,*s.loopEndNs))-origin;
            root["loop_start_ticks"]=begin;root["loop_len_ticks"]=end-begin;
        }
    }
    source["src_start_frames"]=s.sourceStart;source["src_len_frames"]=s.sourceLength;source["warp_enabled"]=s.warpEnabled?1:0;source["warp_mode"]=s.warpMode;source["transpose_semis"]=s.transposeSemis;source["formant_shift"]=s.formantShift;source["reverse"]=s.reverse?1:0;source["channel_mode"]=s.channelMode;
    source["warp_markers"]=nullptr;if(s.warpMarkers){std::vector<std::uint8_t> bytes(s.warpMarkers->size());if(!bytes.empty())std::memcpy(bytes.data(),s.warpMarkers->data(),bytes.size());source["warp_markers"]=Payload::binary(std::move(bytes));}
    Payload playback={{"clip_id",image.at("id")},{"in_tension",optional(s.fadeInTension)},{"out_tension",optional(s.fadeOutTension)},{"content_offset_ns",optional(s.contentOffsetNs)},{"loop_begin_ns",optional(s.loopBeginNs)},{"loop_end_ns",optional(s.loopEndNs)},{"source_end_ns",optional(s.sourceEndNs)}};
    constexpr std::array<const char*,4> nsNames{"in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"};for(std::size_t i=0;i<4;++i)playback[nsNames[i]]=optional(s.fadeClockNs[i]);image["children"]["clip_playback_details"]=Payload::array({edit::defaultRow(store.db(),"clip_playback_details",playback)});
    if(s.signatureNumerator || s.signatureDenominator || endMarker)image["children"]["clip_properties"]=Payload::array({edit::defaultRow(store.db(),"clip_properties",{{"clip_id",image.at("id")},{"end_marker_ticks",optional(endMarker)},{"signature_numerator",optional(s.signatureNumerator)},{"signature_denominator",optional(s.signatureDenominator)}})});
    if(std::any_of(s.fadeClockTicks.begin(),s.fadeClockTicks.end(),[](const auto& value){return value.has_value();})) {
        Payload fades={{"clip_id",image.at("id")}};constexpr std::array<const char*,4> names{"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"};for(std::size_t i=0;i<4;++i)fades[names[i]]=optional(s.fadeClockTicks[i]);image["children"]["clip_warp_fade_domains"]=Payload::array({edit::defaultRow(store.db(),"clip_warp_fade_domains",fades)});
    }
}
} // namespace adi::clip_edit
