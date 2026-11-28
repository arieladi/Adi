// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/clip_details.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace adi::engine {
bool detail::displayableNote(const NoteRecord& note) noexcept {
    return note.dur_ticks > 0 && note.start_ticks <= std::numeric_limits<std::int64_t>::max() - note.dur_ticks &&
           note.key <= 127 && note.vel_on != 0 && note.vel_on <= 127 && note.vel_off <= 127 &&
           note.channel <= 15 && note.probability <= 10000 && std::isfinite(note.tuning_cents) && note.reserved == 0;
}

namespace {
std::optional<std::int64_t> integer(const SQLite::Statement& query, int column) {
    if (query.getColumn(column).isNull()) return {};
    return query.getColumn(column).getInt64();
}
std::optional<std::int64_t> add(std::int64_t a, std::int64_t b) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return {};
    return a + b;
}
std::optional<std::int64_t> ticksAt(const TempoMap& tempo, std::int64_t nanos) {
    double maxBpm = 0;
    for (const auto& event : tempo.events) {
        if (!std::isfinite(event.bpm) || event.bpm <= 0 || event.posTicks < 0) return {};
        maxBpm = std::max(maxBpm, event.bpm);
    }
    const long double bound = static_cast<long double>(nanos) * 1e-9L * maxBpm * 5765760.0L / 60.0L;
    // Leave rounding room before TempoMap's float-to-int conversion. This only
    // affects derived musical context; raw positions are always exposed.
    if (nanos < 0 || !std::isfinite(bound) || bound >= 0x1p62L) return {};
    return tempo.storedNanosecondsToTicks(nanos);
}
void readNotes(ClipDetails& clip, SQLite::Database& db) {
    SQLite::Statement query(db, "SELECT data FROM event_streams WHERE clip_id=? AND stream_kind='notes' "
                                "AND channel IS NULL AND controller IS NULL");
    query.bind(1, clip.id);
    if (!query.executeStep()) return; // no stream is an empty clip
    const auto data = query.getColumn(0);
    if (data.getBytes() > 0) {
        const auto* begin = static_cast<const std::byte*>(data.getBlob());
        clip.encodedNotes.assign(begin, begin + data.getBytes());
    }
    const StreamReader<NoteRecord> reader(clip.encodedNotes, FourCC::Notes);
    if (!reader.ok()) { clip.notesProblem = toString(reader.error()); return; }
    clip.notesRecordSize = reader.header().rec_size;
    if ((reader.header().flags & ~StreamFlags::SortedByTime) != 0) {
        clip.notesProblem = "unsupported note stream flags"; return;
    }
    std::set<std::uint64_t> identities;
    bool ambiguous = false;
    clip.notes.reserve(reader.count());
    for (std::uint32_t index = 0; index < reader.count(); ++index) {
        const auto note = reader.at(index);
        if (!note || !detail::displayableNote(*note)) {
            clip.notes.clear(); clip.notesProblem = "invalid note " + std::to_string(index); return;
        }
        ambiguous = ambiguous || note->note_id == 0 || !identities.insert(note->note_id).second;
        clip.notes.push_back({note->note_id, note->start_ticks, note->dur_ticks, note->key, note->vel_on,
            note->vel_off, note->channel, note->flags, note->probability, note->tuning_cents,
            (note->flags & NoteFlags::Muted) == 0});
    }
    if (ambiguous) clip.notesProblem = "unassigned or duplicate note ids; do not address these notes by a display index";
}
bool expressionValue(std::int64_t dimension,float value) {
    if(!std::isfinite(value))return false;
    switch(dimension) {
        case static_cast<std::int64_t>(ExpressionDim::Pitch): return value>=-48.0f && value<=48.0f;
        case static_cast<std::int64_t>(ExpressionDim::Pressure):
        case static_cast<std::int64_t>(ExpressionDim::Timbre): return value>=0.0f && value<=1.0f;
        case static_cast<std::int64_t>(ExpressionDim::Pan): return value>=-1.0f && value<=1.0f;
        default: return true;
    }
}

