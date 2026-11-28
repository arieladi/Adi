// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_midi.hpp"
#include "adi/engine/midi_expression_clock.hpp"

#include "adi/engine/clip_details.hpp"
#include "adi/engine/curves.hpp"
#include "adi/engine/midi_controllers.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace adi::engine {
namespace {
constexpr std::size_t maximumNotes = 1000000; // ADR-0155 occurrence budget
constexpr std::size_t maximumPoints = 1000000;
constexpr auto tickLimit = std::int64_t{1} << 50;
constexpr auto lastTick = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t identityStart = 14695981039346656037ULL;

void mix(std::uint64_t& key, std::uint64_t value) noexcept { key = (key ^ value) * 1099511628211ULL; }

template<class Point>
void mixPoint(std::uint64_t& key, const Point& point) noexcept {
    mix(key,static_cast<std::uint64_t>(point.tick));
    mix(key,std::bit_cast<std::uint64_t>(point.value));
    mix(key,std::bit_cast<std::uint32_t>(point.tension));
    mix(key,point.curve);
}

bool validPoint(std::int64_t tick, double value, float tension, std::uint8_t curve) noexcept {
    return tick >= -tickLimit && tick <= tickLimit && std::isfinite(value) &&
        std::isfinite(tension) && tension >= -1 && tension <= 1 && curve <= 5;
}

struct Projection {
    const LaunchBlock& block;
    std::int64_t phase;
    [[nodiscard]] std::int64_t sample(std::int64_t tick) const noexcept {
        if (phase > 0 && tick > lastTick - phase)
            return lastTick;
        if (tick < -phase)
            return 0;
        return block.sampleAtTick(block.clock, tick + phase);
    }
    [[nodiscard]] double sourceTick(std::int64_t sample, const LaunchSection& section) const noexcept {
        if (block.preciseTickAtSample)
            return block.preciseTickAtSample(block.clock,sample) - static_cast<double>(phase);
        if (block.tickAtSample)
            return static_cast<double>(block.tickAtSample(block.clock,sample) - phase);
        const auto offset = sample - block.sample - section.offset;
        const auto portion = static_cast<double>(offset) / section.frames;
        return static_cast<double>(section.sourceTick) +
            portion * static_cast<double>(section.endSourceTick-section.sourceTick);
    }
};

template<class Points>
auto nextKnot(const Points& points, const Projection& projection, std::int64_t cursor,
              std::int64_t origin) noexcept {
    return std::lower_bound(points.begin(),points.end(),cursor,[&](const auto& point, auto sample) {
        return projection.sample(origin+point.tick) < sample;
    });
}

template<class Points>
double valueAt(const Points& points, const Projection& projection, std::int64_t sample,
               std::int64_t origin, double tick, std::optional<std::int64_t> exactKnotFrom) noexcept {
    const auto next = std::upper_bound(points.begin(),points.end(),sample,[&](auto at, const auto& point) {
        return at < projection.sample(origin+point.tick);
    });
    if (next == points.begin()) return next->value;
    const auto& previous = *(next-1);
    if (next == points.end() || (exactKnotFrom && origin+previous.tick >= *exactKnotFrom &&
        projection.sample(origin+previous.tick) == sample))
        return previous.value;
    const auto duration = static_cast<double>(next->tick-previous.tick);
    return curveValue(previous.value,next->value,
        std::clamp((tick-static_cast<double>(previous.tick))/duration,0.0,1.0),previous.curve,previous.tension);
}

std::int64_t nextGrid(std::int64_t cursor, std::int64_t step) noexcept {
    const auto remainder = cursor % step;
    const auto delta = remainder ? step - remainder : 0;
    return delta > lastTick - cursor ? lastTick : cursor + delta;
}
} // namespace

