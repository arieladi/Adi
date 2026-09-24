// SPDX-License-Identifier: GPL-3.0-or-later
//
// Automation read into the engine (ADR-0159, part two): placement in session
// samples, validation that refuses a whole lane, and valueAt/fill evaluated
// through SPEC §6.3.2's curves without allocating.
#include "adi/blob.hpp"
#include "adi/engine/automation.hpp"
#include "adi/engine/curves.hpp"
#include "adi/store.hpp"
#include "adi/store_rows.hpp"
#include "temp_directory.hpp"

#include <SQLiteCpp/SQLiteCpp.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Allocation counter: fill and valueAt must not allocate.
namespace {
std::atomic<long> allocations{0};
std::atomic<bool> counting{false};
}  // namespace
void* operator new(std::size_t n) {
    if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace adi;
using namespace adi::engine;

int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}

constexpr std::int64_t kQuarter = 5765760;   // ticks per quarter (SPEC §4.2)

AutomationPoint pt(std::int64_t time, double value, std::uint8_t curve = 1, float tension = 0.0f) {
    AutomationPoint p{};
    p.time = time;
    p.value = value;
    p.curve = curve;
    p.tension = tension;
    return p;
}

std::vector<std::byte> stream(const std::vector<AutomationPoint>& points, bool nanos) {
    std::uint32_t flags = StreamFlags::SortedByTime | (nanos ? StreamFlags::TimeIsNanos : 0u);
    return writeStream<AutomationPoint>(FourCC::Automation, std::span<const AutomationPoint>(points), 1, flags);
}

rows::AutomationLane laneRow(std::int64_t id, std::int64_t timeBase, std::string owner = "track") {
    rows::AutomationLane l;
    l.id = id;
    l.ownerKind = std::move(owner);
    l.ownerId = 7;
    l.paramRef = "volume";
    l.timeBase = timeBase;
    l.valueDomain = "real";
    l.defaultValue = -6.0;
    return l;
}

void add(rows::Model& m, rows::AutomationLane lane, const std::vector<AutomationPoint>& points) {
    rows::AutomationData d;
    d.laneId = lane.id;
    d.blob = stream(points, lane.timeBase == 1);
    m.automationLanes.push_back(std::move(lane));
    m.automationData.push_back(std::move(d));
}

bool hasProblem(const AutomationProgram& p, const std::string& a, const std::string& b = "") {
    for (const auto& s : p.problems())
        if (s.find(a) != std::string::npos && s.find(b) != std::string::npos) return true;
    return false;
}

// --- placement --------------------------------------------------------------------------

void placement() {
    const std::array<double, 6> rates{44100.0, 48000.0, 96000.0, 192000.0, 384000.0, 768000.0};
    bool ticks = true, nanos = true, tempo = true;
    for (double rate : rates) {
        rows::Model m;   // no tempo rows: 120 BPM
        add(m, laneRow(1, 0), {pt(0, 0.0), pt(kQuarter, 1.0), pt(3 * kQuarter, 2.0)});
        add(m, laneRow(2, 1), {pt(0, 0.0), pt(1250000000, 1.0)});
        AutomationProgram prog(m, rate);
        const auto* t = prog.lane(1);
        const auto* n = prog.lane(2);
        ticks = ticks && t && t->points.size() == 3 && t->points[1].sample == std::llround(0.5 * rate) &&
                t->points[2].sample == std::llround(1.5 * rate);
        nanos = nanos && n && n->points[1].sample == std::llround(1.25 * rate);

        rows::Model changed;   // 120 BPM for a quarter, then 60
        changed.tempo = {{0, 120.0, 0, 0.0}, {kQuarter, 60.0, 0, 0.0}};
        add(changed, laneRow(1, 0), {pt(0, 0.0), pt(2 * kQuarter, 1.0)});
        AutomationProgram p2(changed, rate);
        tempo = tempo && p2.lane(1) && p2.lane(1)->points[1].sample == std::llround(1.5 * rate) &&
                p2.problems().empty();
    }
    check(ticks, "time_base 0: ticks placed through the tempo map, 44.1 to 768 kHz");
    check(nanos, "time_base 1: nanoseconds placed at the session rate, 44.1 to 768 kHz");
    check(tempo, "a tempo change moves a later point: a quarter at 120 then a quarter at 60 is 1.5 s");

    rows::Model ramp;
    ramp.tempo = {{0, 120.0, 0, 0.0}, {kQuarter, 90.0, 1, 0.0}};
    add(ramp, laneRow(1, 0), {pt(0, 0.0)});
    AutomationProgram pr(ramp, 48000.0);
    check(hasProblem(pr, "tempo ramps") && pr.lane(1) != nullptr, "tempo ramps are reported as ADR-0155 reports them; the lane still compiles");
}

