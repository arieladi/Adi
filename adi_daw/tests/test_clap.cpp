// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CLAP host. ADR-0052, ADR-0075.
//
// This suite needs NO PLUGIN and NO JUCE, and that is the point being made as
// much as anything tested here. `clap/clap.h` is a header-only MIT C API with
// no dependencies, so the host side compiles into `adi_core` and every check
// below runs wherever the main suite runs. The VST3 equivalents can only ever
// run in the single CI job that has a plugin SDK.
//
// "Wherever the main suite runs" is six CI configurations, not seven: the
// i386/ILP32 job never builds the tree or runs ctest -- it hand-compiles
// test_main.cpp and blob.cpp with a 32-bit g++ to prove the StreamReader
// size_t behaviour. Stated exactly because the first version of this comment
// said "all seven ABIs" and that was an assertion rather than a check.
//
// What it checks is mostly the CONVERSION, because that is where a host gets
// things silently wrong: a well-formed event carrying the wrong number makes a
// note brighter instead of sharper, and nothing fails.

#include "juce/clap_host.hpp"

#include <cmath>
#include <cstring>
#include <vector>
#include <cstdio>
#include <string>
#include <type_traits>

namespace {

using namespace adi;
using namespace adi::device;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

engine::Event noteOn(std::uint64_t id, int key, double vel, std::int32_t frame = 0) {
    engine::Event e;
    e.type = engine::EventType::NoteOn;
    e.noteId = id;
    e.dim = static_cast<std::uint16_t>(key);
    e.value = vel;
    e.frame = frame;
    e.channel = 2;
    return e;
}

engine::Event expr(std::uint64_t id, ExpressionDim d, double v) {
    engine::Event e;
    e.type = engine::EventType::NoteExpression;
    e.noteId = id;
    e.dim = static_cast<std::uint16_t>(d);
    e.value = v;
    return e;
}

// --- the mapping -------------------------------------------------------------

void testEveryDimensionHasANamedClapId() {
    section("ADR-0075 -- every ADI dimension maps to a NAMED CLAP expression");

    check(clapExprFor(ExpressionDim::Pitch) == CLAP_NOTE_EXPRESSION_TUNING,
          "Pitch is TUNING");
    check(clapExprFor(ExpressionDim::Timbre) == CLAP_NOTE_EXPRESSION_BRIGHTNESS,
          "Timbre is BRIGHTNESS");
    check(clapExprFor(ExpressionDim::Gain) == CLAP_NOTE_EXPRESSION_VOLUME, "Gain is VOLUME");
    check(clapExprFor(ExpressionDim::Pan) == CLAP_NOTE_EXPRESSION_PAN, "Pan is PAN");

    // The one that shows the difference. CLAP NAMES pressure; VST3 has no
    // pressure type at all, so MPE's Z axis had to be mapped onto
    // kExpressionTypeID by convention (ADR-0057). A convention is a thing two
    // implementations can disagree about; a named id is not.
    check(clapExprFor(ExpressionDim::Pressure) == CLAP_NOTE_EXPRESSION_PRESSURE,
          "Pressure is CLAP's own PRESSURE, not a convention");
    check(CLAP_NOTE_EXPRESSION_PRESSURE != CLAP_NOTE_EXPRESSION_EXPRESSION,
          "and PRESSURE is genuinely a different id from EXPRESSION");
}

void testTuningNeedsNoConversion() {
    section("ADR-0075 -- CLAP tuning is already semitones, so there is no conversion");

    // CLAP: "Relative tuning in semitones, from -120 to +120 ... doubles".
    // engine::Event carries semitones. VST3 needed norm = plain/240 + 0.5.
    for (double s : {-48.0, -12.0, -0.5, 0.0, 0.25, 12.0, 48.0, 120.0, -120.0}) {
        check(semitonesToClapTuning(s) == s,
              "identity within range: " + std::to_string(s));
    }

    // The clamp still applies, because an MPE zone configured beyond CLAP's
    // declared +/-120 would otherwise hand a plugin a value outside the range
    // its own API promises.
    check(semitonesToClapTuning(500.0) == 120.0, "beyond +120 clamps");
    check(semitonesToClapTuning(-500.0) == -120.0, "and below -120");
}

// --- the event list ----------------------------------------------------------

void testEventListCarriesNoteIdAndDouble() {
    section("ADR-0054 -- a real note id and a double, all the way to the plugin");

    ClapEventList list;
    list.reserve(64);

    check(list.add(noteOn(7, 60, 100.0 / 127.0)), "a note on is translated");
    check(list.add(expr(7, ExpressionDim::Pitch, 12.0)), "and a pitch expression");
    check(list.size() == 2, "both are in the list");

    const auto* h0 = list.at(0);
    check(h0 != nullptr && h0->type == CLAP_EVENT_NOTE_ON, "the first is a note on");
    const auto* n = reinterpret_cast<const clap_event_note_t*>(h0);
    check(n->note_id == 7, "carrying a REAL note id, not -1");
    check(n->key == 60, "the key");
    check(n->channel == 1, "and the channel, zero-based as CLAP wants it");
    check(std::abs(n->velocity - 100.0 / 127.0) < 1e-15,
          "velocity is a double, not value/127 rounded to 7 bits");

    const auto* h1 = list.at(1);
    check(h1 != nullptr && h1->type == CLAP_EVENT_NOTE_EXPRESSION, "the second is expression");
    const auto* x = reinterpret_cast<const clap_event_note_expression_t*>(h1);
    check(x->note_id == 7, "anchored to the same note");
    check(x->expression_id == CLAP_NOTE_EXPRESSION_TUNING, "as TUNING");
    check(x->value == 12.0,
          "and the value passes through UNCHANGED -- one octave is 12.0 "
          "semitones, not a fraction of ten octaves");

    // The value type itself, structurally. ADR-0054's trap is a narrow type in
    // an event struct, and this is the last hop before the plugin.
    static_assert(std::is_same_v<decltype(clap_event_note_expression_t::value), double>,
                  "CLAP note expression value must be a double -- ADR-0054");
    check(sizeof(x->value) == 8, "the expression value is 8 bytes");
}

void testModulationIsItsOwnEvent() {
    section("ADR-0046/0052 -- CLAP carries modulation natively");

    ClapEventList list;
    list.reserve(8);

    engine::Event val;
    val.type = engine::EventType::ParamValue;
    val.paramId = 3;
    val.value = 0.5;
    engine::Event mod;
    mod.type = engine::EventType::ParamMod;
    mod.paramId = 3;
    mod.value = 0.25;

    check(list.add(val) && list.add(mod), "both are translated");
    check(list.at(0)->type == CLAP_EVENT_PARAM_VALUE, "a stored value is PARAM_VALUE");

    // The one that matters. A modulation offset leaves the user's stored value
    // alone. On VST3 the host must resolve the two into one number and shadow
    // the user's setting (ADR-0052); CLAP has a separate event and the plugin
    // does it properly.
    check(list.at(1)->type == CLAP_EVENT_PARAM_MOD,
          "and a modulation offset is a DIFFERENT event, not folded into the value");
    check(list.at(0)->type != list.at(1)->type, "the two are genuinely distinct");
}

void testOverflowIsCountedNotSilent() {
    section("overflow is counted, not broken past in silence");
    ClapEventList list;
    list.reserve(2);
    check(list.add(noteOn(1, 60, 1.0)), "one fits");
    check(list.add(noteOn(2, 61, 1.0)), "two fit");
    check(!list.add(noteOn(3, 62, 1.0)), "the third is refused");
    check(list.dropped() == 1, "and counted -- JUCE's VST3 path breaks at 2048 silently");
    check(list.size() == 2, "the list did not grow");
}

void testTheCallbackStructIsUsable() {
    section("the clap_input_events_t handed to a plugin actually works");
    ClapEventList list;
    list.reserve(16);
    list.add(noteOn(5, 64, 0.8));
    list.add(expr(5, ExpressionDim::Pressure, 0.6));

    // Read it back exactly as a plugin would, through the C function pointers
    // rather than through our own accessors -- which is the only way to catch
    // a ctx pointer left dangling by a reallocation.
    const clap_input_events_t* in = list.inputEvents();
    check(in != nullptr && in->size != nullptr && in->get != nullptr, "the struct is populated");
    check(in->size(in) == 2, "size() reports two");
    const auto* h = in->get(in, 1);
    check(h != nullptr && h->type == CLAP_EVENT_NOTE_EXPRESSION, "get(1) is the expression");
    const auto* x = reinterpret_cast<const clap_event_note_expression_t*>(h);
    check(x->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE, "and it is PRESSURE");
    check(in->get(in, 99) == nullptr, "an out-of-range index yields null rather than garbage");
}

// --- parameter ids -----------------------------------------------------------

void testParamIdRoundTrip() {
    section("clap_id <-> TEXT, because plugin_params.param_id is TEXT");

    for (clap_id id : {0u, 1u, 0x1234abcdu, 0xFFFFFFFFu}) {
        const std::string s = ClapDevice::paramIdToText(id);
        check(s.size() == 8, "fixed width, so it sorts stably: " + s);
        clap_id back = 0;
        check(ClapDevice::paramIdFromText(s, back), "parses back");
        check(back == id, "round trips: " + s);
    }

    clap_id out = 0;
    check(!ClapDevice::paramIdFromText("123", out), "a short string is refused");
    check(!ClapDevice::paramIdFromText("zzzzzzzz", out), "and a non-hex one");
    // One spelling per id, or two rows collide on a UNIQUE key that thinks
    // they are different parameters.
    check(!ClapDevice::paramIdFromText("1234ABCD", out),
          "uppercase is refused -- one spelling per id");
}

// --- the host glue -----------------------------------------------------------

void testHostGlueReportsAndReturns() {
    section("ADR-0066 -- the host callbacks report and do nothing else");

    ClapHostGlue glue;
    const clap_host_t* h = glue.host();
    check(h != nullptr, "the host struct exists");
    check(h->host_data == &glue, "and points back at us");
    check(std::string(h->name) == "adi_daw", "named");
    check(h->clap_version.major == CLAP_VERSION_MAJOR, "declaring the CLAP version it built against");

    check(glue.restartRequests() == 0, "no restarts yet");
    h->request_restart(h);
    h->request_restart(h);
    check(glue.restartRequests() == 2,
          "a restart request is COUNTED -- the plugin may call this from any "
          "thread, and rebuilding a graph on that thread is what ADR-0066 forbids");

    h->request_process(h);
    h->request_callback(h);
    check(glue.processRequests() == 1, "process requests counted separately");
    check(glue.callbackRequests() == 1, "and callback requests");

    // An unknown extension must be nullptr. A host that returned something
    // here would have plugins calling into functions it does not implement.
    check(h->get_extension(h, "nonexistent.extension") == nullptr,
          "an unimplemented extension is nullptr, not a lie");
}

// --- the missing plugin, on this format too ----------------------------------

void testClapDeviceWithNoPluginIsSafe() {
    section("ADR-0011 -- a null plugin is inert rather than a crash");
    DeviceIdentity id;
    id.format = "clap";
    id.name = "Surge XT";
    ClapDevice d(nullptr, id);

    check(!d.loaded(), "it knows it did not load");
    check(d.paramCount() == 0, "no parameters");
    check(d.paramAt(0) == nullptr, "and reading one is safe");
    check(d.stateRoles().empty(), "no state roles");
    check(d.saveState("chunk").empty(), "saving yields nothing");
    check(!d.loadState("chunk", {1, 2, 3}), "and loading is refused");
    check(!d.setParam("00000001", ParamValue::fromNormalized(0.5)), "setParam is refused");

    // The conservative defaults, which must hold on this format too: never
    // suspend, never shift.
    check(d.tailSamples() == engine::kInfiniteTail, "an unknown tail is infinite");
    check(d.latencySamples() == 0, "and an unknown latency is zero");

    d.prepare(48000.0, 512);    // must not crash
    d.release();
    check(id.describe().find("Surge XT") != std::string::npos,
          "and the identity still names the plugin");
}

// --- a fake CLAP plugin ------------------------------------------------------
//
// CLAP is a plain C ABI: a plugin is a struct of function pointers. So a fake
// one is thirty lines, and it makes ClapDevice testable END TO END with
// nothing installed -- tail, latency, parameters and opaque state, on all
// seven ABIs.
//
// This exists because planting a defect found a hole: with a null plugin
// there is no tail extension, so the UINT32_MAX mapping below had no test at
// all and a planted defect in it passed. The fake is the fix.

struct Fake {
    clap_plugin_t plugin{};
    clap_plugin_tail_t tail{};
    clap_plugin_latency_t latency{};
    clap_plugin_params_t params{};
    clap_plugin_state_t state{};

