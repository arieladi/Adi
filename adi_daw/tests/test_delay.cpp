// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/live_delay.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numbers>
#include <vector>
namespace {
bool counting = false;
std::size_t allocations = 0;
} // namespace
void *operator new(std::size_t n) {
    if (counting)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {
using D = adi::dsp::LiveDelay;
int checks = 0, failures = 0;
void check(bool v, const char *s) {
    ++checks;
    if (!v) {
        ++failures;
        std::printf("FAIL %s\n", s);
    }
}
void clean(D &d) {
    // The mechanics these oracles were written for: free time, no filter, the
    // linear Dry/Wet (since DEV-22 the defaults are Live's: synced, filtered,
    // equal-power).
    d.set(D::LeftSync, 0);
    d.set(D::RightSync, 0);
    d.set(D::Filter, 0);
    d.set(D::EqualLoudness, 0);
    d.set(D::Feedback, 0);
    d.set(D::Mix, 100);
    d.set(D::Transition, 2);
    d.prepare(48000);
}
std::vector<float> render(D &d, const std::vector<float> &input, std::size_t block = 64) {
    std::vector<float> l(input.size()), r(input.size());
    for (std::size_t i = 0; i < input.size(); i += block)
        d.process(input.data() + i, input.data() + i, l.data() + i, r.data() + i,
                  std::min(block, input.size() - i));
    return l;
}
void timing() {
    for (double ms : {1., 10., 125., 1000., 5000.}) {
        D d;
        clean(d);
        d.set(D::LeftMs, ms);
        std::vector<float> in(240128);
        in[0] = 1;
        const auto out = render(d, in);
        const auto index = static_cast<std::size_t>(ms * 48);
        check(out[index] == 1, "millisecond impulse arrives at exact sample");
        check(std::count(out.begin(), out.end(), 1.f) == 1, "no duplicate impulse");
    }
    for (double bpm : {60., 90., 120., 180.}) {
        D d;
        clean(d);
        d.set(D::LeftSync, 1);
        d.set(D::LeftNotes, 4);
        d.tempo(bpm);
        std::vector<float> in(49000);
        in[0] = 1;
        const auto out = render(d, in);
        check(out[static_cast<std::size_t>(48000 * 60 / bpm)] == 1,
              "four sixteenths equal one quarter at known tempo");
    }
    D d;
    clean(d);
    d.set(D::LeftSync, 1);
    d.set(D::LeftNotes, 4);   // a quarter: 24000 samples at 120
    d.set(D::LeftOffset, 10);
    std::vector<float> in(30000), right(in.size());
    in[0] = 1;
    auto out = render(d, in);
    check(out[26400] == 1, "positive offset extends sync time");
    d.prepare(48000);
    d.set(D::RightMs, 20);
    d.set(D::RightSync, 0);
    d.set(D::LeftOffset, 0);
    out = render(d, in);
    check(out[960] == 1, "Stereo Link follows right-channel edits");
}
void feedback() {
    D d;
    clean(d);
    d.set(D::LeftMs, 1);
    d.set(D::Feedback, 50);
    d.set(D::PingPong, 1);
    std::vector<float> in(512), zero(512), l(512), r(512);
    in[0] = 1;
    d.process(in.data(), zero.data(), l.data(), r.data(), 512);
    check(l[48] == 1 && r[96] == 0.5f && l[144] == 0.25f,
          "Ping Pong alternates feedback between channels");
    d.prepare(48000);
    d.set(D::PingPong, 0);
    d.process(in.data(), zero.data(), l.data(), r.data(), 512);
    check(r == zero, "ordinary feedback stays independent");
    d.prepare(48000);
    d.set(D::LeftMs, 2);
    d.process(in.data(), zero.data(), l.data(), r.data(), 32);
    d.set(D::Freeze, 1);
    std::fill(in.begin(), in.end(), 0.25f);
    d.process(in.data(), in.data(), l.data(), r.data(), 512);
    check(l[64] == 1 && l[160] == 1 && l[256] == 1, "Freeze repeats without loss");
    check(r == zero, "Freeze rejects new input");
}
void transitions() {
    std::vector<float> input(16384);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(
            0.2 * std::sin(2 * std::numbers::pi * 733 * static_cast<double>(i) / 48000));
    double steps[3]{};
    std::vector<float> outputs[3];
    for (int mode = 0; mode < 3; ++mode) {
        D d;
        clean(d);
        d.set(D::Transition, mode);
        d.set(D::LeftMs, 20);
        outputs[mode].resize(input.size());
        std::vector<float> right(input.size());
        d.process(input.data(), input.data(), outputs[mode].data(), right.data(), 8192);
        d.set(D::LeftMs, 25);
        d.process(input.data() + 8192, input.data() + 8192, outputs[mode].data() + 8192,
                  right.data() + 8192, input.size() - 8192);
        for (std::size_t i = 8192; i < 9192; ++i)
            steps[mode] = std::max(steps[mode], std::abs(static_cast<double>(outputs[mode][i]) -
                                                         outputs[mode][i - 1]));
    }
    std::printf("METRIC time edit max steps Repitch %.6f Fade %.6f Jump %.6f\n", steps[0], steps[1],
                steps[2]);
    check(steps[2] > steps[1] * 3, "Fade reduces discontinuity compared with Jump");
    check(outputs[0] != outputs[1] && outputs[1] != outputs[2],
          "three time-change modes have distinct audible behavior");
    D d;
    clean(d);
    d.set(D::LeftMs, 10.123);
    auto linear = render(d, input);
    d.prepare(48000);
    d.set(D::HiQuality, 1);
    auto cubic = render(d, input);
    check(linear != cubic, "HiQuality selects different fractional interpolation");
    d.prepare(48000);
    d.set(D::Filter, 1);
    d.set(D::Freq, 8000);
    d.set(D::Width, 1);
    auto filtered = render(d, input);
    double a = 0, b = 0;
    for (std::size_t i = 8192; i < input.size(); ++i) {
        a += linear[i] * linear[i];
        b += filtered[i] * filtered[i];
    }
    check(b < a * 0.05, "bandpass rejects frequencies outside its passband");
}
void partition() {
    std::vector<float> in(32768);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(
            0.2 * std::sin(2 * std::numbers::pi * 733 * static_cast<double>(i) / 48000));
    for (int mode = 0; mode < 3; ++mode) {
        std::vector<float> expected;
        for (std::size_t block : {32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u}) {
            D d;
            d.prepare(48000);
            d.set(D::LeftMs, 10.5);
            d.set(D::Transition, mode);
            d.set(D::LfoDelay, 20);
            d.set(D::Filter, 1);
            d.set(D::LfoFilter, 10);
            d.set(D::HiQuality, 1);
            auto out = render(d, in, block);
            if (expected.empty())
                expected = out;
            check(out == expected, "all transition modes are block-independent");
        }
    }
    D d;
    clean(d);
    d.set(D::LeftMs, 2);
    std::vector<float> l(in.size()), r(in.size());
    counting = true;
    allocations = 0;
    d.process(in.data(), in.data(), l.data(), r.data(), in.size());
    counting = false;
    check(allocations == 0, "delay process allocates nothing");
    for (int wave = 0; wave < 7; ++wave)
        for (int rate = 0; rate < 6; ++rate) {
            d.set(D::Wave, wave);
            d.set(D::RateMode, rate);
            d.set(D::LfoDelay, 100);
            d.set(D::LfoFilter, 100);
            d.set(D::Filter, 1);
            auto out = render(d, in);
            check(std::all_of(out.begin(), out.end(), [](float v) { return std::isfinite(v); }),
                  "all LFO waveform/rate combinations remain finite");
        }
}
// DEV-22: Live 12.4's Delay at its browser default (synced 3/16 at 120 BPM,
// Feedback 50 %, Filter on at 1 kHz over 8 octaves, Dry/Wet 50 %), measured by
// the director's research at 48 kHz. Its first output sample is 0.7071 of an
// impulse (the dry path, equal-power). Its six tones are dry plus the filtered
// loop: solved for the loop filter they give a 12 dB/oct high-pass and
// low-pass (+79 degrees at 71 Hz, where one pole gives +41). The capture was
// not fully settled (a few percent of the feedback's transient remain after
// 2 s), so the tones are held to 0.5 dB and 2.5 degrees, not to the decimals.
void liveDefault() {
    D d;
    d.tempo(120);
    d.prepare(48000);
    std::vector<float> imp(256, 0.f);
    imp[0] = 1.f;
    const auto y = render(d, imp);
    check(std::abs(y[0] - 0.7071078420) < 2e-6, "Live default: the first sample is 0.7071 (equal-power dry)");

    struct Tone { double hz, gainDb, phaseDeg; };
    const Tone live[] = {{71, -8.5337909, -27.68}, {223, -11.1561, 26.85}, {997, 2.5788, 44.11},
                         {3001, -9.6027, -37.89}, {7001, -11.3895, -24.54}, {11003, -3.2267, -52.25}};
    D t;
    t.tempo(120);
    t.prepare(48000);
    const std::size_t n = 480000, from = n - 96000;
    std::vector<float> x(n, 0.f);
    for (const auto& tone : live)
        for (std::size_t i = 0; i < n; ++i)
            x[i] += static_cast<float>(0.05 * std::sin(2 * std::numbers::pi * tone.hz * static_cast<double>(i) / 48000));
    const auto out = render(t, x, 256);
    double worstDb = 0, worstDeg = 0;
    for (const auto& tone : live) {
        std::complex<double> in = 0, o = 0, ph = 1;
        const auto step = std::polar(1., -2 * std::numbers::pi * tone.hz / 48000);
        for (std::size_t i = 0; i < n; ++i) {
            if (i >= from) {
                in += static_cast<double>(x[i]) * ph;
                o += static_cast<double>(out[i]) * ph;
            }
            ph *= step;
        }
        const auto h = o / in;
        worstDb = std::max(worstDb, std::abs(20 * std::log10(std::abs(h)) - tone.gainDb));
        worstDeg = std::max(worstDeg, std::abs(std::remainder(std::arg(h) * 180 / std::numbers::pi - tone.phaseDeg, 360.)));
    }
    std::printf("Live default: six tones within %.3f dB and %.2f degrees\n", worstDb, worstDeg);
    check(worstDb < 0.5 && worstDeg < 2.5, "Live default: six tones within 0.5 dB and 2.5 degrees");
}
} // namespace
int main() {
    timing();
    feedback();
    transitions();
    partition();
    liveDefault();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
