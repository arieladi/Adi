// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/link_playback.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/audio/warp.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/media/blake3.hpp"
#include "adi/digest.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#ifdef ADI_TEST_LINK_WRAP_LOCKS
#include <pthread.h>
#endif

namespace { std::atomic<unsigned> allocations{0}, frees{0}, locks{0}; }
#ifdef ADI_TEST_LINK_WRAP_LOCKS
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_trylock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    if (adi::audio::onAudioThread) ++locks;
    return __real_pthread_mutex_lock(mutex);
}
extern "C" int __wrap_pthread_mutex_trylock(pthread_mutex_t* mutex) {
    if (adi::audio::onAudioThread) ++locks;
    return __real_pthread_mutex_trylock(mutex);
}
#endif
void* operator new(std::size_t n) {
    if (adi::audio::onAudioThread) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (p && adi::audio::onAudioThread) ++frees;
    std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
// Nothrow allocations use the same counted malloc/free pair under sanitizers.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return ::operator new[](n); } catch (...) { return nullptr; }
}
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }

namespace {
using namespace adi;
using namespace adi::engine;
constexpr auto ppq = textproj::kPPQ;
constexpr double rate = 48000;
constexpr std::int32_t blockSize = 64;
int checks = 0, failures = 0;
void check(bool good, const char* message) {
    ++checks;
    if (!good) { ++failures; std::printf("FAIL: %s\n", message); }
}

struct Audit {
    unsigned allocationsBefore = allocations.load(), freesBefore = frees.load();
    unsigned ioBefore = audio::callbackFileIo.load(), locksBefore = locks.load();
    bool clean() const {
        return allocationsBefore == allocations.load() && freesBefore == frees.load() &&
               ioBefore == audio::callbackFileIo.load() && locksBefore == locks.load();
    }
};

// The right channel is a real MIDI instrument; the left is a real streamed,
// warped audio clip. Checking both catches a clock update that moves only one.
struct Instrument final : device::DeviceInstance {
    device::DeviceIdentity identity_;
    std::array<Event, 256> events{};
    std::size_t count = 0;
    std::uint64_t active = 0;
    TransportInfo last{};
    unsigned stateLoads = 0;
    std::int32_t delayFrames = 0;
    std::array<double, 256> delay{};
    std::size_t delayAt = 0;
    const device::DeviceIdentity& identity() const noexcept override { return identity_; }
    bool loaded() const noexcept override { return true; }
    bool processesDouble() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    std::int64_t tailSamples() const noexcept override { return kInfiniteTail; }
    std::int32_t latencySamples() const noexcept override { return delayFrames; }
    bool loadState(const std::string&, const std::vector<std::uint8_t>&) override {
        ++stateLoads; return true;
    }
    void process(const NodeIo& io) noexcept override {
        if (io.transport) last = *io.transport;
        for (const auto& event : io.events)
            if ((event.type == EventType::NoteOn || event.type == EventType::NoteOff) && count < events.size())
                events[count++] = event;
        const auto* next = io.events.begin();
        for (std::int32_t frame = 0; frame < io.frames; ++frame) {
            const auto at = io.blockOffset + frame;
            while (next != io.events.end() && next->frame <= at) {
                const auto event = *next++;
                if (event.type == EventType::NoteOn) active = event.noteId;
                if (event.type == EventType::NoteOff && event.noteId == active) active = 0;
            }
            const double input = active ? .125 : 0;
            delay[delayAt] = input;
            const auto read = (delayAt + delay.size() - static_cast<std::size_t>(delayFrames)) % delay.size();
            const double delayed = delay[read];
            delayAt = (delayAt + 1) % delay.size();
            for (std::int32_t channel = 0; channel < io.channels; ++channel) {
                const double value = channel == 1 ? delayed : 0;
                if (io.wide()) io.out64[channel][at] = value;
                else io.out[channel][at] = static_cast<float>(value);
            }
        }
    }
    std::size_t ons() const {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const Event& event) { return event.type == EventType::NoteOn; }));
    }
    std::size_t offs() const {
        return static_cast<std::size_t>(std::count_if(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count),
            [](const Event& event) { return event.type == EventType::NoteOff; }));
    }
};

struct Content {
    std::int64_t clipStart = 0;
    bool ramp = false;
    std::optional<std::pair<std::int64_t, std::int64_t>> loop;
};

