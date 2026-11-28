// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/appdata.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/recording_commands.hpp"
#include "adi/warp_commands.hpp"
#include "adi/audio/warp_map.hpp"
#include "adi/engine/session.hpp"
#include "adi/engine/audio_recording.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/engine/recording_recovery_plan.hpp"
#include "adi/digest.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/scan_process.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <exception>
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <limits>
#include <string_view>
#include <thread>
namespace {
using namespace adi;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* what){++checks;if(!ok){++failures;std::printf("FAIL %s\n",what);}}
AudioRecordingResult finish(AudioInputRecorder& recorder) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline){if(auto result=recorder.poll())return *result;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    throw std::runtime_error("recording did not finish");
}
struct Fixture {
    test::TempDirectory temporary{"recording_commands","project"};std::unique_ptr<Store> store;
    Fixture(){StoreError error{};store=Store::createEmpty(temporary.path()/"song.adi",error);if(!store)throw std::runtime_error(toString(error));
        store->db().exec("INSERT INTO tracks(id,kind,name,record_armed) VALUES(21,'audio','One',1),(22,'audio','Two',1),(9,'master','Main',0)");}
    static float sample(std::size_t frame){return static_cast<float>(frame%17)/32-0.25f;}
    AudioRecordingResult capture(bool two=false,Precision precision=Precision::Float32,bool loop=false,int totalFrames=300,
        const std::filesystem::path& workFolder={},std::int64_t startSample=512,
        bool recoverable=false,audio::WavFormat encoding=audio::WavFormat::Float32) {
        Session session;SessionSpec format{2,48000,128};format.precision=precision;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},format))throw std::runtime_error(session.error());
        AudioRecordingSpec request;request.workFolder=workFolder.empty()?temporary.path():workFolder;request.sampleRate=48000;request.maxFrames=128;request.ringFrames=4096;request.targets={{21,{0}}};if(two)request.targets.push_back({22,{0}});
        if(loop)request.loop=AudioRecordingLoop{512,640,8};
        request.format=encoding;
        std::string error;
        if(recoverable && !recording::configureRecovery(*store,request,error))throw std::runtime_error(error);
        if(!session.startRecording(request,error))throw std::runtime_error(error);
        if(recoverable) {
            for(const auto& entry:std::filesystem::directory_iterator(request.workFolder)) {
                if(!std::filesystem::is_regular_file(entry.path()/"take.json"))continue;
                check(!std::filesystem::exists(entry.path()/"start.json"),
                      "the immutable recording plan reaches disk before the first input block");
                check(recording::recoverInterrupted(*store,request.workFolder,entry.path()).status==
                      recording::RecoveryStatus::Refused,"a live recorder holds the recovery lease");
            }
        }
        if(loop)session.transport().loop(512,640);
        session.transport().locate(startSample);session.transport().play();std::array<float,128> source{};const float* inputs[]={source.data()};std::array<float,256> out{};float* outputs[]={out.data(),out.data()+128};
        std::int64_t stream=0;
        for(int consumed=0;consumed<totalFrames;) {
            const auto count=std::min(128,totalFrames-consumed);consumed+=count;
            for(int frame=0;frame<count;++frame)source[static_cast<std::size_t>(frame)]=sample(static_cast<std::size_t>(stream+frame));
            AudioIo io;io.in=inputs;io.numIn=1;io.out=outputs;io.numOut=2;io.frames=count;io.streamTimeSamples=stream;session.process(io);stream+=count;
        }
        session.recording().stop();AudioIo idle;idle.frames=1;idle.out=outputs;idle.numOut=2;session.process(idle);return finish(session.recording());
    }
    std::vector<double> play(Precision precision) {
        Session session;SessionSpec format{2,48000,128};format.precision=precision;session.graph().setFadeFrames(0);
        if(!session.load(*store,{},format) || !session.problems().empty())throw std::runtime_error("load attached recording");
        session.transport().play();std::array<float,256> narrow{};std::array<double,256> wide{};float* out[]={narrow.data(),narrow.data()+128};double* out64[]={wide.data(),wide.data()+128};std::vector<double> result(1024);
        for(std::size_t first=0;first<result.size();first+=128) {
            if(!session.clips()->prime(std::chrono::seconds(5)))throw std::runtime_error("prime recorded clip");
            AudioIo io;io.frames=128;io.numOut=2;io.out=out;if(precision==Precision::Float64)io.out64=out64;session.process(io);
            for(std::size_t frame=0;frame<128;++frame)result[first+frame]=precision==Precision::Float64?wide[frame]:narrow[frame];
        }
        return result;
    }
};
void latencyCompensatedPlacement() {
    struct DriverLatency {
        std::int64_t input=256,output=384;
        double errorCompensationMs=1.0;
    } driver;
    constexpr std::uint32_t rate=48000;
    constexpr std::int64_t capturedAt=800;
    constexpr std::uint32_t frames=128;
    const auto offset=driver.input+driver.output+std::llround(driver.errorCompensationMs*rate/1000.0);
    check(offset==688,"simulated input, output and 1 ms driver error total 688 samples");
    struct Case {std::int64_t offset,position;};
    for(const auto test:{Case{offset,112},Case{0,capturedAt},Case{-24,824},Case{offset+capturedAt,0}}) {
        Fixture f;
        auto recorded=f.capture(false,Precision::Float32,false,frames,{},capturedAt);
        const std::array<recording::ClipIds,1> ids={{{7,8,"Latency placement"}}};
        const auto bad=recording::prepareCommit(*f.store,recorded,ids,std::numeric_limits<std::int64_t>::min());
        check(!bad.ok && std::filesystem::is_regular_file(recorded.tracks[0].file),
              "an overflowing placement offset leaves the private recording available");
        auto prepared=recording::prepareCommit(*f.store,recorded,ids,test.offset);
        const auto committed=prepared.ok && recording::commit(*f.store,prepared).ok;
        check(committed,"latency-compensated take commits through the normal recording commands");
        if(!committed)continue;
        const auto heard=f.play(Precision::Float32);
        bool exact=recorded.startSample==capturedAt && recorded.frames==frames;
        for(std::size_t frame=0;frame<heard.size();++frame) {
            const auto local=static_cast<std::int64_t>(frame)-test.position;
            const auto expected=local>=0 && local<frames ? Fixture::sample(static_cast<std::size_t>(local)) : 0.0f;
            exact=exact && heard[frame]==expected;
        }
        check(exact,"corrected song position shifts the whole take without changing its recorded samples");
    }
}
void midiLinearTakes() {
    Fixture fixture;
    auto& store = *fixture.store;
    auto& db = store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(23,'midi','MPE'),(24,'midi','Empty MIDI')");
    const auto before = digestProject(store).text;
    const std::array<notes::NoteValues, 2> notes{{
        {.start=400, .duration=200, .id=17, .key=60, .velocity=96, .releaseVelocity=41},
        {.start=120, .duration=180, .id=18, .key=65, .velocity=80, .channel=2},
    }};
    const auto point = [](std::int64_t at, float value) { return ExpressionPoint{at, value, 0.0f, 0, 0, {}}; };
    const std::array bend{point(0, -2.0f), point(80, 2.5f)};
    const std::array slide{point(25, .125f)};
    const std::array pressure{point(42, .75f)};
    const std::array<recording::MidiExpressionLane, 3> lanes{{
        {17, ExpressionDim::Pitch, bend}, {17, ExpressionDim::Timbre, slide}, {18, ExpressionDim::Pressure, pressure},
    }};
    const std::array<recording::MidiLinearTake, 2> takes{{
        {23, 7, 960, 960, "MPE Take", notes, lanes},
        {24, 8, 960, 960, "Silent Take", {}, {}},
    }};
    auto badLanes = lanes;
    badLanes.back().noteId = 400;
    auto orphan = takes;
    orphan.front().expression = badLanes;
    check(!recording::prepareMidiLinearCommit(store, orphan, 48000).ok && digestProject(store).text == before,
          "linear take rejects expression with no captured note without writing project rows");
    auto prepared = recording::prepareMidiLinearCommit(store, takes, 48000);
    check(prepared.ok && digestProject(store).text == before &&
              !std::filesystem::exists(media::projectFolder(store) / "Samples" / "Recorded"),
          "MIDI-only preparation changes no rows and creates no audio files");
    const auto committed = recording::commit(store, prepared);
    check(committed.ok && committed.seqs.size() == 1, "both armed MIDI takes land in one placement");
    if (!committed.ok)
        return;
    const auto read = readClipDetails(store, 7, true, true);
    check(read.clip && read.clip->notes.size() == 2 && read.clip->notes[0].id == 18 &&
              read.clip->notes[1].id == 17 && read.clip->notes[1].startTicks == 400 &&
              read.clip->notes[1].durationTicks == 200 && read.clip->notes[1].releaseVelocity == 41 &&
              (read.clip->notes[1].flags & NoteFlags::HasExpression) != 0,
          "the recorded notes retain play timing, release velocity and expression ownership");
    check(read.clip && read.clip->posTicks == 960 && read.clip->lengthTicks == 960 && read.clip->loopEnabled &&
              read.clip->loopStartTicks == 0 && read.clip->loopLengthTicks == 960,
          "the recorded clip initially spans one loop of its captured material");
    const auto silent = readClipDetails(store, 8);
    check(silent.clip && silent.clip->lengthTicks == 960 && silent.clip->notes.empty(),
          "an armed MIDI track with no notes keeps its recorded interval");
    const auto expression = readNoteExpressions(store, 7);
    check(expression.problems.empty() && expression.curves.size() == 3 && expression.curves[0].noteId == 17 &&
              expression.curves[0].points.size() == 2 && expression.curves[0].points.back().time_ticks == 80 &&
              expression.curves[0].points.back().value == 2.5f && expression.curves[1].noteId == 17 &&
              expression.curves[1].dimension == static_cast<int>(ExpressionDim::Timbre) &&
              expression.curves[2].noteId == 18 &&
              expression.curves[2].dimension == static_cast<int>(ExpressionDim::Pressure),
          "the two notes keep their own pitch, slide, and pressure curves");
    const auto nativeBend = store.getNoteExpression(7, 17, static_cast<int>(ExpressionDim::Pitch));
    History history(store);
    check(history.nextUndo() && history.nextUndo()->label == "Record MIDI" && history.undo().ok &&
              digestProject(store).text == before && !store.getNoteExpression(7, 17, static_cast<int>(ExpressionDim::Pitch)),
          "one Undo removes the two takes and their expression");
    check(nativeBend && history.redo().ok &&
              store.getNoteExpression(7, 17, static_cast<int>(ExpressionDim::Pitch)) == nativeBend,
          "Redo restores the exact captured expression bytes");
}

void midiOverdub() {
    Fixture fixture;
    auto& store = *fixture.store;
    auto& db = store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(23,'midi','One period'),(24,'midi','Repeated')");
    const std::array<notes::NoteValues, 1> oldNotes{{{.start=100, .duration=60, .id=17, .key=60, .velocity=80}}};
    const std::array modBefore{controllers::Point{1, 100, .25}};
    const std::array pedalBefore{controllers::Point{1, 80, 1.}};
    const std::array<recording::MidiControllerLane, 2> oldControls{{{1, 1, modBefore}, {1, 64, pedalBefore}}};
    const std::array<recording::MidiLinearTake, 2> original{{
        {23, 7, 960, 960, "One period", oldNotes, {}, false, oldControls},
        {24, 8, 960, 960, "Repeated", oldNotes, {}, false, oldControls},
    }};
    auto seed = recording::prepareMidiLinearCommit(store, original, 48000);
    if (!seed.ok || !recording::commit(store, seed).ok)
        throw std::runtime_error("seed the overdub's original clips");
    db.exec("UPDATE clips SET length_ticks=1920 WHERE id=8");
    const auto before = digestProject(store).text;
    const auto firstControls = controllers::readLanes(store, 7);
    const auto repeatControls = controllers::readLanes(store, 8);
    check(firstControls.ok && firstControls.lanes.size() == 2 && repeatControls.ok && repeatControls.lanes.size() == 2,
          "linear take images retain independently addressed mod and sustain lanes");

    const std::array<notes::NoteValues, 1> input{{{.start=120, .duration=90, .id=17, .key=67, .velocity=90}}};
    const std::array<ExpressionPoint, 1> bend{{{0, 2.f, 0.f, 0, 0, {}}}};
    const std::array<recording::MidiExpressionLane, 1> expression{{{17, ExpressionDim::Pitch, bend}}};
    const std::array modInput{controllers::Point{1, 120, .875}};
    const std::array pitchInput{controllers::Point{1, 80, .75}};
    const std::array<recording::MidiControllerLane, 2> controllerInput{{
        {1, 1, modInput}, {1, controllers::kPitchBend, pitchInput},
    }};
    const std::array<recording::MidiLinearTake, 2> overdub{{
        {23, 9, 1200, 240, "Merge", input, expression, true, controllerInput},
        {24, 10, 2160, 240, "Repeat Safety", input, expression, true, controllerInput},
    }};
    auto edit = recording::prepareMidiLinearCommit(store, overdub, 48000);
    check(edit.ok && edit.midiLandings.size() == 2 && digestProject(store).text == before,
          "planning an overdub leaves both Arrangement clips unchanged");
    if (!edit.ok || edit.midiLandings.size() != 2)
        return;
    const auto committed = recording::commit(store, edit);
    check(committed.ok, "the in-place overdub and safe take lane land in one recording transaction");
    if (!committed.ok)
        return;
    const auto& merge = edit.midiLandings[0];
    const auto& fallback = edit.midiLandings[1];
    const auto one = readClipDetails(store, 7, true, true);
    const auto repeat = readClipDetails(store, 8, true, true);
    const auto saved = readClipDetails(store, 10, true, true);
    check(merge.clip == 7 && merge.sourceStart == 240 && !merge.inTakeLane && merge.noteIds == std::vector<std::uint64_t>{18} &&
              one.clip && one.clip->notes.size() == 2 && one.clip->notes[0].id == 17 && one.clip->notes[0].startTicks == 100 &&
              one.clip->notes[1].id == 18 && one.clip->notes[1].startTicks == 360 &&
              one.clip->noteExpressions.curves.size() == 1 && one.clip->noteExpressions.curves[0].noteId == 18 &&
              one.clip->noteExpressions.curves[0].points.size() == 1 && one.clip->noteExpressions.curves[0].points[0].value == 2.,
          "overdub retains the original note and clip, and assigns the new note and curve a fresh identity");
    check(fallback.clip == 10 && fallback.inTakeLane && saved.clip && saved.clip->laneId &&
              saved.clip->notes.size() == 1 && saved.clip->noteExpressions.curves.size() == 1 &&
              repeat.clip && repeat.clip->lengthTicks == 1920 && repeat.clip->notes.size() == 1,
          "taking a later loop occurrence preserves the recorded notes and leaves the repeating clip intact");
    const auto mergedLanes = controllers::readLanes(store, 7);
    const auto fallbackLanes = controllers::readLanes(store, 10);
    check(firstControls.lanes.size() == 2 && mergedLanes.ok && mergedLanes.lanes.size() == 3 &&
              mergedLanes.lanes[0].id == firstControls.lanes[0].id &&
              mergedLanes.lanes[0].points == std::vector<controllers::Point>{{1, 100, .25}, {2, 360, .875}} &&
              mergedLanes.lanes[1] == firstControls.lanes[1] && mergedLanes.lanes[2].controller == controllers::kPitchBend &&
              mergedLanes.lanes[2].points == std::vector<controllers::Point>{{1, 320, .75}},
          "overdub adds shifted points with fresh identities and preserves the other envelope bytes");
    check(fallbackLanes.ok && fallbackLanes.lanes.size() == 2 &&
              fallbackLanes.lanes[0].points == std::vector<controllers::Point>(modInput.begin(), modInput.end()) &&
              fallbackLanes.lanes[1].points == std::vector<controllers::Point>(pitchInput.begin(), pitchInput.end()) &&
              controllers::readLanes(store, 8).lanes == repeatControls.lanes,
          "a fallback take keeps the new trajectories and preserves the repeating source envelopes");

    const std::array<recording::MidiRecordQuantize, 1> targets{{{7, merge.sourceStart, 1200, 240, 240, merge.noteIds}}};
    const auto quantize = recording::prepareRecordQuantize(store, targets);
    check(quantize.ok && notes::commitDiff(store, quantize).ok, "quantize the newly overdubbed identity");
    const auto snapped = readClipDetails(store, 7, true, true);
    check(snapped.clip && snapped.clip->notes.size() == 2 && snapped.clip->notes[0].startTicks == 100 &&
              snapped.clip->notes[1].startTicks == 240 && snapped.clip->notes[1].durationTicks == 90 &&
              snapped.clip->noteExpressions.curves.size() == 1 && snapped.clip->noteExpressions.curves[0].noteId == 18,
          "record quantization preserves the old note, overdub duration, and expression attachment");
    History history(store);
    check(history.undo().ok, "undo the overdub quantization");
    const auto raw = readClipDetails(store, 7);
    check(raw.clip && raw.clip->notes.size() == 2 && raw.clip->notes[1].startTicks == 360 &&
              history.undo().ok && digestProject(store).text == before,
          "Undo first restores the raw overdub, then removes the new note and the separate take lane");
}

