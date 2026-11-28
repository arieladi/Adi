// SPDX-License-Identifier: AGPL-3.0-or-later
#define ADI_TEST_COUNT_REPLACED_DELETE
#include "replace_operator_new.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/blob.hpp"
#include "adi/engine/graph.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/transport.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
std::atomic<unsigned> allocations{0}, releases{0};
}
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread) ++allocations;
}
void adi::test::countReplacedDelete(void* pointer) noexcept {
    if (pointer && adi::audio::onAudioThread) ++releases;
}

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t rate = 48'000;
constexpr std::uint32_t bar = 96'000;
constexpr std::int64_t trackOne = 1, trackTwo = 2, sceneOne = 10, sceneTwo = 20;
constexpr std::uint64_t incarnation = 7;
constexpr std::int32_t blockFrames = 257;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    test::TempDirectory folder{"launch_audio_runtime", "project"};
    std::unique_ptr<Store> store;
    std::shared_ptr<const LaunchAudioProject> sceneAudio;

    explicit Fixture(LaunchQuantization global = LaunchQuantization::Bar, bool fade = false) {
        StoreError error{};
        store = Store::createEmpty(folder.path() / "song.adi", error);
        require(static_cast<bool>(store), "create launch render store");
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','Sampler',0),(2,'audio','Empty Arrangement',1),(9,'master','Main',2);"
            "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(1,'arrangement','arrangement.wav','wav');"
            "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(10,1,'audio',1,0,4000000000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(10,1,0,192000);");
        write("arrangement.wav", 0.125f, 2 * bar);
        write("scene_one_track_one.wav", 0.25f, 2 * bar);
        write("scene_one_track_two.wav", 0.125f, 2 * bar);
        write("scene_two_track_one.wav", -0.25f, 2 * bar);
        std::vector<LaunchAudioClip> pcm;
        constexpr std::uint64_t budget = 4 * bar * sizeof(float);
        pcm.push_back(LaunchAudioClip::read(101, 0, folder.path() / "scene_one_track_one.wav", rate, budget));
        pcm.push_back(LaunchAudioClip::read(102, 0, folder.path() / "scene_one_track_two.wav", rate, budget));
        pcm.push_back(LaunchAudioClip::read(103, 0, folder.path() / "scene_two_track_one.wav", rate, budget));
        std::vector<LaunchSlot> slots;
        slots.push_back(slot(trackOne, sceneOne, 101));
        slots.push_back(slot(trackOne, sceneTwo, 103));
        slots.push_back(slot(trackTwo, sceneOne, 102));
        for (auto& entry : slots)
            entry.fade = fade;
        sceneAudio = std::make_shared<const LaunchAudioProject>(incarnation, 120,
            std::vector<std::int64_t>{trackOne, trackTwo}, std::move(slots),
            std::vector<std::int64_t>{sceneOne, sceneTwo}, std::move(pcm), global);
    }

    void write(const char* name, float sample, std::uint32_t frames) {
        const std::vector<float> signal(frames, sample);
        audio::WavWriter wav(folder.path() / name, rate, 1, audio::WavFormat::Float32);
        wav.write(signal.data(), frames);
        wav.close();
    }

    static LaunchSlot slot(std::int64_t track, std::int64_t scene, std::int64_t clip) {
        LaunchSlot slot;
        slot.track = track;
        slot.scene = scene;
        slot.clip = clip;
        slot.endTick = 2 * textproj::kWhole;
        return slot;
    }

    static LaunchRequest scene(std::int64_t id) {
        LaunchRequest request;
        request.kind = LaunchRequestKind::ScenePress;
        request.scene = id;
        request.incarnation = incarnation;
        return request;
    }

    void load(Session& session, Precision width, bool performance) {
        SessionSpec spec;
        spec.sampleRate = rate;
        spec.maxFrames = blockFrames;
        spec.precision = width;
        spec.performanceState = performance;
        spec.offline = !performance;
        spec.exporting = !performance;
        spec.synchronousClipReads = true;
        spec.launchAudio = sceneAudio;
        session.graph().setFadeFrames(0);
        if (!session.load(*store, {}, spec)) throw std::runtime_error(session.error());
    }
};

