// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parameter-op glue (ADR-0124): a device's own broadcasts become one
// `device.setParam` per gesture; an op from anywhere else reaches the device
// with the echo guard armed; a CLAP's output events arrive from the audio
// thread; and the whole loop closes through the journal and an undo.
// ADR-0142: a preset picked in the plugin's own browser is one
// `device.loadState`, and that loop closes through the journal too.
//
// No JUCE. The VST3 listener is the same three calls into the same sink and
// is exercised by the JUCE build.
//
// Every device here is declared BEFORE the glue: the glue's destructor unhooks
// the sinks it installed, so the devices must still exist. linux's ASan run
// (#75) caught the first version of this file getting that backwards.

#include "temp_directory.hpp"

#include "adi/engine/param_ops.hpp"
#include "adi/engine/session.hpp"
#include "adi/history.hpp"
#include "adi/media/blake3.hpp"
#include "adi/ops.hpp"
#include "adi/store.hpp"
#include "juce/clap_host.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
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
    Knobs k;        // the device outlives the glue (ADR-0124 contract)
    ParamOps ops;
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
    Knobs k;        // the device outlives the glue (ADR-0124 contract)
    ParamOps ops;
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
    Knobs k;        // the device outlives the glue (ADR-0124 contract)
    ParamOps ops;
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
    Knobs k;        // the device outlives the glue (ADR-0124 contract)
    ParamOps ops;
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
    Knobs a, b;
    ParamOps ops;
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
    auto store = Store::createEmpty(scratch.path() / "p.adi", e);
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
    const auto cutoffTouch = ops.lastTouchSequence(100);
    check(cutoffTouch != 0 && ops.lastTouched(100) == 0, "SC-317 the live Session publishes its drained native touch");
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

    // ADR-0142 aside: the undo cleared the row, so cutoff is never-touched
    // again, and its next edit must write where it starts -- or undoing THAT
    // edit would only delete the row and leave the plugin where it went.
    knobs->begin(0); knobs->value(0, 0.7); knobs->end(0);
    std::vector<OpRequest> again;
    eqi(static_cast<long long>(ops.drain(1400, again)), 2,
        "the next edit after the undo: an opener again, then the edit");
    check(ops.lastTouchSequence(100) > cutoffTouch && ops.lastTouched(100) == 0,
          "SC-317 the same live Session parameter produces a new touch");
}

// ---------------------------------------------------------------------------
// ADR-0142: the chunk. A device with state outside its parameters
// ---------------------------------------------------------------------------

