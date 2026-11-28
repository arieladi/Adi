// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/engine/session.hpp"
#include "adi/gesture_presentation.hpp"
#include "adi/gesture_producers.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace adi::test {
unsigned sceneClockCallbackAllocations() noexcept;
}

namespace {
using namespace adi;
using namespace adi::engine;
namespace g = adi::gesture;

constexpr std::uint32_t rate = 48000;
constexpr std::int32_t blockFrames = 257;
constexpr auto bar = 4 * textproj::kPPQ;
constexpr std::int64_t prepareLead = 396;
struct Clock {
    std::int64_t cut = 96000;
    std::int32_t before = 120, after = 140;
    bool opensAtCut = false;
    [[nodiscard]] std::int64_t prepareAt() const noexcept { return cut - prepareLead; }
    [[nodiscard]] std::int64_t crossingStart() const noexcept { return opensAtCut ? cut : prepareAt() + blockFrames; }
};
constexpr std::size_t mediaFrames = 240000;
constexpr std::int64_t durationNs = 4'000'000'000;
constexpr std::array<std::int64_t,4> clips{101,201,102,202};
constexpr std::array<std::int64_t,4> starts{2400,3600,4800,6000};
constexpr std::array<float,4> levels{0.109375f,0.234375f,-0.125f,-0.25f};
constexpr std::array<std::size_t,4> periods{251,241,257,239};
static_assert(prepareLead > blockFrames && prepareLead < 2 * blockFrames);
static_assert(durationNs / 1'000'000'000 * rate > 3 * ClipPlayback::readAheadFrames(rate));
static_assert(starts[2]+8000 < ClipPlayback::readAheadFrames(rate));
static_assert(starts[3]+6000 < ClipPlayback::readAheadFrames(rate));
static_assert(Clock{}.cut-blockFrames > 3 * ClipPlayback::readAheadFrames(rate));

void require(bool pass, const char* detail) {
    if (!pass) throw std::runtime_error(detail);
}

float signal(std::size_t clip, std::int64_t frame) {
    return levels[clip] + (static_cast<float>(static_cast<std::size_t>(frame) % periods[clip]) - 128.f) / 8192.f;
}

float rightSignal(std::size_t clip, std::int64_t frame) {
    return -0.5f * signal(clip,frame) + 0.03125f;
}

void persistSlot(Store& store, const std::filesystem::path& directory, std::size_t index, bool stereo) {
    const auto id = clips[index];
    const bool legato = index >= 2;
    const auto track = static_cast<std::int64_t>(index % 2 + 1);
    const std::int64_t startNs = starts[index] * 1'000'000'000 / rate;
    const auto name = std::to_string(id) + ".wav";
    const auto path = directory / name;
    const std::uint16_t channels = stereo ? 2 : 1;
    std::vector<float> source(mediaFrames * channels);
    for (std::size_t frame = 0; frame < mediaFrames; ++frame) {
        source[frame * channels] = signal(index,static_cast<std::int64_t>(frame));
        if (stereo) source[frame * channels + 1] = rightSignal(index,static_cast<std::int64_t>(frame));
    }
    {
        audio::WavWriter file(path,rate,channels,audio::WavFormat::Float32);
        file.write(source.data(),static_cast<std::uint32_t>(mediaFrames));
        file.close();
    }
    const auto digest = media::blake3File(path);
    require(static_cast<bool>(digest),"hash the raw Session source");
    auto& db = store.db();
    SQLite::Statement media(db,"INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames)"
                               " VALUES(?,?,?,'wav',48000,?,240000)");
    media.bind(1,id); media.bind(2,digest.hex); media.bind(3,name);
    media.bind(4,static_cast<int>(channels)); media.exec();
    SQLite::Statement where(db,"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled)"
                               " VALUES(?,?,'audio',1,0,?,0)");
    where.bind(1,id); where.bind(2,track); where.bind(3,durationNs); where.exec();
    SQLite::Statement raw(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled)"
                             " VALUES(?,?,240000,0)");
    raw.bind(1,id); raw.bind(2,id); raw.exec();
    SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(?,?,?)");
    pins.bind(1,id); pins.bind(2,startNs); pins.bind(3,startNs+durationNs); pins.exec();
    SQLite::Statement slot(db,"INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(?,?,?)");
    slot.bind(1,track); slot.bind(2,legato ? 20 : 10); slot.bind(3,id); slot.exec();
    SQLite::Statement launch(db,"INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade,launch_quant)"
                                " VALUES(?,?,?,0,?)");
    launch.bind(1,id); launch.bind(2,legato ? 1 : 0); launch.bind(3,legato ? 0 : 1);
    launch.bind(4,legato ? 8 : 1); launch.exec();
}

std::unique_ptr<Store> persistedProject(const test::TempDirectory& folder, Clock clock, bool stereo = false) {
    StoreError error{};
    const auto project = folder.path() / "song.adi";
    auto store = Store::createEmpty(project,error);
    require(static_cast<bool>(store),"create the Session project");
    test::applySessionSchema(store->db());
    store->db().exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Physical Legato Tempo Cut',48000);"
                     "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
                     "(1,'audio','A',0),(2,'audio','B',1),(9,'master','Main',2);"
                     "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled) VALUES(10,0,120,0),(20,1,140,1);");
    {
        SQLite::Statement scene(store->db(),"UPDATE scenes SET tempo_bpm=? WHERE id=20");
        scene.bind(1,clock.after); scene.exec();
        SQLite::Statement tempo(store->db(),"INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,?)");
        tempo.bind(1,clock.before); tempo.exec();
    }
    for (std::size_t index = 0; index < clips.size(); ++index)
        persistSlot(*store,folder.path(),index,stereo);
    store.reset();
    store = Store::open(project,error);
    require(static_cast<bool>(store),"reopen the saved Session scenes and sources");
    return store;
}

