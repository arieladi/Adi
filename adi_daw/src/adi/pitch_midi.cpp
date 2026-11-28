// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_midi.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/clip_image.hpp"
#include "adi/controller_data.hpp"
#include "adi/edit_plan.hpp"
#include "adi/edit_rows.hpp"
#include "adi/edit_time.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/mpe_output.hpp"
#include "adi/pitch_edit_store.hpp"
#include "adi/row_ids.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

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
constexpr auto extractionTable = "clip_pitch_extractions";

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
    double playbackRate = 0;
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
        "d.content_offset_ns,d.loop_begin_ns,d.loop_end_ns,d.source_end_ns,a.warp_mode "
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
    const auto transpose = row.getColumn(7).getDouble();
    result.playbackRate = identity.sampleRate;
    if (row.getColumn(8).getInt() != 0) {
        const auto mode = audio::parseWarpMode(row.getColumn(14).getString());
        if (!mode) throw CaptureError{PitchMidiProblem::Source, "Warp mode is not available"};
        if (*mode != audio::WarpMode::RePitch) {
            if (!(std::abs(transpose) <= 96)) throw CaptureError{PitchMidiProblem::Source, "invalid clip transpose"};
            result.transposeCents = transpose * 100;
        }
        const auto column = row.getColumn(9);
        if (column.isNull()) throw CaptureError{PitchMidiProblem::Source, "Warp needs a readable marker map"};
        const auto* bytes = static_cast<const std::byte*>(column.getBlob());
        auto map = audio::readWarpMap({bytes, static_cast<std::size_t>(column.getBytes())},
                                      static_cast<std::uint64_t>(frames));
        if (!map || map.markers.back().tick > maxTick)
            throw CaptureError{PitchMidiProblem::Source, "invalid source Warp map"};
        if (analysis.reversed) {
            // Playback reflects both Warp clocks before positioning the reversed source.
            const auto end = map.markers.back();
            std::reverse(map.markers.begin(), map.markers.end());
            for (auto& marker : map.markers) marker = {end.sourceFrame - marker.sourceFrame, end.tick - marker.tick};
        }
        result.warp = std::move(map.markers);
    } else {
        result.playbackRate = audio::pitchedSampleRate(identity.sampleRate, transpose);
        if (!(result.playbackRate > 0)) throw CaptureError{PitchMidiProblem::Source, "invalid unwarped clip transpose"};
        result.transposeCents = 1200 * std::log2(result.playbackRate / identity.sampleRate);
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
    std::uint32_t analysisRate;
    std::int64_t positionTicks = 0, lengthTicks = 0, positionNs = 0;
    std::int64_t offset = 0, sourceEnd = 0, span = 0, loopBegin = 0, loopEnd = 0;

    Clock(const Source& captured, const edit::TimeMap& tempo, std::uint32_t sampleRate, std::uint64_t frames)
        : source(captured), time(tempo), analysisRate(sampleRate) {
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
            if (begin >= 0 && length >= 0 && length <= std::numeric_limits<std::int64_t>::max() - begin) {
                try {
                    const auto first = !warped() && source.loopBeginNs ? *source.loopBeginNs : musical(begin);
                    const auto last = !warped() && source.loopEndNs ? *source.loopEndNs : musical(begin + length);
                    if (first >= 0 && first < last && last <= sourceEnd) {
                        loopBegin = first;
                        loopEnd = last;
                    }
                } catch (const std::runtime_error&) {
                    if (warped()) throw;
                }
            }
            // The unwarped player falls back to one pass when its effective
            // rate puts legacy Loop bounds beyond the source window.
            if (warped() && loopEnd == 0)
                throw CaptureError{PitchMidiProblem::Source, "the source loop extends beyond the audio window"};
        }
        if (loopEnd == 0) {
            if (!warped() && source.endNs) {
                sourceEnd = std::min(sourceEnd, *source.endNs);
            } else if (clip.endMarkerLocal && clip.endMarkerTicks) {
                auto end = musical(*clip.endMarkerTicks);
                if (!warped() && clip.timeBase == 1 && !source.offsetNs &&
                    *clip.endMarkerTicks == edit::addTime(contentOffset, lengthTicks))
                    end = edit::addTime(offset, span);
                sourceEnd = std::min(sourceEnd, end);
            }
        }
    }

    [[nodiscard]] bool warped() const noexcept { return !source.warp.empty(); }

    [[nodiscard]] std::int64_t musical(std::int64_t ticks) const {
        return warped() ? ticks : time.duration(positionTicks, ticks);
    }

    [[nodiscard]] std::int64_t atFrame(std::uint64_t frame) const {
        if (!warped()) return edit::nanosFromSeconds(static_cast<double>(frame) / source.playbackRate);
        const auto next = std::lower_bound(source.warp.begin(), source.warp.end(), frame,
            [](const WarpMarker& value, auto position) { return static_cast<std::uint64_t>(value.sourceFrame) < position; });
        if (next == source.warp.end()) throw CaptureError{PitchMidiProblem::Source, "note outside Warp coverage"};
        if (static_cast<std::uint64_t>(next->sourceFrame) == frame) return next->tick;
        const auto& previous = *std::prev(next);
        const auto local = frame - static_cast<std::uint64_t>(previous.sourceFrame);
        const auto fraction = static_cast<long double>(local) /
            static_cast<long double>(next->sourceFrame - previous.sourceFrame);
        return previous.tick + std::llround(fraction * static_cast<long double>(next->tick - previous.tick));
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
    struct Point { std::int64_t at; double value; };
    std::vector<Point> curve, volume;
};