void midiControllerOnlyTake() {
    Fixture fixture;
    auto& store = *fixture.store;
    store.db().exec("INSERT INTO tracks(id,kind,name) VALUES(23,'midi','Wheels')");
    const std::array points{controllers::Point{1, 120, .5}};
    const std::array<recording::MidiControllerLane, 1> wheel{{{15, controllers::kPitchBend, points}}};
    const std::array<recording::MidiLinearTake, 1> fresh{{{23, 7, 1000, 1000, "Wheel", {}, {}, false, wheel}}};
    auto seed = recording::prepareMidiLinearCommit(store, fresh, 48000);
    check(seed.ok && recording::commit(store, seed).ok, "record a new pitch-only Arrangement take");
    if (!seed.ok)
        return;
    const auto original = controllers::readLanes(store, 7);
    const auto prior = digestProject(store).text;
    const std::array moved{controllers::Point{1, 120, .125}};
    const std::array<recording::MidiControllerLane, 1> movedWheel{{{15, controllers::kPitchBend, moved}}};
    const std::array<recording::MidiLinearTake, 1> take{{{23, 8, 1000, 800, "Move", {}, {}, true, movedWheel}}};
    auto overdub = recording::prepareMidiLinearCommit(store, take, 48000);
    check(overdub.ok && recording::commit(store, overdub).ok, "overdub a pitch-only take into its existing source");
    const auto landed = controllers::readLanes(store, 7);
    check(original.ok && original.lanes.size() == 1 && landed.ok && landed.lanes.size() == 1 &&
              landed.lanes.front().id == original.lanes.front().id && landed.lanes.front().channel == 15 &&
              landed.lanes.front().points == std::vector<controllers::Point>{{1, 120, .5}, {2, 120, .125}},
          "same-tick overdub preserves the old point and orders the captured wheel value after it");
    History history(store);
    check(history.undo().ok && digestProject(store).text == prior &&
              history.redo().ok && controllers::readLanes(store, 7).lanes == landed.lanes,
          "one undo or redo restores the complete controller-only overdub");
}

void mixedLinearTake() {
    Fixture fixture;
    auto& store = *fixture.store;
    auto& db = store.db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(23,'midi','Keys');"
            "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES(8,23,'midi','Earlier MIDI',0,1000)");
    auto firstAudio = fixture.capture(false, Precision::Float32, false, 300, {}, 512);
    const std::array<recording::ClipIds, 1> firstIds{{{7, 8, "Earlier Audio", "Input Guitar"}}};
    auto first = recording::prepareCommit(store, firstAudio, firstIds);
    if (!first.ok || !recording::commit(store, first).ok)
        throw std::runtime_error("could not seed the earlier audio take");
    const auto before = digestProject(store).text;
    auto audio = fixture.capture(false, Precision::Float32, false, 100, {}, 600);
    const std::array<recording::ClipIds, 1> audioIds{{{9, 10, "New Audio", "Input Guitar"}}};
    const std::array<notes::NoteValues, 1> notes{{{.start=10, .duration=25, .id=17, .key=64, .velocity=80}}};
    const std::array<recording::MidiLinearTake, 1> midi{{{23, 10, 400, 200, "New MIDI", notes, {}}}};
    auto invalid = midi;
    invalid.front().clip = audioIds.front().clip;
    check(!recording::prepareCommit(store, audio, audioIds, invalid).ok && digestProject(store).text == before &&
              std::filesystem::is_regular_file(audio.tracks[0].file),
          "a colliding mixed take leaves the project and its private audio unmodified");
    auto prepared = recording::prepareCommit(store, audio, audioIds, midi);
    const auto committed = recording::commit(store, prepared);
    if (!committed.ok)
        throw std::runtime_error(committed.error);
    const auto count = [&](const char* sql) { return db.execAndGet(sql).getInt(); };
    check(count("SELECT count(*) FROM clips WHERE track_id=21 AND lane_id IS NULL") == 3 &&
              count("SELECT count(*) FROM clips WHERE track_id=23 AND lane_id IS NULL") == 3 &&
              count("SELECT count(*) FROM clips WHERE id=9 AND kind='audio' AND name='New Audio'") == 1 &&
              count("SELECT count(*) FROM clips WHERE id=10 AND kind='midi'") == 1,
          "one mixed splice splits both covered clips and reserves both incoming IDs before allocating the fragments");
    check(media::mediaRow(db, 8).at("rel_path") == "Samples/Recorded/Input Guitar 0001.wav" &&
              media::mediaRow(db, 10).at("rel_path") == "Samples/Recorded/Input Guitar 0002.wav",
          "audio-only and mixed takes use the same track-based file sequence and keep their clip labels");
    History history(store);
    check(history.nextUndo() && history.nextUndo()->label == "Record" && history.undo().ok &&
              digestProject(store).text == before,
          "one Undo removes the mixed take and restores the original clips whole");
    check(history.redo().ok && count("SELECT count(*) FROM clips WHERE lane_id IS NULL") == 6 &&
              count("SELECT count(*) FROM clips WHERE lane_id IS NOT NULL") == 2,
          "one Redo restores both audio and MIDI, their surviving fragments and their archived takes");
}

void midiLoopTakes() {
    constexpr std::int64_t width=5765760;
    Fixture f;
    auto& store=*f.store;
    store.db().exec("INSERT INTO tracks(id,kind,name) VALUES(23,'midi','MIDI Takes')");
    const std::array<notes::NoteValues,1> held{{{.start=3*width/4,.duration=width/2,.id=17,
        .key=60,.velocity=100,.releaseVelocity=71}}};
    std::array<std::array<notes::NoteValues,1>,3> later{};
    for(std::size_t i=0;i<later.size();++i)
        later[i][0]={.start=width/8,.duration=width/8,.id=18+i,.key=64,.velocity=90};
    const std::array<recording::MidiTakePass,4> passes{{
        {{101,201,"First"},301,width,held,held},
        {{102,202,"Second"},302,width,later[0],held},
        {{103,203,"Third"},303,width,later[1],held},
        {{104,204,"Partial"},304,width/2,later[2],held},
    }};
    const recording::MidiLoopPlacement placement{23,2*width,width,200,300,"MIDI Take"};
    const auto before=digestProject(store).text;
    auto invalid=passes;
    invalid[1].length=width/2;
    check(!recording::prepareMidiLoopCommit(store,placement,invalid).ok && digestProject(store).text==before,
          "a partial MIDI pass in the middle is refused without creating a clip");
    auto prepared=recording::prepareMidiLoopCommit(store,placement,passes);
    if(!prepared.ok)throw std::runtime_error(prepared.error);
    check(digestProject(store).text==before,"MIDI take preparation publishes no files or project rows");
    const auto commit=recording::commit(store,prepared);
    if(!commit.ok)throw std::runtime_error(commit.error);
    const auto count=[&](const char* sql){return store.db().execAndGet(sql).getInt();};
    check(count("SELECT count(*) FROM clips WHERE track_id=23 AND lane_id IS NOT NULL")==4 &&
          count("SELECT count(*) FROM clips WHERE track_id=23 AND lane_id IS NULL")==1 &&
          count("SELECT count(*) FROM lanes WHERE track_id=23 AND kind='take' AND id=101+ord")==4 &&
          count("SELECT count(*) FROM clips WHERE id=204 AND length_ticks=2882880")==1,
          "3.5 captured passes create four ordered take clips, one main clip and a half-length final take");
    const auto readSingle=[&](std::int64_t clip) {
        const auto snapshot=notes::captureNotes(store,clip);
        if(!snapshot.ok || snapshot.records().size()!=1)throw std::runtime_error("expected one stored MIDI note");
        std::string error;
        const auto decoded=notes::values(snapshot.records().front(),error);
        if(!decoded)throw std::runtime_error(error);
        return *decoded;
    };
    const auto first=readSingle(201),main=readSingle(200);
    check(first.id==17 && first.duration==width/4 && first.releaseVelocity==0 &&
          main.id==17 && main.duration==width/2 && main.releaseVelocity==71,
          "the take closes the held note at the pass end while the main copy retains its hold and real release");
    {
        const auto last=notes::captureNotes(store,204);
        auto snapped=readSingle(204);snapped.start=0;
        std::string error;
        auto replacement=notes::withValues(last.records().front(),snapped,error);
        if(!replacement)throw std::runtime_error(error);
        notes::Diff diff;
        diff.changed.push_back({last.records().front(),*replacement});
        const auto quantize=recording::prepareRecordQuantize(store,204,diff);
        History history(store);
        check(quantize.ok && notes::commitDiff(store,quantize).ok && readSingle(204).start==0 &&
              history.nextUndo() && history.nextUndo()->label=="Record Quantize",
              "record quantize commits as a distinct Undo step after the raw pass");
        check(history.undo().ok && readSingle(204).start==width/8 &&
              count("SELECT count(*) FROM clips WHERE track_id=23")==5,
              "Undo Record Quantize restores the raw note and keeps every take");
    }
    History history(store);
    for(int remaining=3;remaining>=0;--remaining) {
        const auto next=history.nextUndo();
        check(next && next->label=="Record MIDI" && history.undo().ok &&
              count("SELECT count(*) FROM clips WHERE track_id=23 AND lane_id IS NOT NULL")==remaining &&
              count("SELECT count(*) FROM clips WHERE track_id=23 AND lane_id IS NULL")== (remaining>0?1:0),
              "one Record MIDI Undo removes one take and restores the previous main image");
    }
    for(int restored=1;restored<=4;++restored)
        check(history.redo().ok && count("SELECT count(*) FROM clips WHERE track_id=23 AND lane_id IS NOT NULL")==restored,
              "Redo restores one literal MIDI pass");
    check(readSingle(200)==main && readSingle(204).start==width/8,
          "pass redo preserves note identities and the unquantized captured timing");
}