NoteExpressionCurve readExpressionRow(const SQLite::Statement& row) {
    NoteExpressionCurve curve;
    curve.clipId=row.getColumn(1).getInt64();
    curve.noteId=static_cast<std::uint64_t>(row.getColumn(2).getInt64());
    curve.dimension=row.getColumn(3).getInt64();
    if(curve.noteId==0)throw std::runtime_error("missing stable note identity");
    if(curve.dimension<0 || (curve.dimension>static_cast<std::int64_t>(ExpressionDim::Pan) && curve.dimension<64))
        throw std::runtime_error("unassigned expression dimension");
    const auto data=row.getColumn(4);
    const std::span bytes(static_cast<const std::byte*>(data.getBlob()),static_cast<std::size_t>(data.getBytes()));
    const StreamReader<ExpressionPoint> reader(bytes,FourCC::Expression);
    if(!reader.ok())throw std::runtime_error(toString(reader.error()));
    if((reader.header().flags & ~StreamFlags::SortedByTime)!=0)
        throw std::runtime_error("unsupported expression stream flags");
    auto points=reader.all();
    if(!points)throw std::runtime_error("expression point count exceeds the reader budget");
    for(auto& point:*points) {
        if(point.curve>static_cast<std::uint8_t>(Curve::Bezier) || !expressionValue(curve.dimension,point.value) ||
           std::any_of(std::begin(point.reserved),std::end(point.reserved),[](std::uint8_t value){return value!=0;}))
            throw std::runtime_error("invalid expression point");
        point.tension=std::isnan(point.tension)?0.0f:std::clamp(point.tension,-1.0f,1.0f);
    }
    const auto timeOrder=[](const auto& a,const auto& b){return a.time_ticks<b.time_ticks;};
    if((reader.header().flags & StreamFlags::SortedByTime)==0)
        std::sort(points->begin(),points->end(),timeOrder);
    if(std::adjacent_find(points->begin(),points->end(),[&](const auto& a,const auto& b){return !timeOrder(a,b);})!=points->end())
        throw std::runtime_error("expression point times must strictly increase");
    curve.recordSize=reader.recSize();
    curve.points=std::move(*points);
    return curve;
}

void localProperties(SQLite::Database& db, ClipDetails& clip) {
    // N33's additive table can be read before or after its format PR lands.
    // Absence means inherited properties. A present but broken table is a SQL
    // error, never silently treated as an empty project setting.
    SQLite::Statement exists(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='clip_properties' "
                                "UNION ALL SELECT 1 FROM sqlite_temp_master WHERE type='table' AND name='clip_properties'");
    if (!exists.executeStep()) return;
    SQLite::Statement query(db, "SELECT end_marker_ticks,signature_numerator,signature_denominator,scale_root,scale_id,scale_mask "
                               "FROM clip_properties WHERE clip_id=?");
    query.bind(1, clip.id);
    if (!query.executeStep()) return;
    if (!query.getColumn(0).isNull()) { clip.endMarkerTicks = integer(query, 0); clip.endMarkerLocal = true; }
    if (!query.getColumn(1).isNull()) {
        clip.signature = {query.getColumn(1).getInt64(), query.getColumn(2).getInt64(), true};
    }
    if (!query.getColumn(3).isNull()) {
        clip.scale.root = query.getColumn(3).getInt64(); clip.scale.id = query.getColumn(4).getString();
        clip.scale.mask = query.getColumn(5).getInt64(); clip.scale.local = true;
        clip.scale.tuningId.reset(); clip.scale.degrees.clear(); // a local Live scale is 12-TET
    }
}
} // namespace

NoteExpressionsRead readNoteExpressions(const Store& store,std::optional<std::int64_t> clipId) {
    NoteExpressionsRead result;
    try {
        auto& db=const_cast<SQLite::Database&>(store.db());
        SQLite::Statement rows(db,clipId?
            "SELECT id,clip_id,note_id,dimension,data FROM note_expression WHERE clip_id=? ORDER BY note_id,dimension":
            "SELECT id,clip_id,note_id,dimension,data FROM note_expression ORDER BY clip_id,note_id,dimension");
        if(clipId)rows.bind(1,*clipId);
        while(rows.executeStep()) {
            try {result.curves.push_back(readExpressionRow(rows));}
            catch(const std::runtime_error& error) {
                result.problems.push_back("note_expression#"+std::to_string(rows.getColumn(0).getInt64())+" " +error.what());
            }
        }
    } catch(const std::exception& error) {
        result.curves.clear();
        result.problems.push_back(error.what());
    }
    return result;
}

