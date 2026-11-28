// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"

#include "adi/applied_batches.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/engine/launch_audio_runtime.hpp"
#include "adi/engine/launch_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <sqlite3.h>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::engine;
using Status = LaunchOperationStatus;

constexpr std::uint64_t incarnation = 7;
constexpr std::int32_t frames = 64;

void require(bool good, const char* message) {
    if (!good)
        throw std::runtime_error(message);
}

OpRequest operation(const char* name, Payload payload) {
    OpRequest request;
    request.opType = name;
    request.payload = std::move(payload);
    return request;
}

struct Fixture {
    test::TempDirectory directory{"launch_ops", "audit"};
    std::unique_ptr<Store> store;
    Session session;
    std::int64_t stream = 0;

    Fixture() {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "session.adi", error);
        require(static_cast<bool>(store), "create test project");
        store->db().exec("INSERT INTO tracks(id,kind,name,index_in_parent) VALUES "
                         "(1,'audio','Track',0),(9,'master','Main',1)");
        const auto media = directory.path() / "clip.wav";
        {
            std::array<float, 512> pcm;
            pcm.fill(0.25f);
            audio::WavWriter writer(media, 48000, 1, audio::WavFormat::Float32);
            writer.write(pcm.data(), static_cast<std::uint32_t>(pcm.size()));
            writer.close();
        }
        LaunchSlot slot;
        slot.track = 1;
        slot.scene = 10;
        slot.clip = 100;
        slot.endTick = textproj::kPPQ;
        slot.mode = LaunchMode::Gate;
        slot.velocityAmount = 1.0f;
        SessionSpec spec;
        spec.sampleRate = 48000;
        spec.maxFrames = frames;
        spec.performanceState = true;
        spec.synchronousClipReads = true;
        spec.launchAudio = std::make_shared<const LaunchAudioProject>(incarnation, 120,
            std::vector<std::int64_t>{1}, std::vector{slot}, std::vector<std::int64_t>{10},
            std::vector{LaunchAudioClip::read(100, 0, media, 48000, 4096)}, LaunchQuantization::None);
        session.graph().setFadeFrames(0);
        require(session.load(*store, {}, spec), "load real Session graph and resident slot");
        session.transport().play();
    }

    void block(float expected) {
        std::array<float, frames> left{}, right{};
        float* outputs[]{left.data(), right.data()};
        AudioIo io;
        io.frames = frames;
        io.numOut = 2;
        io.out = outputs;
        io.streamTimeSamples = stream;
        auto driver = session.beginRender();
        require(driver.process(io), "process engine block");
        const auto matches = [expected](auto value) { return value == expected; };
        require(std::ranges::all_of(left, matches) && std::ranges::all_of(right, matches),
                "runtime launch command has the expected audible result");
        stream += frames;
    }
};

struct AuditObserver final : AppliedObserver {
    Session& session;
    std::shared_ptr<const AppliedBatch> last;
    LaunchOperationResult delivery;
    explicit AuditObserver(Session& target) : session(target) {}
    void committed(std::shared_ptr<const AppliedBatch> batch) override {
        delivery = dispatchLaunchOperations(session, *batch);
        last = std::move(batch);
    }
    void sourceInvalidated(const AppliedSource&, AppliedSourceInvalidation) override { last.reset(); }
};

