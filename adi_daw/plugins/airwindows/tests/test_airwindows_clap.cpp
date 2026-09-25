// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI Airwindows, driven the way a host drives it (ADR-0166): the factory,
// every kept effect created, described, run and saved, parameter changes on
// the sample they are stamped with, and auto gain where the catalogue says.
//
// Its own binary: it replaces global operator new to observe that process()
// allocates nothing, for every effect.

#include "airwindows_clap.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using adi::airwindows::kAutoGainParamId;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

constexpr double kFs = 48000.0;
constexpr uint32_t kBlock = 512;
constexpr double kPi = 3.14159265358979323846;

// --- a host, as small as CLAP allows -------------------------------------------

const void* hostExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}

clap_host_t makeHost() {
    clap_host_t h{};
    h.clap_version = CLAP_VERSION_INIT;
    h.name = "adi_airwindows_tests";
    h.vendor = "ADI";
    h.url = "";
    h.version = "0";
    h.get_extension = hostExtension;
    h.request_restart = hostRequest;
    h.request_process = hostRequest;
    h.request_callback = hostRequest;
    return h;
}
const clap_host_t g_host = makeHost();

struct Events {
    std::vector<clap_event_param_value_t> list;
    clap_input_events_t in{};
    Events() {
        list.reserve(16);
        in.ctx = this;
        in.size = [](const clap_input_events_t* e) {
            return static_cast<uint32_t>(static_cast<const Events*>(e->ctx)->list.size());
        };
        in.get = [](const clap_input_events_t* e, uint32_t i) {
            return &static_cast<const Events*>(e->ctx)->list[i].header;
        };
    }
    void add(uint32_t time, clap_id id, double value) {
        clap_event_param_value_t e{};
        e.header.size = sizeof(e);
        e.header.time = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = id;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = value;
        list.push_back(e);
    }
};

bool noPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t g_noOut{nullptr, noPush};

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t n = 0) : l(n), r(n) {}
};

/// -20 dBFS of a 220 Hz tone under noise, the same every run.
Stereo source(std::size_t frames) {
    Stereo s(frames);
    unsigned seed = 777u;
    for (std::size_t i = 0; i < frames; ++i) {
        const double tone = std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / kFs);
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 16777216.0) * 2.0 - 1.0;
        s.l[i] = static_cast<float>(0.1 * (0.7 * tone + 0.3 * noise));
        s.r[i] = static_cast<float>(0.1 * (0.7 * tone - 0.3 * noise));
    }
    return s;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i) e += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(e / static_cast<double>(to - from) + 1e-30);
}