struct Render {
    struct Edge { LaunchTransition transition; std::int64_t stream; };
    Session& session;
    Precision width;
    std::int64_t stream = 0;
    std::array<float, 2 * blockFrames> pcm{};
    std::array<double, 2 * blockFrames> wide{};
    std::array<Edge, 8> edges{};
    std::size_t edgeCount = 0;

    template <typename Expected>
    void block(std::int32_t size, Expected expected, double tolerance = 0.0) {
        require(session.clips()->prime(), "prime the ordinary disk clip");
        float* out[]{pcm.data(), pcm.data() + blockFrames};
        double* out64[]{wide.data(), wide.data() + blockFrames};
        AudioIo io;
        io.frames = size;
        io.numOut = 2;
        io.streamTimeSamples = stream;
        if (width == Precision::Float64) io.out64 = out64;
        else io.out = out;
        bool rendered;
        {
            auto driver = session.beginRender();
            driver.observeLaunchTransitions(this, [](void* context, const LaunchTransition& edge,
                                                     std::int64_t streamSample) noexcept {
                auto& render = *static_cast<Render*>(context);
                if (render.edgeCount < render.edges.size())
                    render.edges[render.edgeCount++] = {edge, streamSample};
            });
            rendered = driver.process(io);
        }
        require(rendered, "render Session launch audio through its pinned graph");
        for (std::int32_t frame = 0; frame < size; ++frame) {
            const auto value = expected(stream + frame);
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto index = channel * blockFrames + static_cast<std::size_t>(frame);
                const auto received = width == Precision::Float64 ? wide[index] : pcm[index];
                if (std::abs(received - value) > tolerance) {
                    std::printf("frame %lld channel %zu received %.9g wanted %.9g\n",
                        static_cast<long long>(stream + frame), channel, received, value);
                    throw std::runtime_error("Session output differed");
                }
            }
        }
        stream += size;
    }
};

void live(Fixture& fixture, Precision width) {
    Session session;
    fixture.load(session, width, true);
    Render render{session, width};
    require(session.postLaunchGesture(std::array{Fixture::scene(sceneOne)}), "queue first scene");
    render.block(41, [](auto) { return 0.0; });
    require(!session.transport().playing(), "driver owns the transport start");
    session.transport().play();
    render.block(137, [](auto) { return 0.375; });
    require(session.setDelayCompensation(false), "rebuild the graph during a launched scene");
    render.block(81, [](auto) { return 0.375; });
    require(session.postLaunchGesture(std::array{Fixture::scene(sceneTwo)}), "queue second scene");
    while (render.stream < 41 + bar + blockFrames) {
        render.block(blockFrames, [](auto frame) { return frame < 41 + bar ? 0.375 : -0.25; });
    }
    require(render.edgeCount == 4 && render.edges[0].stream == 41 && render.edges[1].stream == 41 &&
        render.edges[2].stream == 41 + bar && render.edges[3].stream == 41 + bar &&
        render.edges[2].transition.sample == bar && render.edges[3].transition.sample == bar &&
        render.edges[2].transition.newClip == 103 && render.edges[3].transition.newClip == 0 &&
        !render.edges[3].transition.arrangement, "observer stamps the performed scene and Stop-button edges");
    LaunchRequest back;
    back.kind = LaunchRequestKind::BackToArrangement;
    back.incarnation = incarnation;
    require(session.postLaunchGesture(std::array{back}), "queue Back to Arrangement");
    render.block(193, [](auto) { return 0.125; });
}

void isolated(Fixture& fixture) {
    Session session;
    fixture.load(session, Precision::Float32, false);
    require(!session.postLaunchGesture(std::array{Fixture::scene(sceneOne)}), "export has no launch lane");
    session.transport().play();
    Render render{session, Precision::Float32};
    render.block(blockFrames, [](auto) { return 0.125; });
}

