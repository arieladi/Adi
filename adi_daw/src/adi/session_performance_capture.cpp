// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/session_performance_capture.hpp"

#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_time.hpp"
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
#include <map>
#include <stdexcept>
#include <utility>

namespace adi {

struct SessionPerformanceSource::Data {
    struct SourceFile {
        std::filesystem::path path;
        std::optional<media::FileRevision> revision;
    };
    AppliedSource source;
    std::optional<std::int64_t> head;
    engine::TempoMap clock;
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
        if (!std::isfinite(bpm) || bpm < 20 || bpm > 999 || tempo.getColumn(2).getInt() != 0 ||
            (!clock.events.empty() && bpm != clock.events.front().bpm))
            throw std::runtime_error("exact Session capture needs a constant authored tempo");
        clock.events.push_back({tick, bpm, 0});
    }
    if (clock.events.empty()) clock.events.push_back({0, 120, 0});
    // Identical tempo nodes do not change musical time. Keep their authored
    // timestamps, since the Arrangement renderer also uses their rounding.
    return clock;
}

std::int64_t sampleAt(const engine::TempoMap& clock, std::int64_t tick, std::uint32_t rate) {
    if (tick < 0 || tick > clockLimit)
        throw std::runtime_error("Session capture exceeds musical clock precision");
    const auto sample = clock.ticksToSeconds(tick) * rate;
    if (!std::isfinite(sample) || sample < 0 || sample > static_cast<double>(clockLimit))
        throw std::runtime_error("Session capture exceeds sample clock precision");
    return std::llround(sample);
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
    std::int64_t end = 0, loopBegin = 0, loopEnd = 0;
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
                   std::uint32_t rate, clip_edit::CopyIds& ids) {
    const auto& source = image.at("row");
    if (!source.at("alias_of").is_null())
        throw std::runtime_error("Session capture needs an independent source clip image");
    const bool midi = source.at("kind") == "midi";
    const bool muted = source.at("muted") == 1;
    const auto* audio = firstChild(image, "audio_clips");
    if (!midi && !audio) throw std::runtime_error("Session capture has no authored audio source window");
    if (!muted && (!std::isfinite(span.velocity) || span.velocity != 1.0f || (!midi && span.fade)))
        throw std::runtime_error("Session launch Fade or velocity needs an exact capture render contract");
    if (!midi && !muted && source.at("gain_db") != 0)
        throw std::runtime_error("authored Session audio gain needs an exact capture render contract");
    const auto& sourceChildren = image.at("children");
    if (!muted && (!sourceChildren.at("automation_data").empty() ||
                   !sourceChildren.at("automation_segment_ranges").empty()))
        throw std::runtime_error("Session clip envelopes are not present in this performance trace");
    if (midi && !muted && (!sourceChildren.at("note_expression").empty() ||
        std::any_of(sourceChildren.at("event_streams").begin(), sourceChildren.at("event_streams").end(),
            [](const auto& stream) { return stream.at("stream_kind") == "cc"; })))
        throw std::runtime_error("Session MIDI controller or expression playback is not present in this trace");
    const bool warped = audio && audio->at("warp_enabled") == 1;
    const auto extent = sourceExtent(image, fixed, !midi && !warped);
    if (span.sourceTick < 0 || span.sourceTick >= (extent.loopEnd > 0 ? extent.loopEnd : extent.end))
        throw std::runtime_error("performed Session source phase is outside the frozen clip");
    if (midi) {
        if (extent.loopEnd <= 0 && span.end.tick - span.begin.tick > extent.end - span.sourceTick)
            throw std::runtime_error("Session MIDI runs past its exactly representable source range");
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
    clip_edit::PreparedEdit refused;
    try {
        if (audio::onAudioThread || store.readOnly() || !source.data_ ||
            !source.data_->source.sameConnection(appliedSource(store)))
            throw std::runtime_error("Session performance belongs to another open project or driver");
        if (capture.error != engine::LaunchCaptureError::None || rate < 44100 || rate > 768000)
            throw std::runtime_error("Session performance trace is invalid at this rate");
        Read read(store.db());
        const auto& frozen = *source.data_;
        if (OpJournal(store).headSeq() != frozen.head || captureClock(store.db()).events != frozen.clock.events)
            throw std::runtime_error("project material or tempo changed during Session capture");
        if (capture.spans.empty()) throw std::runtime_error("no Session material was performed in this capture");

        const engine::TempoMap fixed{{{0, frozen.clock.bpmAt(0), 0}}};
        clip_edit::CopyIds ids(store.db());
        edit_plan::detail::PlacementBatch placement;
        std::map<std::int64_t, engine::LaunchCaptureTime> trackEnds;
        std::map<std::filesystem::path, media::FileRevision> mediaProofs;
        auto first = clockLimit, last = std::int64_t{0};
        for (const auto& span : capture.spans) {
            if (span.track <= 0 || span.clip < 0 || span.begin.tick >= span.end.tick ||
                span.begin.sample >= span.end.sample ||
                sampleAt(frozen.clock, span.begin.tick, rate) != span.begin.sample ||
                sampleAt(frozen.clock, span.end.tick, rate) != span.end.sample)
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
            placement.incoming.push_back(arrangeRun(store, image->second, span, frozen.clock, fixed, rate, ids));
            if (const auto* audio = firstChild(image->second, "audio_clips"); audio && image->second.at("row").at("muted") == 0) {
                const auto file = frozen.mediaFiles.find(detail::signedInteger(audio->at("media_id")));
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
        auto scope = edit_plan::materialScope(tracks, PlacementClock::Ticks, {first, last});
        scope.domains = edit_plan::Domains::Clips;
        edit_plan::Options options{static_cast<double>(rate), {}};
        options.policy.automationFollowsClips = false;
        auto result = edit_plan::detail::prepareCaptured(store, scope, std::move(placement), options, ids,
            "Capture Session Performance");
        if (!result.ok || result.requests.empty()) return result;
        if (result.requests.size() != 1 || result.requests.front().opType != "arranger.splice")
            throw std::runtime_error("unexpected Arrangement capture packet");
        auto& request = result.requests.front();
        OpContext context{store, store.db()};
        validateCaptureRows(context, request.payload.at("changes"));
        request.opType = operationName;
        request.payload.erase("id");
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
    static constexpr Field fields[]{{"changes", FieldType::Array, true, "Literal Arrangement clip row changes"}};
    static constexpr OpDescriptor descriptor[]{
        {operationName, "Capture a recorded Session performance into Arrangement", Scope::Edit,
         EngineImpact::GraphRebuild, fields, false, false, applyCapture, inverseCapture, ""},
    };
    return descriptor;
}

} // namespace adi