void saveAsRecoveryContract() {
    Fixture fixture;
    auto& store=*fixture.store;
    AudioRecordingSpec stale;
    stale.workFolder=fixture.temporary.path();
    stale.sampleRate=48000;stale.maxFrames=128;stale.ringFrames=4096;stale.targets={{21,{0}}};
    std::string error;
    if(!recording::configureRecovery(store,stale,error))throw std::runtime_error(error);
    Session session;
    if(!session.load(store,{},SessionSpec{2,48000,128}))throw std::runtime_error(session.error());
    const auto successor=fixture.temporary.path()/"successor.adi";
    const auto saved=store.saveAs(successor);
    if(!saved.ok || !session.refresh(store))throw std::runtime_error(saved.ok?session.error():saved.error);
    AudioRecordingSpec fresh=stale;
    if(!recording::configureRecovery(store,fresh,error))throw std::runtime_error(error);
    check(fresh.sourceProject!=stale.sourceProject && std::filesystem::equivalent(store.path(),successor),
          "Save As switches the Store to the successor and configures a distinct recording path");
    const auto finalize=[&](bool discard) {
        session.recording().stop(discard);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<deadline) {
            if(auto result=session.recording().poll(true))return *result;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("Save As recorder did not stop");
    };
    const bool acceptedStale=session.startRecording(stale,error);
    check(!acceptedStale && error.find("different project")!=std::string::npos,
          "recording options saved before Save As are refused");
    if(acceptedStale) {
        auto discarded=finalize(true);
        if(auto cleanup=discarded.discard())throw std::runtime_error(cleanup.message());
    }
    const bool acceptedFresh=session.startRecording(fresh,error);
    check(acceptedFresh,error.empty()?"fresh successor recovery options are accepted":error.c_str());
    if(!acceptedFresh)return;
    session.transport().locate(512);
    session.transport().play();
    std::array<float,128> input{},left{},right{};
    for(std::size_t i=0;i<input.size();++i)input[i]=Fixture::sample(i);
    const float* channels[]{input.data()};
    float* output[]{left.data(),right.data()};
    AudioIo io;io.in=channels;io.numIn=1;io.out=output;io.numOut=2;io.frames=128;
    session.process(io);
    auto captured=finalize(false);
    check(captured.failure==RecordingFailure::None && captured.sourceProject()==fresh.sourceProject &&
          captured.frames==input.size() && !captured.recordingId().empty(),
          "the new project's recording receipt points to the successor");
    const std::array<recording::ClipIds,1> ids={{{901,902,"After Save As"}}};
    auto ready=recording::prepareCommit(store,captured,ids);
    check(ready.ok && recording::commit(store,ready).ok && History(store).undo().ok,
          "the successor places its take and can undo it");
}
void publicationStagingOwnership() {
    namespace fs=std::filesystem;
    Fixture f;auto recorded=f.capture();const auto before=digestProject(*f.store).text;
    const auto audioFolder=media::projectFolder(*f.store)/"Samples"/"Recorded";
    const auto unrelated=audioFolder/".adi-media-unrelated";fs::create_directories(unrelated);
    const auto sentinel=unrelated/"keep.txt";{std::ofstream out(sentinel);out<<"unowned staging";}
    const auto sentinelHash=media::blake3File(sentinel),takeHash=media::blake3File(recorded.tracks[0].file);
    const std::array<recording::ClipIds,1> ids={{{7,8,"Staging"}}};
    std::vector<fs::path> owned;
    {
        const auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        check(prepared.ok && digestProject(*f.store).text==before,"prepare leaves project rows unchanged while staging publication beside project media");
        for(const auto& entry:fs::directory_iterator(audioFolder))
            if(entry.is_directory() && entry.path()!=unrelated && entry.path().filename().string().starts_with(".adi-media-"))owned.push_back(entry.path());
        check(owned.size()==1 && fs::is_regular_file(owned.front()/"asset-0.wav") &&
            media::blake3File(owned.front()/"asset-0.wav").hex==takeHash.hex,
            "the verified publication copy lives in one owned .adi-media- directory on the project filesystem");
        std::size_t captureDirectories=0;
        for(const auto& entry:fs::directory_iterator(recorded.directory()))if(entry.is_directory())++captureDirectories;
        check(captureDirectories==0,"preparation adds no publication staging beneath the recoverable capture directory");
    }
    bool cleaned=true;for(const auto& path:owned)cleaned=cleaned && !fs::exists(path);
    check(cleaned && media::blake3File(recorded.tracks[0].file).hex==takeHash.hex &&
        media::blake3File(sentinel).hex==sentinelHash.hex && digestProject(*f.store).text==before,
        "abandoning preparation removes only its publication directory and preserves the private take and unrelated stage");
    const std::array<recording::ClipIds,1> invalid={{{0,8,"Invalid"}}};
    {const auto refused=recording::prepareCommit(*f.store,recorded,invalid);check(!refused.ok,"invalid recording allocation refuses after reserving its private stage");}
    std::size_t remaining=0;for(const auto& entry:fs::directory_iterator(audioFolder))if(entry.is_directory())++remaining;
    check(remaining==1 && fs::exists(sentinel) && media::blake3File(recorded.tracks[0].file).hex==takeHash.hex,
        "failed preparation cleans its project-side stage without removing foreign directories or the capture");
    recorded.discard();
}
void crossVolumePublication(const std::filesystem::path& workFolder) {
#if defined(_WIN32)
    namespace fs=std::filesystem;
    if(!workFolder.is_absolute() || !fs::is_directory(workFolder))throw std::runtime_error("cross-volume work folder must be an existing absolute directory");
    Fixture f;auto recorded=f.capture(false,Precision::Float32,false,300,workFolder);
    const auto source=recorded.tracks[0].file;
    std::error_code error;
    auto sourceLease=media::ProjectFileLease::openShared(source,error);
    auto projectLease=media::ProjectFileLease::openShared(f.temporary.path()/"song.adi",error);
    if(!sourceLease || !projectLease || sourceLease->identity().volume==projectLease->identity().volume)
        throw std::runtime_error("cross-volume regression requires distinct native capture and project volumes");
    std::printf("CROSS_VOLUME capture=%llu project=%llu\n",static_cast<unsigned long long>(sourceLease->identity().volume),
        static_cast<unsigned long long>(projectLease->identity().volume));
    sourceLease.reset();projectLease.reset();
    const auto control=recorded.directory()/"cross-volume-control.wav",target=f.temporary.path()/"control.wav";
    fs::copy_file(source,control);
    const auto rejected=media::publishFile(control,target,{},media::PublishPolicy::AtomicOnly);
    check(rejected.status==media::PublishStatus::failed && !rejected.usedFallback && fs::exists(control) && !fs::exists(target),
        "real cross-volume AtomicOnly control refuses a direct publication and retains its source");
    fs::remove(control);
    const auto hash=media::blake3File(source);const auto before=digestProject(*f.store);
    const std::array<recording::ClipIds,1> ids={{{7,8,"Other volume"}}};
    auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    check(prepared.ok,"prepare a real recording captured on another native volume");
    const auto committed=recording::commit(*f.store,prepared);
    if(!committed.ok)std::printf("cross-volume commit: %s\n",committed.error.c_str());
    check(committed.ok && committed.seqs.size()==2,"cross-volume capture commits through project-side AtomicOnly publication as one group (media, placement)");
    if(committed.ok) {
        const auto owned=media::projectFolder(*f.store)/media::pathFromUtf8(f.store->db().execAndGet("SELECT rel_path FROM media_files").getString());
        check(owned.parent_path()==media::projectFolder(*f.store)/"Samples"/"Recorded" && media::blake3File(owned).hex==hash.hex && !fs::exists(recorded.directory()),
            "cross-volume commit preserves exact captured bytes in project audio and discards its private take");
        const auto heard=f.play(Precision::Float32);bool exact=true;
        for(std::size_t frame=0;frame<heard.size();++frame)exact=exact && heard[frame]==(frame>=512 && frame<812?Fixture::sample(frame-512):0);
        check(exact,"cross-volume recording plays its captured PCM at the original sample positions");
        const auto after=digestProject(*f.store).text;History history(*f.store);
        check(history.undo().ok && digestProject(*f.store).text==before.text && fs::exists(owned) &&
            history.redo().ok && digestProject(*f.store).text==after && f.play(Precision::Float32)==heard,
            "cross-volume recording has one undo step and restores the same retained audio on redo");
    } else recorded.discard();
#else
    (void)workFolder;throw std::runtime_error("the opt-in cross-volume native identity regression is Windows-only");
#endif
}
// Default-suite stand-in for a stage that cannot reach its destination's
// volume: the native exclusive rename refuses with the cross-device error.
struct CrossDeviceRename {
    static inline int renames=0,waits=0;
    static inline std::filesystem::path source,target;
    static std::error_code rename(const std::filesystem::path& from,const std::filesystem::path& to) {
        ++renames;source=from;target=to;
#if defined(_WIN32)
        return {17,std::system_category()}; // ERROR_NOT_SAME_DEVICE
#else
        return std::make_error_code(std::errc::cross_device_link);
#endif
    }
    static void wait(unsigned){++waits;}
};
void forcedCrossDevicePublication() {
    namespace fs=std::filesystem;
    Fixture f;auto recorded=f.capture();const auto before=digestProject(*f.store).text;
    const auto take=recorded.tracks[0].file;const auto takeHash=media::blake3File(take);
    const auto audioFolder=media::projectFolder(*f.store)/"Samples"/"Recorded";
    const std::array<recording::ClipIds,1> ids={{{7,8,"Other device"}}};
    fs::path published,staged;
    {
        auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        check(prepared.ok,"prepare a take for a forced cross-device publication");
        if(!prepared.ok)throw std::runtime_error(prepared.error);
        for(const auto& request:prepared.requests)if(request.opType=="media.import")
            published=media::projectFolder(*f.store)/media::pathFromUtf8(request.payload.at("row").at("rel_path").get<std::string>());
        prepared.publicationHooks.rename=CrossDeviceRename::rename;prepared.publicationHooks.retryWait=CrossDeviceRename::wait;
        const auto refused=recording::commit(*f.store,prepared);
        staged=CrossDeviceRename::source;
        check(!refused.ok && refused.error.find("Cannot publish recorded audio")!=std::string::npos &&
            refused.error.find("atomic publication")!=std::string::npos,
            "a cross-device refusal fails the take with a reason naming the recorded audio publication");
        check(CrossDeviceRename::renames==1 && CrossDeviceRename::waits==0 && !published.empty() && CrossDeviceRename::target==published,
            "the cross-device refusal is tried once at the owned destination and never retried");
        check(!fs::exists(published) && digestProject(*f.store).text==before &&
            f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
            f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==0,
            "refused publication leaves no destination file and no project rows");
        check(staged.parent_path().parent_path()==audioFolder && staged.parent_path().filename().string().starts_with(".adi-media-") &&
            fs::is_regular_file(staged) && media::blake3File(staged).hex==takeHash.hex,
            "the refused stage stays in its preparation's project-side directory with the take's bytes");
        check(fs::is_regular_file(take) && media::blake3File(take).hex==takeHash.hex,
            "the private take is retained for retry after a cross-device refusal");
        prepared.publicationHooks={};
        const auto retried=recording::commit(*f.store,prepared);
        if(!retried.ok)std::printf("cross-device retry: %s\n",retried.error.c_str());
        check(retried.ok && retried.seqs.size()==2 && media::blake3File(published).hex==takeHash.hex && !fs::exists(recorded.directory()),
            "the same preparation publishes its retained stage once the rename succeeds");
    }
    check(!staged.empty() && !fs::exists(staged.parent_path()) && fs::exists(published),
        "releasing the preparation removes only its own stage directory");
}
void heardPlacementAndHistory() {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture f;auto recorded=f.capture(false,precision);const auto before=digestProject(*f.store).text;
        const std::array<recording::ClipIds,1> ids={{{7,8,"Take 1"}}};auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        if(!prepared.ok)std::printf("prepare: %s\n",prepared.error.c_str());
        check(prepared.ok && digestProject(*f.store).text==before,"recording attachment captures without project writes");
        const auto committed=recording::commit(*f.store,prepared);if(!committed.ok)std::printf("commit: %s\n",committed.error.c_str());
        check(committed.ok && committed.seqs.size()==2,"recorded media and the take's placement commit as one native undo group");
        auto& db=f.store->db();const auto owned=media::projectFolder(*f.store)/media::pathFromUtf8(db.execAndGet("SELECT rel_path FROM media_files").getString());
        check(owned.filename()=="One 0001.wav" &&
              db.execAndGet("SELECT orig_name FROM media_files").getString()=="One 0001.wav",
              "the recorded WAV and media name use the track name rather than the clip label");
        check(std::filesystem::exists(owned) && !std::filesystem::exists(recorded.directory()),"successful recording publication retains owned audio and removes only private take staging");
        const auto heard=f.play(precision);bool exact=true;
        for(std::size_t frame=0;frame<heard.size();++frame)exact=exact && heard[frame]==(frame>=512 && frame<812?Fixture::sample(frame-512):0);
        check(exact,"actual recorded input is heard at exact original sample placement and duration in either graph width");
        const auto after=digestProject(*f.store).text;History history(*f.store);
        check(history.undo().ok && digestProject(*f.store).text==before && std::filesystem::exists(owned),"undo removes captured rows while retaining recorded audio for redo");
        check(history.redo().ok && digestProject(*f.store).text==after && f.play(precision)==heard,"redo restores literal media and heard PCM without recording again");
        check(!recording::commit(*f.store,prepared).ok && std::filesystem::exists(owned),"repeat commit cannot remove an already published recording");
        f.store.reset();StoreError opened{};f.store=Store::open(f.temporary.path()/"song.adi",opened);
        check(f.store && digestProject(*f.store).text==after && f.play(precision)==heard,"reopened recording history and owned media restore the same project and heard PCM");
    }
}
void numberedRecordingFiles() {
    namespace fs=std::filesystem;
    Fixture f;
    const auto folder=media::projectFolder(*f.store)/"Samples"/"Recorded";
    const auto land=[&](int frames,std::int64_t clip,std::int64_t mediaId,const std::string& trackName) {
        auto capture=f.capture(false,Precision::Float32,false,frames);
        const auto hash=media::blake3File(capture.tracks.front().file);
        const std::array<recording::ClipIds,1> ids={{{clip,mediaId,"Clip title",trackName}}};
        auto edit=recording::prepareCommit(*f.store,capture,ids);
        const auto committed=edit.ok?recording::commit(*f.store,edit):CommitResult{};
        check(committed.ok,"a track-named recording publishes through the normal commit path");
        if(!committed.ok)return fs::path{};
        const auto row=media::mediaRow(f.store->db(),mediaId);
        const auto path=media::projectFolder(*f.store)/media::pathFromUtf8(row.at("rel_path").get<std::string>());
        check(path.parent_path()==folder && row.at("orig_name")==media::pathUtf8(path.filename()) &&
              hash && media::blake3File(path).hex==hash.hex,
              "the media row has the published WAV's name, relative path and exact content");
        return path;
    };
    const auto first=land(300,7,8,"Guitar");
    const auto second=land(256,17,18,"Guitar");
    check(first.filename()=="Guitar 0001.wav" && second.filename()=="Guitar 0002.wav",
          "the displayed track name overrides the stored name and takes count upward");
    const auto occupant=folder/"gUiTaR 0003.WAV";
    {std::ofstream file(occupant);file<<"unrelated file";}
    const auto occupantHash=media::blake3File(occupant);
    const auto next=land(128,27,28,"Guitar");
    check(next.filename()=="Guitar 0004.wav" && occupantHash &&
          media::blake3File(occupant).hex==occupantHash.hex,
          "numbering skips a case-insensitive occupied name without modifying its file");

    const auto portable=land(192,37,38,"AUX .desk/Left");
    check(portable.filename()=="_AUX .desk_Left 0001.wav",
          "track names cannot make a device filename or escape Samples/Recorded");
    std::string longTrack="x";
    for(int i=0;i<110;++i)longTrack+="\xc3\xa9";
    const auto longPath=land(224,47,48,longTrack);
    const auto longName=media::pathUtf8(longPath.filename());
    constexpr std::string_view suffix=" 0001.wav";
    check(longName.size()<=200 && longName.ends_with(suffix) && longName.size()>suffix.size()+1 &&
          (longName.size()-suffix.size())%2==1 && longTrack.starts_with(longName.substr(0,longName.size()-suffix.size())),
          "a long Unicode track name keeps complete characters and room for the WAV suffix");

    if(first.empty())return;
    auto duplicate=f.capture();
    const std::array<recording::ClipIds,1> duplicateIds={{{57,58,"Another clip","Another Track"}}};
    auto reuse=recording::prepareCommit(*f.store,duplicate,duplicateIds);
    check(reuse.ok && recording::commit(*f.store,reuse).ok &&
          media::mediaRow(f.store->db(),8).at("rel_path")=="Samples/Recorded/Guitar 0001.wav" &&
          !fs::exists(folder/"Another Track 0001.wav"),
          "identical recorded content keeps the existing owned WAV and its first name");
}

