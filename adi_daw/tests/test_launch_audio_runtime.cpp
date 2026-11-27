// SPDX-License-Identifier: AGPL-3.0-or-later
#define ADI_TEST_COUNT_REPLACED_DELETE
#include "replace_operator_new.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
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
