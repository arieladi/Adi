// SPDX-License-Identifier: GPL-3.0-or-later
//
// The format-agnostic device contract: src/juce/device_model.hpp.
//
// Not one line of JUCE and not one line of VST3, which is the property under
// test as much as anything else here. If hosting a plugin needed a type from a
// plugin SDK to be visible at this level, then Pure Data (ADR-0035), CLAP
// (ADR-0052) and a remote AudioGridder device (ADR-0053) would each need an
// exception, and hybrid tracks, modulation and delay compensation would get
// implemented twice.
//
// The three declarations and their deliberately inconsistent defaults are the
// centre of it:
//
//     tailSamples()     kInfiniteTail   never suspend
//     latencySamples()  0               never shift
//     alwaysProcess()   false
//
// Both defaults are the answer that cannot corrupt, and they point opposite
// ways because the failures are not symmetric: a missed tail processes a node
// more often than it needed, a missed latency MOVES AUDIO THAT WAS ALIGNED.

#include "juce/device_model.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace adi;
using adi::device::DeviceIdentity;
using adi::device::DeviceInstance;
using adi::device::DeviceNode;
using adi::device::MissingDevice;
using adi::device::ParamDescriptor;
using adi::device::ParamDomain;
using adi::device::ParamValue;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void section(const char* s) { std::printf("[%s]\n", s); }

/// A device that declares whatever the test needs it to.
class FakeDevice final : public DeviceInstance {
public:
    explicit FakeDevice(DeviceIdentity id) : id_(std::move(id)) {}

    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return true; }

    void prepare(double, std::int32_t) override { ++prepares_; }
    void release() override { ++releases_; }

    void process(const engine::NodeIo& io) noexcept override {
        ++calls_;
        // Writes something distinguishable from both silence and pass-through,
        // so "the node ran" and "the node was bypassed" cannot be confused.
        if (io.out == nullptr) return;
        for (std::int32_t c = 0; c < io.channels; ++c)
            for (std::int32_t i = 0; i < io.frames; ++i)
                io.out[c][i] = 0.5f;
    }

    [[nodiscard]] std::int64_t tailSamples() const noexcept override { return tail_; }
    [[nodiscard]] std::int32_t latencySamples() const noexcept override { return lat_; }

    [[nodiscard]] engine::EventFlow eventFlow() const noexcept override { return flow_; }

    void setTail(std::int64_t t) { tail_ = t; }
    void setLatency(std::int32_t l) { lat_ = l; }
    void setFlow(engine::EventFlow f) { flow_ = f; }

    [[nodiscard]] int prepares() const { return prepares_; }
    [[nodiscard]] int releases() const { return releases_; }
    [[nodiscard]] int calls() const { return calls_; }

private:
    engine::EventFlow flow_ = engine::EventFlow::Through;
    DeviceIdentity id_;
    std::int64_t tail_ = 0;
    std::int32_t lat_ = 0;
    int prepares_ = 0, releases_ = 0, calls_ = 0;
};

/// A device that overrides NOTHING, to read the base class's own answers.
class SilentDevice final : public DeviceInstance {
public:
    [[nodiscard]] const DeviceIdentity& identity() const noexcept override { return id_; }
    [[nodiscard]] bool loaded() const noexcept override { return true; }
    void process(const engine::NodeIo&) noexcept override {}
private:
    DeviceIdentity id_;
};

struct Buffers {
    std::vector<std::vector<float>> chans;
    std::vector<float*> ptrs;
    std::vector<const float*> cptrs;

    Buffers(int n, int frames, float fill) {
        chans.assign(static_cast<std::size_t>(n),
                     std::vector<float>(static_cast<std::size_t>(frames), fill));
        for (auto& c : chans) { ptrs.push_back(c.data()); cptrs.push_back(c.data()); }
    }
    float* const* out() { return ptrs.data(); }
    const float* const* in() { return cptrs.data(); }
    bool all(float v, int frames) const {
        for (const auto& c : chans)
            for (int i = 0; i < frames; ++i)
                if (c[static_cast<std::size_t>(i)] != v) return false;
        return true;
    }
};

