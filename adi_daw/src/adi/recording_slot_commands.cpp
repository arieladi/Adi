// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/recording_commands.hpp"

#include "adi/clip_image.hpp"
#include "adi/controller_data.hpp"
#include "adi/engine/slot_recording.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/midi_capture.hpp"
#include "adi/store.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace adi::recording {
namespace {
struct SlotMidiContent {
    std::vector<notes::NoteValues> notes;
    std::map<std::pair<std::uint64_t, ExpressionDim>, std::vector<ExpressionPoint>> expression;
    std::map<std::pair<int, int>, std::vector<controllers::Point>> controllers;
};

int capturedVelocity(double value, bool onset) {
    if (std::signbit(value) || value > 1 || (onset && value == 0))
        throw std::runtime_error("invalid slot note velocity");
    const auto velocity = std::lround(value * 127);
    if (static_cast<double>(velocity) / 127 != value)
        throw std::runtime_error("slot note velocity cannot be represented exactly in the native note stream");
    return static_cast<int>(velocity);
}

template<class SampleToTick>
SlotMidiContent readSlotMidi(const engine::launch::SlotRecordingResult& capture,
                            std::int64_t length, SampleToTick tickAt, engine::MidiInputFilter input) {
    SlotMidiContent content;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> held;
    std::int64_t spanBegin = 0, spanEnd = 0, previous = 0;
    std::optional<std::uint64_t> epoch;
    for (const auto& packet : capture.midi) {
        if (packet.epoch == 0 || (epoch && *epoch != packet.epoch) || packet.pass != 0)
            throw std::runtime_error("slot MIDI packets span multiple capture epochs or passes");
        epoch = packet.epoch;
        if (packet.kind == engine::MidiCapturePacket::Kind::Span) {
            if (packet.frames <= 0 || packet.timelineSample != spanEnd || packet.frames > capture.frames - spanEnd)
                throw std::runtime_error("slot MIDI packets do not cover a continuous capture");
            spanBegin = spanEnd;
            spanEnd += packet.frames;
            continue;
        }

        const auto& event = packet.event;
        const auto sample = packet.timelineSample;
        if (sample < spanBegin || sample >= spanEnd || sample < previous ||
            event.frame != sample - spanBegin || !std::isfinite(event.value) || event.channel > 15)
            throw std::runtime_error("invalid slot MIDI event or sample order");
        previous = sample;
        if (!input.matches(packet.port, event.channel)) continue;
        const auto tick = tickAt(sample);
        const bool channelPitch = adi::capture::isChannelPitchWheel(event);
        if (event.type == engine::EventType::Controller || channelPitch) {
            const auto address = channelPitch ? controllers::kPitchBend : event.paramId;
            const auto value = channelPitch ? adi::capture::channelPitchWheelValue(event.value) : event.value;
            if (!controllers::validController(address) || value < 0 || value > 1)
                throw std::runtime_error("invalid slot MIDI controller address or value");
            auto& points = content.controllers[{event.channel, static_cast<int>(address)}];
            points.push_back({points.size() + 1, tick, value});
            continue;
        }
        if (event.noteId == 0)
            throw std::runtime_error("slot MIDI note or expression has no note identity");
        const auto identity = std::pair{packet.port, event.noteId};
        if (event.type == engine::EventType::NoteOn) {
            if (event.dim > 127 || !held.emplace(identity, content.notes.size()).second)
                throw std::runtime_error("invalid or duplicate held slot note");
            content.notes.push_back({.start = tick, .duration = length - tick, .id = content.notes.size() + 1,
                .key = event.dim, .velocity = capturedVelocity(event.value, true), .channel = event.channel});
            continue;
        }

        const auto active = held.find(identity);
        if (active == held.end()) continue; // The matching onset can precede the launch boundary.
        auto& note = content.notes[active->second];
        if (event.type == engine::EventType::NoteOff) {
            if (event.dim != note.key || event.channel != note.channel || tick <= note.start)
                throw std::runtime_error("slot MIDI release mismatches its onset or has no duration");
            note.duration = tick - note.start;
            note.releaseVelocity = capturedVelocity(event.value, false);
            held.erase(active);
        } else if (event.type == engine::EventType::NoteExpression) {
            if (event.dim > std::numeric_limits<std::uint8_t>::max() ||
                std::abs(event.value) > std::numeric_limits<float>::max())
                throw std::runtime_error("invalid slot expression dimension or value");
            const auto dimension = static_cast<ExpressionDim>(event.dim);
            auto& lane = content.expression[{note.id, dimension}];
            const ExpressionPoint point{tick - note.start, static_cast<float>(event.value), 0, 0, 0, {}};
            if (!lane.empty() && lane.back().time_ticks == point.time_ticks) lane.back() = point;
            else lane.push_back(point);
        } else {
            throw std::runtime_error("unexpected addressed event in slot MIDI capture");
        }
    }
    if (spanEnd != capture.frames)
        throw std::runtime_error("slot MIDI capture has missing frames");
    return content;
}

std::vector<MidiExpressionLane> expressionLanes(const SlotMidiContent& midi) {
    std::vector<MidiExpressionLane> expression;
    expression.reserve(midi.expression.size());
    for (const auto& [key, points] : midi.expression)
        expression.push_back({key.first, key.second, points});
    return expression;
}
} // namespace