std::span<const float> sourceSamples(const audio::PitchNote& note, const PitchMidiOptions& options) {
    return options.velocitySource.subspan(static_cast<std::size_t>(note.startFrame),
        static_cast<std::size_t>(note.endFrame - note.startFrame));
}

double rms(std::span<const float> frames, std::stop_token stop) {
    double energy = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i % 4096 == 0) cancellation(stop);
        const double sample = frames[i];
        if (!std::isfinite(sample))
            throw CaptureError{PitchMidiProblem::Source, "the volume source contains non-finite audio"};
        energy += sample * sample;
    }
    return std::sqrt(energy / static_cast<double>(frames.size()));
}

std::uint8_t velocityFor(const audio::PitchNote& note, const audio::PitchNoteEdit& edit,
                         const PitchMidiOptions& options) {
    if (options.velocitySource.empty() || options.volumeMode != PitchMidiVolumeMode::Velocity) return options.velocity;
    const auto level = std::min(1.0, rms(sourceSamples(note, options), options.stop) *
        std::numbers::sqrt2 * std::pow(10.0, edit.gainDb / 20));
    return static_cast<std::uint8_t>(std::clamp(std::lround(options.velocity * level), 1L, 127L));
}

double curveAt(std::span<const Event::Point> points, double at) {
    const auto after = std::upper_bound(points.begin(), points.end(), at,
        [](double value, const auto& point) { return value < static_cast<double>(point.at); });
    if (after == points.begin()) return after->value;
    const auto before = std::prev(after);
    if (after == points.end()) return before->value;
    const auto progress = (at - static_cast<double>(before->at)) / static_cast<double>(after->at - before->at);
    return std::lerp(before->value, after->value, progress);
}

std::uint64_t windowFrame(const media::ClipPitchAnalysis& analysis, std::uint64_t frame) {
    return analysis.reversed ? analysis.sourceEndFrame - frame : frame - analysis.sourceStartFrame;
}

template<class Add>
void addWarpEdges(const media::ClipPitchAnalysis& analysis, std::uint64_t begin, std::uint64_t end,
                  const Clock& clock, std::stop_token stop, const Add& add) {
    if (!clock.warped()) return;
    const auto windowBegin = windowFrame(analysis, analysis.reversed ? end : begin);
    const auto windowEnd = windowFrame(analysis, analysis.reversed ? begin : end);
    auto marker = std::upper_bound(clock.source.warp.begin(), clock.source.warp.end(), windowBegin,
        [](auto frame, const auto& value) { return frame < static_cast<std::uint64_t>(value.sourceFrame); });
    for (; marker != clock.source.warp.end() && static_cast<std::uint64_t>(marker->sourceFrame) < windowEnd; ++marker) {
        cancellation(stop);
        const auto frame = static_cast<std::uint64_t>(marker->sourceFrame);
        add(analysis.reversed ? analysis.sourceEndFrame - frame : analysis.sourceStartFrame + frame);
    }
}