engine::NodeIo makeIo(Buffers& out, Buffers* in, int chans, int frames) {
    engine::NodeIo io;
    io.out = out.out();
    io.in = (in != nullptr) ? in->in() : nullptr;
    io.channels = chans;
    io.frames = frames;
    io.sampleRate = 48000.0;
    return io;
}

// --- the defaults ------------------------------------------------------------

void testTheDefaultsPointOppositeWays() {
    section("the three declarations, and why two defaults disagree");
    SilentDevice d;

    check(d.tailSamples() == engine::kInfiniteTail,
          "a device that declares nothing is NEVER suspended");
    check(d.latencySamples() == 0,
          "and declares NO latency -- the opposite default, deliberately");

    // The same pair on the Node side, which is what the scheduler reads.
    DeviceNode n(d);
    check(n.tailSamples() == engine::kInfiniteTail, "the node forwards the tail");
    check(n.latencySamples() == 0, "and the latency");
    check(!n.alwaysProcess(), "always_process defaults off");

    // ADR-0055's cost, recorded because it cost four test fixtures: a SOURCE
    // declaring tail 0 is suspended on its first block, because a node with no
    // inputs has vacuously silent input, no events and an expired tail. So a
    // generator that produces without events must declare kInfiniteTail, and
    // the base class's default is already the right answer for it.
    check(engine::kInfiniteTail == INT64_MAX,
          "kInfiniteTail matches VST3's getTailSamples convention, so a plugin "
          "reporting it maps straight through with no translation");
}

void testForwardingAndOverrides() {
    section("the node forwards what the device says, except where the project overrides");
    FakeDevice d({});
    d.setTail(12345);
    d.setLatency(2048);
    DeviceNode n(d);

    check(n.tailSamples() == 12345, "the declared tail is forwarded verbatim");
    check(n.latencySamples() == 2048, "and the declared latency");

    // ADR-0043's escape hatch. `devices.always_process` is set by somebody who
    // has heard their reverb cut off, so it has to WIN over a report of zero
    // rather than be combined with it.
    n.setAlwaysProcess(true);
    check(n.tailSamples() == engine::kInfiniteTail,
          "always_process forces an infinite tail whatever the device claims");
    check(n.alwaysProcess(), "and the node says so, so the scheduler can too");

    // But it must NOT touch latency. Forcing a device to keep processing says
    // nothing about how far it shifts its output, and reporting an infinite
    // latency here would be nonsense the compensator would act on.
    check(n.latencySamples() == 2048,
          "always_process does NOT change the latency -- they answer different questions");
}

void testBypassReportsNothing() {
    section("a bypassed device declares no tail and no latency");
    FakeDevice d({});
    d.setTail(9999);
    d.setLatency(512);
    DeviceNode n(d);
    n.setBypassed(true);

    check(n.tailSamples() == 0,
          "a bypassed device has nothing to finish -- reporting a tail would hold "
          "a whole chain awake behind a device the user switched off");

    // The half most likely to be got wrong. A bypassed plugin delays nothing,
    // so continuing to report its latency compensates the rest of the graph
    // against a delay that is no longer there -- which moves audio that was
    // aligned, the exact failure ADR-0058's default was chosen to avoid.
    check(n.latencySamples() == 0, "and delays nothing, so it reports no latency");

    n.setBypassed(false);
    check(n.latencySamples() == 512, "un-bypassing restores the report");
}

void testBypassPassesAudioThrough() {
    section("bypass passes audio through; it does not silence");
    FakeDevice d({});
    DeviceNode n(d);
    Buffers in(2, 64, 0.25f);
    Buffers out(2, 64, -1.0f);

    auto io = makeIo(out, &in, 2, 64);
    n.process(io);
    check(out.all(0.5f, 64), "not bypassed: the device wrote its own output");
    check(d.calls() == 1, "and was called");

    n.setBypassed(true);
    Buffers out2(2, 64, -1.0f);
    auto io2 = makeIo(out2, &in, 2, 64);
    n.process(io2);
    check(out2.all(0.25f, 64),
          "bypassed: the input arrives unchanged, NOT silence -- a bypassed "
          "insert is what the user chose, and silence is not recoverable by ear");
    check(d.calls() == 1, "and the device was not called");

    // No input at all. Writing nothing hands the next node whatever was in
    // that buffer, which is the previous block repeated, and it reaches the
    // monitors.
    Buffers out3(2, 64, -1.0f);
    auto io3 = makeIo(out3, nullptr, 2, 64);
    n.process(io3);
    check(out3.all(0.0f, 64), "with no input, a bypassed node writes silence rather than stale memory");
}