void run() {
    Fixture fixture;
    auto& session = fixture.session;
    auto& store = *fixture.store;
    OpJournal journal(store);
    const auto launch = operation("session.launchClip",
        {{"track", 1}, {"scene", 10}, {"clip", 100}, {"incarnation", incarnation}, {"velocity", 0.5}});
    const auto scene = operation("session.launchScene", {{"scene", 10}, {"incarnation", incarnation}});
    const auto stop = operation("session.stopTrack", {{"track", 1}, {"incarnation", incarnation}});
    const auto stopAll = operation("session.stopAll", {{"incarnation", incarnation}});
    const auto cancel = operation("session.cancelSceneLaunch", {{"scene", 10}, {"incarnation", incarnation}});
    const auto back = operation("session.backToArrangement", {{"incarnation", incarnation}});
    const auto& registry = OpRegistry::instance();
    for (const auto& command : {launch, scene, stop, stopAll, cancel, back}) {
        const auto* descriptor = registry.find(command.opType);
        require(descriptor && descriptor->scope == Scope::Session && descriptor->ephemeral &&
            !descriptor->buildInverse && validateSubmission(*descriptor, command.payload).empty(),
            "registered Session descriptor admits the intended closed payload and has no undo");
    }

    require(journal.commit(operation("track.rename", {{"id", 1}, {"name", "Launch test"}})).ok,
        "establish a persisted undo head before the transport audit");
    const auto head = journal.headSeq();
    const auto journalRows = journal.count();
    const auto changes = sqlite3_total_changes64(store.db().getHandle());
    auto malformed = stop;
    malformed.payload["position"] = 0;
    const auto refused = dispatchLaunchOperations(session, std::array{launch, malformed});
    require(refused.status == Status::InvalidPayload && refused.operation == 1 && !refused.issues.empty(),
            "a malformed second op refuses the entire gesture at decode");
    fixture.block(0.0f);
    auto stale = stop;
    stale.payload["incarnation"] = incarnation + 1;
    require(dispatchLaunchOperations(session, std::array{launch, stale}).status == Status::Queued,
            "owner admits a whole valid payload gesture to the driver");
    fixture.block(0.0f);
    auto wrongClip = launch;
    wrongClip.payload["clip"] = 999;
    require(dispatchLaunchOperations(session, std::array{scene, wrongClip}).status == Status::Queued,
            "driver receives both the literal Scene and mismatched clip");
    fixture.block(0.0f);
    require(dispatchLaunchOperations(session, std::span(&launch, 1)).status == Status::Queued,
            "direct launch reaches the real Session queue");
    fixture.block(0.125f);
    auto released = launch;
    released.payload["release"] = true;
    require(dispatchLaunchOperations(session, std::span(&released, 1)).status == Status::Queued,
            "release becomes the actual driver Gate edge");
    fixture.block(0.0f);
    require(dispatchLaunchOperations(session, std::span(&scene, 1)).status == Status::Queued,
            "a scene uses the default full velocity");
    fixture.block(0.25f);
    require(dispatchLaunchOperations(session, std::span(&stop, 1)).status == Status::Queued,
            "track stop addresses the current Session run by literal track");
    fixture.block(0.0f);
    require(sqlite3_total_changes64(store.db().getHandle()) == changes && journal.count() == journalRows &&
            journal.headSeq() == head, "direct dispatch changes neither SQLite nor Undo");
    {
        audio::CallbackScope driver;
        require(dispatchLaunchOperations(session, std::span(&launch, 1)).status == Status::AudioThread,
                "submission validation stays out of the audio callback");
    }

    const auto observer = std::make_shared<AuditObserver>(session);
    auto subscription = appliedSource(store).observe(observer);
    CommitOptions refuse;
    refuse.beforeCommit = [](SQLite::Database&, std::int64_t, std::string&) { return false; };
    require(!journal.commit(std::span(&scene, 1), refuse).ok && !observer->last,
            "a rolled-back audit never publishes a launch gesture");
    fixture.block(0.0f);
    require(journal.commit(std::array{scene, cancel}).ok && observer->delivery.status == Status::Queued,
            "one committed audit batch delivers its two commands together");
    fixture.block(0.0f);
    require(journal.commit(scene).ok && observer->delivery.status == Status::Queued,
            "a committed scene opens the live Session lane");
    fixture.block(0.25f);
    Session unrelated;
    require(observer->last && dispatchLaunchOperations(unrelated, *observer->last).status == Status::UnrelatedProject,
            "an audit batch is bound to its original live project");
    require(journal.commit(std::array{stopAll, back}).ok && observer->delivery.status == Status::Queued,
            "committed stop and Back to Arrangement share one gesture");
    fixture.block(0.0f);
    const auto audit = journal.recent(5);
    require(journal.headSeq() == head && journal.count() == journalRows + 5 && audit.size() == 5 &&
            std::ranges::all_of(audit, [](const auto& row) {
                return row.ephemeral && !row.parentSeq && !row.inverse;
            }), "audit rows stay outside the unchanged Undo chain");
}
} // namespace

int main() {
    try {
        run();
        std::puts("PASS -- Session launch op validation, driver batches, audit and unchanged Undo");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL -- %s\n", error.what());
        return 1;
    }
}