const clap_plugin_factory_t* factory() {
    return static_cast<const clap_plugin_factory_t*>(
        adi::airwindows::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
}

/// One running instance.
struct Instance {
    const clap_plugin_t* p = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_audio_ports_t* ports = nullptr;

    explicit Instance(const char* id) {
        p = factory()->create_plugin(factory(), &g_host, id);
        if (p == nullptr || !p->init(p)) {
            p = nullptr;
            return;
        }
        params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
        ports = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
        p->activate(p, kFs, 1, kBlock);
        p->start_processing(p);
    }
    ~Instance() {
        if (p != nullptr) {
            p->stop_processing(p);
            p->deactivate(p);
            p->destroy(p);
        }
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    /// Runs `in` through in blocks into `out` (sized already, so nothing is
    /// allocated here); `firstBlockEvents` go into the first block.
    void runInto(const Stereo& in, Stereo& out, Events* firstBlockEvents = nullptr,
                 uint32_t block = kBlock) {
        for (std::size_t s = 0; s < in.l.size(); s += block) {
            const uint32_t n = static_cast<uint32_t>(std::min<std::size_t>(block, in.l.size() - s));
            float* ip[2] = {const_cast<float*>(in.l.data() + s), const_cast<float*>(in.r.data() + s)};
            float* op[2] = {out.l.data() + s, out.r.data() + s};
            clap_audio_buffer_t ib{};
            ib.data32 = ip;
            ib.channel_count = 2;
            clap_audio_buffer_t ob{};
            ob.data32 = op;
            ob.channel_count = 2;
            clap_process_t proc{};
            proc.steady_time = -1;
            proc.frames_count = n;
            proc.audio_inputs = &ib;
            proc.audio_outputs = &ob;
            proc.audio_inputs_count = 1;
            proc.audio_outputs_count = 1;
            proc.in_events = (s == 0 && firstBlockEvents != nullptr) ? &firstBlockEvents->in : &none_.in;
            proc.out_events = &g_noOut;
            p->process(p, &proc);
        }
    }
    Stereo run(const Stereo& in, Events* firstBlockEvents = nullptr, uint32_t block = kBlock) {
        Stereo out(in.l.size());
        runInto(in, out, firstBlockEvents, block);
        return out;
    }
    Events none_;

    void set(clap_id id, double value) {
        Events e;
        e.add(0, id, value);
        params->flush(p, &e.in, &g_noOut);
    }
    double get(clap_id id) const {
        double v = -1.0;
        params->get_value(p, id, &v);
        return v;
    }
    std::string save() const {
        std::string out;
        clap_ostream_t os{&out, [](const clap_ostream_t* s, const void* buf, uint64_t n) -> int64_t {
                              static_cast<std::string*>(s->ctx)->append(static_cast<const char*>(buf), n);
                              return static_cast<int64_t>(n);
                          }};
        return state->save(p, &os) ? out : std::string("(save failed)");
    }
    bool load(const std::string& data) {
        struct Reader {
            const std::string* s;
            std::size_t at;
        } r{&data, 0};
        clap_istream_t is{&r, [](const clap_istream_t* s, void* buf, uint64_t n) -> int64_t {
                              auto* rd = static_cast<Reader*>(s->ctx);
                              const std::size_t k = std::min<std::size_t>(n, rd->s->size() - rd->at);
                              std::memcpy(buf, rd->s->data() + rd->at, k);
                              rd->at += k;
                              return static_cast<int64_t>(k);
                          }};
        return state->load(p, &is);
    }
};

std::string idOf(const char* name) {
    std::string id = "com.adi.airwindows.";
    for (const char* c = name; *c != '\0'; ++c) id += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    return id;
}

// --- the tests -----------------------------------------------------------------

void testTheFactoryListsTheKeptEffects() {
    section("ADR-0166 -- one binary, every kept effect its own CLAP plug-in");

    check(adi::airwindows::entryInit(""), "the entry initialises");
    const clap_plugin_factory_t* f = factory();
    check(f != nullptr, "and hands out the plug-in factory");
    check(adi::airwindows::entryGetFactory("clap.nothing") == nullptr, "and no other factory");
    const uint32_t n = f->get_plugin_count(f);
    check(n >= 130 && n <= 160, "about the 141 of CATALOGUE.md, not all 524: " + std::to_string(n));
    std::set<std::string> ids;
    bool shaped = true;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
        ids.insert(d->id);
        const char* const* feat = d->features;
        int k = 0;
        while (feat[k] != nullptr) ++k;
        if (std::string(d->id) != idOf(d->name) || std::strlen(d->description) == 0 ||
            std::strcmp(feat[0], CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) != 0 ||
            std::strcmp(feat[k - 1], CLAP_PLUGIN_FEATURE_STEREO) != 0 ||
            !clap_version_is_compatible(d->clap_version))
            shaped = false;
    }
    check(ids.size() == n, "every id is different");
    check(shaped, "every descriptor: our id from its name, a description, audio-effect ... stereo");
    for (const char* name : {"Density3", "Pressure5", "Channel9", "ToTape9", "Galactic", "kCathedral5"})
        check(ids.count(idOf(name)) == 1, std::string(name) + " is there");
    for (const char* name : {"Density", "Density2", "Pressure4", "Galactic3"})
        check(ids.count(idOf(name)) == 0, std::string(name) + " is stripped");
    check(f->create_plugin(f, &g_host, "com.adi.airwindows.nothing") == nullptr,
          "an unknown id creates nothing");
}

void testEveryEffectRunsCleanly() {
    section("ADR-0166 -- every kept effect: parameters, a second of audio with no allocation, state");

    const clap_plugin_factory_t* f = factory();
    const Stereo in = source(static_cast<std::size_t>(kFs));
    int created = 0, described = 0, finite = 0, restored = 0;
    long allocs = 0;
    std::string bad;
    for (uint32_t i = 0; i < f->get_plugin_count(f); ++i) {
        const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
        Instance fx(d->id);
        if (fx.p == nullptr || fx.params == nullptr || fx.state == nullptr || fx.ports == nullptr) {
            bad += std::string(" ") + d->name + "(create)";
            continue;
        }
        ++created;

        const uint32_t np = fx.params->count(fx.p);
        bool ok = np >= 1;
        for (uint32_t k = 0; k < np; ++k) {
            clap_param_info_t info{};
            ok = ok && fx.params->get_info(fx.p, k, &info) && info.name[0] != '\0' &&
                 info.min_value == 0.0 && info.max_value == 1.0 && info.default_value >= 0.0 &&
                 info.default_value <= 1.0;
            char text[256] = {};
            ok = ok && fx.params->value_to_text(fx.p, info.id, info.default_value, text, sizeof(text)) &&
                 text[0] != '\0';
            if (k + 1 == np) ok = ok && info.id == kAutoGainParamId;
        }
        clap_audio_port_info_t port{};
        ok = ok && fx.ports->count(fx.p, true) == 1 && fx.ports->get(fx.p, 0, true, &port) &&
             port.channel_count == 2;
        if (ok) ++described;
        else bad += std::string(" ") + d->name + "(params)";

        Stereo out(in.l.size());
        g_allocs.store(0);
        g_counting.store(true);
        fx.runInto(in, out);
        g_counting.store(false);
        if (g_allocs.load() != 0) bad += std::string(" ") + d->name + "(alloc)";
        allocs += g_allocs.load();
        bool fin = true;
        for (std::size_t k = 0; k < out.l.size(); ++k)
            if (!std::isfinite(out.l[k]) || !std::isfinite(out.r[k])) fin = false;
        if (fin) ++finite;
        else bad += std::string(" ") + d->name + "(nan)";

        // Move every parameter off its default, save, load into a fresh one.
        for (uint32_t k = 0; k + 1 < np; ++k) fx.set(k, 0.25 + 0.5 * ((k * 37) % 11) / 11.0);
        fx.set(kAutoGainParamId, 1.0);
        const std::string saved = fx.save();
        Instance again(d->id);
        if (again.p != nullptr && again.load(saved) && again.save() == saved &&
            again.get(kAutoGainParamId) == 1.0)
            ++restored;
        else bad += std::string(" ") + d->name + "(state)";
    }
    const int n = static_cast<int>(f->get_plugin_count(f));
    check(created == n, "all " + std::to_string(n) + " create and initialise");
    check(described == n, "all describe their parameters, value texts and stereo ports");
    check(allocs == 0, "none allocates while processing: " + std::to_string(allocs) + " allocation(s)");
    check(finite == n, "all produce finite audio");
    check(restored == n, "all restore their state exactly, auto gain included");
    if (!bad.empty()) std::printf("        problems:%s\n", bad.c_str());
}

void testParameterChangesLandOnTheirSample() {
    section("ADR-0166 -- a parameter change lands on the sample it is stamped with");

    const std::string id = idOf("PurestGain");
    const Stereo in = source(kBlock);
    // Airwindows seeds each instance's output dither from rand() when it is
    // constructed; the same seed makes two instances comparable bit for bit.
    // A: one block, the change stamped at 256.
    std::srand(1);
    Instance a(id.c_str());
    Events ea;
    ea.add(256, 0, 0.2);
    const Stereo outA = a.run(in, &ea, kBlock);
    // B: the same audio as two halves, the change at the start of the second.
    std::srand(1);
    Instance b(id.c_str());
    Stereo first(256), second(256);
    for (std::size_t i = 0; i < 256; ++i) {
        first.l[i] = in.l[i];
        first.r[i] = in.r[i];
        second.l[i] = in.l[256 + i];
        second.r[i] = in.r[256 + i];
    }
    const Stereo b1 = b.run(first);
    Events eb;
    eb.add(0, 0, 0.2);
    const Stereo b2 = b.run(second, &eb);
    bool same = true;
    for (std::size_t i = 0; i < 256; ++i)
        if (outA.l[i] != b1.l[i] || outA.l[256 + i] != b2.l[i] || outA.r[256 + i] != b2.r[i]) same = false;
    check(same, "stamped at 256 = the block split at 256, bit for bit");
    // C: the change at 0 instead, to show the stamp mattered.
    std::srand(1);
    Instance c(id.c_str());
    Events ec;
    ec.add(0, 0, 0.2);
    const Stereo outC = c.run(in, &ec, kBlock);
    bool differs = false;
    for (std::size_t i = 0; i < 256; ++i)
        if (outC.l[i] != outA.l[i]) differs = true;
    check(differs, "and differs from the change stamped at 0");
}

void testAutoGainIsOnWhereItIsMissing() {
    section("ADR-0166 -- auto gain: on for a tone stage with no output control, off elsewhere");

    auto defaultOf = [](const char* name) {
        Instance fx(idOf(name).c_str());
        const uint32_t np = fx.params->count(fx.p);
        clap_param_info_t info{};
        fx.params->get_info(fx.p, np - 1, &info);
        return info.default_value;
    };
    check(defaultOf("Tube2") == 1.0, "Tube2 (Input, Tube; no output) starts with auto gain on");
    check(defaultOf("Density3") == 0.0, "Density3 has an Output of its own: off");
    check(defaultOf("kCathedral5") == 0.0, "a reverb: off");
    check(defaultOf("PurestGain") == 0.0, "a gain utility, whose job is level: off");

    // Tube2 driven hard: louder with auto gain off, as loud as its input with it on.
    const Stereo in = source(static_cast<std::size_t>(5 * kFs));
    const std::size_t n = in.l.size();
    const double inDb = rmsDb(in.l, n - 48000, n);
    Instance on(idOf("Tube2").c_str());
    on.set(0, 1.0);
    on.set(1, 1.0);
    const Stereo withAg = on.run(in);
    Instance off(idOf("Tube2").c_str());
    off.set(0, 1.0);
    off.set(1, 1.0);
    off.set(kAutoGainParamId, 0.0);
    const Stereo without = off.run(in);
    const double offDb = rmsDb(without.l, n - 48000, n) - inDb;
    const double onDb = rmsDb(withAg.l, n - 48000, n) - inDb;
    std::printf("        Tube2 driven: %+.2f dB without auto gain, %+.2f dB with it\n", offDb, onDb);
    check(std::fabs(offDb) > 2.0, "driven, Tube2 changes the level by more than 2 dB");
    check(std::fabs(onDb) < 1.0, "auto gain brings it back to within 1 dB of its input");

    // State carries the switch, and refuses another effect's state.
    Instance a(idOf("Tube2").c_str());
    a.set(kAutoGainParamId, 0.0);
    const std::string saved = a.save();
    Instance b(idOf("Tube2").c_str());
    check(b.get(kAutoGainParamId) == 1.0 && b.load(saved) && b.get(kAutoGainParamId) == 0.0,
          "auto gain off survives save and load");
    Instance other(idOf("Tube").c_str());
    check(!other.load(saved), "Tube refuses Tube2's state");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_airwindows_tests -- the kept Airwindows as CLAP plug-ins\n\n");
    testTheFactoryListsTheKeptEffects();
    testEveryEffectRunsCleanly();
    testParameterChangesLandOnTheirSample();
    testAutoGainIsOnWhereItIsMissing();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