/// A plugin with a preset browser. Its chunk is its two parameters AND a
/// patch number that is no parameter at all -- the wavetable, the sample, the
/// thing ADR-0110 d1 says a parameter op can never carry.
///
/// Its `loadState` behaves like a VST3 through our host: the answer to our own
/// load is muted (the epoch does not move), and `answerLater` is the plugin
/// that answers from its own timer instead.
class Sampler final : public device::DeviceInstance {
public:
    explicit Sampler(bool presetControls = false) {
        device::ParamDescriptor c;
        c.id = "cutoff"; c.name = "Cutoff";
        params_.push_back(c);
        device::ParamDescriptor g;
        g.id = "gain"; g.name = "Gain";
        params_.push_back(g);
        values_ = {0.5, 0.25};
        if (presetControls) {
            device::ParamDescriptor configure;
            configure.id = "configure";
            configure.name = "Configuration";
            configure.automatable = false;
            configure.configurable = true;
            params_.push_back(configure);
            values_.push_back(.75);
            device::ParamDescriptor meter;
            meter.id = "meter";
            meter.name = "Read-only meter";
            meter.automatable = false;
            params_.push_back(meter);
            values_.push_back(.8);
        }
        id_.format = "test"; id_.name = "Sampler";
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
        for (std::size_t i = 0; i < params_.size(); ++i)
            if (params_[i].id == id) return device::ParamValue::fromNormalized(values_[i]);
        return {};
    }
    bool setParam(const std::string& id, const device::ParamValue& v) override {
        for (std::size_t i = 0; i < params_.size(); ++i) {
            if (params_[i].id != id) continue;
            ++sets;
            values_[i] = v.normalized;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> stateRoles() const override {
        if (stateless) return {};
        return {"chunk"};
    }
    [[nodiscard]] std::string_view stateCaptureIssue() const noexcept override {
        return refuseSave && staleWhenRefused ? "remote Sampler state is stale" : "";
    }
    [[nodiscard]] std::vector<std::uint8_t> saveState(const std::string& role) const override {
        ++saveCalls;
        if (refuseSave) return {};
        if (stateless || role != "chunk") return {};
        std::vector<std::uint8_t> b(1 + 2 * sizeof(double));
        b[0] = static_cast<std::uint8_t>(patch_);
        std::memcpy(b.data() + 1, &values_[0], sizeof(double));
        std::memcpy(b.data() + 1 + sizeof(double), &values_[1], sizeof(double));
        if (auto edit = std::exchange(afterSave, {})) edit();
        return b;
    }
    bool loadState(const std::string& role, const std::vector<std::uint8_t>& b) override {
        if (stateless || role != "chunk" || b.size() != 1 + 2 * sizeof(double)) return false;
        ++loads;
        patch_ = b[0];
        std::memcpy(&values_[0], b.data() + 1, sizeof(double));
        std::memcpy(&values_[1], b.data() + 1 + sizeof(double), sizeof(double));
        // Our own load: the plugin answers, and the host mutes it.
        return true;
    }
    [[nodiscard]] std::uint64_t stateEpoch() const noexcept override { return epoch_; }

    // --- the plugin's own window ------------------------------------------
    /// Its preset browser: a new patch, the cutoff moved WITHOUT a gesture
    /// (the parameter broadcast a preset load makes), then the boundary.
    /// `broadcast` false is the plugin that moves its values silently and
    /// only signals -- CLAP's params.rescan(VALUES) is exactly that.
    void pickPreset(int patch, double cutoff, bool broadcast = true) {
        patch_ = patch;
        values_[0] = cutoff;
        if (broadcast) broadcastParam(0, ParamEventKind::Value, cutoff);
        ++epoch_;
    }
    /// A signal with nothing changed: a latency restart, a late echo.
    void answerLater() { ++epoch_; }
    void drag(std::int32_t i, double to) {
        broadcastParam(i, ParamEventKind::Begin, 0.0);
        values_[static_cast<std::size_t>(i)] = to;
        broadcastParam(i, ParamEventKind::Value, to);
        broadcastParam(i, ParamEventKind::End, 0.0);
    }
    [[nodiscard]] int patch() const noexcept { return patch_; }
    void changePatchSilently(int value) noexcept { patch_ = value; }

    bool stateless = false;
    bool refuseSave = false;
    bool staleWhenRefused = false;
    mutable int saveCalls = 0;
    mutable std::function<void()> afterSave;
    int sets = 0;
    int loads = 0;

private:
    device::DeviceIdentity id_;
    std::vector<device::ParamDescriptor> params_;
    std::vector<double> values_;
    int patch_ = 0;
    std::uint64_t epoch_ = 0;
};

std::string hashOfState(const Sampler& s) {
    const std::vector<std::uint8_t> b = s.saveState("chunk");
    const auto h = adi::media::blake3Bytes(std::as_bytes(std::span(b.data(), b.size())));
    return h.hex;
}

long long countOf(const std::vector<OpRequest>& v, const char* type) {
    long long n = 0;
    for (const OpRequest& r : v) if (r.opType == type) ++n;
    return n;
}

void testAPresetIsOneSnapshot() {
    section("ADR-0142 d1 -- a preset picked in the plugin's browser is ONE device.loadState, with the chunk it replaced first");
    Sampler s;      // the device outlives the glue (ADR-0124 contract)
    ParamOps ops;
    ops.attach(5, s);
    const std::string before = hashOfState(s);

    s.pickPreset(7, 0.9);
    const std::string after = hashOfState(s);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(1000, out)), 2, "two requests: the opener and the snapshot");
    eqi(countOf(out, "device.loadState"), 2, "both device.loadState");
    eqi(countOf(out, "device.setParam"), 0, "and NO parameter op: the preset's cutoff broadcast was absorbed");
    if (out.size() == 2) {
        check(out[0].payload.value("hash", std::string()) == before,
              "the opener carries the chunk as it was at attach (decision 6)");
        check(out[1].payload.value("hash", std::string()) == after, "the snapshot carries the new chunk");
        check(out[1].payload.value("role", std::string()) == "chunk", "role chunk");
        check(out[1].payload.value("hint", std::string()) == "test", "hinted with the format");
        check(out[1].label == "Sampler: state", "labelled for the undo menu: " + out[1].label);
    }
    const ParamEditCapture::Stats* cs = ops.captureStats(5);
    check(cs != nullptr && cs->absorbed == 1, "the capture absorbed the one ungestured edit: " +
                                                  std::to_string(cs ? cs->absorbed : -1));
    const std::vector<StateBlob> blobs = ops.takeBlobs();
    eqi(static_cast<long long>(blobs.size()), 2, "and two blobs wait to be written");
    if (blobs.size() == 2) {
        check(blobs[0].hash == before && blobs[1].hash == after, "the blobs are the requests' hashes");
        check(!blobs[1].bytes.empty() && blobs[1].bytes[0] == 7, "the new chunk holds the patch");
    }
    check(ops.takeBlobs().empty(), "taken once");

    std::vector<OpRequest> again;
    eqi(static_cast<long long>(ops.drain(1100, again)), 0, "nothing more without a new signal");
    s.answerLater();
    eqi(static_cast<long long>(ops.drain(1200, again)), 0, "a signal with an unchanged chunk writes nothing (decision 8)");
    eqi(ops.stats().snapshotsUnchanged, 1, "counted as unchanged");

    // The second preset has a row behind it now: no opener.
    s.pickPreset(3, 0.6);
    std::vector<OpRequest> second;
    eqi(static_cast<long long>(ops.drain(1300, second)), 1, "the second preset: one snapshot, no opener");
    eqi(ops.stats().snapshotOpeners, 1, "one opener in all");
}

void testAGestureAfterAPresetStartsFromIt() {
    section("ADR-0142 -- the capture is re-seeded: a drag after a preset starts where the preset put it");
    Sampler s;
    ParamOps ops;
    ops.attach(6, s);
    s.pickPreset(2, 0.8);
    std::vector<OpRequest> out;
    ops.drain(1000, out);
    s.drag(0, 0.3);
    std::vector<OpRequest> drag;
    eqi(static_cast<long long>(ops.drain(1100, drag)), 2, "first touch of cutoff: opener and edit");
    if (drag.size() == 2)
        check(near(drag[0].payload.at("norm").get<double>(), 0.8),
              "the opener is the preset's 0.8, not the 0.5 before it: " +
                  std::to_string(drag[0].payload.at("norm").get<double>()));

    // A plugin that moves its values SILENTLY and only signals: nothing was
    // broadcast for the capture to absorb, so only the re-seed knows.
    Sampler quiet;
    ParamOps ops2;
    ops2.attach(16, quiet);
    quiet.pickPreset(2, 0.8, /*broadcast=*/false);
    std::vector<OpRequest> q1;
    ops2.drain(1000, q1);
    quiet.drag(0, 0.3);
    std::vector<OpRequest> q2;
    ops2.drain(1100, q2);
    check(q2.size() == 2 && near(q2[0].payload.at("norm").get<double>(), 0.8),
          "and after a SILENT preset too: the snapshot re-seeded the capture");
}