struct Project {
    test::TempDirectory directory{"link_playback", "project"};
    std::unique_ptr<Store> store;
    Session session;
    Instrument* instrument = nullptr;
    unsigned instances = 0, sqlOnCallback = 0;
    std::string originalDigest;
    int originalChanges = 0;
    explicit Project(Precision precision, std::optional<double> external = 120, Content content = {}) {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "song.adi", error);
        if (!store) throw std::runtime_error("create Link playback project");
        std::vector<float> samples(96000, 0);
        for (std::size_t i = 0; i < 48000; ++i)
            samples[i * 2] = content.ramp ? .125f + static_cast<float>(i) / 192000.f : .25f;
        {
            audio::WavWriter writer(directory.path() / "source.wav", 48000, 2, audio::WavFormat::Float32);
            writer.write(samples.data(), 48000); writer.close();
        }
        auto& db = store->db();
        db.exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                "(1,'audio','Warp',0),(2,'midi','Notes',1),(9,'master','Main',2);"
                "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES"
                "(1,'vst3','test.link-playback','Native instrument','instrument');"
                "INSERT INTO device_chains(id,track_id) VALUES(1,2);"
                "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Native instrument');"
                "DELETE FROM tempo_map;INSERT INTO tempo_map(pos_ticks,bpm,curve) VALUES(0,120,0);"
                "INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames) VALUES"
                "(1,'fixture','source.wav','wav',48000,2,48000);"
                "INSERT INTO clips(id,track_id,kind,time_base,pos_ticks,length_ticks) VALUES"
                "(7,1,'audio',0,0,11531520),(8,2,'midi',0,0,11531520);"
                "INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled,warp_mode) VALUES"
                "(7,1,48000,1,'re-pitch');"
                "INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(7,0,1000000000)");
        if (content.clipStart != 0) {
            SQLite::Statement position(db, "UPDATE clips SET pos_ticks=?");
            position.bind(1, content.clipStart); position.exec();
        }
        if (content.loop) {
            SQLite::Statement loop(db, "INSERT INTO session_state(key,value) VALUES('transport.loop',?)");
            loop.bind(1, Payload{{"on", true}, {"start", content.loop->first}, {"end", content.loop->second}}.dump());
            loop.exec();
        }
        const auto hash = media::blake3File(directory.path() / "source.wav");
        if (!hash) throw std::runtime_error("hash Link playback fixture");
        SQLite::Statement setHash(db, "UPDATE media_files SET hash_blake3=? WHERE id=1");
        setHash.bind(1, hash.hex); setHash.exec();
        const auto warp = audio::writeWarpMap(std::array{WarpMarker{0, 0}, WarpMarker{48000, 2 * ppq}}, 48000);
        SQLite::Statement setWarp(db, "UPDATE audio_clips SET warp_markers=? WHERE clip_id=7");
        setWarp.bind(1, warp.bytes.data(), static_cast<int>(warp.bytes.size())); setWarp.exec();
        NoteRecord note{};
        note.start_ticks = 0; note.dur_ticks = ppq; note.note_id = 42;
        note.key = 60; note.channel = 1; note.vel_on = 100; note.vel_off = 40; note.probability = 10000;
        if (!store->putEventStream(8, "notes", writeStream<NoteRecord>(FourCC::Notes, std::array{note})))
            throw std::runtime_error("write Link playback notes");
        session.graph().setFadeFrames(0);
        session.graph().setLatencyHeadroom(0);
        SessionSpec spec{2, rate, blockSize}; spec.precision = precision; spec.externalTempoBpm = external;
        if (!session.load(*store, [this](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
            auto value = std::make_unique<Instrument>(); instrument = value.get(); ++instances; return value;
        }, spec)) throw std::runtime_error(session.error());
        if (!session.problems().empty()) throw std::runtime_error(session.problems().front());
        prime();
        originalDigest = digestProject(*store).text;
        originalChanges = sqlite3_total_changes(db.getHandle());
        sqlite3_trace_v2(db.getHandle(), SQLITE_TRACE_STMT,
            [](unsigned, void* opaque, void*, void*) {
                if (audio::onAudioThread) ++*static_cast<unsigned*>(opaque);
                return 0;
            }, &sqlOnCallback);
    }
    ~Project() { sqlite3_trace_v2(store->db().getHandle(), 0, nullptr, nullptr); }
    void prime() {
        // The live transport must stay stopped until Link's selected sample.
        // A stopped prime() requests no pages, so prepare its audible launch
        // position through an immutable playing snapshot on this owner thread.
        auto target = session.transport();
        target.play();
        if (!session.clips()->primeAt(target, std::chrono::seconds(10)))
            throw std::runtime_error("prime Link playback PCM");
    }
    bool unchanged() const {
        return originalDigest == digestProject(*store).text &&
               originalChanges == sqlite3_total_changes(store->db().getHandle()) &&
               store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0;
    }
};

struct Buffer {
    std::array<float, blockSize * 2> narrow{};
    std::array<double, blockSize * 2> wide{};
    float* output[2]{narrow.data(), narrow.data() + blockSize};
    double* output64[2]{wide.data(), wide.data() + blockSize};
    AudioIo io;
    explicit Buffer(Precision precision) {
        io.frames = blockSize; io.numOut = 2;
        if (precision == Precision::Float64) io.out64 = output64; else io.out = output;
    }
    double sample(int channel, int frame) const {
        const auto index = static_cast<std::size_t>(channel * blockSize + frame);
        return io.out64 ? wide[index] : narrow[index];
    }
    bool silent(int first, int end) const {
        for (int c = 0; c < 2; ++c) for (int i = first; i < end; ++i)
            if (sample(c, i) != 0) return false;
        return true;
    }
    bool sounds(int first, int end) const {
        for (int c = 0; c < 2; ++c) for (int i = first; i < end; ++i)
            if (std::abs(sample(c, i)) < .01) return false;
        return true;
    }
};

std::int64_t microsForFrames(std::int64_t frames) {
    return static_cast<std::int64_t>(std::llround(static_cast<double>(frames) * 1000000 / rate));
}
LinkFrame captured(std::int64_t audible, bool playing = true, double tempo = 120) {
    LinkFrame frame;
    frame.valid = true; frame.enabled = true; frame.startStopSync = true;
    frame.requestedPlaying = playing; frame.tempo = tempo;
    frame.audibleMicros = audible; frame.transportChangeMicros = audible;
    frame.hasLaunchTime = true; frame.launchMicros = audible;
    return frame;
}
std::unique_ptr<LinkSession> participant() {
    std::string error;
    auto result = LinkSession::create(120, error);
    if (!result) throw std::runtime_error(error);
    return result;
}
LinkPlayback::Result render(LinkPlayback& playback, Buffer& buffer, const LinkFrame& frame,
                            const LinkCommand& command = {}) {
    audio::CallbackScope callback;
    return playback.processCaptured(buffer.io, frame, 4, command);
}
void launchStop() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Project project(width);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(width);
        const auto* instance = project.instrument;
        const auto beforeLoads = instance->stateLoads;
        constexpr std::int64_t origin = 1000000;
        auto frame = captured(origin);
        frame.launchMicros = origin + microsForFrames(17);
        Audit audit;
        const auto start = render(playback, buffer, frame);
        check(start.valid && start.started && start.startOffset == 17 && !start.waiting,
              "captured peer launch starts the real Session at physical frame 17");
        check(buffer.silent(0, 17) && buffer.sounds(17, 64),
              "native Warp audio and MIDI instrument both start at frame 17 in either precision");
        check(project.session.transport().position() == 47 && project.instrument->ons() == 1,
              "only the played physical slice advances the native clock and starts one note");
        const auto noteId = project.instrument->active;
        frame = captured(origin + microsForFrames(64), false);
        frame.transportChangeMicros = frame.audibleMicros + microsForFrames(23);
        frame.beat = 64 * 120. / (60 * rate);
        const auto stop = render(playback, buffer, frame);
        check(stop.valid && stop.stopped && stop.stopOffset == 23 && !project.session.transport().playing(),
              "captured peer stop lands at physical frame 23 of the following callback");
        check(buffer.sounds(0, 23) && buffer.silent(23, 64),
              "actual PCM from both native paths stops at the selected interior sample");
        check(project.session.transport().position() == 70 && project.instrument->offs() == 1 &&
              project.instrument->active == 0 && project.instrument->events[1].noteId == noteId,
              "stop advances exactly 23 frames and releases the original held note identity");
        check(audit.clean() && project.sqlOnCallback == 0,
              "native launch and stop callbacks perform no allocation, free, file I/O, SQL or linked mutex calls");
        check(project.instrument == instance && project.instances == 1 && instance->stateLoads == beforeLoads,
              "start and stop keep the actual loaded device instance and its state");
        check(project.unchanged(), "Link performance writes no project rows, changes or history");
    }
}