void testPassThroughHonoursTheSegmentOffset() {
    section("ADR-0042 -- a pass-through writes where the SEGMENT is, not where the block starts");

    // `NodeIo::in` and `NodeIo::out` are WHOLE-BLOCK pointers and `blockOffset`
    // says where this segment begins inside them. Every node in the graph adds
    // it -- `io.out[c] + io.blockOffset` -- and a node that does not writes
    // every segment on top of the first.
    //
    // This is not a hypothetical split. ADR-0042 splits a block at every
    // distinct event frame, and ADR-0054's MPE+ target is 500 Hz, so a block
    // carrying a controller stream is split many times over. The two things
    // that go through here -- a bypassed insert and, by ADR-0011, EVERY device
    // in a project opened without its plugins -- would then emit the last
    // segment at the block start and stale memory everywhere else.
    //
    // The whole buffer is checked, not just the segment, because writing to
    // the wrong place is only half the defect: the samples that should have
    // been written are the other half.
    constexpr int kBlock = 16;
    constexpr int kOffset = 8;
    constexpr int kSeg = 4;

    FakeDevice d({});
    DeviceNode n(d);
    n.setBypassed(true);

    Buffers in(2, kBlock, 0.0f);
    for (auto& c : in.chans)
        for (int i = 0; i < kBlock; ++i) c[static_cast<std::size_t>(i)] = static_cast<float>(i);

    Buffers out(2, kBlock, -1.0f);
    auto io = makeIo(out, &in, 2, kSeg);
    io.blockOffset = kOffset;
    n.process(io);

    bool placed = true, untouched = true;
    int firstBad = -1;
    for (const auto& c : out.chans)
        for (int i = 0; i < kBlock; ++i) {
            const float got = c[static_cast<std::size_t>(i)];
            const bool inSeg = (i >= kOffset && i < kOffset + kSeg);
            const float want = inSeg ? static_cast<float>(i) : -1.0f;
            if (got != want) {
                if (inSeg) placed = false; else untouched = false;
                if (firstBad < 0) firstBad = i;
            }
        }

    check(placed,
          "a bypassed insert writes its segment at blockOffset" +
              (placed ? std::string()
                      : " -- first wrong sample at " + std::to_string(firstBad)));
    check(untouched,
          "and touches nothing outside it -- writing segment 2 at offset 0 "
          "overwrites segment 1, which is the audible half of this bug" +
              (untouched ? std::string()
                         : " -- first wrong sample at " + std::to_string(firstBad)));

    // Same path, same rule, and this one is ADR-0011: a project opened on a
    // machine without the plugin runs entirely through here.
    MissingDevice m(DeviceIdentity{"vst3", "u", "Gone", "Vendor", "1.0"});
    Buffers out2(2, kBlock, -1.0f);
    auto io2 = makeIo(out2, &in, 2, kSeg);
    io2.blockOffset = kOffset;
    m.process(io2);
    bool missingOk = true;
    for (const auto& c : out2.chans)
        for (int i = 0; i < kBlock; ++i) {
            const bool inSeg = (i >= kOffset && i < kOffset + kSeg);
            if (c[static_cast<std::size_t>(i)] != (inSeg ? static_cast<float>(i) : -1.0f))
                missingOk = false;
        }
    check(missingOk, "and so does a missing plugin, which is the same code path");

    // With no input the segment is SILENCED -- still only the segment.
    Buffers out3(2, kBlock, -1.0f);
    auto io3 = makeIo(out3, nullptr, 2, kSeg);
    io3.blockOffset = kOffset;
    n.process(io3);
    bool zeroedOk = true;
    for (const auto& c : out3.chans)
        for (int i = 0; i < kBlock; ++i) {
            const bool inSeg = (i >= kOffset && i < kOffset + kSeg);
            if (c[static_cast<std::size_t>(i)] != (inSeg ? 0.0f : -1.0f)) zeroedOk = false;
        }
    check(zeroedOk, "with no input, only the segment is zeroed, not the block");
}