void sortCurve(std::vector<Event::Point>& points) {
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
    points.erase(std::unique(points.begin(), points.end(),
        [](const auto& a, const auto& b) { return a.at == b.at; }), points.end());
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
        event.curve.push_back({clock.atFrame(windowFrame(analysis, frame)), semis});
    };
    add(begin);
    for (const auto& point : note.curve) {
        cancellation(options.stop);
        const auto fraction = static_cast<double>(point.frame - note.startFrame) / originalLength;
        const auto frame = edit.startFrame + static_cast<std::uint64_t>(std::llround(fraction * editedLength));
        if (frame > begin && frame < end) add(frame);
    }
    addWarpEdges(analysis, begin, end, clock, options.stop, add);
    add(end);
    sortCurve(event.curve);
}

void buildVolume(Event& event, const audio::PitchNote& note, const audio::PitchNoteEdit& edit,
                 const media::ClipPitchAnalysis& analysis, std::uint64_t begin, std::uint64_t end,
                 const Clock& clock, const PitchMidiOptions& options, std::size_t remaining) {
    constexpr std::size_t meterWindowsPerSecond = 200;
    constexpr double minimumGain = 1e-6; // -120 dB, finite silence
    constexpr double maximumGain = 4.0; // native per-note gain's host range
    const auto frames = sourceSamples(note, options);
    const auto window = std::max(std::size_t{1}, clock.analysisRate / meterWindowsPerSecond);
    const auto count = (frames.size() - 1) / window + 1;
    if (count > remaining)
        throw CaptureError{PitchMidiProblem::Edits, "source volume meter point budget exceeded"};
    const double gain = std::numbers::sqrt2 * std::pow(10.0, edit.gainDb / 20);
    std::vector<Event::Point> levels;
    levels.reserve(count);
    for (std::size_t first = 0; first < frames.size();) {
        const auto size = std::min(window, frames.size() - first);
        const auto level = rms(frames.subspan(first, size), options.stop) * gain;
        levels.push_back({static_cast<std::int64_t>(first + size / 2), level});
        first += size;
    }

    const auto sourceLength = static_cast<double>(frames.size());
    const auto destinationLength = static_cast<double>(edit.endFrame - edit.startFrame);
    const auto add = [&](std::uint64_t frame) {
        if (event.volume.size() == remaining)
            throw CaptureError{PitchMidiProblem::Edits, "mapped volume point budget exceeded"};
        const auto fraction = static_cast<double>(frame - edit.startFrame) / destinationLength;
        const auto amplitude = curveAt(levels, fraction * sourceLength);
        const auto value = options.volumeMode == PitchMidiVolumeMode::NoteGain
            ? 20 * std::log10(std::clamp(amplitude, minimumGain, maximumGain))
            : std::min(amplitude, 1.0);
        event.volume.push_back({clock.atFrame(windowFrame(analysis, frame)), value});
    };
    add(begin);
    for (const auto& point : levels) {
        cancellation(options.stop);
        const auto fraction = static_cast<double>(point.at) / sourceLength;
        const auto frame = edit.startFrame + static_cast<std::uint64_t>(std::llround(fraction * destinationLength));
        if (frame > begin && frame < end) add(frame);
    }
    addWarpEdges(analysis, begin, end, clock, options.stop, add);
    add(end);
    sortCurve(event.volume);
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
    if (options.volumeMode == PitchMidiVolumeMode::ChannelController && options.volumeController >= 120)
        throw CaptureError{PitchMidiProblem::Edits, "volume requires an ordinary controller from 0 through 119"};
    if (options.volumeMode != PitchMidiVolumeMode::Velocity && options.velocitySource.empty())
        throw CaptureError{PitchMidiProblem::Source, "continuous volume requires the verified full source"};
    if (!options.velocitySource.empty() && options.velocitySource.size() != analysis.source.frames)
        throw CaptureError{PitchMidiProblem::Source, "volume PCM must cover the verified full source"};

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
                    options.keepTuning ? static_cast<float>(adjustment) : 0.0f, {}, {}};
        if (options.includePitchCurve) {
            const auto prior = std::lower_bound(timeline.begin(), timeline.end(), edit.startFrame,
                [](const auto& item, auto at) { return item.endFrame < at; });
            const auto* preceding = prior != timeline.end() && prior->endFrame == edit.startFrame ? &*prior : nullptr;
            buildCurve(event, note, edit, preceding, analysis, begin, end, clock, options);
            if (event.curve.size() > maxRecords - curvePoints)
                throw CaptureError{PitchMidiProblem::Edits, "source pitch curve point budget exceeded"};
            curvePoints += event.curve.size();
        }
        if (options.volumeMode != PitchMidiVolumeMode::Velocity) {
            buildVolume(event, note, edit, analysis, begin, end, clock, options, maxRecords - curvePoints);
            curvePoints += event.volume.size();
        }
        events.push_back(std::move(event));
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.begin < b.begin; });
    return events;
}