void preparation() {
    Project project(Precision::Float32, std::nullopt);
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float32);
    auto frame = captured(2000000, false, 90);
    auto first = render(playback, buffer, frame);
    check(first.valid && first.waiting && static_cast<bool>(first.preparation),
          "an unprepared external tempo returns an owned preparation packet");
    Audit audit;
    bool callbackAccepted = false;
    { audio::CallbackScope callback; callbackAccepted = playback.prepare(*project.store, first.preparation, frame.audibleMicros); }
    check(!callbackAccepted && audit.clean() && project.sqlOnCallback == 0,
          "owner preparation rejects callback use before touching SQL or allocating");
    check(!playback.prepare(*project.store, first.preparation, frame.audibleMicros + 2000001),
          "owner refuses a tempo packet more than two seconds old");
    frame.audibleMicros += 1000; frame.tempo = 100;
    const auto replacement = render(playback, buffer, frame);
    check(static_cast<bool>(replacement.preparation) &&
          replacement.preparation.serial != first.preparation.serial &&
          !playback.prepare(*project.store, first.preparation, frame.audibleMicros),
          "a later tempo request supersedes an earlier owner's packet");
    const auto* instance = project.instrument;
    check(playback.prepare(*project.store, replacement.preparation, frame.audibleMicros),
          "serialized owner prepares the requested native performance generation");
    check(!playback.prepare(*project.store, replacement.preparation, frame.audibleMicros),
          "an already consumed preparation packet cannot be applied twice");
    project.prime();
    frame = captured(frame.audibleMicros + 1000, true, 100);
    const auto started = render(playback, buffer, frame);
    check(started.valid && started.started && !started.waiting && buffer.sounds(0, 64),
          "newly prepared tempo drives real native PCM and MIDI on the next launch");
    check(project.instrument == instance && project.instances == 1 && project.instrument->last.bpm == 100,
          "owner clock preparation keeps the instance while its actual TransportInfo changes");
    check(project.unchanged(), "preparation, stale rejection and performance leave authored tempo and history unchanged");
}

void latencyBoundary() {
    Project project(Precision::Float64);
    project.instrument->delayFrames = 37;
    check(project.session.rebuild() && project.session.graph().currentGraph()->latencySamples() == 37,
          "actual delayed instrument makes a nonzero compensated native graph");
    project.prime();
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float64);
    const LinkBuffer timing{link->clockMicros(), 4000, 4};
    LinkCommand play; play.transport = LinkTransport::Play;
    Audit audit;
    LinkPlayback::Result result;
    { audio::CallbackScope callback; result = playback.process(buffer.io, timing, play); }
    check(result.valid && result.frame.audibleMicros == timing.callbackMicros + 4000 + microsForFrames(37),
          "actual SDK capture adds physical latency and the pinned graph compensation exactly once");
    check(buffer.silent(0, 37) && buffer.sounds(37, 64),
          "real instrument delay and compensation align MIDI and Warp PCM at the same sample");
    check(audit.clean() && project.sqlOnCallback == 0,
          "actual SDK-to-native callback with nonzero graph latency remains realtime clean");
}

void continuity() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Project project(width);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(width);
        auto frame = captured(3000000);
        (void)render(playback, buffer, frame);
        const auto held = project.instrument->active;
        check(held != 0 && project.instrument->ons() == 1, "native MIDI note is held before external tempo change");
        const auto initialPosition = project.session.transport().position();
        frame.audibleMicros += microsForFrames(64);
        frame.beat = static_cast<double>(initialPosition) * 120 / (60 * rate);
        frame.tempo = 240;
        const auto request = render(playback, buffer, frame);
        check(static_cast<bool>(request.preparation), "live peer tempo change asks the owner for a performed boundary");
        const auto before = project.session.transport().position();
        check(playback.prepare(*project.store, request.preparation, frame.audibleMicros),
              "live tempo change prepares the private musical clock off callback");
        project.prime();
        frame.audibleMicros += microsForFrames(64);
        frame.beat += 64 * 240. / (60 * rate);
        Audit audit;
        const auto next = render(playback, buffer, frame);
        check(next.valid && project.session.transport().position() == before + 64 && buffer.sounds(0, 64),
              "performed tempo change preserves elapsed samples and continuous native audio");
        check(project.instrument->active == held && project.instrument->ons() == 1 && project.instrument->offs() == 0,
              "clock-only generation preserves the held authored note without off/retrigger");
        check(audit.clean() && project.sqlOnCallback == 0, "clock generation handoff has a clean realtime callback");
        frame.enabled = false;
        const auto disable = render(playback, buffer, frame);
        check(static_cast<bool>(disable.preparation) && !disable.preparation.tempo,
              "disabling Link requests restoration of the authored project tempo");
        check(playback.prepare(*project.store, disable.preparation, frame.audibleMicros),
              "owner restores the authored clock after Link is disabled");
        project.prime();
        const auto restored = render(playback, buffer, frame);
        check(restored.valid && !project.session.spec().externalTempoBpm && project.instrument->last.bpm == 120,
              "disabled consumer actually renders with authored tempo again");
        check(project.instances == 1 && project.unchanged(),
              "tempo change and disable restoration retain one device and the complete authored project");
    }
}