void testAnInstrumentConsumesAndBypassDoesNot() {
    section("ADR-0091 -- a device says whether it stops the note stream");

    FakeDevice d({});
    DeviceNode n(d);
    check(n.eventFlow() == engine::EventFlow::Through,
          "a device that says nothing passes notes on -- the harmless default");

    d.setFlow(engine::EventFlow::Consume);
    check(n.eventFlow() == engine::EventFlow::Consume,
          "an instrument's answer reaches the graph through DeviceNode");

    // Bypass is transparency, all the way down: no tail, no latency, and no
    // consumption. A bypassed synth that still swallowed notes would silence
    // whatever it sits in front of, which is not what bypass means.
    n.setBypassed(true);
    check(n.eventFlow() == engine::EventFlow::Through,
          "a BYPASSED instrument consumes nothing -- it is not running");
    n.setBypassed(false);
    check(n.eventFlow() == engine::EventFlow::Consume, "and un-bypassing restores it");

    // A missing plugin is a placeholder (ADR-0011). If it was a synth, the
    // notes reach the effects after it, which ignore them; if it was an
    // effect, they reach the synth after it, which needs them. Through is the
    // only answer that is right in both cases.
    MissingDevice m(DeviceIdentity{"clap", "u", "Gone", "Vendor", "1.0"});
    DeviceNode mn(m);
    check(mn.eventFlow() == engine::EventFlow::Through,
          "a missing plugin passes notes on, because we cannot know what it was");
}

void testPrepareAndReleaseReachTheDevice() {
    section("prepare and release are forwarded");
    FakeDevice d({});
    DeviceNode n(d);
    n.prepare(48000.0, 4096);
    n.prepare(44100.0, 256);
    n.release();
    check(d.prepares() == 2, "both prepares reached the device");
    check(d.releases() == 1, "and the release");
}

// --- the missing plugin ------------------------------------------------------

void testMissingPluginIsStillADevice() {
    section("ADR-0011 / SPEC 7.1: a missing plugin is never dropped");
    DeviceIdentity id;
    id.format = "vst3";
    id.uid = "56535456...";
    id.name = "VintageVerb";
    id.vendor = "Valhalla DSP";
    id.version = "2.1.0";

    MissingDevice m(id);
    check(!m.loaded(), "it knows it did not load");
    check(m.tailSamples() == 0, "a pass-through holds nothing back");
    check(m.latencySamples() == 0, "and delays nothing");

    // "A plugin is missing" is not actionable. This is.
    const std::string d = m.identity().describe();
    check(d.find("VintageVerb") != std::string::npos, "the description names the plugin");
    check(d.find("Valhalla DSP") != std::string::npos, "and its vendor");
    check(d.find("2.1.0") != std::string::npos, "and its version");
    check(d.find("vst3") != std::string::npos, "and its format");

    // Never empty, even with nothing to go on -- a placeholder that says
    // nothing is the failure SPEC 7.1 is about.
    check(!DeviceIdentity{}.describe().empty(),
          "an identity with no fields still describes itself");
}

void testMissingPluginPassesAudio() {
    section("it passes audio through rather than silencing");
    MissingDevice m({});
    DeviceNode n(m);
    Buffers in(2, 32, 0.75f);
    Buffers out(2, 32, -1.0f);
    auto io = makeIo(out, &in, 2, 32);
    n.process(io);
    check(out.all(0.75f, 32),
          "the signal continues past a device that could not load");
}

void testMissingPluginKeepsStateByteForByte() {
    section("its state survives byte for byte, which is the whole of the rule");
    MissingDevice m({});
    const std::vector<std::uint8_t> chunk{0x00, 0xFF, 0x7F, 0x80, 0x00, 0x41, 0x00};
    m.keepState("component", chunk);
    m.keepState("controller", {0x01, 0x02});

    const auto back = m.saveState("component");
    check(back == chunk,
          "identical bytes come back -- including the embedded NULs, which is "
          "where a std::string round trip would have truncated it");
    check(m.saveState("controller").size() == 2, "the second role too");
    check(m.stateRoles().size() == 2, "both roles are reported");
    check(m.saveState("nosuchrole").empty(), "an unknown role yields nothing, not garbage");

    // Overwriting one role must not disturb the other.
    m.keepState("component", {0x09});
    check(m.saveState("component").size() == 1, "a role can be replaced");
    check(m.saveState("controller").size() == 2, "without touching its neighbour");
}

