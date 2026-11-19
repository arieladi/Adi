#include "adi/digest.hpp"
#include "adi/freeze_commands.hpp"
#include "adi/freeze_state.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "freeze_fixture.hpp"
#include <cstdlib>
#include <new>
namespace {
std::atomic<std::uint32_t> callbackAllocations{0};
}
void *operator new(std::size_t n) {
    if (adi::audio::onAudioThread)
        callbackAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) {
    return operator new(n);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
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
} // namespace
int main() {
    try {
        lifecycle();
        recovery();
        refusals();
        warmSession();
        integrity();
        schemaInvalidation();
    } catch (const std::exception &e) {
        check(false, e.what());
    }
    std::printf("freeze commands: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
