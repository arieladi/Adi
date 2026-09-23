// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parameter-op glue (ADR-0124): a device's own broadcasts become one
// `device.setParam` per gesture; an op from anywhere else reaches the device
// with the echo guard armed; a CLAP's output events arrive from the audio
// thread; and the whole loop closes through the journal and an undo.
//
// No JUCE. The VST3 listener is the same three calls into the same sink and
// is exercised by the JUCE build.

#include "temp_directory.hpp"

#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "juce/clap_host.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace adi;
using namespace adi::engine;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}

void eqi(long long got, long long want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %lld, want %lld\n", what.c_str(), got, want);
    }
}

void section(const char* s) { std::printf("[%s]\n", s); }

bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }

// ---------------------------------------------------------------------------
// A device that broadcasts like a plugin's window would
// ---------------------------------------------------------------------------

class Knobs final : public device::DeviceInstance {
public:
    Knobs() {
        device::ParamDescriptor c;
        c.id = "cutoff"; c.name = "Cutoff"; c.unit = "Hz";
        c.domain = device::ParamDomain::Real;
        c.hasRealRange = true; c.minReal = 20.0; c.maxReal = 20000.0;
        params_.push_back(c);
        values_.push_back(0.5);
        device::ParamDescriptor g;
        g.id = "gain"; g.name = "Gain";
        params_.push_back(g);
        values_.push_back(0.25);
        id_.format = "test"; id_.name = "Knobs";
    }
    [[nodiscard]] const device::DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return true; }
    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return 0; }
    void process(const NodeIo& io) noexcept override { device::passThrough(io); }
    [[nodiscard]] std::int32_t paramCount() const noexcept override {
        return static_cast<std::int32_t>(params_.size());
    }
    [[nodiscard]] const device::ParamDescriptor* paramAt(std::int32_t i) const noexcept override {
        return (i < 0 || i >= paramCount()) ? nullptr : &params_[static_cast<std::size_t>(i)];
    }
    [[nodiscard]] device::ParamValue getParam(const std::string& id) const noexcept override {
        for (std::size_t i = 0; i < params_.size(); ++i) {
            if (params_[i].id != id) continue;
            const double n = values_[i];
            return params_[i].hasRealRange
                       ? device::ParamValue::withReal(n, params_[i].minReal + n * (params_[i].maxReal - params_[i].minReal))
                       : device::ParamValue::fromNormalized(n);
        }
        return {};
    }
    bool setParam(const std::string& id, const device::ParamValue& v) override {
        for (std::size_t i = 0; i < params_.size(); ++i) {
            if (params_[i].id != id) continue;
            ++sets;
            values_[i] = v.normalized;
            // Some plugins tell the host about the value the host just set.
            if (echoOnSet) broadcastParam(static_cast<std::int32_t>(i), ParamEventKind::Value, v.normalized);
            return true;
        }
        return false;
    }

    // The plugin's window, from the outside.
    void begin(std::int32_t i) { broadcastParam(i, ParamEventKind::Begin, 0.0); }
    void value(std::int32_t i, double n) {
        values_[static_cast<std::size_t>(i)] = n;
        broadcastParam(i, ParamEventKind::Value, n);
    }
    void end(std::int32_t i) { broadcastParam(i, ParamEventKind::End, 0.0); }

    int sets = 0;
    bool echoOnSet = false;