RecordingEdit prepareSlotCommit(Store& store, const engine::launch::SlotRecordingResult& capture,
                                std::span<const ClipIds> ids, const engine::TempoMap& clock,
                                const StagedTake* staged) {
    RecordingEdit result;
    try {
        if (store.readOnly() || audio::onAudioThread)
            throw std::runtime_error("slot recording preparation needs a writable project off the callback");
        if (capture.state != engine::launch::SlotRecordingState::Complete || capture.frames <= 0 ||
            capture.sampleRate < 8000 || capture.sampleRate > 768000 || capture.targets.empty() ||
            capture.targets.size() != ids.size() || capture.targets.size() > 256)
            throw std::runtime_error("slot commit needs a complete take and one identity per target");
        if (clock.events.empty() || clock.events.front().posTicks != 0)
            throw std::runtime_error("slot commit needs the performed tempo from take origin");
        std::int64_t previous = -1;
        for (const auto& point : clock.events) {
            if (point.posTicks <= previous || point.curve != 0 || !std::isfinite(point.bpm) ||
                point.bpm < 20 || point.bpm > 999)
                throw std::runtime_error("slot recording needs a valid performed step-tempo map");
            previous = point.posTicks;
        }
        const auto tickAt = [&](std::int64_t sample) { return RecordingEdit::slotTickAt(clock, sample, capture.sampleRate); };
        const auto length = tickAt(capture.frames);
        if (length <= 0) throw std::runtime_error("slot recording has no musical extent");

        std::optional<SQLite::Transaction> read;
        if (sqlite3_get_autocommit(store.db().getHandle()) != 0)
            read.emplace(store.db(), SQLite::TransactionBehavior::DEFERRED);
        if (!store.db().tableExists("scenes") || !store.db().tableExists("clip_slots"))
            throw std::runtime_error("Session slot storage is unavailable in this project");
        std::set<std::int64_t> tracks, clips;
        std::vector<ClipIds> audioIds;
        bool midi = false;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto& target = capture.targets[i];
            const auto& slot = target.slot;
            const auto& id = ids[i];
            if (id.clip <= 0 || !clips.insert(id.clip).second || !tracks.insert(slot.trackId).second ||
                edit::row(store.db(), "clips", id.clip))
                throw std::runtime_error("slot recording needs fresh clip IDs and distinct target tracks");
            SQLite::Statement destination(store.db(),
                "SELECT t.kind,t.locked,t.frozen,cs.clip_id FROM main.tracks t CROSS JOIN main.scenes s "
                "LEFT JOIN main.clip_slots cs ON cs.track_id=t.id AND cs.scene_id=s.id WHERE t.id=? AND s.id=?");
            destination.bind(1, slot.trackId);
            destination.bind(2, slot.sceneId);
            if (!destination.executeStep() || destination.getColumn(1).getInt() || destination.getColumn(2).getInt())
                throw std::runtime_error("slot recording target is missing, locked or frozen");
            if (!destination.getColumn(3).isNull())
                throw std::runtime_error("slot recording target is already occupied");
            const auto kind = destination.getColumn(0).getString();
            if (target.audioChannels.empty()) {
                if ((kind != "instrument" && kind != "midi") || id.media != 0)
                    throw std::runtime_error("MIDI slot recording needs a MIDI target without a media identity");
                if (target.midiInput.channel < 0 || target.midiInput.channel > 16)
                    throw std::runtime_error("slot recording has an invalid frozen MIDI input channel");
                midi = true;
            } else {
                if (!capture.audio || audioIds.size() >= capture.audio->tracks.size())
                    throw std::runtime_error("missing audio for a recorded slot");
                const auto& input = capture.audio->tracks[audioIds.size()];
                if ((kind != "audio" && kind != "midi") || input.trackId != slot.trackId ||
                    input.channels != target.audioChannels.size())
                    throw std::runtime_error("recorded audio does not match its frozen slot target");
                audioIds.push_back(id);
            }
        }
        if (midi ? capture.midiState != engine::MidiCaptureState::Complete : !capture.midi.empty())
            throw std::runtime_error("slot commit needs a complete MIDI capture matching its targets");
        std::vector<SlotMidiContent> midiTakes;
        for (const auto& target : capture.targets)
            if (target.audioChannels.empty())
                midiTakes.push_back(readSlotMidi(capture, length, tickAt, target.midiInput));
        constexpr auto label = "Record Session";
        if (capture.audio) {
            const auto& audio = *capture.audio;
            if (audio.loopSpec() || audio.punchWindow().active() || audio.placementOffsetSamples != 0 ||
                audio.startSample != 0 || audio.sampleRate != capture.sampleRate ||
                audio.frames != static_cast<std::uint64_t>(capture.frames))
                throw std::runtime_error("slot audio must contain its exact capture-local sample range");
        }
        const auto mediaIds = RecordingEdit::prepareSlotFiles(store, capture.sourceProject,
            capture.audio ? &*capture.audio : nullptr, audioIds, result, staged);
        for (auto& request : result.requests) request.label = label;

        clip_edit::CopyIds allocated(store.db());
        std::size_t audioIndex = 0, midiIndex = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto& target = capture.targets[i];
            const auto& id = ids[i];
            const auto track = target.slot.trackId;
            Payload image;
            if (target.audioChannels.empty()) {
                const auto& midiContent = midiTakes[midiIndex++];
                const auto expression = expressionLanes(midiContent);
                std::vector<MidiControllerLane> controllerLanes;
                controllerLanes.reserve(midiContent.controllers.size());
                for (const auto& [key, points] : midiContent.controllers)
                    controllerLanes.push_back({key.first, key.second, points});
                image = RecordingEdit::slotMidiImage(store,
                    {track, id.clip, 0, length, id.name, midiContent.notes, expression, false, controllerLanes}, allocated);
            } else {
                image = RecordingEdit::slotAudioImage(store, clock, id, track, mediaIds[audioIndex++],
                    capture.frames, length, capture.sampleRate);
            }
            edit::RowEdits creation(store.db());
            clip_edit::createImage(creation, image);
            OpRequest make;
            make.opType = "clip.duplicate";
            make.payload = {{"id", id.clip}, {"changes", creation.changes()}};
            make.label = label;
            make.targetKind = "clip";
            make.targetId = id.clip;
            result.requests.push_back(std::move(make));
            OpRequest assign;
            assign.opType = "session.setSlotClip";
            assign.payload = {{"track", track}, {"scene", target.slot.sceneId}, {"clip", id.clip}};
            assign.label = label;
            assign.targetKind = "clip";
            assign.targetId = id.clip;
            result.requests.push_back(std::move(assign));
        }
        result.ok = true;
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

} // namespace adi::recording