void optOutAndValidation() {
    Project project(Precision::Float64);
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float64);
    auto frame = captured(4000000);
    frame.startStopSync = false;
    const auto ignored = render(playback, buffer, frame);
    check(ignored.valid && !ignored.started && !project.session.transport().playing() && buffer.silent(0, 64),
          "Start Stop Sync opt-out ignores a peer's start intention");
    LinkCommand local; local.transport = LinkTransport::Play;
    const auto start = render(playback, buffer, frame, local);
    check(start.valid && start.started && buffer.sounds(0, 64),
          "local Play still starts native playback while peer transport sync is opted out");
    frame.requestedPlaying = false; frame.transportChangeMicros += 1000;
    const auto keep = render(playback, buffer, frame);
    check(keep.valid && !keep.stopped && project.session.transport().playing(),
          "peer stop cannot override the opted-out local transport intention");
    local.transport = LinkTransport::Stop;
    const auto stop = render(playback, buffer, frame, local);
    check(stop.valid && stop.stopped && !project.session.transport().playing() && project.instrument->active == 0,
          "local Stop releases the native instrument while synchronization is opted out");
    check(!playback.setLaunchTick(-1), "negative project launch tick is refused");
    check(playback.setLaunchTick(ppq / 2), "driver accepts a valid musical launch position");
    frame.startStopSync = true; frame.requestedPlaying = true; frame.transportChangeMicros += 1000;
    frame.launchMicros = frame.audibleMicros;
    const auto seek = render(playback, buffer, frame);
    check(seek.valid && seek.started && project.session.transport().position() == 12064 && buffer.sounds(0, 64),
          "quantized launch at a nonzero tick locates native PCM and chases the sustained note");
    project.session.transport().locate(12000);
    project.session.transport().loop(12000, 12032);
    const auto beforeOn = project.instrument->ons(), beforeOff = project.instrument->offs();
    frame.audibleMicros += microsForFrames(64);
    frame.beat = .5;
    const auto loop = render(playback, buffer, frame);
    check(loop.valid && project.session.transport().position() == 12000 &&
          project.instrument->ons() >= beforeOn + 2 && project.instrument->offs() >= beforeOff + 2,
          "native short loop releases and chases note ownership across two interior boundaries");
    project.session.transport().loop(12000, 12032, false);
    const auto before = project.session.transport().position();
    frame.valid = false;
    std::fill(buffer.wide.begin(), buffer.wide.end(), 7);
    const auto invalid = render(playback, buffer, frame);
    check(!invalid.valid && project.session.transport().position() == before && buffer.silent(0, 64),
          "invalid captured SDK frame outputs silence without advancing transport");
}

void drift() {
    Project project(Precision::Float32);
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float32);
    auto frame = captured(5000000);
    (void)render(playback, buffer, frame);
    const auto held = project.instrument->active;
    frame.audibleMicros += microsForFrames(64);
    frame.beat = 64 * 120. / (60 * rate);
    const auto aligned = render(playback, buffer, frame);
    check(aligned.valid && std::abs(aligned.phaseErrorFrames) <= 2 && !aligned.preparation,
          "ordinary tick and microsecond quantization does not request a clock rebuild");
    frame.audibleMicros += microsForFrames(64);
    frame.beat = (128 + 8) * 120. / (60 * rate);
    const auto correction = render(playback, buffer, frame);
    check(correction.valid && correction.phaseErrorFrames > 2 && static_cast<bool>(correction.preparation) &&
          correction.preparation.tempo && *correction.preparation.tempo > 120 &&
          *correction.preparation.tempo <= 120 * 1.005,
          "positive phase drift requests bounded musical rate correction rather than a transport seek");
    const auto before = project.session.transport().position();
    check(playback.prepare(*project.store, correction.preparation, frame.audibleMicros),
          "phase correction prepares on the serialized owner");
    project.prime();
    frame.audibleMicros += microsForFrames(64);
    frame.beat += 64 * 120. / (60 * rate);
    const auto corrected = render(playback, buffer, frame);
    check(corrected.valid && project.session.transport().position() == before + 64 &&
          project.instrument->active == held && project.instrument->ons() == 1 && project.instrument->offs() == 0,
          "prepared phase correction advances native time continuously and never retriggers the held note");
    check(std::abs(corrected.phaseErrorFrames) < std::abs(correction.phaseErrorFrames),
          "native musical-clock phase error decreases after owner correction");
    check(project.unchanged(), "phase correction stays outside the project and undo history");
}

void pendingLaunchUpdates() {
    Project project(Precision::Float32);
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float32);
    constexpr std::int64_t origin = 6000000;
    auto frame = captured(origin);
    frame.launchMicros = origin + microsForFrames(128);
    const auto pending = render(playback, buffer, frame);
    check(pending.valid && pending.waiting && !pending.started && buffer.silent(0, 64),
          "a future peer launch stays pending without advancing the native transport");
    frame.audibleMicros = origin + microsForFrames(64);
    frame.beat = .1; frame.phase = .1;
    frame.launchMicros = frame.audibleMicros + microsForFrames(17);
    const auto moved = render(playback, buffer, frame);
    check(moved.valid && moved.started && moved.startOffset == 17 &&
          buffer.silent(0, 17) && buffer.sounds(17, 64),
          "same transport event follows its newly captured launch time after a peer phase move");
    check(project.session.transport().position() == 47 && project.unchanged(),
          "updated launch time starts at the authored origin with no project writes");
}