private:
    device::DeviceIdentity id_;
    std::vector<device::ParamDescriptor> params_;
    std::vector<double> values_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void testAGestureBecomesOneOp() {
    section("ADR-0110 d2 / ADR-0124 -- one gesture, one op, with the real value beside the normalized one");
    ParamOps ops;
    Knobs k;
    check(ops.attach(7, k), "attached");
    check(!ops.attach(7, k), "and not twice");
    check(k.hasParamSink(), "the device has its sink");

    k.begin(0);
    for (int i = 1; i <= 200; ++i) k.value(0, 0.5 + 0.3 * i / 200.0);
    k.end(0);

    std::vector<OpRequest> out;
    const std::size_t n = ops.drain(1000, out);
    // Two requests: the first-touch opener (decision 4) and the edit.
    eqi(static_cast<long long>(n), 2, "the first gesture on a parameter appends an opener and the edit");
    if (out.size() != 2) return;
    check(out[0].opType == "device.setParam" && out[1].opType == "device.setParam", "both device.setParam");
    check(out[0].payload.at("dev").get<std::int64_t>() == 7, "for device 7");
    check(out[0].payload.at("param").get<std::string>() == "cutoff", "on cutoff");
    check(near(out[0].payload.at("norm").get<double>(), 0.5), "the opener carries where it started: 0.5");
    check(near(out[1].payload.at("norm").get<double>(), 0.8), "the edit carries where it ended: 0.8");
    const double realOut = out[1].payload.value("real", -1.0);   // -1: absent
    check(near(realOut, 20.0 + 0.8 * 19980.0),
          "and the real value from the declared range: " + std::to_string(realOut));
    check(out[1].label == "Knobs: Cutoff", "labelled for the undo menu: " + out[1].label);
    check(out[1].targetKind == "device" && out[1].targetId && *out[1].targetId == 7, "targeting the device");
    const ParamEditCapture::Stats* cs = ops.captureStats(7);
    check(cs != nullptr && cs->unseeded == 0, "the parameter was seeded at attach, so `before` was real");
    eqi(ops.stats().firstTouches, 1, "one first touch");

    std::vector<OpRequest> again;
    eqi(static_cast<long long>(ops.drain(1100, again)), 0, "nothing left");

    // A second gesture on the same parameter: no opener this time.
    k.begin(0); k.value(0, 0.1); k.end(0);
    std::vector<OpRequest> second;
    eqi(static_cast<long long>(ops.drain(1200, second)), 1, "the second gesture is one op");
    check(!second.empty() && near(second[0].payload.at("norm").get<double>(), 0.1), "at 0.1");

    // A gesture on the other parameter, which has no range: no `real`.
    k.begin(1); k.value(1, 0.9); k.end(1);
    std::vector<OpRequest> third;
    eqi(static_cast<long long>(ops.drain(1300, third)), 2, "opener and edit for gain");
    check(third.size() == 2 && !third[1].payload.contains("real"), "gain has no range, so no real");
}

void testAppliedArmsTheEcho() {
    section("ADR-0110 d3 -- an op from elsewhere reaches the device; its echo is not a new op");
    ParamOps ops;
    Knobs k;
    k.echoOnSet = true;
    ops.attach(3, k);

    Payload p = {{"dev", 3}, {"param", "gain"}, {"norm", 0.7}};
    check(ops.applied(p, 1000), "applied set the device");
    eqi(k.sets, 1, "one set");
    check(near(k.getParam("gain").normalized, 0.7), "to 0.7");
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(1200, out)), 0, "the echo did not become an op");
    const ParamEditCapture::Stats* cs = ops.captureStats(3);
    check(cs != nullptr && cs->echoesSwallowed == 1, "it was swallowed: " +
                                                       std::to_string(cs ? cs->echoesSwallowed : -1));
    eqi(ops.stats().applied, 1, "counted");

    // And after the echo, a REAL gesture still works and starts from 0.7.
    k.begin(1); k.value(1, 0.2); k.end(1);
    std::vector<OpRequest> real;
    eqi(static_cast<long long>(ops.drain(1400, real)), 2, "a real gesture after it: opener and edit");
    check(real.size() == 2 && near(real[0].payload.at("norm").get<double>(), 0.7),
          "and the opener knows the applied value was where it started");
}

void testOwnOpsComeBackEqual() {
    section("ADR-0124 d3 -- our own op returning through the journal sets nothing");
    ParamOps ops;
    Knobs k;
    ops.attach(4, k);
    k.begin(0); k.value(0, 0.6); k.end(0);
    std::vector<OpRequest> out;
    ops.drain(1000, out);
    check(out.size() == 2, "two requests");
    if (out.size() != 2) return;
    check(!ops.applied(out[1].payload, 1100), "the edit's payload comes back: not applied");
    eqi(k.sets, 0, "the device was never set by its own edit");
    eqi(ops.stats().appliedEqual, 1, "counted as equal");
}

