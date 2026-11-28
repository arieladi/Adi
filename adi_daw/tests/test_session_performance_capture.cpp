// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/session_performance_capture.hpp"

#include "adi/blob.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/digest.hpp"
#include "adi/engine/clip_playback.hpp"
#include "adi/engine/launcher.hpp"
#include "adi/engine/midi_clips.hpp"
#include "adi/engine/process.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "session_schema_fixture.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
using namespace adi;
using namespace adi::engine;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

constexpr std::int64_t tick(std::int64_t frame) {
    // All fixture boundaries fall on exact ticks at 120 BPM / 48 kHz.
    return frame * textproj::kPPQ / 24000;
}

LaunchTransition transition(std::int64_t from, std::int64_t to, std::int64_t frame,
                            std::int64_t sourceFrame = 0, bool arrangement = false) {
    LaunchTransition edge;
    edge.track = 21;
    edge.oldClip = from;
    edge.newClip = to;
    edge.sourceTick = tick(sourceFrame);
    edge.sample = frame;
    edge.tick = tick(frame);
    edge.arrangement = arrangement;
    return edge;
}

struct Project {
    test::TempDirectory temporary{"session_performance_capture", "project"};
    std::filesystem::path path = temporary.path() / "Session.adi";
    std::unique_ptr<Store> store;

    Project() {
        StoreError error{};
        store = Store::create(path, error);
        require(store != nullptr, "create test project");
        test::applySessionSchema(db());
        db().exec("DELETE FROM tempo_map; INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
                  "INSERT INTO tracks(id,kind,name) VALUES(21,'midi','Notes'),(22,'midi','Unaffected'),(23,'audio','Samples');"
                  "INSERT INTO lanes(id,track_id,ord,name) VALUES(11,21,0,'Take');"
                  "INSERT INTO scenes(id,ord,name) VALUES(1,0,'First');"
                  "INSERT INTO clips(id,track_id,lane_id,kind,name,pos_ticks,length_ticks) VALUES"
                  "(31,21,NULL,'midi','Arrangement',0,240240),"
                  "(32,21,11,'midi','Take',0,240240),"
                  "(33,22,NULL,'midi','Unaffected',0,240240),"
                  "(51,21,NULL,'midi','Session notes',0,144144),"
                  "(52,23,NULL,'audio','Session loop',0,240240);"
                  "UPDATE clips SET loop_enabled=1,loop_start_ticks=0,loop_len_ticks=24025 WHERE id=52;"
                  "INSERT INTO media_files(id,hash_blake3,rel_path,format) VALUES(88,'source.wav hash','source.wav','wav');"
                  "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(52,88,0,1000);"
                  "INSERT INTO clip_slots(track_id,scene_id,clip_id) VALUES(21,1,51),(23,1,52);");
        NoteRecord note{};
        note.note_id = 176;
        note.start_ticks = tick(100);
        note.dur_ticks = tick(60);
        note.key = 60;
        note.channel = 2;
        note.vel_on = 99;
        note.vel_off = 33;
        note.probability = 10000;
        const std::array records{note};
        const auto bytes = writeStream<NoteRecord>(FourCC::Notes, records);
        SQLite::Statement insert(db(), "INSERT INTO event_streams(id,clip_id,stream_kind,data) VALUES(61,51,'notes',?)");
        insert.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        insert.exec();
    }

    SQLite::Database& db() { return store->db(); }
    std::int64_t count(const std::string& sql) { return db().execAndGet(sql).getInt64(); }

    std::filesystem::path writeSource(std::span<const float> pcm) {
        const auto file = path.parent_path() / "source.wav";
        {
            audio::WavWriter writer(file, 48000, 1, audio::WavFormat::Float32);
            writer.write(pcm.data(), static_cast<std::uint32_t>(pcm.size()));
            writer.close();
        }
        const auto hash = media::blake3File(file);
        require(static_cast<bool>(hash), "verify the capture's actual audio source bytes");
        SQLite::Statement update(db(), "UPDATE media_files SET hash_blake3=? WHERE id=88");
        update.bind(1, hash.hex);
        update.exec();
        return file;
    }

    SessionPerformanceSource freeze() const {
        std::string error;
        auto source = captureSessionPerformanceSource(*store, error);
        if (!source) throw std::runtime_error(error);
        return std::move(*source);
    }
};

LaunchCapturePlan performance() {
    std::array<LaunchTransition, 3> storage{};
    LaunchCaptureBuffer buffer(storage);
    require(buffer.append(transition(0, 51, 300, 50)) && buffer.append(transition(51, 0, 500)) &&
            buffer.append(transition(0, 0, 700, 0, true)), "perform source-phase launch, Stop, Back to Arrangement");
    return planLaunchCapture(buffer, {200, tick(200)}, {900, tick(900)}, {});
}