void unquantized() {
    Fixture fixture(LaunchQuantization::None);
    Session session;
    fixture.load(session, Precision::Float32, true);
    Render render{session, Precision::Float32};
    session.transport().play();
    render.block(19, [](auto) { return 0.125; });
    require(session.postLaunchGesture(std::array{Fixture::scene(sceneOne)}), "queue unquantized scene");
    render.block(37, [](auto) { return 0.375; });
    require(render.edgeCount == 2 && render.edges[0].stream == 19 && render.edges[1].stream == 19,
            "Global None permits immediate two-track switch off the bar grid");
}

void individualTrackHandover() {
    Fixture fixture(LaunchQuantization::None);
    Session session;
    fixture.load(session, Precision::Float32, true);
    Render render{session, Precision::Float32};
    session.transport().play();
    render.block(19, [](auto) { return 0.125; });
    LaunchRequest launch;
    launch.kind = LaunchRequestKind::SlotPress;
    launch.track = trackTwo;
    launch.scene = sceneOne;
    launch.clip = 102;
    launch.incarnation = incarnation;
    require(session.postLaunchGesture(std::array{launch}), "queue the other track's slot");
    render.block(31, [](auto) { return 0.25; });
    require(render.edgeCount == 1 && render.edges[0].transition.track == trackTwo &&
            render.edges[0].stream == 19, "only the target track gives up Arrangement");
    launch.kind = LaunchRequestKind::StopTrack;
    launch.scene.reset();
    launch.clip.reset();
    require(session.postLaunchGesture(std::array{launch}), "stop only the launched track");
    render.block(29, [](auto) { return 0.125; });
}

double fadeAt(std::int64_t frame) {
    const auto position = static_cast<double>(std::clamp<std::int64_t>(frame, 0, 192)) / 192.0;
    return position * position * (3.0 - 2.0 * position);
}

void fadeThroughHandover() {
    Fixture fixture(LaunchQuantization::None, true);
    Session session;
    fixture.load(session, Precision::Float64, true);
    Render render{session, Precision::Float64};
    session.transport().play();
    require(session.postLaunchGesture(std::array{Fixture::scene(sceneOne)}), "queue a faded scene");
    render.block(213, [](auto frame) { return 0.375 * fadeAt(frame); }, 1e-8);
    require(session.postLaunchGesture(std::array{Fixture::scene(sceneTwo)}), "queue faded replacement");
    const auto swap = render.stream;
    const auto crossfade = [swap](auto frame) {
        const auto gain = fadeAt(frame - swap);
        return 0.375 * (1.0 - gain) - 0.25 * gain;
    };
    render.block(73, crossfade, 1e-8);
    require(session.setDelayCompensation(false), "publish a graph in the outgoing ramp");
    render.block(157, crossfade, 1e-8);
    LaunchRequest stop;
    stop.kind = LaunchRequestKind::StopTrack;
    stop.track = trackOne;
    stop.incarnation = incarnation;
    require(session.postLaunchGesture(std::array{stop}), "stop the faded replacement");
    const auto end = render.stream;
    render.block(197, [end](auto frame) { return -0.25 * (1.0 - fadeAt(frame - end)); }, 1e-8);
}

struct StopInstrument final : device::DeviceInstance {
    device::DeviceIdentity info;
    std::int64_t callbackStart = 0, releasedAt = -1;
    std::uint64_t voice = 0;
    unsigned starts = 0, releases = 0;
    bool held = false, matchedRelease = false;

    const device::DeviceIdentity& identity() const noexcept override { return info; }
    bool loaded() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }

    void process(const NodeIo& io) noexcept override {
        auto event = io.events.begin();
        const auto end = io.events.end();
        for (auto frame = io.blockOffset; frame < io.blockOffset + io.frames; ++frame) {
            while (event != end && event->frame <= frame) {
                if (event->type == EventType::NoteOn) {
                    ++starts;
                    held = true;
                    voice = event->noteId;
                } else if (event->type == EventType::NoteOff) {
                    ++releases;
                    releasedAt = callbackStart + frame;
                    matchedRelease = held && event->noteId == voice;
                    held = false;
                }
                ++event;
            }
            for (std::int32_t channel = 0; channel < io.channels; ++channel)
                io.out[channel][frame] = held ? 0.5f : 0.0f;
        }
    }
};

