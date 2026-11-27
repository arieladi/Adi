// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_midi.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/clip_image.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_rows.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace adi::clip_edit {
namespace {
constexpr std::int64_t maxTick = std::int64_t{1} << 50; // capture.createMidi extent
constexpr std::uint64_t maxFrame = std::uint64_t{1} << 50; // exact source frame arithmetic
constexpr std::size_t maxRecords = std::size_t{1} << 20;
constexpr int maxKey = 127;
constexpr std::uint8_t releaseVelocity = 64;

template<class Data>
PitchMidiResult<Data> fail(PitchMidiProblem problem) { return {problem, {}, {}}; }

template<class Data>
PitchMidiResult<Data> fail(PitchMidiProblem problem, std::string detail) {
    return {problem, {}, std::move(detail)};
}

struct CaptureError {
    PitchMidiProblem problem;
    const char* detail;
};

void cancellation(std::stop_token stop) {
    if (stop.stop_requested()) throw CaptureError{PitchMidiProblem::Cancelled, nullptr};
}

std::optional<std::int64_t> integer(const SQLite::Statement& row, int index) {
    const auto cell = row.getColumn(index);
    if (cell.isNull()) return {};
    return cell.getInt64();
}

struct Source {
    engine::ClipDetails clip;
    std::optional<std::int64_t> offsetNs, loopBeginNs, loopEndNs, endNs;
    std::vector<WarpMarker> warp;
    double transposeCents = 0;
};

Source readSource(const Store& store, const media::ClipPitchAnalysis& analysis) {
    const auto& identity = analysis.source;
    if (!audio::validPitchAnalysis(analysis.analysis) || analysis.sourceStartFrame >= analysis.sourceEndFrame ||
        analysis.sourceEndFrame > identity.frames || identity.frames > maxFrame ||
        analysis.analysis.sourceFrames != identity.frames ||
        analysis.analysis.sampleRate != identity.sampleRate || identity.hash.size() != 64 ||
        identity.channels == 0 || identity.channels > 64)
        throw CaptureError{PitchMidiProblem::Source, "invalid source pitch analysis"};

    auto clip = engine::readClipDetails(store, analysis.clipId, false);
    if (!clip.clip || clip.clip->kind != "audio" || clip.clip->aliasOf)
        throw CaptureError{PitchMidiProblem::Source, "pitch-to-MIDI requires an audio clip with its own source"};

    SQLite::Statement row(store.db(),
        "SELECT m.hash_blake3,m.sample_rate,m.channels,m.frames,a.src_start_frames,a.src_len_frames,"
        "a.reverse,a.transpose_semis,a.warp_enabled,"
        "CASE WHEN typeof(a.warp_markers)='blob' AND length(a.warp_markers)<=67108864 THEN a.warp_markers END,"
        "d.content_offset_ns,d.loop_begin_ns,d.loop_end_ns,d.source_end_ns "
        "FROM audio_clips a JOIN media_files m ON m.id=a.media_id "
        "LEFT JOIN clip_playback_details d ON d.clip_id=a.clip_id WHERE a.clip_id=?");
    row.bind(1, analysis.clipId);
    if (!row.executeStep()) throw CaptureError{PitchMidiProblem::Source, "the analyzed audio source is gone"};
    const auto start = row.getColumn(4).getInt64();
    const auto frames = row.getColumn(5).getInt64();
    const auto matches = [&row](int index, auto value) {
        const auto column = integer(row, index);
        return !column || (*column >= 0 && static_cast<std::uint64_t>(*column) == value);
    };
    if (row.getColumn(0).getString() != identity.hash || !matches(1, identity.sampleRate) ||
        !matches(2, identity.channels) || !matches(3, identity.frames) || start < 0 || frames <= 0 ||
        static_cast<std::uint64_t>(start) != analysis.sourceStartFrame ||
        static_cast<std::uint64_t>(frames) != analysis.sourceEndFrame - analysis.sourceStartFrame ||
        (row.getColumn(6).getInt() != 0) != analysis.reversed)
        throw CaptureError{PitchMidiProblem::Source, "the audio source, window or direction changed after analysis"};

    Source result;
    result.clip = std::move(*clip.clip);
    result.transposeCents = row.getColumn(7).getDouble() * 100;
    if (!std::isfinite(result.transposeCents) || std::abs(result.transposeCents) > 9600)
        throw CaptureError{PitchMidiProblem::Source, "invalid clip transpose"};
    if (row.getColumn(8).getInt() != 0) {
        const auto column = row.getColumn(9);
        if (column.isNull()) throw CaptureError{PitchMidiProblem::Source, "Warp needs a readable marker map"};
        const auto* bytes = static_cast<const std::byte*>(column.getBlob());
        auto map = audio::readWarpMap({bytes, static_cast<std::size_t>(column.getBytes())},
                                      static_cast<std::uint64_t>(frames));
        if (!map || map.markers.back().tick > maxTick)
            throw CaptureError{PitchMidiProblem::Source, "invalid source Warp map"};
        result.warp = std::move(map.markers);
    }
    result.offsetNs = integer(row, 10);
    result.loopBeginNs = integer(row, 11);
    result.loopEndNs = integer(row, 12);
    result.endNs = integer(row, 13);
    return result;
}

struct Clock {
    const Source& source;
    const edit::TimeMap& time;
    std::uint32_t rate;
    std::int64_t positionTicks = 0, lengthTicks = 0, positionNs = 0;
    std::int64_t offset = 0, sourceEnd = 0, span = 0, loopBegin = 0, loopEnd = 0;