void testMissingPluginAcceptsParameterEdits() {
    section("a missing device still accepts parameter changes");
    MissingDevice m({});
    ParamDescriptor d;
    d.id = "cutoff";
    d.name = "Cutoff";
    d.unit = "Hz";
    d.domain = ParamDomain::Real;
    m.addParam(d, ParamValue::withReal(0.5, 4800.0));

    check(m.paramCount() == 1, "the recorded parameter is there");
    check(m.getParam("cutoff").hasReal, "with its real value");
    check(m.getParam("cutoff").real == 4800.0, "which is what SPEC 6.3.3 prefers a lane store");

    // Not a curiosity: undo must work on a machine where the plugin is absent.
    // The op log is the source of truth (ADR-0038) and the recorded value is
    // what gets written back out on save, so refusing here would make an undo
    // silently do nothing and the next save would flatten it.
    check(m.setParam("cutoff", ParamValue::withReal(0.9, 9000.0)),
          "and it can be changed with the plugin absent");
    check(m.getParam("cutoff").real == 9000.0, "the new value is held");
    check(!m.setParam("nosuchparam", ParamValue::fromNormalized(0.1)),
          "a parameter it never had is refused rather than invented");

    check(m.getParam("nosuchparam").hasReal == false,
          "and reading one back gives a value that says it has no real part");
}

// --- the VST3 edge, stated in the type ---------------------------------------

void testParamValueCarriesBothOrSaysItCannot() {
    section("ADR-0057: a parameter carries both representations, or says it cannot");
    const auto vst3 = ParamValue::fromNormalized(0.42);
    check(vst3.normalized == 0.42, "normalized is always there");
    check(!vst3.hasReal,
          "and a VST3 parameter says it has no real value rather than reporting 0.0 -- "
          "getParamStringByValue returns \"4.80 kHz\", and parsing that back is a guess");
    check(vst3.real == 0.0, "the real field is zero, which is exactly why hasReal must be read");

    const auto clap = ParamValue::withReal(0.5, 120.0);
    check(clap.hasReal && clap.real == 120.0,
          "CLAP, Pd and native devices supply one, and it is what keeps an "
          "automation lane meaningful when the plugin is missing");

    check(std::string(adi::device::toString(ParamDomain::Normalized)) == "normalized",
          "the domain spellings match automation_lanes.value_domain");
    check(std::string(adi::device::toString(ParamDomain::Real)) == "real", "real");
    check(std::string(adi::device::toString(ParamDomain::Enum)) == "enum", "enum");
}

void testBaseClassParamLookup() {
    section("findParam is by id, and a miss is ordinary rather than an error");
    DeviceIdentity id;
    MissingDevice m(id);
    ParamDescriptor a; a.id = "one"; a.defaultValue = ParamValue::fromNormalized(0.1);
    ParamDescriptor b; b.id = "two"; b.defaultValue = ParamValue::fromNormalized(0.2);
    m.addParam(a, a.defaultValue);
    m.addParam(b, b.defaultValue);

    check(m.findParam("two") != nullptr, "found by id");
    check(m.findParam("two")->id == "two", "and it is the right one");
    // A project saved against a newer version of a plugin routinely references
    // parameters this build does not have. That is a fact about versions, not
    // a failure.
    check(m.findParam("three") == nullptr, "a parameter the device lacks yields null");
    check(m.paramAt(-1) == nullptr, "and an out-of-range index does not read off the end");
    check(m.paramAt(99) == nullptr, "in either direction");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_device_model_tests -- the format-agnostic device contract\n\n");
    testTheDefaultsPointOppositeWays();
    testForwardingAndOverrides();
    testBypassReportsNothing();
    testBypassPassesAudioThrough();
    testPassThroughHonoursTheSegmentOffset();
    testAnInstrumentConsumesAndBypassDoesNot();
    testPrepareAndReleaseReachTheDevice();
    testMissingPluginIsStillADevice();
    testMissingPluginPassesAudio();
    testMissingPluginKeepsStateByteForByte();
    testMissingPluginAcceptsParameterEdits();
    testParamValueCarriesBothOrSaysItCannot();
    testBaseClassParamLookup();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