void recordedTakeFollowsTempo() {
    Fixture f;
    constexpr std::int64_t changeTick = 48048;
    constexpr std::int64_t firstTick = 24024;
    constexpr std::int64_t lastTick = 60060;
    f.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120),(48048,60)");
    auto recorded = f.capture(false, Precision::Float32, false, 200, {}, 131);
    const auto sourceHash = media::blake3File(recorded.tracks.front().file);
    const std::array<recording::ClipIds, 1> ids{{{7, 8, "Follow tempo"}}};
    auto prepared = recording::prepareCommit(*f.store, recorded, ids, 31);
    if (!recording::commit(*f.store, prepared).ok)
        throw std::runtime_error("could not publish the tempo-following recording");
    const auto original = SnapshotBuilder::fromStore(*f.store);
    const auto& clip = *original->findTrack(21)->clips.front();
    check(clip.posTicks == firstTick && clip.lengthTicks == lastTick - firstTick && clip.audio &&
              clip.audio->warpEnabled && clip.audio->warpMarkers && clip.audio->sourceStartFrames == 0 &&
              clip.audio->sourceLengthFrames == 200,
          "SC-248: a compensated recording starts at its original musical position with Warp on");
    if (!clip.audio || !clip.audio->warpMarkers)
        return;
    const auto map = audio::readWarpMap(*clip.audio->warpMarkers, 200);
    check(map && map.markers == std::vector<WarpMarker>{{0, 0}, {100, changeTick - firstTick},
                                                        {200, lastTick - firstTick}},
          "SC-248: the take's warp map retains the tempo change inside its captured source");
    const auto landed = f.play(Precision::Float32);
    bool unchanged = true;
    for (std::size_t frame = 0; frame < landed.size(); ++frame)
        unchanged = unchanged && landed[frame] == (frame >= 100 && frame < 300 ? Fixture::sample(frame - 100) : 0.f);
    check(unchanged, "SC-248: at the capture tempo the entire WAV plays at its compensated sample positions");

    struct TempoCase { int first, second; std::size_t begin, middle, end; };
    for (const auto example : {TempoCase{240, 120, 50, 100, 150}, TempoCase{60, 30, 200, 400, 600}}) {
        SQLite::Statement set(f.store->db(), "UPDATE tempo_map SET bpm=? WHERE pos_ticks=?");
        set.bind(1, example.first);
        set.bind(2, 0);
        set.exec();
        set.reset();
        set.bind(1, example.second);
        set.bind(2, changeTick);
        set.exec();
        const auto current = SnapshotBuilder::fromStore(*f.store);
        const auto& take = *current->findTrack(21)->clips.front();
        const auto sampleAt = [&](std::int64_t tick) {
            return static_cast<std::size_t>(std::llround(current->tempo->ticksToSeconds(tick) * 48000));
        };
        const auto heard = f.play(Precision::Float32);
        const bool silentOutside = std::all_of(heard.begin(), heard.begin() + static_cast<std::ptrdiff_t>(example.begin),
                                               [](double value) { return value == 0; }) &&
            std::all_of(heard.begin() + static_cast<std::ptrdiff_t>(example.end), heard.end(),
                        [](double value) { return value == 0; });
        const bool audibleTail = std::any_of(heard.begin() + static_cast<std::ptrdiff_t>(example.end - 8),
                                             heard.begin() + static_cast<std::ptrdiff_t>(example.end),
                                             [](double value) { return value != 0; });
        check(sampleAt(take.posTicks) == example.begin && sampleAt(changeTick) == example.middle &&
                  sampleAt(take.posTicks + take.lengthTicks) == example.end &&
                  heard[example.begin] != 0 && audibleTail && silentOutside,
              "SC-248: the take keeps its musical extent and audible tail at faster and slower tempos");
    }
    std::string why;
    const auto file = media::resolveMedia(media::projectFolder(*f.store), media::mediaRow(f.store->db(), 8), why);
    const auto publishedHash = media::blake3File(file);
    check(sourceHash && publishedHash && sourceHash.hex == publishedHash.hex,
          "SC-248: changing tempo leaves the shared recording file byte-identical");
}

void unwarpedRecordingTempoChanges() {
    constexpr std::int64_t at120 = 256;
    Fixture f;
    f.store->db().exec("INSERT INTO tempo_map(pos_ticks,bpm) VALUES(0,120)");
    auto recorded = f.capture(false, Precision::Float32, false, 300, {}, at120);
    const std::array<recording::ClipIds, 1> ids{{{7, 8, "Unwarped take"}}};
    auto prepared = recording::prepareCommit(*f.store, recorded, ids);
    if (!recording::commit(*f.store, prepared).ok)
        throw std::runtime_error("could not publish the recording");
    // Recorded takes follow the Set by default (SC-248). Turn Warp off
    // through the Clip View command to exercise SC-11.
    clip_edit::WarpOptions warp;
    warp.enabled = false;
    const auto unwarped = clip_edit::prepareSetWarp(*f.store, 7, warp, 48000);
    if (!unwarped.ok || !clip_edit::commitPrepared(*f.store, unwarped).ok)
        throw std::runtime_error("could not disable Warp on the recorded take: " + unwarped.error);
    for (int bpm : {240, 60}) {
        std::array<OpRequest, 2> change;
        change[0].opType = "project.removeTempoEvent";
        change[0].payload = {{"pos", 0}};
        change[1].opType = "project.insertTempoEvent";
        change[1].payload = {{"pos", 0}, {"bpm", bpm}};
        const auto lengthOf = [&] {
            return f.store->db().execAndGet("SELECT length_ticks FROM clips WHERE id=7").getInt64();
        };
        const auto lengthBefore = lengthOf();
        const auto rowsBefore = digestProject(*f.store).text;
        if (!OpJournal(*f.store).commit(change).ok)
            throw std::runtime_error("could not change the recording's tempo");
        // The tempo change rewrites the take's length beside the tempo row: one step, both ways.
        const auto lengthAfter = lengthOf();
        const auto rowsAfter = digestProject(*f.store).text;
        const auto undone = History(*f.store).undo();
        const bool restored = undone.ok && lengthOf() == lengthBefore && digestProject(*f.store).text == rowsBefore;
        const auto redone = History(*f.store).redo();
        check(lengthAfter != lengthBefore && restored && redone.ok && lengthOf() == lengthAfter &&
                  digestProject(*f.store).text == rowsAfter,
              bpm == 240 ? "SC-11: one Undo and Redo of a faster tempo restore the unwarped take's length and every row"
                         : "SC-11: one Undo and Redo of a slower tempo restore the unwarped take's length and every row");
        const auto snapshot = SnapshotBuilder::fromStore(*f.store);
        const auto& clip = *snapshot->findTrack(21)->clips.front();
        const auto first = static_cast<std::size_t>(at120 * 120 / bpm);
        const auto last = first + 300;
        bool exact = std::llround(snapshot->tempo->ticksToSeconds(clip.posTicks + clip.lengthTicks) * 48000)
            == static_cast<std::int64_t>(last);
        const auto heard = f.play(Precision::Float32);
        for (std::size_t frame = 0; frame < heard.size(); ++frame)
            exact = exact && heard[frame] == (frame >= first && frame < last ? Fixture::sample(frame - first) : 0.0f);
        check(exact, bpm == 240
            ? "a faster tempo preserves the recorded take's complete PCM and endpoint"
            : "a slower tempo preserves the recorded take's duration without a silent tail");
    }
}

// walk-B B6: a take recorded over a clip replaces what it covers in the main lane, as Live does, in the
// take's one Undo step; nothing is left under it to play.
void linearTakeReplacesWhatItCovers() {
    Fixture f;
    auto first=f.capture(false,Precision::Float32,false,300,{},512); // [512,812)
    const std::array<recording::ClipIds,1> firstIds={{{7,8,"First"}}};
    auto prepared=recording::prepareCommit(*f.store,first,firstIds);
    check(prepared.ok && recording::commit(*f.store,prepared).ok,"the first take lands");
    const auto before=digestProject(*f.store).text;
    auto second=f.capture(false,Precision::Float32,false,100,{},600); // [600,700), inside the first
    const std::array<recording::ClipIds,1> secondIds={{{17,18,"Second"}}};
    prepared=recording::prepareCommit(*f.store,second,secondIds);
    if(!prepared.ok)std::printf("prepare: %s\n",prepared.error.c_str());
    const auto committed=prepared.ok?recording::commit(*f.store,prepared):CommitResult{};
    check(committed.ok,"a take over a clip commits");
    auto& db=f.store->db();
    const auto count=[&](const char* sql){return db.execAndGet(sql).getInt();};
    check(count("SELECT count(*) FROM clips WHERE track_id=21 AND lane_id IS NULL")==3,
          "the covered clip is split around the new take: left part, take, right part");
    check(count("SELECT count(*) FROM clips a JOIN clips b ON a.id<b.id AND a.track_id=b.track_id "
                "AND a.lane_id IS NULL AND b.lane_id IS NULL "
                "WHERE a.pos_ticks<b.pos_ticks+b.length_ticks AND b.pos_ticks<a.pos_ticks+a.length_ticks")==0,
          "no two clips of the main lane overlap after the take");
    check(count("SELECT count(*) FROM clips WHERE id=17 AND name='Second'")==1 &&
          count("SELECT count(*) FROM clips WHERE id=7")==1,
          "the take keeps its id and name; the left part keeps the covered clip's id");
    check(count("SELECT count(*) FROM clips c JOIN lanes l ON l.id=c.lane_id "
                "JOIN audio_clips a ON a.clip_id=c.id "
                "WHERE l.track_id=21 AND c.name='First' AND a.media_id=8 AND a.src_start_frames=0 "
                "AND c.pos_ticks=(SELECT pos_ticks FROM clips WHERE id=7) "
                "AND c.pos_ticks+c.length_ticks=(SELECT max(pos_ticks+length_ticks) FROM clips WHERE lane_id IS NULL)") == 1,
          "SC-252: the full earlier recording is retained on a take lane with the original source");
    const auto heard=f.play(Precision::Float32);
    bool exact=true;
    for(std::size_t frame=0;frame<heard.size();++frame) {
        const auto expected=frame>=600 && frame<700 ? Fixture::sample(frame-600)
                          : frame>=512 && frame<812 ? Fixture::sample(frame-512) : 0.0f;
        exact=exact && heard[frame]==expected;
    }
    check(exact,"the take is heard where it was recorded and the first take around it, never both at once");
    check(History(*f.store).undo().ok && digestProject(*f.store).text==before,
          "one Undo restores the covered clip whole");
}
void latencyCompensationKeepsPunchProtection() {
    constexpr std::int64_t punchIn=512,punchOut=640,oldStart=416,oldEnd=704,streamStart=480;
    struct Placement {std::int64_t correction, start, end, source;};
    for(const auto placement : {Placement{32,512,608,32},Placement{-32,544,640,0}}) {
        Fixture f;
        auto old=f.capture(false,Precision::Float32,false,static_cast<int>(oldEnd-oldStart),{},oldStart);
        const std::array<recording::ClipIds,1> oldIds{{{7,8,"Before punch"}}};
        auto first=recording::prepareCommit(*f.store,old,oldIds);
        if(!recording::commit(*f.store,first).ok)
            throw std::runtime_error("cannot land the surrounding clip");

        const auto tempo=SnapshotBuilder::fromStore(*f.store)->tempo;
        const auto inTick=tempo->secondsToTicks(static_cast<double>(punchIn)/48000);
        const auto outTick=tempo->secondsToTicks(static_cast<double>(punchOut)/48000);
        OpRequest brace;
        brace.opType="transport.setLoop";
        brace.payload={{"on",false},{"start",inTick},{"end",outTick}};
        OpRequest punch;
        punch.opType="transport.setPunch";
        punch.payload={{"in",true},{"out",true}};
        if(!OpJournal(*f.store).commit(brace).ok || !OpJournal(*f.store).commit(punch).ok)
            throw std::runtime_error("cannot set the punch brace");
        const auto before=digestProject(*f.store).text;
        auto take=f.capture(false,Precision::Float32,false,192,{},streamStart);
        const RecordingPunchWindow bounds{RecordingPunchWindow::Boundary{punchIn,inTick},
                                          RecordingPunchWindow::Boundary{punchOut,outTick}};
        check(take.startSample==punchIn && take.frames==static_cast<std::uint64_t>(punchOut-punchIn) && take.punchWindow()==bounds,
              "the take remembers both authored punch edges from its render clock");
        const auto raw=media::blake3File(take.tracks.front().file);
        const std::array<recording::ClipIds,1> ids{{{17,18,"Punched take"}}};
        auto outside=recording::prepareCommit(*f.store,take,ids,256);
        check(!outside.ok && std::filesystem::is_regular_file(take.tracks.front().file) &&
                  digestProject(*f.store).text==before,
              "a correction outside the brace retains the raw WAV and leaves the project untouched");
        auto prepared=recording::prepareCommit(*f.store,take,ids,placement.correction);
        const auto committed=recording::commit(*f.store,prepared);
        if(!committed.ok)
            throw std::runtime_error("cannot land the punched take: "+committed.error);
        auto &db=f.store->db();
        SQLite::Statement clip(db,"SELECT c.pos_ticks,c.length_ticks,a.src_start_frames,a.src_len_frames, "
                                  "a.warp_enabled,a.warp_mode,a.warp_markers "
                                  "FROM clips c JOIN audio_clips a ON a.clip_id=c.id WHERE c.id=17");
        const auto startTick=tempo->secondsToTicks(static_cast<double>(placement.start)/48000);
        const auto endTick=tempo->secondsToTicks(static_cast<double>(placement.end)/48000);
        const auto length=placement.end-placement.start;
        const bool hasClip=clip.executeStep();
        check(hasClip && clip.getColumn(0).getInt64()==startTick && clip.getColumn(1).getInt64()==endTick-startTick &&
                  clip.getColumn(2).getInt64()==placement.source && clip.getColumn(3).getInt64()==length,
              "the clip uses the compensated intersection and its exact source interval");
        bool hasClippedWarp=false;
        if (hasClip && clip.getColumn(4).getInt()==1 && clip.getColumn(5).getString()=="beats" && !clip.getColumn(6).isNull()) {
            const auto blob=clip.getColumn(6);
            const auto map=audio::readWarpMap({static_cast<const std::byte*>(blob.getBlob()),
                                               static_cast<std::size_t>(blob.getBytes())},
                                              static_cast<std::uint64_t>(length));
            hasClippedWarp=map && map.markers==std::vector<WarpMarker>{{0,0},{length,endTick-startTick}};
        }
        check(hasClippedWarp,"Beats Warp anchors are relative to the clipped source window");
        clip.reset();
        std::string why;
        const auto saved=media::resolveMedia(media::projectFolder(*f.store),media::mediaRow(db,18),why);
        const auto published=media::blake3File(saved);
        check(raw && published && raw.hex==published.hex &&
                  db.execAndGet("SELECT frames FROM media_files WHERE id=18").getInt64()==punchOut-punchIn,
              "the published WAV keeps the entire captured interval byte for byte");

        const auto heard=f.play(Precision::Float32);
        bool exact=true;
        for(std::size_t frame=0;frame<heard.size();++frame) {
            const auto at=static_cast<std::int64_t>(frame);
            const auto expected=at>=placement.start && at<placement.end
                ? Fixture::sample(static_cast<std::size_t>(punchIn-streamStart+placement.source+at-placement.start))
                : at>=oldStart && at<oldEnd ? Fixture::sample(static_cast<std::size_t>(at-oldStart)) : 0.0f;
            exact=exact && heard[frame]==expected;
        }
        check(exact,"both correction signs preserve the audible surrounding clip and shifted samples");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before &&
                  History(*f.store).redo().ok && std::filesystem::is_regular_file(saved),
              "Undo and Redo replay the captured clip edit without replacing its WAV");
    }
}
void existingMediaAndHistoryPaths() {
    for(const bool sourceAlreadyInPool:{false,true}) {
        Fixture f;auto recorded=f.capture();const auto external=f.temporary.path()/"external.wav";
        std::filesystem::copy_file(recorded.tracks[0].file,external);
        const auto priorPath=sourceAlreadyInPool?recorded.tracks[0].file:external;
        check(media::importMedia(*f.store,priorPath,8,0).ok,"prepare existing media with the recording's content identity");
        const auto before=digestProject(*f.store).text;const std::array<recording::ClipIds,1> ids={{{7,18,"Reuse"}}};auto prepared=recording::prepareCommit(*f.store,recorded,ids);
        check(prepared.ok && recording::commit(*f.store,prepared).ok && f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==1,"recording reuses existing media identity while ensuring an owned playable asset");
        std::string error;const auto owned=media::resolveMedia(media::projectFolder(*f.store),media::mediaRow(f.store->db(),8),error);
        check(!owned.empty() && owned.parent_path()==media::projectFolder(*f.store)/"Samples"/"Recorded" && std::filesystem::exists(owned),"existing external content is attached through a verified owned recording file");
        check(History(*f.store).undo().ok && digestProject(*f.store).text==before && std::filesystem::exists(priorPath),"undo restores exact prior media paths with their original files still available");
        recorded.discard();
    }
}
bool rollbackReported(const CommitResult& result,const std::filesystem::path& path) {
    return result.error.find("rollback")!=std::string::npos &&
        result.error.find(media::pathUtf8(path))!=std::string::npos;
}
struct PublicationCommitVeto {
    std::filesystem::path published,moved;
    std::optional<media::FileIdentity> originalIdentity,foreignIdentity;
    std::exception_ptr exception;
    unsigned calls=0;
    static constexpr const char* foreignBytes="later writer owns this path\n";
    static int invoke(void* raw)noexcept {
        auto& self=*static_cast<PublicationCommitVeto*>(raw);++self.calls;
        try {
            // This hook executes at SQLite's actual COMMIT, after the journal's
            // beforeCommit publication. Do not execute SQLite from this hook.
            std::error_code error;
            self.originalIdentity=media::readFileIdentity(self.published,error);
            if(!self.originalIdentity || !std::filesystem::is_regular_file(std::filesystem::symlink_status(self.published)))
                throw std::runtime_error("commit hook did not observe the newly published regular WAV");
            std::filesystem::rename(self.published,self.moved);
            {std::ofstream foreign;foreign.exceptions(std::ios::badbit|std::ios::failbit);
                foreign.open(self.published,std::ios::binary);foreign<<foreignBytes;foreign.close();}
            self.foreignIdentity=media::readFileIdentity(self.published,error);
            if(!self.foreignIdentity || self.foreignIdentity==self.originalIdentity)
                throw std::runtime_error("commit hook failed to create a distinct later file identity");
        }catch(...){self.exception=std::current_exception();}
        return 1; // Veto COMMIT even if test setup failed; never unwind through C.
    }
};
struct ScopedPublicationCommitVeto {
    sqlite3* database;
    ScopedPublicationCommitVeto(SQLite::Database& db,PublicationCommitVeto& state):database(db.getHandle()) {
        // Fixture owns this new Store and has installed no previous commit hook.
        sqlite3_commit_hook(database,&PublicationCommitVeto::invoke,&state);
    }
    ~ScopedPublicationCommitVeto(){sqlite3_commit_hook(database,nullptr,nullptr);}
    ScopedPublicationCommitVeto(const ScopedPublicationCommitVeto&)=delete;
    ScopedPublicationCommitVeto& operator=(const ScopedPublicationCommitVeto&)=delete;
};
void rollbackNeverDeletesLaterWriter() {
    namespace fs=std::filesystem;
    Fixture f;auto recorded=f.capture();const auto before=digestProject(*f.store).text;
    const auto privateSource=recorded.tracks[0].file;
    const auto sourceHash=media::blake3File(privateSource);
    const std::array<recording::ClipIds,1> ids={{{7,8,"Late replacement"}}};
    auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    check(prepared.ok,"prepare actual captured WAV for commit-veto identity regression");
    if(!prepared.ok)throw std::runtime_error(prepared.error);
    PublicationCommitVeto veto;
    for(const auto& request:prepared.requests)if(request.opType=="media.import")
        veto.published=media::projectFolder(*f.store)/media::pathFromUtf8(request.payload.at("row").at("rel_path").get<std::string>());
    if(veto.published.empty())throw std::runtime_error("recording fixture has no captured publication path");
    veto.moved=veto.published.parent_path()/"moved-owned-recording.wav";
    const auto reference=f.temporary.path()/"foreign-reference.txt";
    {std::ofstream foreign(reference,std::ios::binary);foreign<<PublicationCommitVeto::foreignBytes;}
    const auto foreignHash=media::blake3File(reference);
    CommitResult failed;
    {ScopedPublicationCommitVeto hook(f.store->db(),veto);failed=recording::commit(*f.store,prepared);}
    check(veto.calls==1 && !veto.exception,"actual SQLite commit hook replaced a published WAV before vetoing once");
    if(veto.exception)std::rethrow_exception(veto.exception);
    check(!failed.ok && digestProject(*f.store).text==before &&
        f.store->db().execAndGet("SELECT count(*) FROM media_files").getInt()==0 &&
        f.store->db().execAndGet("SELECT count(*) FROM clips").getInt()==0,
        "commit veto rolls back all recording/media/journal rows despite completed file publication");
    const auto foreignAfter=media::blake3File(veto.published),movedAfter=media::blake3File(veto.moved);
    check(foreignAfter && foreignAfter.hex==foreignHash.hex,"recording rollback retains exact bytes of the later foreign ordinary file");
    std::error_code foreignError,movedError;
    const auto currentForeign=media::readFileIdentity(veto.published,foreignError);
    const auto currentMoved=media::readFileIdentity(veto.moved,movedError);
    check(currentForeign && currentForeign==veto.foreignIdentity,"recording rollback does not remove or replace the later writer's file identity");
    check(movedAfter && movedAfter.hex==sourceHash.hex && currentMoved && currentMoved==veto.originalIdentity,
        "recording rollback never searches for or deletes its moved original publication");
    check(rollbackReported(failed,veto.published),"failed recording commit reports the retained or changed publication path");
    check(fs::is_regular_file(privateSource) && media::blake3File(privateSource).hex==sourceHash.hex,
        "commit-veto identity conflict preserves the original private capture for recovery");
    recorded.discard();
}