    Clock(const Source& captured, const edit::TimeMap& tempo, std::uint32_t sampleRate, std::uint64_t frames)
        : source(captured), time(tempo), rate(sampleRate) {
        const auto& clip = source.clip;
        if (clip.timeBase == 0 && clip.posTicks && clip.lengthTicks) {
            positionTicks = *clip.posTicks;
            lengthTicks = *clip.lengthTicks;
        } else if (clip.timeBase == 1 && clip.posNs && clip.lengthNs && *clip.lengthNs > 0) {
            positionNs = *clip.posNs;
            positionTicks = time.ticks(positionNs);
            lengthTicks = time.ticks(edit::addTime(positionNs, *clip.lengthNs)) - positionTicks;
        } else {
            throw CaptureError{PitchMidiProblem::Source, "the audio clip needs a valid timeline placement"};
        }
        if (positionTicks < 0 || positionTicks > maxTick || lengthTicks <= 0 || lengthTicks > maxTick - positionTicks)
            throw CaptureError{PitchMidiProblem::Source, "the MIDI clip would exceed its tick range"};

        const auto contentOffset = clip.contentOffsetTicks;
        if (warped()) {
            span = lengthTicks;
            offset = contentOffset;
        } else {
            if (clip.timeBase == 0) positionNs = time.nanos(positionTicks);
            span = clip.timeBase == 0 ? time.nanos(positionTicks + lengthTicks) - positionNs : *clip.lengthNs;
            offset = source.offsetNs ? *source.offsetNs : musical(contentOffset);
        }
        if (span <= 0 || offset < 0)
            throw CaptureError{PitchMidiProblem::Source, "invalid source clip extent"};
        sourceEnd = atFrame(frames);
        if (clip.loopEnabled) {
            const auto begin = clip.loopStartTicks;
            const auto length = clip.loopLengthTicks.value_or(0);
            if (begin < 0 || length <= 0)
                throw CaptureError{PitchMidiProblem::Source, "invalid source loop extent"};
            if (!warped() && source.loopBeginNs && source.loopEndNs) {
                loopBegin = *source.loopBeginNs;
                loopEnd = *source.loopEndNs;
            } else {
                loopBegin = musical(begin);
                loopEnd = musical(edit::addTime(begin, length));
            }
            if (loopBegin < 0 || loopBegin >= loopEnd || loopEnd > sourceEnd)
                throw CaptureError{PitchMidiProblem::Source, "the source loop extends beyond the audio window"};
        } else if (!warped() && source.endNs) {
            sourceEnd = std::min(sourceEnd, *source.endNs);
        } else if (clip.endMarkerLocal && clip.endMarkerTicks) {
            auto end = musical(*clip.endMarkerTicks);
            if (!warped() && clip.timeBase == 1 && !source.offsetNs &&
                *clip.endMarkerTicks == edit::addTime(contentOffset, lengthTicks))
                end = edit::addTime(offset, span);
            sourceEnd = std::min(sourceEnd, end);
        }
    }

    [[nodiscard]] bool warped() const noexcept { return !source.warp.empty(); }

    [[nodiscard]] std::int64_t musical(std::int64_t ticks) const {
        return warped() ? ticks : time.duration(positionTicks, ticks);
    }

    [[nodiscard]] std::int64_t atFrame(std::uint64_t frame) const {
        if (!warped()) return edit::nanosFromSeconds(static_cast<double>(frame) / rate);
        const auto next = std::lower_bound(source.warp.begin(), source.warp.end(), frame,
            [](const WarpMarker& value, auto position) { return static_cast<std::uint64_t>(value.sourceFrame) < position; });
        if (next == source.warp.end()) throw CaptureError{PitchMidiProblem::Source, "note outside Warp coverage"};
        if (static_cast<std::uint64_t>(next->sourceFrame) == frame) return next->tick;
        const auto& previous = *std::prev(next);
        const auto local = frame - static_cast<std::uint64_t>(previous.sourceFrame);
        const auto fraction = static_cast<long double>(local) / (next->sourceFrame - previous.sourceFrame);
        return previous.tick + std::llround(fraction * (next->tick - previous.tick));
    }