void verifyRenderedMidi(Store& store) {
    Transport transport;
    MidiClips playback(rows::readModel(store), transport, 48000.0, makeMidiClipState());
    const auto nodes = playback.sourcesFor(21);
    require(nodes.size() == 1 && playback.problems().empty(), "build reopened Arrangement MIDI player");
    auto& source = *nodes.front();
    transport.play();
    std::array<Event, 32> storage{};
    EventList events(storage.data(), static_cast<std::int32_t>(storage.size()));
    std::array<std::int64_t, 2> samples{};
    std::array<Event, 2> notes{};
    std::size_t count = 0;
    constexpr std::int32_t block = 64;
    for (std::int64_t offset = 0; offset < 1024; offset += block) {
        events.clear();
        source.sourceEvents(events, block);
        for (const auto& event : events) {
            require(count < samples.size(), "reopened MIDI delivers one onset and one release");
            samples[count] = offset + event.frame;
            notes[count++] = event;
            if (event.type == EventType::NoteOn) source.noteOnDelivered(event);
            if (event.type == EventType::NoteOff) source.noteOffDelivered(event);
        }
        transport.advance(block);
    }
    require(count == 2 && samples[0] == 350 && samples[1] == 410 &&
            notes[0].type == EventType::NoteOn && notes[0].value == 99.0 / 127 &&
            notes[1].type == EventType::NoteOff && notes[1].value == 33.0 / 127 &&
            notes[0].noteId == notes[1].noteId && notes[0].channel == 2 && notes[0].dim == 60,
            "reopened MIDI renders the performed note timing, key, channel, and velocities exactly");
}

void captureAndReopen() {
    Project project;
    const auto original = digestProject(*project.store).text;
    const auto source = project.freeze();
    const auto plan = performance();
    const auto edit = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!edit.ok) throw std::runtime_error(edit.error);
    require(edit.requests.size() == 1 && edit.requests.front().opType == "capture.sessionPerformance", "one reserved capture op");
    require(digestProject(*project.store).text == original, "capture preparation writes nothing");
    const auto result = clip_edit::commitPrepared(*project.store, edit);
    if (!result.ok) throw std::runtime_error(result.error);

    require(project.count("SELECT count(*) FROM clip_slots WHERE clip_id IN (51,52)") == 2 &&
            project.count("SELECT count(*) FROM clips WHERE id=51 AND pos_ticks=0 AND content_offset_ticks=0") == 1,
            "capture keeps both original Session clips owned by their slots");
    require(project.count("SELECT count(*) FROM clips WHERE id IN (32,33) AND pos_ticks=0 AND length_ticks=240240") == 2,
            "nonparticipating track and take lane retain their original clips");
    require(project.count("SELECT count(*) FROM clips WHERE id=31 AND length_ticks=72072") == 1 &&
            project.count("SELECT count(*) FROM clips WHERE name='Arrangement' AND pos_ticks=168168 AND length_ticks=72072") == 1,
            "only the Session-owned part is cleared, including the explicit Stop interval");
    require(project.count("SELECT count(*) FROM clips WHERE name='Session notes' AND pos_ticks=72072 AND "
                          "length_ticks=48048 AND content_offset_ticks=12012 AND lane_id IS NULL") == 1,
            "captured clip preserves the performed source phase and Arrangement placement");
    require(project.count("SELECT count(*) FROM event_streams a,event_streams b JOIN clips c ON c.id=b.clip_id "
                          "WHERE a.clip_id=51 AND c.name='Session notes' AND c.id<>51 AND a.data=b.data AND a.id<>b.id") == 1,
            "new MIDI stream ID retains the exact authored notes and release bytes");
    const auto committed = digestProject(*project.store).text;
    require(History(*project.store).undo().ok && digestProject(*project.store).text == original,
            "one Undo restores the whole Arrangement and original clip rows");
    require(History(*project.store).redo().ok && digestProject(*project.store).text == committed,
            "one Redo applies the frozen literal data");

    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && digestProject(*project.store).text == committed,
            "reopen retains captured clips and their exact bytes");
    require(History(*project.store).undo().ok && digestProject(*project.store).text == original &&
            History(*project.store).redo().ok && digestProject(*project.store).text == committed,
            "reopen can replay the same literal capture op");
    verifyRenderedMidi(*project.store);
}

