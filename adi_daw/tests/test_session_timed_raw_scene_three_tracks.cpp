// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/launch_gain_edits.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/session.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace adi::test {
unsigned sceneClockCallbackAllocations() noexcept;
}

namespace {
using namespace adi;
using namespace adi::engine;

constexpr std::uint32_t rate = 48'000;
constexpr std::int32_t blockFrames = 257;
constexpr std::int64_t cut = 96'000, prepareAt = cut - 396, crossing = prepareAt + blockFrames;
constexpr auto continuedEndSample = cut + 37;
constexpr auto bar = 4 * textproj::kPPQ;
constexpr std::uint32_t mediaFrames = 240'000;
constexpr std::int64_t durationNs = 4'000'000'000;
constexpr std::array<std::int64_t,6> clips{101,102,103,201,202,203};
constexpr std::array<std::int64_t,6> starts{2400,3600,4200,4800,6000,7200};
constexpr std::array<std::int64_t,3> nearStarts{88'000,90'000,92'000};
constexpr std::array<float,6> levels{.125f,.25f,.375f,-.125f,-.25f,-.375f};
static_assert(starts[4] + cut - blockFrames > 3 * ClipPlayback::readAheadFrames(rate));
static_assert(starts[5] + cut - blockFrames > 3 * ClipPlayback::readAheadFrames(rate));
static_assert(starts[3] + cut - nearStarts[0] < ClipPlayback::readAheadFrames(rate));
static_assert(starts[4] + cut - nearStarts[1] < ClipPlayback::readAheadFrames(rate));
static_assert(starts[5] + cut - nearStarts[2] < ClipPlayback::readAheadFrames(rate));

void require(bool pass, const char* detail) {
    if (!pass) throw std::runtime_error(detail);
}

float signal(std::size_t index, std::int64_t frame) {
    return levels[index] + static_cast<float>((frame % (239 + 2 * static_cast<std::int64_t>(index))) - 119) / 8192.f;
}

enum class ThirdCell { Start, Stop, NoStop };

std::unique_ptr<Store> prepareProject(const test::TempDirectory& folder, ThirdCell thirdCell, bool pagedThird,
                                      std::int64_t thirdEnd = 0) {
    StoreError error{};
    const auto project = folder.path() / "scene.adi";
    auto store = Store::createEmpty(project,error);
    require(static_cast<bool>(store),"create the saved three-track raw tempo Scene");
    test::applySessionSchema(store->db());
    auto& db = store->db();
    db.exec("INSERT INTO project(id,name,sample_rate) VALUES(1,'Three raw tracks',48000);"
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES"
            "(1,'audio','A',0),(2,'audio','B',1),(3,'audio','C',2),(9,'master','Main',3);"
            "INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120);"
            "INSERT INTO scenes(id,ord,tempo_bpm,tempo_enabled) VALUES(10,0,120,0),(20,1,140,1);");
    for (std::size_t index = 0; index < clips.size(); ++index) {
        if (index == clips.size() - 1 && thirdCell != ThirdCell::Start) {
            SQLite::Statement slot(db,"INSERT INTO clip_slots(track_id,scene_id,clip_id,has_stop) VALUES(3,20,NULL,?)");
            slot.bind(1,thirdCell == ThirdCell::Stop ? 1 : 0); slot.exec();
            continue;
        }
        const auto id = clips[index];
        const auto lengthNs = index == 2 && thirdEnd != 0 ? thirdEnd * 1'000'000'000 / rate : durationNs;
        const auto track = static_cast<std::int64_t>(index % 3 + 1);
        const bool target = index >= 3;
        const auto name = std::to_string(id) + ".wav";
        const auto path = folder.path() / name;
        std::vector<float> samples(mediaFrames);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
            samples[frame] = signal(index,static_cast<std::int64_t>(frame));
        {
            audio::WavWriter wav(path,rate,1,audio::WavFormat::Float32);
            wav.write(samples.data(),mediaFrames);
            wav.close();
        }
        const auto digest = media::blake3File(path);
        require(static_cast<bool>(digest),"hash a raw Scene WAV");
        SQLite::Statement media(db,"INSERT INTO media_files(id,hash_blake3,rel_path,format,sample_rate,channels,frames)"
                                   " VALUES(?,?,?,'wav',48000,1,240000)");
        media.bind(1,id); media.bind(2,digest.hex); media.bind(3,name); media.exec();
        SQLite::Statement clip(db,"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled)"
                                  " VALUES(?,?,'audio',1,0,?,0)");
        clip.bind(1,id); clip.bind(2,track); clip.bind(3,lengthNs); clip.exec();
        SQLite::Statement raw(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled)"
                                 " VALUES(?,?,240000,0)");
        raw.bind(1,id); raw.bind(2,id); raw.exec();
        const auto ns = starts[index] * 1'000'000'000 / rate;
        SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns) VALUES(?,?,?)");
        pins.bind(1,id); pins.bind(2,ns); pins.bind(3,ns + lengthNs); pins.exec();
        SQLite::Statement slot(db,"INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(?,?,?)");
        slot.bind(1,track); slot.bind(2,target ? 20 : 10); slot.bind(3,id); slot.exec();
        SQLite::Statement launch(db,"INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade,launch_quant)"
                                    " VALUES(?,?,?,0,?)");
        launch.bind(1,id); launch.bind(2,target ? 1 : 0); launch.bind(3,target || (pagedThird && index == 2) ? 0 : 1);
        launch.bind(4,target ? 8 : 1); launch.exec();
    }
    store.reset();
    store = Store::open(project,error);
    require(static_cast<bool>(store),"reopen the persisted three-track project");
    return store;
}