// --- evaluation ------------------------------------------------------------------------

void evaluation() {
    rows::Model m;
    add(m, laneRow(1, 1), {pt(0, 2.0), pt(1000000000, 2.0, 0), pt(1000000000, 5.0), pt(2000000000, 9.0)});
    add(m, laneRow(2, 1), {});
    add(m, laneRow(3, 1), {pt(500000000, 4.0)});
    AutomationProgram prog(m, 48000.0);
    const auto& a = *prog.lane(1);
    check(valueAt(a, -100) == 2.0 && valueAt(a, 0) == 2.0, "before the first point: the first value");
    check(valueAt(a, 47999) == 2.0 && valueAt(a, 48000) == 5.0, "two points at one time are a step: the later value from that sample on");
    check(valueAt(a, 72000) == 7.0, "linear halfway between 5 and 9");
    check(valueAt(a, 96000) == 9.0 && valueAt(a, 1000000000) == 9.0, "at and after the last point: the last value");
    check(valueAt(*prog.lane(2), 12345) == -6.0, "a lane with no points reads its default_value");
    check(valueAt(*prog.lane(3), 0) == 4.0 && valueAt(*prog.lane(3), 99999) == 4.0, "a lane with one point reads it everywhere");

    // Every curve shape through SPEC 6.3.2's evaluator.
    bool shapes = true;
    for (std::uint8_t c = 0; c <= 5; ++c)
        for (float t : {-1.0f, -0.4f, 0.0f, 0.6f, 1.0f}) {
            rows::Model s;
            add(s, laneRow(9, 1), {pt(0, -1.0, c, t), pt(1000000000, 3.0)});
            AutomationProgram ps(s, 48000.0);
            for (std::int64_t k = 0; k <= 48000; k += 1500)
                shapes = shapes && valueAt(*ps.lane(9), k) ==
                                       curveValue(-1.0, 3.0, static_cast<double>(k) / 48000.0, c, static_cast<double>(t));
        }
    check(shapes, "every curve shape and tension evaluates through the curve formulas");

    // fill agrees with valueAt at every stride, and allocates nothing.
    std::vector<double> buf(4096);
    bool agree = true;
    for (std::int64_t stride : {1, 7, 64, 0, -3}) {
        const std::int64_t first = stride < 0 ? 120000 : -500;
        fill(a, first, stride, buf.size(), buf.data());
        for (std::size_t k = 0; k < buf.size(); ++k)
            agree = agree && buf[k] == valueAt(a, first + static_cast<std::int64_t>(k) * stride);
    }
    check(agree, "fill agrees with valueAt: strides 1, 7, 64, 0 and backwards");
    allocations.store(0);
    counting.store(true);
    fill(a, 0, 1, buf.size(), buf.data());
    fill(a, -10, 33, buf.size(), buf.data());
    double sum = 0.0;
    for (std::int64_t k = 0; k < 1000; ++k) sum += valueAt(a, k * 97);
    counting.store(false);
    check(allocations.load() == 0 && std::isfinite(sum), "fill and valueAt allocate nothing");
}

// --- refusals ---------------------------------------------------------------------------