ClipDetailsRead readClipDetails(const Store& store,std::int64_t clipId,bool includeNotes,bool includeExpression) {
    ClipDetailsRead result;
    try {
        auto& db = const_cast<SQLite::Database&>(store.db());
        std::optional<SQLite::Transaction> transaction;
        if(sqlite3_get_autocommit(db.getHandle())!=0)transaction.emplace(db,SQLite::TransactionBehavior::DEFERRED);
        SQLite::Statement query(db,
            "SELECT c.id,track_id,lane_id,kind,name,color,time_base,pos_ticks,pos_ns,length_ticks,length_ns,"
            "loop_enabled,loop_start_ticks,loop_len_ticks,content_offset_ticks,muted,gain_db,"
            "fade_in_ticks,fade_out_ticks,fade_in_curve,fade_out_curve,alias_of,a.transpose_semis,a.warp_enabled,a.warp_mode "
            "FROM clips c LEFT JOIN audio_clips a ON a.clip_id=c.id WHERE c.id=?");
        query.bind(1, clipId);
        if (!query.executeStep()) throw std::runtime_error("no such clip");
        ClipDetails clip;
        clip.id = query.getColumn(0).getInt64(); clip.trackId = query.getColumn(1).getInt64(); clip.laneId = integer(query, 2);
        clip.kind = query.getColumn(3).getString(); clip.name = query.getColumn(4).getString(); clip.color = integer(query, 5);
        clip.timeBase = query.getColumn(6).getInt64(); clip.posTicks = integer(query, 7); clip.posNs = integer(query, 8);
        clip.lengthTicks = integer(query, 9); clip.lengthNs = integer(query, 10);
        clip.loopEnabled = query.getColumn(11).getInt64() != 0; clip.loopStartTicks = query.getColumn(12).getInt64();
        clip.loopLengthTicks = integer(query, 13); clip.contentOffsetTicks = query.getColumn(14).getInt64();
        clip.muted = query.getColumn(15).getInt64() != 0; clip.gainDb = query.getColumn(16).getDouble();
        clip.fadeInTicks = query.getColumn(17).getInt64(); clip.fadeOutTicks = query.getColumn(18).getInt64();
        clip.fadeInCurve = query.getColumn(19).getInt64(); clip.fadeOutCurve = query.getColumn(20).getInt64();
        clip.aliasOf = integer(query, 21); clip.startMarkerTicks = clip.contentOffsetTicks;
        if (!query.getColumn(22).isNull()) {
            clip.transposeSemis = query.getColumn(22).getDouble();
            clip.pitchEnabled = query.getColumn(23).getInt() == 0 || query.getColumn(24).getString() != "re-pitch";
        }
        TempoMap tempo;
        SQLite::Statement tempoQuery(db, "SELECT pos_ticks,bpm,curve FROM tempo_map ORDER BY pos_ticks");
        while (tempoQuery.executeStep()) tempo.events.push_back({tempoQuery.getColumn(0).getInt64(), tempoQuery.getColumn(1).getDouble(), tempoQuery.getColumn(2).getInt()});
        if (tempo.events.empty()) tempo.events.push_back({0,120,0});
        std::optional<std::int64_t> origin = clip.posTicks, duration = clip.lengthTicks;
        if (clip.timeBase == 1) {
            const auto start = clip.posNs.value_or(0);
            const auto endNs = add(start, clip.lengthNs.value_or(0));
            origin = ticksAt(tempo, start);
            const auto end = endNs ? ticksAt(tempo, *endNs) : std::nullopt;
            duration = origin && end ? std::optional<std::int64_t>{*end - *origin} : std::nullopt;
        }
        if (duration) clip.endMarkerTicks = add(clip.startMarkerTicks, *duration);
        if (!origin || !clip.endMarkerTicks) clip.propertiesProblem = "musical marker/context cannot be derived from stored time";
        if (std::any_of(tempo.events.begin(), tempo.events.end(), [](const TempoEvent& event) { return event.curve != 0; }))
            clip.propertiesProblem = "tempo ramps unsupported; musical context uses step tempos";
        if (origin) {
            SQLite::Statement signature(db, "SELECT numerator,denominator FROM time_signature_map WHERE pos_ticks<=? ORDER BY pos_ticks DESC LIMIT 1");
            signature.bind(1, *origin);
            if (signature.executeStep()) clip.signature = {signature.getColumn(0).getInt64(), signature.getColumn(1).getInt64(), false};
            SQLite::Statement key(db, "SELECT id,root,scale_id,scale_mask FROM key_map WHERE pos_ticks<=? ORDER BY pos_ticks DESC LIMIT 1");
            key.bind(1, *origin);
            if (key.executeStep()) {
                clip.scale.root = key.getColumn(1).getInt64(); clip.scale.id = key.getColumn(2).getString(); clip.scale.mask = key.getColumn(3).getInt64();
                SQLite::Statement tuning(db, "SELECT tuning_id FROM key_map_tunings WHERE key_map_id=?"); tuning.bind(1,key.getColumn(0).getInt64());
                if (tuning.executeStep()) {
                    clip.scale.tuningId = tuning.getColumn(0).getInt64();
                    SQLite::Statement degrees(db, "SELECT degree FROM key_map_degrees WHERE key_map_id=? ORDER BY degree"); degrees.bind(1,key.getColumn(0).getInt64());
                    while (degrees.executeStep()) clip.scale.degrees.push_back(degrees.getColumn(0).getInt64());
                }
            }
        }
        localProperties(db, clip);
        if(includeNotes && clip.kind=="midi") {
            readNotes(clip,db);
            if(includeExpression)clip.noteExpressions=readNoteExpressions(store,clip.id);
            if(!clip.noteExpressions.curves.empty()) {
                std::map<std::uint64_t,std::size_t> identities;
                for(const auto& note:clip.notes)++identities[note.id];
                std::erase_if(clip.noteExpressions.curves,[&](const auto& curve) {
                    const auto match=identities.find(curve.noteId);
                    if(match!=identities.end() && match->second==1)return false;
                    clip.noteExpressions.problems.push_back("expression for note "+std::to_string(curve.noteId)+
                        " needs one stable matching note");
                    return true;
                });
            }
        }
        // Finish the active single-row statement before ending the read view.
        query.reset();
        if(transaction)transaction->commit();
        result.clip = std::move(clip);
    } catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
} // namespace adi::engine
