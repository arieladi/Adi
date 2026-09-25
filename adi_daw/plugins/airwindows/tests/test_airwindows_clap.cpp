// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI Airwindows' suites, driven the way a host drives them (ADR-0166,
// ADR-0171): every suite and every algorithm in it, a parameter list that
// never changes and never asks for a rescan, the 5 ms crossfade checked sample
// by sample, auto gain on whatever plays, and state.
//
// Its own binary: it replaces global operator new to observe that process()
// allocates nothing -- through every algorithm and every switch.

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

using namespace adi::airwindows;

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

// --- a host, as small as CLAP allows, that counts rescans ----------------------

int g_rescanAll = 0;      // anything that asks the host to rebuild the list
int g_rescanValues = 0;

void hostRescan(const clap_host_t*, clap_param_rescan_flags flags) {
    if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_TEXT)) ++g_rescanAll;
    if (flags & CLAP_PARAM_RESCAN_VALUES) ++g_rescanValues;
}
void hostClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void hostRequest(const clap_host_t*) {}
const clap_host_params_t g_hostParams{hostRescan, hostClear, hostRequest};

const void* hostExtension(const clap_host_t*, const char* id) {
    return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &g_hostParams : nullptr;
}

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
    void clear() { list.clear(); }
};

bool noPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t g_noOut{nullptr, noPush};

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t n = 0) : l(n), r(n) {}
    Stereo slice(std::size_t from, std::size_t n) const {
        Stereo s(n);
        std::copy(l.begin() + static_cast<long>(from), l.begin() + static_cast<long>(from + n), s.l.begin());
        std::copy(r.begin() + static_cast<long>(from), r.begin() + static_cast<long>(from + n), s.r.begin());
        return s;
    }
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
    return static_cast<const clap_plugin_factory_t*>(entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
}

std::string suiteId(const char* key) { return std::string("com.adi.airwindows.") + key; }

/// One running suite.
struct Instance {
    const clap_plugin_t* p = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    Events none_;
    Events pending_;       // delivered with the next block

