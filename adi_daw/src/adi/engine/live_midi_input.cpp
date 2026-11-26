// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/live_midi_input.hpp"
#include "adi/engine/midi_input_capture.hpp"
#include "adi/engine/transport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adi::engine {
namespace {
constexpr std::array<std::uint32_t, 3> pedals{64, 66, 69};
}

bool LiveMidiInput::push(std::int64_t sample, const Event& event, std::int64_t inputSample) noexcept {
    if (sample < 0 || inputSample < 0 || inputSample > sample || !isNoteStream(event.type) || event.channel > 15 || !std::isfinite(event.value))
        return false;
    if (event.type == EventType::Controller && event.paramId > 127) return false;
    if ((event.type == EventType::NoteOn || event.type == EventType::NoteOff) &&
        (event.dim > 127 || event.noteId == 0)) return false;
    const auto next = written_.load(std::memory_order_relaxed);
    if (next - read_.load(std::memory_order_acquire) == capacity) {
        // The dropped packet could be a release. Cancel the old epoch so
        // the callback releases its voices and does not play stale onsets.
        reset();
        return false;
    }
    ring_[next % capacity] = {sample, inputSample, epoch_.load(std::memory_order_acquire), event};
    written_.store(next + 1, std::memory_order_release);
    return true;
}

void LiveMidiInput::reset() noexcept {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void LiveMidiInput::release(const GraphHost::RenderBlock& block, const Route& route, std::int32_t frame) noexcept {
    auto& state = *route.voices;
    state.retry = false;
    for (auto& voice : state.active) {
        if (!voice.sourceId || (!state.flush && !voice.releasing)) continue;
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
    state.flush = state.retry;
}

void LiveMidiInput::deliver(const GraphHost::RenderBlock& block, const Route& route,
                            const Event& event) noexcept {
    auto& state = *route.voices;
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
    const auto voice = std::find_if(state.active.begin(), state.active.end(),
                                    [&](const Voices::Voice& v) { return v.sourceId == event.noteId; });
    auto note = event;
    if (event.type == EventType::NoteOn) {
        if (voice != state.active.end() || voiceSerial_ == kLiveNoteIdPrefix - 1) return;
        const auto free = std::find_if(state.active.begin(), state.active.end(),
                                       [](const Voices::Voice& v) { return v.sourceId == 0; });
        if (free == state.active.end()) return;
        note.noteId = kLiveNoteIdPrefix | ++voiceSerial_;
        if (!block.pushInputEvent(route.slot, note)) return;
        note.type = EventType::NoteOff;
        note.value = 0;
        *free = {event.noteId, note};
        return;
    }
    if (voice == state.active.end() || voice->releasing) return;
    if (note.type == EventType::NoteOff) {
        note = voice->off;
        note.frame = event.frame;
        note.value = event.value;
        if (block.pushInputEvent(route.slot, note)) *voice = {};
        else {
            voice->off = note;
            voice->releasing = true;
            state.retry = true;
        }
    } else {
        note.noteId = voice->off.noteId;
        (void)block.pushInputEvent(route.slot, note);
    }
}

void LiveMidiInput::render(const GraphHost::RenderBlock& block, const AudioIo& io,
                          std::span<const Route> routes, const Transport& transport,
                          MidiInputCapture* capture, RecordingPunch punch, const RecordingPunchWindow& punchWindow) noexcept {
    const auto epoch = epoch_.load(std::memory_order_acquire);
    if (capture) capture->beginSlice(io, transport, epoch, punch, punchWindow);
    const auto generation = block.generation();
    for (const auto& route : routes) {
        if (generation != generation_)
            route.slot = route.processes ? block.find(*route.node) : kInvalidNode;
        auto& state = *route.voices;
        if (state.track != route.monitorTrack || epoch != renderedEpoch_) state.flush = true;
        state.track = route.monitorTrack;
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
    const auto eventFrame = [&io](const Timed& item) {
        return static_cast<std::int32_t>(std::max(item.sample - io.streamTimeSamples, std::int64_t{0}));
    };
    auto next = first;
    for (; next != written; ++next) {
        const auto& item = ring_[next % capacity];
        if (item.epoch < epoch) continue;
        if (item.epoch != epoch || item.sample >= end) break;
        if (capture) {
            auto event = item.event;
            event.frame = eventFrame(item);
            capture->record(item.sample, item.inputSample, event);
        }
    }
    // Only monitoring is gated. Recording above sees the original input regardless of the track's Monitor switch.
    for (const auto& route : routes) {
        if (!route.monitorTrack || route.slot == kInvalidNode) continue;
        auto& state = *route.voices;
        const bool followClips = transport.playing() && !route.blockedRanges.empty();
        auto input = first;
        for (std::int32_t frame = 0; frame < io.frames;) {
            auto runEnd = io.frames;
            bool blocked = false;
            if (followClips) {
                const auto sample = transport.sampleAt(frame);
                auto remaining = transport.contiguousFrames(frame, io.frames - frame);
                const auto range = std::upper_bound(route.blockedRanges.begin(), route.blockedRanges.end(), sample,
                    [](std::int64_t at, const MidiMonitorRange& span) { return at < span.end; });
                if (range != route.blockedRanges.end()) {
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
            for (; input != next; ++input) {
                const auto& item = ring_[input % capacity];
                if (item.epoch < epoch) continue;
                const auto at = eventFrame(item);
                if (at >= runEnd) break;
                if (!blocked && !state.flush) {
                    auto event = item.event;
                    event.frame = at;
                    deliver(block, route, event);
                }
            }
            frame = runEnd;
        }
    }
    read_.store(next, std::memory_order_release);
    if (capture) capture->endSlice();
}

}  // namespace adi::engine