void restartClearsPerformedHistory() {
    Project project(Precision::Float64);
    check(project.session.setExternalTempoAt(*project.store, 240, ppq / 2),
          "fixture prepares a performed history with an earlier 120 BPM segment and latest 240 BPM");
    project.prime();
    {
        auto block = project.session.beginRender();
        check(!block.constantExternalTempo() && block.sampleAtTick(ppq) == 18000,
              "performed-history fixture retains its old tempo before the boundary");
    }
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float64);
    auto frame = captured(7000000, true, 240);
    const auto request = render(playback, buffer, frame);
    check(request.valid && request.waiting && !request.started &&
          request.preparation && request.preparation.reset && buffer.silent(0, 64),
          "restart at tick zero refuses to reuse an old performed-history segment merely because latest BPM matches");
    check(playback.prepare(*project.store, request.preparation, frame.audibleMicros),
          "restart owner resets the performed tempo history to the current peer tempo");
    project.prime();
    frame.audibleMicros += microsForFrames(64); frame.launchMicros = frame.audibleMicros;
    const auto restarted = render(playback, buffer, frame);
    check(restarted.valid && restarted.started && restarted.startOffset == 0 && buffer.sounds(0, 64) &&
          project.instrument->last.bpm == 240,
          "restarted native Warp audio and MIDI use current 240 BPM from tick zero");
    {
        auto block = project.session.beginRender();
        check(block.constantExternalTempo() && block.sampleAtTick(ppq) == 12000,
              "new start's first beat takes 12000 samples instead of the previous performed history's 18000");
    }
    check(project.instances == 1 && project.unchanged(),
          "resetting performed history keeps the actual device and authored project intact");
}

void cancelPendingIntent() {
    {
        Project project(Precision::Float32);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(Precision::Float32);
        auto frame = captured(8000000);
        frame.launchMicros += microsForFrames(128);
        const auto pending = render(playback, buffer, frame);
        check(pending.waiting && !pending.started, "peer start is pending before Start Stop Sync opt-out");
        frame.startStopSync = false;
        frame.audibleMicros += microsForFrames(64);
        frame.launchMicros = frame.audibleMicros + microsForFrames(17);
        const auto cancelled = render(playback, buffer, frame);
        frame.audibleMicros += microsForFrames(128);
        const auto after = render(playback, buffer, frame);
        check(cancelled.valid && !cancelled.started && !after.started &&
              !project.session.transport().playing() && buffer.silent(0, 64) && project.instrument->ons() == 0,
              "opting out cancels an already pending peer start even after its launch time passes");
    }
    {
        Project project(Precision::Float32, std::nullopt);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(Precision::Float32);
        auto frame = captured(9000000, false, 90);
        const auto pending = render(playback, buffer, frame);
        check(static_cast<bool>(pending.preparation), "tempo preparation is pending while the native graph is still authored");
        frame.enabled = false; frame.audibleMicros += 1000;
        const auto disabled = render(playback, buffer, frame);
        check(disabled.valid && !disabled.preparation &&
              !playback.prepare(*project.store, pending.preparation, frame.audibleMicros) &&
              !project.session.spec().externalTempoBpm && project.unchanged(),
              "disable invalidates an in-flight external packet even when no external generation had reached the graph");
    }
}

void captureTimeAdmission() {
    Project project(Precision::Float32);
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float32);
    // No networking: the disabled actual SDK capture requests restoration from
    // the fixture's external clock. Positive physical latency must not make
    // its owner packet appear to have been captured in the future.
    const LinkBuffer timing{link->clockMicros(), 100000, 4};
    LinkPlayback::Result result;
    { audio::CallbackScope callback; result = playback.process(buffer.io, timing); }
    const auto now = link->clockMicros();
    check(result.valid && result.preparation && result.frame.capturedMicros &&
          *result.frame.capturedMicros == timing.callbackMicros &&
          result.preparation.capturedMicros == timing.callbackMicros &&
          result.frame.audibleMicros > result.preparation.capturedMicros,
          "actual SDK owner packet retains capture time separately from latency-shifted audible time");
    check(playback.prepare(*project.store, result.preparation, now) && !project.session.spec().externalTempoBpm,
          "immediate owner preparation with the real clock accepts positive-output-latency SDK capture");
}

void slowOwnerAndLoops() {
    {
        Project project(Precision::Float32);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(Precision::Float32);
        constexpr std::int64_t origin = 10000000;
        auto frame = captured(origin);
        (void)render(playback, buffer, frame);
        const auto held = project.instrument->active;
        frame.audibleMicros = origin + microsForFrames(64);
        frame.beat = 72 * 120. / (60 * rate);
        const auto first = render(playback, buffer, frame);
        check(static_cast<bool>(first.preparation), "drift starts one owner preparation before the owner is serviced");
        bool samePacket = true;
        Audit audit;
        for (int callback = 2; callback <= 12; ++callback) {
            frame.audibleMicros = origin + microsForFrames(callback * 64);
            frame.beat = (callback * 64 + 8 + .25 * callback) * 120 / (60 * rate);
            const auto next = render(playback, buffer, frame);
            samePacket = samePacket && next.valid && next.preparation &&
                next.preparation.serial == first.preparation.serial &&
                next.preparation.capturedMicros == first.preparation.capturedMicros;
        }
        check(samePacket && audit.clean(),
              "audio callbacks coalesce changing drift corrections while a slow owner works on the same peer tempo");
        check(playback.prepare(*project.store, first.preparation, frame.audibleMicros),
              "the original slow-owner drift packet remains eligible after twelve callbacks");
        project.prime();
        frame.audibleMicros += microsForFrames(64); frame.beat += 64 * 120. / (60 * rate);
        const auto after = render(playback, buffer, frame);
        check(after.valid && project.instrument->active == held &&
              project.instrument->ons() == 1 && project.instrument->offs() == 0 && project.unchanged(),
              "slow-owner clock publication preserves the sustained note and authored state");
    }
    {
        Project project(Precision::Float64);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(Precision::Float64);
        constexpr std::int64_t origin = 11000000;
        auto frame = captured(origin);
        (void)render(playback, buffer, frame);
        project.session.transport().loop(0, 32);
        bool follows = true;
        Audit audit;
        for (int callback = 1; callback <= 1024; ++callback) {
            frame.audibleMicros = origin + microsForFrames(callback * 64);
            frame.beat = callback * 64 * 120. / (60 * rate);
            const auto next = render(playback, buffer, frame);
            follows = follows && next.valid && std::abs(next.phaseErrorFrames) <= 2 &&
                !next.preparation && project.session.transport().position() == 0 && buffer.sounds(0, 64);
        }
        check(follows && audit.clean(),
              "over a second of repeated 32-sample loops preserves fractional beats without fabricated phase correction");
        check(project.instrument->ons() > 1 && project.instrument->offs() > 0 && project.unchanged(),
              "loop regression exercises actual native note release/chase without authoring clock changes");
    }
}