    explicit Instance(const std::string& id) {
        p = factory()->create_plugin(factory(), &g_host, id.c_str());
        if (p == nullptr || !p->init(p)) {
            p = nullptr;
            return;
        }
        params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
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

    /// Runs `in` into `out` (both sized), in blocks; pending events go with the
    /// first block. Allocates nothing.
    void runInto(const Stereo& in, Stereo& out, uint32_t block = kBlock) {
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
            proc.in_events = s == 0 ? &pending_.in : &none_.in;
            proc.out_events = &g_noOut;
            p->process(p, &proc);
        }
        pending_.clear();
    }
    Stereo run(const Stereo& in, uint32_t block = kBlock) {
        Stereo out(in.l.size());
        runInto(in, out, block);
        return out;
    }
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
    int algorithmCount() const {
        clap_param_info_t info{};
        params->get_info(p, 0, &info);
        return static_cast<int>(info.max_value) + 1;
    }
    int algorithmNamed(const char* name) const {
        double v = -1.0;
        return params->text_to_value(p, kAlgorithmParamId, name, &v) ? static_cast<int>(v) : -1;
    }
    /// Every parameter's id, name and module, in order.
    std::vector<std::string> list() const {
        std::vector<std::string> out;
        for (uint32_t i = 0; i < params->count(p); ++i) {
            clap_param_info_t info{};
            params->get_info(p, i, &info);
            out.push_back(std::to_string(info.id) + "|" + info.module + "|" + info.name);
        }
        return out;
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

// ADR-0173: the Consoles suite is gone; its algorithms are the mixer's group summing.
const char* const kSuites[] = {"distortion", "tape", "ampsims", "reverb", "lofimod",
                               "noisedyn", "secret", "delay", "stereo", "sub"};
const int kSuiteSizes[] = {47, 6, 15, 17, 22, 13, 6, 4, 3, 2};
constexpr int kSuiteCount = 10;

// --- the tests -----------------------------------------------------------------

void testTheFactoryHoldsTenSuites() {
    section("ADR-0171, ADR-0173 -- ten suites, each one CLAP plug-in holding its group's algorithms");

    check(entryInit(""), "the entry initialises");
    const clap_plugin_factory_t* f = factory();
    check(f->get_plugin_count(f) == kSuiteCount, "ten suites in the binary that carries them all");
    std::set<std::string> ids;
    bool named = true;
    for (uint32_t i = 0; i < f->get_plugin_count(f); ++i) {
        const clap_plugin_descriptor_t* d = f->get_plugin_descriptor(f, i);
        ids.insert(d->id);
        named = named && std::string(d->name).rfind("ADI Airwindows - ", 0) == 0;
    }
    check(ids.size() == kSuiteCount, "every suite has its own id");
    check(named, "every suite is called \"ADI Airwindows - <group>\"");
    int total = 0;
    for (int s = 0; s < kSuiteCount; ++s) {
        Instance fx(suiteId(kSuites[s]));
        const int n = fx.p != nullptr ? fx.algorithmCount() : -1;
        total += n;
        check(n == kSuiteSizes[s], std::string(kSuites[s]) + " holds " + std::to_string(kSuiteSizes[s]) +
                                       " algorithms: " + std::to_string(n));
    }
    // Melt and StarChild2 are Ambience, which is in no other suite; the other
    // four secret weapons are also in their category's suite.
    check(total == 135, "131 algorithms, four of them in two suites: 135 slots, got " + std::to_string(total));
    check(factory()->create_plugin(factory(), &g_host, "com.adi.airwindows.consoles") == nullptr,
          "and no Consoles suite: the consoles are the mixer's (ADR-0173)");
    Instance secret(suiteId("secret"));
    for (const char* name : {"Melt", "TapeDust", "GrooveWear", "StarChild2", "Vibrato", "NonlinearSpace"})
        check(secret.algorithmNamed(name) >= 0, std::string("Secret Weapons holds ") + name);
    Instance delay(suiteId("delay"));
    check(delay.algorithmNamed("Melt") < 0, "Delay does not: it is the four the director named");
}

void testEveryAlgorithmPlaysAndTheListNeverMoves() {
    section("ADR-0171 -- every algorithm plays through a switch, allocating nothing, and the list never moves");

    const Stereo in = source(kBlock * 12);
    long allocs = 0;
    int played = 0, finite = 0, stable = 0, restored = 0, suites = 0;
    std::string bad;
    for (const char* key : kSuites) {
        Instance fx(suiteId(key));
        if (fx.p == nullptr) {
            bad += std::string(" ") + key + "(create)";
            continue;
        }
        ++suites;
        const std::vector<std::string> before = fx.list();
        std::set<std::string> idset;
        for (const std::string& s : before) idset.insert(s.substr(0, s.find('|')));
        check(idset.size() == before.size(), std::string(key) + ": every parameter id is unique");

        const int n = fx.algorithmCount();
        const int rescansBefore = g_rescanAll;
        Stereo out(in.l.size());
        for (int a = 0; a < n; ++a) {
            fx.pending_.add(100, kAlgorithmParamId, static_cast<double>(a));   // mid-block
            g_allocs.store(0);
            g_counting.store(true);
            fx.runInto(in, out);
            g_counting.store(false);
            allocs += g_allocs.load();
            ++played;
            bool ok = true;
            for (std::size_t i = 0; i < out.l.size(); ++i)
                ok = ok && std::isfinite(out.l[i]) && std::isfinite(out.r[i]);
            if (ok) ++finite;
            else bad += std::string(" ") + key + "#" + std::to_string(a) + "(nan)";
        }
        if (fx.list() == before && g_rescanAll == rescansBefore) ++stable;
        else bad += std::string(" ") + key + "(list moved)";

        // Every parameter off its default, the last algorithm chosen, auto gain
        // off; saved, loaded into a fresh suite, saved again.
        for (uint32_t i = 2; i < fx.params->count(fx.p); ++i) {
            clap_param_info_t info{};
            fx.params->get_info(fx.p, i, &info);
            fx.set(info.id, 0.25 + 0.5 * ((i * 37) % 11) / 11.0);
        }
        fx.set(kAlgorithmParamId, static_cast<double>(n - 1));
        fx.set(kAutoGainParamId, 0.0);
        const std::string saved = fx.save();
        const int valueRescans = g_rescanValues;
        Instance again(suiteId(key));
        if (again.load(saved) && again.save() == saved && again.get(kAlgorithmParamId) == n - 1 &&
            g_rescanValues == valueRescans + 1)
            ++restored;
        else bad += std::string(" ") + key + "(state)";
    }
    check(suites == kSuiteCount, "all ten suites create and initialise");
    check(played == 135 && finite == 135, "all 135 algorithm slots play, switched to mid-block, finite: " +
                                              std::to_string(finite));
    check(allocs == 0, "no switch and no algorithm allocates: " + std::to_string(allocs) + " allocation(s)");
    check(stable == kSuiteCount, "no suite's parameter list changed, and none asked for a rescan");
    check(restored == kSuiteCount, "every suite restores its state exactly, asking only for a values rescan");
    Instance other(suiteId("tape"));
    Instance dist(suiteId("distortion"));
    check(!other.load(dist.save()), "Tape refuses Distortion's state");
    if (!bad.empty()) std::printf("        problems:%s\n", bad.c_str());
}

void testTheSwitchIsAFiveMillisecondCrossfade() {
    section("ADR-0171 -- a switch is a 5 ms linear crossfade, sample for sample");

    // Airwindows seeds each instance's output dither from rand() when it is
    // constructed, and a suite constructs its algorithms in order: the same
    // seed makes two suites' instances identical.
    const std::string id = suiteId("distortion");
    const Stereo in = source(kBlock * 4);
    const int from = 0;
    std::srand(1);
    Instance probe(id);
    const int to = probe.algorithmNamed("Density3");
    const uint32_t at = kBlock * 2;
    const int fade = static_cast<int>(std::lround(kCrossfadeSeconds * kFs));

    // A: algorithm `from`, then a switch to `to` at `at`.
    std::srand(1);
    Instance a(id);
    a.set(kAutoGainParamId, 0.0);
    a.set(kAlgorithmParamId, from);
    const Stereo head = in.slice(0, at), tail = in.slice(at, in.l.size() - at);
    Stereo aHead(head.l.size()), aTail(tail.l.size());
    a.runInto(head, aHead);
    a.pending_.add(0, kAlgorithmParamId, to);
    a.runInto(tail, aTail);
    // B: `from` alone, called as it is inside A: the head in blocks, then the
    // fade's 240 frames in one call. (Some algorithms work per call, so a
    // reference cut differently would differ by that, not by the fade.)
    std::srand(1);
    Instance b(id);
    b.set(kAutoGainParamId, 0.0);
    b.set(kAlgorithmParamId, from);
    const Stereo bHead = b.run(head);
    const Stereo bFade = b.run(tail.slice(0, static_cast<std::size_t>(fade)), static_cast<uint32_t>(fade));
    // D: `to` alone, fresh, fed from the switch on -- which is what the idle
    // instance inside A is when it is chosen.
    std::srand(1);
    Instance d(id);
    d.set(kAutoGainParamId, 0.0);
    d.set(kAlgorithmParamId, to);
    const Stereo outD = d.run(tail);

    // Checked in two parts, so a failure says which side is wrong.
    double inFade = 0.0, after = 0.0, incomingInFade = 0.0;
    for (std::size_t i = 0; i < aTail.l.size(); ++i) {
        if (static_cast<int>(i) < fade) {
            const double g = static_cast<double>(i + 1) / static_cast<double>(fade + 1);
            const double want = bFade.l[i] * (1.0 - g) + outD.l[i] * g;
            inFade = std::fmax(inFade, std::fabs(aTail.l[i] - want));
            incomingInFade = std::fmax(incomingInFade, std::fabs(aTail.l[i] - outD.l[i]));
        } else {
            after = std::fmax(after, std::fabs(aTail.l[i] - outD.l[i]));
        }
    }
    check(to > 0, "Density3 is in the Distortion suite, and not its first algorithm");
    check(fade == 240, "5 ms at 48 kHz is 240 samples");
    check(after < 1e-6, "after 240 samples, the incoming algorithm alone: worst " + std::to_string(after));
    check(inFade < 1e-6, "during them, outgoing x (1 - g) + incoming x g: worst " + std::to_string(inFade) +
                             " (against the incoming alone: " + std::to_string(incomingInFade) + ")");
    bool before = true;
    for (std::size_t i = 0; i < head.l.size(); ++i) before = before && aHead.l[i] == bHead.l[i];
    check(before, "and before the switch, the outgoing algorithm untouched");
}

void testParameterChangesLandOnTheirSample() {
    section("ADR-0171 -- an algorithm's parameter change lands on the sample it is stamped with");

    const std::string id = suiteId("distortion");
    const Stereo in = source(kBlock);
    std::srand(2);
    Instance probe(id);
    const int algo = probe.algorithmNamed("Density3");
    const clap_id param = kAlgoParamBase + static_cast<clap_id>(algo) * kAlgoParamStride;   // its first
    auto make = [&](Instance& fx) {
        fx.set(kAlgorithmParamId, algo);
        fx.set(kAutoGainParamId, 0.0);
    };
    std::srand(2);
    Instance a(id);
    make(a);
    a.pending_.add(256, param, 0.9);
    const Stereo outA = a.run(in);
    std::srand(2);
    Instance b(id);
    make(b);
    const Stereo b1 = b.run(in.slice(0, 256));
    b.pending_.add(0, param, 0.9);
    const Stereo b2 = b.run(in.slice(256, 256));
    bool same = true;
    for (std::size_t i = 0; i < 256; ++i)
        same = same && outA.l[i] == b1.l[i] && outA.l[256 + i] == b2.l[i] && outA.r[256 + i] == b2.r[i];
    check(same, "stamped at 256 = the block split at 256, bit for bit");
}

void testAutoGainFollowsWhatPlays() {
    section("ADR-0171 -- auto gain levels whatever algorithm plays");

    const Stereo in = source(static_cast<std::size_t>(5 * kFs));
    const std::size_t n = in.l.size();
    const double inDb = rmsDb(in.l, n - 48000, n);
    auto driven = [&](bool autoGain) {
        Instance fx(suiteId("distortion"));
        const int tube = fx.algorithmNamed("Tube2");
        const clap_id base = kAlgoParamBase + static_cast<clap_id>(tube) * kAlgoParamStride;
        fx.set(kAlgorithmParamId, tube);
        fx.set(base + 0, 1.0);   // Input
        fx.set(base + 1, 1.0);   // Tube
        fx.set(kAutoGainParamId, autoGain ? 1.0 : 0.0);
        const Stereo out = fx.run(in);
        return rmsDb(out.l, n - 48000, n) - inDb;
    };
    const double off = driven(false), on = driven(true);
    std::printf("        Tube2 driven in the Distortion suite: %+.2f dB without auto gain, %+.2f dB with it\n",
                off, on);
    check(std::fabs(off) > 2.0, "driven, Tube2 changes the level by more than 2 dB");
    check(std::fabs(on) < 1.0, "auto gain brings it back to within 1 dB of its input");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_airwindows_tests -- the Airwindows suites as CLAP plug-ins\n\n");
    testTheFactoryHoldsTenSuites();
    testEveryAlgorithmPlaysAndTheListNeverMoves();
    testTheSwitchIsAFiveMillisecondCrossfade();
    testParameterChangesLandOnTheirSample();
    testAutoGainFollowsWhatPlays();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