    [[nodiscard]] std::int64_t relativeTicks(std::int64_t elapsed) const {
        if (elapsed <= 0) return 0;
        if (elapsed >= span) return lengthTicks;
        if (warped()) return elapsed;
        return std::clamp(time.ticks(edit::addTime(positionNs, elapsed)) - positionTicks,
                          std::int64_t{0}, lengthTicks);
    }
};

struct Event {
    std::int64_t begin, end;
    std::uint8_t key, velocity;
    float tuning;
    struct Point { std::int64_t at; float semis; };
    std::vector<Point> curve;
};

std::uint8_t velocityFor(const audio::PitchNote& note, const audio::PitchNoteEdit& edit,
                         const PitchMidiOptions& options) {
    if (options.velocitySource.empty()) return options.velocity;
    const auto frames = options.velocitySource.subspan(static_cast<std::size_t>(note.startFrame),
        static_cast<std::size_t>(note.endFrame - note.startFrame));
    double energy = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i % 4096 == 0) cancellation(options.stop);
        const double sample = frames[i];
        if (!std::isfinite(sample))
            throw CaptureError{PitchMidiProblem::Source, "dynamic velocity source contains non-finite audio"};
        energy += sample * sample;
    }
    const auto rms = std::sqrt(energy / static_cast<double>(frames.size()));
    const auto level = std::min(1.0, rms * std::numbers::sqrt2 * std::pow(10.0, edit.gainDb / 20));
    return static_cast<std::uint8_t>(std::clamp(std::lround(options.velocity * level), 1L, 127L));
}

double originalPitchAt(const audio::PitchNote& note, double frame) {
    const auto after = std::upper_bound(note.curve.begin(), note.curve.end(), frame,
        [](double value, const auto& point) { return value < static_cast<double>(point.frame); });
    if (after == note.curve.begin()) return after->cents;
    const auto before = std::prev(after);
    if (after == note.curve.end()) return before->cents;
    const auto fraction = (frame - static_cast<double>(before->frame)) /
        static_cast<double>(after->frame - before->frame);
    return std::lerp(before->cents, after->cents, fraction);
}