void startSource(Store& store, const Session& session, std::size_t index) {
    OpRequest open;
    open.opType = "session.launchClip";
    open.payload = {{"track",index+1}, {"scene",10}, {"clip",clips[index]}, {"incarnation",session.launchIncarnation()}};
    require(OpJournal(store).commit(open).ok,"launch the saved preceding slot");
}

std::array<LaunchTrackStatus,2> trackStates(const Session& session) {
    std::array<LaunchTrackStatus,2> tracks{};
    LaunchStatusHeader header;
    require(session.readLaunchStatus(tracks,header) && header.trackCount == tracks.size() &&
            tracks[0].track == 1 && tracks[1].track == 2,"read both published Session tracks");
    return tracks;
}

struct Playback {
    Session& session;
    std::span<float> pcm;
    Clock clock;
    std::span<float> rightPcm{};
    std::array<LaunchTransition,8> transitions{};
    std::array<LaunchSceneTransition,4> scenes{};
    std::size_t transitionCount = 0, sceneCount = 0;
    std::int64_t first = 0;
    bool crossed = false;
    double openCrossingBpm = 0;
    std::binary_semaphore* pinned = nullptr;
    std::binary_semaphore* released = nullptr;
    std::binary_semaphore* receipt = nullptr;
    std::binary_semaphore* complete = nullptr;