void refusals() {
    Project project;
    const auto source = project.freeze();
    const auto original = digestProject(*project.store).text;
    LaunchCapturePlan plan{LaunchCaptureError::None, {{23, 52, 0, {300, tick(300)}, {350, tick(350)}}}};
    plan.spans.front().fade = true;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "unsupported Session fade fails closed");
    plan = performance();
    plan.spans.front().velocity = 0.73f;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "unsupported velocity scaling fails closed");
    plan = performance();
    ++plan.spans.front().begin.sample;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "uncapturable sample edge fails closed");
    plan = performance();
    plan.spans.front().clip = 31;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok, "Arrangement is not an admitted Session source");
    const LaunchCapturePlan fractional{LaunchCaptureError::None,
        {{23, 52, 0, {300, tick(300)}, {550, tick(550)}}}};
    const auto refused = prepareSessionPerformance(*project.store, source, fractional, 48000);
    require(!refused.ok && refused.error.find("fractional") != std::string::npos, "fractional audio wrap requires exact performed ranges");
    require(digestProject(*project.store).text == original, "unsupported captures do not write any rows");

    OpRequest rename;
    rename.opType = "scene.rename";
    rename.payload = {{"id", 1}, {"name", "Changed"}};
    require(OpJournal(*project.store).commit(rename).ok, "change the frozen capture's journal head");
    require(!prepareSessionPerformance(*project.store, source, performance(), 48000).ok,
            "cannot reconstruct an in-flight capture from changed source rows");
}

void sourceMediaChanged() {
    Project project;
    const std::array<float, 1000> pcm{};
    const auto path = project.writeSource(pcm);
    const auto source = project.freeze();
    const auto original = digestProject(*project.store).text;
    const LaunchCapturePlan plan{LaunchCaptureError::None, {{23, 52, 0, {300, tick(300)}, {350, tick(350)}}}};
    const auto prepared = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    {
        std::ofstream changed(path, std::ios::binary | std::ios::app);
        changed.put('x');
        require(static_cast<bool>(changed), "change audio bytes after Arrangement preparation");
    }
    require(!clip_edit::commitPrepared(*project.store, prepared).ok && digestProject(*project.store).text == original,
            "late external media change rolls back rows and history");
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok,
            "original performed image does not adopt newly changed media");
}

void appliedSceneClock() {
    Project project;
    std::array<float, 1000> pcm{};
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<float>(i) / 1024;
    (void)project.writeSource(pcm);
    project.db().exec("UPDATE clips SET loop_enabled=0,loop_len_ticks=NULL,gain_db=6.020599913 WHERE id=52;"
                      "INSERT INTO clips(id,track_id,kind,name,pos_ticks,length_ticks) VALUES(63,23,'audio','Original',0,240240);"
                      "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(63,88,0,1000);"
                      "INSERT INTO tempo_map(id,pos_ticks,bpm,tension) VALUES(2,108108,100,0.25),(3,216216,90,0.125);"
                      "INSERT INTO time_signature_map(id,pos_ticks,numerator,denominator) VALUES(2,108108,5,4),(3,216216,7,8);"
                      "INSERT INTO automation_lanes(id,owner_kind,owner_id,param_ref) VALUES(91,'track',23,'volume');"
                      "INSERT INTO automation_data(id,lane_id,data) VALUES(92,91,X'01FF');");
    const auto source = project.freeze();
    const auto before = digestProject(*project.store).text;
    // Frame 300 is at 120 BPM. Launching 180 BPM there makes the next 300
    // old-tempo ticks take 200 frames, so the right survivor starts at 500.
    std::array performed{
        LaunchSceneTransition{.scene = 1, .sample = 300, .tick = tick(300), .ordinal = 1,
            .preparedToken = 7, .tempoBpm = 180, .signatureNumerator = 3, .signatureDenominator = 4},
        LaunchSceneTransition{.scene = 1, .sample = 400, .tick = tick(450), .ordinal = 2,
            .preparedToken = 8, .signatureNumerator = 9, .signatureDenominator = 8}};
    const LaunchCapturePlan plan{LaunchCaptureError::None,
        {{23, 52, tick(75), {300, tick(300)}, {500, tick(600)}}}};
    auto tape = [&performed] {
        return SessionPerformanceClock{{200, tick(200)}, {500, tick(600)}, 120, performed};
    };
    const SessionPerformanceAudio unchanged{Precision::Float32, true};
    ++performed[0].sample;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000, tape(), unchanged).ok,
            "scene tempo receipt must agree with the applied sample clock");
    --performed[0].sample;
    ++performed[1].sample;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000, tape(), unchanged).ok,
            "meter-only receipt must agree with the applied sample clock");
    --performed[1].sample;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000, tape()).ok &&
            !prepareSessionPerformance(*project.store, source, plan, 48000, tape(),
                SessionPerformanceAudio{Precision::Float64, true}).ok &&
            !prepareSessionPerformance(*project.store, source, plan, 48000, tape(),
                SessionPerformanceAudio{Precision::Float32, false}).ok,
            "audible gain requires a Float32 receipt without transient launch edits");
    const auto prepared = prepareSessionPerformance(*project.store, source, plan, 48000, tape(), unchanged);
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    if (!committed.ok) throw std::runtime_error(committed.error);
    require(prepared.requests.size() == 1 &&
            project.count("SELECT count(*) FROM tempo_map WHERE pos_ticks=72072 AND bpm=180") == 1 &&
            project.count("SELECT count(*) FROM tempo_map WHERE pos_ticks=144144 AND bpm=100") == 1 &&
            project.count("SELECT count(*) FROM tempo_map WHERE pos_ticks=108108") == 0 &&
            project.count("SELECT count(*) FROM tempo_map WHERE id=3 AND pos_ticks=216216 AND bpm=90 AND tension=0.125") == 1,
            "one capture installs performed tempo, resumes authored tempo, and preserves the future tempo row");
    require(project.count("SELECT count(*) FROM time_signature_map WHERE pos_ticks=72072 AND numerator=3 AND denominator=4") == 1 &&
            project.count("SELECT count(*) FROM time_signature_map WHERE pos_ticks=108108 AND numerator=9 AND denominator=8") == 1 &&
            project.count("SELECT count(*) FROM time_signature_map WHERE pos_ticks=144144 AND numerator=5 AND denominator=4") == 1 &&
            project.count("SELECT count(*) FROM time_signature_map WHERE id=3 AND pos_ticks=216216 AND numerator=7 AND denominator=8") == 1,
            "one capture writes both performed meters and preserves the authored meter after capture");
    require(project.count("SELECT count(*) FROM automation_data WHERE id=92 AND lane_id=91 AND data=X'01FF'") == 1,
            "performed tempo leaves track automation rows byte-identical");

    Transport transport;
    ClipPlayback playback(readClipProject(*project.store, rows::readModel(*project.store)),
                          transport, 48000.0, 1, 64, nullptr, true);
    const auto nodes = playback.sourcesFor(23);
    require(nodes.size() == 1 && playback.problems().empty(), "build the captured audio from the durable clock");
    transport.play();
    std::array<float, 640> rendered{};
    for (std::size_t frame = 0; frame < rendered.size(); frame += 64) {
        require(playback.prime(), "prime Arrangement capture pages");
        auto* target = rendered.data() + frame;
        NodeIo io;
        io.out = &target;
        io.channels = 1;
        io.frames = 64;
        io.sampleRate = 48000;
        nodes.front()->process(io);
        transport.advance(io.frames);
    }
    for (std::size_t frame = 0; frame < rendered.size(); ++frame) {
        const bool captured = frame >= 300 && frame < 500;
        const auto expected = captured ? 2 * pcm[frame - 250] : pcm[frame];
        require(rendered[frame] == expected, "exact varying gained PCM at the applied tempo and both Arrangement edges");
    }
    const auto after = digestProject(*project.store).text;
    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && digestProject(*project.store).text == after &&
            History(*project.store).undo().ok && digestProject(*project.store).text == before &&
            History(*project.store).redo().ok && digestProject(*project.store).text == after,
            "reopened Undo and Redo cover tempo, meter, clips and physical survivor offsets");
}