    std::uint32_t tailValue = 0;
    std::uint32_t latencyValue = 0;
    double paramValue = 0.25;
    std::vector<std::uint8_t> saved{0xDE, 0x00, 0xAD, 0x00, 0xBE};  // embedded NULs
    int activations = 0, starts = 0;

    static Fake& self(const clap_plugin_t* p) {
        return *static_cast<Fake*>(p->plugin_data);
    }

    Fake() {
        plugin.plugin_data = this;
        plugin.init = [](const clap_plugin_t*) { return true; };
        plugin.destroy = [](const clap_plugin_t*) {};
        plugin.activate = [](const clap_plugin_t* p, double, std::uint32_t, std::uint32_t) {
            ++self(p).activations; return true; };
        plugin.deactivate = [](const clap_plugin_t*) {};
        plugin.start_processing = [](const clap_plugin_t* p) { ++self(p).starts; return true; };
        plugin.stop_processing = [](const clap_plugin_t*) {};
        plugin.reset = [](const clap_plugin_t*) {};
        plugin.process = [](const clap_plugin_t*, const clap_process_t*)
            -> clap_process_status { return CLAP_PROCESS_CONTINUE; };
        plugin.on_main_thread = [](const clap_plugin_t*) {};
        plugin.get_extension = [](const clap_plugin_t* p, const char* id) -> const void* {
            Fake& f = self(p);
            if (std::strcmp(id, CLAP_EXT_TAIL) == 0)    return &f.tail;
            if (std::strcmp(id, CLAP_EXT_LATENCY) == 0) return &f.latency;
            if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)  return &f.params;
            if (std::strcmp(id, CLAP_EXT_STATE) == 0)   return &f.state;
            return nullptr;
        };

