// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/digest.hpp"
#include "adi/freeze_commands.hpp"
#include "adi/freeze_groups.hpp"
#include "adi/freeze_state.hpp"
#include "adi/history.hpp"
#include "freeze_fixture.hpp"
namespace {
struct BusCounts {
    Counts children;
    int loads = 0;
    std::uint64_t frames = 0;
};
class BusEffect final : public device::DeviceInstance {
    BusCounts *counts_;
    device::DeviceIdentity identity_;
    std::array<std::array<double, 64>, 2> ring_{};
    unsigned at_ = 0;

  public:
    explicit BusEffect(BusCounts &counts) : counts_(&counts) {
        identity_.format = "vst3";
        identity_.uid = "test.group";
        identity_.version = "actual-4";
    }
    const device::DeviceIdentity &identity() const noexcept override {
        return identity_;
    }
    bool loaded() const noexcept override {
        return true;
    }
    bool processesDouble() const noexcept override {
        return true;
    }
    std::int32_t latencySamples() const noexcept override {
        return 5;
    }
    void process(const engine::NodeIo &io) noexcept override {
        for (int i = 0; i < io.frames; ++i) {
            for (int c = 0; c < io.channels; ++c) {
                const auto channel = static_cast<std::size_t>(c),
                           frame = static_cast<std::size_t>(io.blockOffset + i);
                const auto value = ring_[channel][(at_ + 59) % 64] * 0.5;
                ring_[channel][at_] = io.wide() ? io.in64[c][frame] : io.in[c][frame];
                if (io.wide())
                    io.out64[c][frame] = value;
                else
                    io.out[c][frame] = static_cast<float>(value);
            }
            at_ = (at_ + 1) % 64;
        }
        counts_->frames += static_cast<std::uint64_t>(io.frames);
    }
};
struct GroupProject : Project {
    GroupProject() {
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(10,'group','Bus',3);UPDATE tracks SET "
            "parent_id=10 WHERE id IN(1,2);UPDATE tracks SET record_armed=1 WHERE id=1;INSERT INTO "
            "mixer_strip(track_id) VALUES(10);INSERT INTO plugin_refs(id,format,uid,name,version,subtype) "
            "VALUES(4,'vst3','test.group','Bus FX','stored-4','effect');INSERT INTO "
            "device_chains(id,track_id) VALUES(2,10);INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) "
            "VALUES(4,2,0,4,'Bus FX')");
    }
    engine::DeviceLoader loader(BusCounts &counts) {
        auto child = Project::loader(counts.children);
        return [&counts, child](const engine::DeviceRequest &request,
                                std::string &error) -> std::unique_ptr<device::DeviceInstance> {
            if (request.device->id == 4) {
                ++counts.loads;
                return std::make_unique<BusEffect>(counts);
            }
            return child(request, error);
        };
    }
    render::FreezeCaptureSpec groupSpec(engine::Precision precision = engine::Precision::Float32) {
        auto result = Project::spec(precision);
        result.tracks = {10};
        result.wholeGroups = true;
        result.endSample = 3000;
        return result;
    }
};
OpRequest command(const char *name, Payload value) {
    OpRequest request;
    request.opType = name;
    request.payload = std::move(value);
    return request;
}
bool tap(engine::Session &session, engine::Tap point, float multiplier) {
    const auto endpoint = session.offlineTapOutput(10, point);
    if (!endpoint)
        return false;
    std::array<float, 64> left{}, right{};
    float *pointers[]{left.data(), right.data()};
    engine::AudioIo copy;
    copy.out = pointers;
    copy.numOut = 2;
    copy.frames = 64;
    if (!session.graph().currentGraph()->copyNodeOutput(endpoint->node, copy))
        return false;
    for (unsigned i = 0; i < 64; ++i)
        if (std::abs(left[i] - multiplier * signal(i, 0)) > 1e-6f ||
            std::abs(right[i] - multiplier * signal(i, 1)) > 1e-6f)
            return false;
    return true;
}
void lifecycle() {
    for (const auto precision : {engine::Precision::Float32, engine::Precision::Float64}) {
        GroupProject p;
        BusCounts counts;
        const auto original = digestProject(*p.store).text;
        const auto beforeSnapshot = engine::SnapshotBuilder::fromStore(*p.store);
        auto captured = render::captureFreezeAudio(*p.store, p.groupSpec(precision), p.loader(counts));
        check(captured.ready(), captured.error().c_str());
        if (!captured.ready())
            continue;
        check(captured.streams()[0].wholeGroup && captured.streams()[0].contributors.size() == 2 &&
                  captured.streams()[0].postFxLatency == 41,
              "one group bus capture retains child runtime identities and compensation");
        auto prepared = freeze::prepareFreeze(*p.store, captured, 0, 100);
        check(prepared.ready(), prepared.error().c_str());
        const auto committed = freeze::commitFreeze(*p.store, prepared, 0);
        check(committed.ok, committed.error.c_str());
        if (!committed.ok)
            continue;
        check(freeze::groups(p.store->db()).at(10).size() == 2 &&
                  freeze::state(p.store->db(), 1).at("armed") == 0,
              "whole-group metadata disarms original members without deleting their material");
        const auto snapshot = engine::SnapshotBuilder::fromStore(*p.store, *beforeSnapshot);
        check(snapshot->findTrack(10)->freezeMode == "group" && snapshot->findTrack(1)->frozenByGroup == 10 &&
                  snapshot->findTrack(1)->frozen && snapshot->findTrack(1)->freezeMode == "group-member" &&
                  snapshot->findTrack(1) != beforeSnapshot->findTrack(1),
              "snapshot exposes group ownership and invalidates child structural sharing");
        const auto unchanged = engine::SnapshotBuilder::fromStore(*p.store, *snapshot);
        check(unchanged->findTrack(1) == snapshot->findTrack(1),
              "unchanged group membership shares snapshot children");
        History history(*p.store);
        check(history.undo().ok && digestProject(*p.store).text == original,
              "one undo restores exact pre-group material, membership and armed states");
        check(history.redo().ok, "one redo restores the group cache and scope together");
        check(!OpJournal(*p.store).commit(command("mixer.setVolume", {{"id", 1}, {"db", -6.0}})).ok,
              "child fader cannot silently bypass the group-source fingerprint via live-control fast path");
        check(!OpJournal(*p.store).commit(command("track.setArm", {{"id", 1}, {"armed", true}})).ok,
              "group member cannot be rearmed while its source is parked");
        check(OpJournal(*p.store).commit(command("mixer.setVolume", {{"id", 10}, {"db", -6.0}})).ok,
              "root group fader remains live");
        check(history.undo().ok, "undo of live group fader keeps cache valid");
        fs::rename(p.root.path() / "source.wav", p.root.path() / "original-unavailable.wav");
        engine::Session session;
        int loads = 0;
        auto missing = [&](const engine::DeviceRequest &, std::string &) {
            ++loads;
            return std::unique_ptr<device::DeviceInstance>{};
        };
        const bool loaded = session.load(*p.store, missing, p.groupSpec(precision).session);
        check(loaded, session.error().c_str());
        check(loaded && loads == 0 && session.problems().empty() && !session.graph().current()->has(1) &&
                  !session.graph().current()->has(2) && session.graph().current()->has(10),
              "cached group opens without original files/plugins and actually removes child graph nodes");
        if (!loaded)
            continue;
        session.transport().locate(0);
        session.transport().play();
        check(session.clips()->prime(), "group cache reader primes");
        std::array<float, 64> left{}, right{};
        float *output[]{left.data(), right.data()};
        session.process({output, nullptr, 2, 0, 64, 0});
        check(tap(session, engine::Tap::PreFx, 1.25f) && tap(session, engine::Tap::PostFx, 0.625f),
              "actual cached group dry/wet PCM preserves summed children and root inserts at both widths");
        fs::rename(p.root.path() / "original-unavailable.wav", p.root.path() / "source.wav");
        const std::array<std::int64_t, 1> ids{10};
        check(freeze::unfreeze(*p.store, ids).ok && freeze::state(p.store->db(), 1).at("armed") == 1,
              "unfreeze restores saved child armed states");
        check(history.undo().ok && freeze::groups(p.store->db()).contains(10),
              "undo of group unfreeze restores complete cache scope");
        const auto frozen = digestProject(*p.store).text;
        check(OpJournal(*p.store).commit(command("track.delete", {{"id", 10}})).ok,
              "delete captures group cache membership as dependent data");
        check(history.undo().ok && digestProject(*p.store).text == frozen,
              "generic track-delete undo preserves group metadata and original children");
    }
}
void routesAndRefusals() {
    GroupProject p;
    BusCounts counts;
    p.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(30,'return','Outside "
                       "return',4);INSERT INTO mixer_strip(track_id) VALUES(30);INSERT INTO "
                       "routing(id,src_kind,src_id,dst_kind,dst_id,kind,origin) "
                       "VALUES(21,'track',2,'track',30,'send','user')");
    auto spec = p.groupSpec();
    auto unacknowledged = render::captureFreezeAudio(*p.store, spec, p.loader(counts));
    check(!unacknowledged.ready() && unacknowledged.error().find("acknowledge") != std::string::npos &&
              p.stages().empty(),
          "child audio routes require acknowledgement before group capture and failure cleans its stage");
    spec.acknowledgeChildAudioRoutes = true;
    auto captured = render::captureFreezeAudio(*p.store, spec, p.loader(counts));
    auto prepared = freeze::prepareFreeze(*p.store, captured, 0, 100);
    check(captured.ready() && prepared.ready() && freeze::commitFreeze(*p.store, prepared, 0).ok,
          "acknowledged group capture preserves the complete original routing in the project");
    check(p.store->db().execAndGet("SELECT enabled FROM routing WHERE id=21").getInt() == 1,
          "freezing does not rewrite or disable the original child send row");
    engine::Session session;
    const bool loaded = session.load(*p.store, {}, p.groupSpec().session);
    check(loaded, session.error().c_str());
    if (loaded) {
        const auto &edges = session.graph().current()->chainEdges();
        const bool noSend = std::none_of(edges.begin(), edges.end(), [](const auto &edge) {
            return edge.key.leg == engine::RealizedGraph::ChainKey::Leg::Send && edge.key.name == 21;
        });
        check(session.graph().current()->has(30) && !session.graph().current()->has(2) && noSend,
              "outside return remains live but the parked child's audio send is absent from the realized "
              "graph");
        session.transport().play();
        session.clips()->prime();
        std::array<float, 64> left{}, right{};
        float *outputs[]{left.data(), right.data()};
        session.process({outputs, nullptr, 2, 0, 64, 0});
        bool heard = true;
        for (unsigned i = 0; i < 64; ++i)
            heard = heard && std::abs(left[i] - 0.625f * signal(i, 0)) < 1e-6f;
        check(heard, "actual master output contains the group cache once and no disabled child send path");
    }
    GroupProject solo;
    BusCounts soloCounts;
    solo.store->db().exec("UPDATE tracks SET soloed=1 WHERE id=2");
    const auto soloed = render::captureFreezeAudio(*solo.store, solo.groupSpec(), solo.loader(soloCounts));
    check(!soloed.ready() && soloed.error().find("solo") != std::string::npos,
          "capture refuses a solo-filtered partial group mix");
    GroupProject keyed;
    BusCounts keyCounts;
    keyed.store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(3,'audio','External key',4);INSERT INTO "
        "routing(id,src_kind,src_id,dst_kind,dst_id,kind) VALUES(22,'track',3,'track',1,'sidechain')");
    const auto external =
        render::captureFreezeAudio(*keyed.store, keyed.groupSpec(), keyed.loader(keyCounts));
    check(!external.ready() && external.error().find("outside the group") != std::string::npos,
          "external child sidechain refuses instead of baking an unresponsive live key");
    GroupProject forwarded;
    BusCounts midiCounts;
    forwarded.store->db().exec(
        "INSERT INTO clips(id,track_id,kind,time_base,pos_ns,length_ns) VALUES(3,2,'midi',1,0,62500000)");
    const auto midi =
        render::captureFreezeAudio(*forwarded.store, forwarded.groupSpec(), forwarded.loader(midiCounts));
    check(!midi.ready() && midi.error().find("forwarded MIDI") != std::string::npos,
          "group cache cannot silently discard MIDI forwarded to another instrument");
}
void ownershipSharingAndRecovery() {
    GroupProject p;
    BusCounts counts;
    auto raw = render::captureFreezeAudio(*p.store, p.groupSpec(), p.loader(counts));
    auto prepared = freeze::prepareFreeze(*p.store, raw, 0, 100);
    p.store->db().exec(
        "CREATE TABLE late_failure(id INTEGER REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED)");
    CommitOptions late;
    late.beforeCommit = [](SQLite::Database &db, std::int64_t, std::string &) {
        db.exec("INSERT INTO late_failure VALUES(999)");
        return true;
    };
    check(prepared.ready() && !freeze::commitFreeze(*p.store, prepared, 0, late).ok &&
              freeze::groups(p.store->db()).empty() && freeze::state(p.store->db(), 1).at("armed") == 1 &&
              fs::exists(raw.streams()[0].postFx),
          "late SQL failure rolls back group membership and child arm mutations while retaining original "
          "renders");
    check(freeze::commitFreeze(*p.store, prepared, 0).ok,
          "the same prepared whole-group job can retry after a late SQL failure");
    const auto before = engine::SnapshotBuilder::fromStore(*p.store);
    const std::array<std::int64_t, 1> ids{10};
    check(freeze::unfreeze(*p.store, ids).ok, "inner group can be unfrozen before freezing its outer group");
    p.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(11,'group','Outer',4);UPDATE "
                       "tracks SET parent_id=11 WHERE id=10;INSERT INTO mixer_strip(track_id) VALUES(11)");
    auto outer = p.groupSpec();
    outer.tracks = {11};
    BusCounts next;
    auto captured = render::captureFreezeAudio(*p.store, outer, p.loader(next));
    auto outerEdit = freeze::prepareFreeze(*p.store, captured, 0, 300);
    check(captured.ready() && outerEdit.ready() && freeze::commitFreeze(*p.store, outerEdit, 0).ok,
          "outer group freezes the original complete descendant tree");
    const auto after = engine::SnapshotBuilder::fromStore(*p.store, *before);
    check(before->findTrack(1)->frozenByGroup == 10 && after->findTrack(1)->frozenByGroup == 11 &&
              after->findTrack(1) != before->findTrack(1) &&
              after->findTrack(1)->clips == before->findTrack(1)->clips,
          "changing only effective freeze owner replaces a child snapshot while sharing unchanged clips");
}
void analogBus() {
    GroupProject p;
    BusCounts counts;
    p.store->db().exec(
        "INSERT INTO group_summing(track_id,enabled,flavor,drive_db) VALUES(10,1,'console9',6.0)");
    auto captured =
        render::captureFreezeAudio(*p.store, p.groupSpec(engine::Precision::Float64), p.loader(counts));
    check(captured.ready(), captured.error().c_str());
    if (!captured.ready())
        return;
    auto prepared = freeze::prepareFreeze(*p.store, captured, 0, 100);
    check(prepared.ready() && freeze::commitFreeze(*p.store, prepared, 0).ok,
          "analog group buss and channel halves are captured before parking");
    engine::Session session;
    const bool loaded = session.load(*p.store, {}, p.groupSpec(engine::Precision::Float64).session);
    check(loaded, session.error().c_str());
    if (!loaded)
        return;
    session.transport().play();
    session.clips()->prime();
    std::array<float, 64> left{}, right{};
    float *output[]{left.data(), right.data()};
    session.process({output, nullptr, 2, 0, 64, 0});
    bool exact = true;
    for (unsigned point = 0; point < 2; ++point) {
        audio::WavReader reference(point ? captured.streams()[0].postFx : captured.streams()[0].preFx);
        std::array<float, 128> pcm{};
        exact = exact && reference.read(pcm.data(), 64) == 64;
        const auto endpoint = session.offlineTapOutput(10, point ? engine::Tap::PostFx : engine::Tap::PreFx);
        engine::AudioIo copy;
        copy.out = output;
        copy.numOut = 2;
        copy.frames = 64;
        exact = exact && endpoint && session.graph().currentGraph()->copyNodeOutput(endpoint->node, copy);
        for (unsigned i = 0; i < 64; ++i)
            exact = exact && left[i] == pcm[i * 2] && right[i] == pcm[i * 2 + 1];
    }
    check(
        exact,
        "actual cached dry/wet taps match encoded analog-bus renders without applying the group buss twice");
}
void mediaFallbackAndIndependentCapture() {
    GroupProject p;
    BusCounts counts;
    auto captured = render::captureFreezeAudio(*p.store, p.groupSpec(), p.loader(counts));
    auto prepared = freeze::prepareFreeze(*p.store, captured, 0, 100);
    check(prepared.ready() && freeze::commitFreeze(*p.store, prepared, 0).ok, "fallback fixture freezes");
    const auto relocated = p.root.path() / "relocated";
    fs::create_directory(relocated);
    {
        SQLite::Statement files(p.store->db(),
                                "SELECT m.id,m.rel_path FROM media_files m JOIN frozen_renders f ON "
                                "m.id=f.pre_media_id OR m.id=f.post_media_id WHERE f.track_id=10");
        while (files.executeStep()) {
            const auto source = p.root.path() / media::pathFromUtf8(files.getColumn(1).getString());
            const auto target = relocated / source.filename();
            fs::rename(source, target);
            SQLite::Statement hint(p.store->db(), "UPDATE media_files SET abs_path_hint=? WHERE id=?");
            hint.bind(1, media::pathUtf8(target));
            hint.bind(2, files.getColumn(0).getInt64());
            hint.exec();
        }
    }
    fs::rename(p.root.path() / "source.wav", p.root.path() / "original-unavailable.wav");
    engine::Session session;
    const bool loaded = session.load(*p.store, {}, p.groupSpec().session);
    check(loaded && session.problems().empty(),
          "verified fallback is used by playback instead of reopening a missing relative cache path");
    if (loaded) {
        session.transport().play();
        session.clips()->prime();
        std::array<float, 64> left{}, right{};
        float *output[]{left.data(), right.data()};
        session.process({output, nullptr, 2, 0, 64, 0});
        check(tap(session, engine::Tap::PostFx, 0.625f),
              "actual group PCM plays from its verified fallback location");
    }
    p.store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) "
                       "VALUES(3,'audio','Independent',4);INSERT INTO mixer_strip(track_id) VALUES(3)");
    auto independent = p.groupSpec();
    independent.tracks = {3};
    independent.wholeGroups = false;
    BusCounts next;
    const auto other = render::captureFreezeAudio(*p.store, independent, p.loader(next));
    check(other.ready(), other.error().c_str());
    check(other.ready() && next.loads == 0 && next.children.loads == 0,
          "new capture ignores missing originals and parked plug-ins beneath an already frozen group");
}
} // namespace
int main() {
    try {
        lifecycle();
        routesAndRefusals();
        ownershipSharingAndRecovery();
        analogBus();
        mediaFallbackAndIndependentCapture();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("freeze group: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
