#include "adi/digest.hpp"
#include "adi/bounce_commands.hpp"
#include "adi/engine/snapshot.hpp"
#include <algorithm>
#include "adi/freeze_commands.hpp"
#include "adi/freeze_state.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "freeze_fixture.hpp"
#include "replace_operator_new.hpp"
#include <cstdlib>
#include <new>
namespace {
std::atomic<std::uint32_t> callbackAllocations{0};
}
void adi::test::countReplacedNew() noexcept {
    if (adi::audio::onAudioThread)
        callbackAllocations.fetch_add(1, std::memory_order_relaxed);
}
namespace {
OpRequest op(const char *type, Payload payload) {
    OpRequest q;
    q.opType = type;
    q.payload = std::move(payload);
    return q;
}
render::FreezeAudioCapture capture(Project &p, Counts &counts,
                                   engine::Precision precision = engine::Precision::Float32) {
    auto spec = p.spec(precision);
    spec.endSample = 3000;
    return render::captureFreezeAudio(*p.store, spec, p.loader(counts));
}
bool heard(engine::Session &session, float gain, float dryGain = 1.0f) {
    session.transport().locate(0);
    session.transport().play();
    if (!session.clips() || !session.clips()->prime())
        return false;
    std::array<float, 64> left{}, right{};
    float *outputs[]{left.data(), right.data()};
    engine::AudioIo io;
    io.out = outputs;
    io.numOut = 2;
    io.frames = 64;
    const auto before = audio::callbackFileIo.load();
    const auto beforeAlloc = callbackAllocations.load();
    {
        audio::CallbackScope mark;
        session.process(io);
    }
    if (audio::callbackFileIo.load() != before || callbackAllocations.load() != beforeAlloc)
        return false;
    const auto wet = session.offlineTapOutput(1, engine::Tap::PostFx),
               dry = session.offlineTapOutput(1, engine::Tap::PreFx);
    if (!wet || !dry)
        return false;
    std::array<double, 128> pcm{};
    double *copied[]{pcm.data(), pcm.data() + 64};
    engine::AudioIo copy;
    copy.out64 = copied;
    copy.numOut = 2;
    copy.frames = 64;
    if (!session.graph().currentGraph()->copyNodeOutput(wet->node, copy))
        return false;
    for (unsigned f = 0; f < 64; ++f)
        for (unsigned c = 0; c < 2; ++c)
            if (std::abs(pcm[c * 64 + f] - gain * signal(f, c)) > 1e-6)
                return false;
    if (!session.graph().currentGraph()->copyNodeOutput(dry->node, copy))
        return false;
    for (unsigned f = 0; f < 64; ++f)
        for (unsigned c = 0; c < 2; ++c)
            if (std::abs(pcm[c * 64 + f] - dryGain * signal(f, c)) > 1e-6)
                return false;
    return true;
}
void mainLaneExtentMatrix() {
    constexpr std::int64_t rate = 48000, mainFrames = 6 * rate;
    const auto exactPcm = [](const fs::path &path, std::int64_t expectedFrames) {
        audio::WavReader reader(path);
        if (reader.channels() != 2 || reader.sampleRate() != 48000 ||
            reader.format() != audio::WavFormat::Float32 ||
            reader.frames() != static_cast<std::uint64_t>(expectedFrames))
            return false;
        std::array<float, 8192> samples{};
        std::uint64_t first = 0;
        for (;;) {
            const auto count = reader.read(samples.data(), 4096);
            if (!count)
                break;
            for (std::uint32_t frame = 0; frame < count; ++frame)
                for (unsigned channel = 0; channel < 2; ++channel) {
                    const auto expected = first + frame < 6u * 48000u
                                              ? (channel == 0 ? 0.25f : -0.125f)
                                              : 0.0f;
                    if (samples[static_cast<std::size_t>(frame) * 2 + channel] != expected)
                        return false;
                }
            first += count;
        }
        return first == static_cast<std::uint64_t>(expectedFrames);
    };
    // Eight independent cases: both time bases, standalone/group hierarchy,
    // and main-only/with a later unmuted take containing nonzero source audio.
    for (const bool ticks : {false, true})
        for (const bool group : {false, true})
            for (const bool take : {false, true}) {
                test::TempDirectory directory{"freeze_commands", "main_lane_extent"};
                StoreError error{};
                auto store = Store::create(directory.path() / "source.adi", error);
                if (!store)
                    throw std::runtime_error(toString(error));
                const std::string label = std::string(ticks ? "ticks/" : "ns/") +
                                          (group ? "group/" : "track/") + (take ? "take" : "main");
                const auto expect = [&label](bool ok, const char *message) {
                    const auto text = label + ": " + message;
                    check(ok, text.c_str());
                };
                auto &db = store->db();
                db.exec("UPDATE project SET sample_rate=48000;DELETE FROM tempo_map;"
                        "INSERT INTO tempo_map(id,pos_ticks,bpm) VALUES(1,0,120);"
                        "INSERT INTO tracks(id,kind,name,index_in_parent) "
                        "VALUES(1,'audio','Source',0),(9,'master','Main',1);"
                        "INSERT INTO mixer_strip(track_id) VALUES(1),(9);");
                if (group)
                    db.exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(10,'group','Parent',2);"
                            "INSERT INTO mixer_strip(track_id) VALUES(10);UPDATE tracks SET parent_id=10 WHERE id=1;");
                const auto source = directory.path() / "input.wav";
                {
                    audio::WavWriter writer(source, 48000, 2, audio::WavFormat::Float32);
                    std::array<float, 2048> samples{};
                    for (std::size_t frame = 0; frame < samples.size() / 2; ++frame) {
                        samples[frame * 2] = 0.25f;
                        samples[frame * 2 + 1] = -0.125f;
                    }
                    for (std::int64_t first = 0; first < mainFrames; first += 1024)
                        writer.write(samples.data(), static_cast<std::uint32_t>(
                                                         std::min<std::int64_t>(1024, mainFrames - first)));
                    writer.close();
                }
                OpRequest importRequest;
                const auto imported = media::prepareImport(*store, source, 1, 0, importRequest);
                if (!imported.ok || !OpJournal(*store).commit(importRequest).ok)
                    throw std::runtime_error("main-lane extent media import");
                engine::TempoMap tempo;
                tempo.events.push_back({0, 120, 0});
                const auto addClip = [&](int id, int seconds, bool inTake) {
                    const auto position = ticks ? tempo.secondsToTicks(seconds)
                                                : static_cast<std::int64_t>(seconds) * 1000000000;
                    const auto length = ticks ? tempo.secondsToTicks(6) : std::int64_t{6000000000};
                    db.exec("INSERT INTO clips(id,track_id,lane_id,kind,time_base," +
                            std::string(ticks ? "pos_ticks,length_ticks" : "pos_ns,length_ns") +
                            ") VALUES(" + std::to_string(id) + ",1," + (inTake ? "7" : "NULL") +
                            ",'audio'," + (ticks ? "0" : "1") + "," + std::to_string(position) + "," +
                            std::to_string(length) + ");INSERT INTO "
                            "audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(" +
                            std::to_string(id) + ",1,0,288000);");
                };
                addClip(1, 0, false);
                if (take) {
                    db.exec("INSERT INTO lanes(id,track_id,kind,name) VALUES(7,1,'take','Later take');");
                    addClip(2, 8, true);
                }
                const auto verifyRange = [&](std::int64_t seconds) {
                    const auto before = digestProject(*store).text;
                    render::FreezeCaptureSpec spec;
                    spec.tracks = {group ? 10 : 1};
                    spec.wholeGroups = group;
                    spec.endSample = seconds * rate;
                    spec.session = {2, 48000, 256};
                    auto captured = render::captureFreezeAudio(*store, spec);
                    expect(captured.ready(), "actual native capture is ready");
                    if (captured.ready()) {
                        expect(captured.streams().size() == 1, "one selected freeze endpoint is captured");
                        if (captured.streams().size() == 1) {
                            const auto &stream = captured.streams().front();
                            expect(exactPcm(stream.preFx, spec.endSample),
                                   "pre-FX PCM contains six exact source seconds then silence");
                            expect(exactPcm(stream.postFx, spec.endSample),
                                   "post-FX PCM contains six exact source seconds then silence");
                        }
                        const auto frozen = freeze::prepareFreeze(*store, captured, 0, 100);
                        expect(frozen.ready(), "Freeze extent covers audible main clips without requiring later takes");
                    }
                    // Bounce tests the regular source track in both hierarchies;
                    // whole-group Bounce is not part of this command's contract.
                    clip_edit::InPlaceBounceOptions options;
                    options.sourceTrack = 1;
                    options.clipId = 50;
                    options.mediaId = 60;
                    options.job.beginSample = 0;
                    options.job.endSample = spec.endSample;
                    options.job.session = spec.session;
                    options.job.encoding.format = audio::WavFormat::Float32;
                    options.job.encoding.dither = audio::WavDither::None;
                    const auto bounced = clip_edit::prepareBounceInPlace(*store, options);
                    expect(bounced.ready(), "Bounce in Place extent covers audible main clips without requiring later takes");
                    expect(digestProject(*store).text == before, "both preparations leave original project rows unchanged");
                };
                verifyRange(7); // Main at 0..6 s; later take at 8..14 s must not extend it.
                if (!ticks && !group && take)
                    verifyRange(14); // Prove the nonzero later take is inaudible even inside the captured range.
            }
}

