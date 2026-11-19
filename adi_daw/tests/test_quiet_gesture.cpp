// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/digest.hpp"
#include "adi/engine/project_view.hpp"
#include "adi/engine/session.hpp"
#include "adi/gesture_presentation.hpp"
#include "adi/gesture_producers.hpp"
#include "adi/history.hpp"
#include "adi/quiet_gesture.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace {
using namespace adi;
using namespace adi::engine;
namespace g = adi::gesture;
int checks = 0, failures = 0;
void check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        ++failures;
        std::printf("FAIL %s\n", message.c_str());
    }
}
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Source final : Node {
    void prepare(double, std::int32_t) override {}
    bool processesDouble() const noexcept override { return true; }
    const char* name() const noexcept override { return "quiet native source"; }
    void process(const NodeIo& io) noexcept override {
        for (int channel = 0; channel < io.channels; ++channel) {
            const auto value = channel == 0 ? .25 : .5;
            if (io.wide())
                std::fill_n(io.out64[channel] + io.blockOffset, io.frames, value);
            else
                std::fill_n(io.out[channel] + io.blockOffset, io.frames, static_cast<float>(value));
        }
    }
};
struct Fixture {
    test::TempDirectory directory{"quiet_gesture", "native"};
    std::unique_ptr<Store> store;
    Source source;
    engine::Session engine;
    ProjectView view;
    std::shared_ptr<g::EnginePresentation> presentation;
    std::shared_ptr<g::Coordinator> coordinator;
    Precision precision;
    std::int64_t rendered = 0;
    explicit Fixture(Precision width) : precision(width) {
        StoreError error{};
        store = Store::createEmpty(directory.path() / "project.adi", error);
        require(bool(store), "create quiet fixture");
        store->db().exec(
            "INSERT INTO tracks(id,kind,name,index_in_parent) "
            "VALUES(1,'audio','Source',0),(2,'return','Return',1),(9,'master','Main',2);"
            "INSERT INTO mixer_strip(track_id,pan_law) VALUES(1,2),(2,2),(9,2);"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind) "
            "VALUES(1,'track',1,'track',9,'main'),(2,'track',2,'track',9,'main');"
            "INSERT INTO routing(id,src_kind,src_id,dst_kind,dst_id,kind,gain_db) "
            "VALUES(3,'track',1,'track',2,'send',-150)");
        engine.graph().setFadeFrames(0);
        engine.setSourcesFor([this](std::int64_t track) {
            return track == 1 ? std::vector<Node*>{&source} : std::vector<Node*>{};
        });
        SessionSpec spec;
        spec.sampleRate = 48000;
        spec.maxFrames = 64;
        spec.precision = width;
        require(engine.load(
                    *store,
                    [](const DeviceRequest&,
                       std::string&) -> std::unique_ptr<device::DeviceInstance> { return {}; },
                    spec),
                "load quiet native Session: " + engine.error());
        view.refresh(*store);
        presentation = std::make_shared<g::EnginePresentation>(view, engine);
        auto attached = g::Coordinator::attach(*store, presentation);
        require(attached.status.ok() && attached.value, "attach quiet coordinator");
        coordinator = std::move(attached.value);
        engine.transport().play();
    }
    std::unique_ptr<g::Session> begin(g::ScalarTarget target = {g::ScalarKind::TrackVolume, 1}) {
        g::Spec spec;
        spec.strategy = g::Strategy::LiveValues;
        spec.label = "Quiet native scalar";
        auto result = coordinator->begin(spec, g::scalarProducer(target));
        require(result.status.ok() && result.session, "begin quiet native Session");
        return std::move(result.session);
    }
    std::unique_ptr<g::QuietSession>
    quiet(std::uint64_t window = 300, g::ScalarTarget target = {g::ScalarKind::TrackVolume, 1}) {
        return std::make_unique<g::QuietSession>(begin(target), window);
    }
    void value(g::QuietSession& gesture, double db, std::uint64_t now) {
        check(gesture.update(std::make_unique<g::ScalarInput>(db), now).status.ok(),
              "accepted quiet value");
    }
    double stored() {
        return store->db()
            .execAndGet("SELECT volume_db FROM mixer_strip WHERE track_id=1")
            .getDouble();
    }
    void sound(double left, double right, const std::string& label) {
        bool equal = true;
        for (int n = 0; n < 9; ++n) {
            std::array<float, 64> a{}, b{};
            std::array<double, 64> aw{}, bw{};
            float* narrow[]{a.data(), b.data()};
            double* wide[]{aw.data(), bw.data()};
            AudioIo io;
            io.out = narrow;
            io.numOut = 2;
            io.frames = 64;
            io.streamTimeSamples = rendered;
            if (precision == Precision::Float64)
                io.out64 = wide;
            engine.process(io);
            rendered += 64;
            if (n == 8)
                for (std::size_t i = 0; i < a.size(); ++i) {
                    const auto l =
                        precision == Precision::Float64 ? aw[i] : static_cast<double>(a[i]);
                    const auto r =
                        precision == Precision::Float64 ? bw[i] : static_cast<double>(b[i]);
                    equal = equal && std::abs(l - left) < 1e-7 && std::abs(r - right) < 1e-7;
                }
        }
        check(equal, label + ": independent actual PCM");
    }
};
void ordinary(Precision width) {
    Fixture f(width);
    const auto digest = digestProject(*f.store).text;
    const auto count = OpJournal(*f.store).count();
    auto quiet = f.quiet();
    check(!quiet->poll(0).ended, "no input has no quiet deadline");
    for (int n = 1; n <= 20; ++n) {
        f.value(*quiet, -n, static_cast<std::uint64_t>(90 + n * 10));
        if (n == 1 || n == 10) {
            const auto gain = std::pow(10.0, -static_cast<double>(n) / 20.0);
            f.sound(.25 * gain, .5 * gain, "unbracketed value heard during run");
        }
    }
    check(f.stored() == 0 && OpJournal(*f.store).count() == count &&
              digestProject(*f.store).text == digest,
          "twenty values have no durable writes");
    f.sound(.025, .05, "twenty quiet values audible before End");
    check(!quiet->poll(589).ended && f.stored() == 0,
          "quiet period measured from last accepted value");
    const auto end = quiet->poll(590);
    check(end.status.ok() && end.ended && end.ended->durable && end.ended->durable->ok &&
              quiet->state() == g::State::Committed,
          "exact quiet boundary submits ordinary End");
    check(f.stored() == -20 && OpJournal(*f.store).count() == count + 1,
          "quiet End one scalar operation");
    const auto log = OpJournal(*f.store).recent();
    check(log.size() == 1 && log.front().label == "Quiet native scalar",
          "quiet End retains one frozen source label");
    check(!quiet->poll(1000).ended && OpJournal(*f.store).count() == count + 1,
          "later polling never submits twice");
    (void)quiet->cancel();
    check(History(*f.store).undo().ok, "quiet End releases navigation");
    require(f.coordinator->drain(1).status.ok(), "drain quiet Undo");
    f.sound(.25, .5, "one Undo restores quiet gesture origin");
    auto noop = f.quiet();
    f.value(*noop, 0, 0);
    const auto unchanged = noop->poll(300);
    check(unchanged.status.ok() && unchanged.ended && unchanged.ended->semanticNoOp,
          "unchanged quiet End costs no history");
}
void otherScalars(Precision width) {
    Fixture f(width);
    auto pan = f.quiet(300, {g::ScalarKind::TrackPan, 1});
    f.value(*pan, 1, 100);
    f.sound(0, .5, "quiet pan heard before End");
    const auto panEnd = pan->poll(400);
    check(
        panEnd.status.ok() && panEnd.ended &&
            f.store->db().execAndGet("SELECT pan FROM mixer_strip WHERE track_id=1").getDouble() ==
                1,
        "quiet pan End uses the ordinary reversible pan op");
    (void)pan->cancel();
    check(History(*f.store).undo().ok, "quiet pan one Undo");
    require(f.coordinator->drain(10).status.ok(), "drain quiet pan Undo");
    f.sound(.25, .5, "quiet pan Undo restores actual native balance");
    auto send = f.quiet(300, {g::ScalarKind::RoutingGain, 3});
    f.value(*send, 0, 1000);
    f.sound(.5, 1, "quiet send audible at return before End");
    const auto sendEnd = send->poll(1300);
    check(sendEnd.status.ok() && sendEnd.ended &&
              f.store->db().execAndGet("SELECT gain_db FROM routing WHERE id=3").getDouble() == 0,
          "quiet send End uses the ordinary reversible routing op");
    (void)send->cancel();
    check(History(*f.store).undo().ok, "quiet send one Undo");
    require(f.coordinator->drain(11).status.ok(), "drain quiet send Undo");
    f.sound(.25, .5, "quiet send Undo silences only its restored send");
    auto cancel = f.quiet(300, {g::ScalarKind::TrackPan, 1});
    f.value(*cancel, 1, 0);
    f.sound(0, .5, "quiet pan before explicit Cancel");
    check(cancel->cancel().status.ok(), "quiet pan Cancel");
    f.sound(.25, .5, "quiet Cancel immediately restores native PCM");
}