void lateSqlAndOwnedReuse() {
    Fixture f;auto recorded=f.capture();const std::array<recording::ClipIds,1> ids={{{7,8,"One"}}};auto& db=f.store->db();
    const auto publicationTarget=[&](const recording::RecordingEdit& prepared) {
        for(const auto& request:prepared.requests)
            if(request.opType=="media.import")
                return media::projectFolder(*f.store)/
                       media::pathFromUtf8(request.payload.at("row").at("rel_path").get<std::string>());
        throw std::runtime_error("recording fixture has no captured publication path");
    };
    db.exec("CREATE TABLE late_parent(id INTEGER PRIMARY KEY);CREATE TABLE late_child(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES late_parent(id) DEFERRABLE INITIALLY DEFERRED);CREATE TRIGGER reject_recording AFTER INSERT ON audio_clips BEGIN INSERT INTO late_child(id,parent) VALUES(NEW.clip_id,999);END");
    const auto before=digestProject(*f.store).text;auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    auto published=publicationTarget(prepared);
    const auto failed=recording::commit(*f.store,prepared);
    const bool retained=std::filesystem::exists(published);
#if defined(_WIN32)
    const bool cleanupAccepted=!retained;
#else
    // POSIX pathname unlink cannot atomically compare-and-remove an identity.
    const auto sourceHash=media::blake3File(recorded.tracks[0].file);
    const bool cleanupAccepted=retained && sourceHash && media::blake3File(published).hex==sourceHash.hex && rollbackReported(failed,published);
#endif
    check(prepared.ok && !failed.ok && digestProject(*f.store).text==before && cleanupAccepted && std::filesystem::exists(recorded.tracks[0].file),"late SQL failure rolls back rows, safely cleans or reports retained owned publication, and preserves the private take");
    db.exec("DROP TRIGGER reject_recording;DROP TABLE late_child;DROP TABLE late_parent");
    prepared=recording::prepareCommit(*f.store,recorded,ids);published=publicationTarget(prepared);
    const auto retried=recording::commit(*f.store,prepared);check(retried.ok,"retry the same completed take after fixing the SQL failure");
    const auto originalHash=media::blake3File(published);check(History(*f.store).undo().ok && std::filesystem::exists(published),"undo keeps the owned recording available to a later recording branch");
    auto repeated=f.capture();db.exec("CREATE TABLE late_parent(id INTEGER PRIMARY KEY);CREATE TABLE late_child(id INTEGER PRIMARY KEY,parent INTEGER REFERENCES late_parent(id) DEFERRABLE INITIALLY DEFERRED);CREATE TRIGGER reject_recording AFTER INSERT ON audio_clips BEGIN INSERT INTO late_child(id,parent) VALUES(NEW.clip_id,999);END");
    const auto reusedBefore=digestProject(*f.store).text;auto reuse=recording::prepareCommit(*f.store,repeated,ids);
    const auto reusedPath=publicationTarget(reuse);
    std::filesystem::copy_file(repeated.tracks[0].file,reusedPath);
    check(reuse.ok && !recording::commit(*f.store,reuse).ok && digestProject(*f.store).text==reusedBefore &&
          media::blake3File(reusedPath).hex==originalHash.hex && media::blake3File(published).hex==originalHash.hex &&
          std::filesystem::exists(repeated.tracks[0].file),
          "failed SQL after reusing an identical late arrival keeps both old WAVs and the private take");
    db.exec("DROP TRIGGER reject_recording;DROP TABLE late_child;DROP TABLE late_parent");
    auto collision=recording::prepareCommit(*f.store,repeated,ids);
    const auto occupied=publicationTarget(collision);
    {std::ofstream foreign(occupied,std::ios::binary);foreign<<"different content";}
    const auto foreignHash=media::blake3File(occupied);
    check(collision.ok && !recording::commit(*f.store,collision).ok && media::blake3File(occupied).hex==foreignHash.hex &&
          std::filesystem::exists(repeated.tracks[0].file),
          "recording publication never overwrites a different file that arrives after preparation");
    repeated.discard();
}
void groupDedupAndGuards() {
    Fixture f;auto recorded=f.capture(true);const std::array<recording::ClipIds,2> ids={{{7,8,"One"},{9,10,"Two"}}};auto prepared=recording::prepareCommit(*f.store,recorded,ids);
    check(prepared.ok,"prepare a two-track recording before media rows exist");
    const auto committed=recording::commit(*f.store,prepared);check(committed.ok && committed.seqs.size()==2,"identical simultaneous takes deduplicate inside the captured batch: one media row, one placement");
    auto& db=f.store->db();check(db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 && db.execAndGet("SELECT count(*) FROM audio_clips WHERE media_id=8").getInt()==2,"both recorded clips reference the one captured content identity");
    check(History(*f.store).undo().ok && db.execAndGet("SELECT count(*) FROM clips").getInt()==0,"one undo removes every track in the recording pass");
    auto another=f.capture();const std::array<recording::ClipIds,1> one={{{17,18,"Another"}}};auto stale=recording::prepareCommit(*f.store,another,one);
    OpRequest rename;rename.opType="track.rename";rename.payload={{"id",21},{"name","Changed"}};
    check(OpJournal(*f.store).commit(rename).ok && !recording::commit(*f.store,stale).ok && std::filesystem::exists(another.tracks[0].file),"stale attachment rejects without losing the private recording");
    Fixture other;check(!recording::prepareCommit(*other.store,another,one).ok,"another project's connection cannot attach a stale document recording");
    auto changed=recording::prepareCommit(*f.store,another,one);const auto before=digestProject(*f.store).text;
    {std::ofstream bytes(another.tracks[0].file,std::ios::binary|std::ios::app);bytes<<"changed";}
    check(changed.ok && !recording::commit(*f.store,changed).ok && digestProject(*f.store).text==before && std::filesystem::exists(another.tracks[0].file),"changed take bytes roll back the complete attachment while preserving recovery files");another.discard();
}
void stagedPublicationAndRefresh() {
    Fixture f;
    auto recorded = f.capture(true);
    const auto folder = media::projectFolder(*f.store);
    const auto staged = std::async(std::launch::async, [&] { return recording::stageTake(recorded, folder); }).get();
    check(staged.ok, "staging worker verifies the closed native WAVs");
    if (!staged.ok) return;
    std::size_t stagedCopies = 0;
    for (const auto& stage : std::filesystem::directory_iterator(folder / "Samples" / "Recorded"))
        if (stage.is_directory())
            for (const auto& file : std::filesystem::directory_iterator(stage.path())) stagedCopies += file.is_regular_file();
    check(stagedCopies == 1, "equal tracks make one verified project-side staging copy");
    const std::array initial{recording::ClipIds{7,8,"One","Displayed Track"},recording::ClipIds{9,10,"Two"}};
    const auto before = digestProject(*f.store).text;
    auto edit = recording::prepareStagedCommit(*f.store, staged, initial);
    check(edit.ok && !recording::commit(*f.store, edit).ok, "owner requires completed worker publication before SQL");
    if (!edit.ok) return;
    const bool published = std::async(std::launch::async, [&] { return recording::publishStagedTake(edit); }).get();
    check(published && digestProject(*f.store).text == before, "publication worker never changes project rows");
    if (!published) return;
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id",21},{"name","Renamed during publication"}};
    const std::array rebased{recording::ClipIds{17,8,"Renamed","Renamed during publication"},recording::ClipIds{19,10,"Two"}};
    check(OpJournal(*f.store).commit(rename).ok &&
          recording::refreshStagedCommit(*f.store, staged, edit, rebased) == recording::StagedRefresh::ready,
          "owner rebases the edit when the head changes and the published media binding is stable");
    const auto committed = recording::commit(*f.store, edit);
    auto& db = f.store->db();
    check(committed.ok && db.execAndGet("SELECT count(*) FROM media_files").getInt() == 1 &&
          db.execAndGet("SELECT count(*) FROM audio_clips WHERE clip_id IN (17,19) AND media_id=8").getInt() == 2 &&
          media::mediaRow(db, 8).at("rel_path") == "Samples/Recorded/Displayed Track 0001.wav" &&
          media::mediaRow(db, 8).at("orig_name") == "Displayed Track 0001.wav",
          "one atomic project edit lands the current clips and reuses the published media");
    check(std::filesystem::exists(recorded.tracks.front().file), "staged commit leaves private cleanup to the worker");
    const auto cleanup = std::async(std::launch::async, [&] { return recording::cleanupStagedTake(edit); }).get();
    check(cleanup.empty() && !std::filesystem::exists(recorded.tracks.front().file) && History(*f.store).undo().ok &&
          db.execAndGet("SELECT count(*) FROM clips").getInt() == 0 &&
          db.execAndGet("SELECT name FROM tracks WHERE id=21").getString() == "Renamed during publication",
          "one Undo removes this take and preserves the intervening edit");

    if (!committed.ok || !History(*f.store).redo().ok) return;
    auto duplicate = f.capture();
    const auto duplicateStage = recording::stageTake(duplicate, folder);
    const std::array repeated{recording::ClipIds{27,28,"Repeat","Other track name"}};
    auto reuse = recording::prepareStagedCommit(*f.store, duplicateStage, repeated);
    check(reuse.ok && recording::publishStagedTake(reuse) &&
          std::none_of(reuse.requests.begin(), reuse.requests.end(), [](const auto &request) {
              return request.opType == "media.import" || request.opType == "media.relink";
          }) && recording::commit(*f.store, reuse).ok && recording::cleanupStagedTake(reuse).empty() &&
          media::mediaRow(db, 8).at("rel_path") == "Samples/Recorded/Displayed Track 0001.wav" &&
          !std::filesystem::exists(folder / "Samples" / "Recorded" / "Other track name 0001.wav"),
          "the publication worker verifies and reuses an identical owned WAV without renaming it");
}