template<class Add>
void projectCurve(std::span<const Event::Point> points, const NoteRecord& note, std::int64_t begin,
                  std::int64_t end, std::int64_t timeline, const Clock& clock, std::stop_token stop, const Add& add) {
    add(0, curveAt(points, static_cast<double>(begin)));
    auto point = std::upper_bound(points.begin(), points.end(), begin,
        [](auto position, const auto& value) { return position < value.at; });
    for (; point != points.end() && point->at < end; ++point) {
        cancellation(stop);
        const auto ticks = clock.relativeTicks(timeline + (point->at - begin)) - note.start_ticks;
        if (ticks > 0 && ticks < note.dur_ticks) add(ticks, point->value);
    }
    add(note.dur_ticks, curveAt(points, static_cast<double>(end)));
}

PitchMidiProjection::NoteCurve noteExpression(std::span<const Event::Point> points, const NoteRecord& note,
    std::int64_t begin, std::int64_t end, std::int64_t timeline, const Clock& clock, std::stop_token stop,
    std::size_t remaining) {
    PitchMidiProjection::NoteCurve curve{note.note_id, {}};
    const auto add = [&curve, remaining](std::int64_t time, double value) {
        ExpressionPoint point{};
        point.time_ticks = time;
        point.value = static_cast<float>(value);
        point.curve = static_cast<std::uint8_t>(Curve::Linear);
        if (!curve.points.empty() && curve.points.back().time_ticks == time) curve.points.back() = point;
        else {
            if (curve.points.size() == remaining)
                throw CaptureError{PitchMidiProblem::Edits, "extracted note expression point budget exceeded"};
            curve.points.push_back(point);
        }
    };
    projectCurve(points, note, begin, end, timeline, clock, stop, add);
    return curve;
}

void appendController(PitchMidiProjection& result, std::span<const Event::Point> source, const NoteRecord& note,
                      std::int64_t begin, std::int64_t end, std::int64_t timeline, const Clock& clock,
                      std::stop_token stop) {
    auto& points = result.volumeCurve->points;
    const auto add = [&points, &note, &result](std::int64_t time, double value) {
        const bool release = time == note.dur_ticks;
        AutomationPoint point{};
        // ACTL's final point must be inside the half-open clip. Hold across
        // rests so the next note's attack cannot affect this note's level.
        point.time = note.start_ticks + (release ? time - 1 : time);
        point.value = value;
        point.curve = static_cast<std::uint8_t>(release ? Curve::Hold : Curve::Linear);
        if (!points.empty() && points.back().time == point.time) {
            if (release && note.dur_ticks == 1) points.back().curve = point.curve;
            else {
                point.point_id = points.back().point_id;
                points.back() = point;
            }
        } else {
            if (points.size() >= maxRecords - result.notes.size())
                throw CaptureError{PitchMidiProblem::Edits, "extracted CC volume point budget exceeded"};
            point.point_id = points.size() + 1;
            points.push_back(point);
        }
    };
    projectCurve(source, note, begin, end, timeline, clock, stop, add);
}

PitchMidiChannelBend channelBend(const PitchMidiProjection&, PitchMidiBendMode, int, std::stop_token);