void repeatedLocalPlay() {
    Project project(Precision::Float64);
    auto link = participant();
    LinkPlayback playback(project.session, *link);
    Buffer buffer(Precision::Float64);
    constexpr std::int64_t origin = 12000000;
    auto frame = captured(origin);
    (void)render(playback, buffer, frame);
    const auto held = project.instrument->active;
    check(playback.setLaunchTick(ppq / 2), "a future stopped launch can have a different musical position");
    LinkCommand play; play.transport = LinkTransport::Play;
    bool continuous = true;
    Audit audit;
    for (int callback = 1; callback <= 4; ++callback) {
        const auto before = project.session.transport().position();
        frame.audibleMicros = origin + microsForFrames(callback * 64);
        frame.beat = callback * 64 * 120. / (60 * rate);
        frame.launchMicros = frame.audibleMicros;
        const auto again = render(playback, buffer, frame, play);
        continuous = continuous && again.valid && !again.started && !again.waiting &&
            !again.preparation && project.session.transport().position() == before + 64 &&
            project.instrument->active == held && buffer.sounds(0, 64);
    }
    check(continuous && project.instrument->ons() == 1 && project.instrument->offs() == 0,
          "explicit Play on an already playing Session is idempotent and never seeks or retriggers its held note");
    check(audit.clean() && project.instances == 1 && project.unchanged(),
          "repeated local Play preserves realtime safety, device ownership and authored state");
}

void localPlayCancelsPendingPeerStop() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Project project(width);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(width);
        constexpr std::int64_t origin = 19000000;
        auto frame = captured(origin);
        (void)render(playback, buffer, frame);
        const auto held = project.instrument->active;
        const auto revision = project.session.transport().revision();
        frame.audibleMicros = origin + microsForFrames(64);
        frame.beat = 64 * 120. / (60 * rate);
        frame.requestedPlaying = false;
        frame.transportChangeMicros = origin + microsForFrames(3 * 64 + 23);
        Audit audit;
        const auto queued = render(playback, buffer, frame);
        check(queued.valid && !queued.stopped && project.session.transport().playing() &&
              project.session.transport().position() == 128 && buffer.sounds(0, 64),
              "a future peer Stop remains pending while native PCM and MIDI continue");

        frame = captured(origin + microsForFrames(128));
        frame.beat = 128 * 120. / (60 * rate);
        LinkCommand play; play.transport = LinkTransport::Play;
        const auto renewed = render(playback, buffer, frame, play);
        check(renewed.valid && !renewed.started && !renewed.stopped &&
              project.session.transport().position() == 192 &&
              project.session.transport().revision() == revision && buffer.sounds(0, 64),
              "newer local Play cancels the pending peer Stop without restarting an already playing Session");

        bool continuous = true;
        for (int callback = 3; callback <= 4; ++callback) {
            frame.audibleMicros = origin + microsForFrames(callback * 64);
            frame.beat = callback * 64 * 120. / (60 * rate);
            const auto after = render(playback, buffer, frame);
            continuous = continuous && after.valid && !after.started && !after.stopped &&
                project.session.transport().playing() &&
                project.session.transport().position() == (callback + 1) * 64 && buffer.sounds(0, 64);
        }
        check(continuous && project.session.transport().revision() == revision && held != 0 &&
              project.instrument->active == held && project.instrument->ons() == 1 && project.instrument->offs() == 0,
              "obsolete Stop at frame 23 never interrupts native PCM, releases MIDI or relocates after a newer Play in either width");
        check(audit.clean() && project.sqlOnCallback == 0 && project.instances == 1 && project.unchanged(),
              "cancelling a pending peer Stop retains realtime safety, device ownership and authored history");
    }
}