struct Playback {
    Session& session;
    std::span<float> pcm;
    std::array<LaunchTransition,12> edges{};
    std::array<std::size_t,12> edgeOrder{};
    LaunchSceneTransition scene{};
    std::size_t edgeCount = 0, sceneCount = 0, noticeOrder = 0, sceneOrder = 0;
    std::int64_t cursor = 0;
    bool crossedOnOldClock = false, pacePaged = false;

    void through(std::int64_t end) {
        std::array<float,blockFrames> left{}, right{};
        float* output[]{left.data(),right.data()};
        while (cursor < end) {
            const auto frames = static_cast<std::int32_t>(std::min<std::int64_t>(blockFrames,end - cursor));
            const AudioIo io{output,nullptr,2,0,frames,cursor};
            {
                auto render = session.beginRender();
                if ((cursor == crossing && frames == blockFrames) || cursor == cut)
                    crossedOnOldClock = render.bpmAtTick(bar) == 120;
                render.observeLaunchTransitions(this,[](void* opaque,const LaunchTransition& edge,std::int64_t) noexcept {
                    auto& playback = *static_cast<Playback*>(opaque);
                    ++playback.noticeOrder;
                    if (playback.edgeCount < playback.edges.size()) {
                        playback.edges[playback.edgeCount] = edge;
                        playback.edgeOrder[playback.edgeCount] = playback.noticeOrder;
                    }
                    ++playback.edgeCount;
                });
                render.observeSceneLaunches(this,[](void* opaque,const LaunchSceneTransition& edge,std::int64_t) noexcept {
                    auto& playback = *static_cast<Playback*>(opaque);
                    playback.scene = edge;
                    playback.sceneOrder = ++playback.noticeOrder;
                    ++playback.sceneCount;
                });
                require(render.process(io),"render the real three-track Session callback");
            }
            require(std::equal(left.begin(),left.begin()+frames,right.begin()),"all three raw voices have matched stereo PCM");
            std::copy_n(left.begin(),frames,pcm.begin()+cursor);
            cursor += frames;
            if (pacePaged) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            else if (cursor >= prepareAt) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

std::array<LaunchTrackStatus,3> readTracks(const Session& session) {
    std::array<LaunchTrackStatus,3> rows{};
    LaunchStatusHeader header;
    require(session.readLaunchStatus(rows,header) && header.trackCount == rows.size(),"read all three stored audio tracks");
    return rows;
}

enum class Outcome {
    Warm, ColdThird, StopTrack, SavedStop, ColdSavedStop, CompetingSavedStop, ResidentNoStop,
    Continues, ColdContinue, CompetingStopContinue, CompetingBackContinue,
    ShortContinue, ColdShortContinue, StopShortContinue,
    EndAtCut, ColdEndAtCut, EndBeforeCut, IdleNoStop, ColdIdleNoStop, BackIdleNoStop
};

void exercise(Outcome outcome) {
    const bool warm = outcome == Outcome::Warm || outcome == Outcome::SavedStop ||
        outcome == Outcome::Continues || outcome == Outcome::ShortContinue || outcome == Outcome::EndAtCut ||
        outcome == Outcome::EndBeforeCut || outcome == Outcome::IdleNoStop;
    const bool savedStop = outcome == Outcome::SavedStop || outcome == Outcome::ColdSavedStop ||
        outcome == Outcome::CompetingSavedStop;
    const bool endAtCut = outcome == Outcome::EndAtCut || outcome == Outcome::ColdEndAtCut;
    const bool endBeforeCut = outcome == Outcome::EndBeforeCut;
    const bool shortContinue = endAtCut || endBeforeCut || outcome == Outcome::ShortContinue ||
        outcome == Outcome::ColdShortContinue || outcome == Outcome::StopShortContinue;
    const auto naturalEnd = endAtCut ? cut : endBeforeCut ? cut - 1 : continuedEndSample;
    const bool idleInitially = outcome == Outcome::IdleNoStop || outcome == Outcome::ColdIdleNoStop ||
        outcome == Outcome::BackIdleNoStop;
    const bool continues = shortContinue || idleInitially || outcome == Outcome::Continues || outcome == Outcome::ColdContinue ||
        outcome == Outcome::CompetingStopContinue || outcome == Outcome::CompetingBackContinue;
    const bool residentNoStop = outcome == Outcome::ResidentNoStop;
    const bool back = outcome == Outcome::CompetingBackContinue || outcome == Outcome::BackIdleNoStop;
    std::optional<std::size_t> manualStop;
    if (outcome == Outcome::StopTrack) manualStop = 1;
    if (outcome == Outcome::CompetingSavedStop || outcome == Outcome::CompetingStopContinue) manualStop = 0;
    if (outcome == Outcome::CompetingBackContinue || outcome == Outcome::StopShortContinue) manualStop = 2;
    if (outcome == Outcome::BackIdleNoStop) manualStop = 0;
    constexpr std::array names{"warm","cold-third","stop","saved-stop","cold-saved-stop","competing-saved-stop",
                               "resident-no-stop","continue","cold-continue","competing-stop-continue","competing-back-continue",
                               "short-continue","cold-short-continue","stop-short-continue",
                               "end-at-cut","cold-end-at-cut","end-before-cut",
                               "idle-no-stop","cold-idle-no-stop","back-idle-no-stop"};
    const auto* mode = names[static_cast<std::size_t>(outcome)];
    auto startedAt = nearStarts;
    if (outcome == Outcome::ColdThird) startedAt[2] = blockFrames;
    if (outcome == Outcome::ColdSavedStop || outcome == Outcome::ColdContinue ||
        outcome == Outcome::ColdShortContinue || outcome == Outcome::ColdEndAtCut || outcome == Outcome::ColdIdleNoStop)
        startedAt[1] = blockFrames;
    if (continues) startedAt[2] = 0;
    constexpr auto end = cut + 3 * blockFrames;
    constexpr auto gainAt = cut - 1000;
    test::TempDirectory folder{"three_raw_timed_scene",mode};
    auto store = prepareProject(folder,savedStop ? ThirdCell::Stop :
        continues || residentNoStop ? ThirdCell::NoStop : ThirdCell::Start,continues,shortContinue ? naturalEnd : 0);
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(),"load the old stored raw sources");
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(end));
    Playback playback{session,pcm};
    playback.pacePaged = continues;
    const auto initialAllocations = test::sceneClockCallbackAllocations();
    const auto initialFileIo = audio::callbackFileIo.load();
    std::array<std::size_t,3> order{0,1,2};
    std::ranges::sort(order,{},[&](auto index) { return startedAt[index]; });
    for (const auto index : order) {
        if (idleInitially && index == 2) continue;
        playback.through(startedAt[index]);
        LaunchRequest start;
        start.kind = LaunchRequestKind::SlotPress;
        start.incarnation = session.launchIncarnation();
        start.track = static_cast<std::int64_t>(index + 1);
        start.clip = clips[index];
        start.scene = 10;
        require(session.postLaunchGesture({&start,1}),"queue a saved outgoing raw slot");
    }
    if (continues && !idleInitially) {
        playback.through(gainAt);
        const auto source = readTracks(session)[2];
        require(session.postLaunchClipGainEdit({3,clips[2],session.launchIncarnation(),source.run,0.5f,ClipUpdateRate::None}),
                "apply half gain to the live paged run before offering the clock");
    }
    playback.through(prepareAt);
    const auto old = readTracks(session);
    for (std::size_t index = 0; index < old.size(); ++index) {
        if (idleInitially && index == 2)
            require(old[index].clip == 0 && old[index].run == 0,"C has no run before preparing the Scene");
        else
            require(old[index].clip == clips[index] && old[index].run != 0,"keep each outgoing run alive up to the Scene");
    }
    const auto initialEdges = idleInitially ? 2u : 3u;
    require(playback.edgeCount == initialEdges && playback.sceneCount == 0,"only the commanded outgoing slots have launched");
    const bool staged = session.prepareSceneLaunch(*store,20,bar);
    if (residentNoStop) require(!staged,"continuation without an adopted pager is outside timed raw admission");
    else if (!staged) throw std::runtime_error(session.error());
    if (back) {
        playback.through(cut);
        LaunchRequest request;
        request.kind = LaunchRequestKind::BackToArrangement;
        request.incarnation = session.launchIncarnation();
        request.track = static_cast<std::int64_t>(*manualStop + 1);
        require(session.postLaunchGesture({&request,1}),"post competing Back on the queued cut sample");
    } else if (manualStop) {
        playback.through(crossing);
        OpRequest command;
        command.opType = "session.stopTrack";
        command.payload = {{"track",*manualStop + 1},{"incarnation",session.launchIncarnation()}};
        require(OpJournal(*store).commit(command).ok,"queue competing StopTrack on the old clock");
    }
    if (endAtCut || endBeforeCut || idleInitially) {
        playback.through(cut);
        const auto atCut = readTracks(session);
        require(playback.sceneCount == 0,"the audited old prefix has no prepared Scene receipt");
        if (endBeforeCut) {
            const auto& edge = playback.edges[initialEdges];
            require(playback.edgeCount == initialEdges + 1 && edge.track == 3 && edge.oldRun == old[2].run &&
                    edge.newClip == 0 && edge.sample == naturalEnd && atCut[2].clip == 0 && atCut[2].run == 0,
                    "a run already ended in the old prefix has no live identity at the cut");
        } else if (idleInitially) {
            require(playback.edgeCount == initialEdges && atCut[2].clip == 0 && atCut[2].run == 0,
                    "the saved No Stop cell still has no live run at the audited cut opening");
        } else {
            require(playback.edgeCount == initialEdges && atCut[2].clip == old[2].clip && atCut[2].run == old[2].run,
                    "the half-open old prefix reaches End with the same live run and no Stop yet");
        }
    }
    playback.through(end);

    const auto after = readTracks(session);
    const bool naturalStop = shortContinue && !manualStop;
    const auto expectedEdges = initialEdges + (warm ? (continues ? 2u : 3u) : manualStop ? 1u : 0u)
        + (naturalStop ? 1u : 0u);
    const auto firstCommitted = initialEdges + (endBeforeCut ? 1u : 0u);
    require(playback.crossedOnOldClock && session.beginRender().bpmAtTick(bar) == (warm ? 140 : 120) &&
            playback.sceneCount == (warm ? 1u : 0u) && playback.edgeCount == expectedEdges,
            "clock, receipt and every due track action are one driver decision");
    if (warm) require(playback.scene.sample == cut && playback.scene.tempoBpm == 140,
                      "the single receipt uses the same exact 140 BPM cut");
    if (manualStop) {
        const auto index = *manualStop;
        const auto& edge = playback.edges[initialEdges];
        require(edge.track == static_cast<std::int64_t>(index + 1) && edge.oldClip == clips[index] &&
                edge.oldRun == old[index].run && edge.newClip == 0 && edge.sample == cut && edge.tick == bar &&
                edge.arrangement == back,
                "the manual command alone dispatches on the old clock while the timed Scene refuses");
    }
    if (naturalStop) {
        const auto& edge = playback.edges[endBeforeCut ? initialEdges : playback.edgeCount - 1];
        const auto pin = session.beginRender();
        require(edge.track == 3 && edge.oldClip == clips[2] && edge.oldRun == old[2].run && edge.newClip == 0 &&
                edge.oldPhysicalSourceAnchorFrame == starts[2] && edge.oldFramesSinceSourceAnchor == naturalEnd &&
                edge.sample == naturalEnd && edge.tick == pin.tickAtSample(naturalEnd),
                "the same paged run reaches its exclusive physical End on the selected clock");
    }
    if (outcome == Outcome::EndAtCut)
        require(playback.sceneOrder < playback.edgeOrder[3] && playback.edgeOrder[3] < playback.edgeOrder[4] &&
                playback.edgeOrder[4] < playback.edgeOrder[5],
                "the receipt and both Starts precede the sole natural Stop in the same callback");
    if (endBeforeCut)
        require(playback.edgeOrder[initialEdges] < playback.sceneOrder &&
                playback.sceneOrder < playback.edgeOrder[firstCommitted] &&
                playback.edgeOrder[firstCommitted] < playback.edgeOrder[firstCommitted + 1],
                "the sole old-prefix natural Stop precedes the receipt and both Starts");
    const auto stopsAtCut = [&](std::size_t index) {
        return (manualStop && *manualStop == index) || (warm && savedStop && index == 2);
    };
    for (std::size_t index = 0; index < after.size(); ++index) {
        if (warm && !(continues && index == 2)) {
            const auto& edge = playback.edges[firstCommitted + index];
            const auto expectedClip = stopsAtCut(index) ? 0 : clips[index + 3];
            require(edge.sample == cut && edge.tick == bar && edge.oldClip == clips[index] &&
                    edge.oldRun == old[index].run && edge.newClip == expectedClip &&
                    after[index].clip == expectedClip && after[index].run != old[index].run,
                    "every saved Start or Stop takes effect at the exact performed sample");
        } else if (stopsAtCut(index) || (naturalStop && index == 2)) {
            require(after[index].clip == 0,"the stopped run stays silent for the rest of the callback");
        } else {
            require(after[index].clip == old[index].clip && after[index].run == old[index].run,
                    "every track without a Start or Stop keeps its original run");
        }
    }
    for (auto sample = std::int64_t{0}; sample < end; ++sample) {
        auto expected = 0.f;
        for (std::size_t index = 0; index < startedAt.size(); ++index) {
            if (sample < startedAt[index] || (sample >= cut && stopsAtCut(index)) ||
                (index == 2 && (idleInitially || (shortContinue && sample >= naturalEnd)))) continue;
            const auto clip = warm && sample >= cut && !(continues && index == 2) ? index + 3 : index;
            const auto gain = continues && index == 2 && sample >= gainAt ? 0.5f : 1.0f;
            expected += gain * signal(clip,sample - startedAt[index] + starts[clip]);
        }
        require(pcm[static_cast<std::size_t>(sample)] == expected,
                "all three stereo voices or the exact old suffix match saved PCM");
    }
    require(test::sceneClockCallbackAllocations() == initialAllocations && audio::callbackFileIo.load() == initialFileIo,
            "bounded three-track admission performs no callback allocation or file reads");
    std::printf("Three-track raw Scene %s: exact stereo PCM, %zu clip edges, %zu Scene receipts\n",
                mode,playback.edgeCount,playback.sceneCount);
}
constexpr std::int64_t manyClip(std::size_t track, bool target) {
    if (track < 3) return clips[track + (target ? 3 : 0)];
    return (target ? 400 : 300) + static_cast<std::int64_t>(track);
}

constexpr std::int64_t earlyStopFrames = 1024;

std::unique_ptr<Store> prepareManyTrackProject(const test::TempDirectory& folder, std::size_t count,
                                               std::span<const std::size_t> savedStops,
                                               std::optional<std::size_t> shortOutgoing = {}) {
    auto store = prepareProject(folder,ThirdCell::Start,false);
    auto& db = store->db();
    {
        SQLite::Statement order(db,"UPDATE tracks SET index_in_parent=? WHERE id=9");
        order.bind(1,static_cast<std::int64_t>(count));
        order.exec();
    }
    for (std::size_t index = 3; index < count; ++index) {
        const auto track = static_cast<std::int64_t>(index + 1);
        SQLite::Statement addTrack(db,"INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(?,'audio',?,?)");
        addTrack.bind(1,track); addTrack.bind(2,"Additional raw track");
        addTrack.bind(3,static_cast<std::int64_t>(index)); addTrack.exec();
        for (const bool target : {false,true}) {
            const auto prototype = clips[index % 3 + (target ? 3 : 0)];
            const auto id = manyClip(index,target);
            SQLite::Statement clip(db,"INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns,loop_enabled)"
                                      " SELECT ?,?,kind,time_base,pos_ns,length_ns,loop_enabled FROM clips WHERE id=?");
            clip.bind(1,id); clip.bind(2,track); clip.bind(3,prototype); clip.exec();
            SQLite::Statement raw(db,"INSERT INTO audio_clips(clip_id,media_id,src_len_frames,warp_enabled)"
                                     " SELECT ?,media_id,src_len_frames,warp_enabled FROM audio_clips WHERE clip_id=?");
            raw.bind(1,id); raw.bind(2,prototype); raw.exec();
            SQLite::Statement pins(db,"INSERT INTO clip_playback_details(clip_id,content_offset_ns,source_end_ns)"
                                      " SELECT ?,content_offset_ns,source_end_ns FROM clip_playback_details WHERE clip_id=?");
            pins.bind(1,id); pins.bind(2,prototype); pins.exec();
            SQLite::Statement slot(db,"INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(?,?,?)");
            slot.bind(1,track); slot.bind(2,target ? 20 : 10); slot.bind(3,id); slot.exec();
            SQLite::Statement launch(db,"INSERT INTO clip_launch(clip_id,legato,ram_mode,clip_fade,launch_quant)"
                                        " SELECT ?,legato,ram_mode,clip_fade,launch_quant FROM clip_launch WHERE clip_id=?");
            launch.bind(1,id); launch.bind(2,prototype); launch.exec();
        }
    }
    if (!savedStops.empty()) {
        // Create every cloned media link before removing any template clip.
        SQLite::Transaction replacement(db);
        SQLite::Statement stop(db,"UPDATE clip_slots SET clip_id=NULL,has_stop=1 WHERE clip_id=?");
        SQLite::Statement removeTemplate(db,"DELETE FROM clips WHERE id=?");
        for (const auto track : savedStops) {
            const auto target = manyClip(track,true);
            stop.bind(1,target); stop.exec(); stop.reset();
            removeTemplate.bind(1,target); removeTemplate.exec(); removeTemplate.reset();
        }
        replacement.commit();
    }
    if (shortOutgoing) {
        const auto clip = manyClip(*shortOutgoing,false);
        const auto lengthNs = earlyStopFrames * 1'000'000'000 / rate;
        SQLite::Statement length(db,"UPDATE clips SET length_ns=? WHERE id=?");
        length.bind(1,lengthNs); length.bind(2,clip); length.exec();
        SQLite::Statement end(db,"UPDATE clip_playback_details SET source_end_ns=content_offset_ns+? WHERE clip_id=?");
        end.bind(1,lengthNs); end.bind(2,clip); end.exec();
    }
    store.reset();
    StoreError error{};
    store = Store::open(folder.path() / "scene.adi",error);
    require(static_cast<bool>(store),"reopen every added raw Scene track");
    return store;
}

void exerciseManyTracks(std::size_t count, Outcome outcome, std::size_t savedStopCount, bool idleSavedStop = false) {
    const bool warm = outcome == Outcome::Warm;
    const bool coldLast = outcome == Outcome::ColdThird;
    const bool stop = outcome == Outcome::StopTrack;
    constexpr auto end = cut + 3 * blockFrames;
    constexpr std::array<std::size_t,2> stopTracks{2,1};
    const auto savedStops = std::span(stopTracks).first(savedStopCount);
    const auto userStoppedTrack = savedStopCount == 2 ? std::size_t{0} : std::size_t{1};
    const auto idleTrack = idleSavedStop ? std::optional{savedStops.front()} : std::nullopt;
    const bool back = idleSavedStop && stop;
    const auto* mode = warm ? "warm" : coldLast ? "cold-last" : back ? "back-on-idle" : "stop";
    test::TempDirectory folder{"many_raw_timed_scene",mode};
    auto store = prepareManyTrackProject(folder,count,savedStops,idleTrack);
    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(),"load every prepared raw track");
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(end));
    Playback playback{session,pcm};
    const auto initialAllocations = test::sceneClockCallbackAllocations();
    const auto initialFileIo = audio::callbackFileIo.load();

    std::vector<std::int64_t> startedAt(count);
    const auto start = [&](std::size_t index, std::int64_t sample) {
        startedAt[index] = sample;
        playback.through(sample);
        LaunchRequest request;
        request.kind = LaunchRequestKind::SlotPress;
        request.incarnation = session.launchIncarnation();
        request.track = static_cast<std::int64_t>(index + 1);
        request.scene = 10;
        request.clip = manyClip(index,false);
        require(session.postLaunchGesture({&request,1}),"start each saved outgoing raw slot");
    };
    if (coldLast) {
        // The final target has never been read and its inherited frame is
        // far outside its initial raw page window.
        const auto finalTarget = (count - 1) % 3 + 3;
        require(starts[finalTarget] + cut - blockFrames > 3 * ClipPlayback::readAheadFrames(rate),
                "the only cold target lies beyond initial worker read-ahead");
        start(count - 1,blockFrames);
    }
    for (std::size_t index = 0; index < count - (coldLast ? 1u : 0u); ++index)
        start(index,cut - 8000 + 1000 * static_cast<std::int64_t>(index));
    playback.through(prepareAt);
    const auto read = [&] {
        std::vector<LaunchTrackStatus> rows(count);
        LaunchStatusHeader header;
        require(session.readLaunchStatus(rows,header) && header.trackCount == count,"read all saved raw tracks");
        return rows;
    };
    const auto old = read();
    const auto initialEdges = count + (idleTrack ? 1u : 0u);
    require(playback.edgeCount == initialEdges && playback.sceneCount == 0,"only outgoing edges precede the Scene");
    const auto earlyStop = idleTrack ? startedAt[*idleTrack] + earlyStopFrames : 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (idleTrack == index) {
            require(old[index].clip == 0 && old[index].run == 0 && old[index].source == LaunchTrackSource::Silence,
                    "the first saved Stop is already silent after its physical End");
            const auto beforeCut = std::span(playback.edges).first(initialEdges);
            require(std::ranges::any_of(beforeCut,[&](const auto& edge) {
                return edge.track == static_cast<std::int64_t>(index + 1) &&
                    edge.oldClip == manyClip(index,false) && edge.oldRun != 0 && edge.newClip == 0 &&
                    edge.sample == earlyStop && !edge.arrangement;
            }),"the first saved Stop's old run ended once before staging");
        } else {
            require(old[index].clip == manyClip(index,false) && old[index].run != 0,"each other old raw run is active");
        }
    }

    if (!session.prepareSceneLaunch(*store,20,bar)) throw std::runtime_error(session.error());
    if (back) {
        playback.through(cut);
        LaunchRequest request;
        request.kind = LaunchRequestKind::BackToArrangement;
        request.incarnation = session.launchIncarnation();
        request.track = static_cast<std::int64_t>(*idleTrack + 1);
        require(session.postLaunchGesture({&request,1}),"queue Back for the silent Stop track on the old cut");
    } else if (stop) {
        playback.through(cut);
        OpRequest command;
        command.opType = "session.stopTrack";
        command.payload = {{"track",userStoppedTrack + 1},{"incarnation",session.launchIncarnation()}};
        require(OpJournal(*store).commit(command).ok,"queue audited StopTrack at the old cut opening");
    }
    playback.through(end);

    const auto after = read();
    const auto sceneEdges = count - (idleTrack ? 1u : 0u);
    require(playback.crossedOnOldClock && session.beginRender().bpmAtTick(bar) == (warm ? 140 : 120) &&
            playback.sceneCount == (warm ? 1u : 0u) &&
            playback.edgeCount == initialEdges + (warm ? sceneEdges : stop ? 1u : 0u),
            "the clock, receipt and complete raw track group resolve together");
    if (warm) {
        require(playback.scene.sample == cut && playback.scene.tick == bar && playback.scene.tempoBpm == 140,
                "one receipt carries the adopted 140 BPM clock at the exact cut");
    } else if (stop) {
        const auto index = back ? *idleTrack : userStoppedTrack;
        const auto& edge = playback.edges[initialEdges];
        require(edge.track == static_cast<std::int64_t>(index + 1) && edge.oldClip == old[index].clip &&
                edge.oldRun == old[index].run && edge.newClip == 0 && edge.sample == cut && edge.tick == bar &&
                edge.arrangement == back,"only the independent manual command takes effect on the old Bar");
    }
    const auto stopsAtCut = [&](std::size_t index) {
        return (stop && !back && index == userStoppedTrack) ||
            (warm && std::ranges::find(savedStops,index) != savedStops.end());
    };
    auto nextEdge = initialEdges;
    for (std::size_t index = 0; index < count; ++index) {
        if (idleTrack == index) {
            require(after[index].clip == 0 && after[index].run == 0 &&
                    after[index].source == (back ? LaunchTrackSource::Arrangement : LaunchTrackSource::Silence),
                    "the idle saved Stop emits no duplicate transition and preserves its source unless Back wins");
        } else if (warm) {
            const auto& edge = playback.edges[nextEdge++];
            const auto expectedClip = stopsAtCut(index) ? 0 : manyClip(index,true);
            require(edge.track == static_cast<std::int64_t>(index + 1) && edge.sample == cut && edge.tick == bar &&
                    edge.oldClip == old[index].clip && edge.oldRun == old[index].run && edge.newClip == expectedClip &&
                    after[index].clip == expectedClip && after[index].run != old[index].run,
                    "every saved Start and Stop applies at the exact performed sample");
        } else if (stopsAtCut(index)) {
            require(after[index].clip == 0,"the manually stopped track stays stopped");
        } else {
            require(after[index].clip == old[index].clip && after[index].run == old[index].run,
                    "every untouched track keeps its exact outgoing run");
        }
    }
    for (auto sample = std::int64_t{0}; sample < end; ++sample) {
        auto expected = 0.f;
        for (std::size_t index = 0; index < count; ++index) {
            if (sample < startedAt[index] || (idleTrack == index && sample >= earlyStop) ||
                (sample >= cut && stopsAtCut(index))) continue;
            const auto prototype = index % 3 + (warm && sample >= cut ? 3 : 0);
            expected += signal(prototype,sample - startedAt[index] + starts[prototype]);
        }
        require(pcm[static_cast<std::size_t>(sample)] == expected,"exact full-buffer stereo PCM across the group cut");
    }
    require(test::sceneClockCallbackAllocations() == initialAllocations && audio::callbackFileIo.load() == initialFileIo,
            "the bounded larger group performs no driver allocation or file reads");
    std::printf("%zu-track raw Scene with %zu saved Stops %s%s: exact stereo PCM, %zu clip edges, %zu Scene receipts\n",
                count,savedStopCount,idleTrack ? "(one idle) " : "",mode,playback.edgeCount,playback.sceneCount);
}