        tail.get = [](const clap_plugin_t* p) { return self(p).tailValue; };
        latency.get = [](const clap_plugin_t* p) { return self(p).latencyValue; };

        params.count = [](const clap_plugin_t*) -> std::uint32_t { return 1; };
        params.get_info = [](const clap_plugin_t*, std::uint32_t i,
                             clap_param_info_t* out) {
            if (i != 0) return false;
            *out = clap_param_info_t{};
            out->id = 0x1234abcd;
            out->flags = CLAP_PARAM_IS_AUTOMATABLE;
            std::snprintf(out->name, sizeof out->name, "Cutoff");
            // PLAIN values, which is the whole point: 20 Hz to 20 kHz, not
            // a normalised 0..1 that means nothing without the plugin.
            out->min_value = 20.0;
            out->max_value = 20000.0;
            out->default_value = 4800.0;
            return true;
        };
        params.get_value = [](const clap_plugin_t* p, clap_id id, double* out) {
            if (id != 0x1234abcd) return false;
            *out = self(p).paramValue; return true; };
        params.value_to_text = [](const clap_plugin_t*, clap_id, double,
                                  char* buf, std::uint32_t n) {
            std::snprintf(buf, n, "x"); return true; };
        params.text_to_value = [](const clap_plugin_t*, clap_id, const char*, double*) {
            return false; };
        params.flush = [](const clap_plugin_t*, const clap_input_events_t*,
                          const clap_output_events_t*) {};

