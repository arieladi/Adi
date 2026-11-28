// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/engine/slot_recording.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
std::atomic<unsigned> callbackAllocations{0};
thread_local bool rejectNextAllocation = false;
}
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t size) {
    if (adi::audio::onAudioThread) ++callbackAllocations;
    if (std::exchange(rejectNextAllocation, false)) throw std::bad_alloc();
    if (auto* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
ADI_TEST_NOINLINE void* operator new[](std::size_t size) { return operator new(size); }
ADI_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
ADI_TEST_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
ADI_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
ADI_TEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#undef ADI_TEST_NOINLINE

namespace {
using namespace adi::engine;
using namespace adi::engine::launch;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void input(SlotInputRecorder& recorder, const AudioIo& io,
           std::optional<SlotRecordingWindow> window = {}, std::span<const SlotMidiInput> midi = {},
           std::uint64_t inputEpoch = 1) {
    adi::audio::CallbackScope callback;
    recorder.process(io, window, midi, inputEpoch);
}

SlotRecordingResult finish(SlotInputRecorder& recorder, bool deviceStopped = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = recorder.poll(deviceStopped)) return std::move(*result);
        if (!deviceStopped) input(recorder, {});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("slot recording did not finalize");
}

SlotMidiInput event(std::int64_t stream, std::int32_t frame, EventType type, std::uint64_t id,
                   std::uint64_t port = 0) {
    Event message;
    message.type = type;
    message.frame = frame;
    message.noteId = id;
    message.dim = 64;
    message.value = 0.5;
    return {stream + frame, stream + frame, message, port};
}

void mixedTake() {
    adi::test::TempDirectory directory{"slot_recording", "mixed"};
    AudioRecordingSpec format;
    format.sampleRate = 48000;
    format.workFolder = directory.path();
    format.sourceProject = "mixed slot test";
    format.maxFrames = 64;
    format.ringFrames = 256;
    SlotInputRecorder recorder;
    const std::array targets{SlotRecordingTarget{{22, 7}, {0}}, SlotRecordingTarget{{11, 7}, {}}};
    std::string error;
    require(recorder.start(format, targets, error), "prepare private writers and freeze slot keys");
    require(!recorder.start(format, targets, error), "one recording gesture may own the input writers");

    std::array<float, 100> source{};
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i) / 128;
    constexpr std::int64_t stream = 1234;
    constexpr std::int64_t first = stream + 12;
    constexpr std::int64_t last = first + 80;
    constexpr auto keys = midiInputKey("keys");
    constexpr auto pads = midiInputKey("pads");
    AudioIo block;
    block.numIn = 1;
    const float* channels[]{source.data()};
    block.in = channels;
    block.frames = 8;
    block.streamTimeSamples = stream - block.frames;
    input(recorder, block, SlotRecordingWindow{first, {}});
    require(recorder.state() == SlotRecordingState::Armed, "no input is admitted before the quantized edge");

    block.frames = 32;
    block.streamTimeSamples = stream;
    const std::array beginEvents{event(stream, 11, EventType::NoteOn, 1, pads),
                                 event(stream, 12, EventType::NoteOn, 2, keys)};
    input(recorder, block, SlotRecordingWindow{first, {}}, beginEvents);
    require(recorder.state() == SlotRecordingState::Recording, "begin inside the block starts the take");
    require(!recorder.poll(), "drain live MIDI packets without finalizing the active take");

    channels[0] = source.data() + 32;
    block.frames = 17;
    block.streamTimeSamples += 32;
    auto late = event(block.streamTimeSamples, 0, EventType::NoteOn, 3);
    late.scheduledSample = first - 1;
    late.inputSample = late.scheduledSample;
    const std::array nextEvents{late, event(block.streamTimeSamples, 0, EventType::NoteOff, 2, keys)};
    input(recorder, block, SlotRecordingWindow{first, last}, nextEvents);

    channels[0] = source.data() + 49;
    block.frames = 51;
    block.streamTimeSamples += 17;
    const std::array endEvents{event(block.streamTimeSamples, 42, EventType::NoteOn, 4, pads),
                               event(block.streamTimeSamples, 43, EventType::NoteOn, 5)};
    input(recorder, block, SlotRecordingWindow{first, last}, endEvents);
    require(recorder.state() == SlotRecordingState::Complete, "stop inside the callback closes the take");
    require(!recorder.poll(), "writer must acknowledge the stop on the driver before returning a file");
    auto result = finish(recorder);
    require(result.state == SlotRecordingState::Complete && result.frames == 80 && result.sampleRate == 48000,
            "half-open capture has exactly 80 clip-local frames");
    require(result.sourceProject == format.sourceProject, "the capture carries its originating document identity");
    require(result.targets.size() == 2 && result.targets[0].slot == targets[0].slot &&
            result.targets[1].slot == targets[1].slot, "completed media and MIDI stay bound to their slots");
    require(result.audio && result.audio->failure == RecordingFailure::None && result.audio->frames == 80 &&
            result.audio->startSample == 0 && result.audio->tracks.size() == 1 &&
            result.audio->tracks[0].trackId == targets[0].slot.trackId,
            "native WAV recorder finalizes the exact slot-local range");
    std::array<float, 80> recorded{};
    {
        adi::audio::WavReader reader(result.audio->tracks[0].file);
        require(reader.frames() == 80 && reader.read(recorded.data(), 80) == 80 &&
                std::equal(recorded.begin(), recorded.end(), source.begin() + 12),
                "all 80 recorded PCM frames match their source exactly");
    }
    std::vector<MidiCapturePacket> midiEvents;
    for (const auto& packet : result.midi)
        if (packet.kind == MidiCapturePacket::Kind::Event) midiEvents.push_back(packet);
    require(result.midiState == MidiCaptureState::Complete && midiEvents.size() == 3 &&
            midiEvents[0].timelineSample == 0 && midiEvents[0].event.noteId == 2 && midiEvents[0].port == keys &&
            midiEvents[1].timelineSample == 20 && midiEvents[1].event.noteId == 2 && midiEvents[1].port == keys &&
            midiEvents[2].timelineSample == 79 && midiEvents[2].event.noteId == 4 && midiEvents[2].port == pads,
            "MIDI keeps its source ports, note identities and exact samples within the capture window");
    require(!result.audio->discard() && !recorder.poll(), "completed capture can be discarded and returns once");

    require(recorder.start(format, targets, error), "rearm the mixed take after publication");
    block.in = nullptr;
    const std::array failedEvents{event(block.streamTimeSamples, 0, EventType::NoteOn, 6)};
    input(recorder, block, SlotRecordingWindow{block.streamTimeSamples, {}}, failedEvents);
    result = finish(recorder);
    require(result.state == SlotRecordingState::InputFailure && result.frames == 0 && result.audio &&
            result.audio->failure == RecordingFailure::MissingInput && result.midi.empty(),
            "an audio failure never publishes half a mixed capture slice");
    require(!result.audio->discard(), "failed private media can be discarded");

    require(recorder.start(format, targets, error), "prepare writers before queue admission");
    recorder.cancel();
    input(recorder, {});
    result = finish(recorder);
    require(result.state == SlotRecordingState::Cancelled && result.frames == 0 && result.audio &&
            result.audio->failure == RecordingFailure::Cancelled && result.audio->directory().empty(),
            "cancelling an unqueued slot take releases private audio and returns to idle");
    require(recorder.state() == SlotRecordingState::Idle, "cancelled slot recorder can be armed again");
}

void arrangementCrashRecoveryRefused() {
    adi::test::TempDirectory directory{"slot_recording", "recovery"};
    AudioRecordingSpec format;
    format.workFolder = directory.path();
    format.sourceProject = (directory.path() / "Song.adi").string();
    format.recoveryProjectUuid = "slot-recording-project";
    const std::array targets{SlotRecordingTarget{{11, 7}, {0}}, SlotRecordingTarget{{22, 7}, {}}};
    SlotInputRecorder recorder;
    std::string error;
    require(!recorder.start(format, targets, error) && recorder.state() == SlotRecordingState::Idle &&
            error.find("scene ownership") != std::string::npos && std::filesystem::is_empty(directory.path()),
            "reject an Arrangement crash journal before either mixed slot writer arms");
    format.recoveryProjectUuid.clear();
    require(recorder.start(format, targets, error), "ordinary private WAV capture can arm after recovery refusal");
    recorder.cancel();
    input(recorder, {});
    const auto cancelled = finish(recorder);
    require(cancelled.state == SlotRecordingState::Cancelled && cancelled.frames == 0 &&
            std::filesystem::is_empty(directory.path()), "ordinary cancelled take clears its private writer");
}

void interruptionAndReuse() {
    const std::array targets{SlotRecordingTarget{{11, 7}, {}}};
    const std::array invalid{targets[0], SlotRecordingTarget{{11, 8}, {}}};
    SlotInputRecorder recorder;
    std::string error;
    require(!recorder.start({}, invalid, error) && recorder.state() == SlotRecordingState::Idle,
            "refuse two recording slots on one track before changing state");
    require(recorder.start({}, targets, error), "arm a MIDI-only take without an audio folder");
    AudioIo block;
    block.frames = 16;
    block.streamTimeSamples = 200;
    input(recorder, block, SlotRecordingWindow{200, {}});
    block.streamTimeSamples += 17;
    input(recorder, block, SlotRecordingWindow{200, {}});
    auto result = finish(recorder);
    require(result.state == SlotRecordingState::Interrupted && result.frames == 16 &&
            !result.audio && result.midi.size() == 1 && result.midi[0].frames == 16,
            "stream discontinuity preserves the recorded prefix for explicit recovery");

    require(recorder.start({}, targets, error), "rearm after draining the interrupted gesture");
    input(recorder, block, SlotRecordingWindow{220, 220});
    result = finish(recorder);
    require(result.state == SlotRecordingState::Complete && result.frames == 0 && result.midi.empty(),
            "cancel at the start boundary without recycling old media or packets");
    require(recorder.start({}, targets, error), "rearm after a zero-length gesture");
    input(recorder, block, SlotRecordingWindow{220, {}});
    result = finish(recorder, true);
    require(result.state == SlotRecordingState::Interrupted && result.frames == 13 &&
            result.midiState == MidiCaptureState::DeviceStopped && result.midi.size() == 1,
            "actual device stop yields the quiesced prefix, with the native MIDI stop reason");

    require(recorder.start({}, targets, error), "rearm after a device interruption");
    input(recorder, block, SlotRecordingWindow{block.streamTimeSamples, {}}, {}, 2);
    const auto first = block.streamTimeSamples;
    block.streamTimeSamples += block.frames;
    input(recorder, block, SlotRecordingWindow{first, {}}, {}, 3);
    result = finish(recorder);
    require(result.state == SlotRecordingState::InputFailure && result.frames == 16 &&
            result.midiState == MidiCaptureState::InputReset && result.midi.size() == 1,
            "MIDI input reset keeps the previously accepted slice and ends the gesture");
}

void retryOwnerDrain() {
    SlotInputRecorder recorder;
    const std::array targets{SlotRecordingTarget{{11, 7}, {}}};
    std::string error;
    require(recorder.start({}, targets, error), "arm MIDI for owner drain retry");
    AudioIo block;
    block.frames = 16;
    const std::array messages{event(0, 0, EventType::NoteOn, 99)};
    input(recorder, block, SlotRecordingWindow{0, 16}, messages);
    bool allocationFailed = false;
    rejectNextAllocation = true;
    try {
        (void)recorder.poll();
    } catch (const std::bad_alloc&) {
        allocationFailed = true;
    }
    rejectNextAllocation = false;
    require(allocationFailed, "test an exhausted owner-side destination buffer");
    const auto result = finish(recorder);
    require(result.midi.size() == 2 && result.midi[1].event.noteId == 99,
            "retry drains the same span and event after owner allocation failure");
}

} // namespace

int main() {
    try {
        mixedTake();
        arrangementCrashRecoveryRefused();
        interruptionAndReuse();
        retryOwnerDrain();
        require(callbackAllocations == 0, "slot recording performs zero callback allocations");
        require(adi::audio::callbackFileIo == 0, "slot recording performs zero callback file I/O");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("Slot recording captured 80 frames at 48 kHz, zero boundary/PCM error, zero callback allocations.\n");
}
