// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/live_midi_input.hpp"
#include "adi/blob.hpp"
#include "adi/controller_data.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/engine/transport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
constexpr std::array<std::uint32_t, 3> pedals{64, 66, 69};
constexpr auto pitch = static_cast<std::uint16_t>(ExpressionDim::Pitch);

std::int64_t playDelay(const LiveMidiInput::Route& route, const LiveMidiInput::PlayClock& clock,
                       const Transport& transport, std::int32_t frame) noexcept {
    if (route.playGrid == 0 || !transport.playing())
        return 0;
    const auto timeline = transport.sampleAt(frame);
    const auto release = playQuantizeRelease(timeline, clock.tempo, clock.sampleRate,
                                              static_cast<double>(route.playGrid), route.playForgiveness);
    if (!release)
        return 0;
    // A wrap starts a new pass. Use its boundary for a grid line beyond this pass.
    const auto due = transport.looping() ? std::min(*release, transport.loopEnd()) : *release;
    return due - timeline;
}
}

bool LiveMidiInput::push(std::int64_t sample, const Event& event, std::int64_t inputSample,
                         std::uint64_t port) noexcept {
    if (sample < 0 || inputSample < 0 || inputSample > sample || !isNoteStream(event.type) || event.channel > 15 || !std::isfinite(event.value))
        return false;
    if (event.type == EventType::Controller &&
        (!controllers::validController(event.paramId) ||
         (event.paramId == controllers::kPitchBend && (event.value < 0 || event.value > 1)))) return false;
    if ((event.type == EventType::NoteOn || event.type == EventType::NoteOff) &&
        (event.dim > 127 || event.noteId == 0)) return false;
    const auto next = written_.load(std::memory_order_relaxed);
    if (next - read_.load(std::memory_order_acquire) == capacity) {
        // The dropped packet could be a release. Cancel the old epoch so
        // the callback releases its voices and does not play stale onsets.
        reset();
        return false;
    }
    ring_[next % capacity] = {sample, inputSample, epoch_.load(std::memory_order_acquire), event, port};
    written_.store(next + 1, std::memory_order_release);
    return true;
}

