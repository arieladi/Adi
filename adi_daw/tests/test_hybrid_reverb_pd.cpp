// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/wav_file.hpp"
#include "adi/pd_builtins/hybrid_reverb_sample.hpp"
#include "juce/pd_engine.hpp"
#include "temp_directory.hpp"
#include <atomic>
#include <cmath>
#include <complex>
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
thread_local unsigned allocations = 0;
int checks = 0, failures = 0;
void check(bool b, const char *s) {
    ++checks;
    if (!b) {
        ++failures;
        std::printf("FAIL %s\n", s);
    }
}
} // namespace
#if defined(_MSC_VER) && defined(_DEBUG)
int allocationHook(int kind, void *, std::size_t, int, long, const unsigned char *, int) {
    if (audit && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC))
        ++allocations;
    return 1;
}
#endif
void *operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
using namespace adi;
std::unique_ptr<device::PdSampleBuffer> impulse(float gain = .5f) {
    auto s = std::make_unique<device::PdSampleBuffer>();
    s->id = 1;
    s->channels = 1;
    s->sampleRate = 48000;
    s->frames = 301;
    s->interleaved.assign(301, 0);
    s->interleaved[0] = gain;
    s->interleaved[300] = .25f;
    return device::prepareHybridReverbSample(std::move(s));
}
double bin(const std::vector<float> &audio, int index) {
    std::complex<double> sum{};
    for (std::size_t i = 0; i < audio.size(); ++i)
        sum += static_cast<double>(audio[i]) *
               std::polar(1., -6.283185307179586 * index * static_cast<double>(i) /
                                  static_cast<double>(audio.size()));
    return 2 * std::abs(sum) / static_cast<double>(audio.size());
}
int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(allocationHook);
#endif
    test::TempDirectory dir("hybrid", "render");
    device::LibPdEngine engine(ADI_HYBRID_PATCH_DIR, "Hybrid Reverb.pd", 2, 2);
    engine.addSearchPath(ADI_PD_PATCH_DIR);
    device::PdLatencyReceiver latency;
    std::string error;
    check(engine.open(latency, error), "Hybrid Reverb top-level patch opens");
    if (!error.empty())
        std::printf("%s\n", error.c_str());
    std::ifstream file(std::string(ADI_HYBRID_PATCH_DIR) + "/Hybrid Reverb.pd");
    std::stringstream text;
    text << file.rdbuf();
    auto declarations = device::parsePdDeclarations(text.str());
    check(declarations.problems.empty() && declarations.samples.size() == 1,
          "parameter and sample declarations parse");
    engine.prepare(48000, 4096);
    engine.bindParameters(declarations);
    engine.bindSamples(declarations);
    engine.requestLatencyReport();
    check(latency.latencySamples() == 256,
          "intrinsic FFT latency reported separately from Pd adapter");
    const auto path=dir.path()/"impulse.wav";
    std::array<float,301> ir{};ir[0]=.5f;ir[300]=.25f;
    {audio::WavWriter writer(path,48000,1,audio::WavFormat::Float32);writer.write(ir.data(),301);writer.close();}
    auto decoded=device::pdDecodeSample(1,path,path,error);
    check(decoded && error.empty(),"host decodes impulse file off audio");
    check(engine.publishSample(1,device::prepareHybridReverbSample(std::move(decoded))),"decoded IR publishes through adi.sample host slot");
    engine.sendParameter(1, 3);
    engine.sendParameter(14, 100);
    std::array<float, 64> l{}, r{}, ol{}, orr{};
    const float *in[]{l.data(), r.data()};
    float *out[]{ol.data(), orr.data()};
    engine::NodeIo io{};
    io.in = in;
    io.out = out;
    io.channels = 2;
    io.frames = 64;
    std::size_t at = 0;
    auto block = [&] {
        for (std::size_t i = 0; i < 64; ++i) {
            l[i] = r[i] = static_cast<float>(
                .4 * std::sin(6.283185307179586 * 16 * static_cast<double>(at++) / 1024));
        }
        engine.process(io);
    };
    for (int i = 0; i < 150; ++i)
        block();
    std::vector<float> audio(4096);
    allocations = 0;
    audit = true;
    for (std::size_t i = 0; i < audio.size(); i += 64) {
        block();
        std::copy(ol.begin(), ol.end(), audio.begin() + static_cast<std::ptrdiff_t>(i));
    }
    audit = false;
    check(allocations == 0, "LibPdEngine convolution plus Surge allocates nothing");
    check(bin(audio, 733) < 1e-7, "far bin floor before trusting peak");
    const double expected =
        .4 * std::abs(.5 + .25 * std::polar(1., -6.283185307179586 * 16 * 300 / 1024));
    check(std::abs(bin(audio, 64) - expected) < 1e-6,
          "actual Pd wet output matches IR response, not bypass or silence");
    audio::WavWriter writer(dir.path() / "hybrid-reverb.wav", 48000, 1, audio::WavFormat::Float32);
    writer.write(audio.data(),static_cast<std::uint32_t>(audio.size()));
    writer.close();
    audio::WavReader reader(dir.path()/"hybrid-reverb.wav");std::vector<float> recovered(audio.size());
    check(reader.read(recovered.data(),static_cast<std::uint32_t>(recovered.size()))==recovered.size() && recovered==audio,"float WAV round-trip preserves actual samples");
    std::atomic<bool> running{true}, ready{false}, bad{false};
    std::atomic<int> rendered{0};
    std::thread worker([&] {
        allocations = 0;
        ready = true;
        while (running.load()) {
            audit = true;
            block();
            audit = false;
            if (allocations)
                bad = true;
            for (float v : ol)
                if (!std::isfinite(v))
                    bad = true;
            ++rendered;
        }
    });
    while (!ready.load())
        std::this_thread::yield();
    bool published = true;
    for (int i = 0; i < 24; ++i) {
        published &= engine.publishSample(1, impulse(i % 2 ? .5f : .25f));
        engine.collectSamples();
    }
    running = false;
    worker.join();
    check(published && rendered > 0 && !bad,
          "IR replacement while rendering is safe and allocation-free");
    for (const auto &line : engine.consoleLines())
        std::printf("PD: %s\n", line.c_str());
    check(engine.consoleLines().empty(),
          "console hook clean through load, controls and IR replacement");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