    void through(std::int64_t end) {
        std::array<float,blockFrames> left{}, right{};
        float* out[]{left.data(),right.data()};
        while (first < end) {
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(end-first,blockFrames));
            AudioIo data{out,nullptr,2,0,frames,first};
            bool rendered;
            {
                const audio::CallbackScope callback;
                auto block = session.beginRender();
                if (first == clock.crossingStart()) {
                    openCrossingBpm = block.bpmAtTick(bar);
                    if (pinned) {
                        pinned->release();
                        released->acquire();
                    }
                }
                block.observeLaunchTransitions(this,[](void* opaque,const LaunchTransition& event,std::int64_t) noexcept {
                    auto& self = *static_cast<Playback*>(opaque);
                    if (self.transitionCount < self.transitions.size())
                        self.transitions[self.transitionCount++] = event;
                });
                block.observeSceneLaunches(this,[](void* opaque,const LaunchSceneTransition& scene,std::int64_t) noexcept {
                    auto& self = *static_cast<Playback*>(opaque);
                    if (self.sceneCount < self.scenes.size())
                        self.scenes[self.sceneCount++] = scene;
                    if (self.receipt && self.sceneCount == 1) {
                        self.receipt->release();
                        self.complete->acquire();
                    }
                });
                rendered = block.process(data);
            }
            require(rendered,"render the published Session callback");
            if (first <= clock.cut && first + frames > clock.cut)
                crossed = first == clock.crossingStart() && frames == blockFrames;
            std::copy_n(left.begin(),frames,pcm.begin()+first);
            if (!rightPcm.empty()) std::copy_n(right.begin(),frames,rightPcm.begin()+first);
            first += frames;
            if (first >= clock.prepareAt()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

g::Status attemptStructuralPreview(g::Coordinator& coordinator) {
    g::Spec spec;
    spec.strategy = g::Strategy::Structural;
    spec.label = "saved timing transaction Track Delay preview";
    auto opened = coordinator.begin(spec,g::scalarProducer({g::ScalarKind::TrackDelay,1}));
    if (!opened.status.ok()) return opened.status;
    const auto attempted = opened.session->update(std::make_unique<g::ScalarInput>(21.2));
    (void)opened.session->cancel();
    return attempted.status;
}

bool postOldSlot(Session& session, std::size_t index) {
    LaunchRequest request;
    request.kind = LaunchRequestKind::SlotPress;
    request.incarnation = session.launchIncarnation();
    request.track = static_cast<std::int64_t>(index + 1);
    request.scene = 10;
    request.clip = clips[index];
    return session.postLaunchGesture({&request,1});
}

bool submitScene(Session& session) {
    LaunchRequest request;
    request.kind = LaunchRequestKind::ScenePress;
    request.incarnation = session.launchIncarnation();
    request.scene = 20;
    return session.submitLaunchGesture({&request,1});
}

class TicketClock final : public GraphTransportState {
public:
    explicit TicketClock(std::int64_t sample) : sample_(sample) {}
    [[nodiscard]] const TransportInfo* beginBlock() const noexcept override { return &info_; }
    [[nodiscard]] std::int64_t timedAdoptionSample() const noexcept override { return sample_; }
    [[nodiscard]] bool mayPublishTimedAdoption() const noexcept override { return !receipt.load(); }
    std::atomic<bool> receipt{false};
private:
    std::int64_t sample_;
    TransportInfo info_{};
};

void exerciseTicketBeforeReceipt() {
    GraphHost host;
    rows::Model model;
    rows::Track main;
    main.id = 9;
    main.kind = "master";
    main.name = "Master";
    model.tracks.push_back(main);
    const auto original = std::make_shared<TicketClock>(-1);
    const auto staged = std::make_shared<TicketClock>(96000);
    const auto unrelated = std::make_shared<TicketClock>(-1);
    const auto publish = [&](const auto& timing) { return host.rebuild(model,{},rate,blockFrames,{},timing); };
    require(publish(original),"publish initial native GraphHost timing");
    {
        const auto prior = host.beginRender();
        require(prior.transportState() == original.get(),"announce the original native graph");
    }
    require(publish(staged),"publish the native timed offer");
    std::binary_semaphore committing(0), completed(0);
    bool selected = false, finishSucceeded = false;
    std::thread driver([&] {
        auto block = host.beginRender();
        const auto offer = block.pendingTimedCut();
        selected = block.transportState() == original.get() && offer && block.commitTimedCut(*offer,96000);
        committing.release();
        completed.acquire();
        staged->receipt.store(true);
        finishSucceeded = offer && block.completeTimedCut(*offer);
    });
    committing.acquire();
    const bool beforeReceipt = !staged->receipt.load();
    const bool cancelled = host.cancelTimedCut(*staged);
    const bool moved = publish(unrelated);
    const bool duplicate = publish(staged);
    completed.release();
    driver.join();
    require(selected && beforeReceipt && !cancelled && !moved && !duplicate && finishSucceeded,
            "CAS precedes receipt and blocks competing owner publication and cancellation until the cut completes");
    require(publish(staged),"the same completed timing can publish as an ordinary reuse");
    const auto after = host.beginRender();
    require(after.transportState() == staged.get() && !after.pendingTimedCut(),"reused timing has no second ticket");
    std::printf("Native ticket fence: commit selected, cancel and both publications refused before receipt, one-shot reuse admitted\n");
}

void checkRoundedCallbackClock(Session& session, const Clock& clock, double expected) {
    // Probe the same selected graph's native callback metadata, in a separate
    // driver handle at the exact cut sample. This leaves the persisted raw
    // graph free of test source nodes and avoids calling beginBlock twice in
    // the Scene's rendering slice.
    session.transport().locate(clock.cut);
    const auto block = session.beginRender();
    const auto* state = session.graph().renderedTransportState();
    require(state != nullptr,"pin the real Session transport state at the rounded cut");
    const auto* info = state->beginBlock();
    const auto device = info->devicePosition(info->deviceClockContext,clock.cut);
    std::printf("Rounded callback metadata at %lld: BPM %.0f, device BPM %.0f, expected %.0f\n",
                static_cast<long long>(clock.cut),info->bpm,device.bpm,expected);
    require(info->timelineSample == clock.cut && info->bpm == expected && device.bpm == expected,
            "callback and device clocks agree on the actual rounded cut sample");
}

void exercise(std::string_view mode) {
    const auto initialAllocations = test::sceneClockCallbackAllocations();
    const auto initialFileIo = audio::callbackFileIo.load();
    const bool structural = mode == "structural";
    const bool reuse = mode == "reuse";
    const bool roundedUp = mode == "round-up" || mode == "cold-up";
    const bool roundedDown = mode == "round-down" || mode == "cold-down";
    const bool fractional = roundedUp || roundedDown;
    const bool warm = mode != "cold" && mode != "cold-up" && mode != "cold-down" && !structural;
    const bool alternate = mode == "alternate";
    const bool opening = mode == "opening" || roundedDown;
    const bool fadedTarget = mode == "fade";
    auto clock = alternate ? Clock{115200,100,160} : roundedUp ? Clock{90709,127,143} :
        roundedDown ? Clock{89302,129,143} : Clock{};
    clock.opensAtCut = opening;
    const auto cut = clock.cut;
    const auto prepareAt = clock.prepareAt();
    const bool cancel = mode == "cancel";
    const bool supersede = mode == "supersede";
    const bool locate = mode == "locate";
    const bool competingStop = mode == "stop";
    const std::int64_t a = cut - 8000, bWarm = cut - 6000, bCold = blockFrames;
    const auto b = warm ? bWarm : bCold;
    const auto renderEnd = warm ? cut + 3 * blockFrames : cut + rate/2;
    test::TempDirectory folder{"physical_legato_tempo_cut",warm ? "warm" : "cold"};
    auto store = persistedProject(folder,clock,fractional);
    if (fadedTarget) store->db().exec("UPDATE clip_launch SET clip_fade=1 WHERE clip_id=202");
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(),"prepare the saved raw source pages");
    ProjectView view;
    std::shared_ptr<g::Coordinator> coordinator;
    if (structural) {
        view.refresh(*store);
        auto attached = g::Coordinator::attach(*store,std::make_shared<g::EnginePresentation>(view,session));
        require(attached.status.ok() && attached.value,"attach the real structural gesture coordinator");
        coordinator = std::move(attached.value);
    }
    session.transport().play();
    constexpr std::int64_t secondCut = 192000, secondEnd = secondCut + 3 * blockFrames;
    const bool reuseRounded = fractional && warm;
    const auto capacity = structural ? secondEnd : renderEnd + (reuse || reuseRounded ? blockFrames : 0);
    std::vector<float> pcm(static_cast<std::size_t>(capacity));
    std::vector<float> rightPcm(fractional ? static_cast<std::size_t>(capacity) : 0);
    Playback playback{session,pcm,clock,rightPcm};
    playback.through(std::min(a,b));
    startSource(*store,session,warm ? 0u : 1u);
    playback.through(std::max(a,b));
    startSource(*store,session,warm ? 1u : 0u);
    playback.through(prepareAt);
    const auto old = trackStates(session);
    require(old[0].clip == clips[0] && old[1].clip == clips[1] && old[0].run != 0 && old[1].run != 0 &&
            playback.transitionCount == 2 &&
            playback.transitions[0].sample == std::min(a,b) &&
            playback.transitions[1].sample == std::max(a,b),"both old clips start at the intended samples");

    {
        const auto projected = session.beginRender();
        const auto inverse = projected.tickAtSample(cut);
        require(projected.sampleAtTick(bar) == cut &&
                (roundedUp ? inverse > bar : roundedDown ? inverse < bar : inverse == bar),
                "the selected fixture has the intended inverse rounding at its canonical bar sample");
    }
    const bool staged = session.prepareSceneLaunch(*store,20,bar);
    const auto stageError = session.error();
    const auto checkPreviewRefusal = [&](const char* when) {
        const auto published = session.graph().stats().published;
        const auto status = attemptStructuralPreview(*coordinator);
        std::printf("Structural preview %s: accepted %d, %s\n",when,status.ok(),status.message.c_str());
        require(!status.ok() && status.message.find("resolve the outstanding timed graph cut") != std::string::npos &&
                session.graph().stats().published == published,"a real structural publication cannot replace the outstanding cut");
    };
    if (staged && structural) checkPreviewRefusal("while pending");
    const auto cancelScene = [&] {
        LaunchRequest request;
        request.kind = LaunchRequestKind::CancelScene;
        request.incarnation = session.launchIncarnation();
        request.scene = 20;
        return session.submitLaunchGesture({&request,1});
    };
    if (staged && cancel) require(cancelScene(),"ordinary owner cancel wins before the timed callback");
    if (staged && locate) session.transport().locate(prepareAt - 1);
    if (staged && competingStop) {
        playback.through(clock.crossingStart());
        OpRequest stop;
        stop.opType = "session.stopTrack";
        stop.payload = {{"track",2},{"incarnation",session.launchIncarnation()}};
        require(OpJournal(*store).commit(stop).ok,"queue a real StopTrack onto the old clock's Bar grid");
    }
    const bool commits = mode == "warm" || alternate || reuse || mode == "opening" || reuseRounded;
    if (staged && supersede) {
        playback.through(clock.crossingStart());
        std::binary_semaphore pinned(0), released(0);
        playback.pinned = &pinned;
        playback.released = &released;
        std::exception_ptr driverFailure;
        std::thread driver([&] {
            try { playback.through(renderEnd); }
            catch (...) { driverFailure = std::current_exception(); }
        });
        if (!pinned.try_acquire_for(std::chrono::seconds(5))) {
            released.release();
            driver.join();
            if (driverFailure) std::rethrow_exception(driverFailure);
            throw std::runtime_error("the driver did not pin the crossing callback");
        }
        // The old physical callback is already pinned. An ordinary saved
        // edit replaces the staged Scene before that callback seals its cut.
        bool refreshed = false;
        try {
            store->db().exec("UPDATE tracks SET name='A revised' WHERE id=1");
            refreshed = session.refresh(*store);
        } catch (...) {
            released.release();
            driver.join();
            throw;
        }
        released.release();
        driver.join();
        playback.pinned = playback.released = nullptr;
        if (driverFailure) std::rethrow_exception(driverFailure);
        require(refreshed,"ordinary graph refresh supersedes a pending cut while the old callback is pinned");
    } else if (staged && reuse) {
        playback.through(clock.crossingStart());
        std::binary_semaphore receipt(0), complete(0);
        playback.receipt = &receipt;
        playback.complete = &complete;
        std::exception_ptr driverFailure;
        std::thread driver([&] {
            try { playback.through(renderEnd); }
            catch (...) { driverFailure = std::current_exception(); }
        });
        const bool received = receipt.try_acquire_for(std::chrono::seconds(5));
        bool deferredRefresh = false, deferredDevice = false, exchanged = false;
        try {
            if (received) {
                const auto published = session.graph().stats().published;
                deferredRefresh = !session.refresh(*store);
                deferredDevice = !session.devices().rebuildNow();
                exchanged = session.graph().stats().published != published;
            }
        } catch (...) {
            complete.release();
            driver.join();
            throw;
        }
        complete.release();
        driver.join();
        playback.receipt = playback.complete = nullptr;
        if (driverFailure) std::rethrow_exception(driverFailure);
        require(received && deferredRefresh && deferredDevice && !exchanged,
                "owner refresh and reused timing cannot publish until the driver finishes the accepted cut");
    } else {
        if (opening) playback.through(cut);
        playback.through(renderEnd);
    }
    const auto state = trackStates(session);
    const auto bpm = session.beginRender().bpmAtTick(bar);
    std::int64_t mismatch = -1;
    for (std::int64_t frame = 0; frame < renderEnd; ++frame) {
        const auto left = frame >= a ? signal(commits && frame >= cut ? 2u : 0u,
            frame - a + starts[commits && frame >= cut ? 2u : 0u]) : 0.0f;
        const auto right = frame >= b && (!competingStop || frame < cut) ? signal(commits && frame >= cut ? 3u : 1u,
            frame - b + starts[commits && frame >= cut ? 3u : 1u]) : 0.0f;
        if (pcm[static_cast<std::size_t>(frame)] != left + right) { mismatch = frame; break; }
        if (fractional) {
            const auto clipA = commits && frame >= cut ? 2u : 0u;
            const auto clipB = clipA + 1;
            const auto expectedA = frame >= a ? rightSignal(clipA,frame - a + starts[clipA]) : 0.0f;
            const auto expectedB = frame >= b ? rightSignal(clipB,frame - b + starts[clipB]) : 0.0f;
            if (rightPcm[static_cast<std::size_t>(frame)] != expectedA + expectedB) { mismatch = frame; break; }
        }
    }
    std::printf("Persisted tempo cut %s: staged %d, %zu slot edges, %zu scene receipts, BPM %.0f,"
                " expected cues %lld/%lld, first PCM mismatch %lld, allocs %u, file I/O %u\n",
                mode.data(),staged,playback.transitionCount,playback.sceneCount,bpm,
                static_cast<long long>(starts[2]+cut-a),static_cast<long long>(starts[3]+cut-b),
                static_cast<long long>(mismatch),test::sceneClockCallbackAllocations()-initialAllocations,
                audio::callbackFileIo.load()-initialFileIo);
    if (!staged) std::printf("Owner admission: %s\n",stageError.c_str());
    require(playback.crossed && playback.openCrossingBpm == clock.before,
            "a complete callback starts on the old clock and includes the exact Scene boundary");
    require(staged == !fadedTarget,"stage raw targets only when the timed scope admits them");
    if (commits) {
        require(playback.transitionCount == 4 && playback.transitions[2].track == 1 &&
                playback.transitions[3].track == 2 && playback.transitions[2].newClip == clips[2] &&
                playback.transitions[3].newClip == clips[3] && playback.transitions[2].sample == cut &&
                playback.transitions[3].sample == cut && state[0].clip == clips[2] && state[1].clip == clips[3] &&
                state[0].run != old[0].run && state[1].run != old[1].run,
                "both inherited clips start in order on the same exact sample");
        require(playback.sceneCount == 1 && playback.scenes[0].scene == 20 &&
                playback.scenes[0].sample == cut && playback.scenes[0].tick == bar &&
                playback.scenes[0].tempoBpm == clock.after && bpm == clock.after,
                "the Scene receipt, clock and two clip starts share the boundary");
    } else if (competingStop) {
        const auto& edge = playback.transitions[2];
        require(playback.transitionCount == 3 && playback.sceneCount == 0 && bpm == clock.before &&
                edge.track == 2 && edge.oldClip == old[1].clip && edge.oldRun == old[1].run &&
                edge.newClip == 0 && edge.sample == cut && edge.tick == bar &&
                state[0].clip == old[0].clip && state[0].run == old[0].run && state[1].clip == 0,
                "a competing StopTrack rejects the cut and stops B once on the old Bar while A keeps playing");
    } else {
        require(playback.transitionCount == 2 && playback.sceneCount == 0 && bpm == clock.before &&
                state[0].clip == old[0].clip && state[1].clip == old[1].clip &&
                state[0].run == old[0].run && state[1].run == old[1].run,
                "one rejected transaction leaves both old runs, the tempo and all receipts untouched");
    }
    if (structural) {
        checkPreviewRefusal("after driver rejection");
        bool intercepted = false;
        session.setSourcesFor([&](std::int64_t track) -> std::vector<Node*> {
            if (track == 1) {
                intercepted = true;
                throw std::runtime_error("force one ordinary source preparation refusal");
            }
            return {};
        });
        require(!submitScene(session) && intercepted,"a forced owner rollback build failure is observed by the Scene request");
        session.setSourcesFor({});
        checkPreviewRefusal("after failed rollback preparation");
        require(session.beginRender().bpmAtTick(bar) == 120,"the old driver clock survives failed owner restoration");
        constexpr std::int64_t restartA = 184000, restartB = 186000;
        playback.through(restartA);
        require(postOldSlot(session,0),"restart the first old raw clip without an owner refresh");
        if (!submitScene(session)) throw std::runtime_error("retry with the next valid Scene: " + session.error());
        playback.through(restartB);
        require(postOldSlot(session,1),"restart the second old raw clip without an owner refresh");
        playback.clock.cut = secondCut;
        playback.through(playback.clock.prepareAt());
        playback.through(secondEnd);
        {
            const auto next = session.beginRender();
            require(next.bpmAtTick(bar) == 120 && next.sampleAtTick(2 * bar) == secondCut &&
                    next.bpmAtTick(2 * bar) == 140,"the next scene uses the original clock's next 8q boundary");
        }
        require(playback.sceneCount == 1 && playback.scenes[0].sample == secondCut &&
                playback.scenes[0].tick == 2 * bar && playback.transitionCount == 6 &&
                playback.transitions[4].sample == secondCut && playback.transitions[5].sample == secondCut,
                "the retry emits one Scene receipt and two Starts at 192000");
        for (auto sample = restartB; sample < secondEnd; ++sample) {
            const auto which = sample < secondCut ? 0u : 2u;
            const auto expected = signal(which,sample - restartA + starts[which]) +
                signal(which + 1,sample - restartB + starts[which + 1]);
            require(pcm[static_cast<std::size_t>(sample)] == expected,"retry preserves exact old and new raw source phases");
        }
        std::printf("Structural retry: one Scene receipt at %lld, tick %lld, old first bar 120 BPM\n",
            static_cast<long long>(playback.scenes[0].sample),static_cast<long long>(playback.scenes[0].tick));
    } else if (reuse) {
        require(session.devices().rebuildNow(),"ordinary DeviceHost may reuse completed timing without arming another cut");
        playback.through(capacity);
        require(playback.sceneCount == 1 && playback.transitionCount == 4 && session.beginRender().bpmAtTick(bar) == 140,
                "the reused timing produces no duplicate receipt, no second cut and no tempo rollback");
        require(session.refresh(*store) && session.beginRender().bpmAtTick(bar) == 140,
                "ordinary refresh preserves the previously performed tempo after a device rebuild");
        for (auto sample = renderEnd; sample < capacity; ++sample) {
            const auto expected = signal(2,sample-a+starts[2]) + signal(3,sample-b+starts[3]);
            require(pcm[static_cast<std::size_t>(sample)] == expected,"post-receipt rebuild keeps exact PCM continuity");
        }
        std::printf("Receipt fence: refresh and device rebuild held through the suffix, completed timing rebuilt once at 140 BPM\n");
    } else if (reuseRounded) {
        require(session.devices().rebuildNow(),"the rounded cut's completed timing can publish as an ordinary rebuild");
        playback.through(capacity);
        require(playback.sceneCount == 1 && playback.transitionCount == 4,
                "the rounded cut retains its one-shot receipt after a DeviceHost rebuild");
        for (auto sample = renderEnd; sample < capacity; ++sample) {
            const auto at = static_cast<std::size_t>(sample);
            const auto frameA = sample - a + starts[2];
            const auto frameB = sample - b + starts[3];
            require(pcm[at] == signal(2,frameA) + signal(3,frameB) &&
                    rightPcm[at] == rightSignal(2,frameA) + rightSignal(3,frameB),
                    "the rounded cut keeps exact stereo PCM across its one-shot rebuild");
        }
    } else if (!commits) {
        require(session.refresh(*store),"ordinary owner refresh reconciles a failed or superseded cut");
        require(session.setExternalTempoAt(*store,125,6 * textproj::kPPQ),
                "the next ordinary tempo publication is no longer held by the failed scene");
        const auto control = session.beginRender();
        const auto expectedQuarterFive = static_cast<std::int64_t>(std::llround(5.0 * 60 * rate / clock.before));
        require(control.bpmAtTick(bar) == clock.before &&
                control.sampleAtTick(5 * textproj::kPPQ) == expectedQuarterFive && control.bpmAtTick(6 * textproj::kPPQ) == 125,
                "a new publication has the original prefix and its own tempo boundary");
    }
    require(mismatch == -1,"both source phases match the exact raw PCM across the boundary");
    if (fractional) checkRoundedCallbackClock(session,clock,commits ? clock.after : clock.before);
    require(store->db().execAndGet("SELECT bpm FROM tempo_map WHERE pos_ticks=0").getDouble() == clock.before &&
            session.problems().empty() && test::sceneClockCallbackAllocations() == initialAllocations &&
            audio::callbackFileIo.load() == initialFileIo,
            "the cut leaves saved tempo intact with no callback allocation or file reads");
}
void exerciseIdleSavedStart() {
    const Clock clock;
    constexpr auto sourceA = Clock{}.cut - 8000;
    constexpr auto end = Clock{}.cut + 3 * blockFrames;
    test::TempDirectory folder{"physical_legato_tempo_cut","idle"};
    auto store = persistedProject(folder,clock);
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(end));
    Playback playback{session,pcm,clock};
    const auto initialAllocations = test::sceneClockCallbackAllocations();
    const auto initialFileIo = audio::callbackFileIo.load();
    playback.through(sourceA);
    startSource(*store,session,0);
    playback.through(clock.prepareAt());
    const auto old = trackStates(session);
    require(old[0].clip == clips[0] && old[0].run != 0 && old[1].clip == 0 && old[1].run == 0,
            "A is running while B has no outgoing Session phase");
    require(session.prepareSceneLaunch(*store,20,bar),"stage the warm saved-Start cut");
    playback.through(end);
    const auto selected = trackStates(session);
    require(playback.crossed && playback.openCrossingBpm == clock.before &&
            playback.transitionCount == 3 && playback.sceneCount == 1 &&
            playback.scenes[0].sample == clock.cut && session.beginRender().bpmAtTick(bar) == clock.after &&
            playback.transitions[1].sample == clock.cut && playback.transitions[1].oldClip == clips[0] &&
            playback.transitions[1].newClip == clips[2] && selected[0].clip == clips[2] &&
            playback.transitions[2].sample == clock.cut && playback.transitions[2].oldClip == 0 &&
            playback.transitions[2].newClip == clips[3] && selected[1].clip == clips[3],
            "the inherited A and idle B starts share the selected cut and clock");
    for (auto sample = sourceA; sample < end; ++sample) {
        const bool switched = sample >= clock.cut;
        const auto clipA = switched ? 2u : 0u;
        const auto expected = signal(clipA,sample - sourceA + starts[clipA]) +
            (switched ? signal(3,sample - clock.cut + starts[3]) : 0.0f);
        require(pcm[static_cast<std::size_t>(sample)] == expected,
                "idle B uses exact saved Start with inherited A PCM");
    }
    require(test::sceneClockCallbackAllocations() == initialAllocations && audio::callbackFileIo.load() == initialFileIo,
            "saved-Start cut uses no driver allocation or file reads");
    std::printf("Idle raw Scene track starts at saved frame %lld, inherited track and clock switch at %lld\n",
                static_cast<long long>(starts[3]),static_cast<long long>(clock.cut));
}

void exerciseExpiredRenderHandle() {
    const Clock clock;
    test::TempDirectory folder{"physical_legato_tempo_cut","expired_handle"};
    auto store = persistedProject(folder,clock);
    Session session;
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(clock.cut));
    Playback driver{session,pcm,clock};
    driver.through(88000);
    startSource(*store,session,0);
    driver.through(90000);
    startSource(*store,session,1);
    driver.through(clock.prepareAt());
    require(session.prepareSceneLaunch(*store,20,bar),"stage a warm cut for callback expiry");
    driver.through(clock.crossingStart());

