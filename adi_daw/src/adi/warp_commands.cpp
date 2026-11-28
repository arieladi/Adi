// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/warp_commands.hpp"
#include "adi/audio/decode.hpp"
#include "adi/audio/transients.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>
namespace adi::clip_edit {
namespace {
struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db){if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);}
};
struct Decoded {
    std::filesystem::path file;bool pinned=false;
    explicit Decoded(const std::filesystem::path& input):file(audio::playableFile(input)),pinned(file!=input){}
    ~Decoded(){if(pinned)try{audio::defaultDecodeCache().release(file);}catch(...){}}
};
void cancelled(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Warp edit cancelled");}
std::int64_t integer(const Payload& row,const char* column){return detail::signedInteger(row.at(column));}
// Each interval is linear in the source map. The same interpolation converts
// source positions in either direction without depending on the song tempo.
std::int64_t mapPosition(std::span<const WarpMarker> markers,std::int64_t value,
                         std::int64_t WarpMarker::*from,std::int64_t WarpMarker::*to) {
    if(value<=markers.front().*from)return markers.front().*to;
    if(value>=markers.back().*from)return markers.back().*to;
    const auto right=std::upper_bound(markers.begin(),markers.end(),value,
        [from](auto position,const WarpMarker& point){return position<point.*from;});
    const auto left=right-1;
    const auto fraction=static_cast<long double>(value-(*left).*from)/
                        static_cast<long double>((*right).*from-(*left).*from);
    return (*left).*to+std::llround(fraction*static_cast<long double>((*right).*to-(*left).*to));
}
std::int64_t scaleWarpTicks(std::int64_t ticks,long double ratio) {
    const auto value=static_cast<long double>(ticks)*ratio;
    if(!std::isfinite(value) || value<0 ||
       value>=static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("source tempo exceeds the supported musical clock");
    return std::llround(value);
}
long double correctWarpMap(std::vector<WarpMarker>& markers,double rate,double requested) {
    const auto bpm=audio::sourceTempoBpm(markers,rate);
    if(bpm<=0)throw std::runtime_error("Warp has no readable source tempo");
    const auto ratio=static_cast<long double>(requested)/bpm;
    for(auto& marker:markers)marker.tick=scaleWarpTicks(marker.tick,ratio);
    return ratio;
}
Payload capturedWarpMap(std::span<const WarpMarker> markers,std::uint64_t frames) {
    const auto blob=audio::writeWarpMap(markers,frames);
    if(!blob)throw std::runtime_error("cannot capture the Warp markers");
    std::vector<std::uint8_t> bytes(blob.bytes.size());
    if(!bytes.empty())std::memcpy(bytes.data(),blob.bytes.data(),bytes.size());
    return Payload::binary(std::move(bytes));
}

struct WarpStretch {std::int64_t position,length;};
PreparedEdit prepareWarpChange(Store& store,std::int64_t clipId,const WarpOptions& options,double rate,
                               std::optional<WarpStretch> stretch={}) {
    PreparedEdit result;
    try {
        cancelled(options.stop);
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(!std::isfinite(rate) || rate<44100 || rate>768000 || rate!=std::floor(rate))throw std::runtime_error("invalid Warp session rate");
        const auto* mode=audio::warpModeName(options.mode);
        if(!mode)throw std::runtime_error("unknown Warp mode");
        Read read(store.db());result.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement location(store.db(),"PRAGMA database_list");
        while(location.executeStep())if(location.getColumn(1).getString()=="main")result.sourceFile=location.getColumn(2).getString();
        OpContext context{store,store.db()};auto image=captureDeleted(context,clipId);auto root=image.at("row");
        const auto& sources=image.at("children").at("audio_clips");
        if(root.at("kind")!="audio" || !root.at("alias_of").is_null() || sources.size()!=1)throw std::runtime_error("Warp needs an attached, independent audio clip");
        auto source=sources[0];
        const bool warped=integer(source,"warp_enabled")!=0;
        const bool looping=integer(root,"loop_enabled")!=0;
        const auto requested=options.enabled?options.analysis.sourceBpm:std::nullopt;
        const bool retiming=warped && (requested || stretch);
        if(stretch && (!warped || stretch->position<0 || stretch->length<=0))
            throw std::runtime_error("Stretch needs a positive span on a warped clip.");
        if(requested && (!std::isfinite(*requested) || *requested<=0 || *requested>=1000))
            throw std::runtime_error("source BPM must be greater than zero and below 1000");
        const auto capture=[&](const edit::RowEdits& plan,const char* label) {
            OpRequest request;request.opType="arranger.splice";request.payload={{"id",clipId},{"changes",plan.changes()}};
            request.targetKind="clip";request.targetId=clipId;request.label=label;
            result.requests.push_back(std::move(request));
        };
        if(warped==options.enabled && !retiming) {
            if(!warped || source.at("warp_mode")==mode)
                throw std::runtime_error("clip already has the requested Warp state");
            source["warp_mode"]=mode;
            edit::RowEdits plan(store.db());plan.set("audio_clips",clipId,source);
            capture(plan,"Change Warp Mode");
            result.options.beforeCommit=[stop=options.stop](SQLite::Database&,std::int64_t,std::string& error) {
                if(!stop.stop_requested())return true;
                error="Warp edit cancelled";return false;
            };
            cancelled(options.stop);result.ok=true;return result;
        }
        if(!retiming && ((options.enabled && looping) || integer(source,"reverse")!=0 ||
           source.at("transpose_semis")!=0 || source.at("formant_shift")!=0))
            throw std::runtime_error("Warp switching requires a clip without looping, reverse or pitch edits");
        const auto& savedProperties=image.at("children").at("clip_properties");
        auto properties=savedProperties.empty()?Payload(nullptr):savedProperties.front();
        const bool hasEnd=!properties.is_null() && !properties.at("end_marker_ticks").is_null();
        auto details=image.at("children").at("clip_playback_details").empty()?
            edit::defaultRow(store.db(),"clip_playback_details",{{"clip_id",clipId}}):image.at("children").at("clip_playback_details")[0];
        const auto start=integer(source,"src_start_frames"),storedLength=integer(source,"src_len_frames");
        std::filesystem::path file;
        std::string expectedHash;
        std::optional<Decoded> decoded;
        std::optional<audio::WavReader> reader;
        std::uint64_t frames=0;
        if(stretch) {
            if(start<0 || storedLength<=0 || storedLength>0x7fffffff)
                throw std::runtime_error("Warp source window is outside supported bounds");
            frames=static_cast<std::uint64_t>(storedLength);
        } else {
            const auto mediaId=integer(source,"media_id");
            const auto mediaRow=media::mediaRow(store.db(),mediaId);
            std::string mediaError;
            file=media::resolveMedia(media::projectFolder(store),mediaRow,mediaError);
            if(file.empty())throw std::runtime_error(mediaError.empty()?"Warp source is missing":mediaError);
            const auto hash=media::blake3File(file,options.stop);cancelled(options.stop);
            if(!hash)throw std::runtime_error("cannot verify Warp source");
            expectedHash=hash.hex;
            decoded.emplace(file);
            reader.emplace(decoded->file);
            if(reader->frames()>0x7fffffff || start<0 || storedLength<0 || static_cast<std::uint64_t>(start)>=reader->frames())
                throw std::runtime_error("Warp source window is outside supported bounds");
            frames=storedLength>0?static_cast<std::uint64_t>(storedLength):reader->frames()-static_cast<std::uint64_t>(start);
            if(frames==0 || frames>reader->frames()-static_cast<std::uint64_t>(start) || reader->channels()>64 ||
               reader->sampleRate()<8000 || reader->sampleRate()>768000)
                throw std::runtime_error("Warp source window is outside supported bounds");
        }
        const auto physicalNs=reader?edit::nanosFromSeconds(static_cast<double>(frames)/reader->sampleRate()):0;
        edit::TimeMap time(store.db());const auto base=integer(root,"time_base");
        const auto oldOrigin=base==0?integer(root,"pos_ticks"):time.ticks(integer(root,"pos_ns"));
        const auto oldBeginNs=base==0?time.nanos(oldOrigin):integer(root,"pos_ns");
        const auto spanNs=base==0?time.duration(oldOrigin,integer(root,"length_ticks")):integer(root,"length_ns");
        const auto origin=stretch?stretch->position:oldOrigin;
        const auto beginNs=origin==oldOrigin?oldBeginNs:time.nanos(origin);
        const auto offsetTicks=integer(root,"content_offset_ticks");
        const auto storedMap=[&]{
            if(!source.at("warp_markers").is_binary())throw std::runtime_error("Warp needs a valid stored source map");
            const auto& bytes=source.at("warp_markers").get_binary();
            auto map=audio::readWarpMap(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),frames);
            if(!map || map.futureLayout)throw std::runtime_error("Warp cannot switch an unreadable or future source map");
            return map;
        };
        const auto nanosAtFrame=[&](std::int64_t frame) {
            return edit::nanosFromSeconds(static_cast<double>(frame)/reader->sampleRate());
        };
        const auto frameAtNanos=[&](std::int64_t nanos) {
            return std::llround(static_cast<long double>(nanos)*reader->sampleRate()/1e9L);
        };
        const auto physicalToTicks=[&](std::int64_t nanos){return time.ticks(edit::addTime(beginNs,nanos))-origin;};
        std::vector<WarpMarker> markers;
        long double ratio=1;
        if(retiming) {
            markers=storedMap().markers;
            const auto spanTicks=base==0?integer(root,"length_ticks"):
                                 time.ticks(edit::addTime(oldBeginNs,spanNs))-oldOrigin;
            if(stretch) {
                if(spanTicks<=0 || edit::addTime(stretch->position,stretch->length)<=stretch->position)
                    throw std::runtime_error("Stretch needs a positive span on a warped clip.");
                ratio=static_cast<long double>(stretch->length)/static_cast<long double>(spanTicks);
                for(auto& marker:markers)marker.tick=scaleWarpTicks(marker.tick,ratio);
            } else {
                ratio=correctWarpMap(markers,reader->sampleRate(),*requested);
            }
            source["warp_markers"]=capturedWarpMap(markers,frames);
            const auto scale=[&](std::int64_t ticks){return scaleWarpTicks(ticks,ratio);};
            const auto startTick=scale(offsetTicks);
            const auto length=stretch?stretch->length:scale(edit::addTime(offsetTicks,spanTicks))-startTick;
            if(length<=0)throw std::runtime_error("source tempo collapses the visible clip");
            root["content_offset_ticks"]=startTick;
            if(base==0) {
                if(stretch)root["pos_ticks"]=origin;
                root["length_ticks"]=length;
            } else {
                if(stretch)root["pos_ns"]=beginNs;
                root["length_ns"]=time.duration(origin,length);
            }
            if(hasEnd)properties["end_marker_ticks"]=scale(integer(properties,"end_marker_ticks"));
            const auto loopStart=integer(root,"loop_start_ticks");
            root["loop_start_ticks"]=scale(loopStart);
            if(!root.at("loop_len_ticks").is_null()) {
                const auto loopLength=scale(edit::addTime(loopStart,integer(root,"loop_len_ticks")))-scale(loopStart);
                if(loopLength<=0)throw std::runtime_error("source tempo collapses the clip loop");
                root["loop_len_ticks"]=loopLength;
            }
            for(const char* side:{"fade_in_ticks","fade_out_ticks"})root[side]=std::min(scale(integer(root,side)),length);
        } else if(options.enabled) {
            const auto offsetNs=details.at("content_offset_ns").is_null()?time.duration(origin,offsetTicks):
                                integer(details,"content_offset_ns");
            const auto sourceEndNs=details.at("source_end_ns").is_null()?
                (hasEnd?time.duration(origin,integer(properties,"end_marker_ticks")):physicalNs):
                integer(details,"source_end_ns");
            const auto visibleEndNs=std::min({physicalNs,sourceEndNs,edit::addTime(offsetNs,spanNs)});
            const auto firstFrame=frameAtNanos(offsetNs),lastFrame=frameAtNanos(visibleEndNs);
            if(firstFrame<0 || lastFrame<=firstFrame)
                throw std::runtime_error("Warp needs a nonempty source interval");

            std::int64_t trimmed=0;
            if(source.at("warp_markers").is_null()) {
                std::vector<std::uint64_t> onsets;
                if(!requested) {
                    auto detected=audio::detectTransients(*reader,static_cast<std::uint64_t>(start),frames,50,options.stop);
                    cancelled(options.stop);
                    if(!detected)throw std::runtime_error("cannot analyze Warp source transients");
                    onsets=std::move(detected.frames);
                }
                auto analysisOptions=options.analysis;
                // A manual tempo pins frame zero. Only a whole Auto-Warped
                // clip may adopt its detected first beat as the origin.
                if(requested || firstFrame!=0 || lastFrame!=static_cast<std::int64_t>(frames) || hasEnd)
                    analysisOptions.fromFirstBeat=false;
                auto analysis=audio::autoWarpFromOnsets(onsets,frames,reader->sampleRate(),analysisOptions,options.stop);
                cancelled(options.stop);
                if(!analysis)throw std::runtime_error(analysis.error==audio::AutoWarpError::NoTempo?
                    "Warp needs a detected beat. Enter the source BPM in the Clip View.":"source timing cannot be analyzed with these Warp options");
                source["warp_markers"]=capturedWarpMap(analysis.markers,frames-analysis.trimFrames);
                trimmed=static_cast<std::int64_t>(analysis.trimFrames);
                source["src_start_frames"]=edit::addTime(start,trimmed);
                source["src_len_frames"]=static_cast<std::int64_t>(frames-analysis.trimFrames);
                markers=std::move(analysis.markers);
            } else {
                markers=storedMap().markers;
                if(requested) {
                    correctWarpMap(markers,reader->sampleRate(),*requested);
                    source["warp_markers"]=capturedWarpMap(markers,frames);
                }
            }
            const auto tickAt=[&](std::int64_t frame) {
                return mapPosition(markers,frame-trimmed,&WarpMarker::sourceFrame,&WarpMarker::tick);
            };
            const auto first=tickAt(firstFrame),last=tickAt(lastFrame);
            if(last<=first)throw std::runtime_error("Warp source interval is shorter than one tick");

            root["content_offset_ticks"]=first;
            root["length_ticks"]=last-first;
            if(hasEnd || sourceEndNs<physicalNs) {
                if(properties.is_null())properties=edit::defaultRow(store.db(),"clip_properties",{{"clip_id",clipId}});
                properties["end_marker_ticks"]=tickAt(frameAtNanos(sourceEndNs));
            }
            source["warp_mode"]=mode;
            details["content_offset_ns"]=nullptr;details["source_end_ns"]=nullptr;
        } else {
            const auto map=storedMap();
            const auto frameAt=[&](std::int64_t ticks) {
                return mapPosition(map.markers,ticks,&WarpMarker::tick,&WarpMarker::sourceFrame);
            };
            const auto sourceLimit = map.markers.back().tick;
            auto sourceStart = offsetTicks;
            if (looping) {
                const auto begin = integer(root, "loop_start_ticks");
                const auto length = root.at("loop_len_ticks").is_null() ? 0 : integer(root, "loop_len_ticks");
                if (begin >= 0 && length > 0 && begin <= sourceLimit && length <= sourceLimit - begin) {
                    // Splitting a repeat stores an unrolled offset. Start at the
                    // same sample when the loop is switched off with Warp.
                    if (sourceStart >= begin)
                        sourceStart = begin + (sourceStart - begin) % length;
                } else {
                    root["loop_start_ticks"] = 0;
                    root["loop_len_ticks"] = nullptr;
                }
                root["loop_enabled"] = 0;
            }
            auto sourceEnd=hasEnd?integer(properties,"end_marker_ticks"):sourceLimit;
            if (looping && sourceEnd <= sourceStart)
                sourceEnd = sourceLimit;
            const auto visibleEnd=edit::addTime(sourceStart,base==0?integer(root,"length_ticks"):
                                              time.ticks(edit::addTime(beginNs,spanNs))-origin);
            const auto offsetNs=nanosAtFrame(frameAt(sourceStart));
            const auto lastNs=nanosAtFrame(frameAt(std::min(sourceEnd,visibleEnd)));
            const auto endNs=nanosAtFrame(frameAt(sourceEnd));
            if(lastNs<=offsetNs)throw std::runtime_error("Warp off needs a nonempty source interval");
            root["length_ticks"]=std::max<std::int64_t>(1,physicalToTicks(lastNs-offsetNs));
            root["content_offset_ticks"]=physicalToTicks(offsetNs);
            if(hasEnd)properties["end_marker_ticks"]=std::max(physicalToTicks(endNs),integer(root,"content_offset_ticks")+1);
            details["content_offset_ns"]=offsetNs;
            details["source_end_ns"]=endNs;
        }
        if(options.enabled) {
            engine::TempoMap tempo;SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
            while(points.executeStep())tempo.events.push_back({points.getColumn(0).getInt64(),points.getColumn(1).getDouble(),points.getColumn(2).getInt()});
            if(tempo.events.empty())tempo.events.push_back({0,120,0});
            if(engine::projectWarpAnchors(markers,tempo,origin-integer(root,"content_offset_ticks"),rate).back().outputFrame>0x7fffffffULL)
                throw std::runtime_error("warped clip exceeds supported duration");
        }
        if(!retiming) {
            root["time_base"]=0;root["pos_ticks"]=origin;root["pos_ns"]=nullptr;root["length_ns"]=nullptr;
            for(const char* side:{"fade_in_ticks","fade_out_ticks"})root[side]=std::min(integer(root,side),integer(root,"length_ticks"));
            for(const char* column:{"loop_begin_ns","loop_end_ns","in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"})details[column]=nullptr;
        }
        source["warp_enabled"]=options.enabled?1:0;
        edit::RowEdits plan(store.db());plan.set("clips",clipId,root);plan.set("audio_clips",clipId,source);plan.set("clip_playback_details",clipId,details);
        if(!properties.is_null())plan.set("clip_properties",clipId,properties);
        for(auto domain:image.at("children").at("clip_warp_fade_domains")) {
            for(const char* column:{"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"})
                if(!retiming)domain[column]=nullptr;
                else if(!domain.at(column).is_null())domain[column]=scaleWarpTicks(integer(domain,column),ratio);
            plan.set("clip_warp_fade_domains",clipId,domain);
        }
        capture(plan,stretch?"Stretch Clip":retiming?"Change Source Tempo":options.enabled?"Enable Warp":"Disable Warp");
        if(!stretch)
            result.options.beforeCommit=[file,expected=std::move(expectedHash),stop=options.stop](SQLite::Database&,std::int64_t,std::string& error) {
                if(stop.stop_requested()){error="Warp edit cancelled";return false;}
                const auto actual=media::blake3File(file,stop);
                if(!actual || actual.hex!=expected){error="Warp source changed after analysis";return false;}
                if(stop.stop_requested()){error="Warp edit cancelled";return false;}return true;
            };
        cancelled(options.stop);result.ok=true;
    } catch(const std::exception& exception){result.error=exception.what();}
    return result;
}
}
PreparedEdit prepareSetWarp(Store& store,std::int64_t clipId,const WarpOptions& options,double rate) {
    return prepareWarpChange(store,clipId,options,rate);
}
PreparedEdit prepareStretchWarpedClip(Store& store,std::int64_t clipId,std::int64_t position,std::int64_t length,double rate) {
    return prepareWarpChange(store,clipId,WarpOptions{},rate,WarpStretch{position,length});
}
namespace {
std::vector<std::uint64_t> sourceTransientFrames(const Payload& source,std::span<const std::uint64_t> fileFrames) {
    const auto first=static_cast<std::uint64_t>(integer(source,"src_start_frames"));
    const auto length=static_cast<std::uint64_t>(integer(source,"src_len_frames"));
    const bool reverse=integer(source,"reverse")!=0;
    std::vector<std::uint64_t> frames;
    for(const auto frame:fileFrames) {
        if(frame<first || frame-first>=length)continue;
        const auto local=frame-first;
        frames.push_back(reverse?length-1-local:local);
    }
    std::ranges::sort(frames);
    frames.erase(std::unique(frames.begin(),frames.end()),frames.end());
    return frames;
}
void promoteTransients(std::vector<WarpMarker>& markers,const Payload& source,
                       std::span<const std::uint64_t> fileFrames) {
    const auto interior=sourceTransientFrames(source,fileFrames);
    if(interior.empty())return;
    std::vector<WarpMarker> expanded;
    expanded.reserve(markers.size()+interior.size());
    auto onset=interior.begin();
    expanded.push_back(markers.front());
    for(std::size_t i=1;i<markers.size();++i) {
        const auto& right=markers[i];
        const auto segment=std::span<const WarpMarker>(markers).subspan(i-1,2);
        while(onset!=interior.end() && *onset<static_cast<std::uint64_t>(right.sourceFrame)) {
            const auto frame=static_cast<std::int64_t>(*onset++);
            if(frame<=expanded.back().sourceFrame)continue;
            const auto tick=mapPosition(segment,frame,&WarpMarker::sourceFrame,&WarpMarker::tick);
            if(tick>expanded.back().tick && tick<right.tick)expanded.push_back({frame,tick});
        }
        expanded.push_back(right);
    }
    markers=std::move(expanded);
}
template<class Transform>
PreparedEdit prepareWarpMapEdit(Store& store,std::int64_t clipId,double referenceRate,const char* label,
                                Transform transform) {
    PreparedEdit result;
    try {
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(!std::isfinite(referenceRate) || referenceRate<44100 || referenceRate>768000)
            throw std::runtime_error("invalid Warp session rate");
        Read read(store.db());
        result.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement location(store.db(),"PRAGMA database_list");
        while(location.executeStep())
            if(location.getColumn(1).getString()=="main")result.sourceFile=location.getColumn(2).getString();
        OpContext context{store,store.db()};
        auto image=captureImage(context,clipId);
        const auto& root=image.at("row");
        const auto& sources=image.at("children").at("audio_clips");
        if(root.at("kind")!="audio" || !root.at("alias_of").is_null() || sources.size()!=1)
            throw std::runtime_error("Warp markers need an independent audio clip");
        auto source=sources.front();
        if(integer(source,"warp_enabled")==0 || !source.at("warp_markers").is_binary())
            throw std::runtime_error("Enable Warp before editing its markers.");
        const auto frames=integer(source,"src_len_frames");
        const auto& data=source.at("warp_markers").get_binary();
        auto map=audio::readWarpMap(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),data.size()),
            static_cast<std::uint64_t>(frames));
        if(!map || map.futureLayout)throw std::runtime_error("Warp map is unreadable or in a newer format");
        edit::RowEdits plan(store.db());
        transform(map.markers,source,image,plan);
        source["warp_markers"]=capturedWarpMap(map.markers,static_cast<std::uint64_t>(frames));
        engine::TempoMap tempo;
        SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
        while(points.executeStep())
            tempo.events.push_back({points.getColumn(0).getInt64(),points.getColumn(1).getDouble(),points.getColumn(2).getInt()});
        if(tempo.events.empty() || tempo.events.front().posTicks!=0)tempo.events.insert(tempo.events.begin(),{0,120,0});
        const auto origin=integer(root,"time_base")==0?integer(root,"pos_ticks"):
            tempo.storedNanosecondsToTicks(integer(root,"pos_ns"));
        const auto offset=integer(root,"content_offset_ticks");
        if(offset<0 || engine::projectWarpAnchors(map.markers,tempo,origin-offset,referenceRate).back().outputFrame>0x7fffffffULL)
            throw std::runtime_error("warped clip exceeds supported duration");
        plan.set("audio_clips",clipId,std::move(source));
        auto changes=plan.changes();
        if(!changes.empty()) {
            OpRequest op;
            op.opType="arranger.splice";
            op.payload={{"id",clipId},{"changes",std::move(changes)}};
            op.targetKind="clip";op.targetId=clipId;
            op.label=label;
            result.requests.push_back(std::move(op));
        }
        result.ok=true;
    } catch(const std::exception& error) {result.error=error.what();}
    return result;
}
}
PreparedEdit prepareWarpMarkerEdit(Store& store,std::int64_t clipId,WarpMarkerChange change,
                                  WarpMarker marker,double referenceRate) {
    const auto* label=change==WarpMarkerChange::Add?"Add Warp Marker":
        change==WarpMarkerChange::Move?"Move Warp Marker":"Delete Warp Marker";
    return prepareWarpMapEdit(store,clipId,referenceRate,label,[&](auto& markers,const Payload&,Payload&,edit::RowEdits&) {
        if(marker.sourceFrame<=0 || marker.sourceFrame>=markers.back().sourceFrame)
            throw std::runtime_error("The Warp map's first and last anchors are fixed.");
        const auto it=std::ranges::lower_bound(markers,marker.sourceFrame,{},&WarpMarker::sourceFrame);
        const bool exists=it!=markers.end() && it->sourceFrame==marker.sourceFrame;
        if(change==WarpMarkerChange::Add) {
            if(exists)throw std::runtime_error("There is already a Warp marker on this source frame.");
            markers.insert(it,marker);
        } else {
            if(!exists)throw std::runtime_error("The Warp marker no longer exists.");
            if(change==WarpMarkerChange::Move)it->tick=marker.tick;
            else markers.erase(it);
        }
    });
}
PreparedEdit prepareQuantizeWarpMarkers(Store& store,std::int64_t clipId,std::int64_t gridTicks,double referenceRate,
                                        std::span<const std::uint64_t> fileFrames) {
    return prepareWarpMapEdit(store,clipId,referenceRate,"Quantize Audio",[&](auto& markers,const Payload& source,Payload&,edit::RowEdits&) {
        if(gridTicks<=0 || gridTicks>(std::int64_t{1}<<50))
            throw std::runtime_error("The audio quantize grid is out of range.");
        if(!fileFrames.empty())promoteTransients(markers,source,fileFrames);
        for(std::size_t i=1;i+1<markers.size();++i) {
            const auto tick=markers[i].tick;
            const auto within=tick%gridTicks;
            const auto snapped=tick-within+(within>=(gridTicks+1)/2?gridTicks:0);
            // Keep clips valid when close markers would otherwise land on the same grid point.
            markers[i].tick=std::clamp(snapped,markers[i-1].tick+1,markers[i+1].tick-1);
        }
    });
}
PreparedEdit prepareWarpMarkersAtTransients(Store& store,std::int64_t clipId,std::span<const std::uint64_t> fileFrames,
                                           double referenceRate) {
    return prepareWarpMapEdit(store,clipId,referenceRate,"Add Warp Markers at Transients",
        [&](auto& markers,const Payload& source,Payload&,edit::RowEdits&) {
            promoteTransients(markers,source,fileFrames);
        });
}
PreparedEdit prepareWarpFromHere(Store& store,std::int64_t clipId,std::int64_t startTick,
                                std::span<const std::uint64_t> fileFrames,audio::AutoWarpOptions options,
                                double referenceRate) {
    options.fromFirstBeat=false;
    return prepareWarpMapEdit(store,clipId,referenceRate,"Warp From Here",
        [&](auto& markers,const Payload& source,Payload&,edit::RowEdits&) {
            if(startTick<0 || startTick>=markers.back().tick)
                throw std::runtime_error("Choose a point before the end of the sample.");
            const auto startFrame=mapPosition(markers,startTick,&WarpMarker::tick,&WarpMarker::sourceFrame);
            const auto sourceEnd=markers.back().sourceFrame;
            const auto right=std::ranges::lower_bound(markers,startTick,{},&WarpMarker::tick);
            if(right->tick!=startTick &&
               (startFrame<=std::prev(right)->sourceFrame || startFrame>=right->sourceFrame))
                throw std::runtime_error("The Warp anchor is too close to an existing marker.");
            SQLite::Statement media(store.db(),"SELECT sample_rate FROM media_files WHERE id=?");
            media.bind(1,integer(source,"media_id"));
            const auto rate=media.executeStep()?media.getColumn(0).getInt64():0;
            if(rate<8000 || rate>768000)
                throw std::runtime_error("The Warp source has no supported sample rate.");
            std::vector<std::uint64_t> suffixOnsets;
            if(!options.sourceBpm) {
                suffixOnsets=sourceTransientFrames(source,fileFrames);
                const auto first=static_cast<std::uint64_t>(startFrame);
                suffixOnsets.erase(suffixOnsets.begin(),std::ranges::lower_bound(suffixOnsets,first));
                for(auto& frame:suffixOnsets)frame-=first;
            }
            const auto suffix=audio::autoWarpFromOnsets(suffixOnsets,static_cast<std::uint64_t>(sourceEnd-startFrame),
                                                        static_cast<std::uint32_t>(rate),options);
            if(suffix.error==audio::AutoWarpError::NoTempo)
                throw std::runtime_error("No steady tempo was found after this point. Use the Set BPM Warp command.");
            if(!suffix || suffix.markers.size()<2 || suffix.markers.back().tick>std::numeric_limits<std::int64_t>::max()-startTick)
                throw std::runtime_error("The Warp suffix cannot be represented.");
            markers.erase(right,markers.end());
            markers.push_back({startFrame,startTick});
            const bool straight=!options.detectTempoChanges || options.sourceBpm.has_value();
            for(std::size_t i=straight?suffix.markers.size()-1:1;i<suffix.markers.size();++i) {
                const auto& marker=suffix.markers[i];
                markers.push_back({startFrame+marker.sourceFrame,startTick+marker.tick});
            }
        });
}
PreparedEdit prepareWarpAsLoop(Store& store,std::int64_t clipId,std::optional<WarpLoopSelection> selection,
                              std::int64_t lengthTicks,double referenceRate) {
    return prepareWarpMapEdit(store,clipId,referenceRate,selection?"Warp Selection as Loop":"Warp Sample as Loop",
        [&](auto& markers,const Payload&,Payload& image,edit::RowEdits& plan) {
            const auto range=selection.value_or(WarpLoopSelection{0,markers.back().tick});
            if(lengthTicks<=0 || range.from<0 || range.to<=range.from || range.to>markers.back().tick)
                throw std::runtime_error("Choose a nonempty part of the warped source and a positive loop length.");
            const auto fromFrame=mapPosition(markers,range.from,&WarpMarker::tick,&WarpMarker::sourceFrame);
            const auto toFrame=mapPosition(markers,range.to,&WarpMarker::tick,&WarpMarker::sourceFrame);
            if(toFrame<=fromFrame)throw std::runtime_error("The selection is shorter than one source frame.");
            const auto insertAnchor=[&](std::int64_t tick,std::int64_t frame) {
                const auto next=std::ranges::lower_bound(markers,tick,{},&WarpMarker::tick);
                if(next->tick==tick)return;
                if(frame<=std::prev(next)->sourceFrame || frame>=next->sourceFrame)
                    throw std::runtime_error("The selection boundary is too close to an existing Warp marker.");
                markers.insert(next,{frame,tick});
            };
            insertAnchor(range.from,fromFrame);
            insertAnchor(range.to,toFrame);
            const auto newEnd=edit::addTime(range.from,lengthTicks);
            const auto oldLength=range.to-range.from;
            const auto ratio=static_cast<long double>(lengthTicks)/static_cast<long double>(oldLength);
            for(auto& marker:markers) {
                if(marker.tick>=range.to)
                    marker.tick=edit::addTime(marker.tick,lengthTicks-oldLength);
                else if(marker.tick>range.from)
                    marker.tick=edit::addTime(range.from,scaleWarpTicks(marker.tick-range.from,ratio));
            }

            auto& root=image.at("row");
            const auto oldOffset=integer(root,"content_offset_ticks");
            if(oldOffset<0)throw std::runtime_error("The clip has an invalid Warp source offset.");
            const auto shift=range.from-oldOffset;
            root["content_offset_ticks"]=range.from;
            root["loop_enabled"]=1;
            root["loop_start_ticks"]=range.from;
            root["loop_len_ticks"]=lengthTicks;
            plan.set("clips",clipId,root);
            const auto& children=image.at("children");
            const auto& properties=children.at("clip_properties");
            auto property=properties.empty()?edit::defaultRow(store.db(),"clip_properties",{{"clip_id",clipId}}):properties.front();
            property["end_marker_ticks"]=newEnd;
            plan.set("clip_properties",clipId,std::move(property));
            const auto& fades=children.at("clip_warp_fade_domains");
            if(shift!=0 && !fades.empty()) {
                auto row=fades.front();
                for(const auto* field:{"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"})
                    if(!row.at(field).is_null()) {
                        const auto tick=edit::addTime(integer(row,field),shift);
                        if(tick<0)throw std::runtime_error("The new loop would put a retained fade before the source start.");
                        row[field]=tick;
                    }
                plan.set("clip_warp_fade_domains",clipId,std::move(row));
            }
        });
}
}