void lifecycle() {
    for (const auto precision : {engine::Precision::Float32, engine::Precision::Float64}) {
        Project p;
        Counts counts;
        p.store->db().exec("UPDATE tracks SET record_armed=1 WHERE id=1");
        const auto original = digestProject(*p.store).text;
        auto captured = capture(p, counts, precision);
        check(captured.ready(), "native capture completed");
        auto prepared = freeze::prepareFreeze(*p.store, captured, 0, 100);
        check(prepared.ready(), prepared.error().c_str());
        check(!freeze::commitFreeze(*p.store, prepared, 1).ok,
              "replaced document generation refuses a prepared freeze");
        auto committed = freeze::commitFreeze(*p.store, prepared, 0);
        check(committed.ok, committed.error.c_str());
        check(freeze::renders(p.store->db()).size() == 2 && freeze::state(p.store->db(), 1).at("armed") == 0,
              "one native batch freezes/disarms both tracks and retains provenance");
        check(!freeze::commitFreeze(*p.store, prepared, 0).ok, "committed capture cannot be committed twice");
        History history(*p.store);
        check(history.undo().ok && freeze::renders(p.store->db()).empty(),
              "one undo removes the entire multi-track freeze");
        check(digestProject(*p.store).text == original,
              "undo restores original material, state, metadata and armed switch exactly");
        check(history.redo().ok && freeze::renders(p.store->db()).size() == 2,
              "redo restores both immutable caches");
        CommitOptions injected;
        injected.beforeCommit = [](SQLite::Database &db, std::int64_t, std::string &) {
            db.exec("UPDATE clips SET gain_db=8 WHERE id=1");
            return true;
        };
        const auto injectedFader = op("mixer.setVolume", {{"id", 1}, {"db", -3.0}});
        check(!OpJournal(*p.store).commit(std::span<const OpRequest>(&injectedFader, 1), injected).ok,
              "an opaque commit hook cannot bypass source guards through a live-control batch");
        p.store->db().exec("UPDATE clips SET gain_db=8 WHERE id=1");
        check(!OpJournal(*p.store).commit(op("mixer.setVolume", {{"id", 1}, {"db", -3.0}})).ok,
              "direct SQL invalidates the fast live-control validation token");
        p.store->db().exec("UPDATE clips SET gain_db=0 WHERE id=1");
        const auto frozenHead = OpJournal(*p.store).headSeq();
        check(!OpJournal(*p.store).commit(op("clip.setGain", {{"id", 1}, {"gain", 6.0}})).ok &&
                  OpJournal(*p.store).headSeq() == frozenHead,
              "baked clip edits roll back without moving history");
        check(!OpJournal(*p.store).commit(op("mixer.setInputGain", {{"id", 1}, {"db", 6.0}})).ok,
              "baked input gain stays locked");
        check(!OpJournal(*p.store).commit(op("track.setArm", {{"id", 1}, {"armed", true}})).ok,
              "native arm cannot record into a parked source");
        check(OpJournal(*p.store).commit(op("mixer.setVolume", {{"id", 1}, {"db", -6.0}})).ok,
              "post-effect frozen fader stays live");
        check(history.undo().ok, "undo of a live fader preserves cache validity");
        auto renamed = p.root.path() / "temporarily-unavailable.wav";
        fs::rename(p.root.path() / "source.wav", renamed);
        engine::Session session;
        int loads = 0;
        auto noPlugins = [&](const engine::DeviceRequest &, std::string &) {
            ++loads;
            return std::unique_ptr<device::DeviceInstance>{};
        };
        auto spec = p.spec(precision).session;
        const bool loaded = session.load(*p.store, noPlugins, spec);
        check(loaded, session.error().c_str());
        check(loads == 0 && session.entryCount() == 0 && session.problems().empty(),
              "cached open never loads missing parked plug-ins or original media");
        check(heard(session, 0.25f),
              "actual frozen dry and wet samples play at both graph widths without callback I/O");
        const auto path = p.store->path();
        p.store.reset();
        StoreError error{};
        p.store = Store::open(path, error);
        check(p.store && freeze::renders(p.store->db()).size() == 2, "freeze survives close/reopen");
        fs::rename(renamed, p.root.path() / "source.wav");
        const std::array<std::int64_t, 2> ids{1, 2};
        check(!freeze::unfreeze(*p.store, ids, false,
                                [](std::int64_t, const Payload &, std::string &errorText) {
                                    errorText = "installed plug-in version changed";
                                    return false;
                                })
                   .ok,
              "unfreeze reports changed installed identities before touching state");
        check(freeze::unfreeze(*p.store, ids).ok && freeze::state(p.store->db(), 1).at("armed") == 1,
              "unfreeze restores original armed state and leaves pooled state intact");
        check(History(*p.store).undo().ok && freeze::renders(p.store->db()).size() == 2,
              "unfreeze is itself one undo step");
        const auto frozenBefore = digestProject(*p.store).text;
        check(OpJournal(*p.store).commit(op("track.delete", {{"id", 1}})).ok,
              "frozen track delete captures new dependent metadata");
        check(History(*p.store).undo().ok && digestProject(*p.store).text == frozenBefore,
              "native track-delete undo restores a frozen cache and all original state");
    }
}
void flattenFrozenTrack() {
    Project p;
    auto &db = p.store->db();
    db.exec("UPDATE clips SET name='Original Clip' WHERE id=1;"
            "UPDATE tracks SET monitor_mode=1 WHERE id=1;"
            "UPDATE mixer_strip SET input_gain_db=6,volume_db=-9,pan=0.25 WHERE track_id=1;"
            "INSERT INTO lanes(id,track_id,name) VALUES(20,1,'Earlier Take');"
            "INSERT INTO clips(id,track_id,lane_id,kind,time_base,pos_ns,length_ns) "
            "VALUES(20,1,20,'audio',1,0,62500000);"
            "INSERT INTO audio_clips(clip_id,media_id,src_start_frames,src_len_frames) VALUES(20,1,0,3000)");
    const auto route = OpJournal(*p.store).commit(op("routing.connect", {{"id", 30}, {"srcKind", "track"},
        {"src", 1}, {"dstKind", "track"}, {"dst", 9}, {"kind", "main"}}));
    if (!route.ok)
        throw std::runtime_error(route.error);
    check(!freeze::flatten(*p.store, 1, 40).ok, "SC-86: Flatten refuses an unfrozen track");
    Counts counts;
    auto prepared = freeze::prepareFreeze(*p.store, capture(p, counts), 0, 100);
    const auto frozen = freeze::commitFreeze(*p.store, prepared, 0);
    if (!frozen.ok)
        throw std::runtime_error(frozen.error);
    const auto before = digestProject(*p.store).text;
    const auto cached = freeze::state(db, 1).at("render").at("post_media_id").get<std::int64_t>();
    std::string error;
    const auto wet = media::resolveMedia(media::projectFolder(*p.store), media::mediaRow(db, cached), error);
    if (wet.empty())
        throw std::runtime_error(error);
    const auto away = p.root.path() / "unavailable.wav";
    const auto head = OpJournal(*p.store).headSeq();
    fs::rename(wet, away);
    const auto refused = freeze::flatten(*p.store, 1, 40);
    check(!refused.ok && OpJournal(*p.store).headSeq() == head && digestProject(*p.store).text == before,
          "SC-86: unavailable frozen cache refuses Flatten atomically");
    fs::rename(away, wet);
    const auto flattened = freeze::flatten(*p.store, 1, 40);
    check(flattened.ok, flattened.error.c_str());
    if (!flattened.ok)
        return;
    check(db.execAndGet("SELECT kind FROM tracks WHERE id=1").getString() == "audio" &&
              freeze::state(db, 1).at("frozen") == 0 &&
              db.execAndGet("SELECT count(*) FROM devices").getInt() == 0,
          "SC-86: the frozen MIDI track becomes audio and its devices are removed");
    check(db.execAndGet("SELECT (input_gain_db=0 AND volume_db=-9 AND pan=0.25) "
                       "FROM mixer_strip WHERE track_id=1").getInt() == 1 &&
              db.execAndGet("SELECT monitor_mode FROM tracks WHERE id=1").getInt() == 0 &&
              db.execAndGet("SELECT count(*) FROM routing WHERE id=30 AND src_id=1 AND dst_id=9").getInt() == 1,
          "SC-86: Flatten keeps the mixer and routing while clearing baked input gain and input-only monitoring");
    check(db.execAndGet("SELECT count(*) FROM clips WHERE track_id=1 AND lane_id IS NULL").getInt() == 1 &&
              db.execAndGet("SELECT name FROM clips WHERE id=40").getString() == "Original Clip" &&
              db.execAndGet("SELECT (time_base=1 AND pos_ns=0 AND length_ns=62500000) "
                            "FROM clips WHERE id=40").getInt() == 1 &&
              db.execAndGet("SELECT (src_start_frames=0 AND src_len_frames=3000 AND warp_enabled=0) "
                            "FROM audio_clips WHERE clip_id=40").getInt() == 1 &&
              db.execAndGet("SELECT media_id FROM audio_clips WHERE clip_id=40").getInt64() == cached &&
              db.execAndGet("SELECT lane_id FROM clips WHERE id=20").getInt() == 20,
          "SC-86: the wet cache becomes an absolute, unwarped clip and earlier takes survive");
    engine::Session session;
    const auto expected = static_cast<float>(0.25 * std::pow(10.0, 6.0 / 20.0));
    check(session.load(*p.store, {}, p.spec().session) && session.entryCount() == 0 &&
              heard(session, expected, expected),
          "SC-86: the flattened clip plays the cached samples without loading or reapplying the old chain");
    const auto after = digestProject(*p.store).text;
    History history(*p.store);
    check(history.undo().ok && digestProject(*p.store).text == before,
          "SC-86: one Undo restores the previous frozen instrument, media and track state");
    check(history.redo().ok && digestProject(*p.store).text == after,
          "SC-86: one Redo reapplies the flatten without rendering");
}
void recovery() {
    Project p;
    Counts counts;
    const auto raw = capture(p, counts);
    auto prepared = freeze::prepareFreeze(*p.store, raw, 0, 100);
    check(prepared.ready(), prepared.error().c_str());
    p.store->db().exec(
        "CREATE TABLE late_failure(id INTEGER REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED)");
    CommitOptions options;
    options.beforeCommit = [](SQLite::Database &db, std::int64_t, std::string &) {
        db.exec("INSERT INTO late_failure VALUES(999)");
        return true;
    };
    check(!freeze::commitFreeze(*p.store, prepared, 0, options).ok && freeze::renders(p.store->db()).empty(),
          "late deferred SQL failure rolls back all native rows");
    bool noPublic = true;
    for (const auto &file : fs::directory_iterator(p.root.path()))
        if (file.path().filename().string().starts_with("freeze-"))
            noPublic = false;
    check(noPublic && fs::exists(raw.streams()[0].postFx),
          "late failure removes only newly published files and keeps private renders");
    check(freeze::commitFreeze(*p.store, prepared, 0).ok, "same prepared job retries after late SQL failure");
    check(History(*p.store).undo().ok, "undo leaves successful published assets pooled");
    auto reused = freeze::prepareFreeze(*p.store, raw, 0, 200);
    check(reused.ready() && freeze::commitFreeze(*p.store, reused, 0).ok,
          "identical cache safely reuses prior immutable files after undo");
}
void warmSession() {
    Project p;
    Counts live, rendered;
    engine::Session session;
    check(session.load(*p.store, p.loader(live), p.spec().session), "original live devices load");
    auto *original = session.instanceFor(2);
    session.transport().play();
    session.clips()->prime();
    std::array<float, 64> left{}, right{};
    float *outputs[]{left.data(), right.data()};
    session.process({outputs, nullptr, 2, 0, 64, 0});
    auto raw = capture(p, rendered);
    auto prepared = freeze::prepareFreeze(*p.store, raw, 0, 100);
    check(prepared.ready() && freeze::commitFreeze(*p.store, prepared, 0).ok,
          "freeze is captured independently of a live session");
    check(!session.refresh(*p.store) && session.error().find("stop playback") != std::string::npos,
          "cache topology refuses a running transport before changing source generations");
    session.transport().play(false);
    session.process({outputs, nullptr, 2, 0, 64, 0});
    check(session.refresh(*p.store) && session.problems().empty(),
          "stopped refresh parks devices without missing-device diagnostics");
    const auto frames = live.frames;
    check(heard(session, 0.25f) && live.frames == frames,
          "all original device DSP processes zero frames while cached playback runs");
    session.transport().play(false);
    session.process({outputs, nullptr, 2, 0, 64, 0});
    check(OpJournal(*p.store).commit(op("mixer.setVolume", {{"id", 1}, {"db", -6.0}})).ok &&
              session.refresh(*p.store),
          "live frozen fader rebuild succeeds");
    // Let the existing mixer smoothing settle during stopped callbacks.
    for (unsigned i = 0; i < 8; ++i)
        session.process({outputs, nullptr, 2, 0, 64, 0});
    session.transport().locate(0);
    session.transport().play();
    session.clips()->prime();
    session.process({outputs, nullptr, 2, 0, 64, 0});
    auto endpoint = session.offlineTapOutput(1, engine::Tap::PostFader);
    engine::AudioIo copy;
    copy.out = outputs;
    copy.numOut = 2;
    copy.frames = 64;
    bool scaled = endpoint && session.graph().currentGraph()->copyNodeOutput(endpoint->node, copy);
    const auto gain = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    for (unsigned i = 0; i < 64; ++i)
        scaled = scaled && std::abs(left[i] - 0.25f * gain * signal(i, 0)) < 1e-6f;
    check(scaled && live.frames == frames,
          "actual cached wet fader output changes without waking parked DSP");
    const std::array<std::int64_t, 2> ids{1, 2};
    check(freeze::unfreeze(*p.store, ids).ok, "warm original state unfreezes");
    check(!session.refresh(*p.store), "unfreeze topology also requires stopped playback");
    session.transport().play(false);
    session.process({outputs, nullptr, 2, 0, 64, 0});
    check(session.refresh(*p.store) && live.loads == 6 && session.instanceFor(2) != original,
          "unfreeze restores fresh instances from preserved state instead of resuming stale voices/history");
}
void integrity() {
    Project p;
    Counts counts;
    std::stop_source stop;
    auto spec = p.spec();
    spec.endSample = 3000;
    spec.documentGeneration = 42;
    render::Control control;
    control.stop = stop.get_token();
    auto raw = render::captureFreezeAudio(*p.store, spec, p.loader(counts), control);
    check(raw.ready() && !freeze::prepareFreeze(*p.store, raw, 41, 100).ready(),
          "generation is checked before native preparation as well as commit");
    auto prepared = freeze::prepareFreeze(*p.store, raw, 42, 100);
    stop.request_stop();
    check(!raw.ready() && raw.cancelled() && !freeze::commitFreeze(*p.store, prepared, 42).ok,
          "cancellation after rendering still prevents publication and native mutation");
    p.store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(-1,'audio','Negative legacy ID',3)");
    Counts next;
    raw = capture(p, next);
    prepared = freeze::prepareFreeze(*p.store, raw, 0, 100);
    check(prepared.ready() && freeze::commitFreeze(*p.store, prepared, 0).ok, "integrity fixture freezes");
    SQLite::Statement media(p.store->db(), "SELECT m.rel_path FROM frozen_renders f JOIN media_files m ON "
                                           "m.id=f.post_media_id WHERE f.track_id=1");
    if (!media.executeStep())
        throw std::runtime_error("missing cache fixture");
    const auto target = p.root.path() / media::pathFromUtf8(media.getColumn(0).getString());
    engine::Session valid;
    check(valid.load(*p.store, {}, p.spec().session) && valid.problems().empty(),
          "synthetic cache IDs do not collide with real negative track IDs");
    {
        std::vector<float> zeros(6000);
        audio::WavWriter corrupt(target, 48000, 2, audio::WavFormat::Float32);
        corrupt.write(zeros.data(), 3000);
        corrupt.close();
    }
    engine::Session bad;
    check(!bad.load(*p.store, {}, p.spec().session) && bad.error().find("hash mismatch") != std::string::npos,
          "changed cache bytes refuse loading instead of playing a different sound");
    check(History(*p.store).undo().ok,
          "undo retains a previously published target even after external corruption");
    auto retry = freeze::prepareFreeze(*p.store, raw, 0, 200);
    const auto corrupted = media::blake3File(target);
    check(retry.ready() && !freeze::commitFreeze(*p.store, retry, 0).ok &&
              media::blake3File(target).hex == corrupted.hex && fs::exists(raw.streams()[0].postFx),
          "colliding public bytes remain untouched and private render survives for recovery");
}
void schemaInvalidation() {
    Project p;
    Counts counts;
    p.store->db().exec("CREATE TABLE future_source(id INTEGER PRIMARY KEY,track_id INTEGER REFERENCES "
                       "tracks(id) ON DELETE RESTRICT) STRICT;INSERT INTO future_source VALUES(1,1)");
    auto raw = capture(p, counts);
    auto prepared = freeze::prepareFreeze(*p.store, raw, 0, 100);
    check(prepared.ready() && freeze::commitFreeze(*p.store, prepared, 0).ok,
          "future-source schema fixture freezes");
    p.store->db().exec("ALTER TABLE future_source ADD COLUMN tone REAL NOT NULL DEFAULT 1.25");
    check(!OpJournal(*p.store).commit(op("mixer.setVolume", {{"id", 1}, {"db", -6.0}})).ok,
          "DDL without total_changes invalidates the live-control memo through schema_version");
    const std::array<std::int64_t, 1> ids{1};
    check(freeze::unfreeze(*p.store, ids, true).ok,
          "explicit acknowledgement can unfreeze externally changed source metadata");
}
void refusals() {
    Project p;
    Counts counts;
    auto shortCapture = render::captureFreezeAudio(*p.store, p.spec(), p.loader(counts));
    check(shortCapture.ready() && !freeze::prepareFreeze(*p.store, shortCapture, 0, 100).ready(),
          "partial-track range cannot masquerade as whole-track freeze");
    const auto captured = capture(p, counts);
    p.store->db().exec("UPDATE clips SET gain_db=3 WHERE id=1");
    check(!freeze::prepareFreeze(*p.store, captured, 0, 100).ready(),
          "direct source SQL edit without history movement still invalidates captured source");
    p.store->db().exec("UPDATE clips SET gain_db=0 WHERE id=1");
    check(OpJournal(*p.store).commit(op("track.rename", {{"id", 1}, {"name", "New name"}})).ok &&
              !freeze::prepareFreeze(*p.store, captured, 0, 100).ready(),
          "changed history head refuses old document capture");
}