std::optional<LaunchMidiClip> LaunchMidiClip::prepare(
    std::int64_t clipId, std::span<const rows::Note> input,
    std::span<const NoteExpressionCurve* const> expression,
    std::span<const controllers::Lane* const> controllers, std::vector<std::string>* problems) {
    if (clipId <= 0 || input.size() > maximumNotes)
        return std::nullopt;

    LaunchMidiClip clip;
    clip.id_ = clipId;
    clip.notes_.reserve(input.size());
    for (const auto& row : input) {
        if (row.clipId != clipId || row.durTicks <= 0 || row.durTicks > tickLimit ||
            row.startTicks < -tickLimit || row.startTicks > tickLimit ||
            row.key < 0 || row.key > 127 || row.channel < 0 || row.channel > 15 ||
            row.velOn < 1 || row.velOn > 127 || row.velOff < 0 || row.velOff > 127 ||
            !std::isfinite(row.tuningCents))
            return std::nullopt;
        if (row.flags & NoteFlags::Muted)
            continue;

        Event on;
        on.type = EventType::NoteOn;
        on.channel = static_cast<std::uint8_t>(row.channel);
        on.dim = static_cast<std::uint16_t>(row.key);
        on.value = static_cast<double>(row.velOn) / 127.0;
        auto off = on;
        off.type = EventType::NoteOff;
        off.value = static_cast<double>(row.velOff) / 127.0;
        const auto end = row.startTicks + row.durTicks;
        clip.notes_.push_back({row.startTicks, end, end, row.noteId, on, off, row.tuningCents});
    }

    std::stable_sort(clip.notes_.begin(), clip.notes_.end(), [](const Note& a, const Note& b) {
        return std::tie(a.start, a.end, a.id) < std::tie(b.start, b.end, b.id);
    });
    auto longest = std::numeric_limits<std::int64_t>::min();
    clip.boundaries_.reserve(clip.notes_.size() * 2);
    clip.byId_.reserve(clip.notes_.size());
    for (std::size_t i = 0; i < clip.notes_.size(); ++i) {
        auto& note = clip.notes_[i];
        longest = note.longestEnd = std::max(longest, note.end);
        clip.boundaries_.push_back({note.start, i, true});
        clip.boundaries_.push_back({note.end, i, false});
        if (note.id)
            clip.byId_.emplace_back(note.id, i);
    }
    std::sort(clip.boundaries_.begin(), clip.boundaries_.end(), [](const Boundary& a, const Boundary& b) {
        return std::tie(a.tick, a.start, a.note) < std::tie(b.tick, b.start, b.note);
    });
    std::sort(clip.byId_.begin(), clip.byId_.end());
    for (auto first = clip.byId_.begin(); first != clip.byId_.end();) {
        const auto last = std::upper_bound(first, clip.byId_.end(), first->first,
            [](std::uint64_t id, const auto& entry) { return id < entry.first; });
        if (last - first > 1) {
            for (auto item = first; item != last; ++item)
                clip.notes_[item->second].id = 0;
        }
        first = last;
    }
    const auto report = [&](std::string message) { if (problems) problems->push_back(std::move(message)); };
    for (const auto* curve : expression) {
        if (!curve || curve->clipId != clipId || curve->points.empty() || curve->dimension < 0 ||
            static_cast<std::uint64_t>(curve->dimension) >= kStandardExpressionDimensionCount) continue;
        const auto* index = clip.stableIndex(curve->noteId);
        if (!index) continue;
        auto& note = clip.notes_[*index];
        auto& range = note.expression[static_cast<std::size_t>(curve->dimension)];
        if (range.count || curve->points.size() > maximumPoints - clip.expression_.size() ||
            std::any_of(curve->points.begin(),curve->points.end(),[](const ExpressionPoint& point) {
                return !validPoint(point.time_ticks,point.value,point.tension,point.curve);
            })) {
            report("invalid or oversized note expression curve");
            continue;
        }
        range = {clip.expression_.size(),curve->points.size()};
        for (const auto& point : curve->points)
            clip.expression_.push_back({point.time_ticks,point.value,point.tension,point.curve});
        const auto begin = clip.expression_.begin()+static_cast<std::ptrdiff_t>(range.first);
        std::stable_sort(begin,clip.expression_.end(),[](const auto& a,const auto& b) {return a.tick<b.tick;});
    }
    const std::span expressionPoints{clip.expression_.data(),clip.expression_.size()};
    for (auto& note : clip.notes_) {
        auto key = identityStart;
        for (std::size_t d = 0; d < note.expression.size(); ++d) {
            const auto range = note.expression[d];
            if (!range.count) continue;
            mix(key,d);
            mix(key,range.count);
            for (const auto& point : expressionPoints.subspan(range.first,range.count)) mixPoint(key,point);
        }
        note.expressionIdentity = key == identityStart ? 0 : key;
    }
    std::set<std::uint16_t> addresses;
    for (const auto* row : controllers) {
        if (!row || row->clipId != clipId) continue;
        if (!row->error.empty() || !row->channel || !row->controller ||
            *row->channel < 0 || *row->channel > 15 || !controllers::validController(*row->controller)) {
            report(row->error.empty() ? "invalid controller channel/address" : row->error);
            continue;
        }
        if (row->points.empty()) continue;
        const auto address = controllerAddress(static_cast<int>(*row->channel), static_cast<int>(*row->controller));
        std::set<std::uint64_t> pointIds;
        if (row->points.size() > maximumPoints - clip.controlKnots_.size() || addresses.contains(address) ||
            std::any_of(row->points.begin(),row->points.end(),[&](const auto& point) {
                return !point.id || !pointIds.insert(point.id).second || point.value < 0 || point.value > 1 ||
                    !validPoint(point.timeTicks,point.value,point.tension,point.curve);
            })) {
            report("invalid, duplicate or oversized controller lane");
            continue;
        }
        addresses.insert(address);
        auto& lane = clip.controls_.emplace_back(ControlLane{address,{}});
        lane.points.reserve(row->points.size());
        for (const auto& point : row->points) {
            lane.points.push_back({point.timeTicks,point.value,point.tension,point.curve});
            clip.controlKnots_.push_back(point.timeTicks);
        }
        std::stable_sort(lane.points.begin(),lane.points.end(),[](const auto& a,const auto& b) {return a.tick<b.tick;});
    }
    if (!clip.controls_.empty()) {
        std::sort(clip.controls_.begin(),clip.controls_.end(),[](const auto& a,const auto& b) {return a.address<b.address;});
        std::sort(clip.controlKnots_.begin(),clip.controlKnots_.end());
        clip.controlKnots_.erase(std::unique(clip.controlKnots_.begin(),clip.controlKnots_.end()),clip.controlKnots_.end());
        clip.controllerIdentity_ = identityStart;
        for (const auto& lane : clip.controls_) {
            mix(clip.controllerIdentity_,lane.address);
            mix(clip.controllerIdentity_,lane.points.size());
            for (const auto& point : lane.points) mixPoint(clip.controllerIdentity_,point);
        }
    }
    return clip;
}

