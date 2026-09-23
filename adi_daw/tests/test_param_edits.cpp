// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0122 d11: each letter matches win's capture contract and mutation log.
#include "adi/engine/param_edits.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace {
std::atomic<long> allocations{0};
std::atomic<bool> counting{false};
}
void* operator new(std::size_t n) {
    if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace adi::engine;
using K = ParamEventKind;
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("  FAIL  %s\n", label); }
}
void send(ParamEditCapture& c, K k, double value = 0.0, std::int64_t d = 1, std::int32_t p = 2) {
    check(c.push({d, p, k, value}), "event accepted");
}
bool edit(const ParamEdit& e, double before, double after, bool implicit = false,
          std::int64_t d = 1, std::int32_t p = 2) {
    return e.deviceId == d && e.paramIndex == p && e.before == before &&
           e.after == after && e.implicit == implicit;
}
void a() {
    ParamEditCapture c(256); c.seed(1, 2, 0.25);
    std::vector<ParamEdit> out{{99, 99, 0, 1, false}};
    send(c, K::Begin);
    for (int i = 1; i <= 200; ++i) send(c, K::Value, static_cast<double>(i) / 200);
    check(c.drain(0, out) == 0 && out.size() == 1, "a: no edits before End");
    send(c, K::End);
    check(c.drain(1000, out) == 1 && out.size() == 2, "a: one drag appends one edit");
    check(out.size() == 2 && edit(out.back(), 0.25, 1.0), "a: first before, final after, explicit");
    send(c, K::Begin); send(c, K::Value, 0.6); send(c, K::End);
    check(c.drain(1001, out) == 1 && edit(out.back(), 1.0, 0.6), "a: next gesture uses last ended value");
    check(c.stats().edits == 2 && c.stats().implicitEdits == 0, "a: edit counters");
}
void b() {
    ParamEditCapture c(8); c.seed(1, 2, 0.1); c.expectEcho(1, 2, 0.5, 0);
    std::vector<ParamEdit> out;
    send(c, K::Value, 0.5);
    check(c.drain(0, out) == 0 && c.stats().echoesSwallowed == 1, "b: armed echo swallowed");
    send(c, K::Value, 0.5); c.drain(1, out);
    check(c.drain(151, out) == 1 && edit(out.back(), 0.5, 0.5, true), "b: unarmed same value is an edit; echo updated before");
    check(c.stats().echoesSwallowed == 1, "b: guard consumed once");
}
void c() {
    ParamEditCapture capture(8); capture.seed(1, 2, 0.1);
    std::vector<ParamEdit> out;
    send(capture, K::Value, 0.2); check(capture.drain(0, out) == 0, "c: implicit waits");
    check(capture.drain(149, out) == 0, "c: default quiet boundary not early");
    send(capture, K::Value, 0.8); check(capture.drain(149, out) == 0, "c: value extends quiet window");
    check(capture.drain(298, out) == 0, "c: extended window not early");
    check(capture.drain(299, out) == 1 && edit(out.back(), 0.1, 0.8, true), "c: coalesced edit at exact quiet boundary");
    capture.setQuietMs(10);
    send(capture, K::Value, 0.9); capture.drain(300, out);
    check(capture.drain(309, out) == 0 && capture.drain(310, out) == 1, "c: configurable quiet window");
    send(capture, K::Value, 0.7); send(capture, K::End);
    check(capture.drain(311, out) == 1 && edit(out.back(), 0.9, 0.7, true), "c: End closes an implicit gesture");
    check(capture.stats().strayEnds == 0 && capture.stats().implicitEdits == 3, "c: implicit counters");
}
void d() {
    ParamEditCapture c(8); std::vector<ParamEdit> out;
    send(c, K::End); send(c, K::Begin); send(c, K::Begin); send(c, K::End);
    check(c.drain(0, out) == 0 && out.empty(), "d: empty and stray brackets emit nothing");
    check(c.stats().strayBegins == 1 && c.stats().strayEnds == 1, "d: stray brackets counted");
    c.seed(1, 2, 0.1); send(c, K::Begin); send(c, K::Value, 0.9); send(c, K::Begin); send(c, K::End);
    check(c.drain(1, out) == 1 && edit(out.back(), 0.1, 0.9), "d: stray Begin does not reset pending value");
}
void e() {
    ParamEditCapture c(3); c.seed(1, 2, 0.1); std::vector<ParamEdit> out;
    send(c, K::Begin); send(c, K::Value, 0.7); send(c, K::End);
    check(!c.push({1, 2, K::Value, 0.9}) && !c.push({1, 2, K::End, 0}), "e: exactly capacity slots, full returns false");
    check(c.stats().dropped == 2 && c.stats().pushed == 3, "e: accepted and dropped counts");
    check(c.drain(0, out) == 1 && edit(out.back(), 0.1, 0.7), "e: overflow preserves queued gesture");
    for (int i = 0; i < 20; ++i) {
        send(c, K::Begin); send(c, K::Value, 0.8); send(c, K::End);
        check(c.drain(i + 1, out) == 1, "e: ring reusable across wrap");
    }
    check(c.stats().dropped == 2 && c.stats().pushed == 63, "e: wrap preserves counters");
    ParamEditCapture zero(0);
    check(!zero.push({}) && zero.stats().dropped == 1, "e: zero capacity drops safely");
}
void f() {
    ParamEditCapture c(16); c.seed(1, 2, 0.1); c.seed(1, 3, 0.2); c.seed(2, 2, 0.3);
    std::vector<ParamEdit> out;
    send(c, K::Value, 0.4, 1, 2); send(c, K::Value, 0.5, 1, 3); send(c, K::Value, 0.6, 2, 2);
    c.drain(0, out);
    send(c, K::Value, 0.7, 1, 2); send(c, K::Value, 0.8, 1, 3); c.drain(100, out);
    check(c.drain(150, out) == 1 && edit(out.back(), 0.3, 0.6, true, 2, 2), "f: device identity and independent quiet window");
    check(c.drain(250, out) == 2 && out.size() == 3, "f: two interleaved params emit twice");
    bool first = false, second = false;
    for (const auto& v : out) { first |= edit(v, 0.1, 0.7, true, 1, 2); second |= edit(v, 0.2, 0.8, true, 1, 3); }
    check(first && second, "f: separate before/after per parameter");
}
void g() {
    ParamEditCapture c(16); c.seed(1, 2, 0.1); c.expectEcho(1, 2, 0.5, 0);
    std::vector<ParamEdit> out;
    send(c, K::Value, 0.5000001); c.drain(0, out);
    check(c.stats().echoesSwallowed == 1, "g: default tolerance accepts near echo");
    c.expectEcho(1, 2, 0.5, 1); send(c, K::Value, 0.51); c.drain(1, out);
    check(c.drain(151, out) == 1 && edit(out.back(), 0.5, 0.51, true), "g: nonmatching value is real edit");
    send(c, K::Value, 0.5); c.drain(152, out);
    check(c.stats().echoesSwallowed == 2 && out.size() == 1, "g: nonmatching value leaves guard armed");
    c.setEchoTolerance(0.0); c.expectEcho(1, 2, 0.5, 153);
    send(c, K::Value, 0.5000001); c.drain(153, out);
    check(c.drain(303, out) == 1, "g: zero tolerance rejects near echo");
}
void h() {
    ParamEditCapture c(16); c.seed(1, 2, 0.1); std::vector<ParamEdit> out;
    c.expectEcho(1, 2, 0.5, 0); send(c, K::Value, 0.5); c.drain(500, out);
    check(c.stats().echoesSwallowed == 1 && c.stats().guardsExpired == 0, "h: TTL inclusive at 500ms");
    c.expectEcho(1, 2, 0.6, 500); send(c, K::Value, 0.6); c.drain(1001, out);
    check(c.stats().guardsExpired == 1 && c.drain(1151, out) == 1, "h: older guard expires before queued Value");
    c.setEchoTtlMs(10); c.expectEcho(1, 2, 0.7, 1200); c.drain(1211, out);
    c.drain(1212, out);
    check(c.stats().guardsExpired == 2, "h: expires once on empty drain, configurable TTL");
    c.expectEcho(1, 2, 0.8, 1300); c.expectEcho(1, 2, 0.9, 1309);
    send(c, K::Value, 0.9); c.drain(1319, out);
    check(c.stats().echoesSwallowed == 2 && c.stats().guardsExpired == 2, "h: rearm replaces value and timestamp");
}
void i() {
    ParamEditCapture c(16); c.seed(1, 2, 0.1); c.expectEcho(1, 2, 0.5, 0);
    std::vector<ParamEdit> out;
    send(c, K::Begin); send(c, K::Value, 0.5); send(c, K::End);
    check(c.drain(0, out) == 1 && edit(out.back(), 0.1, 0.5), "i: matching Value inside explicit gesture retained");
    check(c.stats().echoesSwallowed == 0 && c.stats().strayEnds == 0, "i: brackets never swallowed");
    send(c, K::Value, 0.7); c.drain(1, out); send(c, K::Value, 0.5); c.drain(2, out);
    check(c.drain(152, out) == 1 && edit(out.back(), 0.5, 0.5, true), "i: matching Value inside implicit gesture retained");
    check(c.stats().echoesSwallowed == 0, "i: open implicit gesture protects matching value");
}
void j() {
    ParamEditCapture c(16); std::vector<ParamEdit> out;
    send(c, K::Begin); send(c, K::Value, 0.3); send(c, K::Value, 0.8); send(c, K::End);
    check(c.drain(0, out) == 1 && edit(out.back(), 0.3, 0.8), "j: unseeded before is first Value");
    check(c.stats().unseeded == 1, "j: unseeded counted once per gesture");
    send(c, K::Value, 0.9); c.drain(1, out);
    check(c.drain(151, out) == 1 && edit(out.back(), 0.8, 0.9, true), "j: ended value becomes known");
    check(c.stats().unseeded == 1, "j: known subsequent gesture is not unseeded");
}
void k() {
    ParamEditCapture c(8); std::vector<ParamEdit> out;
    for (int round = 0; round < 3; ++round) {
        allocations.store(0); counting.store(true);
        bool accepted = true;
        for (int n = 0; n < 8; ++n) accepted &= c.push({1, 2, static_cast<K>(n % 3), 0.5});
        const bool rejected = !c.push({});
        counting.store(false);
        check(accepted && rejected, "k: pushes exercise empty, occupied, wrap and full paths");
        check(allocations.load() == 0, "k: zero allocations in push");
        c.drain(round * 1000, out);
    }
}
void l() {
    ParamEditCapture c(17); c.seed(1, 2, 0.0); std::vector<ParamEdit> out;
    constexpr int gestures = 10000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int n = 1; n <= gestures; ++n) {
            for (const auto kind : {K::Begin, K::Value, K::End}) {
                const ParamEvent e{1, 2, kind, static_cast<double>(n)};
                while (!c.push(e)) std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        c.drain(0, out);
        (void)c.stats(); // exercise the consumer snapshot while pushes continue
        std::this_thread::yield();
    }
    producer.join(); c.drain(0, out);
    check(out.size() == gestures, "l: concurrent capture loses no accepted gesture");
    bool ordered = true;
    for (std::size_t n = 0; n < out.size(); ++n)
        ordered &= edit(out[n], static_cast<double>(n), static_cast<double>(n + 1));
    check(ordered, "l: concurrent payloads and before/after remain ordered");
    check(c.stats().pushed == gestures * 3 && c.stats().edits == gestures &&
          c.stats().unseeded == 0 && c.stats().strayBegins == 0 && c.stats().strayEnds == 0,
          "l: concurrent counters match consumed stream");
}
} // namespace
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const struct { const char* name; void (*run)(); } cases[] = {
        {"a", a}, {"b", b}, {"c", c}, {"d", d}, {"e", e}, {"f", f},
        {"g", g}, {"h", h}, {"i", i}, {"j", j}, {"k", k}, {"l", l}};
    for (const auto& t : cases) if (argc == 1 || std::strcmp(argv[1], t.name) == 0) {
        std::printf("[param edits %s]\n", t.name); t.run();
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks != 0 ? 0 : 1;
}