void testUnknownsAreCounted() {
    section("ADR-0124 -- unknown device, unknown parameter, a cleared row: counted, never thrown");
    ParamOps ops;
    Knobs k;
    ops.attach(1, k);
    check(!ops.applied({{"dev", 99}, {"param", "cutoff"}, {"norm", 0.5}}, 1), "unknown device");
    eqi(ops.stats().unknownDevice, 1, "counted");
    check(!ops.applied({{"dev", 1}, {"param", "nope"}, {"norm", 0.5}}, 1), "unknown parameter");
    eqi(ops.stats().unknownParam, 1, "counted");
    check(!ops.applied({{"dev", 1}, {"param", "cutoff"}, {"norm", nullptr}}, 1), "a cleared row sets nothing");
    eqi(ops.stats().appliedCleared, 1, "counted");
    check(!ops.applied({{"nonsense", 1}}, 1), "a payload with no dev is refused, not thrown");
    eqi(k.sets, 0, "and the device was never touched");
    ops.detach(1);
    check(!k.hasParamSink(), "detach unhooks the sink");
    k.begin(0); k.value(0, 0.9); k.end(0);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(100, out)), 0, "a detached device's broadcasts go nowhere");
}

void testTwoDevicesTwoRings() {
    section("ADR-0124 d1 -- one ring per device; interleaved gestures on two devices are two edits");
    ParamOps ops;
    Knobs a, b;
    ops.attach(1, a);
    ops.attach(2, b);
    a.begin(0); b.begin(1);
    a.value(0, 0.3); b.value(1, 0.6); a.value(0, 0.35); b.value(1, 0.65);
    a.end(0); b.end(1);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(1000, out)), 4, "two openers and two edits");
    bool sawA = false, sawB = false;
    for (const OpRequest& r : out) {
        const auto dev = r.payload.at("dev").get<std::int64_t>();
        const auto n = r.payload.at("norm").get<double>();
        if (dev == 1 && near(n, 0.35)) sawA = true;
        if (dev == 2 && near(n, 0.65)) sawB = true;
    }
    check(sawA && sawB, "each device's edit ends where its own gesture ended");
}

// ---------------------------------------------------------------------------
// The CLAP path: output events on the audio thread
// ---------------------------------------------------------------------------

struct Emitter {
    clap_plugin_t plugin{};
    clap_plugin_params_t params{};
    bool bracket = true;
    double plain = 800.0;
    static Emitter& self(const clap_plugin_t* p) { return *static_cast<Emitter*>(p->plugin_data); }
    Emitter() {
        plugin.plugin_data = this;
        plugin.init = [](const clap_plugin_t*) { return true; };
        plugin.destroy = [](const clap_plugin_t*) {};
        plugin.activate = [](const clap_plugin_t*, double, std::uint32_t, std::uint32_t) { return true; };
        plugin.deactivate = [](const clap_plugin_t*) {};
        plugin.start_processing = [](const clap_plugin_t*) { return true; };
        plugin.stop_processing = [](const clap_plugin_t*) {};
        plugin.reset = [](const clap_plugin_t*) {};
        plugin.on_main_thread = [](const clap_plugin_t*) {};
        plugin.get_extension = [](const clap_plugin_t* p, const char* id) -> const void* {
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &self(p).params;
            return nullptr;
        };
        plugin.process = [](const clap_plugin_t* p, const clap_process_t* pd) -> clap_process_status {
            Emitter& e = self(p);
            for (std::uint32_t c = 0; c < pd->audio_outputs[0].channel_count; ++c)
                for (std::uint32_t i = 0; i < pd->frames_count; ++i)
                    pd->audio_outputs[0].data32[c][i] = 0.0f;
            if (pd->out_events == nullptr) return CLAP_PROCESS_CONTINUE;
            clap_event_param_gesture_t g{};
            g.header.size = sizeof g; g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            g.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN; g.param_id = 1;
            if (e.bracket) pd->out_events->try_push(pd->out_events, &g.header);
            clap_event_param_value_t v{};
            v.header.size = sizeof v; v.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            v.header.type = CLAP_EVENT_PARAM_VALUE; v.param_id = 1; v.value = e.plain;
            v.note_id = -1; v.port_index = -1; v.channel = -1; v.key = -1;
            pd->out_events->try_push(pd->out_events, &v.header);
            g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
            if (e.bracket) pd->out_events->try_push(pd->out_events, &g.header);
            return CLAP_PROCESS_CONTINUE;
        };
        params.count = [](const clap_plugin_t*) -> std::uint32_t { return 1; };
        params.get_info = [](const clap_plugin_t*, std::uint32_t i, clap_param_info_t* out) {
            if (i != 0) return false;
            *out = clap_param_info_t{};
            out->id = 1; out->flags = CLAP_PARAM_IS_AUTOMATABLE;
            std::snprintf(out->name, sizeof out->name, "Cutoff");
            out->min_value = 20.0; out->max_value = 20000.0; out->default_value = 4800.0;
            return true;
        };
        params.get_value = [](const clap_plugin_t*, clap_id, double* out) { *out = 4800.0; return true; };
        params.value_to_text = [](const clap_plugin_t*, clap_id, double, char* b, std::uint32_t n) {
            std::snprintf(b, n, "x"); return true; };
        params.text_to_value = [](const clap_plugin_t*, clap_id, const char*, double*) { return false; };
        params.flush = [](const clap_plugin_t*, const clap_input_events_t*, const clap_output_events_t*) {};
    }
};