void testAppliedMovesTheBaselineWithoutAnEcho() {
    section("ADR-0142 aside -- applied() records the value it set, even for a plugin that never echoes");
    Knobs k;        // echoOnSet stays false: this plugin says nothing back
    ParamOps ops;
    ops.attach(4, k);
    check(ops.applied({{"dev", 4}, {"param", "gain"}, {"norm", 0.7}}, 1000), "set to 0.7");
    k.begin(1); k.value(1, 0.2); k.end(1);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(2000, out)), 2, "a gesture after it: opener and edit");
    check(out.size() == 2 && near(out[0].payload.at("norm").get<double>(), 0.7),
          "the opener is the applied 0.7, not the 0.25 the capture last saw broadcast");
}

void testRowsFollowTheChunk() {
    section("ADR-0142 d3 -- a row the preset overtook is rewritten in the same transaction");
    Sampler s;
    ParamOps ops;
    ops.attach(7, s);
    s.drag(0, 0.7);                       // cutoff gets a row
    std::vector<OpRequest> edit;
    eqi(static_cast<long long>(ops.drain(1000, edit)), 2, "the drag: opener and edit");
    s.pickPreset(4, 0.2);
    std::vector<OpRequest> out;
    ops.drain(1100, out);
    eqi(countOf(out, "device.loadState"), 2, "opener and snapshot");
    eqi(countOf(out, "device.setParam"), 1, "and the cutoff row, rewritten");
    bool found = false;
    for (const OpRequest& r : out)
        if (r.opType == "device.setParam" && r.payload.value("param", std::string()) == "cutoff")
            found = near(r.payload.at("norm").get<double>(), 0.2);
    check(found, "to the preset's 0.2");
    eqi(ops.stats().rowsFollowed, 1, "counted");
    check(!out.empty() && out.back().opType == "device.setParam",
          "after the snapshot, so a replay ends where the rows say");
}

void testASaveWritesTheFirstRow() {
    section("ADR-0142 -- snapshot() is also the save: a chunk the project lacks is written, once");
    Sampler s;
    ParamOps ops;
    ops.attach(10, s);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.snapshot(10, 1000, out)), 1, "one request: the first row, no opener");
    check(out.size() == 1 && out[0].payload.value("hash", std::string()) == hashOfState(s),
          "the chunk the plugin holds");
    eqi(static_cast<long long>(ops.takeBlobs().size()), 1, "and its blob");
    std::vector<OpRequest> again;
    eqi(static_cast<long long>(ops.snapshot(10, 1100, again)), 0, "a second save of the same chunk writes nothing");
    s.answerLater();
    eqi(static_cast<long long>(ops.drain(1200, again)), 0, "nor does a signal: the drain is not a save");
    eqi(static_cast<long long>(ops.snapshot(99, 1300, again)), 0, "an unknown device saves nothing");
}

void testAStatelessDeviceWritesItsMoves() {
    section("ADR-0142 -- a device with no chunk: the signal's moves are the only record, so they are edits");
    Sampler s;
    s.stateless = true;
    ParamOps ops;
    ops.attach(8, s);
    s.pickPreset(9, 0.95);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(1000, out)), 2, "the cutoff move: opener and edit");
    eqi(countOf(out, "device.loadState"), 0, "no chunk to write");
    if (out.size() == 2) {
        check(near(out[0].payload.at("norm").get<double>(), 0.5), "the opener is where it was");
        check(near(out[1].payload.at("norm").get<double>(), 0.95), "the edit is where the preset put it");
    }
    eqi(ops.stats().statelessMoves, 1, "counted");
}

void testAnUndoneChunkReseedsTheCapture() {
    const adi::test::TempDirectory scratch("param_ops", "reseed");
    section("ADR-0142 -- an undone preset re-reads the plugin: the next drag starts where the undo put it");
    StoreError e = StoreError::Ok;
    auto store = Store::createEmpty(scratch.path() / "p.adi", e);
    check(store != nullptr, "created");
    if (!store) return;
    Sampler s;
    ParamOps ops;
    ops.attach(9, s);
    s.pickPreset(5, 0.9);
    std::vector<OpRequest> out;
    ops.drain(1000, out);
    std::string err;
    check(out.size() == 2 && ops.writeBlobs(*store, err), "opener and snapshot, blobs written: " + err);
    if (out.size() != 2) return;
    // The snapshot's inverse, by hand: the chunk the opener wrote.
    const Payload back = {{"dev", 9}, {"role", "chunk"}, {"hash", out[0].payload.at("hash")}};
    check(ops.appliedState(*store, back, 1100), "the chunk is loaded back");
    eqi(s.patch(), 0, "patch 0");
    check(!ops.appliedState(*store, back, 1150), "and a second time is a no-op: the device holds it");
    eqi(ops.stats().statesAppliedEqual, 1, "counted as equal");
    s.drag(0, 0.4);
    std::vector<OpRequest> drag;
    ops.drain(1200, drag);
    check(drag.size() == 2 && near(drag[0].payload.at("norm").get<double>(), 0.5),
          "the drag's opener is the undone 0.5, not the preset's 0.9");
    const Payload missing = {{"dev", 9}, {"role", "chunk"}, {"hash", std::string(64, 'f')}};
    check(!ops.appliedState(*store, missing, 1300), "a hash with no blob is refused");
    eqi(ops.stats().statesRefused, 1, "and counted");
}