bool LaunchMidiClip::Note::sameAuthored(const Note& other) const noexcept {
    return id == other.id && start == other.start && end == other.end &&
        on.channel == other.on.channel && on.dim == other.on.dim &&
        on.value == other.on.value && off.value == other.off.value && tuning == other.tuning &&
        expressionIdentity == other.expressionIdentity;
}

const std::size_t* LaunchMidiClip::stableIndex(std::uint64_t id) const noexcept {
    if (id == 0 || byId_.empty())
        return nullptr;
    const auto* end = byId_.data() + byId_.size();
    const auto* found = std::lower_bound(byId_.data(), end, id,
        [](const auto& entry, auto value) { return entry.first < value; });
    if (found == end || found->first != id || (found + 1 != end && found[1].first == id))
        return nullptr;
    return &found->second;
}

LaunchMidiTrack::Voice* LaunchMidiTrack::voice(std::uint64_t id) noexcept {
    return const_cast<Voice*>(std::as_const(*this).voice(id));
}

const LaunchMidiTrack::Voice* LaunchMidiTrack::voice(std::uint64_t id) const noexcept {
    if (id != 0) {
        for (const auto& item : active_)
            if (item.runtimeId == id)
                return &item;
    }
    return nullptr;
}

bool LaunchMidiTrack::stop(Voice& item, EventList& out, std::int32_t offset) noexcept {
    if (item.pending)
        return true;
    item.releasing = true;
    auto event = item.note.off;
    event.frame = offset;
    event.noteId = item.runtimeId;
    item.pending = out.push(event);
    return item.pending;
}