struct HeardNote {
    std::int64_t sample = 0;
    EventType type = EventType::NoteOn;
    std::uint8_t channel = 0;
    std::uint16_t dim = 0;
    std::uint32_t param = 0;
    double value = 0;
    bool operator==(const HeardNote&) const = default;
};

struct NoteProbe final : device::DeviceInstance {
    device::DeviceIdentity identityRow;
    std::array<HeardNote, 24> notes{};
    std::array<std::uint64_t, 24> voices{};
    std::size_t count = 0;
    std::int64_t sample = 0;

    const device::DeviceIdentity& identity() const noexcept override { return identityRow; }
    bool loaded() const noexcept override { return true; }
    EventFlow eventFlow() const noexcept override { return EventFlow::Consume; }
    void process(const NodeIo& io) noexcept override {
        for (const auto& event : io.events) {
            if (!isNoteStream(event.type) || count == notes.size()) continue;
            notes[count] = {sample + event.frame, event.type, event.channel, event.dim, event.paramId, event.value};
            voices[count++] = event.noteId;
        }
        for (std::int32_t channel = 0; channel < io.channels; ++channel)
            std::fill_n(io.out[channel] + io.blockOffset, io.frames, 0.0f);
    }
};

std::vector<HeardNote> renderMidi(Store& store, LaunchCaptureBuffer* tape, std::int64_t launch = 55) {
    Session session;
    session.graph().setFadeFrames(0);
    NoteProbe* probe = nullptr;
    const SessionSpec spec{2, 48000, 257};
    if (!session.load(store, [&probe](const DeviceRequest&, std::string&) {
            auto instrument = std::make_unique<NoteProbe>();
            probe = instrument.get();
            return instrument;
        }, spec)) throw std::runtime_error(session.error());
    require(probe && session.problems().empty(), "load the actual native MIDI graph");
    session.transport().play();
    std::array<float, 514> pcm{};
    std::array output{pcm.data(), pcm.data() + 257};
    std::int64_t at = 0;
    for (const auto frames : {55, 197, 257, 221, 79}) {
        if (tape && (at == launch || at == 730)) {
            LaunchRequest request;
            request.kind = at == launch ? LaunchRequestKind::SlotPress : LaunchRequestKind::BackToArrangement;
            request.track = 21;
            request.scene = 1;
            request.clip = 51;
            request.incarnation = session.launchIncarnation();
            require(session.postLaunchGesture(std::array{request}), "apply the launched MIDI source or Arrangement return");
        }
        AudioIo io;
        io.out = output.data();
        io.numOut = 2;
        io.frames = frames;
        io.streamTimeSamples = at;
        probe->sample = at;
        auto block = session.beginRender();
        if (tape) block.observeLaunchCapture(*tape);
        require(block.process(io), "render a native note/expression trace");
        at += frames;
    }
    require(probe->count < probe->notes.size(), "the MIDI trace fits the probe buffer");
    std::uint64_t voice = 0;
    for (std::size_t i = 0; i < probe->count; ++i) {
        if (probe->notes[i].type == EventType::Controller) continue;
        if (voice == 0) voice = probe->voices[i];
        require(voice != 0 && probe->voices[i] == voice, "all observed note events belong to one voice");
    }
    return {probe->notes.begin(), probe->notes.begin() + static_cast<std::ptrdiff_t>(probe->count)};
}