void buildCurve(Event& event, const audio::PitchNote& note, const audio::PitchNoteEdit& edit,
                const audio::PitchNoteEdit* preceding, const media::ClipPitchAnalysis& analysis,
                std::uint64_t begin, std::uint64_t end, const Clock& clock, const PitchMidiOptions& options) {
    const auto originalLength = static_cast<double>(note.endFrame - note.startFrame);
    const auto editedLength = static_cast<double>(edit.endFrame - edit.startFrame);
    const auto originalsAt = [&](std::uint64_t destination) {
        const auto fraction = static_cast<double>(destination - edit.startFrame) / editedLength;
        return static_cast<double>(note.startFrame) + std::min(fraction * originalLength, originalLength - 1);
    };
    const auto add = [&](std::uint64_t frame) {
        if (frame < begin || frame > end) return;
        const auto original = originalsAt(frame);
        const auto delta = audio::pitchCorrectionCents(note, edit, original, preceding);
        const auto semis = (originalPitchAt(note, original) + delta + clock.source.transposeCents
                            - 100 * event.key - event.tuning) / 100;
        if (!std::isfinite(semis) || std::abs(semis) > 48)
            throw CaptureError{PitchMidiProblem::Edits, "extracted pitch curve exceeds native expression range"};
        const auto windowFrame = analysis.reversed ? analysis.sourceEndFrame - frame : frame - analysis.sourceStartFrame;
        event.curve.push_back({clock.atFrame(windowFrame), static_cast<float>(semis)});
    };
    add(begin);
    for (const auto& point : note.curve) {
        cancellation(options.stop);
        const auto fraction = static_cast<double>(point.frame - note.startFrame) / originalLength;
        const auto frame = edit.startFrame + static_cast<std::uint64_t>(std::llround(fraction * editedLength));
        if (frame > begin && frame < end) add(frame);
    }
    // A Warp slope change inside a pitched note changes the rate of its bend.
    if (clock.warped()) {
        const auto windowBegin = analysis.reversed ? analysis.sourceEndFrame - end : begin - analysis.sourceStartFrame;
        const auto windowEnd = analysis.reversed ? analysis.sourceEndFrame - begin : end - analysis.sourceStartFrame;
        auto marker = std::upper_bound(clock.source.warp.begin(), clock.source.warp.end(), windowBegin,
            [](auto frame, const auto& value) { return frame < static_cast<std::uint64_t>(value.sourceFrame); });
        for (; marker != clock.source.warp.end() && static_cast<std::uint64_t>(marker->sourceFrame) < windowEnd; ++marker) {
            cancellation(options.stop);
            const auto frame = static_cast<std::uint64_t>(marker->sourceFrame);
            add(analysis.reversed ? analysis.sourceEndFrame - frame : analysis.sourceStartFrame + frame);
        }
    }
    add(end);
    std::stable_sort(event.curve.begin(), event.curve.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
    event.curve.erase(std::unique(event.curve.begin(), event.curve.end(),
        [](const auto& a, const auto& b) { return a.at == b.at; }), event.curve.end());
}

std::vector<Event> collectNotes(const media::ClipPitchAnalysis& analysis, const audio::PitchAnalysis& notes,
                                const audio::PitchEditState& edits,
                                const PitchMidiOptions& options, const Clock& clock) {
    auto selected = std::vector(options.selectedNotes.begin(), options.selectedNotes.end());
    std::sort(selected.begin(), selected.end());
    if ((!selected.empty() && selected.back() >= notes.notes.size()) ||
        std::adjacent_find(selected.begin(), selected.end()) != selected.end())
        throw CaptureError{PitchMidiProblem::Edits, "invalid or duplicate source pitch note selection"};
    if (options.velocity == 0 || options.velocity > 127)
        throw CaptureError{PitchMidiProblem::Edits, "MIDI velocity must be 1 through 127"};
    if (!options.velocitySource.empty() && options.velocitySource.size() != analysis.source.frames)
        throw CaptureError{PitchMidiProblem::Source, "dynamic velocity requires the verified full source"};

    // Transition addresses the immediately preceding destination note, which
    // can be unselected or originate at a later source index after a move.
    std::vector<audio::PitchNoteEdit> timeline;
    if (options.includePitchCurve &&
        std::any_of(edits.notes.begin(), edits.notes.end(), [](const auto& edit) { return edit.transitionFrames != 0; })) {
        timeline.reserve(notes.notes.size());
        for (std::size_t index = 0; index < notes.notes.size(); ++index)
            timeline.push_back(audio::resolvePitchNoteEdit(notes, edits, index));
        std::sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) { return a.endFrame < b.endFrame; });
    }

    const auto first = analysis.sourceStartFrame;
    const auto last = analysis.sourceEndFrame;
    std::vector<Event> events;
    std::size_t curvePoints = 0;
    for (std::size_t index = 0; index < notes.notes.size(); ++index) {
        cancellation(options.stop);
        if (!selected.empty() && !std::binary_search(selected.begin(), selected.end(), index)) continue;
        const auto& note = notes.notes[index];
        const auto edit = audio::resolvePitchNoteEdit(notes, edits, index);
        if (edit.gainDb == -std::numeric_limits<double>::infinity()) continue;
        const auto begin = std::max(first, edit.startFrame);
        const auto end = std::min(last, edit.endFrame);
        if (end <= begin) continue;

        const auto beginFrame = analysis.reversed ? last - end : begin - first;
        const auto endFrame = analysis.reversed ? last - begin : end - first;
        const double pitch = note.pitchCents + edit.pitchShiftCents + clock.source.transposeCents;
        if (pitch < -50 || pitch > 100 * maxKey + 50)
            throw CaptureError{PitchMidiProblem::Edits, "source pitch is outside the MIDI key range"};
        const auto key = static_cast<std::uint8_t>(std::clamp(std::lround(pitch / 100), 0L, long{maxKey}));
        const auto adjustment = pitch - 100 * key;
        const auto a = clock.atFrame(beginFrame), b = clock.atFrame(endFrame);
        if (a >= b) continue;
        Event event{a, b, key, velocityFor(note, edit, options),
                    options.keepTuning ? static_cast<float>(adjustment) : 0.0f, {}};
        if (options.includePitchCurve) {
            const auto prior = std::lower_bound(timeline.begin(), timeline.end(), edit.startFrame,
                [](const auto& item, auto at) { return item.endFrame < at; });
            const auto* preceding = prior != timeline.end() && prior->endFrame == edit.startFrame ? &*prior : nullptr;
            buildCurve(event, note, edit, preceding, analysis, begin, end, clock, options);
            if (event.curve.size() > maxRecords - curvePoints)
                throw CaptureError{PitchMidiProblem::Edits, "source pitch curve point budget exceeded"};
            curvePoints += event.curve.size();
        }
        events.push_back(std::move(event));
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.begin < b.begin; });
    return events;
}