void stagedPublicationRefusesChangedContent() {
    for (const bool bindingChanged : {false,true}) {
        Fixture f;
        auto recorded = f.capture();
        const auto folder = media::projectFolder(*f.store);
        const auto staged = recording::stageTake(recorded, folder);
        const auto hash = media::blake3File(recorded.tracks.front().file);
        const std::array ids{recording::ClipIds{7,8,"One"}};
        auto edit = recording::prepareStagedCommit(*f.store, staged, ids);
        const auto published = edit.ok && recording::publishStagedTake(edit);
        check(published && hash, "publish a staging proof before changing the source or binding");
        if (!published || !hash) continue;
        if (bindingChanged) {
            OpRequest rename;
            rename.opType = "track.rename";
            rename.payload = {{"id",21},{"name","Renamed during publication"}};
            const std::array changed{recording::ClipIds{9,10,"One"}};
            check(OpJournal(*f.store).commit(rename).ok &&
                  recording::refreshStagedCommit(*f.store, staged, edit, changed) == recording::StagedRefresh::restage &&
                  std::filesystem::is_regular_file(recorded.tracks.front().file),
                  "a changed media binding requests restaging and retains the original take");
        } else {
            const auto target = folder / "Samples" / "Recorded" / "One 0001.wav";
            const auto modified = std::filesystem::last_write_time(target);
            const auto before = digestProject(*f.store).text;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            {
                std::fstream changed(target, std::ios::in | std::ios::out | std::ios::binary);
                changed.seekp(-1, std::ios::end);
                changed.put('\x7f');
                check(static_cast<bool>(changed), "modify one published byte without changing file size");
            }
            std::filesystem::last_write_time(target, modified);
            check(!recording::commit(*f.store, edit).ok && digestProject(*f.store).text == before &&
                  std::filesystem::is_regular_file(recorded.tracks.front().file),
                  "owner rejects in-place changes even when mtime and length are restored");
        }
    }
}
void loopPublicationRefusesFlattening() {
    for(const auto frames:{32,300}) {
        Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,frames);
        check(recorded.failure==RecordingFailure::None && recorded.loopSpec() && recorded.passes().size()==(frames==32?1u:3u),"actual Session supplies valid partial-single and multiple-pass loop sources");
        const auto before=digestProject(*fixture.store).text;
        const std::array<recording::ClipIds,1> ids={{{7,8,"Loop"}}};
        const auto edit=recording::prepareCommit(*fixture.store,recorded,ids);
        check(!edit.ok && edit.requests.empty() && edit.error.find("take-pass publication")!=std::string::npos,"linear materializer explicitly refuses opted-in loop captures, even a single partial pass");
        check(digestProject(*fixture.store).text==before && std::filesystem::exists(recorded.tracks[0].file),"refused loop publication leaves project and recoverable continuous source unchanged");
        recorded.discard();
    }
}
std::vector<recording::LoopTrackIds> loopIds(const AudioRecordingResult& recorded) {
    std::vector<recording::LoopTrackIds> result;
    for(std::size_t track=0;track<recorded.tracks.size();++track) {
        recording::LoopTrackIds ids;ids.media=300+static_cast<std::int64_t>(track);
        for(std::size_t pass=0;pass<recorded.passes().size();++pass) {
            const auto offset=static_cast<std::int64_t>(track*16+pass);
            ids.passes.push_back({100+offset,200+offset,{}});
        }
        result.push_back(std::move(ids));
    }
    return result;
}
std::vector<double> loopOracle(int frames,std::int64_t start,std::vector<double> initial=std::vector<double>(1024)) {
    auto at=start;
    for(int source=0;source<frames;++source) {
        if(at>=640)at=512+(at-640)%128;
        initial.at(static_cast<std::size_t>(at))=Fixture::sample(static_cast<std::size_t>(source));++at;
    }
    return initial;
}
void loopPassesAndHistory(const std::filesystem::path& workFolder={}) {
    for(const auto precision:{Precision::Float32,Precision::Float64}) {
        Fixture fixture;auto recorded=fixture.capture(false,precision,true,448,workFolder);
        check(recorded.failure==RecordingFailure::None && recorded.passes().size()==4,"actual Session finishes one source containing three and a half loop passes");
        const auto before=digestProject(*fixture.store).text;const auto ids=loopIds(recorded);
        const std::array<std::optional<Payload>,4> selections={Payload{{"clips",{91}}},Payload{{"clips",{92}}},Payload{{"clips",{93}}},Payload{{"clips",{94}}}};
        auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids,selections);
        if(!prepared.ok)std::printf("loop prepare: %s\n",prepared.error.c_str());
        check(prepared.ok && digestProject(*fixture.store).text==before,"loop preparation stages source once without changing project rows");
        if(!prepared.ok)continue;
        SQLite::Database outside((fixture.temporary.path()/"song.adi").string(),SQLite::OPEN_READONLY);
        int hooks=0;
        prepared.options.beforeCommit=[&](SQLite::Database& db,std::int64_t,std::string&) {
            ++hooks;
            return db.execAndGet("SELECT count(*) FROM lanes").getInt()==4 &&
                db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 &&
                outside.execAndGet("SELECT count(*) FROM lanes").getInt()==0 &&
                outside.execAndGet("SELECT count(*) FROM media_files").getInt()==0;
        };
        const auto committed=recording::commit(*fixture.store,prepared);
        if(!committed.ok)std::printf("loop commit: %s\n",committed.error.c_str());
        check(committed.ok && committed.groupTxnIds.size()==4 && hooks==1,"four pass Undo groups and one publication callback share one atomic commit");
        if(!committed.ok)continue;
        auto& db=fixture.store->db();
        check(db.execAndGet("SELECT count(*) FROM media_files").getInt()==1 && db.execAndGet("SELECT count(*) FROM clips WHERE lane_id IS NOT NULL").getInt()==4,
            "four takes share exactly one media row");
        SQLite::Statement windows(db,"SELECT c.name,a.src_start_frames,a.src_len_frames FROM clips c "
            "JOIN audio_clips a ON a.clip_id=c.id WHERE c.lane_id IS NOT NULL "
            "AND a.warp_enabled=1 AND a.warp_mode='beats' ORDER BY c.id");
        bool exactWindows=true;std::size_t pass=0;
        while(windows.executeStep()) {
            const auto expectedName=pass==0?std::string("Audio"):"Audio "+std::to_string(pass+1);
            exactWindows=exactWindows && pass<4 && windows.getColumn(0).getString()==expectedName &&
                windows.getColumn(1).getInt64()==static_cast<std::int64_t>(pass*128) && windows.getColumn(2).getInt64()==(pass==3?64:128);++pass;
        }
        check(exactWindows && pass==4 && db.execAndGet("SELECT count(*) FROM lanes WHERE name='Lane'").getInt()==4,"loop takes follow tempo and retain their names, exact source windows and default lane names");
        SQLite::Statement laneOrder(db,"SELECT id FROM lanes ORDER BY ord,id");
        bool chronological=true;pass=0;
        while(laneOrder.executeStep()) {
            chronological=chronological && pass<ids.front().passes.size() &&
                laneOrder.getColumn(0).getInt64()==ids.front().passes[pass].lane;
            ++pass;
        }
        check(chronological && pass==4,"recording presents its take lanes in pass order");
        check(db.execAndGet("SELECT count(*) FROM clips WHERE lane_id IS NULL").getInt()==2 && fixture.play(precision)==loopOracle(448,512),
            "partial pass four replaces its prefix while pass three retains the uncovered tail, with exact PCM");
        const auto owned=media::projectFolder(*fixture.store)/media::pathFromUtf8(db.execAndGet("SELECT rel_path FROM media_files").getString());
        check(std::filesystem::exists(owned) && !std::filesystem::exists(recorded.directory()),"successful loop publication retains one owned source and removes private capture");
        const auto after=digestProject(*fixture.store).text;History history(*fixture.store);
        for(int remaining=3;remaining>=0;--remaining) {
            const auto next=history.nextUndo();const auto undone=history.undo();
            check(next && next->txnId==committed.groupTxnIds[static_cast<std::size_t>(remaining)] && next->label=="Record" &&
                undone.ok && undone.selBefore==selections[static_cast<std::size_t>(remaining)] &&
                db.execAndGet("SELECT count(*) FROM lanes").getInt()==remaining &&
                db.execAndGet("SELECT count(*) FROM media_files").getInt()==(remaining?1:0),"one Undo restores the preceding pass, selection and shared-media lifetime");
            check(fixture.play(precision)==loopOracle(remaining*128,512),"each undo restores exact preceding main-lane PCM");
        }
        check(digestProject(*fixture.store).text==before && std::filesystem::exists(owned),"undoing all passes restores the project but keeps owned audio for Redo");
        for(int restored=1;restored<=4;++restored)check(history.redo().ok,"redo restores one recorded pass");
        check(digestProject(*fixture.store).text==after && fixture.play(precision)==loopOracle(448,512),"redo restores exact complete loop comp and journal material");
    }
}
void loopPartialEdgesAndPriorComp() {
    for(const auto start:{480,560}) {
        Fixture fixture;auto previous=fixture.capture(false,Precision::Float32,false,256,{},448);
        const std::array<recording::ClipIds,1> oldIds={{{7,8,"Original"}}};
        auto old=recording::prepareCommit(*fixture.store,previous,oldIds);
        check(old.ok && recording::commit(*fixture.store,old).ok,"establish real pre-existing main audio on both sides of the loop");
        const auto heardBefore=fixture.play(Precision::Float32);const auto before=digestProject(*fixture.store).text;
        auto recorded=fixture.capture(false,Precision::Float32,true,350,{},start);const auto ids=loopIds(recorded);
        auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
        if(!prepared.ok)std::printf("edge loop prepare: %s\n",prepared.error.c_str());
        const auto committed=recording::commit(*fixture.store,prepared);
        if(!committed.ok)std::printf("edge loop commit: %s\n",committed.error.c_str());
        check(committed.ok && fixture.play(Precision::Float32)==loopOracle(350,start,heardBefore),
            "partial first and last passes preserve preceding main material outside every captured span");
        History history(*fixture.store);
        for(std::size_t pass=0;pass<recorded.passes().size();++pass)check(history.undo().ok,"edge pass undoes independently");
        check(digestProject(*fixture.store).text==before && fixture.play(Precision::Float32)==heardBefore,"undo restores original comp identities, source windows and exact PCM");
    }
}
void loopSinglePassBoundaries() {
    for(const auto frames:{32,128}) {
        Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,frames);const auto ids=loopIds(recorded);
        auto malformed=recorded;--malformed.frames;
        check(!recording::prepareLoopCommit(*fixture.store,malformed,ids).ok,"pass log cannot be attached to a different source extent");
        auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
        const auto committed=recording::commit(*fixture.store,prepared);
        check(committed.ok && committed.groupTxnIds.size()==1 && fixture.store->db().execAndGet("SELECT count(*) FROM lanes").getInt()==1 &&
            fixture.play(Precision::Float32)==loopOracle(frames,512),"single partial and exact-end loop stops create one nonempty take and exact main material");
        check(History(*fixture.store).undo().ok && fixture.store->db().execAndGet("SELECT count(*) FROM lanes").getInt()==0,
            "single loop pass still owns one complete Undo group");
    }
}
void loopAtomicRefusalAndDedup() {
    Fixture fixture;auto recorded=fixture.capture(true,Precision::Float32,true,448);const auto ids=loopIds(recorded);
    const auto before=digestProject(*fixture.store).text;auto& db=fixture.store->db();
    db.exec("CREATE TRIGGER reject_last_take BEFORE INSERT ON clips WHEN NEW.name='Audio 4' BEGIN SELECT RAISE(ABORT,'last pass refused');END");
    auto rejected=recording::prepareLoopCommit(*fixture.store,recorded,ids);const auto result=recording::commit(*fixture.store,rejected);
    check(rejected.ok && !result.ok && result.groupTxnIds.empty() && digestProject(*fixture.store).text==before &&
        std::filesystem::exists(recorded.tracks[0].file),"failure in the final pass rolls back every earlier lane, take, main edit and media row");
    db.exec("DROP TRIGGER reject_last_take");
    auto veto=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    veto.options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& error){error="owned fixture veto";return false;};
    check(veto.ok && !recording::commit(*fixture.store,veto).ok && digestProject(*fixture.store).text==before,
        "publication veto leaves all passes and shared media absent");
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);const auto committed=recording::commit(*fixture.store,prepared);
    check(committed.ok && committed.groupTxnIds.size()==4 && db.execAndGet("SELECT count(*) FROM lanes").getInt()==8 &&
        db.execAndGet("SELECT count(*) FROM media_files").getInt()==1,"two targets commit four shared pass groups and deduplicate identical continuous sources");
    auto doubled=loopOracle(448,512);for(auto& sample:doubled)sample*=2;
    check(fixture.play(Precision::Float32)==doubled,"two target clips hear the same shared source with exact summed PCM");
    const auto after=digestProject(*fixture.store).text;
    auto fresh=fixture.capture(false,Precision::Float32,true,32);const auto occupied=loopIds(fresh);
    check(!recording::prepareLoopCommit(*fixture.store,fresh,occupied).ok && digestProject(*fixture.store).text==after &&
        std::filesystem::exists(fresh.tracks[0].file),"occupied lane/clip allocations refuse without deleting prior takes or private audio");
    fresh.discard();
}