void prepareMidiProbe(Project& project, float tuningCents) {
    project.db().exec("DELETE FROM clip_slots WHERE clip_id=52; DELETE FROM clips WHERE id=52;"
        "UPDATE clips SET length_ticks=216216 WHERE id=51;"
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(99,'master','Main',3);"
        "INSERT INTO clip_launch(clip_id,launch_quant) VALUES(51,1);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(101,'vst3','test.native','Probe','instrument');"
        "INSERT INTO device_chains(id,track_id) VALUES(101,21);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(101,101,0,101,'Probe');");
    test::seedSessionMarker(project.db());
    NoteRecord note{};
    note.start_ticks = tick(20);
    note.dur_ticks = tick(700);
    note.note_id = 176;
    note.key = 64;
    note.vel_on = 99;
    note.vel_off = 33;
    note.probability = 10000;
    note.tuning_cents = tuningCents;
    require(project.store->putEventStream(51, "notes", writeStream<NoteRecord>(FourCC::Notes, std::array{note})),
            "store a native note crossing the capture exit");
}

void expressionCapture(Curve shape) {
    Project project;
    prepareMidiProbe(project, 31.25f);
    const auto encoded = static_cast<std::uint8_t>(shape);
    const std::array<ExpressionPoint, 3> points{{{0, 1.0f, 0.375f, encoded, 0, {}},
        {tick(200), 3.0f, -0.625f, encoded, 0, {}}, {tick(500), -0.5f, 0, 0, 0, {}}}};
    const auto bytes = writeStream<ExpressionPoint>(FourCC::Expression, points);
    {
        SQLite::Statement insert(project.db(), "INSERT INTO note_expression(clip_id,note_id,dimension,data) VALUES(51,176,0,?)");
        insert.bind(1, bytes.data(), static_cast<int>(bytes.size()));
        insert.exec();
    }
    const auto source = project.freeze();
    std::array<LaunchTransition, 8> edges{};
    LaunchCaptureBuffer tape(edges);
    const auto heard = renderMidi(*project.store, &tape);
    const bool held = shape == Curve::Hold;
    require(heard.size() == (held ? 5u : 10u) && heard[0].sample == 75 && heard[0].type == EventType::NoteOn &&
            heard[1].sample == 75 && heard[1].value == 1.3125 &&
            heard[held ? 2u : 4u].sample == 275 && heard[held ? 2u : 4u].value == 3.3125 &&
            heard[held ? 3u : 8u].sample == 575 && heard[held ? 3u : 8u].value == -0.1875 &&
            heard.back().sample == 730 && heard.back().type == EventType::NoteOff,
            "the native Session graph emits exact pitch knots and release");
    if (!held)
        require(heard[5].sample == 288 && heard[5].type == EventType::NoteExpression &&
            heard[5].value > -0.1875 && heard[5].value < 3.3125,
            "the native graph evaluates a shaped value on the interior expression grid");
    const auto plan = planLaunchCapture(tape, {0, 0}, {809, tick(809)}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1, "capture the run from its authored start");
    auto chased = plan;
    chased.spans.front().sourceTick = tick(10);
    const auto unsafe = prepareSessionPerformance(*project.store, source, chased, 48000);
    require(!unsafe.ok && unsafe.error.find("authored start") != std::string::npos,
            "an expression run entering at non-native source phase refuses exact capture");
    const auto before = digestProject(*project.store).text;
    const auto prepared = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    if (!committed.ok) throw std::runtime_error(committed.error);
    require(project.count("SELECT count(*) FROM note_expression a,note_expression b "
        "WHERE a.clip_id=51 AND b.clip_id<>51 AND a.data=b.data AND a.note_id=b.note_id") == 1,
        "literal Arrangement capture retains every original AEXP byte and note identity");
    const auto after = digestProject(*project.store).text;
    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && renderMidi(*project.store, nullptr) == heard,
        "reopened Arrangement delivers the same ordered note, pitch, and release values at every sample");
    require(History(*project.store).undo().ok && digestProject(*project.store).text == before &&
            History(*project.store).redo().ok && digestProject(*project.store).text == after,
            "one reopened Undo and Redo include the literal AEXP source image");
}