        state.save = [](const clap_plugin_t* p, const clap_ostream_t* os) {
            Fake& f = self(p);
            return os->write(os, f.saved.data(), f.saved.size())
                   == static_cast<std::int64_t>(f.saved.size()); };
        state.load = [](const clap_plugin_t* p, const clap_istream_t* is) {
            Fake& f = self(p);
            f.saved.assign(64, 0);
            const auto got = is->read(is, f.saved.data(), f.saved.size());
            f.saved.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
            return true; };
    }
};

void testAgainstAFakePlugin() {
    section("ADR-0075 -- tail, latency, parameters and state, against a real clap_plugin_t");

    Fake f;
    DeviceIdentity id;
    id.format = "clap";
    id.name = "Fake";

    {
        ClapDevice d(&f.plugin, id);
        check(d.loaded(), "it loaded");

        // THE MAPPING THE PLANTED DEFECT SLIPPED THROUGH. CLAP spells an
        // unbounded tail UINT32_MAX; ours is INT64_MAX. A plain cast makes
        // "never suspend" into 4294967295 samples -- about twenty-four hours,
        // which is wrong in a way nobody would ever notice.
        f.tailValue = UINT32_MAX;
        check(d.tailSamples() == engine::kInfiniteTail,
              "UINT32_MAX is MAPPED to kInfiniteTail, not cast");
        f.tailValue = 48000;
        check(d.tailSamples() == 48000, "and an ordinary tail passes through");
        f.tailValue = 0;
        check(d.tailSamples() == 0, "including zero, which means no tail at all");

        f.latencyValue = 2048;
        check(d.latencySamples() == 2048, "latency is reported verbatim");
        f.latencyValue = 0;
        check(d.latencySamples() == 0, "and zero stays zero");

        // Parameters, and the panel's finding made concrete.
        check(d.paramCount() == 1, "one parameter");
        const auto* pd = d.paramAt(0);
        check(pd != nullptr && pd->name == "Cutoff", "named");
        check(pd != nullptr && pd->id == "1234abcd", "with a fixed-width hex id");
        check(pd != nullptr && pd->domain == ParamDomain::Real,
              "its domain is REAL -- CLAP gives plain values, VST3 gives a string");
        check(pd != nullptr && pd->hasRealRange, "and a real range");
        check(pd != nullptr && pd->minReal == 20.0 && pd->maxReal == 20000.0,
              "20 Hz to 20 kHz, which still means something with the plugin missing");
        check(pd != nullptr && pd->defaultValue.hasReal, "the default carries a real value");
        check(pd != nullptr && std::abs(pd->defaultValue.real - 4800.0) < 1e-9,
              "of 4800 Hz");
        // And the normalised half, derived rather than invented.
        check(pd != nullptr &&
              std::abs(pd->defaultValue.normalized - (4800.0 - 20.0) / (20000.0 - 20.0)) < 1e-12,
              "with the normalised value derived from the declared range");

        f.paramValue = 9000.0;
        const auto v = d.getParam("1234abcd");
        check(v.hasReal && std::abs(v.real - 9000.0) < 1e-9, "a live read gives the plain value");
        check(v.normalized > 0.0 && v.normalized < 1.0, "and a normalised one in range");
        check(!d.getParam("deadbeef").hasReal, "an unknown id yields no real value");

        // State, byte for byte INCLUDING the embedded NULs -- the place a
        // std::string round trip would silently truncate.
        const auto blob = d.saveState("chunk");
        check(blob.size() == 5, "five bytes out, saw " + std::to_string(blob.size()));
        check(blob == std::vector<std::uint8_t>({0xDE, 0x00, 0xAD, 0x00, 0xBE}),
              "identical bytes, embedded NULs and all");
        check(d.loadState("chunk", blob), "and they load back");
        check(d.saveState("chunk") == blob, "round trip is byte-identical");
        check(d.saveState("nosuchrole").empty(), "an unknown role yields nothing");

        d.prepare(48000.0, 512);
        check(f.activations == 1, "prepare activated the plugin once");
        check(f.starts == 1, "and started processing");
        d.prepare(44100.0, 256);
        check(f.activations == 2, "re-preparing re-activates at the new size");
    }
    // The destructor must deactivate and destroy without a double-free.
    check(true, "destruction did not crash");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_clap_tests -- the CLAP host, no JUCE and no plugin\n\n");
    testEveryDimensionHasANamedClapId();
    testTuningNeedsNoConversion();
    testEventListCarriesNoteIdAndDouble();
    testModulationIsItsOwnEvent();
    testOverflowIsCountedNotSilent();
    testTheCallbackStructIsUsable();
    testParamIdRoundTrip();
    testHostGlueReportsAndReturns();
    testClapDeviceWithNoPluginIsSafe();
    testAgainstAFakePlugin();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