void mutedMidiDoesNotForward() {
    Project p;
    auto& db=p.store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(3,'midi','Muted MIDI');"
            "INSERT INTO mixer_strip(track_id) VALUES(3);"
            "INSERT INTO clips(id,track_id,kind,pos_ticks,length_ticks,muted) "
            "VALUES(3,3,'midi',0,240240,1)");
    if(!OpJournal(*p.store).commit(op("note.insert",
          {{"clip",3},{"note",1},{"start",0},{"dur",120120},{"key",60}})).ok)
        throw std::runtime_error("write forwarded MIDI fixture note");
    auto spec=p.spec();
    spec.tracks.assign(1,3);
    Counts counts;
    const auto muted=render::captureFreezeAudio(*p.store,spec,p.loader(counts));
    const auto prepared=freeze::prepareFreeze(*p.store,muted,0,100);
    check(muted.ready() && prepared.ready(),
          "single-track Freeze accepts muted MIDI without an instrument");

    db.exec("UPDATE clips SET muted=0 WHERE id=3");
    const auto audible=render::captureFreezeAudio(*p.store,spec,p.loader(counts));
    const auto refused=freeze::prepareFreeze(*p.store,audible,0,100);
    check(audible.ready() && !refused.ready() &&
          refused.error().find("forwarded MIDI stream")!=std::string::npos,
          "a playable forwarded note still refuses single-track Freeze");
}
} // namespace
int main() {
    try {
        mainLaneExtentMatrix();
        lifecycle();
        flattenFrozenTrack();
        recovery();
        refusals();
        mutedMidiDoesNotForward();
        warmSession();
        integrity();
        schemaInvalidation();
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    std::printf("freeze commands: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