void testTheLastTouchedParameter() {
    section("ADR-0154 -- the parameter last touched in the plug-in's window: gestures and knob turns, never echoes or presets");
    Sampler s, replacement;
    ParamOps ops;
    ops.attach(20, s);
    std::vector<OpRequest> out;
    eqi(ops.lastTouched(20), -1, "nothing touched yet");
    s.drag(1, 0.6);
    check(ops.lastTouchSequence(20) == 0, "SC-317 producer events stay unpublished until the owner drains them");
    ops.drain(1000, out);
    eqi(ops.lastTouched(20), 1, "a drag of gain: gain is the temporary entry");
    const auto firstGain = ops.lastTouchSequence(20);
    s.drag(1, 0.65);
    ops.drain(1050, out);
    check(firstGain != 0 && ops.lastTouchSequence(20) > firstGain && ops.lastTouched(20) == 1,
          "SC-317 a repeat gesture of the same knob advances the sequence");
    s.drag(0, 0.3);
    ops.drain(1100, out);
    eqi(ops.lastTouched(20), 0, "then cutoff");
    // Our own set, echoed back, is not the user's touch.
    Knobs k;
    k.echoOnSet = true;
    ParamOps ops2;
    ops2.attach(21, k);
    check(ops2.applied({{"dev", 21}, {"param", "gain"}, {"norm", 0.9}}, 2000), "our own set");
    std::vector<OpRequest> o2;
    ops2.drain(2100, o2);
    eqi(ops2.lastTouched(21), -1, "its echo is not a touch");
    check(ops2.lastTouchSequence(21) == 0, "SC-317 an echo leaves the sequence unchanged");
    k.value(1, 0.7);
    ops2.drain(2120, o2);
    const auto firstBare = ops2.lastTouchSequence(21);
    k.value(1, 0.8);
    ops2.drain(2140, o2);
    const auto repeatBare = ops2.lastTouchSequence(21);
    ops2.drain(2340, o2);
    check(firstBare != 0 && repeatBare > firstBare && ops2.lastTouchSequence(21) == repeatBare &&
              ops2.lastTouched(21) == 1,
          "SC-317 repeated bare values touch the same parameter, while quiet completion is not another touch");
    // A preset moves cutoff with no gesture: absorbed, and not a touch.
    s.drag(1, 0.7);
    ops.drain(1200, out);
    const auto beforePreset = ops.lastTouchSequence(20);
    s.pickPreset(4, 0.9);
    ops.drain(1300, out);
    eqi(ops.lastTouched(20), 1, "a preset's own broadcast does not steal the temporary entry");
    check(ops.lastTouchSequence(20) == beforePreset, "SC-317 an absorbed preset broadcast cannot advance the touch sequence");
    eqi(ops.lastTouched(99), -1, "an unknown device has none");
    check(ops.lastTouchSequence(99) == 0, "SC-317 an unknown device has no touch sequence");

    s.drag(1, 0.8); // Leave the old instance's final callbacks in the ring.
    ops.detach(20);
    check(ops.lastTouched(20) == -1 && ops.lastTouchSequence(20) == 0 && ops.recentTouches(20).empty() &&
              ops.attach(20, replacement),
          "SC-317 detaching clears access to the old touch and allows the ID to be reused");
    ops.drain(1400, out);
    check(ops.lastTouched(20) == -1 && ops.lastTouchSequence(20) == 0 && ops.recentTouches(20).empty(),
          "SC-317 an old queued callback cannot name a parameter on the replacement");
    replacement.drag(1, 0.6);
    ops.drain(1500, out);
    check(ops.lastTouched(20) == 1 && ops.lastTouchSequence(20) > beforePreset && ops.recentTouches(20).size() == 1,
          "SC-317 the replacement publishes its own fresh native touch");
}

void testNativeTouchHistory() {
    section("SC-317 -- a drain keeps each native parameter touch in chronological order");
    Knobs k;
    ParamOps ops(8);
    check(ops.attach(20, k), "attach a device with a small producer ring");
    std::vector<OpRequest> out;
    k.value(0, 0.6);
    k.value(1, 0.7);
    k.value(1, 0.8);
    k.value(0, 0.9);
    ops.drain(1000, out);
    const auto initial = ops.recentTouches(20);
    const auto lastInitial = ops.lastTouchSequence(20);
    check(initial.size() == 3 && initial[0].index == 0 && initial[1].index == 1 && initial[2].index == 0 &&
              initial[1].sequence == initial[0].sequence + 2 && initial[2].sequence == lastInitial,
          "an X/Y drain keeps both parameters, compressing only adjacent repeats at their newest sequence");
    for (int pass = 0; pass < 70; ++pass) {
        k.value(1, 0.7);
        k.value(0, 0.5);
        ops.drain(1100 + pass, out);
    }
    const auto recent = ops.recentTouches(20);
    check(recent.size() == 128 && recent.front().index == 1 && recent.front().sequence > lastInitial &&
              recent.back().index == ops.lastTouched(20) && recent.back().sequence == ops.lastTouchSequence(20),
          "the recent history keeps a bounded chronological tail even with a small capture ring");
}

