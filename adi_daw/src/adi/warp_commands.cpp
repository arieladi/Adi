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
}
PreparedEdit prepareSetWarp(Store& store,std::int64_t clipId,const WarpOptions& options,double rate) {
    PreparedEdit result;
    try {
        cancelled(options.stop);
        if(store.readOnly())throw std::runtime_error("project is read-only");
        if(!std::isfinite(rate) || rate<44100 || rate>768000 || rate!=std::floor(rate))throw std::runtime_error("invalid Warp session rate");
        if(!audio::warpModeName(options.mode))throw std::runtime_error("unknown Warp mode");
        Read read(store.db());result.options.expectHead.emplace(OpJournal(store).headSeq());
        SQLite::Statement location(store.db(),"PRAGMA database_list");
        while(location.executeStep())if(location.getColumn(1).getString()=="main")result.sourceFile=location.getColumn(2).getString();
        OpContext context{store,store.db()};auto image=captureDeleted(context,clipId);auto root=image.at("row");
        const auto& sources=image.at("children").at("audio_clips");
        if(root.at("kind")!="audio" || !root.at("alias_of").is_null() || sources.size()!=1)throw std::runtime_error("Warp needs an attached, independent audio clip");
        auto source=sources[0];
        if((integer(source,"warp_enabled")!=0)==options.enabled)throw std::runtime_error("clip already has the requested Warp state");
        if(integer(root,"content_offset_ticks")!=0 || integer(root,"loop_enabled")!=0 || integer(source,"reverse")!=0 ||
           source.at("transpose_semis")!=0 || source.at("formant_shift")!=0)
            throw std::runtime_error("Auto-Warp currently needs a clean full-source clip without offsets, loops, reverse or pitch edits");
        if(image.at("children").contains("clip_properties"))for(const auto& properties:image.at("children").at("clip_properties"))
            if(!properties.at("end_marker_ticks").is_null())throw std::runtime_error("Auto-Warp needs a clean clip without an edited End marker");
        auto details=image.at("children").at("clip_playback_details").empty()?
            edit::defaultRow(store.db(),"clip_playback_details",{{"clip_id",clipId}}):image.at("children").at("clip_playback_details")[0];
        if(!details.at("content_offset_ns").is_null() && integer(details,"content_offset_ns")!=0)
            throw std::runtime_error("Auto-Warp needs a clean clip without a physical source offset");
        const auto mediaId=integer(source,"media_id");const auto mediaRow=media::mediaRow(store.db(),mediaId);std::string mediaError;
        const auto file=media::resolveMedia(media::projectFolder(store),mediaRow,mediaError);
        if(file.empty())throw std::runtime_error(mediaError.empty()?"Warp source is missing":mediaError);
        const auto hash=media::blake3File(file,options.stop);cancelled(options.stop);
        if(!hash)throw std::runtime_error("cannot verify Warp source");
        Decoded decoded(file);audio::WavReader reader(decoded.file);
        const auto start=integer(source,"src_start_frames"),storedLength=integer(source,"src_len_frames");
        if(reader.frames()>0x7fffffff || start<0 || storedLength<0 || static_cast<std::uint64_t>(start)>=reader.frames())throw std::runtime_error("Warp source window is outside supported bounds");
        const auto frames=storedLength>0?static_cast<std::uint64_t>(storedLength):reader.frames()-static_cast<std::uint64_t>(start);
        if(frames==0 || frames>reader.frames()-static_cast<std::uint64_t>(start) || reader.channels()>64 || reader.sampleRate()<8000 || reader.sampleRate()>768000)
            throw std::runtime_error("Warp source window is outside supported bounds");
        if(options.enabled && !source.at("warp_markers").is_null()) {
            if(!source.at("warp_markers").is_binary())throw std::runtime_error("stored Warp map has an invalid type");
            const auto& bytes=source.at("warp_markers").get_binary();
            const auto previous=audio::readWarpMap(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),frames);
            if(!previous || previous.futureLayout)throw std::runtime_error("Auto-Warp cannot rewrite an unreadable or future Warp map");
        }
        const auto physicalNs=edit::nanosFromSeconds(static_cast<double>(frames)/reader.sampleRate());
        edit::TimeMap time(store.db());const auto base=integer(root,"time_base");
        const auto origin=base==0?integer(root,"pos_ticks"):time.ticks(integer(root,"pos_ns"));
        const auto beginNs=base==0?time.nanos(origin):integer(root,"pos_ns");
        const auto spanNs=base==0?time.duration(origin,integer(root,"length_ticks")):integer(root,"length_ns");
        const auto oneSampleNs=static_cast<std::int64_t>(std::ceil(1e9/rate));
        if(options.enabled) {
            if(std::abs(static_cast<long double>(spanNs)-physicalNs)>oneSampleNs ||
               (!details.at("source_end_ns").is_null() && std::abs(static_cast<long double>(integer(details,"source_end_ns"))-physicalNs)>oneSampleNs))
                throw std::runtime_error("Auto-Warp currently needs a clean full-source clip");
            const auto onsets=audio::detectTransients(reader,static_cast<std::uint64_t>(start),frames,50,options.stop);cancelled(options.stop);
            if(!onsets)throw std::runtime_error("cannot analyze Warp source transients");
            const auto analysis=audio::autoWarpFromOnsets(onsets.frames,frames,reader.sampleRate(),options.analysis,options.stop);cancelled(options.stop);
            if(!analysis)throw std::runtime_error(analysis.error==audio::AutoWarpError::NoTempo?"Warp needs a detected beat or an explicit source tempo":"source timing cannot be analyzed with these Warp options");
            const auto markers=audio::writeWarpMap(analysis.markers,frames-analysis.trimFrames);
            if(!markers)throw std::runtime_error("cannot capture Warp markers");
            engine::TempoMap tempo;SQLite::Statement points(store.db(),"SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks,id");
            while(points.executeStep())tempo.events.push_back({points.getColumn(0).getInt64(),points.getColumn(1).getDouble(),points.getColumn(2).getInt()});
            if(tempo.events.empty())tempo.events.push_back({0,120,0});
            if(engine::projectWarpAnchors(analysis.markers,tempo,origin,rate).back().outputFrame>0x7fffffffULL)throw std::runtime_error("warped clip exceeds supported duration");
            std::vector<std::uint8_t> bytes(markers.bytes.size());if(!bytes.empty())std::memcpy(bytes.data(),markers.bytes.data(),bytes.size());
            source["warp_markers"]=Payload::binary(std::move(bytes));source["warp_mode"]=audio::warpModeName(options.mode);
            source["src_start_frames"]=edit::addTime(start,static_cast<std::int64_t>(analysis.trimFrames));source["src_len_frames"]=static_cast<std::int64_t>(frames-analysis.trimFrames);
            root["length_ticks"]=analysis.markers.back().tick;
            details["content_offset_ns"]=nullptr;details["source_end_ns"]=nullptr;
        } else {
            if(source.at("warp_markers").is_null() || !source.at("warp_markers").is_binary())throw std::runtime_error("Warp off needs the stored source map");
            const auto& bytes=source.at("warp_markers").get_binary();
            const auto markers=audio::readWarpMap(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),bytes.size()),frames);
            if(!markers || std::abs(static_cast<long double>(spanNs)-time.duration(origin,markers.markers.back().tick))>oneSampleNs)throw std::runtime_error("Warp off currently needs an untrimmed full-source Warp clip");
            root["length_ticks"]=time.ticks(edit::addTime(beginNs,physicalNs))-origin;
            details["content_offset_ns"]=0;details["source_end_ns"]=physicalNs;
        }
        root["time_base"]=0;root["pos_ticks"]=origin;root["pos_ns"]=nullptr;root["length_ns"]=nullptr;
        for(const char* side:{"fade_in_ticks","fade_out_ticks"})root[side]=std::min(integer(root,side),integer(root,"length_ticks"));
        for(const char* column:{"loop_begin_ns","loop_end_ns","in_begin_ns","in_end_ns","out_begin_ns","out_end_ns"})details[column]=nullptr;
        source["warp_enabled"]=options.enabled?1:0;
        edit::RowEdits plan(store.db());plan.set("clips",clipId,root);plan.set("audio_clips",clipId,source);plan.set("clip_playback_details",clipId,details);
        for(auto domain:image.at("children").at("clip_warp_fade_domains")) {
            for(const char* column:{"in_begin_ticks","in_end_ticks","out_begin_ticks","out_end_ticks"})domain[column]=nullptr;
            plan.set("clip_warp_fade_domains",clipId,domain);
        }
        OpRequest request;request.opType="arranger.splice";request.payload={{"id",clipId},{"changes",plan.changes()}};
        request.targetKind="clip";request.targetId=clipId;request.label=options.enabled?"Enable Warp":"Disable Warp";
        result.requests.push_back(std::move(request));
        result.options.beforeCommit=[file,expected=hash.hex,stop=options.stop](SQLite::Database&,std::int64_t,std::string& error) {
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