void heldControllerCapture() {
    Project project;
    prepareMidiProbe(project, 0.0f);
    const std::array points{controllers::Point{1, 0, 0.25, 0, 0, 0},
        controllers::Point{2, tick(493), 0.75, 0, 0, 0}};
    const auto control = controllers::prepareWrite(*project.store, 51, 3, 7, points);
    require(control.ok && controllers::commitPrepared(*project.store, control).ok, "store unshared CC7 at two held knots");
    const auto source = project.freeze();
    std::array<LaunchTransition, 8> edges{};
    LaunchCaptureBuffer tape(edges);
    const auto heard = renderMidi(*project.store, &tape);
    require(heard.size() == 4 && heard[0] == HeardNote{55, EventType::Controller, 3, 0, 7, 0.25} &&
        heard[1].sample == 75 && heard[1].type == EventType::NoteOn &&
        heard[2] == HeardNote{548, EventType::Controller, 3, 0, 7, 0.75} &&
        heard[3].sample == 730 && heard[3].type == EventType::NoteOff,
        "the real Session graph emits held CC knots and a note without an extra exit controller");
    const auto plan = planLaunchCapture(tape, {0, 0}, {809, tick(809)}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1, "capture the isolated held CC run");
    const auto prepared = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!prepared.ok) throw std::runtime_error(prepared.error);

    project.db().exec("INSERT INTO event_streams(id,clip_id,stream_kind,channel,controller,data) "
        "SELECT 997,31,stream_kind,channel,controller,data FROM event_streams WHERE clip_id=51 AND stream_kind='cc'");
    const auto competing = digestProject(*project.store).text;
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok &&
        !prepareSessionPerformance(*project.store, project.freeze(), plan, 48000).ok &&
        !clip_edit::commitPrepared(*project.store, prepared).ok && digestProject(*project.store).text == competing,
        "another same-address Arrangement owner refuses capture and rolls back an already prepared commit");
    project.db().exec("DELETE FROM event_streams WHERE id=997;"
        "UPDATE event_streams SET controller=64 WHERE clip_id=51 AND stream_kind='cc'");
    require(!prepareSessionPerformance(*project.store, source, plan, 48000).ok &&
        !prepareSessionPerformance(*project.store, project.freeze(), plan, 48000).ok,
        "an altered address and a pedal are each refused");
    project.db().exec("UPDATE event_streams SET controller=7 WHERE clip_id=51 AND stream_kind='cc'");
    auto moving = points;
    moving.front().curve = static_cast<std::uint8_t>(Curve::Linear);
    const auto writeMoving = controllers::prepareWrite(*project.store, 51, 3, 7, moving);
    require(writeMoving.ok && controllers::commitPrepared(*project.store, writeMoving).ok &&
            !prepareSessionPerformance(*project.store, project.freeze(), plan, 48000).ok,
            "a moving controller and its final-frame probe are outside exact capture");
    require(History(*project.store).undo().ok, "restore the held controller before committing its capture");
    const auto before = digestProject(*project.store).text;
    const auto edit = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!edit.ok) throw std::runtime_error(edit.error);
    const auto committed = clip_edit::commitPrepared(*project.store, edit);
    if (!committed.ok) throw std::runtime_error(committed.error);
    require(project.count("SELECT count(*) FROM event_streams a,event_streams b WHERE a.clip_id=51 "
        "AND b.clip_id<>51 AND a.stream_kind='cc' AND b.stream_kind='cc' AND a.channel=b.channel "
        "AND a.controller=b.controller AND a.id<>b.id AND a.data=b.data") == 1,
        "Arrangement capture keeps each original ACTL byte, channel and address");
    const auto after = digestProject(*project.store).text;
    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && renderMidi(*project.store, nullptr) == heard &&
        History(*project.store).undo().ok && digestProject(*project.store).text == before &&
        History(*project.store).redo().ok && digestProject(*project.store).text == after,
        "reopened Arrangement matches the literal CC performance and one Undo and Redo");
}