void testTheStateLoopThroughTheJournal() {
    const adi::test::TempDirectory scratch("param_ops", "state");
    section("ADR-0142 -- a preset through the journal: reload keeps it, undo reaches the plugin, a row on top survives");
    StoreError e = StoreError::Ok;
    auto store = Store::createEmpty(scratch.path() / "p.adi", e);
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
                         "VALUES (1, 'vst3', 'adi.test.sampler', 'ADI', 'Sampler', '1.0', 'instrument')");
        r.opType = "chain.create";
        r.payload = {{"id", 10}, {"track", 1}};
        commits(j, r, "chain");
        r.opType = "device.insert";
        r.payload = {{"id", 100}, {"chain", 10}, {"ord", 0}, {"ref", 1}, {"name", "Sampler"}};
        commits(j, r, "device");
    }
    std::vector<Sampler*> made;
    auto loader = [&](const DeviceRequest& rq, std::string& err) -> std::unique_ptr<device::DeviceInstance> {
        if (rq.ref != nullptr && rq.ref->uid == "adi.test.sampler") {
            auto d = std::make_unique<Sampler>();
            made.push_back(d.get());
            return d;
        }
        err = "not here";
        return nullptr;
    };
    auto rowHash = [&]() -> std::string {
        SQLite::Statement st(store->db(),
            "SELECT state_hash FROM plugin_state WHERE device_id = 100 AND stream_role = 'chunk'");
        return st.executeStep() ? st.getColumn(0).getString() : std::string("(none)");
    };

    Session s;
    check(s.load(*store, loader, SessionSpec{}), "loaded: " + s.error());
    if (made.size() != 1) { check(false, "the loader made one Sampler"); return; }
    Sampler& live = *made[0];
    ParamOps ops;
    eqi(static_cast<long long>(ops.attachSession(s)), 1, "attached");
    const std::string baseline = hashOfState(live);

    // --- the preset, committed -------------------------------------------
    live.pickPreset(7, 0.9);
    std::vector<OpRequest> out;
    eqi(static_cast<long long>(ops.drain(1000, out)), 2, "opener and snapshot");
    OpJournal j(*store);
    auto legacy = out;
    for (auto& request : legacy) request.capturedTransaction.reset();
    const CommitResult bad = j.commit(legacy);
    check(!bad.ok && rowHash() == "(none)", "raw legacy state packet without blobs still refuses its foreign key: " + bad.error);
    const CommitResult res = j.commit(out);
    check(res.ok && !out.empty() && out.front().capturedTransaction &&
              out.front().capturedTransaction->phase() == CapturedSubmissionPhase::Durable,
          "session-bound typed packet atomically installs its owned blobs without writeBlobs: " + res.error);
    const auto committedCount = j.count();
    const auto committedHead = j.headSeq();
    const CommitResult repeated = j.commit(out);
    check(!repeated.ok && j.count() == committedCount && j.headSeq() == committedHead,
          "the same Durable captured preset cannot submit a second transaction");
    std::string err;
    check(ops.writeBlobs(*store, err), "legacy staging bridge remains valid after atomic commit: " + err);
    const std::string presetHash = hashOfState(live);
    check(rowHash() == presetHash, "the row holds the preset's chunk");

    // --- a second session, from the file -----------------------------------
    {
        std::vector<Sampler*> keep;
        keep.swap(made);
        Session s2;
        check(s2.load(*store, loader, SessionSpec{}), "reloaded: " + s2.error());
        eqi(s2.stats().statesLoaded, 1, "the chunk loaded");
        check(made.size() == 1 && made[0]->patch() == 7, "the patch came back: 7");
        check(made.size() == 1 && near(made[0]->getParam("cutoff").normalized, 0.9), "and cutoff 0.9");
        made.swap(keep);
    }

    // --- undo: the inverses reach the plugin, newest first -----------------
    History h(*store);
    const History::Result u = h.undo();
    check(u.ok, "undone: " + u.error);
    check(rowHash() == "(none)", "the row is gone again: the project had no chunk before the preset");
    int loadedBack = 0;
    for (const OpJournal::LoggedOp& op : j.recent(2))
        if (op.opType == "device.loadState" && op.inverse && ops.appliedState(*store, *op.inverse, 1100))
            ++loadedBack;
    eqi(loadedBack, 1, "one inverse loaded a chunk; the other clears the row");
    eqi(live.patch(), 0, "the plugin's patch is back to 0");
    check(near(live.getParam("cutoff").normalized, 0.5), "and cutoff to 0.5");
    check(hashOfState(live) == baseline, "byte for byte the chunk it had");
    eqi(ops.stats().statesCleared, 1, "the opener's inverse was the clearing one");
    std::vector<OpRequest> after;
    eqi(static_cast<long long>(ops.drain(1200, after)), 0, "the undo made no op");
    live.answerLater();   // the plugin answers the load from its timer
    eqi(static_cast<long long>(ops.drain(1300, after)), 0, "nor did the plugin's late answer (decision 8)");

    // --- redo: the forward payloads, oldest first -----------------------------
    const History::Result rd = h.redo();
    check(rd.ok, "redone: " + rd.error);
    std::vector<OpJournal::LoggedOp> fwd = j.recent(2);
    int redone = 0;
    for (auto it = fwd.rbegin(); it != fwd.rend(); ++it)
        if (it->opType == "device.loadState" && ops.appliedState(*store, it->payload, 1400)) ++redone;
    eqi(redone, 1, "the opener's chunk is already there; the preset's is loaded");
    eqi(live.patch(), 7, "the patch is 7 again");
    check(rowHash() == presetHash, "and the row holds it");

    // --- a drag after the preset: a row newer than the chunk ------------------
    live.drag(0, 0.35);
    std::vector<OpRequest> drag;
    eqi(static_cast<long long>(ops.drain(1500, drag)), 2, "opener and edit");
    check(j.commit(drag).ok, "committed");
    {
        std::vector<Sampler*> keep;
        keep.swap(made);
        Session s3;
        check(s3.load(*store, loader, SessionSpec{}), "reloaded: " + s3.error());
        eqi(s3.stats().statesLoaded, 1, "the chunk loaded");
        eqi(s3.stats().paramsApplied, 1, "and the one row it disagrees with went on top");
        check(made.size() == 1 && made[0]->patch() == 7, "the patch is the chunk's");
        check(made.size() == 1 && near(made[0]->getParam("cutoff").normalized, 0.35),
              "the cutoff is the drag's 0.35, not the chunk's 0.9");
        made.swap(keep);
    }

    // --- a second preset overtakes the row -------------------------------------
    live.pickPreset(3, 0.6);
    std::vector<OpRequest> p2;
    ops.drain(1600, p2);
    eqi(countOf(p2, "device.loadState"), 1, "one snapshot: the project has a row now, no opener");
    eqi(countOf(p2, "device.setParam"), 1, "and the cutoff row follows it");
    check(ops.writeBlobs(*store, err), "blobs: " + err);
    check(j.commit(p2).ok, "committed");
    {
        std::vector<Sampler*> keep;
        keep.swap(made);
        Session s4;
        check(s4.load(*store, loader, SessionSpec{}), "reloaded: " + s4.error());
        check(made.size() == 1 && made[0]->patch() == 3, "patch 3");
        check(made.size() == 1 && near(made[0]->getParam("cutoff").normalized, 0.6),
              "cutoff 0.6: the row followed the chunk, so it did not drag the old 0.35 back on top");
        eqi(s4.stats().paramsMatched, 1, "the row matched the chunk");
        made.swap(keep);
    }
}