PitchMidiProjection::ChannelBendCurve nativeChannelBend(const PitchMidiProjection& midi, PitchMidiBendMode mode,
                                                         std::stop_token stop) {
    const auto bends = channelBend(midi, mode, static_cast<int>(engine::kChannelBendSemitones), stop);
    PitchMidiProjection::ChannelBendCurve result{bends.channel, {}};
    const auto remaining = maxRecords - midi.notes.size() - (midi.volumeCurve ? midi.volumeCurve->points.size() : 0);
    for (const auto& bend : bends.events) {
        cancellation(stop);
        // The engine resets a wheel whose playback ownership ends. Within the
        // clip, the lane gives the effective state after events at this tick.
        if (bend.tick == midi.lengthTicks) continue;
        const auto value = controllers::pitchBendFromWord(bend.word);
        if (!result.points.empty() && result.points.back().time == bend.tick) {
            result.points.back().value = value;
        } else {
            if (result.points.size() == remaining)
                throw CaptureError{PitchMidiProblem::Edits, "MIDI channel bend exceeds the native stream budget"};
            AutomationPoint point{};
            point.point_id = result.points.size() + 1;
            point.time = bend.tick;
            point.value = value;
            point.curve = static_cast<std::uint8_t>(Curve::Hold);
            result.points.push_back(point);
        }
    }
    return result;
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
    auto capture = options;
    if (options.channelBend) {
        capture.keepTuning = true;
        capture.includePitchCurve = *options.channelBend == PitchMidiBendMode::Continuous;
    }
    const auto events = collectNotes(analysis, notes, edits, capture, clock);
    PitchMidiProjection result{clock.positionTicks, clock.lengthTicks, {}, {}, {}, {}};
    if (options.volumeMode == PitchMidiVolumeMode::ChannelController)
        result.volumeCurve.emplace(PitchMidiProjection::ControllerCurve{options.volumeController, {}});
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
            if (result.notes.size() == maxRecords ||
                (result.volumeCurve && result.notes.size() >= maxRecords - result.volumeCurve->points.size()))
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
                auto expression = noteExpression(at->curve, note, clippedBegin, clippedEnd, atTimeline,
                                                 clock, options.stop, maxRecords - points);
                points += expression.points.size();
                result.pitchCurves.push_back(std::move(expression));
            }
            if (options.volumeMode == PitchMidiVolumeMode::NoteGain) {
                auto expression = noteExpression(at->volume, note, clippedBegin, clippedEnd, atTimeline,
                                                 clock, options.stop, maxRecords - points);
                points += expression.points.size();
                result.gainCurves.push_back(std::move(expression));
            } else if (result.volumeCurve) {
                appendController(result, at->volume, note, clippedBegin, clippedEnd, atTimeline, clock, options.stop);
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
    if (options.channelBend) result.channelBendCurve = nativeChannelBend(result, *options.channelBend, options.stop);
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

Payload controllerData(std::span<const AutomationPoint> points) {
    const auto bytes = writeStream<AutomationPoint>(FourCC::Controller, points, 1, StreamFlags::SortedByTime);
    return binary(bytes);
}

Payload notesData(const PitchMidiProjection& midi) {
    auto notes = std::span(midi.notes);
    std::vector<NoteRecord> untuned;
    if (midi.channelBendCurve) {
        untuned = midi.notes;
        for (auto& note : untuned) note.tuning_cents = 0;
        notes = untuned;
    }
    const auto bytes = writeStream<NoteRecord>(FourCC::Notes, notes, 1, StreamFlags::SortedByTime);
    return binary(bytes);
}

Payload midiImage(Store& store, const PitchMidiProjection& midi, std::int64_t track, std::int64_t clip,
                  std::int64_t stream, const std::string& title, RowIds& ids, bool markExtraction = false) {
    auto& db = store.db();
    const auto row = edit::defaultRow(db, "clips",
        {{"id", clip}, {"track_id", track}, {"kind", "midi"}, {"name", title},
         {"pos_ticks", midi.positionTicks}, {"length_ticks", midi.lengthTicks}});
    auto children = Payload::object();
    for (const auto& child : childTables()) children[child.table] = Payload::array();
    if (markExtraction) children[extractionTable].push_back({{"clip_id", clip}});
    children["event_streams"] = Payload::array({edit::defaultRow(db, "event_streams",
        {{"id", stream}, {"clip_id", clip}, {"stream_kind", "notes"}, {"data", notesData(midi)}})});
    const auto expression = [&children, &ids, clip](std::span<const PitchMidiProjection::NoteCurve> curves,
                                                    ExpressionDim dimension) {
        for (const auto& curve : curves) {
            const auto bytes = writeStream<ExpressionPoint>(FourCC::Expression, curve.points, 1, StreamFlags::SortedByTime);
            children["note_expression"].push_back({{"id", ids.next("note_expression")}, {"clip_id", clip},
                {"note_id", curve.noteId}, {"dimension", static_cast<int>(dimension)}, {"data", binary(bytes)}});
        }
    };
    if (!midi.channelBendCurve) expression(midi.pitchCurves, ExpressionDim::Pitch);
    expression(midi.gainCurves, ExpressionDim::Gain);
    if (midi.volumeCurve) {
        children["event_streams"].push_back(edit::defaultRow(db, "event_streams",
            {{"id", ids.next("event_streams")}, {"clip_id", clip}, {"stream_kind", "cc"},
             {"channel", 0}, {"controller", midi.volumeCurve->controller},
             {"data", controllerData(midi.volumeCurve->points)}}));
    }
    if (midi.channelBendCurve) {
        children["event_streams"].push_back(edit::defaultRow(db, "event_streams",
            {{"id", ids.next("event_streams")}, {"clip_id", clip}, {"stream_kind", "pitchbend"},
             {"channel", midi.channelBendCurve->channel}, {"controller", nullptr},
             {"data", controllerData(midi.channelBendCurve->points)}}));
    }
    return {{"id", clip}, {"row", row}, {"children", std::move(children)}, {"alias_links", Payload::array()}};
}

std::string titleFor(const Source& source, const char* label) {
    return source.clip.name.empty() ? label : source.clip.name;
}

std::vector<std::int64_t> previousExtractions(const SQLite::Database& db, std::int64_t track,
                                               const PitchMidiOptions& options, bool marked) {
    std::vector<std::int64_t> result;
    if (options.previousExtractionClip) {
        SQLite::Statement existing(db, "SELECT 1 FROM clips WHERE id=? AND track_id=? AND kind='midi' AND lane_id IS NULL");
        existing.bind(1, *options.previousExtractionClip);
        existing.bind(2, track);
        if (!existing.executeStep())
            throw CaptureError{PitchMidiProblem::Storage, "previous extraction is absent from the selected destination"};
        result.push_back(*options.previousExtractionClip);
    }
    if (marked) {
        SQLite::Statement existing(db, "SELECT c.id FROM clips c JOIN clip_pitch_extractions e ON e.clip_id=c.id "
                                       "WHERE c.track_id=? AND c.kind='midi' AND c.lane_id IS NULL ORDER BY c.id");
        existing.bind(1, track);
        while (existing.executeStep()) {
            cancellation(options.stop);
            const auto id = existing.getColumn(0).getInt64();
            if (options.previousExtractionClip != id) result.push_back(id);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<OpRequest> requestsFor(Store& store, const Source& source, const PitchMidiProjection& midi,
                                    std::int64_t track, std::int64_t clip, std::int64_t stream,
                                    const char* label, const PitchMidiOptions& options, RowIds& ids) {
    const auto& db = store.db();
    const auto title = titleFor(source, label);
    const auto markExtraction = options.content == PitchMidiContent::EditedPitch && db.tableExists(extractionTable);
    if (options.destinationTrack) {
        SQLite::Statement destination(db, "SELECT kind FROM tracks WHERE id=?");
        destination.bind(1, track);
        if (!destination.executeStep())
            throw CaptureError{PitchMidiProblem::Storage, "MIDI extraction destination is missing"};
        const auto kind = destination.getColumn(0).getString();
        if (kind != "midi" && kind != "instrument")
            throw CaptureError{PitchMidiProblem::Storage, "MIDI extraction destination must be a MIDI or instrument track"};
        destination.reset();
        const auto previous = previousExtractions(db, track, options, markExtraction);
        auto image = midiImage(store, midi, track, clip, stream, title, ids, markExtraction);
        edit_plan::detail::PlacementBatch batch;
        batch.incoming.push_back(image);
        const auto scope = edit_plan::materialScope(std::span(&track, 1), edit_plan::PlacementClock::Ticks,
            {midi.positionTicks, midi.positionTicks + midi.lengthTicks});
        CopyIds copyIds(store.db());
        auto placement = edit_plan::detail::prepareCaptured(store, scope, std::move(batch), {}, copyIds, label);
        if (!placement.ok) throw std::runtime_error(placement.error);
        const auto description = edit_plan::describe(placement);
        for (const auto& effect : description.effects) {
            if (effect.before && !std::binary_search(previous.begin(), previous.end(), effect.before->id))
                throw CaptureError{PitchMidiProblem::Storage, "MIDI extraction overlaps other destination material"};
        }
        if (previous.empty()) return std::move(placement.requests);

        std::vector<OpRequest> requests;
        for (const auto id : previous) {
            cancellation(options.stop);
            addRequest(requests, "clip.delete", {{"id", id}}, label, "clip", id);
        }
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
    if (!markExtraction && (midi.pitchCurves.empty() || midi.channelBendCurve) && midi.gainCurves.empty()) {
        auto controllerPackets = Payload::array();
        if (midi.volumeCurve) {
            controllerPackets.push_back({{"id", ids.next("event_streams")}, {"channel", 0},
                {"controller", midi.volumeCurve->controller}, {"bytes", controllerData(midi.volumeCurve->points)}});
        }
        if (midi.channelBendCurve) {
            controllerPackets.push_back({{"id", ids.next("event_streams")}, {"channel", midi.channelBendCurve->channel},
                {"controller", controllers::kPitchBend}, {"bytes", controllerData(midi.channelBendCurve->points)}});
        }
        addRequest(requests, "capture.createMidi",
            {{"clip", {{"id", clip}, {"track", track}, {"kind", "midi"}, {"name", title},
                       {"pos", midi.positionTicks}, {"length", midi.lengthTicks}}},
             {"controllers", std::move(controllerPackets)}, {"notes", {{"id", stream}, {"bytes", notesData(midi)}}}},
            label, "clip", clip);
    } else {
        edit::RowEdits rows(store.db());
        createImage(rows, midiImage(store, midi, track, clip, stream, title, ids, markExtraction));
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
    if (stored.blocksSource(analysis.source.hash)) return {{}, std::move(stored.error)};
    if (stored.edits && stored.edits->sourceHash == analysis.source.hash)
        return resolveClipPitchEdits(*stored.edits, analysis);
    return {audio::PitchEditState{}, {}};
}

template<class T>
PitchMidiResult<T> errorResult(const CaptureError& error) {
    return error.detail ? fail<T>(error.problem, error.detail) : fail<T>(error.problem);
}

PitchMidiChannelBend channelBend(const PitchMidiProjection& projection, PitchMidiBendMode mode,
                                 int range, std::stop_token stop) {
    constexpr auto sampleStep = textproj::kPPQ / 192;
    constexpr std::uint16_t center = 8192;
    using Kind = PitchMidiBendEvent::Kind;
    cancellation(stop);
    if (range < 1 || range > 24)
        throw CaptureError{PitchMidiProblem::Edits, "MIDI bend sensitivity must be 1 through 24 semitones"};
    if (projection.notes.empty())
        throw CaptureError{PitchMidiProblem::Empty, "no projected notes for channel bend"};
    if (projection.lengthTicks <= 0 || projection.lengthTicks > maxTick)
        throw CaptureError{PitchMidiProblem::Edits, "invalid MIDI channel bend clip length"};
    if (mode != PitchMidiBendMode::Static && mode != PitchMidiBendMode::Continuous)
        throw CaptureError{PitchMidiProblem::Edits, "unknown MIDI channel bend mode"};
    if (mode == PitchMidiBendMode::Continuous && projection.pitchCurves.size() != projection.notes.size())
        throw CaptureError{PitchMidiProblem::Edits, "continuous channel bend needs one pitch contour per note"};

    PitchMidiChannelBend result{projection.notes.front().channel, range, {}};
    const auto append = [&result](std::int64_t tick, std::uint64_t note, std::uint16_t word, Kind kind) {
        if (result.events.size() == maxRecords)
            throw CaptureError{PitchMidiProblem::Edits, "MIDI channel bend event budget exceeded"};
        result.events.push_back({tick, note, word, kind});
    };
    const auto bendWord = [range](double semitones) {
        if (!std::isfinite(semitones) || std::abs(semitones) > range)
            throw CaptureError{PitchMidiProblem::Edits, "pitch exceeds the requested MIDI bend sensitivity"};
        return engine::semitonesToBendWord(semitones, range);
    };
    append(0, 0, center, Kind::InitialCenter);
    std::int64_t lastEnd = 0;
    std::uint64_t lastId = 0;
    std::size_t samples = 0;
    const auto countSample = [&samples, stop] {
        cancellation(stop);
        if (samples == maxRecords)
            throw CaptureError{PitchMidiProblem::Edits, "MIDI channel bend contour sample budget exceeded"};
        ++samples;
    };
    for (std::size_t i = 0; i < projection.notes.size(); ++i) {
        cancellation(stop);
        const auto& note = projection.notes[i];
        if (note.channel != result.channel || note.channel > 15 || note.note_id <= lastId ||
            note.start_ticks < 0 || note.start_ticks >= projection.lengthTicks || note.dur_ticks <= 0 ||
            note.dur_ticks > projection.lengthTicks - note.start_ticks)
            throw CaptureError{PitchMidiProblem::Edits, "invalid projected note for channel bend"};
        if (note.start_ticks < lastEnd)
            throw CaptureError{PitchMidiProblem::Edits, "overlapping projected notes cannot share a channel bend"};
        lastEnd = note.start_ticks + note.dur_ticks;
        lastId = note.note_id;
        const auto tuning = static_cast<double>(note.tuning_cents) / 100;
        if (mode == PitchMidiBendMode::Static) {
            append(note.start_ticks, lastId, bendWord(tuning), Kind::Attack);
        } else {
            const auto& curve = projection.pitchCurves[i];
            if (curve.noteId != lastId || curve.points.size() < 2 || curve.points.front().time_ticks != 0 ||
                curve.points.back().time_ticks != note.dur_ticks ||
                curve.points.front().curve != static_cast<std::uint8_t>(Curve::Linear))
                throw CaptureError{PitchMidiProblem::Edits, "invalid projected channel bend contour endpoints"};
            auto from = tuning + curve.points.front().value;
            auto fromTick = std::int64_t{0};
            auto sent = bendWord(from);
            append(note.start_ticks, lastId, sent, Kind::Attack);
            for (const auto& point : std::span(curve.points).subspan(1)) {
                countSample();
                const auto to = tuning + point.value;
                const auto word = bendWord(to);
                if (point.time_ticks <= fromTick || point.time_ticks > note.dur_ticks ||
                    point.curve != static_cast<std::uint8_t>(Curve::Linear))
                    throw CaptureError{PitchMidiProblem::Edits, "invalid projected channel bend contour point"};
                if (sent != word) {
                    const auto sample = [&](std::int64_t tick) {
                        countSample();
                        const auto fraction = static_cast<double>(tick - fromTick) /
                                              static_cast<double>(point.time_ticks - fromTick);
                        const auto next = engine::semitonesToBendWord(std::lerp(from, to, fraction), range);
                        if (next == sent) return;
                        append(note.start_ticks + tick, lastId, next, Kind::Movement);
                        sent = next;
                    };
                    const auto lastTick = std::min(point.time_ticks, note.dur_ticks - 1);
                    for (auto tick = fromTick + sampleStep; tick < lastTick; tick += sampleStep) sample(tick);
                    if (lastTick > fromTick) sample(lastTick);
                }
                fromTick = point.time_ticks;
                from = to;
            }
        }
        append(lastEnd, lastId, center, Kind::ReleaseCenter);
    }
    cancellation(stop);
    return result;
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

PitchMidiResult<PitchMidiChannelBend> projectPitchMidiChannelBend(
    const PitchMidiProjection& projection, PitchMidiBendMode mode, int rangeSemitones, std::stop_token stop) {
    if (audio::onAudioThread) return fail<PitchMidiChannelBend>(PitchMidiProblem::Realtime);
    try {
        return {PitchMidiProblem::None, channelBend(projection, mode, rangeSemitones, stop), {}};
    } catch (const CaptureError& error) {
        return errorResult<PitchMidiChannelBend>(error);
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