void boundaries(Precision width) {
    Fixture f(width);
    auto quiet = f.quiet();
    f.value(*quiet, -6, 1000);
    check(quiet->update(std::make_unique<g::ScalarInput>(-20), 1300).status.error == g::Error::Busy,
          "late value cannot extend an already expired gesture");
    const auto end = quiet->poll(1300);
    check(end.status.ok() && end.ended && f.stored() == -6,
          "late packet leaves old accepted intent for End");
    (void)quiet->cancel();
    auto next = f.quiet();
    f.value(*next, -12, 1301);
    check(next->poll(1601).status.ok() && f.stored() == -12,
          "new source run captures new committed origin");
    (void)next->cancel();
    check(History(*f.store).undo().ok && f.stored() == -6,
          "next run's one Undo lands on previous quiet End");
    require(f.coordinator->drain(2).status.ok(), "drain next run Undo");
    auto invalid = f.quiet();
    f.value(*invalid, -5, 2000);
    check(!invalid
               ->update(std::make_unique<g::ScalarInput>(std::numeric_limits<double>::quiet_NaN()),
                        2290)
               .status.ok(),
          "rejected value cannot renew the deadline");
    const auto validEnd = invalid->poll(2300);
    check(validEnd.status.ok() && validEnd.ended && f.stored() == -5,
          "deadline commits last accepted value after rejection");
    (void)invalid->cancel();
    auto backwards = f.quiet();
    f.value(*backwards, -10, 3000);
    check(backwards->update(std::make_unique<g::ScalarInput>(-20), 2999).status.error ==
              g::Error::InvalidInput,
          "backwards input clock refuses");
    check(backwards->poll(2999).status.error == g::Error::InvalidInput, "backwards poll refuses");
    check(backwards->poll(3300).status.ok() && f.stored() == -10,
          "bad clocks cannot change the last accepted intent");
    (void)backwards->cancel();
    auto ceiling = f.quiet();
    const auto max = std::numeric_limits<std::uint64_t>::max();
    f.value(*ceiling, -20, max - 300);
    check(!ceiling->poll(max - 1).ended, "quiet interval near clock ceiling does not wrap early");
    check(ceiling->poll(max).status.ok() && f.stored() == -20,
          "clock ceiling expires exactly without addition overflow");
    (void)ceiling->cancel();
    auto beyond = f.quiet(400);
    f.value(*beyond, -25, max - 300);
    check(!beyond->poll(max).ended,
          "unrepresentable future deadline cannot wrap into an early End");
    (void)beyond->cancel();
    auto cancelled = f.quiet();
    const auto digest = digestProject(*f.store).text;
    f.value(*cancelled, -30, 0);
    check(cancelled->cancel().status.ok(), "explicit cancel quiet gesture");
    check(!cancelled->poll(500).ended && digestProject(*f.store).text == digest,
          "cancel has no delayed End or material writes");
}
void admissions(Precision width) {
    Fixture f(width);
    auto quiet = f.quiet();
    f.value(*quiet, -10, 10);
    const auto head = OpJournal(*f.store).headSeq();
    {
        audio::CallbackScope callback;
        check(quiet->poll(310).status.error == g::Error::WrongThread,
              "callback cannot End even at deadline");
        check(quiet->update(std::make_unique<g::ScalarInput>(-20), 20).status.error ==
                  g::Error::WrongThread,
              "callback cannot update timer or preview");
    }
    g::Status foreign;
    std::thread other([&] { foreign = quiet->poll(310).status; });
    other.join();
    check(foreign.error == g::Error::WrongThread && OpJournal(*f.store).headSeq() == head,
          "foreign thread refuses before SQL or timer state");
    check(quiet->poll(309).status.ok(), "refused foreign call cannot advance owner clock");
    f.store->db().exec("BEGIN");
    const auto refused = quiet->poll(310);
    check(!refused.status.ok() && refused.ended && quiet->state() != g::State::Committed,
          "caller SQL transaction refuses quiet End");
    f.store->db().exec("ROLLBACK");
    check(quiet->poll(310).status.ok() && f.stored() == -10,
          "ordinary retry Ends once after caller rollback");
    check(quiet->update(std::make_unique<g::ScalarInput>(-20), 311).status.error ==
              g::Error::Closed,
          "ended detector cannot accept another source run");
    (void)quiet->cancel();
    auto failure = f.quiet();
    f.value(*failure, -20, 400);
    const auto before = OpJournal(*f.store).count();
    f.store->db().exec("CREATE TEMP TRIGGER fail_quiet BEFORE UPDATE OF volume_db ON mixer_strip "
                       "WHEN NEW.track_id=1 BEGIN SELECT RAISE(ABORT,'quiet failure'); END");
    const auto failed = failure->poll(700);
    check(!failed.status.ok() && failed.ended && f.stored() == -10 &&
              OpJournal(*f.store).count() == before,
          "failed End rolls back and retains one retryable owner");
    f.store->db().exec("DROP TRIGGER fail_quiet");
    const auto retry = failure->poll(701);
    check(retry.status.ok() && retry.ended && f.stored() == -20 &&
              OpJournal(*f.store).count() == before + 1,
          "failed quiet End can retry exactly once");
    check(!failure->poll(702).ended, "successful retry cannot duplicate End");
    (void)failure->cancel();
    auto early = f.quiet();
    f.value(*early, -30, 800);
    const auto released = early->end();
    check(released.status.ok() && released.durable && released.durable->ok,
          "explicit source release Ends before quiet boundary");
    check(!early->poll(2000).ended, "explicit End cancels quiet completion");
    (void)early->cancel();
    auto lost = f.quiet();
    f.value(*lost, -40, 0);
    f.engine.transport().play(false);
    f.store.reset();
    check(lost->poll(299).status.error == g::Error::SourceLost &&
              lost->state() == g::State::SourceLost,
          "source loss never dereferences a closed Store");
}
void construction() {
    Fixture f(Precision::Float64);
    bool zero = false, empty = false, previewed = false;
    try {
        g::QuietSession bad(f.begin(), 0);
    } catch (const std::invalid_argument&) {
        zero = true;
    }
    try {
        g::QuietSession bad({}, 300);
    } catch (const std::invalid_argument&) {
        empty = true;
    }
    auto started = f.begin();
    require(started->update(std::make_unique<g::ScalarInput>(-10)).status.ok(),
            "prepare already previewed constructor fixture");
    try {
        g::QuietSession bad(std::move(started), 300);
    } catch (const std::invalid_argument&) {
        previewed = true;
    }
    check(zero && empty && previewed,
          "detector requires positive interval and fresh owned Session");
    check(OpJournal(*f.store).count() == 0 && f.stored() == 0,
          "failed construction never Ends or journals transferred ownership");
    auto unused = f.quiet();
    check(!unused->poll(UINT64_MAX).ended,
          "no accepted input cannot time out even at clock ceiling");
    check(unused->cancel().status.ok(), "unused source run can release its reservation");
}
} // namespace
int main() {
    for (const auto width : {Precision::Float32, Precision::Float64})
        try {
            ordinary(width);
            otherScalars(width);
            boundaries(width);
            admissions(width);
        } catch (const std::exception& e) {
            check(false, std::string("unexpected quiet fixture exception: ") + e.what());
        }
    try {
        construction();
    } catch (const std::exception& e) {
        check(false, std::string("unexpected constructor fixture exception: ") + e.what());
    }
    std::printf("quiet gesture: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
