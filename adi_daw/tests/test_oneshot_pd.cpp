// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/pd_builtins/oneshot_sample.hpp"
#include "juce/pd_engine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <sstream>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace {
thread_local bool audit = false;
thread_local unsigned allocations = 0, deallocations = 0;
int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", message); }
}
}
#if defined(_MSC_VER) && defined(_DEBUG)
int allocationHook(int kind, void*, std::size_t, int, long, const unsigned char*, int) {
    if (audit) {
        if (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC) ++allocations;
        if (kind == _HOOK_FREE) ++deallocations;
    }
    return 1;
}
#endif
void* operator new(std::size_t n) {
    if (audit) ++allocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (audit && p) ++deallocations; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace {
using namespace adi;
std::unique_ptr<device::PdSampleBuffer> sample(float left, float right) {
    auto raw = std::make_unique<device::PdSampleBuffer>();
    raw->id = 1; raw->channels = 2; raw->frames = 65536; raw->sampleRate = 48000;
    raw->blake3 = "test-source-hash";
    raw->interleaved.resize(2 * 65536);
    for (std::size_t i = 0; i < raw->interleaved.size(); i += 2) {
        raw->interleaved[i] = left; raw->interleaved[i + 1] = right;
    }
    return device::prepareOneShotSample(std::move(raw));
}
engine::Event note(std::uint64_t id, bool on = true) {
    return {0, on ? engine::EventType::NoteOn : engine::EventType::NoteOff, 0, 60, id, 0,
            on ? 1. : 0.};
}
struct Fixture {
    device::LibPdEngine pd{ADI_DEVICE_PATCH_DIR, "OneShot.pd", 2, 2};
    device::PdLatencyReceiver latency;
    device::PdDeclarations declarations;
    std::array<float, 1024> left{}, right{};
    bool ready = false, realtime = true;
    Fixture() {
        pd.addSearchPath(ADI_PD_PATCH_DIR);
        std::string error;
        ready = pd.open(latency, error);
        check(ready && error.empty(), "OneShot compiled external and top-level canvas open");
        if (!ready) { std::printf("%s\n", error.c_str()); return; }
        std::ifstream file(std::string(ADI_DEVICE_PATCH_DIR) + "/OneShot.pd");
        std::stringstream text; text << file.rdbuf();
        declarations = device::parsePdDeclarations(text.str());
        check(declarations.problems.empty() && declarations.params.size() == device::OneShot::Count &&
              declarations.samples.size() == 1, "all forty controls and one sample slot parse");
        device::OneShot native;
        bool equal = declarations.params.size() == device::OneShot::Count;
        for (int i = 0; i < device::OneShot::Count; ++i) {
            const auto* p = native.paramAt(i);
            const auto* d = declarations.param(i + 1);
            equal &= d && d->name == p->name && d->unit == p->unit &&
                     d->min == p->minReal && d->max == p->maxReal && d->def == p->defaultValue.real;
        }
        check(equal, "Pd panel names units ranges and defaults match the recovered native contract");
        pd.prepare(48000, 1024);
        pd.bindSamples(declarations);
        pd.bindParameters(declarations);
        pd.requestLatencyReport();
        check(latency.latencySamples() == 0 && pd.adapterLatencySamples() == 64,
              "native sampler has zero intrinsic delay and main's Pd adapter reports its tick");
        bool routed = true;
        for (const auto& p : declarations.params) routed &= pd.sendParameter(p.id, static_cast<float>(p.def));
        check(routed, "every declared control reaches its compiled-in external");
        check(pd.sendParameter(device::OneShot::Attack + 1, 0) &&
              pd.sendParameter(device::OneShot::Release + 1, 0) &&
              pd.sendParameter(device::OneShot::Volume + 1, 0) &&
              pd.sendParameter(device::OneShot::FadeOut + 1, 0), "oracle controls reach native core");
    }
    void block(const engine::Event* event = nullptr, int frames = 64, int offset = 0) {
        float* output[]{left.data(), right.data()};
        engine::NodeIo io{};
        io.out = output; io.channels = 2; io.frames = frames; io.blockOffset = offset;
        io.events = event ? engine::EventSpan{event, 1} : engine::EventSpan{};
        allocations = 0; deallocations = 0; audit = true;
        pd.process(io);
        audit = false;
        realtime &= allocations == 0 && deallocations == 0;
    }
    bool constant(float l, float r, int frames = 64, int offset = 0) const {
        for (int i = offset; i < offset + frames; ++i) {
            const auto j = static_cast<std::size_t>(i);
            if (!std::isfinite(left[j]) || !std::isfinite(right[j]) ||
                std::abs(left[j] - l) > 1e-7f || std::abs(right[j] - r) > 1e-7f) return false;
        }
        return true;
    }
};
void run() {
    Fixture f;
    if (!f.ready) return;
    auto on = note(1);
    f.block(&on); f.block();
    check(f.constant(0, 0), "missing sample produces silence");
    auto a = sample(.25f, -.125f);
    check(a && a->blake3 == "test-source-hash" && a->frames == 65536 && a->interleaved[1] == -.125f,
          "prepared slot preserves source hash and stereo audio metadata");
    check(f.pd.publishSample(1, std::move(a)), "prepared sample publishes through the existing host slot");
    on = note(2); f.block(&on); f.block();
    check(f.constant(.25f, -.125f), "real Pd MIDI plays the independent stereo amplitude oracle");
    auto off = note(2, false); f.block(&off); f.block();
    check(f.constant(0, 0), "real Pd note-off releases both channels");
    on = note(3); f.block(&on); f.block();
    check(f.pd.publishSample(1, sample(.5f, .125f)), "replacement sample publishes");
    f.pd.collectSamples(); f.block(); f.block();
    check(f.constant(0, 0), "replacement kills previous voices after the reported adapter tail");
    on = note(4); f.block(&on); f.block();
    check(f.constant(.5f, .125f), "new note uses replacement sample");
    check(f.pd.publishSample(1, device::clearOneShotSample(1)), "empty snapshot publishes a clear");
    f.block(); f.block(); f.pd.collectSamples();
    check(f.constant(0, 0), "clear retires old sample and silences held voices");
    check(f.pd.publishSample(1, sample(.25f, -.125f)), "sample recovers after clear");
    on = note(5); f.block(&on); f.block();
    check(f.constant(.25f, -.125f), "note on publication boundary reads new immutable data");
    check(f.pd.publishSample(1, sample(.5f, 0)) && f.pd.publishSample(1, sample(.375f, .0625f)),
          "multiple off-thread publications before a segment are accepted");
    on = note(6); f.block(&on); f.block(); f.pd.collectSamples();
    check(f.constant(.375f, .0625f), "latest publication wins without borrowing a retired sample");
    auto raw = std::make_unique<device::PdSampleBuffer>();
    raw->id = 1; raw->channels = 1; raw->frames = 64; raw->sampleRate = 48000;
    raw->interleaved.assign(64, 1.f);
    check(f.pd.publishSample(1, std::move(raw)), "generic unprepared sample can reach declared slot");
    f.block(); f.block();
    check(f.constant(0, 0), "unprepared replacement resets instead of analyzing or retaining old voices");
    check(f.pd.publishSample(1, sample(.25f, -.125f)), "prepared data restores playback");
    on = note(7); f.block(&on); f.block();
    bool chunks = true;
    for (int size : {1, 7, 63, 65, 127, 255}) {
        f.left.fill(9); f.right.fill(9);
        f.block(nullptr, size, 17);
        chunks &= f.constant(.25f, -.125f, size, 17) && f.left[16] == 9 &&
                  f.right[16] == 9 && f.left[static_cast<std::size_t>(17 + size)] == 9 &&
                  f.right[static_cast<std::size_t>(17 + size)] == 9;
    }
    check(chunks, "awkward segments preserve output offsets and surrounding guards");
    // Current LibPdEngine does not notify an already-running external on a
    // same-rate prepare, even though it resets its own MIDI router. Explicit
    // reset is a supported device operation; automatic host integration is not
    // claimed here. The native-core prepare/reset is tested independently.
    f.pd.prepare(48000, 1024);
    check(f.pd.sendFloat("oneshot-reset", 0), "explicit reset receiver is bound");
    f.block(); f.block();
    check(f.constant(0, 0), "explicit bounded reset clears held notes after host reprepare");
    Fixture other;
    if (other.ready) {
        check(other.pd.publishSample(1, sample(-.375f, .125f)), "second instance publishes independently");
        on = note(20); f.block(&on); f.block(); other.block(&on); other.block();
        check(f.constant(.25f, -.125f) && other.constant(-.375f, .125f),
              "two Pd instances keep sample slots and MIDI identities separate");
        check(other.realtime && other.pd.consoleLines().empty(), "second instance callback and console remain clean");
    }
    std::atomic<bool> running{true}, bad{false};
    std::atomic<int> rendered{0};
    std::thread worker([&] {
        int n = 0;
        while (running.load() && n < 20000) {
            auto trigger = note(static_cast<std::uint64_t>(100 + n));
            f.block(&trigger);
            if (!f.realtime) bad = true;
            for (int i = 0; i < 64; ++i)
                if (!std::isfinite(f.left[static_cast<std::size_t>(i)]) ||
                    !std::isfinite(f.right[static_cast<std::size_t>(i)])) bad = true;
            ++n; rendered = n;
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rendered.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    bool published = true;
    for (int i = 0; i < 16; ++i) {
        published &= f.pd.publishSample(1, i % 3 == 0 ? device::clearOneShotSample(1) :
                                                        sample(i % 2 ? .25f : -.5f, .125f));
        f.pd.collectSamples();
    }
    running = false; worker.join();
    check(published && rendered.load() > 0 && !bad.load(),
          "concurrent publish clear and retire never allocate free or read invalid data on audio");
    check(f.realtime, "real-Pd callback performs no audited allocation or deallocation");
    const auto console = f.pd.consoleLines();
    for (const auto& line : console) std::printf("PD %s\n", line.c_str());
    check(console.empty(), "Pd console is clean through controls MIDI and sample replacements");
}
}
int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(allocationHook);
#endif
    run();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