float curveAt(std::span<const Event::Point> points, std::int64_t at) {
    const auto after = std::upper_bound(points.begin(), points.end(), at,
        [](auto value, const auto& point) { return value < point.at; });
    if (after == points.begin()) return after->semis;
    const auto before = std::prev(after);
    if (after == points.end()) return before->semis;
    const auto progress = static_cast<double>(at - before->at) / static_cast<double>(after->at - before->at);
    return static_cast<float>(std::lerp(static_cast<double>(before->semis), static_cast<double>(after->semis), progress));
}

PitchMidiProjection::PitchCurve projectCurve(const Event& event, const NoteRecord& note,
    std::int64_t begin, std::int64_t end, std::int64_t timeline, const Clock& clock) {
    PitchMidiProjection::PitchCurve curve{note.note_id, {}};
    const auto add = [&curve](std::int64_t time, float value) {
        ExpressionPoint point{};
        point.time_ticks = time;
        point.value = value;
        point.curve = static_cast<std::uint8_t>(Curve::Linear);
        if (!curve.points.empty() && curve.points.back().time_ticks == time) curve.points.back() = point;
        else curve.points.push_back(point);
    };
    add(0, curveAt(event.curve, begin));
    auto point = std::upper_bound(event.curve.begin(), event.curve.end(), begin,
        [](auto position, const auto& value) { return position < value.at; });
    for (; point != event.curve.end() && point->at < end; ++point) {
        const auto ticks = clock.relativeTicks(timeline + (point->at - begin)) - note.start_ticks;
        if (ticks > 0 && ticks < note.dur_ticks) add(ticks, point->semis);
    }
    add(note.dur_ticks, curveAt(event.curve, end));
    return curve;
}

PitchMidiProjection project(const Source& source, const edit::TimeMap& time,
                            const media::ClipPitchAnalysis& analysis, const audio::PitchAnalysis& notes,
                            const audio::PitchEditState& edited,
                            const PitchMidiOptions& options) {
    const audio::PitchEditState empty;
    const auto& edits = options.content == PitchMidiContent::DetectedMelody ? empty : edited;
    const auto validation = audio::validatePitchEdits(notes, edits);
    if (validation != audio::PitchEditError::None)
        throw CaptureError{PitchMidiProblem::Edits, "pitch MIDI requires valid, nonoverlapping edits"};
    const Clock clock(source, time, analysis.analysis.sampleRate,
                      analysis.sourceEndFrame - analysis.sourceStartFrame);
    const auto events = collectNotes(analysis, notes, edits, options, clock);
    PitchMidiProjection result{clock.positionTicks, clock.lengthTicks, {}, {}};
    std::size_t points = 0;
    const auto intersecting = [&events](std::int64_t begin) {
        return std::lower_bound(events.begin(), events.end(), begin,
            [](const Event& event, auto time) { return event.end <= time; });
    };
    const auto append = [&result, &clock, &options, &intersecting, &events, &points]
        (std::int64_t begin, std::int64_t end, std::int64_t offset) {
        if (end <= begin) return;
        for (auto at = intersecting(begin); at != events.end() && at->begin < end; ++at) {
            cancellation(options.stop);
            const auto clippedBegin = std::max(at->begin, begin);
            const auto clippedEnd = std::min(at->end, end);
            const auto atTimeline = offset + (clippedBegin - begin);
            const auto first = clock.relativeTicks(atTimeline);
            const auto last = clock.relativeTicks(offset + (clippedEnd - begin));
            if (last <= first) continue;
            if (result.notes.size() == maxRecords)
                throw CaptureError{PitchMidiProblem::Edits, "extracted note count exceeds the MIDI stream budget"};
            NoteRecord note{};
            note.note_id = result.notes.size() + 1;
            note.start_ticks = first;
            note.dur_ticks = last - first;
            note.key = at->key;
            note.tuning_cents = at->tuning;
            note.vel_on = at->velocity;
            note.vel_off = releaseVelocity;
            note.probability = 10000;
            result.notes.push_back(note);
            if (!at->curve.empty()) {
                auto expression = projectCurve(*at, note, clippedBegin, clippedEnd, atTimeline, clock);
                if (expression.points.size() > maxRecords - points)
                    throw CaptureError{PitchMidiProblem::Edits, "extracted pitch curve point budget exceeded"};
                points += expression.points.size();
                result.pitchCurves.push_back(std::move(expression));
            }
        }
    };
    if (clock.loopEnd <= clock.loopBegin) {
        append(clock.offset, std::min(edit::addTime(clock.offset, clock.span), clock.sourceEnd), 0);
    } else {
        const auto cycle = clock.loopEnd - clock.loopBegin;
        const auto first = clock.offset < clock.loopEnd ? clock.offset
            : clock.loopBegin + (clock.offset - clock.loopBegin) % cycle;
        const auto initial = std::min(clock.span, edit::addTime(clock.loopEnd, -first));
        append(first, edit::addTime(first, initial), 0);
        const auto repeated = intersecting(clock.loopBegin);
        if (repeated != events.end() && repeated->begin < clock.loopEnd) {
            for (auto position = initial; position < clock.span;) {
                cancellation(options.stop);
                const auto count = std::min(cycle, clock.span - position);
                append(clock.loopBegin, clock.loopBegin + count, position);
                position += count;
            }
        }
    }
    if (result.notes.empty()) throw CaptureError{PitchMidiProblem::Empty, "no pitched notes in the selected clip range"};
    cancellation(options.stop);
    return result;
}