void testTheClapPathOnTheAudioThread() {
    section("ADR-0110 d4 / ADR-0124 -- a CLAP's output events on the audio thread become one op on the message thread");
    Emitter f;
    device::DeviceIdentity id; id.format = "clap"; id.name = "Emitter";
    device::ClapDevice d(&f.plugin, id);
    ParamOps ops;
    check(ops.attach(5, d), "attached");
    d.prepare(48000.0, 64);
    std::vector<float> l(64, 0.0f), r(64, 0.0f);
    float* outp[2] = {l.data(), r.data()};
    NodeIo io;
    io.out = outp; io.channels = 2; io.frames = 64; io.sampleRate = 48000.0;
    std::thread audio([&] { d.process(io); });
    audio.join();

    std::vector<OpRequest> out;
    const std::size_t n = ops.drain(1000, out);
    eqi(static_cast<long long>(n), 2, "opener and edit");
    if (out.size() != 2) return;
    const device::ParamDescriptor* pd = d.paramAt(0);
    check(pd != nullptr && out[1].payload.at("param").get<std::string>() == pd->id,
          "on the parameter's text id: " + out[1].payload.at("param").get<std::string>());
    const double norm = out[1].payload.at("norm").get<double>();
    check(near(norm, (800.0 - 20.0) / 19980.0, 1e-9), "normalized from the declared range: " + std::to_string(norm));
    check(near(out[1].payload.value("real", -1.0), 800.0, 1e-6), "and the plain value back as real");
    const ParamEditCapture::Stats* cs = ops.captureStats(5);
    check(cs != nullptr && cs->implicitEdits == 0, "bracketed: an explicit gesture, not an implicit one");
    check(cs != nullptr && cs->unseeded == 0, "seeded from get_value at attach");
    check(near(out[0].payload.value("real", -1.0), 4800.0, 1e-6), "the opener carries the seeded 4800 Hz");
    d.release();
}

// ---------------------------------------------------------------------------
// The whole loop: a real project, the journal, an undo
// ---------------------------------------------------------------------------

bool commits(OpJournal& j, const OpRequest& r, const std::string& what) {
    const CommitResult res = j.commit(r);
    check(res.ok, what + ": " + res.error);
    return res.ok;
}