bool LaunchMidiTrack::release(EventList& out, std::int32_t offset, LaunchMidiControllers controls) noexcept {
    bool complete = true;
    for (auto& item : active_)
        if (item.runtimeId && !stop(item, out, offset))
            complete = false;
    if (controllerIdentity_ && controls.reset && !controls.reset(controls.context,out,offset))
        complete = false;
    if (complete) {
        clip_ = 0;
        controllerIdentity_ = 0;
    }
    return complete;
}

void LaunchMidiTrack::emitOn(const Voice& item, float velocity, EventList& out, std::int32_t offset) noexcept {
    auto event = item.note.on;
    event.noteId = item.runtimeId;
    event.frame = offset;
    event.value *= velocity;
    (void)out.push(event);
    if (item.note.tuning != 0.0 && !item.note.expression[0].count) {
        event.type = EventType::NoteExpression;
        event.dim = static_cast<std::uint16_t>(ExpressionDim::Pitch);
        event.value = item.note.tuning / 100.0;
        (void)out.push(event);
    }
}

void LaunchMidiTrack::start(const LaunchMidiClip& clip, std::size_t index, float velocity,
                            EventList& out, std::int32_t offset, LaunchMidiVoiceIds ids) noexcept {
    if (velocity <= 0.0f)
        return;
    Voice* empty = nullptr;
    std::int32_t owed = 0;
    for (auto& item : active_) {
        if (item.runtimeId && !item.releasing && item.index == index)
            return;
        if (item.runtimeId && !item.pending)
            ++owed;
        if (!item.runtimeId && !empty)
            empty = &item;
    }
    const auto& note = clip.notes_[index];
    // In addition to the onset and optional tuning expression, leave room
    // for its own release and each release still owed in this block.
    const auto required = owed + (note.tuning == 0.0 ? 2 : 3);
    if (!empty || out.capacity() - out.size() < required) {
        ++dropped_;
        return;
    }
    const auto id = ids.take(ids.context);
    if (!id || voice(id)) {
        ++dropped_;
        return;
    }
    *empty = {note, id, index, clip.id(), revision_};
    empty->expressionFresh = note.expressionIdentity != 0;
    emitOn(*empty, velocity, out, offset);
}

void LaunchMidiTrack::adoptProgram(std::uint64_t program) noexcept {
    if (adoptedProgram_ == program)
        return;
    adoptedProgram_ = program;
    for (auto& item : active_) {
        if (!item.runtimeId)
            continue;
        if (item.pending && item.retainedOff != program)
            item.pending = false;
        if (!item.sounding && !item.releasing && item.retainedOn != program)
            item.retry = true;
    }
}

std::int64_t LaunchMidiTrack::eventClip(const Event& event) const noexcept {
    const auto* item = voice(event.noteId);
    return item ? item->clip : 0;
}

bool LaunchMidiTrack::canRetainControllers(const LaunchMidiClip* clip, LaunchMidiRevision revision) const noexcept {
    return clip && clip->id() == clip_ && controllerIdentity_ && clip->controllerIdentity_ == controllerIdentity_ &&
        revision.route == revision_.route;
}

bool LaunchMidiTrack::canRetainPendingEvent(const Event& event, const LaunchMidiClip* clip,
                                           LaunchMidiRevision revision) const noexcept {
    const auto* item = voice(event.noteId);
    if (!item || item->revision.route != revision.route)
        return false;
    if (event.type == EventType::NoteOff)
        return item->pending;
    if (event.type != EventType::NoteOn && event.type != EventType::NoteExpression)
        return false;
    if (item->revision.program == revision.program)
        return true;
    if (!clip || clip->id() != item->clip)
        return false;
    const auto* index = clip->stableIndex(item->note.id);
    return index && clip->notes_[*index].sameAuthored(item->note);
}

void LaunchMidiTrack::pendingEventRetained(const Event& event, std::uint64_t program) noexcept {
    auto* item = voice(event.noteId);
    if (!item)
        return;
    if (event.type == EventType::NoteOn)
        item->retainedOn = program;
    if (event.type == EventType::NoteOff)
        item->retainedOff = program;
}