std::optional<SQLite::Transaction> snapshot(Store& store) {
    if (sqlite3_get_autocommit(store.db().getHandle()) != 0)
        return std::optional<SQLite::Transaction>(std::in_place, store.db(), SQLite::TransactionBehavior::DEFERRED);
    return {};
}

void addRequest(std::vector<OpRequest>& requests, const char* type, Payload payload, const char* label,
                const char* target, std::int64_t id) {
    OpRequest request;
    request.opType = type;
    request.payload = std::move(payload);
    request.label = label;
    request.targetKind = target;
    request.targetId = id;
    requests.push_back(std::move(request));
}

Payload binary(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> image;
    image.reserve(bytes.size());
    for (const auto byte : bytes) image.push_back(std::to_integer<std::uint8_t>(byte));
    return Payload::binary(std::move(image));
}

Payload midiImage(Store& store, const PitchMidiProjection& midi, std::int64_t track, std::int64_t clip,
                  std::int64_t stream, const std::string& title, RowIds& ids) {
    auto& db = store.db();
    const auto row = edit::defaultRow(db, "clips",
        {{"id", clip}, {"track_id", track}, {"kind", "midi"}, {"name", title},
         {"pos_ticks", midi.positionTicks}, {"length_ticks", midi.lengthTicks}});
    const auto noteBytes = writeStream<NoteRecord>(FourCC::Notes, midi.notes, 1, StreamFlags::SortedByTime);
    auto children = Payload::object();
    for (const auto& child : childTables()) children[child.table] = Payload::array();
    children["event_streams"] = Payload::array({edit::defaultRow(db, "event_streams",
        {{"id", stream}, {"clip_id", clip}, {"stream_kind", "notes"}, {"data", binary(noteBytes)}})});
    for (const auto& curve : midi.pitchCurves) {
        const auto bytes = writeStream<ExpressionPoint>(FourCC::Expression, curve.points, 1, StreamFlags::SortedByTime);
        children["note_expression"].push_back({{"id", ids.next("note_expression")}, {"clip_id", clip},
            {"note_id", curve.noteId}, {"dimension", static_cast<int>(ExpressionDim::Pitch)}, {"data", binary(bytes)}});
    }
    return {{"id", clip}, {"row", row}, {"children", std::move(children)}, {"alias_links", Payload::array()}};
}

std::string titleFor(const Source& source, const char* label) {
    return source.clip.name.empty() ? label : source.clip.name;
}