void distantLaunchReadiness() {
    for (const auto width : {Precision::Float32, Precision::Float64}) {
        Content content; content.clipStart = 64 * ppq; content.ramp = true;
        Project project(width, 120, content);
        // Re-Pitch filters the finite source edge even at ratio one. Prepare
        // the expected musical source separately from Session's clip placement
        // and compare every launched sample, rather than assuming raw PCM.
        std::array<float, blockSize * 2> reference{};
        {
            audio::WarpRenderPlan plan;
            plan.source = project.directory.path() / "source.wav";
            plan.workFolder = project.directory.path() / "reference";
            plan.sourceFrames = 48000; plan.outputRate = 48000;
            plan.mode = audio::WarpMode::RePitch;
            plan.anchors = {{0, 0}, {48000, 48000}};
            const auto prepared = audio::prepareWarp(plan);
            if (!prepared) throw std::runtime_error("prepare independent distant Warp reference");
            audio::WavReader reader(prepared.prepared->file);
            if (reader.read(reference.data(), blockSize) != blockSize)
                throw std::runtime_error("read independent distant Warp reference");
        }
        project.session.clips()->stallForTest(true);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(width);
        check(playback.setLaunchTick(content.clipStart), "native consumer accepts a launch 64 beats into the project");
        constexpr std::int64_t origin = 13000000;
        auto frame = captured(origin);
        frame.launchMicros += microsForFrames(17);
        Audit audit;
        const auto waiting = render(playback, buffer, frame);
        check(waiting.valid && waiting.waiting && !waiting.started && waiting.preparation &&
              waiting.preparation.reset && waiting.preparation.boundaryTick == content.clipStart &&
              !project.session.transport().playing() && project.session.transport().position() == 0 &&
              project.instrument->ons() == 0 && buffer.silent(0, 64),
              "missing distant Warp pages hold launch and request owner preparation at the exact launch tick");
        check(audit.clean() && project.sqlOnCallback == 0,
              "callback launch readiness never waits, allocates, performs I/O or touches SQL");
        check(playback.prepare(*project.store, waiting.preparation, frame.audibleMicros),
              "owner prepares a new distant candidate even while the previous source worker is stalled");
        frame.audibleMicros = origin + microsForFrames(64);
        frame.launchMicros = frame.audibleMicros + microsForFrames(17);
        const auto start = render(playback, buffer, frame);
        bool exactPcm = true;
        for (int i = 17; i < blockSize; ++i)
            exactPcm = exactPcm &&
                buffer.sample(0, i) == static_cast<double>(reference[static_cast<std::size_t>(i - 17) * 2]) &&
                buffer.sample(1, i) == .125;
        check(start.valid && start.started && !start.waiting && !start.preparation && start.startOffset == 17 &&
              buffer.silent(0, 17) && buffer.sounds(17, 64) &&
              exactPcm &&
              buffer.sample(0, 63) > buffer.sample(0, 17) &&
              project.session.transport().position() == 64 * 24000 + 47,
              "prepared distant launch starts real ramp Warp PCM and MIDI together at frame 17 in both widths");
        check(project.instrument->ons() == 1 && project.instances == 1 && project.unchanged(),
              "distant candidate publication retains the device and starts one authored note without project changes");
    }
}

bool currentClockAgrees(Session& session) {
    const auto block = session.beginRender();
    const auto selected = block.externalTempoBpm();
    return block.valid() && selected && std::abs(block.bpmAtTick(block.tick()) - *selected) < 1e-8;
}

void performedHistoryLoopRepair() {
    for (const auto initial : {120., 240.}) for (const auto begin : {std::int64_t{0}, ppq / 4}) {
        const double tempo = initial == 120 ? 240 : 120;
        Content content; content.loop = std::pair{begin, 3 * ppq / 4};
        Project project(Precision::Float64, initial, content);
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(Precision::Float64);
        std::int64_t boundarySample = 0;
        {
            const auto block = project.session.beginRender();
            boundarySample = block.sampleAtTick(ppq / 2);
        }
        project.session.transport().locate(boundarySample - blockSize);
        project.session.transport().play();
        constexpr std::int64_t origin = 14000000;
        auto frame = captured(origin, true, initial);
        frame.beat = .5 - blockSize * initial / (60 * rate);
        (void)render(playback, buffer, frame);
        frame.audibleMicros += microsForFrames(blockSize); frame.beat = .5; frame.tempo = tempo;
        const auto change = render(playback, buffer, frame);
        check(change.valid && change.waiting && change.preparation && !change.preparation.reset &&
              change.preparation.boundaryTick == ppq / 2 &&
              project.instrument->last.bpm == initial && buffer.sounds(0, blockSize),
              "consumer tempo change at a positive exact boundary explicitly renders the old clock while preparation is pending");
        check(playback.prepare(*project.store, change.preparation, frame.audibleMicros),
              "owner publishes the changed performed tempo before the loop reaches its prior history");
        frame.audibleMicros += microsForFrames(blockSize); frame.beat += blockSize * tempo / (60 * rate);
        bool valid = true, exactRepairs = true, prepared = true, readyAfterOwner = true, sawMismatch = false;
        unsigned wraps = 0, ownerActions = 0;
        Audit audit;
        for (int callback = 0; callback < 2000; ++callback) {
            const bool agrees = currentClockAgrees(project.session);
            const auto before = project.session.transport().position();
            const auto next = render(playback, buffer, frame);
            valid = valid && next.valid && project.session.transport().playing() && buffer.sounds(0, blockSize);
            if (project.session.transport().position() < before) ++wraps;
            if (!agrees) {
                sawMismatch = true;
                exactRepairs = exactRepairs && next.waiting && next.preparation && !next.preparation.reset &&
                    next.preparation.boundaryTick == begin;
            }
            if (next.preparation) {
                ++ownerActions;
                if (ownerActions > 32 || !playback.prepare(*project.store, next.preparation, frame.audibleMicros)) {
                    prepared = false;
                    break;
                }
                // The previous callback may honestly use its old pinned graph.
                // Once owner preparation completes, current and selected BPM
                // must agree; a prior-history segment is never a ready clock.
                readyAfterOwner = readyAfterOwner && currentClockAgrees(project.session);
            } else if (!agrees) {
                readyAfterOwner = false;
            }
            frame.audibleMicros += microsForFrames(blockSize);
            frame.beat += blockSize * tempo / (60 * rate);
            if (wraps >= 4 && currentClockAgrees(project.session)) break;
        }
        check(valid && wraps >= 4 && sawMismatch && exactRepairs,
              "both tempo directions detect old history on repeated loops and repair at exact zero or nonzero loop begin");
        check(prepared && ownerActions > 0 && readyAfterOwner && currentClockAgrees(project.session),
              "bounded owner preparation makes every observed current BPM agree with its selected clock across later complete passes");
        check(audit.clean() && project.instances == 1 && project.unchanged(),
              "performed loop repairs preserve actual native audio, device ownership, realtime safety and authored history");
    }
}