void testTheWholeLoopThroughTheJournal() {
    const adi::test::TempDirectory scratch("param_ops", "loop");
    section("ADR-0124 -- session, glue, journal, undo: the plugin ends where it started");
    StoreError e = StoreError::Ok;
    auto store = Store::create(scratch.path() / "p.adi", e);
    check(store != nullptr, "created");
    if (!store) return;
    {
        OpJournal j(*store);
        OpRequest r;
        r.opType = "track.create";
        r.payload = {{"id", 1}, {"kind", "midi"}, {"name", "Keys"}};
        commits(j, r, "Keys");
        r.payload = {{"id", 9}, {"kind", "master"}, {"name", "Master"}, {"index", 1}};
        commits(j, r, "Master");
        store->db().exec("INSERT INTO plugin_refs(id, format, uid, vendor, name, version, subtype) "
                         "VALUES (1, 'vst3', 'adi.test.knobs', 'ADI', 'Knobs', '1.0', 'effect')");
        r.opType = "chain.create";
        r.payload = {{"id", 10}, {"track", 1}};
        commits(j, r, "chain");
        r.opType = "device.insert";
        r.payload = {{"id", 100}, {"chain", 10}, {"ord", 0}, {"ref", 1}, {"name", "Knobs"}};
        commits(j, r, "device");
    }

    Session s;
    Knobs* knobs = nullptr;
    auto loader = [&](const DeviceRequest& rq, std::string& err) -> std::unique_ptr<device::DeviceInstance> {
        if (rq.ref != nullptr && rq.ref->uid == "adi.test.knobs") {
            auto k = std::make_unique<Knobs>();
            knobs = k.get();
            return k;
        }
        err = "not here";
        return nullptr;
    };
    check(s.load(*store, loader, SessionSpec{}), "loaded: " + s.error());
    check(knobs != nullptr, "the loader made Knobs");
    if (knobs == nullptr) return;

    ParamOps ops;
    eqi(static_cast<long long>(ops.attachSession(s)), 1, "attachSession attached the one loaded device");
    eqi(static_cast<long long>(ops.attachSession(s)), 0, "and nothing twice");
    check(ops.isAttached(100), "device 100 is attached");

    // The user drags cutoff in the plugin's window.
    knobs->begin(0); knobs->value(0, 0.9); knobs->end(0);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(1000, out)), 2, "opener and edit");
    if (out.size() != 2) return;

    OpJournal j(*store);
    const CommitResult res = j.commit(out);   // one transaction: one undo step
    check(res.ok, "committed as one transaction: " + res.error);
    eqi(static_cast<long long>(res.seqs.size()), 2, "two ops in it");
    {
        SQLite::Statement st(store->db(),
            "SELECT normalized_value, real_value FROM plugin_params WHERE device_id = 100 AND param_id = 'cutoff'");
        check(st.executeStep(), "the mirror row exists now");
        if (st.hasRow()) {
            check(near(st.getColumn(0).getDouble(), 0.9), "at 0.9");
            check(near(st.getColumn(1).getDouble(), 20.0 + 0.9 * 19980.0), "with its real value");
        }
    }
    // Our own ops coming back (a UI that applies every committed op): nothing to do.
    check(!ops.applied(out[1].payload, 1100), "the edit comes back equal");
    eqi(knobs->sets, 0, "the device was not touched");

    // Undo.
    History h(*store);
    check(h.canUndo(), "undo is available");
    const History::Result u = h.undo();
    check(u.ok, "undone: " + u.error);
    {
        SQLite::Statement st(store->db(),
            "SELECT COUNT(*) FROM plugin_params WHERE device_id = 100 AND param_id = 'cutoff'");
        st.executeStep();
        eqi(st.getColumn(0).getInt64(), 0, "the row is gone again: the parameter is back to never-touched");
    }
    // `History::undo` applies the stored INVERSES and moves the head; it logs
    // nothing new. So what reaches the device is each undone op's inverse,
    // newest op first: the edit's inverse (norm 0.5), then the opener's (a
    // cleared row). That is the UI's job in step 7; here the test is the UI.
    const std::vector<OpJournal::LoggedOp> recent = j.recent(2);
    int applied = 0;
    for (const OpJournal::LoggedOp& op : recent)
        if (op.opType == "device.setParam" && op.inverse && ops.applied(*op.inverse, 1200)) ++applied;
    eqi(applied, 1, "one of the two inverses set the device (the other clears a row)");
    check(near(knobs->getParam("cutoff").normalized, 0.5),
          "and the plugin is back at 0.5, where the gesture found it: " +
              std::to_string(knobs->getParam("cutoff").normalized));
    std::vector<OpRequest> after;
    eqi(static_cast<long long>(ops.drain(1300, after)), 0, "the undo's set produced no new op");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_param_ops_tests -- broadcasts in, ops out (ADR-0124)\n\n");
    testAGestureBecomesOneOp();
    testAppliedArmsTheEcho();
    testOwnOpsComeBackEqual();
    testUnknownsAreCounted();
    testTwoDevicesTwoRings();
    testTheClapPathOnTheAudioThread();
    testTheWholeLoopThroughTheJournal();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