std::vector<OpRequest> requestsFor(Store& store, const Source& source, const PitchMidiProjection& midi,
                                    std::int64_t track, std::int64_t clip, std::int64_t stream,
                                    const char* label, const PitchMidiOptions& options, RowIds& ids) {
    const auto& db = store.db();
    const auto title = titleFor(source, label);
    if (options.destinationTrack) {
        SQLite::Statement destination(db, "SELECT kind FROM tracks WHERE id=?");
        destination.bind(1, track);
        if (!destination.executeStep())
            throw CaptureError{PitchMidiProblem::Storage, "MIDI extraction destination is missing"};
        const auto kind = destination.getColumn(0).getString();
        if (kind != "midi" && kind != "instrument")
            throw CaptureError{PitchMidiProblem::Storage, "MIDI extraction destination must be a MIDI or instrument track"};
        destination.reset();
        if (options.previousExtractionClip) {
            SQLite::Statement existing(db, "SELECT 1 FROM clips WHERE id=? AND track_id=? AND kind='midi' AND lane_id IS NULL");
            existing.bind(1, *options.previousExtractionClip);
            existing.bind(2, track);
            if (!existing.executeStep())
                throw CaptureError{PitchMidiProblem::Storage, "previous extraction is absent from the selected destination"};
        }

        auto image = midiImage(store, midi, track, clip, stream, title, ids);
        edit_plan::detail::PlacementBatch batch;
        batch.incoming.push_back(image);
        const auto scope = edit_plan::materialScope(std::span(&track, 1), edit_plan::PlacementClock::Ticks,
            {midi.positionTicks, midi.positionTicks + midi.lengthTicks});
        CopyIds copyIds(store.db());
        auto placement = edit_plan::detail::prepareCaptured(store, scope, std::move(batch), {}, copyIds, label);
        if (!placement.ok) throw std::runtime_error(placement.error);
        const auto description = edit_plan::describe(placement);
        for (const auto& effect : description.effects) {
            if (effect.before && effect.before->id != options.previousExtractionClip)
                throw CaptureError{PitchMidiProblem::Storage, "MIDI extraction overlaps other destination material"};
        }
        if (!options.previousExtractionClip) return std::move(placement.requests);

        const auto previous = *options.previousExtractionClip;
        std::vector<OpRequest> requests;
        addRequest(requests, "clip.delete", {{"id", previous}}, label, "clip", previous);
        edit::RowEdits rows(store.db());
        createImage(rows, image);
        addRequest(requests, "clip.duplicate", {{"id", clip}, {"changes", rows.changes()}}, label, "clip", clip);
        return requests;
    }
    if (options.previousExtractionClip)
        throw CaptureError{PitchMidiProblem::Storage, "a previous extraction needs its selected destination track"};

    SQLite::Statement origin(db, "SELECT parent_id,index_in_parent FROM tracks WHERE id=?");
    origin.bind(1, source.clip.trackId);
    if (!origin.executeStep()) throw CaptureError{PitchMidiProblem::Storage, "source track has disappeared"};
    const auto parent = integer(origin, 0);
    const auto index = edit::addTime(origin.getColumn(1).getInt64(), 1);
    std::vector<OpRequest> requests;
    SQLite::Statement siblings(db, "SELECT id,index_in_parent FROM tracks "
        "WHERE parent_id IS ? AND index_in_parent>=? ORDER BY index_in_parent DESC,id DESC");
    if (parent) siblings.bind(1, *parent);
    else siblings.bind(1);
    siblings.bind(2, index);
    while (siblings.executeStep()) {
        const auto id = siblings.getColumn(0).getInt64();
        const auto next = edit::addTime(siblings.getColumn(1).getInt64(), 1);
        addRequest(requests, "track.reorder", {{"id", id}, {"index", next}}, label, "track", id);
    }
    addRequest(requests, "track.create",
        {{"id", track}, {"kind", "midi"}, {"name", title},
         {"parent", parent ? Payload(*parent) : Payload(nullptr)}, {"index", index}},
        label, "track", track);
    if (midi.pitchCurves.empty()) {
        const auto bytes = writeStream<NoteRecord>(FourCC::Notes, midi.notes, 1, StreamFlags::SortedByTime);
        addRequest(requests, "capture.createMidi",
            {{"clip", {{"id", clip}, {"track", track}, {"kind", "midi"}, {"name", title},
                       {"pos", midi.positionTicks}, {"length", midi.lengthTicks}}},
             {"controllers", Payload::array()}, {"notes", {{"id", stream}, {"bytes", binary(bytes)}}}},
            label, "clip", clip);
    } else {
        edit::RowEdits rows(store.db());
        createImage(rows, midiImage(store, midi, track, clip, stream, title, ids));
        addRequest(requests, "clip.duplicate", {{"id", clip}, {"changes", rows.changes()}}, label, "clip", clip);
    }
    return requests;
}

const char* labelFor(PitchMidiContent content) {
    return content == PitchMidiContent::DetectedMelody ? "Convert Melody" : "Extract MIDI";
}

ClipPitchEditsResolve readEdits(Store& store, const media::ClipPitchAnalysis& analysis,
                               const PitchMidiOptions& options) {
    if (options.content == PitchMidiContent::DetectedMelody) return {audio::PitchEditState{}, {}};
    auto stored = readClipPitchEdits(store, analysis.clipId);
    if (!stored.error.empty()) {
        // The authored payload may be broken but its typed source hash still
        // identifies an edit dormant under a different attachment.
        SQLite::Statement identity(store.db(), "SELECT source_hash FROM clip_pitch_edits WHERE clip_id=?");
        identity.bind(1, analysis.clipId);
        if (!identity.executeStep() || identity.getColumn(0).getString() == analysis.source.hash)
            return {{}, std::move(stored.error)};
    }
    if (stored.edits && stored.edits->sourceHash == analysis.source.hash)
        return resolveClipPitchEdits(*stored.edits, analysis);
    return {audio::PitchEditState{}, {}};
}

template<class T>
PitchMidiResult<T> errorResult(const CaptureError& error) {
    return error.detail ? fail<T>(error.problem, error.detail) : fail<T>(error.problem);
}
} // namespace