std::int64_t fixtureTicks(std::int64_t sample) {
    TempoMap tempo;tempo.events.push_back({0,120,0});return tempo.secondsToTicks(static_cast<double>(sample)/48000);
}
void existingTake(Store& store,std::int64_t lane,std::int64_t order,std::int64_t track=21) {
    SQLite::Statement row(store.db(),"INSERT INTO lanes(id,track_id,ord,kind,name) VALUES(?,?,?,'take','Existing')");
    row.bind(1,lane);row.bind(2,track);row.bind(3,order);row.exec();
}
void existingTakeClip(Store& store,std::int64_t id,std::int64_t lane,std::int64_t begin,
    std::optional<std::int64_t> length,bool nanos=false,std::int64_t track=21) {
    const auto sql=nanos?"INSERT INTO clips(id,track_id,lane_id,kind,time_base,pos_ns,length_ns) VALUES(?,?,?,'midi',1,?,?)":
        "INSERT INTO clips(id,track_id,lane_id,kind,time_base,pos_ticks,length_ticks) VALUES(?,?,?,'midi',0,?,?)";
    SQLite::Statement row(store.db(),sql);row.bind(1,id);row.bind(2,track);row.bind(3,lane);row.bind(4,begin);
    if(length)row.bind(5,*length);else row.bind(5);row.exec();
}
std::vector<std::int64_t> takeOrder(Store& store) {
    SQLite::Statement rows(store.db(),"SELECT id FROM lanes WHERE track_id=21 AND kind='take' ORDER BY ord,id");
    std::vector<std::int64_t> result;while(rows.executeStep())result.push_back(rows.getColumn(0).getInt64());return result;
}
std::int64_t preparedTakeLane(const recording::RecordingEdit& prepared,std::int64_t clip) {
    for(const auto& request:prepared.requests)if(request.opType=="clip.duplicate" && request.payload.at("id")==clip)
        for(const auto& change:request.payload.at("changes"))if(change.at("table")=="clips")
            return change.at("after").at("lane_id").get<std::int64_t>();
    return -1;
}
void loopReusesOrderedLanes() {
    Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,448);auto& db=fixture.store->db();
    // Visible order differs from ID order. The first lane has both clocks and
    // ends at the exact musical boundary; ns floor is strictly before it.
    existingTake(*fixture.store,30,4);existingTake(*fixture.store,20,9);existingTake(*fixture.store,10,17);
    existingTake(*fixture.store,5,0,22);
    existingTakeClip(*fixture.store,40,30,0,fixtureTicks(512));
    existingTakeClip(*fixture.store,41,30,0,10666666,true);
    existingTakeClip(*fixture.store,42,20,0,fixtureTicks(480));
    const auto before=digestProject(*fixture.store).text;const auto ids=loopIds(recorded);
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    check(prepared.ok && preparedTakeLane(prepared,200)==30 && preparedTakeLane(prepared,201)==20 &&
        preparedTakeLane(prepared,202)==10 && preparedTakeLane(prepared,203)==103,
        "reuse takes visible same-track lanes once each, including exact ends, then a fresh fallback");
    check(prepared.ok && digestProject(*fixture.store).text==before,"reuse preparation leaves prior lanes and material unchanged");
    const auto result=recording::commit(*fixture.store,prepared);
    if(!result.ok)std::printf("reuse commit: %s\n",result.error.c_str());
    check(result.ok && result.groupTxnIds.size()==4,"reused and new lanes share one transaction with four pass Undo groups");
    if(!result.ok)return;
    check(takeOrder(*fixture.store)==std::vector<std::int64_t>{103,30,20,10} &&
        db.execAndGet("SELECT count(*) FROM lanes WHERE id IN (100,101,102)").getInt()==0,
        "unused fallback IDs stay absent and pre-existing lanes retain relative order");
    check(db.execAndGet("SELECT count(*) FROM clips WHERE id IN (40,41,42)").getInt()==3 &&
        db.execAndGet("SELECT count(*) FROM clips WHERE lane_id=5").getInt()==0 &&
        fixture.play(Precision::Float32)==loopOracle(448,512),
        "existing take content and other-track lanes survive while the main comp sounds exact");
    const auto after=digestProject(*fixture.store).text;History history(*fixture.store);
    for(int remaining=3;remaining>=0;--remaining) {
        const auto undone=history.undo();
        check(undone.ok && takeOrder(*fixture.store)==std::vector<std::int64_t>{30,20,10} &&
            db.execAndGet("SELECT count(*) FROM clips WHERE id IN (40,41,42)").getInt()==3 &&
            fixture.play(Precision::Float32)==loopOracle(remaining*128,512),
            "each reused pass Undo preserves pre-existing lanes and restores preceding main audio");
    }
    check(digestProject(*fixture.store).text==before,"all reuse Undo restores exact old clip and sparse lane ordinal bytes");
    for(int pass=0;pass<4;++pass)check(history.redo().ok,"reuse pass redoes independently");
    check(digestProject(*fixture.store).text==after,"reuse Redo restores literal assigned lanes and source windows");
}
void loopMixedReuseAndFallback() {
    Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,448,{},480);
    existingTake(*fixture.store,77,12);
    existingTakeClip(*fixture.store,40,77,0,fixtureTicks(500));
    const auto ids=loopIds(recorded);const auto before=digestProject(*fixture.store).text;
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    check(prepared.ok && preparedTakeLane(prepared,200)==100 && preparedTakeLane(prepared,201)==77 &&
        preparedTakeLane(prepared,202)==102 && preparedTakeLane(prepared,203)==103,
        "partial pre-loop pass falls back before a later pass can safely reuse the same visible lane");
    prepared.options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& error){error="reuse veto";return false;};
    check(!recording::commit(*fixture.store,prepared).ok && digestProject(*fixture.store).text==before &&
        std::filesystem::exists(recorded.tracks[0].file),"reuse veto rolls every pass back and preserves old rows and private audio");
    auto retry=recording::prepareLoopCommit(*fixture.store,recorded,ids);const auto committed=recording::commit(*fixture.store,retry);
    check(committed.ok && takeOrder(*fixture.store)==std::vector<std::int64_t>{100,102,103,77} &&
        fixture.play(Precision::Float32)==loopOracle(448,480),"new fallback lanes stay in pass order around a reused pass");
    if(!committed.ok)return;
    History history(*fixture.store);
    const std::array<std::vector<std::int64_t>,4> previous={std::vector<std::int64_t>{100,102,77},
        std::vector<std::int64_t>{100,77},std::vector<std::int64_t>{100,77},std::vector<std::int64_t>{77}};
    for(const auto& order:previous)check(history.undo().ok && takeOrder(*fixture.store)==order,
        "mixed pass Undo restores the preceding chronological fallback group without moving old lanes");
    check(digestProject(*fixture.store).text==before,"mixed reuse restores exact original lane ordinal and material");
}
void loopReuseRefusesUnprovedExtents() {
    Fixture fixture;auto recorded=fixture.capture(false,Precision::Float32,true,32);auto& db=fixture.store->db();
    for(std::int64_t lane=50;lane<57;++lane)existingTake(*fixture.store,lane,lane);
    existingTakeClip(*fixture.store,40,50,0,fixtureTicks(513)); // Overlaps start.
    existingTakeClip(*fixture.store,41,51,fixtureTicks(900),fixtureTicks(10)); // Later, even without overlap.
    existingTakeClip(*fixture.store,42,52,0,10666667,true); // One ns beyond the safe floor.
    existingTakeClip(*fixture.store,43,53,0,std::nullopt); // Unbounded.
    existingTakeClip(*fixture.store,44,54,std::numeric_limits<std::int64_t>::max(),1); // Sum overflow.
    existingTakeClip(*fixture.store,45,55,0,0,false,22); // Inconsistent owning track.
    existingTakeClip(*fixture.store,46,56,0,-1); // Malformed native extent.
    db.exec("UPDATE clips SET muted=1 WHERE id=41");
    const auto before=digestProject(*fixture.store).text;const auto ids=loopIds(recorded);
    auto prepared=recording::prepareLoopCommit(*fixture.store,recorded,ids);
    check(prepared.ok && preparedTakeLane(prepared,200)==100 && digestProject(*fixture.store).text==before,
        "overlap, future muted material, fractional-ns ambiguity, unknown, overflow and wrong ownership all prevent reuse safely");
    // An exact physical boundary is eligible too: 480 samples at 48 kHz is 10 ms.
    Fixture exact;auto shortTake=exact.capture(false,Precision::Float32,true,32,{},480);
    existingTake(*exact.store,70,9);existingTakeClip(*exact.store,40,70,0,10000000,true);
    auto physical=recording::prepareLoopCommit(*exact.store,shortTake,loopIds(shortTake));
    check(physical.ok && preparedTakeLane(physical,200)==70,"nanosecond clip ending exactly at pass start is reusable");
}