void slotPitchWheel() {
    Project project;
    prepareMidiProbe(project, 0.0f);
    const std::array points{controllers::Point{1, 0, 0.25}, controllers::Point{2, tick(493), 0.75}};
    for (const auto address : {7, static_cast<int>(controllers::kPitchBend)}) {
        const auto wheel = controllers::prepareWrite(*project.store, 51, 3, address, points);
        require(wheel.ok && controllers::commitPrepared(*project.store, wheel).ok,
                "store independently addressed CC7 and pitch wheel in the slot");
    }
    std::array<LaunchTransition, 8> edges{};
    LaunchCaptureBuffer tape(edges);
    const auto heard = renderMidi(*project.store, &tape);
    const auto wheel = static_cast<std::uint32_t>(controllers::kPitchBend);
    require(heard.size() == 7 &&
        heard[0] == HeardNote{55, EventType::Controller, 3, 0, 7, 0.25} &&
        heard[1] == HeardNote{55, EventType::Controller, 3, 0, wheel, 0.25} &&
        heard[2].sample == 75 && heard[2].type == EventType::NoteOn &&
        heard[3] == HeardNote{548, EventType::Controller, 3, 0, 7, 0.75} &&
        heard[4] == HeardNote{548, EventType::Controller, 3, 0, wheel, 0.75} &&
        heard[5].sample == 730 && heard[5].type == EventType::NoteOff &&
        heard[6] == HeardNote{730, EventType::Controller, 3, 0, wheel, 0.5},
        "Session chases the stored wheel on launch and releases it at the Arrangement handoff");
    project.db().exec("DELETE FROM event_streams WHERE clip_id=51 AND stream_kind='cc'");
    const auto plan = planLaunchCapture(tape, {0, 0}, {809, tick(809)}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1 &&
        !prepareSessionPerformance(*project.store, project.freeze(), plan, 48000).ok,
        "performance capture refuses an unproven pitch wheel release even without ordinary CC");
    std::puts("PASS -- Session plays and releases its native pitch wheel lane");
}

void centeredPitchWheelCapture() {
    Project project;
    prepareMidiProbe(project, 0.0f);
    const std::array points{controllers::Point{1, 0, 0.25}, controllers::Point{2, tick(493), 0.75},
                            controllers::Point{3, tick(600), 0.5}};
    const auto wheel = controllers::prepareWrite(*project.store, 51, 3, controllers::kPitchBend, points);
    require(wheel.ok && controllers::commitPrepared(*project.store, wheel).ok, "store a pitch wheel that returns to center");
    const auto source = project.freeze();
    std::array<LaunchTransition, 8> edges{};
    LaunchCaptureBuffer tape(edges);
    const auto heard = renderMidi(*project.store, &tape);
    const auto address = static_cast<std::uint32_t>(controllers::kPitchBend);
    require(heard.size() == 5 && heard[0] == HeardNote{55, EventType::Controller, 3, 0, address, 0.25} &&
        heard[2] == HeardNote{548, EventType::Controller, 3, 0, address, 0.75} &&
        heard[3] == HeardNote{655, EventType::Controller, 3, 0, address, 0.5},
        "Session renders the authored center before the exit with no extra reset");
    const auto plan = planLaunchCapture(tape, {0, 0}, {809, tick(809)}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1, "capture the isolated wheel run");
    const auto edit = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!edit.ok) throw std::runtime_error(edit.error);
    const auto before = digestProject(*project.store).text;
    const auto committed = clip_edit::commitPrepared(*project.store, edit);
    if (!committed.ok) throw std::runtime_error(committed.error);
    require(project.count("SELECT count(*) FROM event_streams a,event_streams b WHERE a.clip_id=51 "
        "AND b.clip_id<>51 AND a.stream_kind='pitchbend' AND b.stream_kind='pitchbend' AND a.channel=b.channel "
        "AND a.controller IS NULL AND b.controller IS NULL AND a.data=b.data") == 1,
        "Arrangement stores the native pitch wheel stream and its exact bytes");
    const auto after = digestProject(*project.store).text;
    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && renderMidi(*project.store, nullptr) == heard &&
        History(*project.store).undo().ok && digestProject(*project.store).text == before &&
        History(*project.store).redo().ok && digestProject(*project.store).text == after,
        "reopened Arrangement matches the wheel and note trace through Undo and Redo");
    std::puts("PASS -- an isolated pitch wheel returning to center replays exact MIDI samples and bytes");
}