void refusals() {
    struct Case { const char* what; const char* expect; std::vector<AutomationPoint> points; };
    AutomationPoint reserved = pt(20, 1.0);
    reserved.reserved = 1;
    const std::vector<Case> cases = {
        {"times out of order", "out of order", {pt(0, 0.0), pt(100, 1.0), pt(50, 0.5), pt(200, 1.0)}},
        {"a non-finite value", "not finite", {pt(0, 0.0), pt(10, std::numeric_limits<double>::quiet_NaN())}},
        {"an infinite value", "not finite", {pt(0, 0.0), pt(10, std::numeric_limits<double>::infinity())}},
        {"a value below min_value", "below min_value", {pt(0, 0.0), pt(10, -1.5)}},
        {"a value above max_value", "above max_value", {pt(0, 0.0), pt(10, 12.5)}},
        {"curve above 5", "above 5", {pt(0, 0.0, 6), pt(10, 1.0)}},
        {"non-zero reserved bytes", "reserved bytes", {pt(0, 0.0), reserved}},
        {"a time before the project start", "before the project start", {pt(-10, 0.0), pt(10, 1.0)}},
    };
    for (const auto& c : cases) {
        rows::Model m;
        auto bad = laneRow(4, 0);
        bad.minValue = -1.0;
        bad.maxValue = 12.0;
        add(m, bad, c.points);
        add(m, laneRow(5, 0), {pt(0, 1.0), pt(kQuarter, 2.0)});   // a good lane beside it
        AutomationProgram prog(m, 48000.0);
        check(prog.lane(4) == nullptr && hasProblem(prog, "automation_lanes#4: refused", c.expect) && prog.lane(5) != nullptr,
              std::string("refused whole, with a named problem: ") + c.what);
    }

    // Header-level refusals.
    rows::Model m;
    rows::AutomationData flags;
    flags.laneId = 6;
    flags.blob = writeStream<AutomationPoint>(FourCC::Automation, std::vector<AutomationPoint>{pt(0, 0.0)}, 1,
                                              StreamFlags::SortedByTime | (1u << 5));
    m.automationLanes.push_back(laneRow(6, 0));
    m.automationData.push_back(flags);
    add(m, laneRow(7, 0), {pt(0, 0.0)});
    m.automationData.back().blob = stream({pt(0, 0.0)}, true);   // says ns; the lane says ticks
    rows::AutomationData notAut;
    notAut.laneId = 8;
    notAut.blob = writeStream<NoteRecord>(FourCC::Notes, std::vector<NoteRecord>(1));
    m.automationLanes.push_back(laneRow(8, 0));
    m.automationData.push_back(notAut);
    AutomationProgram prog(m, 48000.0);
    check(!prog.lane(6) && hasProblem(prog, "#6: refused", "reserved header flags"), "refused: reserved header flag bits");
    check(!prog.lane(7) && hasProblem(prog, "#7: refused", "time unit"), "refused: a stream in ns on a lane in ticks");
    check(!prog.lane(8) && hasProblem(prog, "#8: refused", "not readable"), "refused: a stream that is not AAUT");

    // A bad point late in a long lane: nothing of it is kept.
    rows::Model late;
    std::vector<AutomationPoint> many;
    for (int k = 0; k < 100; ++k) many.push_back(pt(k * 1000, k));
    many[97].value = std::numeric_limits<double>::quiet_NaN();
    add(late, laneRow(11, 0), many);
    AutomationProgram lp(late, 48000.0);
    check(lp.lane(11) == nullptr && lp.lanes().empty() && hasProblem(lp, "#11: refused", "point 97"),
          "a bad point 97 of 100 refuses the whole lane, never the first 97");

    bool twoAtOnce = true;
    rows::Model step;
    add(step, laneRow(12, 0), {pt(0, 0.0), pt(kQuarter, 0.0), pt(kQuarter, 1.0)});
    AutomationProgram sp(step, 48000.0);
    twoAtOnce = sp.lane(12) != nullptr && sp.problems().empty();
    check(twoAtOnce, "two points at one time are a step, allowed");

    AutomationProgram noRate(step, 0.0);
    check(noRate.lanes().empty() && hasProblem(noRate, "session rate"), "a session rate of 0 compiles nothing, and says so");
}

void notPlayedYet() {
    rows::Model m;
    add(m, laneRow(1, 0, "project"), {pt(0, 1.0)});
    add(m, laneRow(2, 0, "routing"), {pt(0, 1.0)});
    add(m, laneRow(3, 0, "clip"), {pt(0, 1.0)});
    add(m, laneRow(4, 0, "device"), {pt(0, 1.0)});
    rows::AutomationData env;
    env.laneId = 4;
    env.clipId = 77;
    env.blob = stream({pt(0, 0.5)}, false);
    m.automationData.push_back(env);
    AutomationProgram prog(m, 48000.0);
    check(!prog.lane(1) && hasProblem(prog, "#1", "owner 'project' not played yet"), "a 'project' lane is reported as not played yet");
    check(!prog.lane(2) && hasProblem(prog, "#2", "owner 'routing' not played yet"), "a 'routing' lane is reported as not played yet");
    check(!prog.lane(3) && hasProblem(prog, "#3", "owner 'clip' not played yet"), "a clip-owned lane is reported as not played yet");
    check(prog.lane(4) && prog.lane(4)->points.size() == 1 && valueAt(*prog.lane(4), 0) == 1.0 &&
              hasProblem(prog, "#4", "clip envelope (clips#77) not played yet"),
          "a clip envelope is reported and never mixed into its lane's arrangement stream");
}