void backwardSeekRepair() {
    for (const auto initial : {120., 240.}) {
        const double tempo = initial == 120 ? 240 : 120;
        Project project(Precision::Float32, initial);
        check(project.session.setExternalTempoAt(*project.store, tempo, ppq / 2),
              "seek fixture contains an earlier tempo segment and a later performed boundary");
        std::int64_t beforeSeek = 0, landing = 0;
        {
            const auto block = project.session.beginRender();
            beforeSeek = block.sampleAtTick(3 * ppq / 4);
            landing = block.sampleAtTick(ppq / 4);
        }
        project.session.transport().locate(beforeSeek);
        project.session.transport().play();
        project.prime();
        auto link = participant();
        LinkPlayback playback(project.session, *link);
        Buffer buffer(Precision::Float32);
        auto frame = captured(18000000, true, tempo); frame.beat = .75;
        (void)render(playback, buffer, frame);
        project.session.transport().locate(landing);
        project.prime(); // Keep this clock-history regression independent of disk seek latency.
        frame.audibleMicros += microsForFrames(blockSize); frame.beat += blockSize * tempo / (60 * rate);
        const auto seek = render(playback, buffer, frame);
        check(seek.valid && seek.waiting && !seek.started && seek.preparation && !seek.preparation.reset &&
              seek.preparation.boundaryTick == ppq / 4 &&
              project.session.transport().position() == landing + blockSize &&
              project.instrument->last.bpm == initial && buffer.sounds(0, blockSize),
              "backward seek reports the stale actual BPM and repairs from the exact landing tick before callback advancement");
        check(playback.prepare(*project.store, seek.preparation, frame.audibleMicros),
              "owner repairs backward-seek history at its captured landing");
        bool ready = currentClockAgrees(project.session);
        for (int callback = 0; callback < 16; ++callback) {
            frame.audibleMicros += microsForFrames(blockSize); frame.beat += blockSize * tempo / (60 * rate);
            const auto next = render(playback, buffer, frame);
            if (next.preparation)
                ready = playback.prepare(*project.store, next.preparation, frame.audibleMicros) && ready;
            ready = ready && next.valid && buffer.sounds(0, blockSize) && currentClockAgrees(project.session);
        }
        check(ready && project.instances == 1 && project.unchanged(),
              "repaired backward seek continues real native audio at the selected clock without changing the authored project");
    }
}

void peers() {
    Project project(Precision::Float32);
    auto local = participant(), remote = participant();
    LinkPlayback playback(project.session, *local);
    Buffer buffer(Precision::Float32);
    check(local->enableStartStopSync(true) && remote->enableStartStopSync(true) &&
          local->enable(true) && remote->enable(true), "explicit --peers opens two actual SDK participants");
    LinkPlayback::Result result;
    LinkFrame other;
    const auto process = [&] {
        const LinkBuffer timing{local->clockMicros(), 4000, 4};
        { audio::CallbackScope callback;
          other = remote->process(timing);
          result = playback.process(buffer.io, timing); }
        if (result.preparation) {
            if (!playback.prepare(*project.store, result.preparation, local->clockMicros()))
                throw std::runtime_error("prepare real peer consumer");
            project.prime();
        }
    };
    const auto poll = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        do { process(); if (predicate()) return true;
             std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    const bool joined = poll([&] { return result.frame.peers == 1 && other.peers == 1; });
    check(joined, "real SDK discovery reaches the actual native playback consumer");
    if (joined) {
        LinkCommand tempo; tempo.tempo = 123;
        { audio::CallbackScope callback; (void)remote->process({remote->clockMicros(), 4000, 4}, tempo); }
        check(poll([&] { return std::abs(result.frame.tempo - 123) < .001 &&
            project.session.spec().externalTempoBpm && std::abs(*project.session.spec().externalTempoBpm - 123) < .001; }),
            "real peer tempo causes owner preparation of the native playback generation");
        LinkCommand play; play.transport = LinkTransport::Play; play.requestBeat = true;
        { audio::CallbackScope callback; (void)remote->process({remote->clockMicros(), 4000, 4}, play); }
        const bool started = poll([&] { return result.started; });
        check(started, "actual remote SDK start reaches the native Session");
        if (started) {
            const auto offset = static_cast<int>(std::clamp<std::int64_t>(std::llround(
                static_cast<double>(result.frame.launchMicros - result.frame.audibleMicros) * rate / 1000000), 0, 64));
            check(result.startOffset == offset && buffer.silent(0, offset) && buffer.sounds(offset, 64),
                  "actual SDK launch timestamp including physical and graph latency matches both native PCM onsets");
        }
        LinkCommand stop; stop.transport = LinkTransport::Stop;
        { audio::CallbackScope callback; (void)remote->process({remote->clockMicros(), 4000, 4}, stop); }
        check(poll([&] { return result.stopped; }) && project.instrument->active == 0,
              "actual remote SDK stop releases the native instrument");
        check(project.instances == 1 && project.unchanged(),
              "real peer performance preserves device ownership and authored project/history");
    }
    check(local->enable(false) && remote->enable(false), "owner closes both explicit SDK participants");
}
} // namespace

int main(int argc, char** argv) {
    try {
        launchStop(); preparation(); latencyBoundary(); continuity(); optOutAndValidation(); drift();
        pendingLaunchUpdates(); restartClearsPerformedHistory(); cancelPendingIntent();
        captureTimeAdmission(); slowOwnerAndLoops(); repeatedLocalPlay(); localPlayCancelsPendingPeerStop();
        distantLaunchReadiness();
        performedHistoryLoopRepair(); backwardSeekRepair();
        if (argc == 2 && std::string_view(argv[1]) == "--peers") peers();
    } catch (const std::exception& error) {
        ++failures; std::printf("EXCEPTION: %s\n", error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