bool LaunchMidiTrack::retainedEventIsValid(const Event& event, std::uint64_t program) const noexcept {
    const auto* item = voice(event.noteId);
    return item && (event.type == EventType::NoteOff
        ? item->pending && item->retainedOff == program
        : item->revision.program == program);
}

bool LaunchMidiTrack::holdingNotes() const noexcept {
    return std::any_of(active_.begin(), active_.end(),
        [](const Voice& item) { return item.runtimeId && !item.releasing; });
}

void LaunchMidiTrack::noteOnDelivered(const Event& event) noexcept {
    if (auto* item = voice(event.noteId)) {
        item->sounding = true;
        item->retry = false;
    }
}

void LaunchMidiTrack::noteOnRejected(const Event& event) noexcept {
    if (auto* item = voice(event.noteId); item && !item->sounding && !item->releasing)
        item->retry = true;
}

void LaunchMidiTrack::noteOffDelivered(const Event& event) noexcept {
    if (auto* item = voice(event.noteId); item && item->pending)
        *item = {};
}

void LaunchMidiTrack::noteOffRejected(const Event& event) noexcept {
    if (auto* item = voice(event.noteId); item && item->releasing)
        item->pending = false;
}

bool LaunchMidiTrack::render(const LaunchBlock& block, const LaunchSection& section, std::int32_t baseOffset,
                             const LaunchMidiClip* clip, LaunchMidiRevision revision,
                             LaunchMidiVoiceIds ids, EventList& out, LaunchMidiControllers controls) noexcept {
    constexpr auto lastOffset = std::numeric_limits<std::int32_t>::max();
    if (section.track != track_ || block.sample < 0 || block.frames <= 0 ||
        block.sample > lastTick - block.frames || baseOffset < 0 || baseOffset > lastOffset - block.frames ||
        section.offset < 0 || section.frames <= 0 || section.offset > block.frames - section.frames)
        return false;
    const auto firstOffset = baseOffset + section.offset;
    if (section.arrangement || section.clip == 0 || !clip)
        return release(out, firstOffset, controls);
    if (!block.sampleAtTick || !ids.take || clip->id() != section.clip ||
        section.timelineTick < 0 || section.sourceTick < 0 || section.endSourceTick < section.sourceTick ||
        section.framesSinceStart < 0 || section.framesSinceStart > lastTick - section.frames ||
        !std::isfinite(section.velocity))
        return false;

    adoptProgram(revision.program);
    const Projection projection{block, section.timelineTick - section.sourceTick};
    const auto first = block.sample + section.offset;
    const auto end = first + section.frames;
    const auto velocity = std::clamp(section.velocity, 0.0f, 1.0f);
    const bool consecutive = section.run == run_ && section.sourceTick == nextTick_ &&
        section.framesSinceStart == nextFrame_ && revision.route == revision_.route;
    // An authorized Capture and Insert Scene changes the literal clip ID of
    // a continuing run. A launch, loop seam or route replacement still owns
    // its original note-off and starts a fresh note when needed.
    const bool retarget = consecutive && clip_ != 0 && section.clip != clip_ && section.run != 0;
    const bool restart = !consecutive || (section.clip != clip_ && !retarget);
    const bool adopt = !restart && (retarget || revision.program != revision_.program);
    for (auto& item : active_) {
        if (item.runtimeId == 0)
            continue;
        const bool retainedPair = item.retainedOn == revision.program && item.retainedOff == revision.program;
        if (adopt && item.clip == clip_ && (!item.releasing || retainedPair)) {
            auto* index = clip->stableIndex(item.note.id);
            // A literal clone preserves duplicate and legacy note order as
            // well as its stable IDs. Retain at a matching index only for this
            // continuing run; arbitrary graph edits still need unique IDs.
            if (!index && retarget && item.note.id == 0 && item.index < clip->notes_.size())
                index = &item.index;
            if (!index || !clip->notes_[*index].sameAuthored(item.note))
                item.releasing = true;
            else {
                item.index = *index;
                item.clip = section.clip;
                item.note = clip->notes_[*index];
                item.revision.program = revision.program;
            }
        }
        if ((restart || item.releasing || projection.sample(item.note.end) <= first) &&
            !stop(item, out, firstOffset)) {
            clip_ = 0;
            return false;
        }
    }
    const auto chaseControls = (restart || controllerIdentity_ != clip->controllerIdentity_);
    if (chaseControls && (controllerIdentity_ || clip->controllerIdentity_) && controls.reset &&
        !controls.reset(controls.context,out,firstOffset)) return false;
    auto controlsBlocked = false;
    const auto emitControllers = [&](std::int64_t sample, std::int32_t offset) {
        const auto tick = std::max(static_cast<double>(section.sourceTick),projection.sourceTick(sample,section));
        for (const auto& lane : std::span(clip->controls_.data(),clip->controls_.size())) {
            const std::span points(lane.points.data(),lane.points.size());
            const auto value = valueAt(points,projection,sample,0,tick,std::nullopt);
            if (!controls.send(controls.context,out,offset,lane.address,value)) {
                controlsBlocked = true;
                return;
            }
        }
    };
    const bool controlsAtFirst = chaseControls && controls.send && !clip->controls_.empty();
    if (controlsAtFirst) emitControllers(first,firstOffset);
    clip_ = section.clip;
    run_ = section.run;
    nextTick_ = section.endSourceTick;
    nextFrame_ = section.framesSinceStart + section.frames;
    revision_ = revision;
    controllerIdentity_ = clip->controllerIdentity_;

    for (auto& item : active_) {
        if (!item.runtimeId || !item.retry || item.releasing || projection.sample(item.note.end) <= first)
            continue;
        const auto owed = std::count_if(active_.begin(), active_.end(),
            [](const Voice& voice) { return voice.runtimeId && !voice.pending; });
        const auto required = owed + (item.note.tuning == 0.0 ? 1 : 2);
        if (out.capacity() - out.size() >= required) {
            emitOn(item, velocity, out, firstOffset);
            item.retry = false;
            item.expressionFresh = item.note.expressionIdentity != 0;
            item.expressionSent.fill(false);
        }
    }

    if ((restart || adopt) && !clip->notes_.empty()) {
        const std::span notes{clip->notes_.data(), clip->notes_.size()};
        const auto firstPossible = std::ranges::upper_bound(notes, section.sourceTick, {},
                                                           &LaunchMidiClip::Note::longestEnd);
        for (auto item = firstPossible; item != notes.end() && item->start < section.sourceTick; ++item)
            if (projection.sample(item->end) > first)
                start(*clip, static_cast<std::size_t>(item - notes.begin()), velocity, out, firstOffset, ids);
    }

    const std::span boundaries{clip->boundaries_.data(), clip->boundaries_.size()};
    auto item = std::lower_bound(boundaries.begin(), boundaries.end(), first,
        [&](const auto& boundary, auto sample) { return projection.sample(boundary.tick) < sample; });
    const std::span knots{clip->controlKnots_.data(),clip->controlKnots_.size()};
    const std::span expression{clip->expression_.data(),clip->expression_.size()};
    const auto rate = std::isfinite(block.sampleRate) ? std::clamp(block.sampleRate,500.0,768000.0) : 48000.0;
    const auto expressionStep = static_cast<std::int64_t>(rate/500.0);
    auto expressionBlocked = false;
    for (auto cursor = first; cursor < end;) {
        auto sample = item == boundaries.end() ? end : projection.sample(item->tick);
        auto controllerSample = end;
        if (controls.send && !knots.empty() && !controlsBlocked) {
            const auto knot = std::lower_bound(knots.begin(),knots.end(),cursor,[&](auto tick,auto frame) {
                return projection.sample(tick)<frame;
            });
            controllerSample = nextGrid(cursor,64);
            if (knot != knots.end()) controllerSample = std::min(controllerSample,projection.sample(*knot));
            sample = std::min(sample,controllerSample);
        }
        if (!expression.empty() && !expressionBlocked) {
            for (const auto& voice : active_) {
                if (!voice.runtimeId || voice.releasing || !voice.note.expressionIdentity) continue;
                sample = std::min(sample,voice.expressionFresh ? cursor : nextGrid(cursor,expressionStep));
                for (const auto range : voice.note.expression) {
                    if (!range.count) continue;
                    const auto points = expression.subspan(range.first,range.count);
                    const auto knot = nextKnot(points,projection,cursor,voice.note.start);
                    if (knot != points.end()) sample = std::min(sample,projection.sample(voice.note.start+knot->tick));
                }
            }
        }
        if (sample >= end)
            break;
        const auto offset = firstOffset + static_cast<std::int32_t>(sample - first);
        auto next = item;
        while (next != boundaries.end() && projection.sample(next->tick) == sample)
            ++next;

        // Different ticks can round to the same sample. End old voices before
        // admitting new ones on that sample, independent of tick ordering.
        for (auto edge = item; edge != next; ++edge) {
            if (edge->start)
                continue;
            for (auto& voice : active_) {
                if (voice.runtimeId && !voice.releasing && voice.index == edge->note &&
                    !stop(voice, out, offset)) {
                    clip_ = 0;
                    return false;
                }
            }
        }
        if (controls.send && !knots.empty() && !controlsBlocked &&
            (sample == controllerSample || next != item) && (sample != first || !controlsAtFirst))
            emitControllers(sample,offset);
        for (auto edge = item; edge != next; ++edge) {
            if (edge->start && (!restart || edge->tick >= section.sourceTick) &&
                edge->tick < section.endSourceTick && projection.sample(clip->notes_[edge->note].end) > sample)
                start(*clip, edge->note, velocity, out, offset, ids);
        }
        if (!expression.empty() && !expressionBlocked) {
            const auto owed = std::count_if(active_.begin(),active_.end(),
                [](const auto& voice) { return voice.runtimeId && !voice.pending; });
            const auto notePosition = block.notePositionAtSample ? block.notePositionAtSample(block.clock, sample)
                : MidiNotePosition{};
            const auto fallbackTick = block.notePositionAtSample ? 0.0
                : std::max(static_cast<double>(section.sourceTick), projection.sourceTick(sample, section));
            for (auto& voice : active_) {
                if (!voice.runtimeId || voice.releasing || !voice.note.expressionIdentity) continue;
                const auto fresh = voice.expressionFresh;
                const auto visibleStart = std::max(section.sourceTick,voice.note.start);
                const auto startsHere = projection.sample(voice.note.start) == sample;
                const auto noteTick = fresh && (sample == first || startsHere)
                    ? static_cast<double>(visibleStart - voice.note.start)
                    : block.notePositionAtSample ? notePosition.relativeTo(projection.phase, voice.note.start)
                    : fallbackTick - static_cast<double>(voice.note.start);
                voice.expressionFresh = false;
                for (std::size_t d = 0; d < voice.note.expression.size(); ++d) {
                    const auto range = voice.note.expression[d];
                    if (!range.count) continue;
                    const auto points = expression.subspan(range.first,range.count);
                    const auto knot = nextKnot(points,projection,sample,voice.note.start);
                    const bool atKnot = knot != points.end() && projection.sample(voice.note.start+knot->tick) == sample;
                    if (!fresh && !atKnot && sample % expressionStep != 0) continue;
                    const auto value = valueAt(points,projection,sample,voice.note.start,
                        noteTick,visibleStart) + (d==0 ? voice.note.tuning/100.0 : 0.0);
                    if (voice.expressionSent[d] && voice.expressionValue[d] == value) continue;
                    if (out.capacity()-out.size() <= owed) {
                        expressionBlocked = true;
                        voice.expressionFresh = true;
                        break;
                    }
                    auto event = voice.note.on;
                    event.type = EventType::NoteExpression;
                    event.frame = offset;
                    event.noteId = voice.runtimeId;
                    event.dim = static_cast<std::uint16_t>(d);
                    event.paramId = kNativeNoteExpressionUnits;
                    event.value = value;
                    (void)out.push(event);
                    voice.expressionValue[d] = value;
                    voice.expressionSent[d] = true;
                }
            }
        }
        item = next;
        cursor = sample + 1;
    }
    return true;
}

} // namespace adi::engine