    struct Observer {
        Session& session;
        int count = 0;
        std::int64_t sample = -1;
    } observer{session};
    std::array<float,blockFrames> left{}, right{};
    float* outputs[]{left.data(),right.data()};
    AudioIo io{outputs,nullptr,2,0,blockFrames,clock.crossingStart()};
    bool rendered = false;
    {
        auto selected = session.beginRender();
        selected.observeSceneLaunches(&observer,[](void* state,const LaunchSceneTransition& edge,std::int64_t) noexcept {
            auto& context = *static_cast<Observer*>(state);
            ++context.count;
            context.sample = edge.sample;
            // A later driver handle expires the first, even when both select
            // the same publication. Deliberately do that after the receipt.
            const auto replacement = context.session.beginRender();
            (void)replacement;
        });
        rendered = selected.process(io);
    }
    require(!rendered && observer.count == 1 && observer.sample == clock.cut,
            "fault injection expires the accepted suffix handle after its one receipt");
    const bool rebuilt = session.devices().rebuildNow();
    std::printf("Expired driver handle after accepted cut: rendered %d, one receipt, device rebuild %d\n",rendered,rebuilt);
    require(rebuilt && session.refresh(*store),"an expired accepted slice does not strand ordinary owner publication");
    io.streamTimeSamples += io.frames;
    {
        auto following = session.beginRender();
        following.observeSceneLaunches(&observer,[](void* state,const LaunchSceneTransition&,std::int64_t) noexcept {
            ++static_cast<Observer*>(state)->count;
        });
        require(following.bpmAtTick(bar) == 140 && following.process(io) && observer.count == 1,
                "the next valid render preserves the selected tempo without another Scene receipt");
    }
}
}  // namespace

namespace adi::test {
void runTimedRawSceneTests() {
    for (const auto mode : {"warm","opening","cold","stop","cancel","supersede","locate","alternate",
                            "round-up","round-down","cold-up","cold-down","fade","structural","reuse"})
        exercise(mode);
    exerciseTicketBeforeReceipt();
    exerciseIdleSavedStart();
    exerciseExpiredRenderHandle();
}
} // namespace adi::test