void testReadPresetState() {
    section("SC-85 -- capture a coherent live preset without project changes");
    const adi::test::TempDirectory scratch("param_ops", "read_preset");
    StoreError storeError = StoreError::Ok;
    auto store = Store::createEmpty(scratch.path() / "p.adi", storeError);
    check(store != nullptr, "create live preset project");
    if (!store) return;
    store->db().exec(
        "INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys'),(9,'master','Master');"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','preset.sampler','Sampler','instrument');"
        "INSERT INTO device_chains(id,track_id) VALUES(10,1);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES(100,10,0,1,'A'),(101,10,1,1,'B');");
    Session session;
    const auto loader = [](const DeviceRequest& request, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        auto plugin = std::make_unique<Sampler>(true);
        plugin->stateless = request.device->id == 101;
        return plugin;
    };
    check(session.load(*store, loader, SessionSpec{}), "load preset Session");
    auto* sampler = dynamic_cast<Sampler*>(session.instanceFor(100));
    if (!sampler) { check(false, "resolve the preset device"); return; }
    ParamOps ops;
    check(ops.attachSession(session) == 2, "attach both preset devices");
    sampler->changePatchSilently(13);
    check(sampler->setParam("cutoff", device::ParamValue::fromNormalized(.625)), "change the live cutoff without writing a row");
    std::string error;
    const auto saved = ops.readDevicePresetState(100, error);
    check(saved && error.empty() && saved->parameters.size() == 3 && !saved->parameters.contains("meter") &&
          saved->parameters.at("cutoff") == .625 && saved->parameters.at("configure") == .75 &&
          saved->state.size() == 1 && saved->state.at("chunk").front() == std::byte{13},
          "save the current writable values and opaque state, skipping the meter");
    const auto stateless = ops.readDevicePresetState(101, error);
    check(stateless && stateless->parameters.size() == 3 && stateless->state.empty() && error.empty(),
          "a device without opaque streams still exports its parameters");
    sampler->refuseSave = true;
    check(!ops.readDevicePresetState(100, error) && !error.empty(), "refuse an incomplete declared stream");
    sampler->refuseSave = false;
    sampler->afterSave = [&] { sampler->drag(0, .75); };
    check(!ops.readDevicePresetState(100, error) && error.find("changed") != std::string::npos,
          "a control change during serialization refuses the snapshot");
    check(!ops.readDevicePresetState(100, error) && !error.empty(), "capture leaves the queued edit for its normal owner");
    check(!ops.readDevicePresetState(999, error) && !error.empty(), "an unknown device cannot export state");
    check(store->db().execAndGet("SELECT count(*) FROM ops").getInt() == 0 &&
          store->db().execAndGet("SELECT count(*) FROM plugin_state").getInt() == 0 && sampler->loads == 0,
          "reading a preset does not write history, save project state or reload the instance");
}

void testSnapshotWithMissingDevice() {
    section("SC-09 -- an unloaded plug-in keeps its saved rows without a runtime receipt");
    const adi::test::TempDirectory scratch("param_ops", "snapshot_missing");
    StoreError error = StoreError::Ok;
    auto store = Store::createEmpty(scratch.path() / "p.adi", error);
    check(store != nullptr, "create project for an unavailable plug-in");
    if (!store) return;
    store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'midi','Keys',0),(9,'master','Master',1);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','missing.sampler','Sampler','instrument');");
    Session session;
    check(session.load(*store, {}, SessionSpec{}), "load without the sampler plug-in");
    ParamOps ops;
    ops.attachSession(session);
    std::vector<OpRequest> requests(3);
    requests[0].opType = "chain.create";
    requests[0].payload = {{"id", 10}, {"track", 1}, {"ord", 0}, {"name", "Devices"}};
    requests[1].opType = "device.insert";
    requests[1].payload = {{"id", 100}, {"chain", 10}, {"ord", 0}, {"ref", 1}, {"name", "Sampler"}, {"latency", 0}};
    requests[2].opType = "device.setParam";
    requests[2].payload = {{"dev", 100}, {"param", "cutoff"}, {"norm", 0.75}};
    OpJournal journal(*store);
    const auto committed = journal.commit(requests);
    check(committed.ok && session.refresh(*store), "insert and realise a missing plug-in with saved parameters");
    ops.attachSession(session);
    OpRequest rename;
    rename.opType = "track.rename";
    rename.payload = {{"id", 1}, {"name", "After the placeholder"}};
    check(journal.commit(rename).ok && session.refresh(*store),
          "realise a second edit before pumping the earlier receipt");
    ops.attachSession(session);
    const auto captured = ops.snapshotAll(1000);
    const auto *entry = session.entryFor(100);
    check(entry && entry->placeholder && !ops.isAttached(100) && captured.ok() && captured.journalOps == 0 &&
              ops.appliedDeliveryStatus().state == AppliedDeliveryState::Idle &&
              near(store->db().execAndGet("SELECT normalized_value FROM plugin_params").getDouble(), 0.75),
          "save acknowledges the older stored-only receipt without waiting for a plug-in or changing its values");
    const auto bestEffort = ops.snapshotAll(1100, ParamOps::SnapshotGestures::FinishOpen,
                                            ParamOps::SnapshotPolicy::UseStoredOnCaptureFailure);
    check(bestEffort.ok() && bestEffort.staleDevices.empty(),
          "best-effort save retains the existing policy for an unloaded local plug-in");
}