void auditedStops() {
    constexpr std::int64_t midiTrack = 3;
    test::TempDirectory folder{"audited_launch_stops", "project"};
    StoreError error{};
    auto store = Store::createEmpty(folder.path() / "stops.adi", error);
    require(static_cast<bool>(store), "create persisted stop playback fixture");
    test::applySessionSchema(store->db());
    const auto wave = [&](const char* name, float value) {
        const std::vector<float> signal(3 * bar, value);
        audio::WavWriter wav(folder.path() / name, rate, 1, audio::WavFormat::Float32);
        wav.write(signal.data(), 3 * bar);
        wav.close();
    };
    wave("first.wav", 0.25f);
    wave("second.wav", 0.125f);
    wave("arrangement.wav", 0.0625f);
    store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
        "(1,'audio','First audio',0),(2,'audio','Second audio',1),(3,'midi','MIDI',2),(9,'master','Main',3);"
        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
        "INSERT INTO scenes(id,ord,name) VALUES(10,0,'Three clips');"
        "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES"
        "(1,'first','first.wav','wav'),(2,'second','second.wav','wav'),(3,'arrangement','arrangement.wav','wav');"
        "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks) VALUES"
        "(101,1,'audio',0,69189120),(102,2,'audio',0,69189120),(103,3,'midi',0,69189120),"
        "(104,1,'audio',0,69189120);"
        "INSERT INTO audio_clips(clip_id,media_id,src_len_frames) VALUES(101,1,288000),(102,2,288000),(104,3,288000);"
        "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(1,10,101),(2,10,102),(3,10,103);"
        "INSERT INTO clip_launch(clip_id,clip_fade,ram_mode) VALUES(101,0,1),(102,0,1);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','test.stop','Held note probe','instrument');"
        "INSERT INTO device_chains(id,track_id) VALUES(1,3);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(1,1,0,1,'Held note probe');");
    NoteRecord held{};
    held.dur_ticks = 3 * textproj::kWhole;
    held.note_id = 1;
    held.key = 64;
    held.vel_on = 100;
    held.vel_off = 40;
    held.probability = 10000;
    const auto notes = writeStream<NoteRecord>(FourCC::Notes, std::array{held});
    require(store->putEventStream(103, "notes", notes), "store a MIDI note whose natural end is beyond both stops");

    Session session;
    session.graph().setFadeFrames(0);
    StopInstrument* probe = nullptr;
    const auto load = [&](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        auto instrument = std::make_unique<StopInstrument>();
        probe = instrument.get();
        return instrument;
    };
    SessionSpec spec{2, rate, blockFrames};
    spec.synchronousClipReads = true;
    if (!session.load(*store, load, spec)) throw std::runtime_error(session.error());
    const auto stamp = session.launchIncarnation();
    require(stamp != 0 && probe, "native persisted launch and MIDI destination are available");
    const auto audit = [&](const char* operation, Payload payload) {
        OpRequest request;
        request.opType = operation;
        request.payload = std::move(payload);
        require(OpJournal(*store).commit(request).ok,
            "commit a live Session operation through the audit journal");
    };
    audit("session.launchScene", {{"scene", sceneOne}, {"incarnation", stamp}});
    Render render{session, Precision::Float32};
    const auto until = [&](std::int64_t end) {
        while (render.stream <= end) {
            probe->callbackStart = render.stream;
            render.block(blockFrames, [](auto frame) {
                return frame < bar ? 0.875 : frame < 2 * bar ? 0.625 : 0.0;
            });
        }
    };
    until(0);
    audit("session.stopTrack", {{"track", trackOne}, {"incarnation", stamp}});
    audit("session.stopAll", {{"incarnation", stamp + 1}});
    until(bar);
    require(render.edgeCount == 4 && render.edges[3].stream == bar &&
        render.edges[3].transition.track == trackOne && render.edges[3].transition.newClip == 0 &&
        !render.edges[3].transition.arrangement && probe->held && probe->releases == 0,
        "StopTrack mutes its Arrangement source on the Global bar and leaves other clips playing");
    audit("session.stopAll", {{"incarnation", stamp}});
    until(2 * bar + 2 * blockFrames);
    require(render.edgeCount == 6 && render.edges[4].stream == 2 * bar && render.edges[5].stream == 2 * bar &&
        render.edges[4].transition.track == trackTwo && render.edges[5].transition.track == midiTrack &&
        render.edges[4].transition.newClip == 0 && render.edges[5].transition.newClip == 0,
        "StopAll emits one exact stop for each remaining Session source");
    require(probe->starts == 1 && probe->releases == 1 && probe->releasedAt == 2 * bar &&
        probe->matchedRelease && !probe->held && session.transport().playing(),
        "StopAll releases the held MIDI identity at the bar with transport still rolling");
}

