// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/session_performance_capture.hpp"

#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/process.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/warp_projection.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/row_capture.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace adi {

namespace {
struct Meter {
    std::int64_t numerator = 4, denominator = 4;
    bool operator==(const Meter&) const = default;
};
using MeterMap = std::map<std::int64_t, Meter>;
}

struct SessionPerformanceSource::Data {
    struct SourceFile {
        std::filesystem::path path;
        std::optional<media::FileRevision> revision;
    };
    AppliedSource source;
    std::optional<std::int64_t> head;
    engine::TempoMap clock;
    MeterMap meters;
    std::map<std::int64_t, Payload> clips;
    std::map<std::int64_t, SourceFile> mediaFiles;
};

namespace {

using edit_plan::PlacementClock;
constexpr std::int64_t clockLimit = std::int64_t{1} << 50;
constexpr std::string_view operationName = "capture.sessionPerformance";

struct Read {
    std::optional<SQLite::Transaction> transaction;
    explicit Read(SQLite::Database& db) {
        if (sqlite3_get_autocommit(db.getHandle()) != 0)
            transaction.emplace(db, SQLite::TransactionBehavior::DEFERRED);
    }
};

engine::TempoMap captureClock(SQLite::Database& db) {
    engine::TempoMap clock;
    SQLite::Statement tempo(db, "SELECT pos_ticks,bpm,curve FROM main.tempo_map ORDER BY pos_ticks");
    while (tempo.executeStep()) {
        const auto tick = tempo.getColumn(0).getInt64();
        const auto bpm = tempo.getColumn(1).getDouble();
        if (clock.events.empty() && tick != 0) clock.events.push_back({0, 120, 0});
        if (!std::isfinite(bpm) || bpm < 20 || bpm > 999 || tempo.getColumn(2).getInt() != 0)
            throw std::runtime_error("exact Session capture needs an authored step-tempo map");
        clock.events.push_back({tick, bpm, 0});
    }
    if (clock.events.empty()) clock.events.push_back({0, 120, 0});
    return clock;
}

MeterMap captureMeters(SQLite::Database& db) {
    MeterMap result;
    SQLite::Statement query(db, "SELECT pos_ticks,numerator,denominator FROM main.time_signature_map ORDER BY pos_ticks");
    while (query.executeStep())
        result.emplace(query.getColumn(0).getInt64(), Meter{query.getColumn(1).getInt64(), query.getColumn(2).getInt64()});
    return result;
}

bool validMeter(Meter meter, std::int64_t maxNumerator, std::int64_t maxDenominator) {
    return meter.numerator >= 1 && meter.numerator <= maxNumerator &&
        meter.denominator >= 1 && meter.denominator <= maxDenominator &&
        (meter.denominator & (meter.denominator - 1)) == 0;
}

std::int64_t sampleAt(const engine::TempoMap& clock, std::int64_t tick, std::uint32_t rate) {
    if (tick < 0 || tick > clockLimit)
        throw std::runtime_error("Session capture exceeds musical clock precision");
    const auto sample = clock.ticksToSeconds(tick) * rate;
    if (!std::isfinite(sample) || sample < 0 || sample > static_cast<double>(clockLimit))
        throw std::runtime_error("Session capture exceeds sample clock precision");
    return std::llround(sample);
}

struct ClockPlan {
    engine::TempoMap clock;
    Payload changes = Payload::array();
};

template<typename Value, typename Fields>
void replaceClockRows(SQLite::Database& db, const std::string& table, std::int64_t end,
                      std::map<std::int64_t, Value> replacements, Fields fields,
                      clip_edit::CopyIds& ids, edit::RowEdits& rows) {
    if (replacements.empty()) return;
    SQLite::Statement points(db, "SELECT id,pos_ticks FROM main." + table + " WHERE pos_ticks>=? AND pos_ticks<=? ORDER BY pos_ticks");
    points.bind(1, replacements.begin()->first);
    points.bind(2, end);
    while (points.executeStep()) {
        const auto id = points.getColumn(0).getInt64();
        const auto tick = points.getColumn(1).getInt64();
        const auto applied = replacements.find(tick);
        if (applied == replacements.end()) {
            if (tick < end) rows.set(table, id, std::nullopt);
        } else {
            auto row = *rows.current(table, id);
            row.update(fields(applied->second));
            rows.set(table, id, std::move(row));
            replacements.erase(applied);
        }
    }
    for (const auto& [tick, value] : replacements) {
        const auto id = ids.next(table);
        auto row = fields(value);
        row["id"] = id;
        row["pos_ticks"] = tick;
        rows.set(table, id, edit::defaultRow(db, table, row));
    }
}

ClockPlan planClock(Store& store, const engine::TempoMap& authored, const MeterMap& meters,
                    const std::optional<SessionPerformanceClock>& performance, std::uint32_t rate,
                    clip_edit::CopyIds& ids) {
    ClockPlan result{authored};
    if (!performance) {
        if (std::any_of(authored.events.begin(), authored.events.end(), [&](const auto& e) {
                return e.bpm != authored.events.front().bpm;
            })) throw std::runtime_error("varying Session tempo needs the performed clock receipts");
        return result;
    }
    const auto& performed = *performance;
    if (performed.end.tick <= performed.begin.tick || performed.end.sample <= performed.begin.sample ||
        !std::isfinite(performed.initialBpm) || performed.initialBpm < 20 || performed.initialBpm > 999 ||
        performed.sceneEdges.size() > 4096 ||
        sampleAt(authored, performed.begin.tick, rate) != performed.begin.sample)
        throw std::runtime_error("Session performance needs exact clock bounds and an initial tempo");

    std::map<std::int64_t, double> replacements;
    MeterMap replacementMeters;
    if (performed.initialBpm != authored.bpmAt(performed.begin.tick))
        replacements.emplace(performed.begin.tick, performed.initialBpm);
    auto previousTick = performed.begin.tick - 1;
    auto previousSample = performed.begin.sample - 1;
    for (const auto& edge : performed.sceneEdges) {
        const bool tempo = edge.tempoBpm != 0;
        const bool meter = edge.signatureNumerator != 0 || edge.signatureDenominator != 0;
        if (!tempo && !meter) continue;
        if (!edge.preparedToken || (tempo && (!std::isfinite(edge.tempoBpm) || edge.tempoBpm < 20 || edge.tempoBpm > 999)) ||
            (meter && !validMeter({edge.signatureNumerator, edge.signatureDenominator}, 99, 16)) ||
            edge.tick <= previousTick || edge.sample <= previousSample ||
            edge.tick > performed.end.tick || edge.sample > performed.end.sample)
            throw std::runtime_error("Session capture needs ordered applied scene clock receipts");
        previousTick = edge.tick;
        previousSample = edge.sample;
        if (edge.tick == performed.end.tick) continue;
        if (tempo) replacements[edge.tick] = edge.tempoBpm;
        if (meter) replacementMeters[edge.tick] = {edge.signatureNumerator, edge.signatureDenominator};
    }

    edit::RowEdits rows(store.db());
    const auto end = performed.end.tick;
    if (!replacements.empty()) {
        const auto first = replacements.begin()->first;
        const auto resumedBpm = authored.bpmAt(end);
        const auto storedStop = std::find_if(authored.events.begin(), authored.events.end(),
            [end](const auto& event) { return event.posTicks == end; });
        if (storedStop == authored.events.end() && replacements.rbegin()->second != resumedBpm)
            replacements.emplace(end, resumedBpm);

        // setExternalTempoAt holds each performed scene's BPM until the next
        // receipt. Only the shadowed authored points inside that interval
        // are replaced. The existing authored point at capture end wins.
        std::map<std::int64_t, double> prospective = replacements;
        for (const auto& point : authored.events)
            if (point.posTicks < first || point.posTicks >= end)
                prospective.try_emplace(point.posTicks, point.bpm);
        result.clock.events.clear();
        for (const auto& [tick, bpm] : prospective) result.clock.events.push_back({tick, bpm, 0});
    }
    replaceClockRows(store.db(), "tempo_map", end, std::move(replacements),
        [](double bpm) { return Payload{{"bpm", bpm}}; }, ids, rows);

    // A performed signature starts a new meter at the launch boundary,
    // even when its numerator and denominator repeat an earlier signature.
    if (!replacementMeters.empty() && !meters.contains(end)) {
        const auto next = meters.upper_bound(end);
        replacementMeters.emplace(end, next == meters.begin() ? Meter{} : std::prev(next)->second);
    }
    replaceClockRows(store.db(), "time_signature_map", end, std::move(replacementMeters), [](Meter meter) {
        return Payload{{"numerator", meter.numerator}, {"denominator", meter.denominator}};
    }, ids, rows);
    result.changes = rows.changes();

    for (const auto& edge : performed.sceneEdges)
        if ((edge.tempoBpm != 0 || edge.signatureNumerator != 0) && sampleAt(result.clock, edge.tick, rate) != edge.sample)
            throw std::runtime_error("performed scene receipt disagrees with the Arrangement clock");
    if (sampleAt(result.clock, performed.end.tick, rate) != performed.end.sample)
        throw std::runtime_error("performed Session end disagrees with the Arrangement clock");
    return result;
}

std::int64_t addTicks(std::int64_t a, std::int64_t b) {
    if (a < 0 || a > clockLimit || b < 0 || b > clockLimit - a)
        throw std::runtime_error("invalid Session clip source extent");
    return a + b;
}

std::optional<std::int64_t> optionalInt(const Payload& row, const char* key) {
    const auto found = row.find(key);
    return found == row.end() || found->is_null() ? std::nullopt : std::optional{detail::signedInteger(*found)};
}

const Payload* firstChild(const Payload& image, const char* table) {
    const auto child = image.at("children").find(table);
    return child == image.at("children").end() || child->empty() ? nullptr : &child->front();
}

struct SourceExtent {
    std::int64_t start = 0, end = 0, loopBegin = 0, loopEnd = 0;
};

SourceExtent sourceExtent(const Payload& image, const engine::TempoMap& fixed, bool unwarped) {
    const auto& row = image.at("row");
    const auto* pins = firstChild(image, "clip_playback_details");
    const auto* props = firstChild(image, "clip_properties");
    const auto sourceTick = [&](const char* property) -> std::optional<std::int64_t> {
        if (!unwarped || !pins) return std::nullopt;
        const auto ns = optionalInt(*pins, property);
        return ns ? std::optional{fixed.storedNanosecondsToTicks(*ns)} : std::nullopt;
    };
    const auto start = sourceTick("content_offset_ns").value_or(detail::signedInteger(row.at("content_offset_ticks")));
    const auto extent = row.at("time_base") == 0 ? optionalInt(row, "length_ticks").value_or(0)
        : fixed.storedNanosecondsToTicks(optionalInt(row, "length_ns").value_or(0));
    auto end = sourceTick("source_end_ns");
    if (!end && props) end = optionalInt(*props, "end_marker_ticks");
    SourceExtent result;
    result.start = start;
    result.end = end ? *end : addTicks(start, extent);
    if (start < 0 || result.end <= start)
        throw std::runtime_error("invalid Session start or end marker");
    if (row.at("loop_enabled") == 1) {
        result.loopBegin = sourceTick("loop_begin_ns").value_or(detail::signedInteger(row.at("loop_start_ticks")));
        const auto loopEnd = sourceTick("loop_end_ns");
        result.loopEnd = loopEnd ? *loopEnd : addTicks(result.loopBegin, optionalInt(row, "loop_len_ticks").value_or(0));
        if (result.loopBegin < 0 || result.loopEnd <= result.loopBegin)
            throw std::runtime_error("invalid Session loop source markers");
    }
    return result;
}

void checkHeldExpression(Store& store, const engine::LaunchCapturedSpan& span, const SourceExtent& extent,
                         const engine::TempoMap& clock, std::uint32_t rate) {
    const auto refuse = [] { throw std::runtime_error("Session expression capture needs an exact, non-looping held-value run at its authored start"); };
    if (extent.loopEnd != 0 || span.sourceTick != extent.start) refuse();
    const auto details = engine::readClipDetails(store, span.clip, true, true);
    if (!details.clip || !details.clip->notesProblem.empty() || !details.clip->noteExpressions.problems.empty()) refuse();

    // Live Session and Arrangement use different floating point evaluation at
    // interior samples. Hold values are invariant, but note and knot edges
    // still have to land on the same side of sample rounding.
    const auto originSeconds = clock.ticksToSeconds(span.begin.tick);
    const auto frame = [&](std::int64_t tick) {
        const auto seconds = clock.ticksToSeconds(tick);
        const auto launched = std::llround(static_cast<long double>(seconds) * rate);
        const auto placed = std::llround(std::nextafter((originSeconds + seconds - originSeconds) * rate,
                                                      std::numeric_limits<double>::infinity()));
        if (launched != placed) refuse();
        return launched;
    };

    const auto sourceEnd = addTicks(extent.start, span.end.tick - span.begin.tick);
    std::map<std::uint64_t, std::pair<std::int64_t, std::int64_t>> audibleNotes;
    std::vector<std::pair<std::int64_t, std::int64_t>> audibleSamples;
    for (const auto& note : details.clip->notes) {
        if (!note.active || note.startTicks >= sourceEnd) continue;
        if (note.startTicks < -clockLimit || note.startTicks > clockLimit ||
            note.durationTicks <= 0 || note.durationTicks > clockLimit) refuse();
        const auto sourceOff = std::min(sourceEnd, note.startTicks + note.durationTicks);
        if (sourceOff <= extent.start) continue;
        if (note.startTicks < extent.start) refuse(); // chased voices need a performed onset receipt
        const auto on = addTicks(span.begin.tick, note.startTicks - extent.start);
        const auto off = addTicks(span.begin.tick, sourceOff - extent.start);
        const auto first = frame(on), last = frame(off);
        if (last <= first || !audibleNotes.emplace(note.id, std::pair{on, off}).second) refuse();
        audibleSamples.emplace_back(first, last);
    }
    std::sort(audibleSamples.begin(), audibleSamples.end());
    if (std::adjacent_find(audibleSamples.begin(), audibleSamples.end(), [](const auto& a, const auto& b) {
            return a.second > b.first;
        }) != audibleSamples.end()) refuse();

    for (const auto& curve : details.clip->noteExpressions.curves) {
        const auto note = audibleNotes.find(curve.noteId);
        if (note == audibleNotes.end()) continue;
        if (curve.dimension < 0 || static_cast<std::size_t>(curve.dimension) >= kStandardExpressionDimensionCount) refuse();
        const auto [on, off] = note->second;
        std::int64_t previousSample = -1;
        for (const auto& point : curve.points) {
            if (point.time_ticks < 0 || point.curve != static_cast<std::uint8_t>(Curve::Hold)) refuse();
            if (point.time_ticks >= off - on) continue;
            const auto sample = frame(on + point.time_ticks);
            if (sample <= previousSample) refuse();
            previousSample = sample;
        }
    }
}

void checkAudioPhase(const engine::LaunchCapturedSpan& span, const SourceExtent& source,
                     const engine::TempoMap& authored, const engine::TempoMap& fixed,
                     std::uint32_t rate, const Payload* warp) {
    const auto start = sampleAt(fixed, span.sourceTick, rate);
    const auto sourceEnd = source.loopEnd > 0 ? source.loopEnd : source.end;
    const auto limit = sampleAt(fixed, sourceEnd, rate);
    const auto frames = span.end.sample - span.begin.sample;
    if (warp) {
        const auto origin = span.begin.tick - span.sourceTick;
        if (engine::warpSourceSamples(authored, origin, span.sourceTick, rate) != start ||
            engine::warpSourceSamples(authored, origin, sourceEnd, rate) != limit ||
            (source.loopEnd > 0 && engine::warpSourceSamples(authored, origin, source.loopBegin, rate) !=
                sampleAt(fixed, source.loopBegin, rate)))
            throw std::runtime_error("Warp source phase cannot be reproduced on the Arrangement clock");
        const auto sourceFrames = detail::signedInteger(warp->at("src_len_frames"));
        const auto& markers = warp->at("warp_markers");
        if (sourceFrames <= 0 || !markers.is_binary())
            throw std::runtime_error("Session Warp source is missing its canonical markers");
        const auto& bytes = markers.get_binary();
        const auto map = audio::readWarpMap(std::as_bytes(std::span(bytes.data(), bytes.size())),
                                           static_cast<std::uint64_t>(sourceFrames));
        if (!map || engine::projectWarpAnchors(map.markers, fixed, 0, rate) !=
                    engine::projectWarpAnchors(map.markers, authored, origin, rate))
            throw std::runtime_error("Session Warp preparation differs on the Arrangement clock");
    }
    if (source.loopEnd <= 0) {
        if (frames > limit - start)
            throw std::runtime_error("Session audio runs past its exactly representable source range");
        return;
    }
    const auto firstWrap = sampleAt(authored, addTicks(span.begin.tick, source.loopEnd - span.sourceTick), rate)
        - span.begin.sample;
    if (frames <= std::min(firstWrap, limit - start)) return;
    const auto loopTicks = source.loopEnd - source.loopBegin;
    const auto loopFrames = static_cast<long double>(loopTicks) * 60 * rate /
        (static_cast<long double>(textproj::kPPQ) * fixed.bpmAt(0));
    if (firstWrap != limit - start || loopFrames != std::round(loopFrames) ||
        loopFrames != limit - sampleAt(fixed, source.loopBegin, rate))
        throw std::runtime_error("fractional Session audio wrap requires a performed source range trace");
}

Payload arrangeRun(Store& store, const Payload& image, const engine::LaunchCapturedSpan& span,
                   const engine::TempoMap& authored, const engine::TempoMap& fixed,
                   std::uint32_t rate, const std::optional<SessionPerformanceAudio>& audioCapture,
                   clip_edit::CopyIds& ids) {
    const auto& source = image.at("row");
    if (!source.at("alias_of").is_null())
        throw std::runtime_error("Session capture needs an independent source clip image");
    const bool midi = source.at("kind") == "midi";
    const bool muted = source.at("muted") == 1;
    const auto* audio = firstChild(image, "audio_clips");
    if (!midi && !audio) throw std::runtime_error("Session capture has no authored audio source window");
    if (!muted && (!std::isfinite(span.velocity) || span.velocity != 1.0f || (!midi && span.fade)))
        throw std::runtime_error("Session launch Fade or velocity needs an exact capture render contract");
    if (!midi && !muted) {
        if (audioCapture && !audioCapture->gainStayedAuthored)
            throw std::runtime_error("transient Session audio gain requires a performed gain trace");
        const auto gainDb = source.at("gain_db").get<double>();
        if (!std::isfinite(gainDb) || std::abs(gainDb) > 120 ||
            (gainDb != 0 && (!audioCapture || audioCapture->precision != engine::Precision::Float32)))
            throw std::runtime_error("authored Session audio gain needs an exact Float32 render receipt");
    }
    if (!midi && !muted && std::any_of(authored.events.begin(), authored.events.end(), [&](const auto& point) {
            return point.posTicks > span.begin.tick && point.posTicks < span.end.tick && point.bpm != fixed.bpmAt(0);
        })) throw std::runtime_error("Session audio spanning a tempo change needs a performed source range trace");
    const auto& sourceChildren = image.at("children");
    if (!muted && (!sourceChildren.at("automation_data").empty() ||
                   !sourceChildren.at("automation_segment_ranges").empty()))
        throw std::runtime_error("Session clip envelopes are not present in this performance trace");
    if (midi && !muted &&
        std::any_of(sourceChildren.at("event_streams").begin(), sourceChildren.at("event_streams").end(),
            [](const auto& stream) { return stream.at("stream_kind") == "cc"; }))
        throw std::runtime_error("Session MIDI controller capture needs exact performed exit values");
    const bool warped = audio && audio->at("warp_enabled") == 1;
    const auto extent = sourceExtent(image, fixed, !midi && !warped);
    if (span.sourceTick < 0 || span.sourceTick >= (extent.loopEnd > 0 ? extent.loopEnd : extent.end))
        throw std::runtime_error("performed Session source phase is outside the frozen clip");
    if (midi) {
        if (extent.loopEnd <= 0 && span.end.tick - span.begin.tick > extent.end - span.sourceTick)
            throw std::runtime_error("Session MIDI runs past its exactly representable source range");
        if (!muted && !sourceChildren.at("note_expression").empty())
            checkHeldExpression(store, span, extent, authored, rate);
    } else if (!muted) {
        checkAudioPhase(span, extent, authored, fixed, rate, warped ? audio : nullptr);
    }

    const auto target = ids.next("clips");
    auto placed = clip_edit::cloneImage(image, target, ids);
    auto& row = placed.at("row");
    auto& children = placed.at("children");
    row["time_base"] = 0;
    row["pos_ticks"] = span.begin.tick;
    row["length_ticks"] = span.end.tick - span.begin.tick;
    row["pos_ns"] = nullptr;
    row["length_ns"] = nullptr;
    row["lane_id"] = nullptr;
    row["content_offset_ticks"] = span.sourceTick;
    if (extent.loopEnd > 0) {
        row["loop_start_ticks"] = extent.loopBegin;
        row["loop_len_ticks"] = extent.loopEnd - extent.loopBegin;
    }
    // The Session renderer reads the source without Arrangement fades.
    row["fade_in_ticks"] = 0;
    row["fade_out_ticks"] = 0;
    children["clip_warp_fade_domains"] = Payload::array();
    auto& details = children["clip_playback_details"];
    if (!midi && !warped && details.empty())
        details.push_back(edit::defaultRow(store.db(), "clip_playback_details", {{"clip_id", target}}));
    if (!details.empty()) {
        auto& pins = details.front();
        for (const auto* key : {"in_begin_ns", "in_end_ns", "out_begin_ns", "out_end_ns"}) pins[key] = nullptr;
        if (!midi && !warped) {
            const auto ns = [&](std::int64_t tick) {
                return edit::nanosFromSeconds(static_cast<double>(sampleAt(fixed, tick, rate)) / rate);
            };
            pins["content_offset_ns"] = ns(span.sourceTick);
            pins["source_end_ns"] = ns(extent.end);
            if (extent.loopEnd > 0) {
                pins["loop_begin_ns"] = ns(extent.loopBegin);
                pins["loop_end_ns"] = ns(extent.loopEnd);
            }
        }
    }
    return placed;
}

void validateCaptureRows(OpContext& context, const Payload& changes) {
    if (!changes.is_array() || changes.empty())
        throw std::runtime_error("Session performance capture needs literal material changes");
    const auto children = clip_edit::childTables();
    const bool hasSlots = context.db.tableExists("clip_slots");
    for (const auto& change : changes) {
        const auto table = change.at("table").get<std::string>();
        if (table == "tempo_map") {
            for (const auto* part : {"before", "after"}) {
                const auto& row = change.at(part);
                if (row.is_null()) continue;
                const auto bpm = row.at("bpm").get<double>();
                if (!std::isfinite(bpm) || bpm < 20 || bpm > 999 || row.at("curve") != 0)
                    throw std::runtime_error("Session capture changes only exact step-tempo rows");
            }
            continue;
        }
        if (table == "time_signature_map") {
            for (const auto* part : {"before", "after"}) {
                const auto& row = change.at(part);
                if (row.is_null()) continue;
                if (!validMeter({detail::signedInteger(row.at("numerator")), detail::signedInteger(row.at("denominator"))}, 255, 128))
                    throw std::runtime_error("Session capture changes only valid time-signature rows");
            }
            continue;
        }
        const bool root = table == "clips";
        if (!root && std::none_of(children.begin(), children.end(), [&table](const auto& child) {
                return table == child.table;
            })) throw std::runtime_error("Session performance capture may only change Arrangement material rows");
        for (const auto* part : {"before", "after"}) {
            const auto& row = change.at(part);
            if (row.is_null()) continue;
            if (root && !row.at("lane_id").is_null())
                throw std::runtime_error("Session performance capture may only change the Arrangement main lane");
            if (hasSlots) {
                SQLite::Statement slot(context.db, "SELECT 1 FROM main.clip_slots WHERE clip_id=?");
                slot.bind(1, detail::signedInteger(row.at(root ? "id" : "clip_id")));
                if (slot.executeStep())
                    throw std::runtime_error("Session performance capture may not edit its source slot clips");
            }
        }
    }
}

bool applyCapture(OpContext& context, const Payload& payload, std::string& error) {
    try {
        validateCaptureRows(context, payload.at("changes"));
        return edit::applyRows(context, payload, error);
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

bool inverseCapture(OpContext& context, const Payload& payload, Payload& inverse, std::string& error) {
    try {
        validateCaptureRows(context, payload.at("changes"));
        return edit::inverseRows(context, payload, inverse, error);
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

} // namespace

std::optional<SessionPerformanceSource> captureSessionPerformanceSource(Store& store, std::string& error) {
    try {
        if (audio::onAudioThread || store.readOnly())
            throw std::runtime_error("Session capture needs a writable project owner");
        Read read(store.db());
        if (!store.db().tableExists("clip_slots")) throw std::runtime_error("Session slot storage is unavailable");
        if (const auto problem = store.essentialExtensionProblem(kSessionSchemaUserVersion); !problem.empty())
            throw std::runtime_error(problem);
        auto source = std::make_shared<SessionPerformanceSource::Data>();
        source->source = appliedSource(store);
        source->head = OpJournal(store).headSeq();
        source->clock = captureClock(store.db());
        source->meters = captureMeters(store.db());
        const auto folder = media::projectFolder(store);
        OpContext context{store, store.db()};
        SQLite::Statement slots(store.db(), "SELECT clip_id FROM main.clip_slots WHERE clip_id IS NOT NULL ORDER BY clip_id");
        while (slots.executeStep()) {
            const auto clip = slots.getColumn(0).getInt64();
            auto image = clip_edit::captureImage(context, clip);
            if (const auto* audio = firstChild(image, "audio_clips"); audio && image.at("row").at("muted") == 0) {
                const auto id = detail::signedInteger(audio->at("media_id"));
                const auto [file, inserted] = source->mediaFiles.try_emplace(id);
                if (inserted) {
                    std::string unavailable;
                    file->second.path = media::resolveMedia(folder, media::mediaRow(store.db(), id), unavailable);
                    if (!file->second.path.empty()) {
                        std::error_code status;
                        file->second.revision = media::readFileRevision(file->second.path, status);
                    }
                }
            }
            source->clips.emplace(clip, std::move(image));
        }
        SessionPerformanceSource result;
        result.data_ = std::move(source);
        error.clear();
        return result;
    } catch (const std::exception& failure) { error = failure.what(); return std::nullopt; }
}

clip_edit::PreparedEdit prepareSessionPerformance(Store& store, const SessionPerformanceSource& source,
    const engine::LaunchCapturePlan& capture, std::uint32_t rate) {
    return prepareSessionPerformance(store, source, capture, rate, std::nullopt);
}

clip_edit::PreparedEdit prepareSessionPerformance(Store& store, const SessionPerformanceSource& source,
    const engine::LaunchCapturePlan& capture, std::uint32_t rate,
    std::optional<SessionPerformanceClock> performed) {
    return prepareSessionPerformance(store, source, capture, rate, performed, std::nullopt);
}

clip_edit::PreparedEdit prepareSessionPerformance(Store& store, const SessionPerformanceSource& source,
    const engine::LaunchCapturePlan& capture, std::uint32_t rate, std::optional<SessionPerformanceClock> performed,
    std::optional<SessionPerformanceAudio> audio) {
    clip_edit::PreparedEdit refused;
    try {
        if (audio::onAudioThread || store.readOnly() || !source.data_ ||
            !source.data_->source.sameConnection(appliedSource(store)))
            throw std::runtime_error("Session performance belongs to another open project or driver");
        if (capture.error == engine::LaunchCaptureError::SourceEdited)
            throw std::runtime_error("live Session gain or region changed during the performance and needs an exact edit trace");
        if (capture.error != engine::LaunchCaptureError::None || rate < 44100 || rate > 768000)
            throw std::runtime_error("Session performance trace is invalid at this rate");
        Read read(store.db());
        const auto& frozen = *source.data_;
        if (OpJournal(store).headSeq() != frozen.head || captureClock(store.db()).events != frozen.clock.events ||
            captureMeters(store.db()) != frozen.meters)
            throw std::runtime_error("project material or clock changed during Session capture");
        clip_edit::CopyIds ids(store.db());
        auto clock = planClock(store, frozen.clock, frozen.meters, performed, rate, ids);
        if (capture.spans.empty() && clock.changes.empty())
            throw std::runtime_error("no Session material or scene clock was performed in this capture");
        edit_plan::detail::PlacementBatch placement;
        std::map<std::int64_t, engine::LaunchCaptureTime> trackEnds;
        std::map<std::filesystem::path, media::FileRevision> mediaProofs;
        auto first = clockLimit, last = std::int64_t{0};
        for (const auto& span : capture.spans) {
            if (span.track <= 0 || span.clip < 0 || span.begin.tick >= span.end.tick ||
                span.begin.sample >= span.end.sample ||
                (performed && (span.begin.tick < performed->begin.tick || span.end.tick > performed->end.tick)) ||
                sampleAt(clock.clock, span.begin.tick, rate) != span.begin.sample ||
                sampleAt(clock.clock, span.end.tick, rate) != span.end.sample)
                throw std::runtime_error("Session performance edge is not sample-exact on the Arrangement clock");
            const auto [entry, inserted] = trackEnds.try_emplace(span.track, span.end);
            if (!inserted) {
                if (span.begin.sample < entry->second.sample || span.begin.tick < entry->second.tick)
                    throw std::runtime_error("Session performance has overlapping or unordered spans on one track");
                entry->second = span.end;
            }
            first = std::min(first, span.begin.tick);
            last = std::max(last, span.end.tick);
            if (span.clip == 0) {
                placement.clear.push_back({{span.track, std::nullopt}, {span.begin.tick, span.end.tick}});
                continue;
            }
            const auto image = frozen.clips.find(span.clip);
            if (image == frozen.clips.end() || image->second.at("row").at("track_id") != span.track)
                throw std::runtime_error("performed clip is absent from the frozen Session source");
            const engine::TempoMap fixed{{{0, clock.clock.bpmAt(span.begin.tick), 0}}};
            placement.incoming.push_back(arrangeRun(store, image->second, span, clock.clock, fixed, rate, audio, ids));
            if (const auto* audioRow = firstChild(image->second, "audio_clips"); audioRow && image->second.at("row").at("muted") == 0) {
                const auto file = frozen.mediaFiles.find(detail::signedInteger(audioRow->at("media_id")));
                if (file == frozen.mediaFiles.end() || !file->second.revision)
                    throw std::runtime_error("performed audio has no verified source file");
                if (mediaProofs.try_emplace(file->second.path, *file->second.revision).second) {
                    std::error_code status;
                    if (media::readFileRevision(file->second.path, status) != file->second.revision)
                        throw std::runtime_error("Session source media changed during the capture");
                }
            }
        }
        std::vector<std::int64_t> tracks;
        tracks.reserve(trackEnds.size());
        for (const auto& [track, end] : trackEnds) { (void)end; tracks.push_back(track); }
        constexpr auto label = "Capture Session Performance";
        clip_edit::PreparedEdit result;
        if (!tracks.empty()) {
            auto scope = edit_plan::materialScope(tracks, PlacementClock::Ticks, {first, last});
            scope.domains = edit_plan::Domains::Clips;
            edit_plan::Options options{static_cast<double>(rate), {}};
            options.policy.automationFollowsClips = false;
            const edit::TimeMap prospective(clock.clock);
            result = edit_plan::detail::prepareCaptured(store, scope, std::move(placement), options, ids,
                label, &prospective);
            if (!result.ok) return result;
        } else {
            result.sourceFile = store.db().execAndGet("SELECT file FROM pragma_database_list WHERE name='main'").getString();
            result.options.expectHead.emplace(frozen.head);
            result.ok = true;
        }
        auto changes = std::move(clock.changes);
        if (!result.requests.empty()) {
            if (result.requests.size() != 1 || result.requests.front().opType != "arranger.splice")
                throw std::runtime_error("unexpected Arrangement capture packet");
            for (auto& change : result.requests.front().payload.at("changes"))
                changes.push_back(std::move(change));
        }
        if (changes.empty()) return result;
        OpContext context{store, store.db()};
        validateCaptureRows(context, changes);
        OpRequest request;
        request.opType = operationName;
        request.label = label;
        request.payload = {{"changes", std::move(changes)}};
        result.requests.clear();
        result.requests.push_back(std::move(request));
        result.description.reset();
        if (!mediaProofs.empty()) {
            result.options.beforeCommit = [files = std::move(mediaProofs)](SQLite::Database&, std::int64_t, std::string& error) {
                for (const auto& [path, proof] : files) {
                    std::error_code status;
                    if (media::readFileRevision(path, status) != proof) {
                        error = "Session source media changed after capture preparation";
                        return false;
                    }
                }
                return true;
            };
        }
        return result;
    } catch (const std::exception& failure) { refused.error = failure.what(); return refused; }
}

std::span<const OpDescriptor> sessionPerformanceOps() noexcept {
    static constexpr Field fields[]{{"changes", FieldType::Array, true, "Literal Arrangement clip, tempo and meter row changes"}};
    static constexpr OpDescriptor descriptor[]{
        {operationName, "Capture a recorded Session performance into Arrangement", Scope::Edit,
         EngineImpact::GraphRebuild, fields, false, false, applyCapture, inverseCapture, ""},
    };
    return descriptor;
}

} // namespace adi