void LiveMidiInput::reset() noexcept {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void LiveMidiInput::release(const GraphHost::RenderBlock& block, const Route& route, std::int32_t frame) noexcept {
    auto& state = *route.voices;
    state.retry = false;
    if (state.flush)
        state.pendingCount = 0;
    for (auto& voice : state.active) {
        if (!voice.sourceId || (!state.flush && !voice.releasing)) continue;
        if (voice.awaitingOn) {
            voice = {};
            continue;
        }
        voice.off.frame = frame;
        if (block.pushInputEvent(route.slot, voice.off)) voice = {};
        else state.retry = true;
    }
    if (!state.flush) return;
    for (std::size_t ch = 0; ch < state.pedals.size(); ++ch)
        for (std::size_t p = 0; p < pedals.size(); ++p) {
            if (!state.pedals[ch][p]) continue;
            if (block.pushInputEvent(route.slot, {frame, EventType::Controller, static_cast<std::uint8_t>(ch),
                                                 0, 0, pedals[p], 0}))
                state.pedals[ch][p] = false;
            else state.retry = true;
        }
    for (std::size_t ch = 0; ch < state.bends.size(); ++ch) {
        auto& bend = state.bends[ch];
        if (bend == Voices::Bend::None) continue;
        const bool controller = bend == Voices::Bend::Controller;
        // Use the same event representation as the live wheel.
        const Event center{frame, controller ? EventType::Controller : EventType::NoteExpression,
                           static_cast<std::uint8_t>(ch), controller ? std::uint16_t{0} : pitch,
                           0, controller ? controllers::kPitchBend : 0u, controller ? .5 : 0.};
        if (block.pushInputEvent(route.slot, center)) {
            bend = Voices::Bend::None;
            if (frame == 0)
                state.pitchCenteredAtStart |= static_cast<std::uint16_t>(1u << ch);
        } else {
            state.retry = true;
        }
    }
    state.flush = state.retry;
}

void LiveMidiInput::deliver(const GraphHost::RenderBlock& block, const Route& route, const Event& event,
                            std::int64_t streamStart, const Transport& transport, const PlayClock& clock) noexcept {
    auto& state = *route.voices;
    const bool controllerBend = event.type == EventType::Controller && event.paramId == controllers::kPitchBend;
    const bool channelPitch = event.type == EventType::NoteExpression && event.noteId == 0 && event.dim == pitch;
    if (controllerBend || channelPitch) {
        auto wheel = event;
        wheel.noteId = 0;
        auto& bend = state.bends[event.channel];
        const bool centered = event.value == (controllerBend ? .5 : 0.);
        if (block.pushInputEvent(route.slot, wheel)) {
            const auto form = controllerBend ? Voices::Bend::Controller : Voices::Bend::Expression;
            bend = centered ? Voices::Bend::None : form;
        } else if (centered && bend != Voices::Bend::None) {
            state.flush = true;
        }
        return;
    }
    if (event.type == EventType::Controller) {
        auto controller = event;
        controller.noteId = 0;
        const auto pedal = std::find(pedals.begin(), pedals.end(), event.paramId);
        const bool accepted = block.pushInputEvent(route.slot, controller);
        if (pedal != pedals.end()) {
            auto& held = state.pedals[event.channel][static_cast<std::size_t>(pedal - pedals.begin())];
            if (accepted) held = event.value >= 0.5;
            else if (held && event.value < 0.5) state.flush = true;
        }
        return;
    }
    if (event.type == EventType::NoteExpression && event.noteId == 0) {
        (void)block.pushInputEvent(route.slot, event);
        return;
    }
    auto voice = std::find_if(state.active.begin(), state.active.end(),
                               [&](const Voices::Voice& v) { return v.sourceId == event.noteId; });
    if (event.type == EventType::NoteOn) {
        if (voice != state.active.end() || voiceSerial_ == kLiveNoteIdPrefix - 1) return;
        voice = std::find_if(state.active.begin(), state.active.end(),
                             [](const Voices::Voice& v) { return v.sourceId == 0; });
        if (voice == state.active.end()) return;
        auto off = event;
        off.noteId = kLiveNoteIdPrefix | ++voiceSerial_;
        off.type = EventType::NoteOff;
        off.value = 0;
        *voice = {event.noteId, off, false, true, playDelay(route, clock, transport, event.frame)};
    }
    if (voice == state.active.end() || voice->releasing) return;
    if (voice->playDelay == 0) {
        sendVoice(block, route, *voice, event);
        return;
    }

    const auto sample = streamStart + event.frame;
    if (state.pendingCount < state.pending.size() &&
        voice->playDelay <= std::numeric_limits<std::int64_t>::max() - sample) {
        const auto due = sample + voice->playDelay;
        auto delayed = event;
        delayed.noteId = voice->off.noteId;
        const auto end = state.pending.begin() + static_cast<std::ptrdiff_t>(state.pendingCount);
        const auto position = std::upper_bound(state.pending.begin(), end, due,
            [](std::int64_t frame, const Voices::Pending& pending) { return frame < pending.due; });
        std::move_backward(position, end, end + 1);
        *position = {due, delayed};
        ++state.pendingCount;
        return;
    }
    // Overflow may drop an attack or expression. A sounding note still gets its release.
    if (event.type == EventType::NoteOn || (event.type == EventType::NoteOff && voice->awaitingOn))
        *voice = {};
    else if (event.type == EventType::NoteOff)
        sendVoice(block, route, *voice, event);
}

void LiveMidiInput::sendVoice(const GraphHost::RenderBlock& block, const Route& route, Voices::Voice& voice,
                              const Event& event) noexcept {
    auto note = event;
    note.noteId = voice.off.noteId;
    if (note.type == EventType::NoteOn) {
        if (block.pushInputEvent(route.slot, note))
            voice.awaitingOn = false;
        else
            voice = {};
        return;
    }
    if (voice.releasing || voice.awaitingOn) return;
    if (note.type == EventType::NoteOff) {
        note = voice.off;
        note.frame = event.frame;
        note.value = event.value;
        if (block.pushInputEvent(route.slot, note))
            voice = {};
        else {
            voice.off = note;
            voice.releasing = true;
            route.voices->retry = true;
        }
    } else {
        (void)block.pushInputEvent(route.slot, note);
    }
}

void LiveMidiInput::drainPending(const GraphHost::RenderBlock& block, const Route& route,
                                 std::int64_t streamStart, std::int32_t beforeFrame) noexcept {
    auto& state = *route.voices;
    std::size_t count = 0;
    while (count < state.pendingCount && state.pending[count].due < streamStart + beforeFrame) {
        const auto& pending = state.pending[count++];
        const auto voice = std::find_if(state.active.begin(), state.active.end(),
            [&](const Voices::Voice& v) { return v.sourceId != 0 && v.off.noteId == pending.event.noteId; });
        if (voice == state.active.end())
            continue;
        auto event = pending.event;
        event.frame = static_cast<std::int32_t>(std::max<std::int64_t>(0, pending.due - streamStart));
        sendVoice(block, route, *voice, event);
    }
    if (count != 0) {
        std::move(state.pending.begin() + static_cast<std::ptrdiff_t>(count),
                  state.pending.begin() + static_cast<std::ptrdiff_t>(state.pendingCount), state.pending.begin());
        state.pendingCount -= count;
    }
}

void LiveMidiInput::render(const GraphHost::RenderBlock& block, const AudioIo& io,
                          std::span<const Route> routes, std::span<const std::shared_ptr<History>> histories,
                          const Transport& transport, const PlayClock& clock,
                          MidiInputCapture* capture, RecordingPunch punch, const RecordingPunchWindow& punchWindow) noexcept {
    const auto epoch = epoch_.load(std::memory_order_acquire);
    if (capture) capture->beginSlice(io, transport, epoch, punch, punchWindow);
    const auto generation = block.generation();
    for (const auto& route : routes) {
        if (generation != generation_)
            route.slot = route.processes ? block.find(*route.node) : kInvalidNode;
        auto& state = *route.voices;
        state.pitchCenteredAtStart = 0;
        const bool playChanged = state.playGrid != 0 && (state.playGrid != route.playGrid ||
            state.playForgiveness != route.playForgiveness || state.transportRevision != transport.revision());
        if (state.track != route.monitorTrack || state.filter != route.filter || epoch != renderedEpoch_ || playChanged)
            state.flush = true;
        state.track = route.monitorTrack;
        state.filter = route.filter;
        state.playGrid = route.playGrid;
        state.playForgiveness = route.playForgiveness;
        state.transportRevision = transport.revision();
        if (route.slot != kInvalidNode && (state.flush || state.retry)) release(block, route, 0);
    }
    generation_ = generation;
    renderedEpoch_ = epoch;

    if (io.streamTimeSamples < 0 ||
        io.streamTimeSamples > std::numeric_limits<std::int64_t>::max() - io.frames)
        return;
    const auto end = io.streamTimeSamples + io.frames;
    const auto first = read_.load(std::memory_order_relaxed);
    const auto written = written_.load(std::memory_order_acquire);
    // Silent slices advance every history's capture boundary, and a newly
    // armed history can appear without changing the input epoch.
    const bool changesHistory = !histories.empty();
    if (changesHistory) historyRevision_.fetch_add(1);
    for (const auto& history : histories) {
        if (history->inputEpoch == epoch) continue;
        history->memory.setListening(true, false);
        history->memory.discontinuity(io.streamTimeSamples);
        history->inputEpoch = epoch;
    }
    const auto eventFrame = [&io](const Timed& item) {
        return static_cast<std::int32_t>(std::max(item.sample - io.streamTimeSamples, std::int64_t{0}));
    };
    auto next = first;
    for (; next != written; ++next) {
        const auto& item = ring_[next % capacity];
        if (item.epoch < epoch) continue;
        if (item.epoch != epoch || item.sample >= end) break;
        if (capture || !histories.empty()) {
            auto event = item.event;
            event.frame = eventFrame(item);
            if (capture) capture->record(item.sample, item.inputSample, event, item.port);
            for (const auto& history : histories)
                if (history->filter.matches(item.port, event.channel))
                    history->memory.push(event, io.streamTimeSamples + event.frame);
        }
    }
    for (const auto& history : histories) history->memory.advance(end);
    if (changesHistory) historyRevision_.fetch_add(1);
    // Only monitoring is gated. Recording above sees the original input regardless of the track's Monitor switch.
    for (const auto& route : routes) {
        if (!route.monitorTrack || route.slot == kInvalidNode) continue;
        auto& state = *route.voices;
        // During overdub the armed Auto track hears the original clip and the new notes.
        const bool followClips = transport.playing() && !route.blockedRanges.empty() &&
                                 !(capture && capture->overdubs(route.monitorTrack));
        auto input = first;
        for (std::int32_t frame = 0; frame < io.frames;) {
            auto runEnd = io.frames;
            bool blocked = false;
            if (followClips) {
                const auto sample = transport.sampleAt(frame);
                auto remaining = transport.contiguousFrames(frame, io.frames - frame);
                // The nonempty range uses raw pointers to keep Debug vector locks off the callback.
                const auto* firstRange = route.blockedRanges.data();
                const auto* lastRange = firstRange + route.blockedRanges.size();
                const auto* range = std::upper_bound(firstRange, lastRange, sample,
                    [](std::int64_t at, const MidiMonitorRange& span) { return at < span.end; });
                if (range != lastRange) {
                    blocked = sample >= range->start;
                    remaining = std::min(remaining, (blocked ? range->end : range->start) - sample);
                }
                runEnd = frame + static_cast<std::int32_t>(remaining);
            }
            // Clip starts and loop wraps can silence a held note even when no input arrives in this block.
            if (blocked && !state.blocked) {
                state.flush = true;
                release(block, route, frame);
            }
            state.blocked = blocked;
            // Ending overdub inside a clip can hand back to an authored wheel
            // whose value the clip scheduler already sent earlier.
            if (blocked && frame == 0 && route.authoredPitchChase)
                *route.authoredPitchChase |= state.pitchCenteredAtStart;
            for (; input != next; ++input) {
                const auto& item = ring_[input % capacity];
                if (item.epoch < epoch) continue;
                const auto at = eventFrame(item);
                if (at >= runEnd) break;
                if (!blocked && !state.flush && route.filter.matches(item.port, item.event.channel)) {
                    if (state.pendingCount != 0)
                        drainPending(block, route, io.streamTimeSamples, at + 1);
                    auto event = item.event;
                    event.frame = at;
                    deliver(block, route, event, io.streamTimeSamples, transport, clock);
                }
            }
            if (!blocked && !state.flush && state.pendingCount != 0)
                drainPending(block, route, io.streamTimeSamples, runEnd);
            frame = runEnd;
        }
    }
    read_.store(next, std::memory_order_release);
    if (capture) capture->endSlice();
}

bool LiveMidiInput::pushPreview(PreviewCommand command) noexcept {
    const auto next = previewWritten_.load(std::memory_order_relaxed);
    if (next - previewRead_.load(std::memory_order_acquire) == previewCapacity) {
        // A refused command can be a release. Invalidate the queued notes and
        // release the audition on the next callback, independent of Track input.
        previewEpoch_.fetch_add(1, std::memory_order_release);
        return false;
    }
    command.epoch = previewEpoch_.load(std::memory_order_relaxed);
    previewRing_[next % previewCapacity] = command;
    previewWritten_.store(next + 1, std::memory_order_release);
    return true;
}

bool LiveMidiInput::preview(const PreviewRoute& route, int key, int velocity, bool held) noexcept {
    if (!route.node || route.track <= 0 || key < 0 || key > 127 || velocity < 1 || velocity > 127)
        return false;
    return pushPreview({route.node, route.track, 0, key, velocity, held});
}

void LiveMidiInput::stopPreview() noexcept { (void)pushPreview({}); }

void LiveMidiInput::clearPreviewDevice() noexcept {
    previewRead_.store(previewWritten_.load(std::memory_order_acquire), std::memory_order_release);
    activePreview_ = nullptr;
    previewClock_ = 0;
    previewGeneration_ = 0;
}

void LiveMidiInput::renderPreview(const GraphHost::RenderBlock& block, std::int32_t frames,
                                 std::span<const PreviewRoute> routes, double sampleRate) noexcept {
    constexpr double minimumSeconds = 0.020;
    constexpr double tapSeconds = 0.120;
    const auto first = previewClock_;
    const auto end = first + static_cast<std::uint64_t>(frames);
    previewClock_ = end;
    const auto epoch = previewEpoch_.load(std::memory_order_acquire);
    const auto generation = block.generation();
    const PreviewRoute* active = nullptr;
    for (const auto& route : routes) {
        auto& voice = *route.voice;
        if (generation != previewGeneration_)
            route.slot = (route.track || voice.off.noteId) ? block.find(*route.node) : kInvalidNode;
        if (!voice.off.noteId)
            continue;
        if (&voice == activePreview_ && voice.track == route.track && voice.epoch == epoch && route.slot != kInvalidNode) {
            active = &route;
            continue;
        }
        // A removed instance can still be revived by Undo. Keep its release
        // until the same consuming node is present and can take the event.
        voice.off.frame = 0;
        if (route.slot != kInvalidNode && block.pushInputEvent(route.slot, voice.off))
            voice = {};
    }
    previewGeneration_ = generation;
    if (!active)
        activePreview_ = nullptr;

    auto cursor = first;
    const auto releaseActive = [&] {
        if (!active)
            return true;
        auto& voice = *active->voice;
        const auto due = std::max(cursor, voice.releaseAt);
        if (due >= end)
            return false;
        voice.off.frame = static_cast<std::int32_t>(due - first);
        if (!block.pushInputEvent(active->slot, voice.off))
            return false;
        voice = {};
        cursor = due;
        active = nullptr;
        activePreview_ = nullptr;
        return true;
    };

    auto next = previewRead_.load(std::memory_order_relaxed);
    const auto written = previewWritten_.load(std::memory_order_acquire);
    for (; next != written; ++next) {
        const auto& command = previewRing_[next % previewCapacity];
        if (command.epoch != epoch) {
            if (command.epoch > epoch)
                break;
            continue;
        }
        if (!command.node) {
            if (active) {
                auto& voice = *active->voice;
                voice.releaseAt = std::min(voice.releaseAt, std::max(cursor, voice.earliestRelease));
            }
            continue;
        }
        const auto target = std::find_if(routes.begin(), routes.end(), [&](const PreviewRoute& route) {
            return route.track == command.track && route.node == command.node && route.slot != kInvalidNode;
        });
        if (target == routes.end())
            continue;
        if (active) {
            auto& voice = *active->voice;
            voice.releaseAt = std::min(voice.releaseAt, std::max(cursor, voice.earliestRelease));
            if (!releaseActive())
                break;
        }
        if (target->voice->off.noteId)
            break; // do not overtake an old release to the same device
        if (voiceSerial_ == kLiveNoteIdPrefix - 1)
            continue;
        Event note{static_cast<std::int32_t>(cursor - first), EventType::NoteOn, 0,
                   static_cast<std::uint16_t>(command.key), kLiveNoteIdPrefix | (voiceSerial_ + 1), 0,
                   static_cast<double>(command.velocity) / 127.0};
        if (!block.pushInputEvent(target->slot, note))
            break;
        ++voiceSerial_;
        note.type = EventType::NoteOff;
        note.value = 0;
        const auto minimum = static_cast<std::uint64_t>(std::max(1.0, std::ceil(sampleRate * minimumSeconds)));
        const auto tap = static_cast<std::uint64_t>(std::max(1.0, std::ceil(sampleRate * tapSeconds)));
        *target->voice = {note, command.track, epoch, cursor + minimum,
                          command.held ? std::numeric_limits<std::uint64_t>::max() : cursor + tap};
        active = &*target;
        activePreview_ = target->voice;
    }
    previewRead_.store(next, std::memory_order_release);
    (void)releaseActive();
}

}  // namespace adi::engine