// --- the rows, through a real store ------------------------------------------------------

void throughTheStore() {
    const test::TempDirectory scratch("automation", "store");
    StoreError e = StoreError::Ok;
    const auto store = Store::create(scratch.path() / "auto.adi", e);
    check(store != nullptr, "a store to read from");
    if (!store) return;
    store->db().exec(
        "INSERT INTO automation_lanes(id, owner_kind, owner_id, param_ref, param_name, time_base, value_domain, "
        "unit, default_value, min_value, max_value, enabled) "
        "VALUES (3, 'track', 1, 'volume', 'Volume', 0, 'real', 'dB', -6.0, -70.0, 6.0, 0)");
    const auto blob = stream({pt(0, -12.0), pt(kQuarter, 0.0, 2, 0.5f)}, false);
    check(store->putAutomationData(3, std::nullopt, blob), "the lane's stream is stored");
    store->db().exec("INSERT INTO tracks(id, kind, name) VALUES (1, 'audio', 'Bass')");
    store->db().exec("INSERT INTO clips(id, track_id, kind, time_base, pos_ticks) VALUES (42, 1, 'midi', 0, 0)");
    const auto env = stream({pt(0, 1.0)}, false);
    check(store->putAutomationData(3, 42, env), "a clip envelope is stored beside it");
    const rows::Model m = rows::readModel(*store);
    const bool lane = m.automationLanes.size() == 1 && m.automationLanes[0].id == 3 &&
                      m.automationLanes[0].ownerKind == "track" && m.automationLanes[0].unit == "dB" &&
                      m.automationLanes[0].minValue == -70.0 && m.automationLanes[0].maxValue == 6.0 &&
                      !m.automationLanes[0].enabled && m.automationLanes[0].valueDomain == "real";
    check(lane, "readModel reads the lane row, nullable bounds included");
    check(m.automationData.size() == 2 && m.automationData[0].blob == blob && !m.automationData[0].clipId &&
              m.automationData[1].clipId == 42,
          "readModel reads each stream's bytes, the arrangement stream first");
    AutomationProgram prog(m, 48000.0);
    check(prog.problems().size() == 1 && hasProblem(prog, "#3", "clip envelope (clips#42)"),
          "the stored clip envelope is reported, the only problem");
    const auto* l = prog.lane(3);
    check(l && !l->enabled && l->points.size() == 2 && l->points[1].sample == 24000 && l->points[1].curve == 2,
          "compiled from the store: placed, and a disabled lane keeps enabled = false");
}

// --- concurrency: an immutable program read from many threads ---------------------------

void concurrent() {
    rows::Model m;
    std::vector<AutomationPoint> points;
    for (int k = 0; k < 500; ++k) points.push_back(pt(static_cast<std::int64_t>(k) * 10000000, k % 7, static_cast<std::uint8_t>(k % 6), 0.3f));
    add(m, laneRow(1, 1), points);
    const auto program = compileAutomation(m, 96000.0);
    std::atomic<int> mismatches{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
        readers.emplace_back([&, r] {
            std::array<double, 512> buf{};
            const auto* lane = program->lane(1);
            for (int round = 0; round < 50; ++round) {
                const std::int64_t first = static_cast<std::int64_t>(round) * 9000 + r;
                fill(*lane, first, 5, buf.size(), buf.data());
                for (std::size_t k = 0; k < buf.size(); k += 37)
                    if (buf[k] != valueAt(*lane, first + static_cast<std::int64_t>(k) * 5)) mismatches.fetch_add(1);
            }
        });
    const auto other = compileAutomation(m, 48000.0);   // a new generation built meanwhile
    for (auto& t : readers) t.join();
    check(mismatches.load() == 0 && other->lane(1) != nullptr, "four threads read one program while another compiles");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        placement();
        evaluation();
        refusals();
        notPlayedYet();
        throughTheStore();
        concurrent();
    } catch (const std::exception& ex) {
        check(false, std::string("unexpected exception: ") + ex.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