void multipleHeldControllersCapture() {
    Project project;
    prepareMidiProbe(project, 0.0f);
    const std::array volume{controllers::Point{1, 0, 0.25, 0, 0, 0},
        controllers::Point{2, tick(493), 0.75, 0, 0, 0}};
    const std::array cutoff{controllers::Point{3, 0, 0.125, 0, 0, 0},
        controllers::Point{4, tick(331), 0.5, 0, 0, 0}};
    const auto write = [&](int channel, int address, std::span<const controllers::Point> points) {
        const auto prepared = controllers::prepareWrite(*project.store, 51, channel, address, points);
        require(prepared.ok && controllers::commitPrepared(*project.store, prepared).ok, "store authored held CC");
    };
    // Authored order differs from address order. Different channels also
    // make this an exact check of both parts of each native address.
    write(3, 7, volume);
    write(1, 74, cutoff);
    const auto source = project.freeze();
    std::array<LaunchTransition, 8> edges{};
    LaunchCaptureBuffer tape(edges);
    const auto heard = renderMidi(*project.store, &tape, 0);
    require(heard.size() == 6 &&
        heard[0] == HeardNote{0, EventType::Controller, 1, 0, 74, 0.125} &&
        heard[1] == HeardNote{0, EventType::Controller, 3, 0, 7, 0.25} &&
        heard[2].sample == 20 && heard[2].type == EventType::NoteOn &&
        heard[3] == HeardNote{331, EventType::Controller, 1, 0, 74, 0.5} &&
        heard[4] == HeardNote{493, EventType::Controller, 3, 0, 7, 0.75} &&
        heard[5].sample == 720 && heard[5].type == EventType::NoteOff,
        "Session chases both lanes in address order and sends subsequent held changes at their distinct samples");
    const auto plan = planLaunchCapture(tape, {0, 0}, {809, tick(809)}, {});
    require(plan.error == LaunchCaptureError::None && plan.spans.size() == 1, "capture both held lanes in one run");

    auto delayed = plan;
    delayed.spans.front().begin = {55, tick(55)};
    const auto unsafeStart = prepareSessionPerformance(*project.store, source, delayed, 48000);
    require(!unsafeStart.ok && unsafeStart.error.find("song start") != std::string::npos,
        "a later simultaneous chase does not have a known Arrangement address order");
    auto coincident = cutoff;
    coincident.back().timeTicks = volume.back().timeTicks;
    write(1, 74, coincident);
    const auto unsafeKnots = prepareSessionPerformance(*project.store, project.freeze(), plan, 48000);
    require(!unsafeKnots.ok && unsafeKnots.error.find("same sample") != std::string::npos,
        "later same-frame changes have an ambiguous ordering under Arrangement's rotating cursor");
    require(History(*project.store).undo().ok, "restore the staggered controller knots");

    const auto prepared = prepareSessionPerformance(*project.store, source, plan, 48000);
    if (!prepared.ok) throw std::runtime_error(prepared.error);
    project.db().exec("UPDATE event_streams SET controller=75 WHERE clip_id=51 AND stream_kind='cc' AND controller=74");
    const auto altered = digestProject(*project.store).text;
    require(!clip_edit::commitPrepared(*project.store, prepared).ok && digestProject(*project.store).text == altered,
        "one late controller address change rolls back every captured lane atomically");
    project.db().exec("UPDATE event_streams SET controller=74 WHERE clip_id=51 AND stream_kind='cc' AND controller=75");
    const auto before = digestProject(*project.store).text;
    const auto committed = clip_edit::commitPrepared(*project.store, prepared);
    if (!committed.ok) throw std::runtime_error(committed.error);
    require(project.count("SELECT count(*) FROM event_streams a,event_streams b WHERE a.clip_id=51 "
        "AND b.clip_id<>51 AND a.stream_kind='cc' AND b.stream_kind='cc' AND a.channel=b.channel "
        "AND a.controller=b.controller AND a.id<>b.id AND a.data=b.data") == 2,
        "the Arrangement copied both literal ACTL streams with fresh row IDs");
    const auto after = digestProject(*project.store).text;
    project.store.reset();
    StoreError error{};
    project.store = Store::open(project.path, error, false);
    require(project.store && renderMidi(*project.store, nullptr) == heard &&
        History(*project.store).undo().ok && digestProject(*project.store).text == before &&
        History(*project.store).redo().ok && digestProject(*project.store).text == after,
        "reopened Arrangement matches the ordered multi-controller trace and one Undo and Redo");
    std::puts("PASS -- two held MIDI controllers replay in exact frame and address order");
}

} // namespace

int main() {
    try {
        captureAndReopen();
        refusals();
        sourceMediaChanged();
        appliedSceneClock();
        for (const auto shape : {Curve::Hold, Curve::Linear, Curve::Exponential, Curve::Logarithmic,
                                Curve::SCurve, Curve::Bezier})
            expressionCapture(shape);
        heldControllerCapture();
        slotPitchWheel();
        centeredPitchWheelCapture();
        multipleHeldControllersCapture();
        std::printf("Session performance capture replays literal Arrangement ranges and native MIDI bytes.\n");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