PitchMidiResult<PitchMidiProjection> projectPitchMidi(
    Store& store, const media::ClipPitchAnalysis& analysis, const audio::PitchEditState& edits,
    const PitchMidiOptions& options) {
    if (audio::onAudioThread) return fail<PitchMidiProjection>(PitchMidiProblem::Realtime);
    try {
        cancellation(options.stop);
        auto read = snapshot(store);
        auto source = readSource(store, analysis);
        auto result = project(source, edit::TimeMap(store.db()), analysis, analysis.analysis, edits, options);
        if (read) read->commit();
        return {PitchMidiProblem::None, std::move(result), {}};
    } catch (const CaptureError& error) {
        return errorResult<PitchMidiProjection>(error);
    } catch (const std::exception& error) {
        return fail<PitchMidiProjection>(PitchMidiProblem::Storage, error.what());
    }
}

PitchMidiPreparation preparePitchMidi(Store& store, const media::ClipPitchAnalysis& analysis,
                                     const PitchMidiOptions& options) {
    if (audio::onAudioThread) return fail<PreparedPitchMidi>(PitchMidiProblem::Realtime);
    try {
        cancellation(options.stop);
        if (store.readOnly()) return fail<PreparedPitchMidi>(PitchMidiProblem::Storage, "project is read-only");
        auto read = snapshot(store);
        const auto source = readSource(store, analysis);
        auto edits = readEdits(store, analysis, options);
        if (!edits.state) return fail<PreparedPitchMidi>(PitchMidiProblem::Edits, std::move(edits.error));
        const auto& notes = edits.adjustedAnalysis ? *edits.adjustedAnalysis : analysis.analysis;
        PreparedPitchMidi result;
        result.owner = &store;
        result.client = store.clientId();
        result.head = OpJournal(store).headSeq();
        result.projection = project(source, edit::TimeMap(store.db()), analysis, notes, *edits.state, options);
        RowIds ids(store.db());
        result.track = options.destinationTrack ? *options.destinationTrack : ids.next("tracks");
        result.midiClip = ids.next("clips");
        result.requests = requestsFor(store, source, result.projection, result.track, result.midiClip,
                                      ids.next("event_streams"), labelFor(options.content), options, ids);
        cancellation(options.stop);
        if (read) read->commit();
        return {PitchMidiProblem::None, std::move(result), {}};
    } catch (const CaptureError& error) {
        return errorResult<PreparedPitchMidi>(error);
    } catch (const std::exception& error) {
        return fail<PreparedPitchMidi>(PitchMidiProblem::Storage, error.what());
    }
}

PitchMidiResult<ClipCopy> copyPitchMidi(Store& store, const media::ClipPitchAnalysis& analysis,
                                     const PitchMidiOptions& options) {
    if (audio::onAudioThread) return fail<ClipCopy>(PitchMidiProblem::Realtime);
    try {
        cancellation(options.stop);
        auto read = snapshot(store);
        const auto source = readSource(store, analysis);
        auto edits = readEdits(store, analysis, options);
        if (!edits.state) return fail<ClipCopy>(PitchMidiProblem::Edits, std::move(edits.error));
        const auto& notes = edits.adjustedAnalysis ? *edits.adjustedAnalysis : analysis.analysis;
        const auto midi = project(source, edit::TimeMap(store.db()), analysis, notes, *edits.state, options);
        RowIds ids(store.db());
        ClipCopy copy;
        copy.ok = true;
        SQLite::Statement path(store.db(), "SELECT file FROM pragma_database_list WHERE name='main'");
        if (!path.executeStep()) throw std::runtime_error("MIDI clipboard needs a project connection");
        copy.sourceFile = path.getColumn(0).getString();
        path.reset();
        copy.image = midiImage(store, midi, source.clip.trackId, source.clip.id, ids.next("event_streams"),
                               titleFor(source, labelFor(options.content)), ids);
        cancellation(options.stop);
        if (read) read->commit();
        return {PitchMidiProblem::None, std::move(copy), {}};
    } catch (const CaptureError& error) {
        return errorResult<ClipCopy>(error);
    } catch (const std::exception& error) {
        return fail<ClipCopy>(PitchMidiProblem::Storage, error.what());
    }
}

CommitResult commitPitchMidi(Store& store, const PreparedPitchMidi& prepared, Actor actor) {
    CommitResult result;
    if (audio::onAudioThread) {
        result.error = "callback";
        return result;
    }
    if (&store != prepared.owner || store.clientId() != prepared.client || prepared.requests.empty()) {
        result.error = "MIDI extraction belongs to another open document";
        result.stale = true;
        return result;
    }
    auto requests = prepared.requests;
    for (auto& request : requests) request.actor = actor;
    CommitOptions options;
    options.expectHead.emplace(prepared.head);
    return OpJournal(store).commit(requests, options);
}

} // namespace adi::clip_edit