void rejectsDuplicateSlotClip(const Fixture& fixture) {
    const auto view = fixture.sceneAudio->view();
    std::vector<LaunchSlot> duplicate(view.slots.begin(), view.slots.end());
    duplicate[1].clip = duplicate[0].clip;
    std::vector<LaunchAudioClip> pcm;
    pcm.push_back(*fixture.sceneAudio->clip(101));
    pcm.push_back(*fixture.sceneAudio->clip(102));
    try {
        const LaunchAudioProject rejected(incarnation, 120, std::vector{trackOne, trackTwo},
            std::move(duplicate), std::vector{sceneOne, sceneTwo}, std::move(pcm));
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("one occupied clip ID cannot be admitted in two slots");
}
constexpr auto ppq = textproj::kPPQ;
constexpr std::int64_t clockBankCut = 48'000;
constexpr std::int32_t bankBlockFrames = 256;
constexpr float oldBankValue(std::int64_t frame) { return 0.10f + static_cast<float>(frame) / 10000.0f; }
constexpr float newBankValue(std::int64_t frame) { return 0.40f + static_cast<float>(frame) / 10000.0f; }

struct ClockBankFixture {
    std::shared_ptr<const LaunchAudioProject> oldBank, newBank;
    LaunchAudioRuntime runtime;
    LaunchAudioDriver driver{1};
    Transport transport;
    std::array<float, bankBlockFrames> output{};
    std::array<LaunchTrackStatus, 1> status{};
    LaunchStatusHeader header;
    std::array<LaunchTransition, 4> edges{};
    std::size_t edgeCount = 0;

    static std::shared_ptr<const LaunchAudioProject> makeBank(bool newer, bool fade) {
        std::vector<float> pcm(1040);
        for (std::size_t index = 0; index < pcm.size(); ++index)
            pcm[index] = newer ? newBankValue(static_cast<std::int64_t>(index)) : oldBankValue(static_cast<std::int64_t>(index));
        std::vector<LaunchAudioClip> clips;
        clips.push_back(LaunchAudioClip::fromSamples(100, 48000, 1, std::move(pcm)));
        std::vector<LaunchSlot> slots{{1, 10, 100, 0, ppq, 0, 0, LaunchMode::Trigger, {}, 0.0f, false, fade}};
        return std::make_shared<LaunchAudioProject>(7, newer ? 120.0 : 60.0, std::vector<std::int64_t>{1},
            std::move(slots), std::vector<std::int64_t>{10}, std::move(clips), LaunchQuantization::None);
    }

    explicit ClockBankFixture(bool fade = false)
        : oldBank(makeBank(false, fade)), newBank(makeBank(true, fade)), runtime(newBank, {1}, bankBlockFrames, oldBank, clockBankCut) {
        transport.play();
    }

    void start() {
        const std::array requests{LaunchRequest{LaunchRequestKind::SlotPress, 1, 10, 100, 7}};
        require(driver.post(requests), "queue exact slot for the old bank");
    }

    void render(std::int32_t frames) {
        LaunchBlock block;
        block.frames = frames;
        block.global = LaunchQuantization::None;
        block.tickAtSample = [](const void*, std::int64_t at) noexcept {
            return at < clockBankCut ? at * ppq / clockBankCut : ppq + (at - clockBankCut) * ppq / (clockBankCut / 2);
        };
        block.sampleAtTick = [](const void*, std::int64_t at) noexcept {
            return at < ppq ? (at * clockBankCut + ppq / 2) / ppq : clockBankCut + ((at - ppq) * (clockBankCut / 2) + ppq / 2) / ppq;
        };
        output.fill(0.0f);
        float* channels[]{output.data()};
        NodeIo io{};
        io.out = channels;
        io.frames = frames;
        io.channels = 1;
        {
            audio::CallbackScope callback;
            runtime.begin(driver, block, transport, 0, this,
                [](void* context, const LaunchTransition& edge, std::int64_t) noexcept {
                    auto& self = *static_cast<ClockBankFixture*>(context);
                    if (self.edgeCount < self.edges.size()) self.edges[self.edgeCount++] = edge;
                });
            runtime.overlay(1, io);
            runtime.finish();
            transport.advance(frames);
        }
        require(driver.readStatus(status, header), "driver completed one block");
    }
};

bool frameMatches(float actual, float expected) { return std::abs(actual - expected) <= 1e-7f; }

void crossBankSeek() {
    ClockBankFixture fixture;
    fixture.start();
    fixture.render(128);
    const auto initial = fixture.status[0];
    require(frameMatches(fixture.output[127], oldBankValue(127)), "old bank reads source frames before seek");
    fixture.transport.locate(clockBankCut + 180);
    fixture.render(64);
    require(frameMatches(fixture.output[0], newBankValue(64)) && frameMatches(fixture.output[63], newBankValue(127)),
        "forward seek reprojects the live phase to the faster bank on its first sample");
    require(fixture.edgeCount == 1 && fixture.status[0].run == initial.run && fixture.status[0].clip == initial.clip,
        "cross-bank seek retains the active launch run and produces no spurious launch receipt");
    fixture.transport.locate(clockBankCut - 1024);
    fixture.render(64);
    require(frameMatches(fixture.output[0], oldBankValue(256)) && frameMatches(fixture.output[63], oldBankValue(319)),
        "backward seek restores the slower bank at the same musical phase");
    fixture.render(32);
    require(frameMatches(fixture.output[0], oldBankValue(320)), "PCM rebase persists in the continuing run across callbacks");
    fixture.transport.locate(clockBankCut - 400);
    fixture.render(17);
    require(frameMatches(fixture.output[0], oldBankValue(352)) && fixture.status[0].run == initial.run,
        "seek within a fixed prepared bank continues at the exact source frame and retains its run");
}

void renderAcrossPriorStep() {
    ClockBankFixture fixture;
    fixture.transport.locate(clockBankCut - 64);
    fixture.start();
    fixture.render(128);
    require(frameMatches(fixture.output[63], oldBankValue(63)) && frameMatches(fixture.output[64], newBankValue(32)) &&
        frameMatches(fixture.output[127], newBankValue(95)), "prior clock step rebases a surviving run exactly within one callback");
}

void fadeOldBankOnSameSampleStop() {
    ClockBankFixture fixture(true);
    fixture.start();
    fixture.render(256);
    fixture.transport.locate(clockBankCut + 180);
    const std::array stops{LaunchRequest{LaunchRequestKind::StopTrack, 1, {}, {}, 7}};
    require(fixture.driver.post(stops), "queue a coincident stop while locating");
    fixture.render(32);
    require(fixture.edgeCount == 2 && fixture.edges[1].sample == clockBankCut + 180 && fixture.status[0].run == 0,
        "coincident stop wins without retaining or relaunching the run");
    require(frameMatches(fixture.output[0], oldBankValue(256)), "fade at the stop starts from the last playing bank and its original source frame");
}
} // namespace

int main() {
    try {
        Fixture fixture;
        const auto io = audio::callbackFileIo.load();
        live(fixture, Precision::Float32);
        live(fixture, Precision::Float64);
        isolated(fixture);
        unquantized();
        individualTrackHandover();
        fadeThroughHandover();
        crossBankSeek();
        renderAcrossPriorStep();
        fadeOldBankOnSameSampleStop();
        auditedStops();
        rejectsDuplicateSlotClip(fixture);
        require(allocations == 0 && releases == 0 && audio::callbackFileIo == io,
                "Session launch callbacks allocate, release and read no files");
        std::printf("PASS -- Session switches both graph sources at frame %u in both widths, "
                    "0 callback allocations/releases/file calls\n", bar);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