enum class WideContinueCase { Warm, Cold, UserStop, Idle, SavedStop };

void exerciseWideContinue(WideContinueCase scenario) {
    constexpr std::size_t count = 6;
    constexpr std::size_t continued = 2;
    constexpr auto end = cut + 3 * blockFrames;
    constexpr auto gainAt = cut - 1000;
    const bool warm = scenario == WideContinueCase::Warm;
    const bool cold = scenario == WideContinueCase::Cold;
    const bool userStop = scenario == WideContinueCase::UserStop;
    const bool idle = scenario == WideContinueCase::Idle;
    const bool savedStop = scenario == WideContinueCase::SavedStop;
    constexpr std::array names{"warm","cold","user-stop","idle","with-saved-stop"};
    const auto* mode = names[static_cast<std::size_t>(scenario)];
    test::TempDirectory folder{"wide_raw_timed_continue",mode};
    constexpr std::array<std::size_t,1> stopped{1};
    auto store = prepareManyTrackProject(folder,count,
        savedStop ? std::span<const std::size_t>(stopped) : std::span<const std::size_t>{});
    {
        auto& db = store->db();
        SQLite::Transaction change(db);
        SQLite::Statement cell(db,"UPDATE clip_slots SET clip_id=NULL,has_stop=0 WHERE clip_id=?");
        cell.bind(1,manyClip(continued,true));
        cell.exec();
        SQLite::Statement discard(db,"DELETE FROM clips WHERE id=?");
        discard.bind(1,manyClip(continued,true));
        discard.exec();
        SQLite::Statement paged(db,"UPDATE clip_launch SET ram_mode=0 WHERE clip_id=?");
        paged.bind(1,manyClip(continued,false));
        paged.exec();
        change.commit();
    }
    store.reset();
    StoreError error{};
    store = Store::open(folder.path() / "scene.adi",error);
    require(static_cast<bool>(store),"reopen the wider Scene with a saved No Stop cell");

    Session session;
    session.graph().setFadeFrames(0);
    if (!session.load(*store,{}, {2,rate,blockFrames})) throw std::runtime_error(session.error());
    require(session.problems().empty(),"load the paged continuing raw clip and every saved target");
    session.transport().play();
    std::vector<float> pcm(static_cast<std::size_t>(end));
    Playback playback{session,pcm};
    playback.pacePaged = !idle;
    const auto allocations = test::sceneClockCallbackAllocations();
    const auto fileIo = audio::callbackFileIo.load();
    std::array<std::int64_t,count> startedAt{};
    const auto start = [&](std::size_t index, std::int64_t sample) {
        startedAt[index] = sample;
        playback.through(sample);
        LaunchRequest request;
        request.kind = LaunchRequestKind::SlotPress;
        request.incarnation = session.launchIncarnation();
        request.track = static_cast<std::int64_t>(index + 1);
        request.scene = 10;
        request.clip = manyClip(index,false);
        require(session.postLaunchGesture({&request,1}),"launch the saved outgoing slot");
    };
    if (!idle) start(continued,0);
    if (cold) start(count - 1,blockFrames);
    for (std::size_t index = 0; index < count; ++index) {
        if (index != continued && !(cold && index == count - 1))
            start(index,cut - 8'000 + 1'000 * static_cast<std::int64_t>(index));
    }
    const auto read = [&] {
        std::array<LaunchTrackStatus,count> rows{};
        LaunchStatusHeader header;
        require(session.readLaunchStatus(rows,header) && header.trackCount == count,"read the wider raw Scene tracks");
        return rows;
    };
    if (!idle) {
        playback.through(gainAt);
        require(session.postLaunchClipGainEdit({3,manyClip(continued,false),session.launchIncarnation(),
                read()[continued].run,0.5f,ClipUpdateRate::None}),"apply half gain to the continuing paged run");
    }
    playback.through(prepareAt);
    const auto old = read();
    const auto initialEdges = idle ? count - 1 : count;
    require(playback.edgeCount == initialEdges && playback.sceneCount == 0,
            "all requested outgoing slots are active before staging");
    for (std::size_t index = 0; index < count; ++index) {
        if (idle && index == continued)
            require(old[index].clip == 0 && old[index].run == 0,"the wider idle No Stop control has no run");
        else
            require(old[index].clip == manyClip(index,false) && old[index].run != 0,"each commanded old run is active");
    }
    const bool staged = session.prepareSceneLaunch(*store,20,bar);
    if (savedStop) require(!staged,"a wider Scene with both saved Stop and No Stop is refused");
    else require(staged,"the owner stages a wider stored raw Scene for the performed-cut check");
    if (userStop) {
        playback.through(cut);
        OpRequest command;
        command.opType = "session.stopTrack";
        command.payload = {{"track",continued + 1},{"incarnation",session.launchIncarnation()}};
        require(OpJournal(*store).commit(command).ok,"queue StopTrack on the continued track at the old cut opening");
    }
    playback.through(end);

    const auto after = read();
    require(playback.crossedOnOldClock && session.beginRender().bpmAtTick(bar) == (warm ? 140 : 120) &&
            playback.sceneCount == (warm ? 1u : 0u) &&
            playback.edgeCount == initialEdges + (warm ? count - 1 : userStop ? 1u : 0u),
            "the clock, receipt, and five Starts resolve in a single driver decision");
    if (warm) {
        require(playback.scene.sample == cut && playback.scene.tick == bar && playback.scene.tempoBpm == 140,
                "the accepted wider Scene emits one receipt on the exact cut");
    } else if (userStop) {
        const auto& edge = playback.edges[initialEdges];
        require(edge.track == 3 && edge.oldRun == old[continued].run && edge.newClip == 0 &&
                edge.sample == cut && edge.tick == bar && after[continued].run == 0,
                "the independent Stop closes the paged run once on the old Bar");
    }
    auto edgeIndex = initialEdges;
    for (std::size_t index = 0; index < count; ++index) {
        if (warm && index != continued) {
            const auto& edge = playback.edges[edgeIndex++];
            require(edge.track == static_cast<std::int64_t>(index + 1) && edge.sample == cut && edge.tick == bar &&
                    edge.oldRun == old[index].run && edge.newClip == manyClip(index,true) &&
                    after[index].clip == edge.newClip && after[index].run != old[index].run,
                    "every replacement has a new run at the same exact performed sample");
        } else if (!(userStop && index == continued)) {
            require(after[index].clip == old[index].clip && after[index].run == old[index].run,
                    "each continued, idle, or refused track keeps its exact old run state");
        }
    }
    for (auto sample = std::int64_t{0}; sample < end; ++sample) {
        auto expected = 0.f;
        for (std::size_t index = 0; index < count; ++index) {
            if (sample < startedAt[index] || (index == continued && (idle || (userStop && sample >= cut)))) continue;
            const auto prototype = index % 3 + (warm && index != continued && sample >= cut ? 3 : 0);
            const auto gain = index == continued && sample >= gainAt ? 0.5f : 1.0f;
            expected += gain * signal(prototype,sample - startedAt[index] + starts[prototype]);
        }
        require(pcm[static_cast<std::size_t>(sample)] == expected,
                "exact stereo PCM preserves the continuing paged phase and live gain through the cut");
    }
    require(test::sceneClockCallbackAllocations() == allocations && audio::callbackFileIo.load() == fileIo,
            "wider timed continuation performs no callback allocation or file reads");
    std::printf("Six-track raw Scene No Stop %s: exact stereo PCM, %zu clip edges, %zu Scene receipts\n",
                mode,playback.edgeCount,playback.sceneCount);
}

} // namespace