void recoverInterruptedLoop() {
    namespace fs=std::filesystem;
    Fixture f;
    const auto root=f.temporary.path()/"recordings";
    fs::path original;
    {
        auto stopped=f.capture(true,Precision::Float32,true,317,root,512,true);
        original=stopped.directory();
        check(stopped.failure==RecordingFailure::None && !stopped.recordingId().empty() &&
              fs::exists(original/"take.json") && fs::exists(original/"start.json"),
              "Record saves a plan and the actual first sample, independently of the Stop result");
        check(recording::recoverInterrupted(*f.store,root,original).status==recording::RecoveryStatus::Refused,
              "a finishing take still owns the original plan");
    }
    const auto duringSession=recording::discoverInterrupted();
    check(std::none_of(duringSession.begin(),duringSession.end(),
          [&](const auto& item){return item.directory==original;}),
          "startup ignores an unplaced take when its process still holds the session lock");
    const auto damaged=original/"take-0.wav";
    {
        std::fstream file(damaged,std::ios::in|std::ios::out|std::ios::binary);
        const std::array<char,4> stale{};
        file.seekp(4);
        file.write(stale.data(),static_cast<std::streamsize>(stale.size()));
        file.seekp(90); // Mono recorder Float32 data size.
        file.write(stale.data(),static_cast<std::streamsize>(stale.size()));
        if(!file)throw std::runtime_error("cannot make the stale recorder header");
    }
    const auto hash=media::blake3File(damaged);
    check(hash && !hash.hex.empty(),"read original interrupted recorder bytes");
    // Reopen the project through a new owner and leave the original media on disk.
    const auto project=f.store->path();
    f.store.reset();
    StoreError error{};
    auto readOnly=Store::open(project,error,true);
    if(!readOnly)throw std::runtime_error(toString(error));
    check(recording::recoverInterrupted(*readOnly,root,original).status==recording::RecoveryStatus::Refused,
          "a read-only project never prepares interrupted audio");
    readOnly.reset();
    f.store=Store::open(project,error);
    if(!f.store)throw std::runtime_error(toString(error));
    Fixture different;
    check(recording::recoverInterrupted(*different.store,root,original).status==recording::RecoveryStatus::Refused,
          "an interrupted recording cannot be attached to another project");
    check(recording::recoverInterrupted(*f.store,root,original.parent_path()/".."/"recordings"/original.filename()).status==
          recording::RecoveryStatus::Refused,"a parent traversal is rejected before reading its plan");
    auto recovered=recording::recoverInterrupted(*f.store,root,original);
    check(recovered.status==recording::RecoveryStatus::Ready && recovered.capture.recovered(),
          recovered.error.empty()?"reopen a stale-header recorder take from its plan":recovered.error.c_str());
    if(recovered.status!=recording::RecoveryStatus::Ready)return;
    check(recovered.capture.frames==317 && recovered.capture.startSample==512 &&
          recovered.capture.passes().size()==3 && recovered.capture.passes().back().frames==61,
          "recovery keeps completed loop passes and the captured prefix of the last pass");
    {
        audio::WavReader audio(recovered.capture.tracks.at(0).file);
        std::vector<float> heard(317);
        const auto frames=audio.read(heard.data(),317);
        bool exact=frames==317;
        for(std::size_t i=0;i<heard.size();++i) {
            if(heard[i]!=Fixture::sample(i))exact=false;
        }
        check(exact && media::blake3File(damaged).hex==hash.hex,
              "header repair matches captured PCM and leaves the original untouched");
    }
    auto refused=recording::prepareLoopCommit(*f.store,recovered.capture,loopIds(recovered.capture));
    refused.options.beforeCommit=[](SQLite::Database&,std::int64_t,std::string& text) {
        text="injected recovery commit refusal";
        return false;
    };
    check(refused.ok && !recording::commit(*f.store,refused).ok,"failed recovery can still be placed");
    auto edit=recording::prepareLoopCommit(*f.store,recovered.capture,loopIds(recovered.capture));
    const auto committed=recording::commit(*f.store,edit);
    check(committed.ok && takeOrder(*f.store).size()==3,
          committed.error.empty()?"recovery places the same track and loop passes as Stop":committed.error.c_str());
    if(!committed.ok)return;
    check(recording::recoverInterrupted(*f.store,root,original).status==recording::RecoveryStatus::AlreadyPlaced,
          "a committed recording ID skips a leftover original plan");
    check(History(*f.store).undo().ok && takeOrder(*f.store).empty(),"one Undo removes every recovered pass");
    f.store.reset();
    f.store=Store::open(project,error);
    if(!f.store)throw std::runtime_error(toString(error));
    check(recording::recoverInterrupted(*f.store,root,original).status==recording::RecoveryStatus::AlreadyPlaced,
          "one Undo does not resurrect a committed interrupted recording");
    check(History(*f.store).redo().ok && takeOrder(*f.store).size()==3,"one Redo restores the entire recovered recording");
}
void recoverPcm24AndReportDeletedTrack() {
    namespace fs=std::filesystem;
    Fixture f;
    const auto root=f.temporary.path()/"recordings";
    fs::path original;
    {
        auto stopped=f.capture(false,Precision::Float32,false,129,root,512,true,audio::WavFormat::Pcm24);
        check(stopped.failure==RecordingFailure::None,"make an owned odd-length PCM24 source");
        original=stopped.directory();
    }
    {
        auto recovered=recording::recoverInterrupted(*f.store,root,original);
        check(recovered.status==recording::RecoveryStatus::Ready && recovered.capture.frames==129,
              recovered.error.empty()?"PCM24 padding is excluded from the recovered frames":recovered.error.c_str());
        if(recovered.status==recording::RecoveryStatus::Ready) {
            check(media::blake3File(recovered.capture.tracks.at(0).file).hex==
                  media::blake3File(original/"take-0.wav").hex,
                  "already complete PCM24 remains byte-identical after repair");
        }
    }
    f.store->db().exec("DELETE FROM tracks WHERE id=21");
    const auto refused=recording::recoverInterrupted(*f.store,root,original);
    check(refused.status==recording::RecoveryStatus::Refused && refused.error.find("21")!=std::string::npos,
          "a deleted recovery target is named and its private files stay available");
    check(fs::is_regular_file(original/"take-0.wav"),"unplaced source remains recoverable for inspection");
}
void discardOwnedTake() {
    namespace fs=std::filesystem;
    Fixture fixture;
    auto recorded=fixture.capture(false,Precision::Float32,false,128,
                                  fixture.temporary.path()/"discard-take",512,true);
    const auto directory=recorded.directory();
    std::error_code leaseError;
    check(fs::is_regular_file(directory/"start.json") && fs::is_regular_file(directory/"owner.lock") &&
              !media::ProjectFileLease::openExclusive(directory/"owner.lock",leaseError),
          "a captured take retains its active owner until discard");
    const auto discarded=recorded.discard();
    check(!discarded && recorded.directory().empty() && !fs::exists(directory),
          "discard closes the take owner and removes the complete private directory");
    if(discarded)std::printf("discard: %s\n",discarded.message().c_str());
}
void recoverRf64() {
    namespace fs=std::filesystem;
    Fixture fixture;
    const auto root=fixture.temporary.path()/"recordings";
    fs::create_directories(root/"rf64");
    const auto directory=fs::canonical(root)/"rf64";
    const auto file=directory/"take-0.wav";
    const std::array<float,5> samples{0.25f,-0.25f,-0.5f,0.0f,0.5f};
    {
        audio::WavWriter writer(file,48000,1,audio::WavFormat::Float32,nullptr,8);
        writer.write(samples.data(),static_cast<std::uint32_t>(samples.size()));
        writer.close();
    }
    AudioRecordingSpec spec;
    spec.sampleRate=48000;
    spec.targets={{21,{0}}};
    std::string error;
    if(!recording::configureRecovery(*fixture.store,spec,error))throw std::runtime_error(error);
    {
        const auto plan=recording_recovery::begin(spec,directory,spec.recoveryProjectUuid);
        (void)recording_recovery::writeStart(directory,plan.plan.recordingId,512);
    }
    {
        std::fstream header(file,std::ios::in|std::ios::out|std::ios::binary);
        const std::array<char,24> stale{};
        header.seekp(20);
        header.write(stale.data(),static_cast<std::streamsize>(stale.size()));
        if(!header)throw std::runtime_error("cannot make a stale RF64 header");
    }
    const auto sourceHash=media::blake3File(file);
    auto repaired=recording::recoverInterrupted(*fixture.store,root,directory);
    check(repaired.status==recording::RecoveryStatus::Ready,
          repaired.error.empty()?"recover an RF64 capture with stale ds64 sizes":repaired.error.c_str());
    if(repaired.status!=recording::RecoveryStatus::Ready)return;
    {
        audio::WavReader reader(repaired.capture.tracks.at(0).file);
        std::array<float,5> actual{};
        check(reader.frames()==samples.size() && reader.read(actual.data(),5)==5 && actual==samples,
              "RF64 repair recovers the exact PCM from the recorder-owned data chunk");
    }
    check(media::blake3File(file).hex==sourceHash.hex,"RF64 header repair leaves original bytes unchanged");
}
void refuseReboundRecoveryFile() {
    namespace fs=std::filesystem;
    Fixture fixture;
    const auto root=fixture.temporary.path()/"recordings";
    fs::path take;
    {
        const auto recording=fixture.capture(false,Precision::Float32,false,128,root,512,true);
        take=recording.tracks.at(0).file;
    }
    const auto original=fs::canonical(fixture.temporary.path())/"original.wav";
    fs::rename(take,original);
    fs::copy_file(original,take);
    const auto verified=media::blake3File(take);
    const auto refused=recording::recoverInterrupted(*fixture.store,root,take.parent_path());
    check(refused.status==recording::RecoveryStatus::Refused &&
          refused.error.find("identity")!=std::string::npos &&
          verified && media::blake3File(take).hex==verified.hex &&
          media::blake3File(original).hex==verified.hex,
          "a byte-identical replacement recording is refused using native identity and both files remain intact");
}
void ordinaryStopUsesTheSameRecordingReceipt() {
    Fixture fixture;
    const auto root=fixture.temporary.path()/"recordings";
    const auto recorded=fixture.capture(false,Precision::Float32,false,128,root,512,true);
    const auto original=recorded.directory();
    const std::array<recording::ClipIds,1> ids={{{7,8,"Ordinary Stop"}}};
    auto edit=recording::prepareCommit(*fixture.store,recorded,ids);
    const auto committed=recording::commit(*fixture.store,edit);
    SQLite::Statement receipt(fixture.store->db(),"SELECT value FROM adi_meta WHERE key=?");
    receipt.bind(1,"recording.placed."+recorded.recordingId());
    check(committed.ok && receipt.executeStep() && receipt.getColumn(0).getInt64()==committed.txnId &&
          !std::filesystem::exists(original),
          "ordinary Stop writes the same transaction receipt before discarding its private plan");
}
void stagedStopUsesTheSameRecordingReceipt() {
    Fixture fixture;
    const auto root = fixture.temporary.path() / "recordings";
    const auto recorded = fixture.capture(false, Precision::Float32, false, 128, root, 512, true);
    const auto original = recorded.directory();
    const std::array<recording::ClipIds, 1> ids{{{7, 8, "Staged Stop"}}};
    auto staged = recording::stageTake(recorded, media::projectFolder(*fixture.store));
    auto edit = recording::prepareStagedCommit(*fixture.store, staged, ids, {}, recorded.placementOffsetSamples);
    const bool published = staged.ok && edit.ok && recording::publishStagedTake(edit);
    check(published, "SC-250: publish the normal asynchronous take");
    if (!published) return;
    const auto committed = recording::commit(*fixture.store, edit);
    SQLite::Statement receipt(fixture.store->db(), "SELECT value FROM adi_meta WHERE key=?");
    receipt.bind(1, "recording.placed." + recorded.recordingId());
    check(committed.ok && receipt.executeStep() && receipt.getColumn(0).getInt64() == committed.txnId,
          "SC-250: asynchronous placement commits its receipt with its audio op");
    check(recording::cleanupStagedTake(edit).empty() && !std::filesystem::exists(original),
          "SC-250: asynchronous cleanup removes the completed plan after placement");
}
int crashRecordingChild(const std::filesystem::path& project,const std::filesystem::path& root,bool beforeAudio) {
    namespace fs=std::filesystem;
    StoreError openError{};
    auto store=Store::open(project,openError);
    if(!store)throw std::runtime_error(toString(openError));
    Session session;
    if(!session.load(*store,{},SessionSpec{2,48000,128}))throw std::runtime_error(session.error());
    AudioRecordingSpec spec;
    spec.workFolder=root;
    spec.sampleRate=48000;
    spec.maxFrames=128;
    spec.targets={{21,{0}}};
    std::string error;
    if(!recording::configureRecovery(*store,spec,error) || !session.startRecording(spec,error))
        throw std::runtime_error(error);
    if(beforeAudio)std::_Exit(37);
    session.transport().locate(512);
    session.transport().play();
    std::array<float,128> samples{};
    for(std::size_t frame=0;frame<samples.size();++frame)samples[frame]=Fixture::sample(frame);
    const float* in[]={samples.data()};
    std::array<float,128> left{},right{};
    float* out[]={left.data(),right.data()};
    AudioIo io;
    io.in=in;
    io.numIn=1;
    io.out=out;
    io.numOut=2;
    io.frames=128;
    session.process(io);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        for(const auto& entry:fs::directory_iterator(root)) {
            std::error_code sizeError;
            const auto size=fs::file_size(entry.path()/"take-0.wav",sizeError);
            if(!sizeError && size>=94+128*sizeof(float) && fs::is_regular_file(entry.path()/"start.json"))
                std::_Exit(37); // Bypass Stop and all C++ destructors.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return 38;
}
void recoverAfterProcessExit(const std::filesystem::path& executable) {
    namespace fs=std::filesystem;
    Fixture fixture;
    const auto project=fixture.store->path();
    const auto root=fixture.temporary.path()/"crash-recordings";
    fixture.store.reset();
    const auto child=scan::runProcess(fs::canonical(executable),
        {"--crash-recording",media::pathUtf8(project),media::pathUtf8(root)});
    if(child.status!=scan::ProcessStatus::Exited || child.exitCode!=37) {
        const auto description="interrupted fixture child did not reach the live write (status "+
            std::to_string(static_cast<unsigned>(child.status))+", exit "+std::to_string(child.exitCode)+
            "): "+child.output;
        check(false,description.c_str());
        return;
    }
    StoreError error{};
    fixture.store=Store::open(project,error);
    if(!fixture.store)throw std::runtime_error(toString(error));
    const auto directory=fs::directory_iterator(root)->path();
    const auto known=fs::canonical(directory);
    // Exercise both directory scans with a name outside the usual Windows code pages.
    const auto sessions=appdata::pathsFor(appdata::App::Daw).data/"recording-sessions";
    const fs::path stray(u8"0\u05D0\u4E2D\U0001F3B5.txt");
    std::vector<fs::path> strays;
    for(const auto& session:fs::directory_iterator(sessions))
        if(session.is_directory())strays.push_back(session.path()/stray);
    strays.push_back(sessions/stray);
    for(const auto& path:strays) {
        std::ofstream file(path);
        file<<"unrelated file";
        if(!file)throw std::runtime_error("could not create the Unicode discovery fixture");
    }
    const auto launched=recording::discoverInterrupted();
    const auto candidate=std::find_if(launched.begin(),launched.end(),
        [&](const auto& item){return item.directory==known;});
    check(candidate!=launched.end() && candidate->tracks==std::vector<std::int64_t>{21} &&
          !candidate->projectUuid.empty() && !candidate->recordingId.empty(),
          "startup finds the unclosed process's recording without being given its directory despite Unicode entries");
    check(strays.size()>1 && std::ranges::all_of(strays,[](const auto& path){return fs::is_regular_file(path);}),
          "startup leaves unrelated Unicode entries in the registry and session alone");
    auto recovered=recording::recoverInterrupted(*fixture.store,root,directory);
    check(recovered.status==recording::RecoveryStatus::Ready && recovered.capture.frames==128 &&
          recovered.capture.startSample==512,
          recovered.error.empty()?"recover a live take after the recording process exits":recovered.error.c_str());
    if(recovered.status!=recording::RecoveryStatus::Ready)return;
    const std::array<recording::ClipIds,1> ids={{{7,8,"Interrupted take"}}};
    auto edit=recording::prepareCommit(*fixture.store,recovered.capture,ids);
    const auto committed=recording::commit(*fixture.store,edit);
    check(committed.ok,committed.error.empty()?"place recovered live PCM through linear Stop":committed.error.c_str());
    if(committed.ok) {
        check(History(*fixture.store).undo().ok,"the recovered live take is one Undo step");
        const auto after=recording::discoverInterrupted();
        check(std::none_of(after.begin(),after.end(),[&](const auto& item){return item.directory==known;}),
              "startup suppresses placed recording IDs even when their original files remain and the edit is undone");
    }
    const auto emptyRoot=fixture.temporary.path()/"unstarted-recording";
    fixture.store.reset();
    const auto unstarted=scan::runProcess(fs::canonical(executable),
        {"--crash-before-audio",media::pathUtf8(project),media::pathUtf8(emptyRoot)});
    check(unstarted.status==scan::ProcessStatus::Exited && unstarted.exitCode==37 && fs::is_directory(emptyRoot),
          "another process exits after registration and before its first audio");
    const auto afterward=recording::discoverInterrupted();
    check(std::none_of(afterward.begin(),afterward.end(),[&](const auto& item) {
        return fs::exists(emptyRoot) && item.directory.parent_path()==fs::canonical(emptyRoot);
    }),"startup skips an interrupted session that accepted no audio");
}
}
int main(int argc,char** argv){try{
    if(argc==4) {
        const std::string_view command=argv[1];
        if(command=="--crash-recording" || command=="--crash-before-audio")
            return crashRecordingChild(media::pathFromUtf8(argv[2]),media::pathFromUtf8(argv[3]),
                                       command=="--crash-before-audio");
    }
    // Parent and interrupted children share an isolated recording registry.
    // Its directory outlives the process's static session owner lease.
    static const test::TempDirectory recoveryHome("recording_commands", "launch-registry");
#if defined(_WIN32)
    if (_wputenv_s(L"ADI_HOME", recoveryHome.path().c_str()) != 0)
#else
    if (setenv("ADI_HOME", recoveryHome.path().c_str(), 1) != 0)
#endif
        throw std::runtime_error("could not set the recording fixture's application directory");
    if(argc==2 && std::string_view(argv[1])=="--save-as-recovery-contract") {
        saveAsRecoveryContract();
        std::printf("%d checks, %d failures\n",checks,failures);
        return failures?1:0;
    }
    latencyCompensatedPlacement();
    recordedTakeFollowsTempo();
    latencyCompensationKeepsPunchProtection();
    midiLinearTakes();midiOverdub();midiControllerOnlyTake();mixedLinearTake();
    midiLoopTakes();
    if(argc!=1 && (argc!=3 || std::string(argv[1])!="--cross-volume-work-folder"))throw std::runtime_error("usage: recording tests [--cross-volume-work-folder PATH]");
    publicationStagingOwnership();forcedCrossDevicePublication();heardPlacementAndHistory();numberedRecordingFiles();linearTakeReplacesWhatItCovers();rollbackNeverDeletesLaterWriter();lateSqlAndOwnedReuse();existingMediaAndHistoryPaths();groupDedupAndGuards();loopPublicationRefusesFlattening();
    stagedPublicationAndRefresh();stagedPublicationRefusesChangedContent();
    unwarpedRecordingTempoChanges();
    loopPassesAndHistory();loopPartialEdgesAndPriorComp();loopSinglePassBoundaries();loopAtomicRefusalAndDedup();
    loopReusesOrderedLanes();loopMixedReuseAndFallback();loopReuseRefusesUnprovedExtents();
    recoverInterruptedLoop();recoverPcm24AndReportDeletedTrack();discardOwnedTake();recoverRf64();refuseReboundRecoveryFile();
    ordinaryStopUsesTheSameRecordingReceipt();
    stagedStopUsesTheSameRecordingReceipt();
    recoverAfterProcessExit(media::pathFromUtf8(argv[0]));
    if(argc==3) {
        const auto captureFolder=media::pathFromUtf8(argv[2]);
        crossVolumePublication(captureFolder);
        loopPassesAndHistory(captureFolder);
    }
}catch(const std::exception& error){check(false,error.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