void testSnapshotAll() {
    section("PD-03 -- save every silent changed device before a snapshot or close");
    const adi::test::TempDirectory scratch("param_ops", "snapshot_all");
    StoreError error = StoreError::Ok;
    auto store = Store::createEmpty(scratch.path() / "p.adi", error);
    check(store != nullptr, "create snapshot project");
    if (!store) return;
    store->db().exec(
        "INSERT INTO tracks(id,kind,name,index_in_parent) VALUES(1,'midi','Keys',0),(9,'master','Master',1);"
        "INSERT INTO plugin_refs(id,format,uid,name,subtype) VALUES(1,'vst3','silent.sampler','Sampler','instrument');"
        "INSERT INTO device_chains(id,track_id) VALUES(10,1);"
        "INSERT INTO devices(id,chain_id,ord,plugin_ref_id,name) VALUES"
        "(100,10,0,1,'Sampler A'),(101,10,1,1,'Sampler B');");
    Session session;
    const auto loader = [](const DeviceRequest&, std::string&) -> std::unique_ptr<device::DeviceInstance> {
        return std::make_unique<Sampler>();
    };
    check(session.load(*store, loader, SessionSpec{}), "load snapshot Session");
    auto* a = dynamic_cast<Sampler*>(session.instanceFor(100));
    auto* b = dynamic_cast<Sampler*>(session.instanceFor(101));
    if (!a || !b) {
        check(false, "resolve both hosted samplers");
        return;
    }
    ParamOps ops;
    eqi(static_cast<long long>(ops.attachSession(session)), 2, "attach both loaded samplers");
    const auto rowHash = [&](std::int64_t id) {
        SQLite::Statement row(store->db(), "SELECT state_hash FROM plugin_state WHERE device_id=? AND stream_role='chunk'");
        row.bind(1, id);
        return row.executeStep() ? row.getColumn(0).getString() : std::string{};
    };
    const auto count = [&] {
        return store->db().execAndGet("SELECT count(*) FROM ops WHERE op_type='device.loadState'").getInt();
    };

    const auto aBeforeFirst = a->saveCalls;
    const auto bBeforeFirst = b->saveCalls;
    const auto first = ops.snapshotAll(1000);
    check(a->saveCalls == aBeforeFirst + 1 && b->saveCalls == bBeforeFirst + 1,
          "acknowledging the explicit save does not request the captured bytes again");
    check(first.ok() && first.journalOps == 2 && first.stateOps == 2 && count() == 2 &&
          rowHash(100) == hashOfState(*a) && rowHash(101) == hashOfState(*b),
          "first save writes one missing state row per attached device");
    a->changePatchSilently(7);
    const auto atSnapshot = ops.snapshotAll(1100);
    check(a->stateEpoch() == 0 && atSnapshot.ok() && atSnapshot.journalOps == 1 &&
          atSnapshot.stateOps == 1 && count() == 3 && rowHash(100) == hashOfState(*a),
          "Take Snapshot saves even unsignalled edits and writes one changed chunk");
    const auto named = History(*store).takeSnapshot("Snapshot with patch 7", 1000000, 0);
    check(named.ok, "named history point follows the durable device state");

    a->changePatchSilently(9);
    b->changePatchSilently(4);
    const auto beforeClose = ops.snapshotAll(1200);
    check(a->stateEpoch() == 0 && b->stateEpoch() == 0 && beforeClose.ok() && beforeClose.journalOps == 2 &&
          beforeClose.stateOps == 2 && count() == 5 && rowHash(100) == hashOfState(*a) &&
          rowHash(101) == hashOfState(*b),
          "close saves one changed chunk for each of two silent devices");
    check(a->loads == 0 && b->loads == 0, "capturing state does not load a chunk into either live device");

    const auto unchanged = ops.snapshotAll(1300);
    check(unchanged.ok() && unchanged.journalOps == 0 && unchanged.stateOps == 0 && count() == 5,
          "unchanged complete capture succeeds without appending history");

    const auto prior = rowHash(100);
    a->changePatchSilently(12);
    a->refuseSave = true;
    const auto refused = ops.snapshotAll(1400);
    check(!refused.ok() && refused.status.error == gesture::Error::CaptureFailed && refused.failedDevice == 100 &&
          refused.journalOps == 0 && count() == 5 && rowHash(100) == prior,
          "refused bytes are a capture failure with a device receipt");
    a->refuseSave = false;
    const auto retried = ops.snapshotAll(1500);
    check(retried.ok() && retried.stateOps == 1 && count() == 6 && rowHash(100) == hashOfState(*a),
          "once capture resumes, retry saves the same silent edit once");


    const auto beforeRefusal = rowHash(100);
    a->changePatchSilently(15);
    store->db().exec("CREATE TEMP TRIGGER fail_snapshot_insert BEFORE INSERT ON ops "
                     "WHEN NEW.op_type='device.loadState' "
                     "BEGIN SELECT RAISE(ABORT, 'test snapshot commit refusal'); END");
    const auto commitRefused = ops.snapshotAll(1600);
    check(!commitRefused.ok() && commitRefused.status.error == gesture::Error::CommitRefused &&
          commitRefused.failedDevice == 100 && commitRefused.journalOps == 0 &&
          count() == 6 && rowHash(100) == beforeRefusal,
          "journal refusal leaves prior saved bytes and reports failure before publication");
    const auto retained = ops.pendingGestures();
    store->db().exec("DROP TRIGGER fail_snapshot_insert");
    if (retained.size() != 1 || !retained.front().owner) {
        check(false, "refused snapshot retains its owner and exact captured state");
        return;
    }
    const auto resumed = ops.retryPendingCommit(*retained.front().owner, 1700);
    check(resumed.status.ok() && resumed.durable && resumed.durable->ok &&
          count() == 7 && rowHash(100) == hashOfState(*a),
          "explicit journal retry saves the retained snapshot once");

    a->changePatchSilently(21);
    const auto captured = hashOfState(*a);
    a->afterSave = [&] { a->changePatchSilently(22); };
    const auto concurrent = ops.snapshotAll(1800);
    check(concurrent.ok() && concurrent.stateOps == 1 && count() == 8 && rowHash(100) == captured &&
          a->stateEpoch() == 0 && a->patch() == 22 && a->loads == 0,
          "saving the sampled state does not restore it over a later silent device edit");
    const auto latest = ops.snapshotAll(1900);
    check(latest.ok() && latest.stateOps == 1 && count() == 9 && rowHash(100) == hashOfState(*a),
          "the later silent edit is saved at the next explicit request");

    a->changePatchSilently(23);
    b->refuseSave = true;
    const auto selected = ops.snapshotDeviceState(100, 2000, ParamOps::SnapshotGestures::FinishOpen);
    check(selected.ok() && selected.stateOps == 1 && count() == 10 && rowHash(100) == hashOfState(*a),
          "capturing one device does not read an unrelated plug-in's opaque state");

    const auto lastRemoteState = rowHash(100);
    a->changePatchSilently(30);
    a->staleWhenRefused = true;
    a->refuseSave = true;
    const auto remoteRefused = ops.snapshotAll(2100);
    check(!remoteRefused.ok() && remoteRefused.failedDevice == 100 &&
          remoteRefused.status.message == "remote Sampler state is stale" && rowHash(100) == lastRemoteState,
          "a strict remote capture identifies the stale device and its reported diagnostic");

    b->refuseSave = false;
    b->changePatchSilently(31);
    using Gestures = ParamOps::SnapshotGestures;
    using Policy = ParamOps::SnapshotPolicy;
    const auto stale = ops.snapshotAll(2200, Gestures::FinishOpen, Policy::UseStoredOnCaptureFailure);
    check(stale.ok() && stale.stateOps == 1 && count() == 11 && rowHash(100) == lastRemoteState &&
          rowHash(101) == hashOfState(*b) && stale.staleDevices == std::vector<std::int64_t>{100},
          "best-effort save keeps the remote device's old blob and records the other device");

    Session reopened;
    const auto initiallyStale = [](const DeviceRequest& request, std::string&)
        -> std::unique_ptr<device::DeviceInstance> {
        auto sampler = std::make_unique<Sampler>();
        sampler->refuseSave = sampler->staleWhenRefused = request.device->id == 100;
        return sampler;
    };
    ParamOps afterReopen;
    check(reopened.load(*store, initiallyStale, SessionSpec{}) && afterReopen.attachSession(reopened) == 2,
          "an initially stale remote device does not prevent attaching after reopening");
    const auto reopenedSave = afterReopen.snapshotAll(2300, Gestures::FinishOpen, Policy::UseStoredOnCaptureFailure);
    check(reopenedSave.ok() && reopenedSave.staleDevices == std::vector<std::int64_t>{100} &&
          rowHash(100) == lastRemoteState,
          "saving after reopening retains the stored remote state");

    store->db().exec("INSERT INTO remote_hosts(id,name,host,port) VALUES(7,'Fixture','127.0.0.1',55056);"
                     "UPDATE devices SET remote_host_id=7 WHERE id=100;");
    Session disconnected;
    const auto unavailable = [](const DeviceRequest& request, std::string& error)
        -> std::unique_ptr<device::DeviceInstance> {
        if (request.device->remoteHostId) {
            error = "server #7 is unavailable";
            return {};
        }
        return std::make_unique<Sampler>();
    };
    ParamOps disconnectedOps;
    const bool opened = disconnected.load(*store, unavailable, SessionSpec{});
    const auto* placeholder = disconnected.entryFor(100);
    check(opened && disconnectedOps.attachSession(disconnected) == 1 && placeholder && placeholder->placeholder,
          "opening without the server leaves an unloaded remote placeholder");
    const auto disconnectedSave = disconnectedOps.snapshotAll(2400, Gestures::FinishOpen,
                                                              Policy::UseStoredOnCaptureFailure);
    check(disconnectedSave.ok() && disconnectedSave.staleDevices == std::vector<std::int64_t>{100} &&
          rowHash(100) == lastRemoteState,
          "a remote device unavailable on first open enters the stale receipt and keeps its saved bytes");
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
    testAPresetIsOneSnapshot();
    testAGestureAfterAPresetStartsFromIt();
    testAppliedMovesTheBaselineWithoutAnEcho();
    testRowsFollowTheChunk();
    testASaveWritesTheFirstRow();
    testSnapshotWithMissingDevice();
    testSnapshotAll();
    testReadPresetState();
    testAStatelessDeviceWritesItsMoves();
    testAnUndoneChunkReseedsTheCapture();
    testTheStateLoopThroughTheJournal();
    testTheLastTouchedParameter();
    testNativeTouchHistory();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