namespace adi::test {
void runThreeTrackTimedRawSceneTests() {
    for (const auto outcome : {Outcome::Warm,Outcome::ColdThird,Outcome::StopTrack})
        exerciseManyTracks(4,outcome,2,true);
    for (const auto tracks : {std::size_t{4},std::size_t{6}})
        for (const auto savedStops : {std::size_t{0},std::size_t{1},std::size_t{2}})
            for (const auto outcome : {Outcome::Warm,Outcome::ColdThird,Outcome::StopTrack})
                exerciseManyTracks(tracks,outcome,savedStops);
    for (const auto outcome : {Outcome::Warm,Outcome::ColdThird,Outcome::StopTrack,Outcome::SavedStop,
                               Outcome::ColdSavedStop,Outcome::CompetingSavedStop,Outcome::ResidentNoStop,Outcome::Continues,
                               Outcome::ColdContinue,Outcome::CompetingStopContinue,Outcome::CompetingBackContinue})
        exercise(outcome);
    for (const auto outcome : {Outcome::ShortContinue,Outcome::ColdShortContinue,Outcome::StopShortContinue,
                               Outcome::EndAtCut,Outcome::ColdEndAtCut,Outcome::EndBeforeCut})
        exercise(outcome);
    for (const auto outcome : {Outcome::IdleNoStop,Outcome::ColdIdleNoStop,Outcome::BackIdleNoStop})
        exercise(outcome);
    for (const auto scenario : {WideContinueCase::Warm,WideContinueCase::Cold,WideContinueCase::UserStop,
                                WideContinueCase::Idle,WideContinueCase::SavedStop})
        exerciseWideContinue(scenario);
}
} // namespace adi::test
