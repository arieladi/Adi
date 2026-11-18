// SPDX-License-Identifier: AGPL-3.0-or-later
//
// STACK's model (src/adi/ui/stack_model.hpp), headless: the geometry laws, hover and the anchors,
// scrolling, the width budget in both module layouts, the chain lane, rows and scope, identity and
// the capture, keys and focus, the navigator, search, pins, the comparison pair, view state, the
// snapshot adapter, determinism, the work counters and the allocation audit.
//
// Times are milliseconds the test hands in; "at rest" means after advance() past every deadline.
// A row is named by its shown index unless a check says "track".
#include "adi/ui/stack_model.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/ui/arrangement_geometry.hpp"
#include "adi/ui/mix_view_provider.hpp"
#include "adi/ui/snapshot_reader.hpp"
#include "adi/ui/track_order.hpp"
#include "adi/ui/track_tree.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace {
thread_local bool audit = false;
std::atomic<unsigned> allocations{0};
} // namespace
void *operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
    try {
        return operator new(n);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept { return operator new(n, t); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
using namespace adi::ui;
using namespace adi::ui::stack;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}
void section(const char *name) { std::printf("[%s]\n", name); }
bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

// --- fixtures ---------------------------------------------------------------------------------------

constexpr std::uint32_t kQ = 1, kDuck = 2, kClip = 3, kL = 4;
constexpr std::uint64_t kAudio = 0xA0D10, kGroupKind = 0x6E0, kMainKind = 0x3A1;

ModuleKindInput kindOf(std::uint32_t key, std::string_view name, std::string_view search, ItemClass cls, bool keeps,
                       bool preview) {
    ModuleKindInput k;
    k.key = key;
    k.name = name;
    k.search = search;
    k.widthClass = cls;
    k.keepsDormantColumn = keeps;
    k.dormantPreview = preview;
    return k;
}
// Four modules in processing order, as the integrated channel processor runs them today.
std::vector<ModuleKindInput> fourKinds() {
    return {kindOf(kQ, "ADI-Q", "adi-q eq", ItemClass::AdiQ, true, true),
            kindOf(kDuck, "Ducking", "ducking sidechain sc", ItemClass::Ducking, false, false),
            kindOf(kClip, "Clip", "clip", ItemClass::Clip, false, false),
            kindOf(kL, "ADI-L", "adi-l limiter", ItemClass::AdiL, false, false)};
}

struct Fixture {
    std::vector<ModuleKindInput> kinds = fourKinds();
    std::vector<TrackInput> tracks;
    std::vector<std::vector<ItemInput>> chains;
    std::vector<ModuleInput> modules;
    std::vector<ItemInput> flat;
    std::deque<std::string> text; // stable storage for every name a view points at
    std::uint64_t generation = 0;

    std::string_view keep(const std::string &s) {
        text.push_back(s);
        return text.back();
    }
    static std::uint64_t instanceOf(TrackId id, std::size_t k) {
        return static_cast<std::uint64_t>(id) * 100 + static_cast<std::uint64_t>(k) + 1;
    }
    std::size_t add(TrackId id, const std::string &name, TrackId parent = 0, std::uint32_t depth = 0,
                    std::uint64_t identity = kAudio) {
        TrackInput t;
        t.id = id;
        t.parent = parent;
        t.depth = depth;
        t.identity = identity;
        t.name = keep(name);
        t.hasMixFunction = true; // geometry fixtures represent eligible tool channels
        tracks.push_back(t);
        chains.emplace_back();
        for (std::size_t k = 0; k < kinds.size(); ++k) {
            ModuleInput m;
            m.state = ModuleState::Dormant;
            m.instance = instanceOf(id, k);
            modules.push_back(m);
        }
        return tracks.size() - 1;
    }
    std::size_t at(TrackId id) const {
        if (id >= 1 && static_cast<std::size_t>(id) <= tracks.size() && tracks[static_cast<std::size_t>(id) - 1].id == id)
            return static_cast<std::size_t>(id) - 1; // U(n): track i at i - 1
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].id == id)
                return i;
        return tracks.size();
    }
    TrackInput &track(TrackId id) { return tracks[at(id)]; }
    std::size_t kindAt(std::uint32_t key) const {
        for (std::size_t k = 0; k < kinds.size(); ++k)
            if (kinds[k].key == key)
                return k;
        return kinds.size();
    }
    ModuleInput &module(TrackId id, std::uint32_t key) { return modules[at(id) * kinds.size() + kindAt(key)]; }
    void item(TrackId id, DeviceId device, ItemClass cls, const std::string &name, std::uint64_t identity = 0) {
        ItemInput in;
        in.device = device;
        in.identity = identity != 0 ? identity : static_cast<std::uint64_t>(device) * 7 + 3;
        in.cls = cls;
        in.name = keep(name);
        chains[at(id)].push_back(in);
    }
    void erase(TrackId id) {
        const std::size_t i = at(id);
        tracks.erase(tracks.begin() + static_cast<std::ptrdiff_t>(i));
        chains.erase(chains.begin() + static_cast<std::ptrdiff_t>(i));
        const auto from = modules.begin() + static_cast<std::ptrdiff_t>(i * kinds.size());
        modules.erase(from, from + static_cast<std::ptrdiff_t>(kinds.size()));
    }
    void insert(std::size_t position, TrackId id, const std::string &name) {
        add(id, name);
        // move the new track (added last) to `position`
        std::rotate(tracks.begin() + static_cast<std::ptrdiff_t>(position), tracks.end() - 1, tracks.end());
        std::rotate(chains.begin() + static_cast<std::ptrdiff_t>(position), chains.end() - 1, chains.end());
        std::rotate(modules.begin() + static_cast<std::ptrdiff_t>(position * kinds.size()),
                    modules.end() - static_cast<std::ptrdiff_t>(kinds.size()), modules.end());
    }
    RowsInput input() {
        flat.clear();
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            tracks[i].itemBegin = static_cast<std::uint32_t>(flat.size());
            tracks[i].itemCount = static_cast<std::uint32_t>(chains[i].size());
            for (const auto &in : chains[i])
                flat.push_back(in);
        }
        RowsInput in;
        in.kinds = kinds;
        in.tracks = tracks;
        in.items = flat;
        in.modules = modules;
        in.generation = generation;
        return in;
    }
};

// U(n): tracks 1..n "Track 1".., every module dormant, no chain, all in view.
Fixture uniform(std::size_t n) {
    Fixture f;
    f.tracks.reserve(n);
    f.chains.reserve(n);
    f.modules.reserve(n * f.kinds.size());
    for (std::size_t i = 1; i <= n; ++i)
        f.add(static_cast<TrackId>(i), "Track " + std::to_string(i));
    return f;
}

// Eleven rows (and the Reference, never a row), as TrackOrder gives them: shown 0..10 are tracks 1..11.
Fixture eleven() {
    Fixture f;
    f.add(1, "Kick");
    f.track(1).roles = role::Kick;
    f.track(1).colour = 12;
    f.add(2, "Kick Sub");
    f.track(2).colour = 12;
    f.add(3, "Snare");
    f.track(3).colour = 3;
    f.item(3, 31, ItemClass::Eq, "EQ Eight");
    f.item(3, 32, ItemClass::View, "Saturator");
    f.add(4, "Bass Synth");
    f.track(4).roles = role::Bass;
    f.track(4).colour = 20;
    f.module(4, kL).state = ModuleState::Engaged;
    f.add(5, "Vox Verse");
    f.track(5).roles = role::Vocal;
    f.track(5).colour = 33;
    f.add(6, "Vox Chorus");
    f.track(6).colour = 33;
    f.add(7, "Caf\xC3\xA9 Pad");
    f.add(8, "FX Bus", 0, 0, kGroupKind);
    f.track(8).group = true;
    f.track(8).colour = 50;
    f.add(9, "Riser", 8, 1);
    f.add(10, "Impact", 8, 1);
    f.add(12, "Ref Mix");
    f.track(12).reference = true;
    for (auto key : {kQ, kDuck, kClip, kL})
        f.module(12, key) = ModuleInput{};
    f.add(11, "Main", 0, 0, kMainKind);
    f.track(11).main = true;
    f.module(11, kDuck) = ModuleInput{}; // nothing to duck from above Main
    return f;
}

// One row whose chain is EQ, a compressor, three views and an analyser; nothing engaged.
Fixture laneFixture() {
    Fixture f;
    f.add(1, "Lane");
    f.item(1, 101, ItemClass::Eq, "EQ Eight");
    f.item(1, 102, ItemClass::Dynamics, "Compressor");
    f.item(1, 103, ItemClass::View, "Auto Filter");
    f.item(1, 104, ItemClass::View, "Saturator");
    f.item(1, 105, ItemClass::View, "Utility");
    f.item(1, 106, ItemClass::Analyser, "Spectrum");
    return f;
}

StackModel model(Fixture &f, double width = 1264, double height = 600, const Constants &c = Constants{}) {
    StackModel m(c);
    m.setViewport(width, height, 0);
    m.setRows(f.input(), 0);
    return m;
}

std::vector<TrackId> shownIds(const StackModel &m) {
    std::vector<TrackId> ids;
    for (std::size_t i = 0; i < m.shownCount(); ++i)
        ids.push_back(m.shownTrack(i));
    return ids;
}
double contentTopOf(const StackModel &m, std::size_t shown) { return m.row(shown).top + m.scroll(); }
double gapBelow(const StackModel &m, std::size_t shown) {
    const RowGeo a = m.row(shown), b = m.row(shown + 1);
    return b.top - a.top - a.height;
}
// Keyboard focus in STACK, the pointer outside: `shown` enlarged at rest, then the list at `scroll`.
void restWith(StackModel &m, std::size_t shown, double scroll, TimeMs &t) {
    m.setKeyboardFocus(true, t);
    m.focusRow(m.rowRef(m.shownTrack(shown)), t);
    t += 400;
    m.advance(t);
    m.scrollTo(scroll, t);
    t += 400;
    m.advance(t);
}
Hit chipHit(const StackModel &m, std::size_t shown) {
    const ChipGeo g = m.chip(shown);
    return m.hitTest(g.chip.x + 4, g.chip.y + 4);
}
void press(StackModel &m, double x, double y, TimeMs t, Mods mods = Mods{}, int clicks = 1) {
    m.pointerDown(x, y, Button::Left, mods, clicks, t);
    m.pointerUp(x, y, t);
}
void pressChip(StackModel &m, std::size_t shown, TimeMs t, Mods mods = Mods{}, int clicks = 1) {
    const ChipGeo g = m.chip(shown);
    press(m, g.chip.x + 4, g.chip.y + 4, t, mods, clicks);
}
KindScope scopeOf(ItemClass cls, bool keeps, bool engaged, bool dormant) {
    KindScope k;
    k.widthClass = cls;
    k.keepsDormantColumn = keeps;
    k.engaged = engaged;
    k.dormant = dormant;
    return k;
}
// The four kinds' scope with `engaged` (Q, ducking, Clip, L) and every other row dormant.
std::array<KindScope, 4> fourScope(bool q, bool duck, bool clip, bool l) {
    return {scopeOf(ItemClass::AdiQ, true, q, true), scopeOf(ItemClass::Ducking, false, duck, true),
            scopeOf(ItemClass::Clip, false, clip, true), scopeOf(ItemClass::AdiL, false, l, true)};
}

// --- the laws -------------------------------------------------------------------------------------------

void fisheyeAtBase100() {
    section("T1 the fisheye at base 100");
    Fixture f = uniform(20);
    auto m = model(f, 1264, 900);
    TimeMs t = 1000;
    m.pointerMove(600, 700, false, t); // row 5, at base 655..755
    m.advance(t + 59);
    check(!m.enlarged().valid(), "T1: nothing enlarged before the dwell");
    m.advance(t + 60);
    check(m.enlarged().track == 6, "T1: row 5 enlarged after 60 ms");
    m.advance(t + 600);
    const double heights[10] = {100, 100, 100, 110, 129, 190, 129, 110, 100, 100};
    const double gaps[10] = {31, 31, 31, 34, 40, 40, 34, 31, 31, 31};
    const double tops[10] = {0, 131, 262, 393, 537, 706, 936, 1099, 1240, 1371};
    bool h = true, g = true, top = true;
    for (std::size_t i = 0; i < 10; ++i) {
        h = h && m.row(i).height == heights[i];
        g = g && gapBelow(m, i) == gaps[i];
        top = top && contentTopOf(m, i) == tops[i];
    }
    check(h, "T1: heights 100 100 100 110 129 190 129 110 100 100");
    check(g, "T1: gaps 31 31 31 34 40 40 34 31 31 31");
    check(top, "T1: content tops 0 131 262 393 537 706 936 1099 1240 1371");
    check(m.contentHeight() == 2781, "T1: content height 2781");
    check(m.row(5).top == 655, "T1: the promoted row kept its top under the pointer");
    check(m.row(5).tier == Tier::T5 && m.row(4).tier == Tier::T3, "T1: tiers follow the heights");
}

void fisheyeAtEveryDetent() {
    section("T2 the fisheye at every detent");
    struct Row {
        int base, e, d1, d2;
        Tier te, t1, t2, tb;
        int g0, ge1, g12, g2b, pitch;
    };
    const Row table[] = {
        {28, 53, 36, 31, Tier::T1, Tier::T0, Tier::T0, Tier::T0, 9, 11, 10, 9, 37},
        {44, 84, 57, 48, Tier::T2, Tier::T1, Tier::T1, Tier::T1, 14, 18, 15, 14, 58},
        {68, 129, 88, 75, Tier::T3, Tier::T2, Tier::T2, Tier::T2, 21, 27, 23, 21, 89},
        {100, 190, 129, 110, Tier::T5, Tier::T3, Tier::T3, Tier::T3, 31, 40, 34, 31, 131},
        {136, 258, 175, 150, Tier::T5, Tier::T4, Tier::T4, Tier::T4, 42, 54, 47, 42, 178},
        {184, 350, 237, 202, Tier::T5, Tier::T5, Tier::T5, Tier::T5, 57, 73, 63, 57, 241},
    };
    const Constants c;
    for (const auto &r : table) {
        check(fisheyeHeight(r.base, 0, 900, c) == r.e && fisheyeHeight(r.base, 1, 900, c) == r.d1 &&
                  fisheyeHeight(r.base, 2, 900, c) == r.d2 && fisheyeHeight(r.base, 3, 900, c) == r.base,
              "T2: fisheyeHeight at each detent");
        check(gapBetween(r.base, r.base, c) == r.g0 && gapBetween(r.e, r.d1, c) == r.ge1 &&
                  gapBetween(r.d1, r.d2, c) == r.g12 && gapBetween(r.d2, r.base, c) == r.g2b,
              "T2: gapBetween at each detent");
        Fixture f = uniform(20);
        auto m = model(f, 1264, 900);
        TimeMs t = 100;
        m.setBaseHeight(r.base, t);
        m.focusRow(m.rowRef(6), t);
        m.advance(t + 1000);
        check(m.row(5).height == r.e && m.row(4).height == r.d1 && m.row(3).height == r.d2 &&
                  m.row(2).height == r.base,
              "T2: the model's heights at each detent");
        check(m.row(5).tier == r.te && m.row(4).tier == r.t1 && m.row(3).tier == r.t2 && m.row(2).tier == r.tb,
              "T2: the tiers at each detent");
        check(gapBelow(m, 1) == r.g0 && gapBelow(m, 4) == r.ge1 && gapBelow(m, 3) == r.g12 &&
                  gapBelow(m, 2) == r.g2b,
              "T2: the model's gaps at each detent");
        check(contentTopOf(m, 1) == r.pitch, "T2: the base pitch");
    }
}

void capsAndFloor() {
    section("T3 T4 the caps and the floor");
    Fixture f = uniform(9);
    auto m = model(f, 1264, 600);
    TimeMs t = 100;
    m.setBaseHeight(184, t);
    m.focusRow(m.rowRef(5), t);
    m.advance(t + 1000);
    const double heights[9] = {184, 184, 202, 232, 232, 232, 202, 184, 184};
    const double gaps[8] = {57, 57, 63, 72, 72, 63, 57, 57};
    bool h = true, g = true;
    for (std::size_t i = 0; i < 9; ++i)
        h = h && m.row(i).height == heights[i];
    for (std::size_t i = 0; i < 8; ++i)
        g = g && gapBelow(m, i) == gaps[i];
    check(h, "T3: base 184 in 600 px: heights capped at 600 - 368 = 232");
    check(g, "T3: gaps 57 57 63 72 72 63 57 57");
    const Constants c;
    bool atBase = true;
    for (std::size_t d = 0; d < 4; ++d)
        atBase = atBase && fisheyeHeight(100, d, 250, c) == 100;
    check(atBase, "T4: base 100 in 250 px: nothing grows");
    Fixture g250 = uniform(5);
    auto small = model(g250, 1264, 250);
    small.focusRow(small.rowRef(3), 10);
    small.advance(1000);
    check(small.row(2).height == 100 && small.contentHeight() == 5 * 131 - 31, "T4: the model stays at base");
}

void maxRow() {
    section("T5 Max (and m2: its floor, the fixed variant)");
    Fixture f = uniform(20);
    auto m = model(f, 1264, 600);
    TimeMs t = 100;
    m.setKeyboardFocus(true, t);
    m.focusRow(m.rowRef(6), t);
    t += 400;
    m.advance(t);
    auto fx = m.key(Key::Enter, Mods{}, t);
    t += 400;
    m.advance(t);
    check(fx.consumed && m.maximised().track == 6, "T5: Enter maximises the focused row");
    check(m.row(5).height == 538 && m.row(6).height == 100 && m.row(4).height == 100, "T5: Max 538, the rest at base");
    const double tops[8] = {0, 131, 262, 393, 524, 655, 1224, 1355};
    bool tops8 = true;
    for (std::size_t i = 0; i < 8; ++i)
        tops8 = tops8 && contentTopOf(m, i) == tops[i];
    check(tops8, "T5: content tops 0 131 262 393 524 655 1224 1355");
    check(m.row(5).tier == Tier::Max, "T5: the tier is Max at rest");
    check(m.row(5).top == 31, "T5: framed by its two gaps");
    m.pointerMove(600, 300, false, t);
    t += 2000;
    m.advance(t);
    m.scrollBy(400, t);
    m.pointerMove(601, 300, false, t);
    t += 2000;
    m.advance(t);
    check(m.maximised().track == 6 && m.row(5).height == 538, "T5: the pointer dwelling changes nothing");
    m.key(Key::Enter, Mods{}, t);
    t += 400;
    m.advance(t);
    check(!m.maximised().valid() && m.row(5).height != 538, "T5: Enter again restores");
    m.key(Key::Enter, Mods{}, t);
    t += 400;
    m.advance(t);
    fx = m.key(Key::Escape, Mods{}, t);
    t += 400;
    m.advance(t);
    check(fx.escape == EscapeStep::RestoreMax && !m.maximised().valid(), "T5: Esc restores");
    m.key(Key::Enter, Mods{}, t);
    t += 400;
    m.advance(t);
    m.pointerMove(500, 300, false, t);
    press(m, 500, 300, t); // the empty space beside the items
    t += 400;
    m.advance(t);
    check(!m.maximised().valid(), "T5: a press in the row's empty space restores");
    const auto vs = m.viewState();
    check(vs.window.baseHeight == 100, "T5: view state carries no Max");
    // a chip click maximises while the base row is at T0 to T2
    Fixture g = uniform(20);
    auto small = model(g, 1264, 600);
    small.setBaseHeight(44, 10);
    small.pointerMove(10, 5, false, 20);
    pressChip(small, 0, 30);
    check(small.maximised().track == 1, "T5: at base 44 a chip click maximises");
    Fixture h = uniform(20);
    auto big = model(h, 1264, 600);
    big.pointerMove(10, 45, false, 20);
    pressChip(big, 0, 30);
    check(!big.maximised().valid() && big.focus().row.track == 1, "T5: at base 100 a chip click only focuses");
    // maximise is refused during a capture
    const auto cap = big.beginCapture(CaptureKind::Drag, big.moduleRef(2, kQ), 40);
    check(cap.refusal == CaptureRefusal::None, "T5: a capture begins");
    check(big.maximise(big.rowRef(2), 50).refused && !big.maximised().valid(), "T5: Max refused while captured");
    big.endCapture(cap.ref, 60);
    // m2: Max never below base, and the fixed variant
    Fixture k = uniform(5);
    auto tiny = model(k, 1264, 250);
    tiny.setBaseHeight(184, 10);
    tiny.maximise(tiny.rowRef(1), 20);
    tiny.advance(500);
    check(tiny.row(0).height == 184, "m2: Max floors at base (250 - 2 x 57 = 136 would be below it)");
    Constants fixed;
    fixed.maxMode = MaxMode::Fixed;
    fixed.maxFixed = 240;
    Fixture q = uniform(5);
    auto fixedMax = model(q, 1264, 600, fixed);
    fixedMax.maximise(fixedMax.rowRef(2), 20);
    fixedMax.advance(500);
    check(fixedMax.row(1).height == 240, "m2: the fixed variant is 240 high");
}

void tiers() {
    section("T6 tiers");
    const Constants c;
    check(tierFor(43, false, c) == Tier::T0 && tierFor(44, false, c) == Tier::T1 && tierFor(67, false, c) == Tier::T1 &&
              tierFor(68, false, c) == Tier::T2 && tierFor(99, false, c) == Tier::T2 &&
              tierFor(100, false, c) == Tier::T3 && tierFor(135, false, c) == Tier::T3 &&
              tierFor(136, false, c) == Tier::T4 && tierFor(183, false, c) == Tier::T4 &&
              tierFor(184, false, c) == Tier::T5 && tierFor(600, false, c) == Tier::T5,
          "T6: the floors 44 68 100 136 184");
    check(tierFor(30, true, c) == Tier::Max, "T6: maximised is Max");
    Fixture f = uniform(20);
    auto m = model(f, 1264, 900);
    m.focusRow(m.rowRef(6), 0);
    m.advance(40); // mid-animation
    const RowGeo r = m.row(5);
    check(r.animating && r.height > 100 && r.height < 184 && r.tier == tierFor(r.height, false, c) &&
              r.targetTier == Tier::T5,
          "T6: mid-animation the tier follows the height, targetTier the rest");
}

void widths() {
    section("T7 widths (and M8: the reference height scales)");
    const Constants c;
    check(chainItemWidth(ItemClass::Eq, 190, c) == 495 && chainItemWidth(ItemClass::Dynamics, 190, c) == 464 &&
              chainItemWidth(ItemClass::View, 190, c) == 371 && chainItemWidth(ItemClass::Analyser, 190, c) == 433 &&
              chainItemWidth(ItemClass::AdiStill, 190, c) == 557 && chainItemWidth(ItemClass::AdiQ, 190, c) == 619,
          "T7: widths at 190");
    check(chainItemWidth(ItemClass::Dynamics, 129, c) == 357 && chainItemWidth(ItemClass::View, 129, c) == 285 &&
              chainItemWidth(ItemClass::Analyser, 129, c) == 333 && chainItemWidth(ItemClass::Eq, 110, c) == 341,
          "T7: widths at 129 and 110");
    check(chainItemWidth(ItemClass::Eq, 28, c) == 135 && chainItemWidth(ItemClass::View, 28, c) == 120,
          "T7: at 28, EQ 135 and the 120 minimum");
    const auto cols = [&](int base, std::array<int, 5> expected) {
        return columnWidth(ItemClass::AdiQ, true, base, 1, c) == expected[0] &&
               columnWidth(ItemClass::AdiQ, false, base, 1, c) == expected[1] &&
               columnWidth(ItemClass::Ducking, true, base, 1, c) == expected[2] &&
               columnWidth(ItemClass::Clip, true, base, 1, c) == expected[3] &&
               columnWidth(ItemClass::AdiL, true, base, 1, c) == expected[4];
    };
    check(cols(28, {168, 101, 109, 101, 126}) && cols(44, {229, 137, 149, 137, 172}) &&
              cols(68, {308, 185, 200, 185, 231}) && cols(136, {493, 296, 320, 296, 370}) &&
              cols(184, {606, 363, 394, 363, 454}),
          "T7: module columns from the base height only");
    // folding by tier: a plug-in below T3, an analyser below T1, ADI Still below T2
    Fixture f;
    f.add(1, "Fold");
    f.item(1, 11, ItemClass::Plugin, "Plug-in");
    f.item(1, 12, ItemClass::Analyser, "Analyser");
    f.item(1, 13, ItemClass::AdiStill, "ADI Still");
    auto m = model(f, 2560, 600);
    const auto widthsAt = [&](int base, std::vector<std::pair<bool, double>> &out) {
        out.clear();
        m.setBaseHeight(base, 0);
        m.forEachChainItem(0, [&](const ItemGeo &g) { out.emplace_back(g.pill, g.width); });
    };
    std::vector<std::pair<bool, double>> w;
    widthsAt(68, w);
    check(w.size() == 3 && w[0].first && w[0].second == 22 && !w[2].first && w[2].second == 277,
          "T7: at 68 a plug-in is a pill, ADI Still 277");
    widthsAt(28, w);
    check(w.size() == 3 && w[1].first, "T7: at 28 the analyser is a pill");
    widthsAt(44, w);
    check(w.size() == 3 && !w[1].first && w[1].second == 160 && w[2].first, "T7: at 44 the analyser is 160, ADI Still a pill");
    m.setBaseHeight(68, 0);
    m.focusItem(m.chainRef(1, 11), 0);
    m.advance(500);
    m.forEachChainItem(0, [&](const ItemGeo &g) {
        if (g.index == 0)
            check(!g.pill && g.width == 285, "T7: the plug-in in the enlarged row (129, T3) is 285 wide");
    });
    // columns at the same x on every row, the enlarged one included
    Fixture u = uniform(20);
    u.module(3, kQ).state = ModuleState::Engaged;
    auto um = model(u);
    um.focusRow(um.rowRef(6), 0);
    um.advance(500);
    const CellGeo first = um.moduleCell(um.visible().first, 0);
    bool aligned = true;
    um.forEachVisibleRow([&](const RowGeo &r) {
        const CellGeo cell = um.moduleCell(r.shown, 0);
        aligned = aligned && cell.x == first.x && cell.width == first.width;
    });
    check(aligned && first.width == 400, "T7: ADI-Q's column at one x and width on every row");
    // M8: at k = 1.25 the reference height scales with the sizes
    const Constants k = c.scaled(1.25);
    check(chainItemWidth(ItemClass::Eq, 125, k) == 400 && columnWidth(ItemClass::AdiQ, true, 125, 1, k) == 500,
          "M8: at k 1.25 an EQ at base 125 is 400 and ADI-Q's column 500");
}

void widthBudget() {
    section("T8 the width budget (both layouts) and the fit rule");
    const Constants c;
    WindowViewState view;
    const auto none = fourScope(false, false, false, false);
    const auto q = fourScope(true, false, false, false);
    const auto all = fourScope(true, true, true, true);
    const auto lane = [&](const std::array<KindScope, 4> &kinds, double width, const WindowViewState &v) {
        return planProcessor(width, 100, kinds, v, c);
    };
    const double lists[6] = {1264, 1004, 1904, 1644, 2544, 2284};
    const double noneLanes[6] = {660, 400, 1300, 1040, 1940, 1680};
    const double qLanes[6] = {500, 240, 1140, 880, 1780, 1520};
    bool ok = true;
    for (std::size_t i = 0; i < 6; ++i) {
        const auto a = lane(none, lists[i], view), b = lane(q, lists[i], view);
        ok = ok && a.laneW == noneLanes[i] && a.budgetStep == 0 && a.laneMode == LaneMode::Items;
        ok = ok && b.laneW == qLanes[i] && b.budgetStep == 0 && b.laneMode == LaneMode::Items;
    }
    check(ok, "T8: none engaged (fixed 604) and ADI-Q engaged (fixed 764) at every list width");
    const auto s1264 = lane(all, 1264, view);
    check(s1264.budgetStep == 2 && s1264.laneW == 38 && s1264.laneMode == LaneMode::Pills && s1264.navW == 28 &&
              s1264.w[0] == 340 && s1264.w[1] == 221 && s1264.w[2] == 204 && s1264.w[3] == 255,
          "T8: all four at 1264: x 0.85, navigator 28, lane 38 of pills");
    const auto s1004 = lane(all, 1004, view);
    check(s1004.budgetStep == 3 && s1004.laneW == 0 && s1004.shortBy == 222 && s1004.proposeBase &&
              s1004.proposeProcessorFirst,
          "T8: all four at 1004 does not fit: short by 222, both proposals");
    check(lane(all, 1904, view).laneW == 394 && lane(all, 1904, view).laneMode == LaneMode::Items &&
              lane(all, 1644, view).laneW == 134 && lane(all, 1644, view).laneMode == LaneMode::Pills &&
              lane(all, 1644, view).budgetStep == 0 && lane(all, 2544, view).laneW == 1034 &&
              lane(all, 2284, view).laneW == 774,
          "T8: all four at 1904, 1644, 2544, 2284");
    const auto proposed = planProcessor(1004, 68, all, view, c);
    check(proposed.budgetStep == 2 && proposed.w[0] == 262 && proposed.w[1] == 170 && proposed.w[2] == 157 &&
              proposed.w[3] == 196 && proposed.laneW == 13,
          "T8: the proposed base 68 fits at 1004 with a 13 px lane");
    auto clipAbsent = none;
    clipAbsent[2].dormant = false;
    check(lane(clipAbsent, 1264, view).laneW == 686, "T8: Clip absent everywhere: two pills, lane 686");
    WindowViewState hidden;
    hidden.navigator = false;
    check(lane(none, 1264, hidden).laneW == 792 && lane(none, 1264, hidden).navW == 0,
          "T8: the navigator hidden gives its 132 to the lane");
    // the combined card takes its width from the same budget
    WindowViewState card;
    card.moduleLayout = ModuleLayout::Card;
    check(lane(none, 1264, card).laneW == 660 && lane(none, 1264, card).processorW == 324 &&
              lane(q, 1264, card).laneW == 500,
          "B3: card mode, none and ADI-Q engaged: the same budget");
    const auto card1264 = lane(all, 1264, card);
    check(card1264.budgetStep == 2 && card1264.laneW == 50 && card1264.processorW == 1038,
          "B3: card mode, all four at 1264: step 2, lane 50");
    check(lane(all, 1904, card).laneW == 406 && lane(all, 1004, card).budgetStep == 3 &&
              lane(all, 1004, card).shortBy == 210,
          "B3: card mode at 1904 and 1004");
    // the fit rule is a constant (m9): lane >= 0, or lane >= 140
    Constants items = c;
    items.fit = FitRule::LaneShowsItems;
    const auto strict = planProcessor(1644, 100, all, view, items);
    check(strict.budgetStep == 1 && strict.laneW == 314 && strict.laneMode == LaneMode::Items,
          "m9: with LaneShowsItems 1644 steps to x 0.85 and shows items");
}

void positions() {
    section("T9 positions");
    const Constants c;
    WindowViewState view;
    const auto none = planProcessor(1264, 100, fourScope(false, false, false, false), view, c);
    check(none.laneX == 138 && none.laneW == 660 && none.x[0] == 808 && none.w[0] == 240 && none.trayX == 1058 &&
              none.trayW == 74 && none.x[1] == 1058 && none.x[2] == 1084 && none.x[3] == 1110 && none.navX == 1132,
          "T9: none engaged at 1264");
    const auto q = planProcessor(1264, 100, fourScope(true, false, false, false), view, c);
    check(q.laneW == 500 && q.x[0] == 648 && q.w[0] == 400 && q.trayX == 1058, "T9: ADI-Q engaged at 1264");
    const auto all = planProcessor(1904, 100, fourScope(true, true, true, true), view, c);
    check(all.laneW == 394 && all.x[0] == 542 && all.x[1] == 952 && all.x[2] == 1222 && all.x[3] == 1472 &&
              all.navX == 1772,
          "T9: all four at 1904");
    const auto tight = planProcessor(1264, 100, fourScope(true, true, true, true), view, c);
    check(tight.laneW == 38 && tight.x[0] == 186 && tight.x[1] == 536 && tight.x[2] == 767 && tight.x[3] == 981 &&
              tight.x[3] + tight.w[3] == 1236 && tight.navX == 1236,
          "T9: all four at 1264");
    WindowViewState first;
    first.layout = RowLayout::ProcessorFirst;
    const auto pf = planProcessor(1264, 100, fourScope(false, false, false, false), first, c);
    check(pf.x[0] == 138 && pf.trayX == 388 && pf.laneX == 472 && pf.laneW == 660, "T9: processor first, none engaged");
}

void cells() {
    section("T10 cells (and B3's card twin, m8's refused cells)");
    Fixture f = eleven();
    auto m = model(f);
    const auto &p = m.plan();
    check(p.x[0] == 524 && p.w[0] == 240 && p.x[3] == 774 && p.w[3] == 300 && p.x[1] == 1084 && p.x[2] == 1110 &&
              p.laneW == 376,
          "T10: ADI-Q's dormant column, ADI-L's column, two pills, lane 376");
    bool previews = true;
    for (std::size_t i = 0; i < m.shownCount(); ++i)
        previews = previews && m.moduleCell(i, 0).content == CellContent::DormantPreview;
    check(previews, "T10: ADI-Q's dim preview on every row");
    check(m.moduleCell(3, 3).content == CellContent::Item && m.moduleCell(2, 3).content == CellContent::DormantPill,
          "T10: ADI-L engaged on track 4, a pill elsewhere in its column");
    check(m.moduleCell(10, 1).content == CellContent::Refused && m.moduleCell(10, 1).x == 1084 &&
              m.moduleCell(0, 1).content == CellContent::DormantPill,
          "m8: Main's ducking drawn refused in the tray");
    f.module(3, kQ).state = ModuleState::Engaged;
    f.generation = 2;
    m.setRows(f.input(), 10);
    check(m.plan().x[0] == 364 && m.plan().w[0] == 400 && m.plan().laneW == 216, "T10: ADI-Q engaged on track 3");
    check(m.moduleCell(0, 0).content == CellContent::DormantPreview && m.moduleCell(2, 0).content == CellContent::Item,
          "T10: the other rows show ADI-Q's preview in its column");
    m.setSearch("kick", 20);
    check(shownIds(m) == std::vector<TrackId>{1, 2} && m.plan().x[3] == 774 && m.plan().x[0] == 364,
          "T10: a search leaves the columns to the scope");
    m.setSearch("", 30);
    m.setShowDormant(false, 40);
    check(m.plan().trayW == 0 && m.moduleCell(1, 3).content == CellContent::Blank &&
              m.moduleCell(1, 1).content == CellContent::Hidden,
          "T10: dormant hidden: no tray, blank cells in a column");
    f.module(3, kQ).state = ModuleState::Dormant;
    f.generation = 3;
    m.setRows(f.input(), 50);
    check(m.plan().place[0] == ModulePlace::Hidden, "T10: with nothing engaged ADI-Q's column goes too");
    m.setShowDormant(true, 60);
    m.setShowChain(false, 70);
    check(m.plan().laneMode == LaneMode::None && m.plan().x[0] == 138, "T10: chain hidden: no lane, no lane gap");
    // B3: the card
    Fixture g = eleven();
    auto card = model(g);
    card.setModuleLayout(ModuleLayout::Card, 10);
    const auto &cp = card.plan();
    check(cp.processorX == 530 && cp.processorW == 602 && cp.laneW == 382, "B3: the card at 530, 602 wide");
    const auto cellAt = [&](std::size_t shown, std::size_t k) { return card.moduleCell(shown, k); };
    check(cellAt(3, 0).x == 530 && cellAt(3, 0).width == 240 && cellAt(3, 1).x == 776 && cellAt(3, 2).x == 804 &&
              cellAt(3, 3).x == 832 && cellAt(3, 3).width == 300 && cellAt(3, 3).content == CellContent::Item,
          "B3: track 4's card: preview, two pills, ADI-L at its width, in the input's order");
    check(cellAt(2, 3).x == 832 && cellAt(2, 3).width == 22 && cellAt(2, 3).content == CellContent::DormantPill,
          "B3: track 3's ADI-L a pill in the card");
    check(cellAt(10, 1).content == CellContent::Refused && cellAt(10, 1).x == 776,
          "B3: Main's ducking refused in the card");
    check(cellAt(3, 0).place == ModulePlace::Card, "B3: card cells say so");
}

void itemFocusGeometry() {
    section("Item focus: stable target, independent geometry and edit retention");
    Fixture f; f.kinds.clear();
    for (TrackId id = 1; id <= 4; ++id) f.add(id, "Tool channel");
    f.item(1, 11, ItemClass::Eq, "EQ");
    f.item(1, 12, ItemClass::AdiL, "Limiter");
    f.item(1, 13, ItemClass::Plugin, "Ordinary");
    const auto item = [](StackModel &m, DeviceId id) {
        ItemGeo result;
        if (const auto row = m.shownIndex(1))
            m.forEachChainItem(*row, [&](const ItemGeo &g) { if (g.ref.device == id) result = g; });
        return result;
    };
    auto m = model(f, 1500, 700);
    const auto a = item(m, 11), b = item(m, 12);
    const double px = a.x + 120, py = a.top + 20;
    const auto hover = m.pointerMove(px, py, false, 0);
    m.advance(59);
    check(!m.emphasizedItem().row.valid() && !hover.select, "item dwell neither selects nor promotes early");
    m.advance(60); m.advance(220);
    const auto enlarged = item(m, 11), compact = item(m, 12);
    check(m.emphasizedItem() == m.chainRef(1, 11) && enlarged.height == 190 && enlarged.width == 495 &&
          compact.height == 100 && compact.width == b.width && item(m, 13).height == 100,
          "only inspected tool grows; siblings retain compact height, width and tier");
    check(m.hitTest(compact.x + 20, compact.top + 130).part == HitPart::RowSpace &&
          m.hitTest(enlarged.x + 20, enlarged.top + 130).item == enlarged.ref,
          "item hit geometry includes independent vertical bounds, leaving real space below compact siblings");
    for (TimeMs t = 221; t < 500; t += 7) { m.pointerMove(px, py, false, t); m.advance(t); }
    check(m.emphasizedItem() == enlarged.ref, "same-coordinate synthetic moves and own geometry never transfer item ownership");
    m.focusItem(m.chainRef(1, 12), 510); m.advance(700);
    m.pointerMove(px, py, false, 710);
    check(m.emphasizedItem() == m.chainRef(1, 12), "keyboard target wins over an unmoved pointer");
    m.pointerMove(px + 1, py, false, 720); m.advance(779);
    check(m.emphasizedItem() == m.chainRef(1, 12), "real handback to old hover candidate restarts a full item dwell");
    m.advance(780);
    check(m.emphasizedItem() == m.chainRef(1, 11), "real motion reclaims item emphasis at its new dwell boundary");
    m.pointerMove(-5, -5, false, 790); m.pointerMove(px, py, false, 800);
    m.focusItem(m.chainRef(1, 12), 810); m.pointerMove(px + 1, py, false, 820); m.advance(850);
    const auto held = item(m, 12);
    const auto dragCapture = m.beginCapture(CaptureKind::Drag, m.chainRef(1, 12), 850);
    m.advance(1800); m.setViewport(600, 400, 1801);
    check(dragCapture.refusal == CaptureRefusal::None && item(m, 12) == held,
          "drag freezes complete intermediate item rect/tier through time and viewport resize");
    std::swap(f.chains[0][0], f.chains[0][2]); ++f.generation; m.setRows(f.input(), 1802);
    m.setSearch("nothing", 1803);
    check(item(m, 12) == held && m.capture().has_value(), "reorder and filtering retain captured item rectangle and identity");
    m.endCapture(dragCapture.ref, 1810); m.setSearch("", 1820); m.advance(2100);
    TimeMs editTime = 2200;
    for (const auto kind : {CaptureKind::Text, CaptureKind::Menu}) {
        m.focusItem(m.chainRef(1, 12), editTime); m.advance(editTime + 60);
        const auto before = item(m, 12);
        const auto capture = m.beginCapture(kind, m.chainRef(1, 12), editTime + 60);
        m.advance(editTime + 300);
        check(capture.refusal == CaptureRefusal::None && item(m, 12) == before,
              "text and asynchronous menu captures freeze the same full geometry as a drag");
        m.endCapture(capture.ref, editTime + 310);
        editTime += 400;
    }
    m.setViewport(1500, 700, 3100); m.focusItem(m.chainRef(1, 12), 3110); m.advance(3300);
    f.chains[0][1].identity += 123; ++f.generation; m.setRows(f.input(), 3310);
    check(m.emphasizedItem() != held.ref, "same-ID replacement invalidates item emphasis without a row membership change");
    // Exercise frame schedules against exact common event times, including a mid-animation retarget.
    struct ItemTrace {
        ItemRef target;
        std::array<ItemGeo, 3> items;
        LaneGeo lane;
        std::optional<CaptureRef> capture;
        bool operator==(const ItemTrace &) const = default;
    };
    const auto trace = [&](int rate) {
        auto x = model(f, 1500, 700);
        std::vector<ItemTrace> samples;
        std::int64_t frame = 1;
        const auto to = [&](TimeMs until) {
            if (rate > 0) while (frame * 1000 / rate < until) x.advance(frame++ * 1000 / rate);
            x.advance(until);
            ItemTrace state{x.emphasizedItem(), {}, x.lane(*x.shownIndex(1)), x.capture()};
            for (std::size_t j = 0; j < state.items.size(); ++j)
                state.items[j] = item(x, static_cast<DeviceId>(11 + j));
            samples.push_back(state);
        };
        const auto first = item(x, 11);
        x.pointerMove(first.x + 30, first.top + 10, false, 0);
        to(61); to(110); x.focusItem(x.chainRef(1, 12), 110);
        to(130); to(173); x.focusItem(x.chainRef(1, 13), 173);
        to(200); const auto capture = x.beginCapture(CaptureKind::Text, x.chainRef(1, 13), 200);
        to(250); x.endCapture(capture.ref, 270); to(310);
        to(410); x.pointerMove(first.x + 31, first.top + 10, false, 410);
        to(440); to(500); to(900);
        return samples;
    };
    const auto expected = trace(0);
    check(trace(30) == expected && trace(60) == expected && trace(144) == expected,
          "30/60/144 Hz and event-only schedules give identical full item rects, tiers, clipping and lane anchors");
    for (const auto kind : {CaptureKind::Drag, CaptureKind::Text, CaptureKind::Menu}) {
        auto posed = model(f, 1500, 700);
        const auto target = posed.chainRef(1, 12);
        posed.focusItem(target, 0); posed.advance(80);
        const auto rowPose = posed.row(*posed.shownIndex(1));
        const auto lanePose = posed.lane(*posed.shownIndex(1));
        std::vector<ItemGeo> items;
        posed.forEachChainItem(*posed.shownIndex(1), [&](const auto &g) { items.push_back(g); });
        const auto before = item(posed, 12);
        posed.focusItem(target, 87); // child focus advanced unseen geometry seven ms after the frame
        const auto captured = posed.beginCapturePresented(kind, target, rowPose, lanePose, items, 87);
        posed.advance(160);
        check(captured.refusal == CaptureRefusal::None && item(posed, 12) == before &&
              posed.row(*posed.shownIndex(1)).height == rowPose.height,
              "presented-pose capture retains the visible t rectangle rather than the unseen t+7 pose");
        posed.endCapture(captured.ref, 170);
        const auto released = item(posed, 12);
        check(released.x == before.x && released.top == before.top && released.width == before.width &&
              released.height == before.height, "first release frame resumes from the displayed pose without an item jump");
        posed.advance(400);
        check(item(posed, 12).height == 190, "release completes the existing easing from the retained displayed pose");
    }
    {
        auto posed = model(f, 1500, 700);
        posed.focusItem(posed.chainRef(1, 12), 0); posed.advance(220);
        const auto rowPose = posed.row(*posed.shownIndex(1));
        const auto lanePose = posed.lane(*posed.shownIndex(1));
        std::vector<ItemGeo> items;
        posed.forEachChainItem(*posed.shownIndex(1), [&](const auto &g) { items.push_back(g); });
        const auto shownSibling = item(posed, 12);
        posed.focusItem(posed.chainRef(1, 13), 230); posed.advance(450); // old sibling entry is now retired
        const auto target = posed.chainRef(1, 11);
        posed.focusItem(target, 460);
        const auto cap = posed.beginCapturePresented(CaptureKind::Text, target, rowPose, lanePose, items, 470);
        posed.endCapture(cap.ref, 480);
        check(cap.refusal == CaptureRefusal::None && item(posed, 12).height == shownSibling.height &&
              item(posed, 12).x == shownSibling.x,
              "release reconstructs a displayed sibling weight even if unpresented retargeting already retired its animation slot");
        posed.advance(700);
        check(item(posed, 12).height == 100 && item(posed, 11).height == 190,
              "reconstructed sibling shrinks smoothly while the current edit target grows");
    }
    Fixture longChain; longChain.kinds.clear();
    longChain.add(1, "Long chain"); longChain.add(2, "Other"); longChain.add(3, "Other");
    for (DeviceId id = 11; id < 17; ++id) longChain.item(1, id, ItemClass::Eq, "EQ");
    {
        auto posed = model(longChain, 800, 700);
        posed.scrollLane(posed.rowRef(1), 5, 0); posed.advance(60);
        const auto rowPose = posed.row(*posed.shownIndex(1));
        const auto lanePose = posed.lane(*posed.shownIndex(1));
        std::vector<ItemGeo> items;
        posed.forEachChainItem(*posed.shownIndex(1), [&](const auto &g) { items.push_back(g); });
        const auto before = item(posed, 16);
        posed.focusItem(posed.chainRef(1, 16), 67);
        const auto cap = posed.beginCapturePresented(CaptureKind::Menu, posed.chainRef(1, 16), rowPose, lanePose, items, 67);
        posed.endCapture(cap.ref, 80);
        check(cap.refusal == CaptureRefusal::None && item(posed, 16).x == before.x &&
              posed.lane(*posed.shownIndex(1)).offset == lanePose.offset,
              "release retains a displayed end-of-lane pose with legitimate blank slack instead of clamping it into a jump");
    }
    auto anchored = model(longChain, 800, 700);
    auto first = item(anchored, 11);
    anchored.pointerMove(first.x + 250, first.top + 20, false, 0); anchored.advance(220);
    const auto anchoredLane = anchored.lane(*anchored.shownIndex(1));
    check(anchoredLane.first == 0 && anchoredLane.left && anchoredLane.offset > 0,
          "interior pointer anchoring can expose a left arrow while the logical first item is zero");
    anchored.pointerDown(anchoredLane.leftChevron.x + 4, anchoredLane.leftChevron.y + 4, Button::Left, {}, 1, 230);
    anchored.pointerUp(anchoredLane.leftChevron.x + 4, anchoredLane.leftChevron.y + 4, 231);
    anchored.advance(351);
    check(anchored.lane(*anchored.shownIndex(1)).offset == 0,
          "visible left arrow clears pointer anchoring and reaches the first item even from logical zero");
    first = item(anchored, 11);
    anchored.pointerMove(first.x + 251, first.top + 20, false, 360); anchored.advance(600);
    const auto beforeRight = anchored.lane(*anchored.shownIndex(1)).offset;
    anchored.scrollLane(anchored.rowRef(1), 1, 610); anchored.advance(670);
    check(anchored.lane(*anchored.shownIndex(1)).offset >= beforeRight,
          "explicit right navigation starts at the actual pixel pose and never initially jumps backward");
    anchored.advance(800); anchored.focusItem(anchored.chainRef(1, 16), 810); anchored.advance(1100);
    const auto last = item(anchored, 16);
    check(last.ref == anchored.chainRef(1, 16) && !anchored.lane(*anchored.shownIndex(1)).right,
          "rightmost focused device is reachable and has no inoperative next-device arrow");
    anchored.setViewport(500, 700, 1110); anchored.advance(1300);
    check(item(anchored, 16).clipped && item(anchored, 16).width > anchored.lane(*anchored.shownIndex(1)).width,
          "an oversized inspected tool retains its actual size with honest viewport clipping");
    anchored.setViewport(1700, 700, 1310); anchored.setAutoZoom(false, 1320); anchored.advance(1500);
    const auto viewState = anchored.viewState(); anchored.applyViewState(viewState, 1510); anchored.advance(1800);
    check(anchored.emphasizedItem() == anchored.chainRef(1, 16),
          "AutoZoom-off view restore derives emphasis from current focus rather than serializing transient item state");
    const auto countBefore = allocations.load();
    audit = true;
    for (TimeMs t = 2000; t < 2300; t += 10) {
        anchored.focusItem(anchored.chainRef(1, (t / 10) % 6 + 11), t);
        anchored.advance(t + 5);
        anchored.forEachChainItem(*anchored.shownIndex(1), [](const auto &) {});
    }
    audit = false;
    check(allocations.load() == countBefore, "item retargeting, easing and geometry queries allocate nothing after setRows");
    std::array<ItemGeo, 8> poseItems{};
    std::size_t poseCount = 0;
    const auto poseRow = anchored.row(*anchored.shownIndex(1));
    const auto poseLane = anchored.lane(*anchored.shownIndex(1));
    anchored.forEachChainItem(*anchored.shownIndex(1), [&](const auto &g) {
        if (poseCount < poseItems.size()) poseItems[poseCount++] = g;
    });
    const auto captureAllocations = allocations.load();
    audit = true;
    const auto retained = anchored.beginCapturePresented(CaptureKind::Menu, poseItems[0].ref, poseRow, poseLane,
        std::span<const ItemGeo>(poseItems.data(), poseCount), 2400);
    anchored.endCapture(retained.ref, 2410);
    audit = false;
    check(poseCount > 0 && retained.refusal == CaptureRefusal::None && allocations.load() == captureAllocations,
          "presented-pose admission, bounded weight reconstruction and release allocate nothing after setRows");
}

void laneAndChevrons() {
    section("T11 the lane and its chevrons");
    Fixture f = laneFixture();
    auto m = model(f);
    LaneGeo l = m.lane(0);
    check(l.x == 138 && l.width == 660 && l.mode == LaneMode::Items, "T11: the lane [138, 798)");
    std::vector<ItemGeo> items;
    const auto collect = [&] {
        items.clear();
        m.forEachChainItem(0, [&](const ItemGeo &g) { items.push_back(g); });
    };
    collect();
    const double top = m.row(0).top;
    check(items.size() == 2 && items[0].x == 138 && items[0].width == 320 && items[1].x == 468 &&
              items[1].width == 300,
          "T11: at 100 two items fit");
    check(l.right && l.rightChevron == Rect{748, top + 38, 20, 24} && !l.left, "T11: the right chevron on the last");
    TimeMs t = 1000;
    m.scrollLane(m.rowRef(1), 1, t);
    m.advance(t + 60);
    check(m.lane(0).offset == 288.75, "T11: at 60 ms the lane is 288.75 in (330 x 0.875)");
    m.advance(t + 120);
    l = m.lane(0);
    collect();
    check(l.offset == 330 && l.first == 1 && items.size() == 2 && items[0].x == 138 && items[0].width == 300 &&
              items[1].x == 448 && items[1].width == 240,
          "T11: at 120 ms the second item leads");
    check(l.right && l.rightChevron.x == 668 && l.left && l.leftChevron == Rect{138, top + 38, 20, 24},
          "T11: both chevrons once scrolled");
    m.scrollLane(m.rowRef(1), -1, t + 200);
    m.focusItem(m.chainRef(1, 101), t + 400);
    m.advance(t + 1000);
    l = m.lane(0);
    collect();
    const double topE = m.row(0).top;
    check(items.size() == 1 && items[0].width == 495 && l.rightChevron == Rect{613, topE + 83, 20, 24},
          "T11: enlarged (190): one item, 495 wide");
    WheelDelta notch;
    notch.notches = 1;
    Mods shift;
    shift.shift = true;
    const double y = m.row(0).top + 50;
    m.wheel(600, y, notch, shift, t + 2000);
    check(m.lane(0).first == 1, "T11: Shift+wheel one notch moves one item");
    WheelDelta pad;
    pad.pixelsX = 60;
    m.wheel(600, y, pad, shift, t + 2200);
    check(m.lane(0).first == 2, "T11: 60 px of trackpad moves one item");
    pad.pixelsX = 59;
    m.wheel(600, y, pad, shift, t + 2400);
    check(m.lane(0).first == 2, "T11: 59 px does not");
    // a press on the chevron scrolls the lane (auto-zoom off, so nothing grows under it)
    Fixture g = laneFixture();
    auto c = model(g);
    c.setAutoZoom(false, 10);
    const LaneGeo cl = c.lane(0);
    press(c, cl.rightChevron.x + 5, cl.rightChevron.y + 5, 20);
    check(c.lane(0).first == 1, "T11: a click on the chevron scrolls one item");
}

void rowsAndScope() {
    section("T12 rows and scope");
    Fixture f = eleven();
    auto m = model(f);
    check(shownIds(m) == std::vector<TrackId>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
          "T12: the input's order, the Reference never a row, Main last");
    check(m.row(8).depth == 1 && m.row(9).depth == 1 && m.row(7).group, "T12: the group's children indented");
    f.track(6).inView = false;
    f.generation = 2;
    m.setRows(f.input(), 10);
    check(!m.shownIndex(6).has_value() && m.shownCount() == 10, "T12: a track the project view hides is no row");
    f.track(6).inView = true;
    f.generation = 3;
    m.setRows(f.input(), 20);
    const double groupTop = m.row(7).top;
    m.toggleFold(m.rowRef(8), 30);
    check(!m.shownIndex(9) && !m.shownIndex(10) && m.row(7).top == groupTop && m.row(7).folded,
          "T12: a fold hides 9 and 10, the group keeps its place");
    m.toggleFold(m.rowRef(8), 40);
    check(m.shownIndex(9).has_value() && m.shownIndex(10).has_value(), "T12: unfolded again");
    std::swap(f.tracks[4], f.tracks[5]);
    std::swap(f.chains[4], f.chains[5]);
    std::swap_ranges(f.modules.begin() + 16, f.modules.begin() + 20, f.modules.begin() + 20);
    f.generation = 4;
    m.setRows(f.input(), 50);
    check(m.shownTrack(4) == 6 && m.shownTrack(5) == 5, "T12: the order is the input's");
    Fixture bare;
    bare.add(1, "Nothing");
    for (auto key : {kQ, kDuck, kClip, kL})
        bare.module(1, key) = ModuleInput{};
    auto b = model(bare);
    check(b.shownCount() == 1, "T12: a track with nothing on it is still a row");
}

// --- hover --------------------------------------------------------------------------------------------

void dwell() {
    section("T13 the dwell");
    Fixture f = uniform(40);
    auto m = model(f);
    TimeMs t = 100;
    restWith(m, 2, 0, t);
    check(contentTopOf(m, 1) == 144 && contentTopOf(m, 2) == 313 && contentTopOf(m, 3) == 543 &&
              contentTopOf(m, 4) == 706 && m.scroll() == 0,
          "T13: row 2 enlarged at rest, scroll 0");
    m.pointerMove(600, 560, false, 1000);
    m.advance(1059);
    check(m.enlarged().track == 3, "T13: still row 2 at 59 ms");
    m.advance(1060);
    check(m.enlarged().track == 4, "T13: row 3 at 60 ms");
    Fixture g = uniform(40);
    auto r = model(g);
    t = 100;
    restWith(r, 2, 0, t);
    r.pointerMove(600, 560, false, 1000);
    r.pointerMove(600, 400, false, 1030);
    r.pointerMove(600, 561, false, 1050);
    r.advance(1109);
    check(r.enlarged().track == 3, "T13: a re-entry restarts the dwell (not at 1060)");
    r.advance(1110);
    check(r.enlarged().track == 4, "T13: promoted 60 ms after the re-entry");
    Fixture h = uniform(40);
    auto gap = model(h);
    t = 100;
    restWith(gap, 2, 0, t);
    gap.pointerMove(600, 520, false, 1000); // row 2's gap, 503 to 543
    gap.advance(3000);
    check(gap.enlarged().track == 3 && !gap.pointerRow().valid(), "T13: a gap is no row");
    // the dwell runs from the first entry: moves inside the same row do not restart it
    Fixture i = uniform(40);
    auto inside = model(i);
    t = 100;
    restWith(inside, 2, 0, t);
    inside.pointerMove(600, 560, false, 1000); // row 3, 543 to 672
    inside.pointerMove(600, 562, false, 1020);
    inside.pointerMove(601, 563, false, 1040);
    inside.advance(1059);
    check(inside.enlarged().track == 3 && inside.pointerRow().track == 4, "T13: still row 2 at 59 ms after the entry");
    inside.advance(1060);
    check(inside.enlarged().track == 4, "T13: row 3 at 60 ms after the entry, not after the last move");
    // the same for the hold-off: a jiggle after the notch does not push the promotion past 120 ms
    Fixture j = uniform(40);
    auto jiggle = model(j);
    t = 100;
    restWith(jiggle, 2, 0, t);
    jiggle.pointerMove(600, 450, false, 900);
    WheelDelta notch;
    notch.notches = 1;
    jiggle.wheel(600, 450, notch, Mods{}, 1000);
    jiggle.pointerMove(600, 452, false, 1050);
    jiggle.pointerMove(601, 452, false, 1090);
    jiggle.advance(1119);
    check(jiggle.enlarged().track == 3 && jiggle.pointerRow().track == 4, "T13: still row 2 at 119 ms after the notch");
    jiggle.advance(1120);
    check(jiggle.enlarged().track == 4, "T13: the pointer's row at 120 ms after the notch, the jiggle changed nothing");
    // nothing is promoted while a button is down; the dwell counts from the release
    Fixture u = uniform(40);
    auto held = model(u);
    held.pointerMove(600, 300, false, 100);
    held.advance(1000);
    const TrackId a = held.enlarged().track;
    held.pointerDown(600, 300, Button::Left, Mods{}, 1, 1000);
    held.pointerMove(600, 560, true, 1050);
    held.advance(1600);
    const TrackId b = held.pointerRow().track;
    check(a != 0 && b != 0 && b != a && held.enlarged().track == a, "T13: a drag into another row (no capture) promotes nothing while the button is down");
    held.pointerUp(600, 560, 1600);
    held.advance(1659);
    check(held.enlarged().track == a, "T13: ... nor at once on the release");
    held.advance(1660);
    check(held.enlarged().track == b, "T13: ... the row under the pointer grows 60 ms after the release");
    // a jump moving the rows under a still pointer: the dwell counts from where they settle
    Fixture k = uniform(40);
    auto jump = model(k);
    jump.setSearchMode(SearchMode::Jump, 0);
    jump.pointerMove(600, 300, false, 990);
    jump.wheel(600, 300, notch, Mods{}, 1000); // another row under the pointer: hold-off until 1120
    jump.setSearch("track 30", 1050);          // a 140 ms jump to track 30, until 1190
    jump.advance(1120);
    check(!jump.enlarged().valid() && jump.jumpTarget().track == 30, "T13: no promotion while the jump moves");
    jump.advance(1190);
    check(jump.row(29).top == 8 && !jump.enlarged().valid(), "T13: the jump arrived, nothing enlarged yet");
    const TrackId under = jump.pointerRow().track;
    jump.advance(1249);
    check(under != 0 && !jump.enlarged().valid(), "T13: the row that arrived under the pointer waits its own dwell");
    jump.advance(1250);
    check(jump.enlarged().track == under, "T13: and grows 60 ms after the arrival");
}

void keyboardAtOnce() {
    section("T14 keys enlarge at once and reveal (M6: with the pointer resting in the list)");
    const auto start = [](StackModel &m, TimeMs &t) {
        restWith(m, 3, 0, t);
        m.scrollTo(34, t);
        t += 400;
        m.advance(t);
    };
    Fixture f = uniform(40);
    auto m = model(f);
    TimeMs t = 100;
    start(m, t);
    check(m.row(3).top == 410 && m.scroll() == 34, "T14: row 3 at 410 to 600");
    auto fx = m.key(Key::Down, Mods{}, 2000);
    check(m.enlarged().track == 5 && fx.select == 5 && m.row(4).top == 640, "T14: Down: row 4 enlarged at once");
    m.advance(2080);
    const double mid = m.row(4).top;
    m.advance(2160);
    check(mid < 640 && mid > 410 && m.row(4).top == 410 && m.scroll() == 165, "T14: revealed to 410, scroll 165");
    Fixture g = uniform(40);
    auto up = model(g);
    t = 100;
    start(up, t);
    up.key(Key::Up, Mods{}, 2000);
    up.advance(2080);
    const double held = up.row(2).top;
    up.advance(2200);
    check(held == 241 && up.row(2).top == 241 && up.scroll() == 72, "T14: Up: row 2 held at 241, scroll 72");
    // M6: Down with the pointer resting in another row keeps the keys' row until the pointer moves
    Fixture h = uniform(40);
    auto rest = model(h);
    rest.pointerMove(600, 560, false, 100);
    rest.advance(1000);
    const TrackId hovered = rest.enlarged().track;
    rest.key(Key::Down, Mods{}, 1100);
    const TrackId keyed = rest.enlarged().track;
    rest.advance(4000);
    check(keyed != hovered && rest.enlarged().track == keyed && rest.keyboardOwnsEnlarged(),
          "M6: the keys' row stays while the pointer rests");
    rest.pointerMove(601, 560, false, 4000);
    rest.advance(4059);
    check(rest.enlarged().track == keyed, "M6: a move hands it back only after a fresh dwell");
    rest.advance(4060);
    check(rest.enlarged().track == rest.pointerRow().track && rest.enlarged().track != keyed,
          "M6: then the pointer's row grows");
    // M6: the navigator is inside the list but over no row: no promotion, no leave timer
    rest.pointerMove(1200, 300, false, 5000);
    const TrackId before = rest.enlarged().track;
    rest.advance(8000);
    check(rest.enlarged().track == before && rest.nextDeadline() == kNever, "M6: over the navigator nothing changes");
}

void anchor() {
    section("T15 the anchor and the bottom reveal (m4)");
    Fixture f = uniform(40);
    auto m = model(f);
    TimeMs t = 100;
    restWith(m, 12, 1310, t);
    check(m.row(11).top == 144 && m.row(12).top == 313, "T15: rows 11 and 12 at 144 and 313");
    m.pointerMove(600, 200, false, 1000);
    m.advance(1060);
    check(m.enlarged().track == 12, "T15: row 11 promoted at 60 ms");
    bool held = true;
    for (TimeMs s = 1060; s <= 1240; s += 4) {
        m.advance(s);
        held = held && near(m.row(11).top, 144);
    }
    check(held && m.scroll() == 1348, "T15: row 11 held at 144 through the animation, scroll 1348");
    Fixture g = uniform(40);
    auto b = model(g);
    t = 100;
    restWith(b, 12, 1310, t);
    b.pointerMove(600, 560, false, 1000);
    bool still = true;
    for (TimeMs s = 1000; s <= 1220; s += 4) {
        b.advance(s);
        still = still && near(b.row(13).top, 543);
    }
    check(still && b.scroll() == 1211, "T15: row 13 held at 543, scroll 1211 at the end");
    b.advance(1400);
    check(b.row(13).top == 410 && b.scroll() == 1344 && b.pointerRow().track == 14,
          "T15: then revealed to 410 (scroll 1344), the pointer still in it");
    // the non-default anchor: the pointer's fraction of its row holds still, not the row's top
    Constants fractionC;
    fractionC.anchor = Anchor::PointerRowFraction;
    Fixture h = uniform(40);
    auto fr = model(h, 1264, 600, fractionC);
    t = 100;
    restWith(fr, 2, 0, t);
    fr.pointerMove(600, 560, false, 1000); // row 3 at 543, 129 tall: the pointer at 17/129 of it
    const double fraction = 17.0 / 129.0;
    bool kept = true, moved = false;
    for (TimeMs s = 1060; s <= 1220; s += 4) {
        fr.advance(s);
        const RowGeo r = fr.row(3);
        kept = kept && near((560 - r.top) / r.height, fraction, 1e-9);
        moved = moved || !near(r.top, 543);
    }
    check(kept && moved && near(fr.row(3).top, 560 - fraction * 190, 1e-9),
          "T15: PointerRowFraction keeps the pointer's fraction of the row (its top moves) until the reveal");
}

void topSlack() {
    section("T16 M5: the pointer's row is held at the top too");
    Fixture f = uniform(40);
    auto m = model(f);
    TimeMs t = 100;
    restWith(m, 2, 0, t);
    m.pointerMove(600, 560, false, 1000);
    bool inRow = true, held = true;
    for (TimeMs s = 1060; s <= 1220; s += 4) {
        m.advance(s);
        inRow = inRow && m.pointerRow().track == 4;
        held = held && near(m.row(3).top, 543);
    }
    check(held && near(m.scroll(), -99), "M5: row 3 held at 543 (scroll -99), never slid up");
    m.advance(1500);
    check(inRow && m.row(3).top == 410 && m.scroll() == 34 && m.pointerRow().track == 4,
          "M5: the reveal leaves it at 410 (scroll 34), the pointer in it throughout");
}

void noScrollWithButton() {
    section("T17 no reveal while a button is down");
    Fixture f = uniform(40);
    auto m = model(f);
    TimeMs t = 100;
    restWith(m, 12, 1310, t);
    m.pointerMove(500, 560, false, 1000);
    m.advance(1100); // promoted at 1060, the heights moving until 1220
    m.pointerDown(500, 560, Button::Left, Mods{}, 1, 1100);
    m.advance(1300);
    check(m.row(13).top == 543 && m.scroll() == 1211, "T17: held at 543 with the button down");
    m.pointerUp(500, 560, 1400);
    m.advance(1700);
    check(m.row(13).top == 410 && m.scroll() == 1344, "T17: revealed after the release");
    // a press during the reveal stops it where it is; the release finishes it
    Fixture g = uniform(40);
    auto mid = model(g);
    t = 100;
    restWith(mid, 12, 1310, t);
    mid.pointerMove(500, 560, false, 1000);
    mid.advance(1250); // the reveal runs 1220 to 1340
    const double stopped = mid.row(13).top;
    mid.pointerDown(500, 560, Button::Left, Mods{}, 1, 1250);
    mid.advance(1300);
    check(stopped < 543 && stopped > 410 && mid.row(13).top == stopped && !mid.busy(),
          "T17: a press during the reveal freezes the scroll where it is");
    mid.pointerUp(500, 560, 1400);
    mid.advance(1700);
    check(mid.row(13).top == 410 && mid.scroll() == 1344, "T17: ... and the release finishes the reveal");
    // a release the owner reports only through pointerMove runs the reveal then, not at a later click
    Fixture h = uniform(40);
    auto viaMove = model(h);
    t = 100;
    restWith(viaMove, 12, 1310, t);
    viaMove.pointerMove(500, 560, false, 1000);
    viaMove.advance(1100);
    viaMove.pointerDown(500, 560, Button::Left, Mods{}, 1, 1100);
    viaMove.advance(1300);
    viaMove.pointerMove(500, 560, false, 1400);
    viaMove.advance(1700);
    check(viaMove.row(13).top == 410 && viaMove.scroll() == 1344, "T17: a release reported by a move reveals then");
}

void stillPointerEverywhere() {
    section("T18 M5: a still pointer, at every detent and list height");
    const int bases[6] = {28, 44, 68, 100, 136, 184};
    const double heights[4] = {300, 600, 900, 1200};
    std::size_t cases = 0, bad = 0, twice = 0, back = 0, outside = 0;
    for (const int base : bases)
        for (const double height : heights) {
            Fixture f = uniform(40);
            auto m = model(f, 1264, height);
            TimeMs t = 100;
            m.setBaseHeight(base, t);
            m.setKeyboardFocus(true, t);
            const double pitch = base + gapBetween(base, base, m.constants());
            const double scrolls[3] = {0, 13, 10 * pitch + 3};
            int flip = 0;
            for (const double scroll : scrolls)
                for (const std::size_t e : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}})
                    for (double y = 2; y < height; y += 15) {
                        m.focusRow(m.rowRef(m.shownTrack(e)), t);
                        t += 400;
                        m.advance(t);
                        m.scrollTo(scroll, t);
                        t += 300;
                        m.advance(t);
                        flip = 1 - flip;
                        m.pointerMove(600 + flip, y, false, t);
                        std::vector<TrackId> seen{m.enlarged().track};
                        const TimeMs end = t + 2000;
                        for (int guard = 0; guard < 100; ++guard) {
                            const TimeMs d = m.nextDeadline();
                            if (d > end)
                                break;
                            m.advance(d);
                            if (m.enlarged().track != seen.back())
                                seen.push_back(m.enlarged().track);
                        }
                        m.advance(end);
                        t = end;
                        ++cases;
                        bool revisit = false;
                        for (std::size_t i = 0; i + 1 < seen.size(); ++i)
                            for (std::size_t j = i + 1; j < seen.size(); ++j)
                                revisit = revisit || seen[i] == seen[j];
                        const RowRef under = m.pointerRow();
                        const bool inside = !under.valid() || under.track == m.enlarged().track;
                        twice += seen.size() > 2 ? 1u : 0u;
                        back += revisit ? 1u : 0u;
                        outside += inside ? 0u : 1u;
                        bad += seen.size() > 2 || revisit || !inside ? 1u : 0u;
                    }
        }
    std::printf("  %zu still-pointer cases: %zu changed twice, %zu came back, %zu ended outside\n", cases, twice, back,
                outside);
    check(cases > 10000 && bad == 0, "T18: the enlarged row changes at most once, never back, and ends under the pointer");
}

void holdOff() {
    section("T19 the scroll hold-off");
    Fixture f = uniform(40);
    auto m = model(f);
    TimeMs t = 100;
    restWith(m, 2, 0, t);
    m.pointerMove(600, 450, false, 900);
    WheelDelta notch;
    notch.notches = 1;
    m.wheel(600, 450, notch, Mods{}, 1000);
    check(m.scroll() == 131 && m.pointerRow().track == 4, "T19: one notch: scroll 131, row 3 under the pointer");
    m.advance(1060);
    m.advance(1119);
    check(m.enlarged().track == 3, "T19: still row 2 at 119 ms");
    m.advance(1120);
    check(m.enlarged().track == 4 && m.row(3).top == 412, "T19: row 3 at 120 ms, held at 412");
    m.advance(1280);
    check(m.scroll() == 32, "T19: scroll 32 at the animation's end");
    m.advance(1500);
    check(m.row(3).top == 410 && m.scroll() == 34, "T19: revealed to 410");
    Fixture g = uniform(40);
    auto w = model(g);
    t = 100;
    restWith(w, 2, 0, t);
    w.pointerMove(600, 450, false, 900);
    w.wheel(600, 450, notch, Mods{}, 1000);
    w.wheel(600, 450, notch, Mods{}, 1100);
    w.advance(1219);
    check(w.scroll() == 262 && w.pointerRow().track == 5 && w.enlarged().track == 3, "T19: a second notch waits");
    w.advance(1220);
    check(w.enlarged().track == 5, "T19: promoted at 1220");
    Fixture h = uniform(40);
    auto p = model(h);
    t = 100;
    restWith(p, 2, 0, t);
    p.pointerMove(600, 450, false, 900);
    p.key(Key::PageDown, Mods{}, 1000);
    p.advance(1119);
    const TrackId under = p.pointerRow().track;
    check(p.scroll() == 600 && p.enlarged().track == 3, "T19: Page Down waits the same");
    p.advance(1120);
    check(p.enlarged().track == under, "T19: and promotes at 120 ms");
}

void leaving() {
    section("T20 leaving");
    Fixture f = uniform(40);
    auto m = model(f);
    m.pointerMove(600, 300, false, 100);
    m.advance(1000);
    const TrackId hovered = m.enlarged().track;
    m.pointerExit(5000);
    m.advance(5299);
    check(m.enlarged().track == hovered && hovered != 0, "T20: still enlarged at 299 ms");
    m.advance(5300);
    check(!m.enlarged().valid(), "T20: every row back to base at 300 ms");
    Fixture g = uniform(40);
    auto k = model(g);
    k.setKeyboardFocus(true, 50);
    k.pointerMove(20, 20, false, 100);
    pressChip(k, 0, 120); // focus track 1
    k.pointerMove(600, 300, false, 200);
    k.advance(1000);
    k.pointerExit(5000);
    k.advance(5300);
    check(k.enlarged().track == 1, "T20: with the keys in STACK the focused row stays enlarged");
    // keyboard focus lost and regained with the pointer outside: the focused row shrinks, then grows again
    Fixture back = uniform(40);
    auto kb = model(back);
    kb.setKeyboardFocus(true, 50);
    kb.focusRow(kb.rowRef(5), 60);
    kb.advance(500);
    check(kb.enlarged().track == 5 && kb.row(4).height == 190, "T20: the focused row enlarged with the pointer outside");
    kb.setKeyboardFocus(false, 1000);
    kb.advance(1500);
    check(!kb.enlarged().valid() && kb.row(4).height == 100, "T20: the keys gone: back to base");
    kb.setKeyboardFocus(true, 2000);
    check(kb.enlarged().track == 5, "T20: the keys back: the focused row is the enlarged one again");
    kb.advance(2500);
    check(kb.row(4).height == 190, "T20: ... at its full height");
    Fixture h = uniform(40);
    auto x = model(h);
    x.pointerMove(600, 300, false, 100);
    x.advance(1000);
    x.maximise(x.enlarged(), 1000);
    x.advance(1400);
    x.pointerExit(5000);
    x.advance(6000);
    check(x.maximised().valid() && x.row(*x.shownIndex(x.maximised().track)).tier == Tier::Max,
          "T20: a maximised row stays");
    Fixture i = uniform(40);
    auto r = model(i);
    r.pointerMove(600, 300, false, 100);
    r.advance(1000);
    const TrackId before = r.enlarged().track;
    r.pointerExit(5000);
    r.pointerMove(600, 300, false, 5200);
    r.advance(6000);
    check(r.enlarged().track == before, "T20: coming back cancels the timer");
}

void autoZoomOff() {
    section("T21 auto-zoom off");
    Fixture f = uniform(40);
    auto m = model(f);
    m.setAutoZoom(false, 10);
    m.pointerMove(600, 300, false, 100);
    m.advance(2100);
    check(!m.enlarged().valid(), "T21: hover enlarges nothing");
    m.pointerMove(20, 400, false, 2200);
    pressChip(m, 3, 2200);
    check(m.enlarged().track == 4, "T21: a chip click enlarges at once");
    m.setKeyboardFocus(true, 2300);
    m.key(Key::Down, Mods{}, 2300);
    check(m.enlarged().track == 5, "T21: Down enlarges at once");
    m.pointerExit(2400);
    m.advance(5000);
    check(m.enlarged().track == 5, "T21: leaving keeps it");
}

void animation() {
    section("T22 the animation");
    Fixture f = uniform(20);
    auto m = model(f, 1264, 900);
    m.setKeyboardFocus(true, 0);
    m.focusRow(m.rowRef(6), 100);
    m.advance(1000);
    m.key(Key::Down, Mods{}, 1000); // row 6
    m.advance(1080);
    const double at80[7] = {101.25, 112.375, 136.625, 182.375, 126.625, 108.75, 100};
    bool a = true;
    for (std::size_t i = 0; i < 7; ++i)
        a = a && near(m.row(3 + i).height, at80[i], 1e-9);
    check(a, "T22: at 80 ms toward row 6");
    m.key(Key::Down, Mods{}, 1080); // row 7: every height continuous
    bool continuous = true;
    for (std::size_t i = 0; i < 7; ++i)
        continuous = continuous && near(m.row(3 + i).height, at80[i], 1e-9);
    check(continuous, "T22: a retarget continues from the current values");
    m.advance(1160);
    const double at160[7] = {100.15625, 101.546875, 113.328125, 135.671875, 182.078125, 126.46875, 108.75};
    bool b = true;
    for (std::size_t i = 0; i < 7; ++i)
        b = b && near(m.row(3 + i).height, at160[i], 1e-9);
    check(b, "T22: 80 ms toward row 7");
    Fixture g = uniform(20);
    auto one = model(g, 1264, 900);
    one.focusRow(one.rowRef(6), 1000);
    const double grow[10] = {124.39, 143.92, 159.13, 170.56, 178.75, 184.24, 187.57, 189.28, 189.91, 190};
    const double gap[5] = {33.439, 35.392, 36.913, 38.056, 38.875};
    bool h = true, gg = true;
    for (int k = 1; k <= 10; ++k) {
        one.advance(1000 + 16 * k);
        h = h && near(one.row(5).height, grow[k - 1], 1e-9);
        if (k <= 5)
            gg = gg && near(gapBelow(one, 5), gap[k - 1], 1e-9);
    }
    check(h, "T22: a row growing 100 to 190 at 16 ms steps");
    check(gg, "T22: its gap 31 to 40 with it (its neighbours grow to 129)");
    check(one.row(5).height == 190 && gapBelow(one, 5) == 40, "T22: at rest every value is an integer");
}

void scrolling() {
    section("T23 scrolling");
    Fixture f = uniform(40);
    auto m = model(f);
    m.setSelection(9, 0);
    WheelDelta notch;
    notch.notches = 1;
    auto fx = m.wheel(600, 300, notch, Mods{}, 100);
    check(m.scroll() == 131 && fx.select == 0, "T23: a notch is one base pitch, no selection");
    WheelDelta pad;
    pad.pixelsY = 45;
    m.wheel(600, 300, pad, Mods{}, 200);
    check(m.scroll() == 176, "T23: a trackpad by pixels");
    m.key(Key::PageDown, Mods{}, 300);
    check(m.scroll() == 776, "T23: Page Down by the list's height");
    m.key(Key::Home, Mods{}, 400);
    check(m.scroll() == 0, "T23: Home");
    m.key(Key::End, Mods{}, 500);
    check(m.scroll() == 4609 && m.maxScroll() == 4609, "T23: End: 40 x 131 - 31 - 600");
    Mods ctrl;
    ctrl.ctrl = true;
    fx = m.wheel(600, 300, notch, ctrl, 600);
    check(fx.consumed && m.scroll() == 4609 && m.windowView().baseHeight == 100, "T23: Ctrl+wheel does nothing");
    check(!m.focus().row.valid() && m.pins().empty() && m.selection() == 9, "T23: no focus, pin or selection moved");
    m.setBaseHeight(28, 700);
    m.key(Key::Home, Mods{}, 800);
    m.wheel(600, 300, notch, Mods{}, 900);
    check(m.scroll() == 37, "T23: a notch at base 28 is 37");
    m.setBaseHeight(184, 1000);
    m.key(Key::Home, Mods{}, 1100);
    m.wheel(600, 300, notch, Mods{}, 1200);
    check(m.scroll() == 241, "T23: a notch at base 184 is 241");
}

// --- virtualisation and allocation -----------------------------------------------------------------------

struct Work {
    std::uint64_t rows = 0, entries = 0, items = 0, lines = 0, full = 0;
    std::size_t realised = 0, visible = 0, subscribed = 0;
    unsigned allocs = 0;
};

void paint(const StackModel &m) {
    m.forEachVisibleRow([&](const RowGeo &r) {
        (void)m.lane(r.shown);
        m.forEachChainItem(r.shown, [](const ItemGeo &) {});
        m.forEachModuleCell(r.shown, [](const CellGeo &) {});
        (void)m.chip(r.shown);
        (void)m.collisionReference(r.ref); // the chip's button state, per visible row
    });
    m.forEachNavigatorLine([](const NavLine &) {});
}

Fixture withChains(std::size_t n) {
    Fixture f = uniform(n);
    for (std::size_t i = 1; i <= n; ++i) {
        const auto id = static_cast<TrackId>(i);
        f.item(id, 100000 + 2 * id, ItemClass::Eq, "EQ Eight");
        f.item(id, 100001 + 2 * id, ItemClass::Dynamics, "Compressor");
    }
    return f;
}

Work scriptedMinute(std::size_t n) {
    Fixture f = withChains(n);
    auto m = model(f);
    TimeMs t = 1000;
    m.scrollTo(30.0 * 131, t);
    t += 500;
    m.advance(t);
    m.pointerMove(600, 300, false, t);
    paint(m); // warm
    m.resetStats();
    WheelDelta down, up;
    down.notches = 1;
    up.notches = -1;
    audit = true;
    allocations = 0;
    for (int frame = 0; frame < 60; ++frame) {
        t += 16;
        m.advance(t);
        paint(m);
        if (frame % 6 == 0)
            (void)m.hitTest(300 + frame, 50 + 5 * frame);
        if (frame % 12 == 5)
            m.pointerMove(600 + frame, 300 + frame % 3, false, t);
        if (frame == 25)
            m.wheel(600, 300, down, Mods{}, t);
        if (frame == 45)
            m.wheel(600, 300, up, Mods{}, t);
    }
    audit = false;
    Work w;
    w.allocs = allocations;
    w.rows = m.stats().rowEvaluations;
    w.entries = m.stats().entryVisits;
    w.items = m.stats().itemEvaluations;
    w.lines = m.stats().lineEvaluations;
    w.full = m.stats().fullPasses;
    w.realised = m.realised().size();
    w.visible = m.visible().size();
    w.subscribed = m.subscribed().size();
    return w;
}

void workCounter() {
    section("T24 virtualised: the work does not grow with hidden rows (and M9's subscriptions)");
    const Work a = scriptedMinute(64), b = scriptedMinute(1000), c = scriptedMinute(20000);
    std::printf("  rows %llu, entries %llu, items %llu, lines %llu per minute at 64, 1000 and 20000 rows\n",
                static_cast<unsigned long long>(a.rows), static_cast<unsigned long long>(a.entries),
                static_cast<unsigned long long>(a.items), static_cast<unsigned long long>(a.lines));
    check(a.rows == b.rows && b.rows == c.rows && a.rows > 0, "T24: row evaluations equal");
    check(a.entries == b.entries && b.entries == c.entries, "T24: entry visits equal");
    check(a.items == b.items && b.items == c.items && a.items > 0, "T24: item evaluations equal");
    check(a.lines == b.lines && b.lines == c.lines && a.lines > 0, "T24: navigator lines equal");
    check(a.full == 0 && b.full == 0 && c.full == 0, "T24: no full pass in the minute");
    check(a.realised == c.realised && a.realised < 20, "T24: the realised span is the same size");
    check(c.subscribed <= c.visible + 2 && c.realised > c.visible, "M9: subscriptions follow the visible rows");
    check(a.allocs == 0 && b.allocs == 0 && c.allocs == 0, "T25: the minute allocates nothing");
}

Work editingMinute(std::size_t n, const char *jumpQuery = nullptr) {
    Fixture f = withChains(n);
    for (auto &t : f.tracks)
        t.revision = 1;
    auto m = model(f);
    TimeMs t = 1000;
    if (jumpQuery != nullptr) {
        m.setSearchMode(SearchMode::Jump, t);
        m.setSearch(jumpQuery, t);
    }
    m.scrollTo(30.0 * 131, t);
    m.resetStats();
    for (int k = 0; k < 60; ++k) {
        TrackInput &edited = f.track(5);
        edited.name = f.keep(k % 2 == 0 ? "Track 5 edited" : "Track 5");
        edited.revision = static_cast<std::uint64_t>(k) + 2;
        f.generation = static_cast<std::uint64_t>(k) + 1;
        m.setRows(f.input(), t += 10);
    }
    for (int k = 0; k < 10; ++k) {
        const auto cap = m.beginCapture(CaptureKind::Drag, m.moduleRef(m.shownTrack(31), kQ), t += 10);
        m.endCapture(cap.ref, t += 10);
        m.pin(m.rowRef(m.shownTrack(32)), Mods{}, t += 10);
    }
    Work w;
    w.full = m.stats().fullPasses;
    w.items = m.stats().haystackBuilds;
    w.rows = m.stats().rowUpdates;
    w.entries = m.stats().shownEdits;
    w.lines = m.stats().rowCompares;
    return w;
}

void incrementalRows() {
    section("M10 setRows, captures and pins do no work for hidden rows");
    const Work a = editingMinute(64), b = editingMinute(1000), c = editingMinute(20000);
    check(a.full == 0u && b.full == 0u && c.full == 0u, "M10: no full pass for 60 one-track edits, 10 captures, 10 pins");
    check(a.items == 60u && b.items == 60u && c.items == 60u, "M10: one search text folded per edit");
    check(a.rows == 60u && b.rows == 60u && c.rows == 60u, "M10: one row updated per edit");
    check(a.entries == 0u && c.entries == 0u, "M10: the shown list untouched");
    check(a.lines == 60u * 64u && c.lines == 60u * 20000u, "M10: the cheap per-track compare is the only O(rows) step");
    // in Jump mode with a query the matches are rebuilt only when a row's match or the shown list moved
    const Work j = editingMinute(1000, "kick"), jk = editingMinute(20000, "kick");
    check(j.full == 0u && jk.full == 0u && j.rows == 60u, "M10: Jump mode: no match pass for edits that match as before");
    Fixture jf = withChains(500);
    auto jm = model(jf);
    jm.setSearchMode(SearchMode::Jump, 5);
    jm.setSearch("track 7", 10);
    jm.searchKey(Key::Down, 20);
    jm.searchKey(Key::Down, 30);
    check(jm.jumpTarget().track == 71, "M10: the third match is track 71");
    jm.resetStats();
    jf.track(300).name = jf.keep("Renamed");
    jf.generation = 2;
    jm.setRows(jf.input(), 40);
    check(jm.stats().fullPasses == 0 && jm.jumpTarget().track == 71, "M10: a rename that matches as before: no pass, the cursor stays");
    jf.track(8).name = jf.keep("Track 7 copy");
    jf.generation = 3;
    jm.setRows(jf.input(), 50);
    check(jm.stats().fullPasses == 1 && jm.jumpTarget().track == 71,
          "M10: a rename that matches now: one counted pass, the cursor stays on its track");
    // without revision tokens the compare is deeper but still folds only what changed
    Fixture f = withChains(500);
    auto m = model(f);
    m.resetStats();
    f.track(7).name = f.keep("Renamed");
    f.generation = 9;
    m.setRows(f.input(), 10);
    check(m.stats().haystackBuilds == 1 && m.stats().fullPasses == 0 && m.trackName(7) == "Renamed",
          "M10: a rename folds one text");
    const auto same = m.setRows(f.input(), 20);
    check(!same.repaint, "M10: the same generation returns at once");
    audit = true;
    allocations = 0;
    f.generation = 0; // no early return: the whole compare runs
    m.setRows(f.input(), 30);
    audit = false;
    check(allocations == 0, "T25: setRows with the same input allocates nothing");
}

void noAllocation() {
    section("T25 hover, keys and captures allocate nothing");
    Fixture f = withChains(40);
    auto m = model(f);
    m.setKeyboardFocus(true, 0);
    // warm every path once
    m.pointerMove(600, 300, false, 10);
    m.advance(500);
    auto warm = m.beginCapture(CaptureKind::Drag, m.moduleRef(2, kQ), 600);
    m.endCapture(warm.ref, 700);
    m.setSearch("track 1", 800);
    m.setSearch("", 900);
    audit = true;
    allocations = 0;
    TimeMs t = 1000;
    m.pointerMove(600, 560, false, t);
    m.advance(t += 70);
    m.advance(t += 300);
    WheelDelta notch;
    notch.notches = 1;
    m.wheel(600, 300, notch, Mods{}, t += 10);
    m.advance(t += 400);
    m.key(Key::Down, Mods{}, t += 10);
    m.key(Key::Right, Mods{}, t += 10);
    m.key(Key::Right, Mods{}, t += 10);
    m.key(Key::Left, Mods{}, t += 10);
    m.key(Key::Enter, Mods{}, t += 10);
    m.key(Key::Escape, Mods{}, t += 10);
    m.advance(t += 400);
    const auto cap = m.beginCapture(CaptureKind::Drag, m.chainRef(5, 100010), t += 10);
    m.pointerMove(600, 100, true, t += 10);
    m.advance(t += 300);
    m.endCapture(cap.ref, t += 10);
    m.pointerExit(t += 10);
    m.advance(t += 400);
    m.pin(m.rowRef(3), Mods{}, t += 10);
    m.setSearch("track 2", t += 10);
    m.setSearch("", t += 10);
    paint(m);
    audit = false;
    check(cap.refusal == CaptureRefusal::None, "T25: the audited capture began");
    check(allocations == 0, "T25: no allocation in pointer, wheel, key, capture, pin, search or advance events");
}

// --- identity and captures -----------------------------------------------------------------------------

void generations() {
    section("T26 generations (and M1: modules by instance)");
    Fixture f = eleven();
    auto m = model(f);
    const RowRef row3 = m.rowRef(3);
    check(m.live(row3), "T26: a row is live");
    Fixture without = f;
    without.erase(3);
    without.generation = 2;
    m.setRows(without.input(), 10);
    check(!m.live(row3), "T26: gone with its track");
    f.generation = 3;
    m.setRows(f.input(), 20);
    check(!m.live(row3) && m.live(m.rowRef(3)) && m.rowRef(3).gen != row3.gen, "T26: back as a new row");
    const ItemRef eq = m.chainRef(3, 31);
    f.generation = 4;
    m.setRows(f.input(), 30);
    check(m.live(eq), "T26: the same item keeps its ref");
    f.chains[f.at(3)][0].identity = 999;
    f.generation = 5;
    m.setRows(f.input(), 40);
    check(!m.live(eq) && m.live(m.chainRef(3, 31)), "T26: a replaced device under the same id is a new item");
    const ItemRef moved = m.chainRef(3, 32);
    auto saturator = f.chains[f.at(3)][1];
    f.chains[f.at(3)].pop_back();
    f.chains[f.at(5)].push_back(saturator);
    f.generation = 6;
    m.setRows(f.input(), 50);
    check(!m.live(moved) && m.live(m.chainRef(5, 32)), "T26: an item moved to another track is a new item");
    // modules: by (row, key) and the instance token
    const ItemRef q = m.moduleRef(2, kQ);
    f.module(2, kQ).state = ModuleState::Engaged;
    f.module(2, kQ).device = 57;
    f.generation = 7;
    m.setRows(f.input(), 60);
    check(m.live(q) && m.resolve(q)->device == 57 && m.resolve(q)->state == ModuleState::Engaged,
          "M1: the first engage keeps the ref; resolve gives the new device (m10)");
    f.module(2, kQ).state = ModuleState::Dormant;
    f.generation = 8;
    m.setRows(f.input(), 70);
    check(m.live(q), "M1: switched off with its settings kept, the ref stays");
    f.module(2, kQ).instance = 777;
    f.generation = 9;
    m.setRows(f.input(), 80);
    check(!m.live(q), "M1: reset (a new instance) kills the ref");
    const ItemRef q2 = m.moduleRef(2, kQ);
    f.module(2, kQ) = ModuleInput{};
    f.generation = 10;
    m.setRows(f.input(), 90);
    check(!m.live(q2) && m.moduleRef(2, kQ).gen == 0, "M1: Dormant to Absent kills the ref");
    const auto ignoredBefore = m.stats().ignoredInputs;
    f.module(2, kQ).state = ModuleState::Engaged;
    f.module(2, kQ).instance = 0;
    f.generation = 11;
    m.setRows(f.input(), 100);
    check(m.moduleRef(2, kQ).gen == 0 && m.stats().ignoredInputs > ignoredBefore,
          "M1: a module without an instance token is refused and counted");
    f.module(2, kQ).instance = 501;
    f.module(2, kQ).device = 88;
    f.item(2, 88, ItemClass::AdiQ, "the processor itself");
    f.generation = 12;
    m.setRows(f.input(), 110);
    check(m.chainRef(2, 88).gen == 0 && m.moduleRef(2, kQ).device == 88,
          "M1: the processor's own device is never a chain item");
    // a generation is never reused for an item or a module either
    const ItemRef eq2 = m.chainRef(3, 31);
    const ItemInput eqBack = f.chains[f.at(3)][0];
    f.chains[f.at(3)].erase(f.chains[f.at(3)].begin());
    f.generation = 13;
    m.setRows(f.input(), 120);
    check(eq2.gen != 0 && !m.live(eq2), "T26: the device removed: its ref is dead");
    f.chains[f.at(3)].insert(f.chains[f.at(3)].begin(), eqBack);
    f.generation = 14;
    m.setRows(f.input(), 130);
    check(!m.live(eq2) && m.live(m.chainRef(3, 31)) && m.chainRef(3, 31).gen != eq2.gen,
          "T26: back with the same device and identity: a new item, the old ref stays dead");
    const ItemRef l5 = m.moduleRef(5, kL);
    const std::uint64_t instance = f.module(5, kL).instance;
    f.module(5, kL) = ModuleInput{};
    f.generation = 15;
    m.setRows(f.input(), 140);
    check(l5.gen != 0 && !m.live(l5), "T26: the module absent: its ref is dead");
    f.module(5, kL).state = ModuleState::Dormant;
    f.module(5, kL).instance = instance;
    f.generation = 16;
    m.setRows(f.input(), 150);
    check(!m.live(l5) && m.live(m.moduleRef(5, kL)) && m.moduleRef(5, kL).gen != l5.gen,
          "T26: back with the same instance: a new module, the old ref stays dead");
}

void capturedTargetStaysPut() {
    section("T27 a captured target stays put (and M4: x too)");
    Fixture f = uniform(40);
    f.item(13, 1301, ItemClass::Eq, "EQ Eight");
    f.item(13, 1302, ItemClass::View, "Utility");
    auto m = model(f);
    TimeMs t = 100;
    restWith(m, 12, 1310, t);
    check(m.row(12).top == 313 && m.enlarged().track == 13, "T27: row 12 at 313");
    const ItemRef target = m.chainRef(13, 1301);
    const auto cap = m.beginCapture(CaptureKind::Drag, target, 1000);
    check(cap.refusal == CaptureRefusal::None && m.capture().has_value(), "T27: the capture begins");
    double x0 = 0, w0 = 0;
    m.forEachChainItem(12, [&](const ItemGeo &g) {
        if (g.ref == target) {
            x0 = g.x;
            w0 = g.width;
        }
    });
    const double h0 = m.row(12).height;
    const auto same = [&](const char *what) {
        double x = -1, w = -1;
        m.forEachChainItem(*m.shownIndex(13), [&](const ItemGeo &g) {
            if (g.ref == target) {
                x = g.x;
                w = g.width;
            }
        });
        const RowGeo r = m.row(*m.shownIndex(13));
        check(r.top == 313 && r.height == h0 && x == x0 && w == w0 && m.enlarged().track == 13, what);
    };
    for (TimeMs s = 1000; s <= 1500; s += 25)
        m.pointerMove(600, 400 + static_cast<double>(s - 1000) / 5, true, s);
    same("T27: the pointer crossing rows promotes nothing");
    f.insert(3, 99, "Inserted");
    f.generation = 2;
    m.setRows(f.input(), 1510);
    check(m.scroll() == 1441, "T27: a track inserted above: scroll 1441");
    same("T27: ... the row stays at 313");
    m.setSearch("track 2", 1520);
    check(m.row(*m.shownIndex(13)).retained, "T27: a search it does not match keeps it, retained");
    same("T27: ... and in place");
    m.setViewport(1264, 500, 1530);
    same("T27: the list resized to 500");
    f.module(30, kL).state = ModuleState::Engaged; // M4: a module engaged on another row adds a column
    f.generation = 3;
    m.setRows(f.input(), 1540);
    same("M4: another row's module engaged: no column moves");
    m.setViewport(1004, 500, 1550);
    same("M4: the list narrowed: still in place");
    f.chains[f.at(13)].insert(f.chains[f.at(13)].begin(), ItemInput{1300, 4242, ItemClass::Dynamics, "Gate"});
    f.generation = 4;
    m.setRows(f.input(), 1560);
    same("M4: a device inserted before it: still in place");
    for (int k = 0; k < 60; ++k)
        m.advance(1560 + 16 * k);
    same("T27: 60 frames later");
    m.pointerMove(600, 450, false, 2600);
    m.endCapture(cap.ref, 2600);
    check(!m.shownIndex(13).has_value() && !m.capture(), "T27: at the end the search applies");
    m.advance(2659);
    const TrackId waiting = m.enlarged().track;
    m.advance(2660);
    check(m.pointerRow().valid() && m.enlarged().track == m.pointerRow().track && waiting != m.pointerRow().track,
          "T27: the row under the pointer grows 60 ms after the end");
    // overscroll: the captured row keeps its place past the top, then the scroll settles
    Fixture g = uniform(10);
    auto o = model(g);
    o.setAutoZoom(false, 10);
    const auto c2 = o.beginCapture(CaptureKind::Text, ItemRef{o.rowRef(3), ItemPlace::None, 0, 0, o.rowRef(3).gen}, 20);
    const double before = o.row(2).top;
    g.erase(1);
    g.erase(2);
    g.generation = 2;
    o.setRows(g.input(), 30);
    check(o.row(0).top == before && o.scroll() == -262, "T27: past the top the scroll goes negative, the row stays");
    o.endCapture(c2.ref, 40);
    check(o.scroll() == 0, "T27: then it settles to 0");
}

void refusals() {
    section("T28 refusals and lost captures (m8, M2, M3)");
    Fixture f = eleven();
    auto m = model(f);
    const auto cap = m.beginCapture(CaptureKind::Drag, m.chainRef(3, 31), 10);
    check(m.beginCapture(CaptureKind::Menu, m.moduleRef(5, kQ), 20).refusal == CaptureRefusal::Busy,
          "T28: a second capture is refused: Busy");
    check(m.setBaseHeight(68, 30).refused && m.windowView().baseHeight == 100, "T28: the base height is held");
    check(m.maximise(m.rowRef(2), 40).refused, "T28: Max is refused");
    const double scroll = m.scroll();
    WheelDelta notch;
    notch.notches = 3;
    m.wheel(600, 300, notch, Mods{}, 50);
    check(m.scroll() == scroll, "T28: the wheel leaves the scroll alone");
    press(m, 1200, 16 + 14 * 6 + 5, 60);
    check(m.scroll() == scroll, "T28: a navigator click does not jump");
    // M2: an op between frames: the owner asks with the current generation before every edit
    check(m.captureStatus(cap.ref, 1) == CaptureStatus::Behind && m.stale(1),
          "M2: Behind when the project moved on since the last setRows");
    f.chains[f.at(3)].erase(f.chains[f.at(3)].begin());
    f.generation = 1;
    auto fx = m.setRows(f.input(), 70);
    check(fx.captureLost == CaptureStatus::ItemRemoved && m.captureStatus(cap.ref) == CaptureStatus::ItemRemoved &&
              m.captureStatus(cap.ref, 1) == CaptureStatus::ItemRemoved && !m.capture(),
          "T28: its device removed: ItemRemoved, the lock gone");
    const auto c2 = m.beginCapture(CaptureKind::Drag, m.chainRef(3, 32), 80);
    f.chains[f.at(3)][0].identity = 31337;
    f.generation = 2;
    fx = m.setRows(f.input(), 90);
    check(fx.captureLost == CaptureStatus::ItemReplaced, "T28: replaced: ItemReplaced");
    const auto c3 = m.beginCapture(CaptureKind::Text, ItemRef{m.rowRef(6), ItemPlace::None, 0, 0, m.rowRef(6).gen}, 100);
    Fixture gone = f;
    gone.erase(6);
    gone.generation = 3;
    fx = m.setRows(gone.input(), 110);
    check(fx.captureLost == CaptureStatus::RowRemoved && m.captureStatus(c3.ref) == CaptureStatus::RowRemoved,
          "T28: its track removed: RowRemoved");
    f.generation = 4;
    m.setRows(f.input(), 120);
    const auto c4 = m.beginCapture(CaptureKind::Text, ItemRef{m.rowRef(6), ItemPlace::None, 0, 0, m.rowRef(6).gen}, 130);
    f.track(6).identity = 0x31D1; // M3: another kind of track under the same id
    f.generation = 5;
    fx = m.setRows(f.input(), 140);
    check(fx.captureLost == CaptureStatus::RowReplaced, "M3: a rename capture on a track replaced: RowReplaced");
    check(m.beginCapture(CaptureKind::Drag, c2.ref.target, 150).refusal == CaptureRefusal::Stale, "T28: a stale ref: Stale");
    m.setSearch("vox", 160);
    check(m.beginCapture(CaptureKind::Drag, m.moduleRef(3, kQ), 170).refusal == CaptureRefusal::NotShown,
          "T28: a row not shown: NotShown");
    m.setSearch("", 180);
    ItemRef refused;
    refused.row = m.rowRef(11);
    refused.place = ItemPlace::Module;
    refused.module = kDuck;
    refused.gen = 0;
    check(m.beginCapture(CaptureKind::Drag, refused, 190).refusal == CaptureRefusal::Unavailable,
          "m8: Main's ducking: Unavailable");
    const auto c5 = m.beginCapture(CaptureKind::Drag, m.moduleRef(2, kQ), 200);
    m.endCapture(c4.ref, 210);
    check(m.capture().has_value() && m.captureStatus(c5.ref) == CaptureStatus::Live,
          "T28: an earlier capture's token ends nothing");
    m.endCapture(c5.ref, 220);
    check(m.captureStatus(c5.ref) == CaptureStatus::Ended, "T28: ended");
    // a capture on a module of the channel processor: the instance and the state
    const auto c6 = m.beginCapture(CaptureKind::Drag, m.moduleRef(2, kQ), 230);
    f.module(2, kQ).state = ModuleState::Engaged;
    f.generation = 6;
    fx = m.setRows(f.input(), 240);
    check(c6.refusal == CaptureRefusal::None && fx.captureLost == CaptureStatus::Live &&
              m.captureStatus(c6.ref) == CaptureStatus::Live,
          "T28: the module engaged under the capture: still live");
    f.module(2, kQ).instance = 4242;
    f.generation = 7;
    fx = m.setRows(f.input(), 250);
    check(fx.captureLost == CaptureStatus::ItemReplaced && m.captureStatus(c6.ref) == CaptureStatus::ItemReplaced &&
              !m.capture(),
          "T28: its instance changed: ItemReplaced");
    const auto c7 = m.beginCapture(CaptureKind::Drag, m.moduleRef(2, kQ), 260);
    f.module(2, kQ).state = ModuleState::Dormant;
    f.generation = 8;
    fx = m.setRows(f.input(), 270);
    check(c7.refusal == CaptureRefusal::None && fx.captureLost == CaptureStatus::Live &&
              m.captureStatus(c7.ref) == CaptureStatus::Live,
          "T28: switched off with its settings kept: still live");
    f.module(2, kQ) = ModuleInput{};
    f.generation = 9;
    fx = m.setRows(f.input(), 280);
    check(fx.captureLost == CaptureStatus::ItemRemoved && m.captureStatus(c7.ref) == CaptureStatus::ItemRemoved,
          "T28: the module gone: ItemRemoved");
    // M3: identity is required
    Fixture bad = uniform(3);
    bad.track(2).identity = 0;
    bad.item(3, 7, ItemClass::Eq, "EQ", 0);
    bad.chains[bad.at(3)][0].identity = 0;
    auto b = model(bad);
    check(b.shownCount() == 2 && b.stats().ignoredInputs >= 2 && b.chainRef(3, 7).gen == 0,
          "M3: a track or item without identity is refused and counted");
}

void rename() {
    section("T29 rename (and m17: Tab ends, focuses, selects, asks again)");
    Fixture f = uniform(10);
    auto m = model(f);
    m.setKeyboardFocus(true, 0);
    m.focusRow(m.rowRef(6), 10);
    auto fx = m.key(Key::Rename, Mods{}, 20);
    check(fx.rename.track == 6, "T29: Ctrl+R renames the focused row's track");
    const auto cap = m.beginCapture(CaptureKind::Text, ItemRef{fx.rename, ItemPlace::None, 0, 0, fx.rename.gen}, 30);
    fx = m.key(Key::Tab, Mods{}, 40);
    check(fx.captureEnded == cap.ref.token && fx.rename.track == 7 && fx.select == 7 && m.focus().row.track == 7 &&
              !m.capture() && m.captureStatus(cap.ref) == CaptureStatus::Ended,
          "T29: Tab ends this rename and asks for the next track's");
    const auto c2 = m.beginCapture(CaptureKind::Text, ItemRef{fx.rename, ItemPlace::None, 0, 0, fx.rename.gen}, 50);
    Mods shift;
    shift.shift = true;
    fx = m.key(Key::Tab, shift, 60);
    check(fx.captureEnded == c2.ref.token && fx.rename.track == 6, "T29: Shift+Tab goes back");
    m.focusRow(m.rowRef(10), 70);
    fx = m.key(Key::Rename, Mods{}, 80);
    const auto c3 = m.beginCapture(CaptureKind::Text, ItemRef{fx.rename, ItemPlace::None, 0, 0, fx.rename.gen}, 90);
    fx = m.key(Key::Tab, Mods{}, 100);
    check(fx.captureEnded == c3.ref.token && !fx.rename.valid(), "T29: Tab on the last track ends the rename");
    fx = m.key(Key::Tab, Mods{}, 110);
    check(!fx.consumed, "T29: Tab outside a rename is Live's");
}

void menuStaysPut() {
    section("T30 a menu stays put");
    Fixture f = uniform(40);
    auto m = model(f);
    m.scrollTo(1310, 100);
    m.pointerMove(600, 300, false, 500); // row 12 at 262 to 362
    m.advance(1000);
    check(m.enlarged().track == 13, "T30: row 12 enlarged by hover");
    const RowGeo before = m.row(12);
    const auto cap = m.beginCapture(CaptureKind::Menu, m.moduleRef(13, kQ), 1000);
    m.pointerExit(1010);
    m.advance(1400);
    check(m.row(12).top == before.top && m.row(12).height == before.height && m.enlarged().track == 13,
          "T30: no shrink while the menu is open");
    m.endCapture(cap.ref, 1500);
    m.advance(1799);
    check(m.enlarged().track == 13, "T30: the leave rule restarts from the end");
    m.advance(1800);
    check(!m.enlarged().valid(), "T30: 300 ms after the end");
}

// --- keys, focus, Esc ----------------------------------------------------------------------------------

void keys() {
    section("T31 keys (and B3's card order)");
    Fixture f = eleven();
    auto m = model(f);
    auto fx = m.pointerDown(600, 50, Button::Left, Mods{}, 1, 10);
    m.pointerUp(600, 50, 10);
    check(fx.takeFocus, "T31: a press in the list takes focus");
    m.focusRow(m.rowRef(1), 20);
    fx = m.key(Key::Up, Mods{}, 30);
    check(m.focus().row.track == 1 && fx.consumed && fx.select == 0, "T31: Up on the first row: nothing");
    m.focusRow(m.rowRef(11), 40);
    fx = m.key(Key::Down, Mods{}, 50);
    check(m.focus().row.track == 11 && fx.select == 0, "T31: Down on the last: nothing");
    m.focusRow(m.rowRef(3), 60);
    std::vector<std::pair<ItemPlace, std::uint64_t>> order;
    const auto at = [&] {
        const ItemRef i = m.focus().item;
        return std::make_pair(i.place, i.place == ItemPlace::Chain ? static_cast<std::uint64_t>(i.device) : i.module);
    };
    order.push_back(at());
    for (int k = 0; k < 8; ++k) {
        m.key(Key::Right, Mods{}, 70 + k);
        order.push_back(at());
    }
    const std::vector<std::pair<ItemPlace, std::uint64_t>> expected{
        {ItemPlace::None, 0},       {ItemPlace::Chain, 31},     {ItemPlace::Chain, 32},
        {ItemPlace::Module, kQ},    {ItemPlace::Module, kL},    {ItemPlace::Module, kDuck},
        {ItemPlace::Module, kClip}, {ItemPlace::Module, kClip}, {ItemPlace::Module, kClip}};
    check(order == expected, "T31: chip, EQ Eight, Saturator, ADI-Q, ADI-L, then the tray's ducking and Clip");
    m.key(Key::Left, Mods{}, 90);
    check(at() == std::make_pair(ItemPlace::Module, std::uint64_t{kDuck}), "T31: Left steps back");
    const double scroll = m.scroll();
    m.key(Key::Home, Mods{}, 100);
    check(m.focus().row.track == 3, "T31: Home scrolls only");
    (void)scroll;
    const Hit chip = chipHit(m, 0);
    fx = m.pointerDown(chip.rect.x + 2, chip.rect.y + 2, Button::Left, Mods{}, 2, 110);
    m.pointerUp(chip.rect.x + 2, chip.rect.y + 2, 110);
    check(fx.showDevices.track == 1, "T31: a double-click on a chip shows its devices");
    // a focused item scrolled out of the lane brings the lane to it
    Fixture g = laneFixture();
    auto l = model(g);
    l.setAutoZoom(false, 0);
    l.focusRow(l.rowRef(1), 10);
    for (int k = 0; k < 4; ++k)
        l.key(Key::Right, Mods{}, 20 + k);
    l.advance(1000);
    check(l.focus().item.device == 104 && l.lane(0).first >= 2, "T31: Right onto a hidden item scrolls the lane");
    // B3: the card's order is the input's
    Fixture c = eleven();
    auto card = model(c);
    card.setModuleLayout(ModuleLayout::Card, 0);
    card.focusRow(card.rowRef(3), 10);
    for (int k = 0; k < 3; ++k)
        card.key(Key::Right, Mods{}, 20 + k);
    std::vector<std::uint32_t> mods;
    for (int k = 0; k < 4; ++k) {
        mods.push_back(card.focus().item.module);
        card.key(Key::Right, Mods{}, 30 + k);
    }
    check(mods == std::vector<std::uint32_t>{kQ, kDuck, kClip, kL}, "B3: in the card Left and Right follow the input");
}

void escape() {
    section("T32 Esc");
    Fixture f = uniform(20);
    auto m = model(f);
    const auto cap = m.beginCapture(CaptureKind::Text, ItemRef{m.rowRef(2), ItemPlace::None, 0, 0, m.rowRef(2).gen}, 10);
    auto fx = m.key(Key::Escape, Mods{}, 20);
    check(fx.escape == EscapeStep::Capture && fx.consumed && m.capture().has_value(),
          "T32: first the capture (the owner ends it)");
    m.endCapture(cap.ref, 30);
    m.maximise(m.rowRef(2), 40);
    fx = m.key(Key::Escape, Mods{}, 50);
    check(fx.escape == EscapeStep::RestoreMax && !m.maximised().valid(), "T32: then Max");
    m.setSearch("track 1", 60);
    fx = m.key(Key::Escape, Mods{}, 70);
    check(fx.escape == EscapeStep::ClearSearch && fx.windowViewChanged && m.shownCount() == 20,
          "T32: then the search");
    fx = m.key(Key::Escape, Mods{}, 80);
    check(fx.escape == EscapeStep::None && !fx.consumed, "T32: then nothing");
    m.openFromChip(5, ItemKey{}, 90);
    fx = m.key(Key::Escape, Mods{}, 100);
    check(fx.escape == EscapeStep::CloseMixView, "T32: opened from a chip: Esc closes the Mix View");
    m.clearOpenedFrom(110);
    fx = m.key(Key::Escape, Mods{}, 120);
    check(fx.escape == EscapeStep::None, "T32: not after clearOpenedFrom");
    // all four at once: the chain's order, one step per Esc, each leaving the rest as it was
    Fixture g = uniform(20);
    auto all = model(g);
    all.maximise(all.rowRef(2), 10);
    all.setSearch("track", 20);
    all.openFromChip(5, ItemKey{}, 30);
    const auto held = all.beginCapture(CaptureKind::Text, ItemRef{all.rowRef(2), ItemPlace::None, 0, 0, all.rowRef(2).gen}, 40);
    check(held.refusal == CaptureRefusal::None && all.maximised().track == 2 && all.windowView().search == "track" &&
              all.openedFrom() == 5,
          "T32: a capture, Max, a search and an opened-from track at once");
    fx = all.key(Key::Escape, Mods{}, 50);
    check(fx.escape == EscapeStep::Capture && all.capture().has_value() && all.maximised().track == 2 &&
              all.windowView().search == "track" && all.openedFrom() == 5,
          "T32: Esc 1 names the capture and changes nothing else");
    all.endCapture(held.ref, 60);
    fx = all.key(Key::Escape, Mods{}, 70);
    check(fx.escape == EscapeStep::RestoreMax && !all.maximised().valid() && all.windowView().search == "track" &&
              all.openedFrom() == 5,
          "T32: Esc 2 restores Max, the search and the opened-from track stay");
    fx = all.key(Key::Escape, Mods{}, 80);
    check(fx.escape == EscapeStep::ClearSearch && all.windowView().search.empty() && all.openedFrom() == 5,
          "T32: Esc 3 clears the search, the opened-from track stays");
    fx = all.key(Key::Escape, Mods{}, 90);
    check(fx.escape == EscapeStep::CloseMixView, "T32: Esc 4 closes the Mix View");
}

void selectionIsRead() {
    section("T33 selection is read");
    Fixture f = eleven();
    auto m = model(f);
    m.setAutoZoom(false, 5);
    const double scroll = m.scroll();
    m.setSelection(9, 10);
    check(m.row(8).selected && !m.focus().row.valid() && m.scroll() == scroll, "T33: a selection made elsewhere bolds the chip");
    Constants follow;
    follow.followSelection = true;
    Fixture g = eleven();
    auto fm = model(g, 1264, 300, follow);
    auto fx = fm.setSelection(9, 10);
    fm.advance(500);
    check(fm.focus().row.track == 9 && fx.select == 0 && fm.row(8).top >= 0 && fm.row(8).top + fm.row(8).height <= 300,
          "T33: the followSelection variant focuses and reveals it");
    m.pointerMove(20, 50, false, 20);
    fx = m.pointerDown(chipHit(m, 0).rect.x + 2, chipHit(m, 0).rect.y + 2, Button::Left, Mods{}, 1, 30);
    check(fx.select == 1, "T33: a chip press selects");
    m.advance(1000); // the clicked row has grown (auto-zoom off: focus enlarges); nothing moves from here
    const ChipGeo g0 = m.chip(0);
    fx = m.pointerDown(g0.pin.x + 2, g0.pin.y + 2, Button::Left, Mods{}, 1, 1000);
    check(fx.select == 0 && fx.hit.part == HitPart::ChipPin, "T33: the pin does not");
    fx = m.pointerDown(g0.collision.x + 2, g0.collision.y + 2, Button::Left, Mods{}, 1, 1000);
    check(fx.select == 0 && fx.hit.part == HitPart::ChipCollision, "T33: nor the collision reference button");
    fx = m.pointerDown(g0.kick.x + 2, g0.kick.y + 2, Button::Left, Mods{}, 1, 1000);
    check(fx.select == 0 && fx.hit.part == HitPart::ChipKick, "T33: nor Kick");
    fx = m.pointerDown(519, m.row(1).top + 50, Button::Left, Mods{}, 1, 1000); // between the lane and the columns
    check(fx.select == 2 && fx.hit.part == HitPart::RowSpace, "T33: a press in a row's space selects");
    m.advance(2000);
    const CellGeo q = m.moduleCell(2, 0);
    fx = m.pointerDown(q.x + 10, m.row(2).top + 50, Button::Left, Mods{}, 1, 2000);
    check(fx.select == 3 && fx.hit.part == HitPart::ModulePreview, "T33: a press on a module selects");
    fx = m.pointerMove(600, 400, false, 2100);
    m.advance(4000);
    check(fx.select == 0 && m.selection() == 9, "T33: hover never selects");
}

void landing() {
    section("T34 the landing (m10: a module or a chain device)");
    Fixture f = uniform(100);
    auto m = model(f);
    m.pointerMove(600, 450, false, 950); // resting in a row, its dwell not yet done
    auto fx = m.openFromChip(31, ItemKey{kQ, 0}, 1000);
    check(m.scroll() == 3730 && m.row(30).top == 200, "T34: the row a third of the way down");
    check(fx.takeFocus && fx.select == 31 && m.focus().item.module == kQ && m.focus().item.row.track == 31,
          "T34: the row and its module focused");
    m.advance(1080);
    check(m.row(30).top == 200, "T34: held at 200 as it grows");
    m.advance(1160);
    check(m.scroll() == 3781 && m.enlarged().track == 31, "T34: scroll 3781 at 160 ms");
    m.advance(2499);
    check(m.enlarged().track == 31, "T34: enlarged until 1.5 s");
    m.advance(2500);
    check(m.enlarged().track == m.pointerRow().track && m.enlarged().track != 31,
          "T34: then the row under the resting pointer");
    Fixture g = uniform(100);
    auto e = model(g);
    e.pointerMove(600, 450, false, 950);
    e.openFromChip(31, ItemKey{}, 1000);
    e.advance(1300);
    e.pointerMove(600, 470, false, 1400);
    const TrackId entered = e.pointerRow().track;
    e.advance(1459);
    check(e.enlarged().track == 31 && entered != 31, "T34: entering another row ends the hold");
    e.advance(1460);
    check(e.enlarged().track == entered, "T34: and it grows 60 ms later");
    Fixture h = laneFixture();
    h.add(2, "Other");
    auto d = model(h);
    d.openFromChip(1, ItemKey{0, 105}, 10);
    d.advance(500);
    check(d.focus().item.place == ItemPlace::Chain && d.focus().item.device == 105 && d.lane(0).first >= 1,
          "m10: a single ADI-Q or ADI-L inserted as a device lands on that device");
    check(d.openFromChip(77, ItemKey{}, 600).refused, "m10: a track that is no row is refused");
}

// --- the navigator --------------------------------------------------------------------------------------

void navigatorGeometry() {
    section("T35 the navigator and the chip");
    Fixture f = uniform(40);
    auto m = model(f);
    NavGeo n = m.navigator();
    check(n.shown && n.header == Rect{1140, 0, 124, 16} && n.frame.y == 16 && n.frame.h == 70 && n.scroll == 0,
          "T35: header, frame over rows 0 to 4");
    check(m.navigatorLine(3).y == 16 + 42 && m.navigatorLine(3).onScreen && !m.navigatorLine(5).onScreen,
          "T35: line i at 16 + 14 i, on screen or not");
    m.resetStats();
    std::size_t lines = 0;
    m.forEachNavigatorLine([&](const NavLine &) { ++lines; });
    check(lines == 40 && m.stats().lineEvaluations == 40, "T35: every line fits (40 x 14 <= 584)");
    Fixture big = uniform(100);
    auto b = model(big);
    b.scrollTo(7860, 10);
    n = b.navigator();
    check(n.scroll == 326 && n.frame.y == 530 && n.frame.h == 70, "T35: scrolled to rows 60 to 64: frame at 530 to 600");
    Fixture e = eleven();
    auto el = model(e);
    el.pin(el.rowRef(3), Mods{}, 10);
    check(el.navigatorLine(8).indent == 8 && el.navigatorLine(9).indent == 8 && el.navigatorLine(7).group &&
              el.navigatorLine(2).pinned,
          "T35: tracks 9 and 10 (lines 8 and 9) indented, 8 a group, 3 pinned");
    el.setNavigator(true, true, 20);
    check(el.navigator().collapsed && el.navigator().area.w == 20 && el.plan().navW == 28,
          "T35: collapsed: 28 with the same lines");
    el.setNavigator(false, false, 30);
    check(!el.navigator().shown && el.plan().laneW == 376 + 132, "T35: hidden: the lane gains 132");
    // the chip at each base
    Fixture u = uniform(20);
    auto c = model(u);
    const double top = c.row(0).top;
    ChipGeo g = c.chip(0);
    check(g.chip == Rect{6, top + 41, 116, 18} && g.buttonsAbove && g.pin.x == 6 && g.collision.x == 26 && g.kick.x == 46 &&
              g.pin.y == top + 21,
          "T35: at base 100 the buttons above the chip");
    c.setBaseHeight(28, 10);
    g = c.chip(0);
    check(g.chip == Rect{6, c.row(0).top + 5, 56, 18} && !g.buttonsAbove && g.pin.x == 66 && g.collision.x == 86 &&
              g.kick.x == 106 && g.pin.y == c.row(0).top + 6,
          "T35: at base 28 beside it");
    c.setBaseHeight(44, 20);
    check(c.chip(0).chip.y == c.row(0).top + 13 && !c.chip(0).buttonsAbove, "T35: at 44 beside it");
    c.setBaseHeight(68, 30);
    check(c.chip(0).buttonsAbove && c.chip(0).pin.y == c.row(0).top + 5, "T35: at 68 above again");
    c.setBaseHeight(28, 40);
    check(chipHit(c, 0).part == HitPart::Chip, "T35: the chip is a target at base 28");
    c.setBaseHeight(184, 50);
    check(chipHit(c, 0).part == HitPart::Chip, "T35: and at 184");
    check(el.chip(8).chip.x == 14, "T35: at depth 1 the chip starts at 14");
    // only the visible lines are computed
    Fixture many = uniform(20000);
    auto v = model(many);
    v.resetStats();
    std::size_t shown = 0;
    v.forEachNavigatorLine([&](const NavLine &) { ++shown; });
    check(shown == v.navigator().lineSpan.size() && v.stats().lineEvaluations == shown && shown < 50,
          "T35: 20000 rows: only the lines in view");
}

void navigatorClick() {
    section("T36 T37 T38 the navigator's click, drag and wheel");
    Fixture f = uniform(100);
    auto m = model(f);
    m.setKeyboardFocus(true, 0);
    m.pointerMove(1200, 440, false, 900);
    auto fx = m.pointerDown(1200, 440, Button::Left, Mods{}, 1, 1000);
    fx = m.pointerUp(1200, 440, 1000);
    check(m.focus().row.track == 31 && fx.select == 31, "T36: a click on line 30 focuses and selects it");
    m.advance(1160);
    check(m.enlarged().track == 31 && m.row(30).top == 8 && m.scroll() == 3973, "T36: at 160 ms at view top 8");
    Fixture g = uniform(40);
    auto d = model(g);
    d.pointerMove(1200, 44, false, 10);
    d.pointerDown(1200, 44, Button::Left, Mods{}, 1, 20);
    d.pointerMove(1200, 184, true, 30);
    d.pointerUp(1200, 184, 40);
    check(d.scroll() == 1310 && !d.focus().row.valid(), "T37: a drag on the frame scrolls a row per line");
    Fixture h = uniform(40);
    auto c = model(h);
    c.pointerDown(1200, 46, Button::Left, Mods{}, 1, 10);
    c.pointerMove(1200, 48, true, 20);
    c.pointerUp(1200, 48, 30);
    check(c.focus().row.track == 3, "T37: a 2 px move is a click on line 2");
    Fixture i = uniform(40);
    auto w = model(i);
    WheelDelta notch;
    notch.notches = 1;
    w.wheel(1200, 300, notch, Mods{}, 10);
    check(w.scroll() == 131, "T38: the wheel over the navigator scrolls the list");
}

// --- search, pins, the reference ---------------------------------------------------------------------------

void filter() {
    section("T39 filter (m17: inside folds, a child without its group, pins only with none)");
    Fixture f = eleven();
    auto m = model(f);
    const auto is = [&](const char *query, std::vector<TrackId> expected) {
        m.setSearch(query, 10);
        return shownIds(m) == expected;
    };
    check(is("vox", {5, 6}) && is("VOX", {5, 6}), "T39: vox and VOX");
    check(is("cafe", {7}) && is("CAF\xC3\x89", {7}), "T39: cafe and CAFE with an accent");
    check(is("eq eight", {3}), "T39: a chain device's name");
    check(is("adi-l", {4}) && is("adi-q", {}), "T39: an engaged module's name, not a dormant one");
    check(is("vocal", {5}) && is("fx", {8, 9, 10}) && is("main", {11}) && is("zzz", {}) && is("ref", {}),
          "T39: roles, the group's name, Main, nothing, never the Reference");
    m.setSearch("riser", 20);
    check(shownIds(m) == std::vector<TrackId>{9} && m.row(0).depth == 1, "m17: a child shown alone keeps its depth");
    m.setSearch("vox", 30);
    std::size_t lines = 0;
    m.forEachNavigatorLine([&](const NavLine &l) { lines += l.hit ? 1u : 0u; });
    check(lines == 2 && m.navigator().lineSpan.size() == 2, "T39: the navigator has exactly the shown lines");
    m.pin(m.rowRef(3), Mods{}, 40);
    m.pin(m.rowRef(5), Mods{}, 50);
    m.setFilterPinned(true, 60);
    check(shownIds(m) == std::vector<TrackId>{5}, "T39: pins only and a search both apply");
    m.setFilterPinned(false, 70);
    m.setSearch("", 80);
    m.toggleFold(m.rowRef(8), 90);
    m.setSearch("impact", 100);
    check(shownIds(m) == std::vector<TrackId>{10}, "m17: a search finds what a fold hides");
    m.setSearch("", 110);
    check(!m.shownIndex(10), "m17: the fold applies again without it");
    m.setSearch("", 115);
    m.scrollTo(300, 120);
    m.setSearch("a", 130);
    check(m.scroll() == 0 && m.shownCount() > 3, "T39: a new query starts at the top");
    Fixture g = eleven();
    auto p = model(g);
    p.setFilterPinned(true, 10);
    check(p.shownCount() == 0 && p.contentHeight() == 0, "m17: pins only with no pins shows no row");
}

void quickJump() {
    section("T40 the jump search");
    Fixture f = eleven();
    auto m = model(f);
    m.setSearchMode(SearchMode::Jump, 0);
    m.setSearch("vox", 10);
    check(m.shownCount() == 11 && m.jumpTarget().track == 5, "T40: every row shown, the first match targeted");
    m.advance(150);
    check(m.scroll() == 516 && m.row(4).top == 8 && m.maxScroll() == 810, "T40: scrolled to put it 8 px from the top");
    m.searchKey(Key::Down, 200);
    m.advance(400);
    check(m.jumpTarget().track == 6 && m.scroll() == 647, "T40: Down steps to the next");
    m.searchKey(Key::Down, 500);
    check(m.jumpTarget().track == 6, "T40: no wrap");
    m.searchKey(Key::Up, 600);
    check(m.jumpTarget().track == 5, "T40: Up steps back");
    auto fx = m.searchKey(Key::Enter, 700);
    check(m.focus().row.track == 5 && fx.select == 5 && fx.takeFocus, "T40: Enter focuses and hands the keys to the list");
    m.advance(1000);
    check(m.enlarged().track == 5, "T40: then it is enlarged and revealed");
    check(m.navigatorLine(4).hit && m.navigatorLine(5).hit && !m.navigatorLine(3).hit, "T40: the navigator marks the hits");
}

void folding() {
    section("T41 folding the search text");
    std::string out;
    const auto fold = [&](std::string_view in) {
        foldForSearch(in, out);
        return out;
    };
    check(fold("Caf\xC3\xA9") == "cafe" && fold("\xC3\x9C" "BER") == "uber" && fold("Stra\xC3\x9F" "e") == "strasse" &&
              fold("ADI-L") == "adi-l",
          "T41: accents, the sharp s, ASCII case");
    check(fold("\xCE\xA9mega") == "\xCE\xA9mega", "T41: other code points copied");
    check(fold("\xFF") == "\xFF" && fold("a\tb") == "ab", "T41: a lone byte copied, controls dropped");
    check(fold("\xC5\x92uvre \xC4\xB2") == "oeuvre ij" && fold("\xC3\x86") == "ae", "T41: ligatures");
    std::string kept;
    normaliseSearchText(std::string(126, 'a') + "\xC3\xA9\xC3\xA9", 128, kept);
    check(kept.size() == 128 && kept.back() == '\xA9', "T41: the stored text is cut at a character boundary");
    normaliseSearchText(std::string(127, 'a') + "\xC3\xA9", 128, kept);
    check(kept.size() == 127, "T41: never through the middle of one");
}

void pins() {
    section("T42 pins (m17: Shift's range in the shown order)");
    Fixture f = eleven();
    auto m = model(f);
    const auto pinned = [&] { return m.pins(); };
    auto fx = m.pin(m.rowRef(3), Mods{}, 10);
    check(pinned() == std::vector<TrackId>{3} && fx.sharedViewChanged && !fx.windowViewChanged, "T42: a click pins");
    Mods shift;
    shift.shift = true;
    Mods alt;
    alt.alt = true;
    m.pin(m.rowRef(6), shift, 20);
    check(pinned() == std::vector<TrackId>{3, 4, 5, 6}, "T42: Shift pins the range");
    m.pin(m.rowRef(1), alt, 30);
    check(pinned() == std::vector<TrackId>{1}, "T42: Alt pins only this one");
    m.pin(m.rowRef(1), Mods{}, 40);
    check(pinned().empty(), "T42: a click unpins");
    fx = m.pinSameColour(m.rowRef(5), 50);
    check(pinned() == std::vector<TrackId>{5, 6} && fx.sharedViewChanged, "T42: the same colour");
    m.storePinSet("Vox", 60);
    m.pin(m.rowRef(3), Mods{}, 70);
    check(pinned() == std::vector<TrackId>{3, 5, 6}, "T42: one more");
    m.recallPinSet("Vox", 80);
    check(pinned() == std::vector<TrackId>{5, 6}, "T42: a stored set comes back");
    m.setFilterPinned(true, 90);
    check(shownIds(m) == std::vector<TrackId>{5, 6}, "T42: pins only");
    m.pinSameColour(m.rowRef(7), 100);
    check(pinned() == std::vector<TrackId>{5, 6, 7} && shownIds(m) == std::vector<TrackId>{5, 6, 7},
          "T42: a track without a colour pins itself");
    for (int k = 0; k < 31; ++k)
        m.storePinSet("set " + std::to_string(k), 110);
    check(m.storePinSet("one too many", 120).refused && m.sharedView().pinSets.size() == 32, "T42: 32 sets at most");
    check(m.storePinSet(std::string(65, 'x'), 130).refused, "T42: a 65-byte name is refused");
    // m17: the range runs in the shown order, from the last plain click
    Fixture g = eleven();
    auto r = model(g);
    r.setSearch("i", 10); // Kick, Kick Sub, Snare (EQ Eight), Bass Synth (its ADI-L), Riser, Impact, Main
    check(shownIds(r) == std::vector<TrackId>{1, 2, 3, 4, 9, 10, 11}, "m17: the filtered rows");
    r.pin(r.rowRef(2), Mods{}, 20);
    r.pin(r.rowRef(10), shift, 30);
    check(r.pins() == std::vector<TrackId>{2, 3, 4, 9, 10}, "m17: Shift's range is the shown rows between");
    r.setSearch("vox", 40); // the anchor (10) is not shown: a plain pin
    r.pin(r.rowRef(6), shift, 50);
    check(r.pins() == std::vector<TrackId>{2, 3, 4, 6, 9, 10}, "m17: with its anchor hidden Shift pins one row");
}

void reference() {
    section("T43 the comparison pair and the collision reference");
    Fixture f = eleven();
    auto m = model(f);
    m.pointerMove(1200, 16 + 14 * 3 + 5, false, 5);
    Mods shift;
    shift.shift = true;
    m.pointerDown(1200, 16 + 14 * 3 + 5, Button::Left, shift, 1, 10);
    m.pointerUp(1200, 16 + 14 * 3 + 5, 10);
    check(m.navigatorLine(3).pairPending, "T43: the first Shift-click waits");
    m.pointerDown(1200, 16 + 5, Button::Left, shift, 1, 20);
    auto fx = m.pointerUp(1200, 16 + 5, 20);
    check(m.comparePair() == ComparePair{4, 1} && fx.sharedViewChanged, "T43: Shift-click lines of tracks 4 then 1");
    check(m.collisionReference(m.rowRef(4)) == 1 && m.collisionReference(m.rowRef(1)) == 4 &&
              m.collisionReference(m.rowRef(3)) == 4,
          "T43: with a pair its first member is everyone's reference, and the two each other's");
    m.clearComparePair(30);
    check(m.collisionReference(m.rowRef(4)) == 1 && m.collisionReference(m.rowRef(1)) == 4 &&
              m.collisionReference(m.rowRef(3)) == 0,
          "T43: without a pair: the role partner, else nothing to compare with");
    m.focusRow(m.rowRef(6), 40);
    check(m.collisionReference(m.rowRef(3)) == 6, "T43: else the focused row");
    m.openFromChip(2, ItemKey{}, 50);
    m.focusRow(m.rowRef(6), 60);
    check(m.collisionReference(m.rowRef(3)) == 2, "T43: the opened-from track before the focus");
    m.setCollisionRule(CollisionRule::Focused, 70);
    check(m.collisionReference(m.rowRef(3)) == 6, "T43: the Focused rule");
    m.collisionButton(m.rowRef(3), 80);
    check(m.comparePair() == ComparePair{3, 6}, "T43: the collision reference button: (this row, the focused row)");
    m.clearComparePair(90);
    m.pointerDown(1200, 16 + 14 * 2 + 5, Button::Left, shift, 1, 100);
    m.pointerUp(1200, 16 + 14 * 2 + 5, 100);
    m.pointerDown(1200, 16 + 14 * 2 + 5, Button::Left, shift, 1, 110);
    m.pointerUp(1200, 16 + 14 * 2 + 5, 110);
    check(!m.navigatorLine(2).pairPending && m.comparePair() == ComparePair{},
          "T43: the same line twice clears the waiting one");
}

// --- persistence, adapter, determinism, constants, separation ----------------------------------------------

void viewStateRoundTrip() {
    section("T44 view state (m14: the offset at rest)");
    Fixture f = eleven();
    auto m = model(f, 1264, 200);
    TimeMs t = 10;
    m.setBaseHeight(68, t);
    m.setAutoZoom(false, t += 10);
    m.setNavigator(true, true, t += 10);
    m.setSearchMode(SearchMode::Jump, t += 10);
    m.setSearch("vox", t += 10);
    m.setRowLayout(RowLayout::ProcessorFirst, t += 10);
    m.setShowChain(false, t += 10);
    m.setShowDormant(false, t += 10);
    m.setRowSpectrum(RowSpectrum::Off, t += 10);
    m.setCollisionRule(CollisionRule::Focused, t += 10);
    m.setOverlay(true, t += 10);
    m.setLook(Look::Dark, t += 10);
    m.setModuleRange(RangeEntry{4, kL, 0, DisplayRange::Db12}, t += 10);
    m.toggleFold(m.rowRef(8), t += 10);
    m.pin(m.rowRef(3), Mods{}, t += 10);
    m.pin(m.rowRef(5), Mods{}, t += 10);
    m.setComparePair(4, 1, t += 10);
    m.setModuleLayout(ModuleLayout::Card, t += 10);
    m.advance(t += 500);
    m.scrollTo(6 * 89 + 12, t += 10);
    m.advance(t += 500);
    m.storePinSet("Drums", t += 10);
    ViewState vs = m.viewState();
    check(vs.window.scroll.track == 7 && vs.window.scroll.offset == 12, "T44: the scroll as track 7 plus 12");
    StackModel fresh;
    fresh.setViewport(1264, 200, 0);
    Fixture g = eleven();
    fresh.setRows(g.input(), 0);
    fresh.applyViewState(vs, 10);
    fresh.advance(1000);
    check(fresh.viewState() == vs, "T44: the view state round-trips");
    bool rows = fresh.visible() == m.visible();
    for (std::size_t i = 0; i < m.shownCount(); ++i)
        rows = rows && fresh.row(i) == m.row(i);
    check(rows && fresh.plan() == m.plan() && fresh.navigator() == m.navigator(),
          "T44: the same rows, columns and navigator at rest");
    std::swap(g.tracks[0], g.tracks[1]);
    std::swap(g.chains[0], g.chains[1]);
    std::swap_ranges(g.modules.begin(), g.modules.begin() + 4, g.modules.begin() + 4);
    g.generation = 2;
    fresh.setRows(g.input(), 2000);
    check(fresh.shownTrack(0) == 2, "T44: order is the input's, never stored");
    ViewState wild;
    wild.window.baseHeight = 500;
    wild.window.search = std::string(299, 'a') + "\xC3\xA9";
    wild.shared.pins = {-3, 0, 5, 5, 9};
    for (int k = 0; k < 40; ++k)
        wild.shared.pinSets.push_back(PinSet{"s" + std::to_string(k), {1}});
    wild.shared.comparePair = {5, 5};
    wild.window.scroll = {7, std::nan("")};
    const ViewState n = wild.normalised(Constants{});
    check(n.window.baseHeight == 184 && n.window.search.size() <= 128 && n.shared.pins == std::vector<TrackId>{5, 9} &&
              n.shared.pinSets.size() == 32 && n.shared.comparePair == ComparePair{} && n.window.scroll.offset == 0,
          "T44: normalised: base 184, search cut, pins, 32 sets, pair cleared, NaN offset 0");
    wild.window.baseHeight = 10;
    check(wild.normalised(Constants{}).window.baseHeight == 28, "T44: base 10 to 28");
    // the pins of a deleted track stay and draw nothing; it comes back pinned
    Fixture h = eleven();
    auto p = model(h);
    p.pin(p.rowRef(6), Mods{}, 10);
    Fixture gone = h;
    gone.erase(6);
    gone.generation = 2;
    p.setRows(gone.input(), 20);
    check(p.pinned(6) && !p.shownIndex(6), "T44: a deleted track's pin is kept");
    h.generation = 3;
    p.setRows(h.input(), 30);
    check(p.row(*p.shownIndex(6)).pinned, "T44: and its track comes back pinned");
    // a stored pin of a track not in the rows survives applyViewState (an undone delete gets it back)
    ViewState ghost = p.viewState();
    ghost.shared.pins.push_back(999);
    std::sort(ghost.shared.pins.begin(), ghost.shared.pins.end());
    p.applyViewState(ghost, 40);
    const ViewState exported = p.viewState();
    check(p.pinned(999) && p.pinned(6) &&
              std::binary_search(exported.shared.pins.begin(), exported.shared.pins.end(), 999),
          "T44: a pin of a track not in the rows survives applyViewState and is exported again");
    // filterPinned round-trips and the fresh model shows the same rows
    p.setFilterPinned(true, 50);
    const ViewState onlyPinned = p.viewState();
    StackModel q;
    q.setViewport(1264, 600, 0);
    Fixture hq = eleven();
    q.setRows(hq.input(), 0);
    q.applyViewState(onlyPinned, 10);
    check(onlyPinned.window.filterPinned && q.windowView().filterPinned && shownIds(q) == shownIds(p) &&
              shownIds(p) == std::vector<TrackId>{6},
          "T44: filterPinned round-trips: the fresh model shows the pinned rows only");
    // m14: an enlarged first row does not move the reopened view
    Fixture e = uniform(40);
    auto en = model(e);
    en.focusRow(en.rowRef(1), 10);
    en.advance(500);
    en.scrollTo(150, 600);
    check(en.viewState().window.scroll.track == 1 && en.viewState().window.scroll.offset == 100,
          "m14: an offset inside an enlarged row is kept to the row at rest");
}

class V1Provider final : public MixViewProvider {
  public:
    ChannelSlots slots(const adi::engine::TrackNode &t) const noexcept override {
        ChannelSlots s;
        s.eq = t.id == 1;
        s.sidechain = t.id == 3 ? SidechainMode::Rmsc : SidechainMode::Off;
        s.limiter = t.id == 5;
        s.limiterFull = t.kind == "master";
        return s;
    }
    bool live() const noexcept override { return true; }
    std::int64_t reference(const std::vector<std::shared_ptr<const adi::engine::TrackNode>> &) const noexcept override {
        return 6;
    }
};

void adapter() {
    section("T45 the snapshot as it is");
    auto snapshot = std::make_shared<adi::engine::Snapshot>();
    snapshot->seq = 1;
    const auto add = [&](std::int64_t id, std::int64_t parent, std::int64_t index, const char *kind, const char *name) {
        auto t = std::make_shared<adi::engine::TrackNode>();
        t->id = id;
        t->parent = parent;
        t->indexInParent = index;
        t->kind = kind;
        t->name = name;
        snapshot->tracks.push_back(std::move(t));
    };
    add(1, 0, 0, "audio", "Drums");
    add(2, 0, 1, "group", "Bus");
    add(3, 2, 0, "audio", "Pad");
    add(4, 0, 2, "return", "Reverb");
    add(5, 0, 3, "master", "Main");
    add(6, 0, 4, "audio", "Ref");
    SnapshotReader reader{snapshot};
    ArrangementGeometry geometry;
    std::vector<TrackRow> rows;
    rowsFromSnapshot(reader, rows);
    geometry.order.build(reader, rows);
    geometry.hidden = {4};
    V1Provider provider;
    SnapshotRows out;
    tracksFromSnapshot(reader, geometry, provider, out);
    std::vector<TrackId> order;
    for (const auto &t : out.tracks)
        order.push_back(t.id);
    check(order == std::vector<TrackId>{1, 2, 3, 6, 4, 5}, "T45: TrackOrder's order: returns, then Main, last");
    const auto in = [&](TrackId id) -> const TrackInput & {
        for (const auto &t : out.tracks)
            if (t.id == id)
                return t;
        return out.tracks.front();
    };
    check(in(3).depth == 1 && in(3).parent == 2 && !in(4).inView && in(6).reference && in(2).group && in(5).main,
          "T45: depth, the project view, the Reference, groups and Main");
    check(out.kinds.size() == 3 && out.kinds[0].key == 1 && out.kinds[1].key == 2 && out.kinds[2].key == 4,
          "T45: today's provider reports ADI-Q, ducking and ADI-L");
    const auto state = [&](TrackId id, std::size_t k) {
        for (std::size_t i = 0; i < out.tracks.size(); ++i)
            if (out.tracks[i].id == id)
                return out.modules[i * 3 + k].state;
        return ModuleState::Absent;
    };
    check(state(1, 0) == ModuleState::Engaged && state(3, 1) == ModuleState::Engaged && state(5, 2) == ModuleState::Engaged &&
              state(5, 1) == ModuleState::Absent && state(1, 2) == ModuleState::Dormant && state(6, 0) == ModuleState::Absent,
          "T45: the modules from v1, no ducking on Main");
    check(in(1).identity != 0 && in(1).identity == in(3).identity && in(1).identity != in(2).identity,
          "T45: the identity is the track kind");
    audit = true;
    allocations = 0;
    tracksFromSnapshot(reader, geometry, provider, out);
    audit = false;
    check(allocations == 0, "T45: a second call allocates nothing");
    StackModel m;
    m.setViewport(1264, 600, 0);
    m.setChannelScope(ChannelScope::AllChannels, 0);
    m.setRows(out.input(1), 0);
    check(shownIds(m) == std::vector<TrackId>{1, 2, 3, 5}, "T45: the model shows 1, 2, 3 and Main");
}

void actualChainAdapter() {
    section("Actual chain publication, identity and edit retention");
    auto snapshot = std::make_shared<adi::engine::Snapshot>();
    snapshot->seq = 23;
    for (const auto id : {1, 2}) {
        auto track = std::make_shared<adi::engine::TrackNode>();
        track->id = id;
        track->indexInParent = id;
        track->kind = "audio";
        track->name = "Same name"; // duplicate labels must not become identity
        snapshot->tracks.push_back(std::move(track));
    }
    SnapshotReader reader{snapshot};
    ArrangementGeometry geometry;
    std::vector<TrackRow> rows;
    rowsFromSnapshot(reader, rows);
    geometry.order.build(reader, rows);
    StubMixViewProvider provider;
    std::vector<ChainDeviceInput> devices{
        {2, 301, 4001, ItemClass::Plugin, "Same device"},
        {1, 105, 2005, ItemClass::Plugin, "Same device"},
        {999, 900, 9001, ItemClass::Plugin, "Unknown track"},
        {1, 101, 2001, ItemClass::Eq, "First", true, false},
        {1, 102, 2002, ItemClass::Missing, "Unavailable", false, true},
        {1, 103, 0, ItemClass::Plugin, "Invalid identity"},
        {1, 0, 2010, ItemClass::Plugin, "Invalid id"},
    };
    SnapshotRows out;
    const auto refresh = [&] { tracksFromSnapshot(reader, geometry, provider, devices, out); };
    refresh();
    check(out.kinds.empty() && out.modules.empty(), "actual chains reserve no module columns");
    check(out.tracks.size() == 2 && out.items.size() == 4 && out.tracks[0].itemBegin == 0 &&
              out.tracks[0].itemCount == 3 && out.tracks[1].itemBegin == 3 && out.tracks[1].itemCount == 1,
          "actual chain adapter groups interleaved owners, omitting unknown owners and invalid ids");
    check(out.items[0].device == 105 && out.items[1].device == 101 && out.items[2].device == 102 &&
              out.items[3].device == 301,
          "chain order comes from the publication, not numeric device ids or duplicate names");
    check(!out.items[1].enabled && out.items[1].available && !out.items[2].available && out.items[2].enabled,
          "availability and Device Activator are independent publication fields");
    audit = true;
    allocations = 0;
    refresh();
    audit = false;
    check(allocations == 0, "actual chain adapter reuses all scratch capacities on steady input");

    StackModel model;
    model.setViewport(1800, 600, 0);
    model.setChannelScope(ChannelScope::AllChannels, 0);
    model.setRows(out.input(reader.seq(), 1), 0);
    const auto target = model.chainRef(1, 105);
    model.focusItem(target, 1);
    model.setSelection(2, 2);
    const auto begin = model.beginCapture(CaptureKind::Drag, target, 200);
    check(begin.refusal == CaptureRefusal::None, "an actual-device drag captures its instance");
    const auto firstGeometry = model.row(*model.shownIndex(1));
    const auto bypassed = model.resolve(model.chainRef(1, 101));
    const auto missing = model.resolve(model.chainRef(1, 102));
    check(bypassed && bypassed->available && !bypassed->enabled && bypassed->state == ModuleState::Dormant &&
              missing && !missing->available && missing->enabled && missing->state == ModuleState::Absent,
          "resolve exposes bypass and unavailable status without dropping either chain device");
    bool sawBypass = false, sawMissing = false;
    model.forEachChainItem(*model.shownIndex(1), [&](const ItemGeo &item) {
        sawBypass = sawBypass || (item.ref.device == 101 && item.available && !item.enabled);
        sawMissing = sawMissing || (item.ref.device == 102 && !item.available && item.enabled);
    });
    check(sawBypass && sawMissing, "realized chain geometry preserves per-device availability and activation");

    devices[1].enabled = false;
    refresh();
    model.setRows(out.input(reader.seq(), 2), 205);
    bool sawCurrentActivator = false;
    model.forEachChainItem(*model.shownIndex(1), [&](const ItemGeo &item) {
        if (item.ref.device == 105)
            sawCurrentActivator = !item.enabled && item.available;
    });
    check(sawCurrentActivator && model.captureStatus(begin.ref) == CaptureStatus::Live,
          "a captured lane freezes positions while showing current activator state");

    std::swap(devices[1], devices[3]);
    devices[3].name = "Renamed";
    refresh();
    const auto reorder = model.setRows(out.input(reader.seq(), 3), 210);
    check(reorder.captureLost == CaptureStatus::Live && model.captureStatus(begin.ref) == CaptureStatus::Live &&
              model.chainRef(1, 105) == target,
          "reorder and rename preserve actual target identity during a drag");
    check(model.focus().item == target && model.row(*model.shownIndex(2)).selected &&
              near(model.row(*model.shownIndex(1)).top, firstGeometry.top),
          "actual device refresh preserves separate focus/selection and the captured row anchor");
    devices[3].identity = 9005;
    refresh();
    const auto replacement = model.setRows(out.input(reader.seq(), 4), 220);
    check(replacement.captureLost == CaptureStatus::ItemReplaced && !model.live(target) &&
              model.captureStatus(begin.ref) == CaptureStatus::ItemReplaced,
          "same numeric device id with a new instance cancels the old drag exactly once");
    check(model.setRows(out.input(reader.seq(), 5), 221).captureLost == CaptureStatus::Live,
          "a replacement cancellation is not re-emitted on the next publication");

    const auto replacementRef = model.chainRef(1, 105);
    const auto menu = model.beginCapture(CaptureKind::Menu, replacementRef, 230);
    const auto removed = devices[3];
    devices.erase(devices.begin() + 3);
    refresh();
    const auto deletion = model.setRows(out.input(reader.seq(), 6), 240);
    check(deletion.captureLost == CaptureStatus::ItemRemoved && !model.live(replacementRef),
          "deleting an actual device ends its open menu and invalidates the edit target");
    devices.insert(devices.begin() + 3, removed); // the document's undo restores the row
    refresh();
    model.setRows(out.input(reader.seq(), 7), 250);
    check(!model.live(replacementRef) && model.live(model.chainRef(1, 105)) &&
              model.captureStatus(menu.ref) == CaptureStatus::ItemRemoved,
          "undo restores an editable device without resurrecting its old menu or stale handle");
    check(model.resolve(model.chainRef(1, 105))->identity == 9005,
          "the restored target carries the publication's identity rather than a name/index hash");

    // Eligibility is a stable publication field, independent of the whole chain's
    // display classes, user labels, availability and activation.
    auto native = std::find_if(devices.begin(), devices.end(), [](const ChainDeviceInput &d) { return d.device == 101; });
    native->mixFunction = true;
    refresh();
    model.setRows(out.input(reader.seq(), 8), 260);
    model.setChannelScope(ChannelScope::WithTools, 270);
    check(out.tracks[0].hasMixFunction && !out.tracks[1].hasMixFunction &&
              shownIds(model) == std::vector<TrackId>{1},
          "channel membership uses the explicit function flag, including a bypassed native function");
    std::vector<DeviceId> retainedOrder;
    if (const auto row = model.shownIndex(1))
        model.forEachChainItem(*row, [&](const ItemGeo &item) { retainedOrder.push_back(item.ref.device); });
    check(retainedOrder == std::vector<DeviceId>{101, 105, 102} && model.live(model.chainRef(2, 301)),
          "With tools retains ordinary and unavailable inserts in order and keeps hidden track records");
    const auto nativeRef = model.chainRef(1, 101);
    native->available = false;
    native->enabled = true;
    native->name = "User-renamed unavailable function";
    refresh();
    model.setRows(out.input(reader.seq(), 9), 280);
    const auto resolvedNative = model.resolve(nativeRef);
    check(model.shownIndex(1) && model.chainRef(1, 101) == nativeRef && resolvedNative &&
              !resolvedNative->available && resolvedNative->enabled,
          "rename, runtime unavailability and activator changes preserve native membership and target identity");
    native->mixFunction = false;
    for (auto &device : devices)
        if (device.device == 103 || device.device == 0 || device.track == 999)
            device.mixFunction = true;
    refresh();
    model.setRows(out.input(reader.seq(), 10), 290);
    check(model.shownCount() == 0 && model.live(nativeRef) && out.items.size() == 4,
          "invalid device identities and unknown owners cannot make a channel eligible");
    model.setChannelScope(ChannelScope::AllChannels, 300);
    check(shownIds(model) == std::vector<TrackId>{1, 2} && model.chainRef(1, 101) == nativeRef &&
              model.selection() == 2,
          "All channels restores ordinary-only rows without replacing devices or changing selection");
}

void channelMembership() {
    section("Channel membership, hidden records and capture retention");
    check(!TrackInput{}.hasMixFunction && !ChainDeviceInput{}.mixFunction &&
              WindowViewState{}.channelScope == ChannelScope::WithTools,
          "a fresh model requires explicit tool membership and defaults to With tools");

    Fixture f;
    f.kinds.clear();
    f.add(1, "Family Bus", 0, 0, kGroupKind);
    f.track(1).group = true;
    f.add(2, "Match tools", 1, 1);
    f.track(2).roles = role::Bass;
    f.add(3, "Kick key");
    f.track(3).roles = role::Kick;
    f.add(4, "Match ordinary");
    f.add(5, "Reference");
    f.track(5).reference = true;
    f.add(6, "Outside project view");
    f.track(6).inView = false;
    for (auto &track : f.tracks)
        track.hasMixFunction = track.id == 2 || track.id == 5 || track.id == 6;
    f.item(2, 21, ItemClass::Plugin, "Ordinary insert");
    f.item(2, 22, ItemClass::View, "Native function");
    f.item(4, 41, ItemClass::Plugin, "Ordinary only");
    f.generation = 1;
    auto m = model(f, 2400, 900);
    TimeMs now = 0;
    m.setAutoZoom(false, ++now);
    const auto child = m.rowRef(2);
    const auto ordinary = m.chainRef(4, 41);
    check(shownIds(m) == std::vector<TrackId>{2} && m.live(m.rowRef(1)) && m.live(m.rowRef(3)) &&
              m.live(ordinary) && m.row(0).depth == 1,
          "With tools filters shown membership while preserving full ancestry, role tracks and device refs");
    check(m.collisionReference(child) == 3,
          "an ordinary-only Kick remains the eligible Bass channel's role partner");
    m.setSearch("family", ++now);
    check(shownIds(m) == std::vector<TrackId>{2},
          "a hidden tool-free ancestor still contributes its name to descendant search");
    m.setSearch("", ++now);
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    m.toggleFold(m.rowRef(1), ++now);
    check(!m.shownIndex(2) && m.row(*m.shownIndex(1)).folded,
          "All channels applies a saved fold on a group without tools");
    m.setChannelScope(ChannelScope::WithTools, ++now);
    check(shownIds(m) == std::vector<TrackId>{2} && m.rowRef(2) == child,
          "With tools ignores an ineligible ancestor's saved fold so its child remains reachable");
    m.setSearch("family", ++now);
    check(shownIds(m) == std::vector<TrackId>{2},
          "the folded but ineligible ancestor still supplies descendant search text");
    m.setSearch("", ++now);
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    check(!m.shownIndex(2) && m.row(*m.shownIndex(1)).folded && m.rowRef(2) == child,
          "returning to All channels restores the saved ancestor fold without replacing the child");
    m.setChannelScope(ChannelScope::WithTools, ++now);
    f.track(1).hasMixFunction = true;
    ++f.generation;
    m.setRows(f.input(), ++now);
    check(shownIds(m) == std::vector<TrackId>{1} && m.row(0).folded && m.rowRef(2) == child,
          "a group gaining its first tool applies its existing fold to descendants in the same publication");
    f.track(1).hasMixFunction = false;
    ++f.generation;
    m.setRows(f.input(), ++now);
    check(shownIds(m) == std::vector<TrackId>{2} && m.rowRef(2) == child,
          "a group losing its last tool releases the descendant fold without replacing child identity");
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    check(!m.shownIndex(2) && m.row(*m.shownIndex(1)).folded,
          "eligibility changes preserve the saved fold for the All channels view");
    m.toggleFold(m.rowRef(1), ++now);
    m.setChannelScope(ChannelScope::WithTools, ++now);
    const auto selected = m.setSelection(4, ++now);
    check(m.selection() == 4 && !m.shownIndex(4) && selected.select == 0,
          "an externally selected ordinary-only channel stays selected while hidden");
    auto fx = m.setChannelScope(ChannelScope::AllChannels, ++now);
    check(fx.windowViewChanged && !fx.sharedViewChanged && fx.select == 0 &&
              shownIds(m) == std::vector<TrackId>{1, 2, 3, 4} && m.row(*m.shownIndex(4)).selected,
          "explicit All channels restores rows but still excludes Reference and project-hidden tracks");
    fx = m.setChannelScope(ChannelScope::WithTools, ++now);
    check(fx.select == 0 && m.selection() == 4 && m.rowRef(2) == child && m.chainRef(4, 41) == ordinary,
          "scope changes preserve the selected track id and stable row/device generations");

    m.setChannelScope(ChannelScope::AllChannels, ++now);
    m.pin(m.rowRef(2), Mods{}, ++now);
    m.pin(m.rowRef(4), Mods{}, ++now);
    m.setFilterPinned(true, ++now);
    m.setChannelScope(ChannelScope::WithTools, ++now);
    check(shownIds(m) == std::vector<TrackId>{2} && m.pinned(4),
          "pin membership does not override With tools and hidden pins remain stored");
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    check(shownIds(m) == std::vector<TrackId>{2, 4},
          "All channels intersects with the existing pinned-only filter");
    m.setFilterPinned(false, ++now);
    m.setSearchMode(SearchMode::Jump, ++now);
    m.setSearch("match", ++now);
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 4, "All channels includes an ordinary-only Jump match");
    m.setChannelScope(ChannelScope::WithTools, ++now);
    check(m.jumpTarget().track == 2, "With tools removes an ineligible current Jump match");
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 2, "Jump traversal cannot retain an out-of-scope match");
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    check(m.jumpTarget().track == 2, "a surviving Jump cursor keeps its track across scope expansion");
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 4, "scope expansion rebuilds the complete Jump match set");
    const auto state = m.viewState();
    auto restored = model(f, 2400, 900);
    restored.applyViewState(state, ++now);
    check(restored.windowView().channelScope == ChannelScope::AllChannels &&
              shownIds(restored) == shownIds(m) && restored.pins() == m.pins(),
          "explicit All channels round-trips alongside search and stored pins");
    auto invalid = state;
    invalid.window.channelScope = static_cast<ChannelScope>(255);
    check(invalid.normalised(Constants{}).window.channelScope == ChannelScope::WithTools &&
              m.setChannelScope(static_cast<ChannelScope>(255), ++now).refused &&
              m.windowView().channelScope == ChannelScope::AllChannels,
          "invalid restored scope normalises to With tools and an invalid command changes nothing");
    const auto jumpCapture = m.beginCapture(CaptureKind::Menu, ordinary, ++now);
    m.setChannelScope(ChannelScope::WithTools, ++now);
    check(jumpCapture.refusal == CaptureRefusal::None && m.captureStatus(jumpCapture.ref) == CaptureStatus::Live &&
              m.shownIndex(4) && m.jumpTarget().track == 4,
          "a captured ordinary Jump match remains available during the scope change");
    m.endCapture(jumpCapture.ref, ++now);
    check(!m.shownIndex(4) && m.jumpTarget().track == 2 && m.shownIndex(m.jumpTarget().track),
          "capture release removes the filtered row from Jump matches and selects a visible survivor");
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 2,
          "Jump Down after capture release cannot revisit the now-hidden ordinary channel");

    // A device publication can move without changing the project generation.
    Fixture updates;
    updates.kinds.clear();
    updates.add(1, "First");
    updates.add(2, "Second");
    updates.track(1).hasMixFunction = false;
    for (auto &track : updates.tracks)
        track.revision = 1;
    updates.generation = 77;
    StackModel incremental;
    incremental.setViewport(1800, 600, 0);
    const auto publish = [&](std::uint64_t revision) {
        auto input = updates.input();
        input.providerRevision = revision;
        return incremental.setRows(input, ++now);
    };
    publish(1);
    const auto first = incremental.rowRef(1);
    const auto second = incremental.rowRef(2);
    incremental.resetStats();
    updates.track(1).hasMixFunction = true;
    updates.track(1).revision = 2;
    publish(2);
    check(shownIds(incremental) == std::vector<TrackId>{1, 2} && incremental.rowRef(1) == first &&
              incremental.rowRef(2) == second && incremental.stats().rowUpdates == 1 &&
              incremental.stats().shownEdits == 1 && incremental.stats().fullPasses == 0,
          "a provider-only eligibility update inserts one shown row without replacing records or a full pass");
    updates.track(1).hasMixFunction = false;
    updates.track(1).revision = 3;
    publish(3);
    check(shownIds(incremental) == std::vector<TrackId>{2} && incremental.live(first) &&
              incremental.stats().rowUpdates == 2 && incremental.stats().shownEdits == 2,
          "the last-tool flag removal hides one row but its retained record stays live");
    const auto unchanged = publish(3);
    check(!unchanged.repaint, "an unchanged generation/publication pair remains a no-op");
    audit = true;
    allocations = 0;
    for (std::uint64_t revision = 4; revision < 12; ++revision)
        publish(revision);
    audit = false;
    check(allocations == 0 && incremental.stats().rowUpdates == 2,
          "steady provider refreshes with unchanged per-row tokens allocate nothing and update no rows");

    // Losing the last function is a presentation filter. A still-existing ordinary
    // target or chip retains its row; removal of the captured function is different.
    Fixture captures;
    captures.kinds.clear();
    captures.add(1, "Tools");
    captures.add(2, "Other tools");
    captures.item(1, 11, ItemClass::Plugin, "Ordinary retained target");
    captures.item(1, 12, ItemClass::View, "Last function");
    captures.generation = 1;
    auto held = model(captures, 2400, 600);
    held.setAutoZoom(false, ++now);
    const auto ordinaryTarget = held.chainRef(1, 11);
    auto capture = held.beginCapture(CaptureKind::Menu, ordinaryTarget, ++now);
    check(capture.refusal == CaptureRefusal::None, "an ordinary insert on an eligible channel can capture");
    captures.chains[0].pop_back();
    captures.track(1).hasMixFunction = false;
    ++captures.generation;
    fx = held.setRows(captures.input(), ++now);
    const auto retainedRow = held.shownIndex(1);
    check(fx.captureLost == CaptureStatus::Live && held.captureStatus(capture.ref) == CaptureStatus::Live &&
              retainedRow && held.row(*retainedRow).retained && held.chainRef(1, 11) == ordinaryTarget,
          "last-function deletion retains a live ordinary capture with its exact device id");
    held.endCapture(capture.ref, ++now);
    check(!held.shownIndex(1) && held.live(ordinaryTarget) && !held.capture(),
          "ordinary capture release applies With tools without deleting its target record");
    held.setChannelScope(ChannelScope::AllChannels, ++now);
    const auto chipRow = held.rowRef(1);
    capture = held.beginCapture(CaptureKind::Text, ItemRef{chipRow, ItemPlace::None, 0, 0, chipRow.gen}, ++now);
    held.setChannelScope(ChannelScope::WithTools, ++now);
    check(capture.refusal == CaptureRefusal::None && held.shownIndex(1) &&
              held.captureStatus(capture.ref) == CaptureStatus::Live && held.rowRef(1) == chipRow,
          "switching from All channels retains an ineligible chip text capture");
    held.endCapture(capture.ref, ++now);
    check(!held.shownIndex(1) && held.live(chipRow), "chip capture release reapplies the channel filter");
    captures.item(1, 12, ItemClass::View, "Restored function");
    captures.track(1).hasMixFunction = true;
    ++captures.generation;
    held.setRows(captures.input(), ++now);
    const auto functionTarget = held.chainRef(1, 12);
    capture = held.beginCapture(CaptureKind::Drag, functionTarget, ++now);
    check(capture.refusal == CaptureRefusal::None, "the last function itself can capture");
    captures.chains[0].pop_back();
    captures.track(1).hasMixFunction = false;
    ++captures.generation;
    fx = held.setRows(captures.input(), ++now);
    check(fx.captureLost == CaptureStatus::ItemRemoved && !held.capture() && !held.shownIndex(1) &&
              !held.live(functionTarget) && held.captureStatus(capture.ref) == CaptureStatus::ItemRemoved,
          "deleting the captured last function cancels that exact target and removes retained presentation");
    ++captures.generation;
    fx = held.setRows(captures.input(), ++now);
    check(fx.captureLost == CaptureStatus::Live, "last-function deletion cancellation is emitted only once");
    captures.item(1, 12, ItemClass::View, "Undo restored function");
    captures.track(1).hasMixFunction = true;
    ++captures.generation;
    held.setRows(captures.input(), ++now);
    const auto replacement = held.chainRef(1, 12);
    const auto nextCapture = held.beginCapture(CaptureKind::Menu, replacement, ++now);
    held.endCapture(capture.ref, ++now);
    check(nextCapture.refusal == CaptureRefusal::None && !held.live(functionTarget) && held.live(replacement) &&
              held.captureStatus(nextCapture.ref) == CaptureStatus::Live,
          "undo restores membership with a fresh item generation and stale capture completion cannot end its successor");
    held.endCapture(nextCapture.ref, ++now);
}

void channelShortcutScope() {
    section("Explicit channel shortcut scope");
    Fixture f;
    f.kinds.clear();
    f.add(1, "Bus", 0, 0, kGroupKind);
    f.track(1).group = true;
    f.add(2, "Ordinary child", 1, 1);
    f.add(3, "Empty");
    f.add(4, "Native");
    f.add(5, "Reference");
    f.track(5).reference = true;
    f.add(6, "Project hidden");
    f.track(6).inView = false;
    for (auto &track : f.tracks)
        track.hasMixFunction = track.id == 4;
    f.item(2, 21, ItemClass::Plugin, "Ordinary insert");
    auto m = model(f, 1600, 900);
    TimeMs now = 0;
    check(m.hiddenOnlyByToolScope(2) && m.hiddenOnlyByToolScope(3),
          "an ordinary chain and an empty channel are reachable by an explicit All channels switch");
    check(!m.hiddenOnlyByToolScope(4) && !m.hiddenOnlyByToolScope(999) &&
              !m.hiddenOnlyByToolScope(5) && !m.hiddenOnlyByToolScope(6),
          "eligible invalid reference and project-hidden targets cannot change the scope");
    check(m.windowView().channelScope == ChannelScope::WithTools && shownIds(m) == std::vector<TrackId>{4},
          "the shortcut query changes no membership focus selection or settings");
    m.setSearch("empty", ++now);
    check(m.hiddenOnlyByToolScope(3) && !m.hiddenOnlyByToolScope(2),
          "Filter search still decides whether the shortcut can reveal an ordinary channel");
    m.setSearchMode(SearchMode::Jump, ++now);
    check(m.hiddenOnlyByToolScope(2), "Jump search does not hide nonmatching rows");
    m.setSearch("", ++now);
    m.setFilterPinned(true, ++now);
    check(!m.hiddenOnlyByToolScope(2), "an unpinned target cannot override a pinned-only filter");
    m.pin(m.rowRef(2), Mods{}, ++now);
    check(m.hiddenOnlyByToolScope(2), "a pinned ordinary target can request All channels");
    m.setFilterPinned(false, ++now);
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    m.toggleFold(m.rowRef(1), ++now);
    m.setChannelScope(ChannelScope::WithTools, ++now);
    check(!m.hiddenOnlyByToolScope(2),
          "a saved tool-free ancestor fold remains an independent All channels exclusion");
    m.setSearchMode(SearchMode::Filter, ++now);
    m.setSearch("ordinary", ++now);
    check(m.hiddenOnlyByToolScope(2), "matching Filter search retains its existing fold override");
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    check(!m.hiddenOnlyByToolScope(2) && m.shownIndex(2),
          "an already All channels scope never requests another switch");
    const auto capture = m.beginCapture(CaptureKind::Menu, m.chainRef(2, 21), ++now);
    m.setChannelScope(ChannelScope::WithTools, ++now);
    check(capture.refusal == CaptureRefusal::None && m.shownIndex(2) && !m.hiddenOnlyByToolScope(2),
          "an ordinary row retained by an edit is already reachable and cannot change scope");
    m.endCapture(capture.ref, ++now);
    check(!m.shownIndex(2) && m.hiddenOnlyByToolScope(2),
          "the same ordinary row can request All channels once its retained edit ends");
}

void channelScopeJumpPins() {
    section("Jump matches follow pin-filter membership");
    Fixture f;
    f.kinds.clear();
    f.add(2, "Match two");
    f.add(4, "Match four");
    f.generation = 1;
    auto m = model(f, 1800, 600);
    TimeMs now = 0;
    m.setChannelScope(ChannelScope::AllChannels, ++now);
    m.setSearchMode(SearchMode::Jump, ++now);
    m.setSearch("match", ++now);
    m.searchKey(Key::Down, ++now);
    m.pin(m.rowRef(2), Mods{}, ++now);
    check(m.jumpTarget().track == 4 && shownIds(m) == std::vector<TrackId>{2, 4},
          "before pinned-only filtering, the current match can be an unpinned channel");
    m.setFilterPinned(true, ++now);
    check(shownIds(m) == std::vector<TrackId>{2} && m.jumpTarget().track == 2,
          "enabling pinned-only filtering drops an unpinned current Jump match");
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 2, "Jump Down cannot revisit a match hidden by pinned-only filtering");

    m.pin(m.rowRef(4), Mods{}, ++now);
    check(shownIds(m) == std::vector<TrackId>{2, 4} && m.jumpTarget().track == 2,
          "pinning a hidden match expands Jump membership while preserving its surviving cursor");
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 4, "a newly pinned matching channel is reachable by Jump Down");
    m.pin(m.rowRef(4), Mods{}, ++now);
    check(shownIds(m) == std::vector<TrackId>{2} && m.jumpTarget().track == 2,
          "unpinning the current match moves Jump to a remaining shown channel");

    m.pin(m.rowRef(4), Mods{}, ++now);
    Mods only;
    only.alt = true;
    m.pin(m.rowRef(4), only, ++now);
    check(m.pins() == std::vector<TrackId>{4} && shownIds(m) == std::vector<TrackId>{4} &&
              m.jumpTarget().track == 4,
          "bulk pin-only replacement rebuilds Jump matches when the old cursor leaves");
    m.searchKey(Key::Down, ++now);
    check(m.jumpTarget().track == 4, "bulk pin replacement leaves no stale hidden Jump entry");
    m.pin(m.rowRef(2), Mods{}, ++now);
    m.pin(m.rowRef(2), Mods{}, ++now);
    check(shownIds(m) == std::vector<TrackId>{4} && m.jumpTarget().track == 4,
          "pinning and unpinning another match preserves the current shown cursor");
    m.setFilterPinned(false, ++now);
    check(shownIds(m) == std::vector<TrackId>{2, 4} && m.jumpTarget().track == 4,
          "disabling pinned-only filtering expands matches without moving a surviving cursor");
    m.searchKey(Key::Up, ++now);
    check(m.jumpTarget().track == 2, "Jump Up reaches a matching channel restored by disabling the pin filter");
}

void jumpCapturePublicationChanges() {
    section("Jump cursor and captures survive record-vector mutation by stable ids");
    const auto fixture = [] {
        Fixture f;
        f.kinds.clear();
        f.add(10, "Match first");
        f.add(20, "Match last");
        f.item(10, 101, ItemClass::View, "First native function");
        f.item(20, 201, ItemClass::Plugin, "Captured ordinary insert");
        f.item(20, 202, ItemClass::View, "Last native function");
        f.generation = 1;
        return f;
    };
    {
        auto f = fixture();
        auto m = model(f, 2400, 600);
        TimeMs now = 0;
        m.setChannelScope(ChannelScope::AllChannels, ++now);
        m.setSearchMode(SearchMode::Jump, ++now);
        m.setSearch("match", ++now);
        m.searchKey(Key::Down, ++now);
        const auto survivor = m.rowRef(10);
        const auto removedRow = m.rowRef(20);
        const auto target = m.chainRef(20, 201);
        const auto capture = m.beginCapture(CaptureKind::Menu, target, ++now);
        check(m.jumpTarget().track == 20 && capture.refusal == CaptureRefusal::None,
              "the final matching actual-chain row owns the Jump cursor and item capture");
        f.erase(20);
        ++f.generation;
        auto fx = m.setRows(f.input(), ++now);
        check(fx.captureLost == CaptureStatus::RowRemoved && !m.capture() && !m.live(removedRow) &&
                  !m.live(target) && m.captureStatus(capture.ref) == CaptureStatus::RowRemoved,
              "shrinking the records by deleting the captured final row cancels its exact target");
        check(shownIds(m) == std::vector<TrackId>{10} && m.jumpTarget() == survivor && m.live(survivor),
              "after the records shrink, Jump resolves the visible survivor without using the old final index");
        ++f.generation;
        fx = m.setRows(f.input(), ++now);
        check(fx.captureLost == CaptureStatus::Live && m.jumpTarget() == survivor,
              "row-deletion cancellation is emitted once and later publications keep the survivor cursor");
        m.endCapture(capture.ref, ++now);
        check(!m.capture() && m.jumpTarget() == survivor,
              "a late end for the deleted row's capture cannot disturb the recovered Jump cursor");
        const auto next = m.beginCapture(CaptureKind::Menu, m.chainRef(10, 101), ++now);
        m.endCapture(capture.ref, ++now);
        check(next.refusal == CaptureRefusal::None && m.captureStatus(next.ref) == CaptureStatus::Live &&
                  m.jumpTarget() == survivor,
              "the deleted row's stale completion cannot end a new capture on its surviving neighbour");
        m.endCapture(next.ref, ++now);
    }

    for (const bool replace : {true, false}) {
        auto f = fixture();
        auto m = model(f, 2400, 600);
        TimeMs now = 0;
        m.setChannelScope(ChannelScope::AllChannels, ++now);
        m.setSearchMode(SearchMode::Jump, ++now);
        m.setSearch("match", ++now);
        m.searchKey(Key::Down, ++now);
        const auto trackedRow = m.rowRef(20);
        const auto target = m.chainRef(20, 201);
        const auto otherItem = m.chainRef(20, 202);
        const auto capture = m.beginCapture(CaptureKind::Menu, target, ++now);
        check(capture.refusal == CaptureRefusal::None && m.jumpTarget() == trackedRow,
              replace ? "replacement case starts with the final row captured and targeted"
                      : "removal case starts with the final row captured and targeted");
        std::swap(f.tracks[0], f.tracks[1]);
        std::swap(f.chains[0], f.chains[1]);
        ++f.generation;
        auto fx = m.setRows(f.input(), ++now);
        check(fx.captureLost == CaptureStatus::Live && m.captureStatus(capture.ref) == CaptureStatus::Live &&
                  shownIds(m) == std::vector<TrackId>{20, 10} && m.jumpTarget() == trackedRow &&
                  m.chainRef(20, 201) == target,
              replace ? "reordering before replacement preserves the live capture and cursor by track id"
                      : "reordering before removal preserves the live capture and cursor by track id");

        // Move the cursor's track back again: its old record index now names the
        // other matching track. Cancellation must carry the pre-update track id.
        std::swap(f.tracks[0], f.tracks[1]);
        std::swap(f.chains[0], f.chains[1]);
        auto &chain = f.chains[f.at(20)];
        if (replace)
            ++chain[0].identity;
        else
            chain.erase(chain.begin());
        ++f.generation;
        fx = m.setRows(f.input(), ++now);
        const auto expected = replace ? CaptureStatus::ItemReplaced : CaptureStatus::ItemRemoved;
        check(fx.captureLost == expected && m.captureStatus(capture.ref) == expected && !m.capture() &&
                  !m.live(target) && m.rowRef(20) == trackedRow,
              replace ? "same-count reorder plus replacement cancels the old device without replacing its row"
                      : "same-count reorder plus item removal cancels the old device without replacing its row");
        const auto current = m.chainRef(20, 201);
        check((replace ? current.gen != 0 && current != target : current.gen == 0) &&
                  m.chainRef(20, 202) == otherItem && m.live(otherItem),
              replace ? "replacement gives the target a new generation while the neighbouring function remains stable"
                      : "removal leaves no handle for the old target and retains the neighbouring native function");
        check(shownIds(m) == std::vector<TrackId>{10, 20} && m.jumpTarget() == trackedRow,
              replace ? "replacement cancellation preserves the cursor's track rather than its former record index"
                      : "removal cancellation preserves the cursor's track rather than its former record index");
        ++f.generation;
        fx = m.setRows(f.input(), ++now);
        check(fx.captureLost == CaptureStatus::Live && !m.capture() && m.jumpTarget() == trackedRow,
              replace ? "replacement cancellation is not repeated on an unchanged follow-up publication"
                      : "item-removal cancellation is not repeated on an unchanged follow-up publication");
        const auto next = m.beginCapture(CaptureKind::Menu, otherItem, ++now);
        m.endCapture(capture.ref, ++now);
        check(next.refusal == CaptureRefusal::None && m.captureStatus(next.ref) == CaptureStatus::Live &&
                  m.jumpTarget() == trackedRow,
              replace ? "the replaced target's late completion cannot end its neighbour's new capture"
                      : "the removed target's late completion cannot end its neighbour's new capture");
        m.endCapture(next.ref, ++now);
    }
}

struct Trace {
    std::vector<TrackId> enlarged;
    std::vector<double> scrolls;
    std::vector<NavGeo> navs;
    std::vector<RowGeo> rows;
    ViewState view;
};

Trace scripted(int every) {
    Fixture f = uniform(40);
    auto m = model(f);
    Trace trace;
    TimeMs t = 0;
    WheelDelta notch;
    notch.notches = 1;
    std::optional<CaptureRef> cap;
    const std::vector<std::pair<TimeMs, std::function<void(TimeMs)>>> script{
        {1000, [&](TimeMs at) { m.pointerMove(600, 560, false, at); }},
        {1300, [&](TimeMs at) { m.pointerMove(600, 200, false, at); }},
        {1600, [&](TimeMs at) { m.wheel(600, 200, notch, Mods{}, at); }},
        {1700, [&](TimeMs at) { m.wheel(600, 200, notch, Mods{}, at); }},
        {2400, [&](TimeMs at) { m.pointerExit(at); }},
        {3000, [&](TimeMs at) { m.pointerMove(600, 300, false, at); }},
        {3400, [&](TimeMs at) {
             const auto r = m.beginCapture(CaptureKind::Drag, m.moduleRef(m.pointerRow().track, kQ), at);
             if (r.refusal == CaptureRefusal::None)
                 cap = r.ref;
         }},
        {3500, [&](TimeMs at) { m.setSearch("track 1", at); }},
        {3900, [&](TimeMs at) {
             if (cap)
                 m.endCapture(*cap, at);
         }},
        {4000, [&](TimeMs at) { m.setSearch("", at); }},
        {4100, [&](TimeMs at) { m.setKeyboardFocus(true, at); }},
        {4100, [&](TimeMs at) { m.key(Key::Down, Mods{}, at); }},
        {4150, [&](TimeMs at) { m.key(Key::Down, Mods{}, at); }},
        {4400, [&](TimeMs at) { m.pointerMove(1200, 200, false, at); }},
        {4410, [&](TimeMs at) {
             m.pointerDown(1200, 200, Button::Left, Mods{}, 1, at);
             m.pointerUp(1200, 200, at);
         }},
        {5000, [&](TimeMs at) { m.pointerMove(600, 400, false, at); }},
        {5300, [&](TimeMs at) { m.pointerExit(at); }},
    };
    for (const auto &[when, action] : script) {
        if (every > 0)
            while (t + every < when)
                m.advance(t += every);
        t = when;
        action(when);
        trace.enlarged.push_back(m.enlarged().track);
        trace.scrolls.push_back(m.scroll());
        trace.navs.push_back(m.navigator());
    }
    if (every > 0)
        while (t + every < 7000)
            m.advance(t += every);
    m.advance(7000);
    trace.enlarged.push_back(m.enlarged().track);
    trace.scrolls.push_back(m.scroll());
    trace.navs.push_back(m.navigator());
    m.forEachVisibleRow([&](const RowGeo &r) { trace.rows.push_back(r); });
    trace.view = m.viewState();
    return trace;
}

void determinism() {
    section("T46 determinism (m5: the navigator too)");
    const Trace a = scripted(1), b = scripted(16), c = scripted(0), d = scripted(16);
    check(a.enlarged == b.enlarged && b.enlarged == c.enlarged, "T46: the same enlarged row at every event");
    check(a.scrolls == b.scrolls && b.scrolls == c.scrolls, "T46: the same scroll at every event");
    check(a.navs == b.navs && b.navs == c.navs, "T46: the same navigator at every event");
    check(a.rows == b.rows && b.rows == c.rows && a.view == b.view && b.view == c.view,
          "T46: identical rows and view state at the end");
    check(b.rows == d.rows && b.enlarged == d.enlarged, "T46: two models fed the same script agree");
}

void baseHeight() {
    section("T47 the base height");
    Fixture f = uniform(40);
    auto m = model(f);
    m.setBaseHeight(98, 10);
    check(m.windowView().baseHeight == 100, "T47: 98 lands on 100");
    m.setBaseHeight(95, 20);
    check(m.windowView().baseHeight == 95, "T47: 95 stays");
    m.stepBaseHeight(1, 30);
    check(m.windowView().baseHeight == 100, "T47: +1 from 95 is 100");
    m.stepBaseHeight(1, 40);
    check(m.windowView().baseHeight == 136, "T47: +1 from 100 is 136");
    m.setBaseHeight(100, 50);
    m.stepBaseHeight(-1, 60);
    check(m.windowView().baseHeight == 68, "T47: -1 from 100 is 68");
    m.setBaseHeight(27, 70);
    check(m.windowView().baseHeight == 28, "T47: 27 to 28");
    m.setBaseHeight(200, 80);
    check(m.windowView().baseHeight == 184, "T47: 200 to 184");
    m.setBaseHeight(100, 90);
    m.scrollTo(1310, 100);
    m.pointerMove(600, 300, false, 110);
    m.advance(1000);
    m.pointerMove(600, 301, false, 1000);
    const std::size_t under = *m.shownIndex(m.pointerRow().track);
    const double top = m.row(under).top;
    Mods alt;
    alt.alt = true;
    WheelDelta up;
    up.notches = -1;
    auto fx = m.wheel(600, 301, up, alt, 1010);
    check(m.windowView().baseHeight == 136 && fx.windowViewChanged && m.row(under).top == top,
          "T47: Alt+wheel up steps to 136, the pointer's row held");
    // the pointer goes on into the next row at once: its dwell would end at 1070, the hold-off holds it to 1130
    const double next = m.row(under + 1).top + 10;
    m.pointerMove(600, next, false, 1010);
    const TrackId candidate = m.pointerRow().track;
    m.advance(1129);
    check(next < 600 && candidate == m.shownTrack(under + 1) && m.enlarged().track != candidate,
          "T47: the hold-off started");
    m.advance(1130);
    check(m.enlarged().track == candidate, "T47: 120 ms after the step");
    const auto cap = m.beginCapture(CaptureKind::Drag, m.moduleRef(candidate, kQ), 1200);
    check(m.setBaseHeight(68, 1210).refused && m.windowView().baseHeight == 136, "T47: refused during a capture");
    m.endCapture(cap.ref, 1220);
}

struct States {
    TrackId enlarged = 0, pointer = 0, maximised = 0, focusRow = 0, selection = 0;
    std::string search;
    std::vector<TrackId> shown, pins;
    ComparePair pair;
    bool captured = false;
    double scroll = 0;
    int base = 0;
};
States statesOf(const StackModel &m) {
    States s;
    s.enlarged = m.enlarged().track;
    s.pointer = m.pointerRow().track;
    s.maximised = m.maximised().track;
    s.focusRow = m.focus().row.track;
    s.selection = m.selection();
    s.search = m.windowView().search;
    s.shown = shownIds(m);
    s.pins = m.pins();
    s.pair = m.comparePair();
    s.captured = m.capture().has_value();
    s.scroll = m.scroll();
    s.base = m.windowView().baseHeight;
    return s;
}
enum Bit : unsigned {
    kEnlarged = 1,
    kPointer = 2,
    kMax = 4,
    kFocus = 8,
    kSelection = 16,
    kSearch = 32,
    kShown = 64,
    kPins = 128,
    kPair = 256,
    kCapture = 512,
    kScroll = 1024,
    kBase = 2048
};
unsigned changed(const States &a, const States &b) {
    unsigned bits = 0;
    bits |= a.enlarged != b.enlarged ? kEnlarged : 0u;
    bits |= a.pointer != b.pointer ? kPointer : 0u;
    bits |= a.maximised != b.maximised ? kMax : 0u;
    bits |= a.focusRow != b.focusRow ? kFocus : 0u;
    bits |= a.selection != b.selection ? kSelection : 0u;
    bits |= a.search != b.search ? kSearch : 0u;
    bits |= a.shown != b.shown ? kShown : 0u;
    bits |= a.pins != b.pins ? kPins : 0u;
    bits |= !(a.pair == b.pair) ? kPair : 0u;
    bits |= a.captured != b.captured ? kCapture : 0u;
    bits |= a.scroll != b.scroll ? kScroll : 0u;
    bits |= a.base != b.base ? kBase : 0u;
    return bits;
}

void separateStates() {
    section("T48 separate states");
    struct Case {
        const char *what;
        std::function<void(StackModel &)> act;
        unsigned allowed;
    };
    const std::vector<Case> cases{
        {"T48: a pointer move changes only the pointer's row", [](StackModel &m) { m.pointerMove(600, 470, false, 2000); },
         kPointer},
        {"T48: Down moves focus and the enlarged row (and reveals)", [](StackModel &m) { m.key(Key::Down, Mods{}, 2000); },
         kFocus | kEnlarged | kScroll},
        {"T48: setSelection changes only the selection", [](StackModel &m) { m.setSelection(7, 2000); }, kSelection},
        {"T48: a search changes the search, the rows and the scroll (and a focus it hid)",
         [](StackModel &m) { m.setSearch("track 1", 2000); }, kSearch | kShown | kScroll | kPointer | kEnlarged | kFocus},
        {"T48: a pin changes only the pins", [](StackModel &m) { m.pin(m.rowRef(9), Mods{}, 2000); }, kPins},
        {"T48: the pair changes only the pair", [](StackModel &m) { m.setComparePair(3, 4, 2000); }, kPair},
        {"T48: a capture changes only the capture",
         [](StackModel &m) { m.beginCapture(CaptureKind::Drag, m.moduleRef(4, kQ), 2000); }, kCapture},
        {"T48: a wheel notch scrolls (and moves the row under the pointer)",
         [](StackModel &m) {
             WheelDelta notch;
             notch.notches = 1;
             m.wheel(600, 300, notch, Mods{}, 2000);
         },
         kScroll | kPointer},
        {"T48: Alt+wheel changes the base (and the scroll it holds)",
         [](StackModel &m) {
             WheelDelta notch;
             notch.notches = -1;
             Mods alt;
             alt.alt = true;
             m.wheel(600, 300, notch, alt, 2000);
         },
         kBase | kScroll | kPointer},
        {"T48: Enter changes Max (and frames it)", [](StackModel &m) { m.key(Key::Enter, Mods{}, 2000); }, kMax | kScroll},
    };
    for (const auto &c : cases) {
        Fixture f = uniform(40);
        auto m = model(f);
        m.setKeyboardFocus(true, 0);
        m.pointerMove(600, 300, false, 10);
        m.advance(500);
        m.focusRow(m.rowRef(3), 600);
        m.advance(1000);
        const States before = statesOf(m);
        c.act(m);
        const unsigned bits = changed(before, statesOf(m));
        check((bits & ~c.allowed) == 0, c.what);
    }
    Fixture f = uniform(40);
    auto m = model(f);
    const auto cap = m.beginCapture(CaptureKind::Drag, m.moduleRef(2, kQ), 10);
    const States before = statesOf(m);
    m.endCapture(cap.ref, 20);
    check((changed(before, statesOf(m)) & ~(kCapture | kScroll | kEnlarged | kPointer)) == 0,
          "T48: an end changes only the capture and what it held");
}

void constants() {
    section("T49 the constants");
    const Constants c;
    check(c.baseMin == 28 && c.baseMax == 184 && c.baseDefault == 100 &&
              c.detents == std::array<int, 6>{28, 44, 68, 100, 136, 184} && c.enlargedPct == 190 && c.near1Pct == 129 &&
              c.near2Pct == 110 && c.gapPct == 31 && c.gapMin == 6 && c.widthExponent == 0.68 &&
              c.widthRefHeight == 100 && c.heightAnimMs == 160 && c.dwellMs == 60 && c.scrollHoldOffMs == 120 &&
              c.leaveMs == 300 && c.chipColumn == 128 && c.pill == 22 && c.itemGap == 10 && c.navigatorWidth == 132 &&
              c.navigatorCollapsed == 28 && c.landingHoldMs == 1500 && c.w100[0] == 400 && c.w100Dormant[0] == 240,
          "T49: the defaults");
    check(c.scaled(1.0) == c, "T49: scaled(1) is the identity");
    const Constants k = c.scaled(1.25);
    check(k.baseDefault == 125 && k.detents == std::array<int, 6>{35, 55, 85, 125, 170, 230} && k.chipColumn == 160 &&
              k.pill == 28 && k.gapMin == 8 && k.navigatorWidth == 165 && k.navigatorCollapsed == 35 &&
              k.tierFloors == std::array<int, 5>{55, 85, 125, 170, 230} && k.widthRefHeight == 125,
          "T49: every absolute size times 1.25, half up");
    check(k.enlargedPct == 190 && k.near1Pct == 129 && k.near2Pct == 110 && k.gapPct == 31 && k.widthExponent == 0.68 &&
              k.heightAnimMs == 160 && k.dwellMs == 60 && k.scrollHoldOffMs == 120 && k.leaveMs == 300,
          "T49: ratios, the exponent and the times kept");
    Fixture f = uniform(20);
    StackModel m(k);
    m.setViewport(1580, 900, 0);
    m.setRows(f.input(), 0);
    m.focusRow(m.rowRef(6), 10);
    m.advance(1000);
    check(m.row(5).height == 238 && m.row(4).height == 161 && m.row(3).height == 138 && m.row(2).height == 125,
          "T49: the scaled model rests at 238 161 138 125");
    check(gapBelow(m, 1) == 39 && gapBelow(m, 4) == 50 && gapBelow(m, 3) == 43 && gapBelow(m, 2) == 39,
          "T49: with gaps 39 50 43 39");
}

// --- the module list comes from the input (B2) ------------------------------------------------------------

void moduleKinds() {
    section("B2 the modules: how many, which and in what order come from the input");
    Fixture none;
    none.kinds.clear();
    none.add(1, "No processor");
    none.add(2, "Another");
    auto z = model(none);
    check(z.moduleKindCount() == 0 && z.plan().count == 0 && z.plan().laneW == 1264 - 128 - 10 - 132,
          "B2: N 0: no cells, the lane takes the room");
    Fixture one;
    one.kinds = {kindOf(kL, "ADI-L", "adi-l", ItemClass::AdiL, false, false)};
    one.add(1, "A");
    one.add(2, "B");
    one.module(2, kL).state = ModuleState::Engaged;
    auto o = model(one);
    check(o.plan().count == 1 && o.plan().place[0] == ModulePlace::Column && o.plan().w[0] == 300 &&
              o.moduleCell(1, 0).content == CellContent::Item && o.moduleCell(0, 0).content == CellContent::DormantPill,
          "B2: N 1");
    Fixture six;
    six.kinds = fourKinds();
    six.kinds.push_back(kindOf(5, "Five", "five", ItemClass::View, false, false));
    six.kinds.push_back(kindOf(6, "Six", "six", ItemClass::Dynamics, false, false));
    six.add(1, "A");
    six.module(1, 6).state = ModuleState::Engaged;
    auto s = model(six, 2560);
    check(s.plan().count == 6 && s.plan().place[5] == ModulePlace::Column && s.plan().w[5] == 300 &&
              s.plan().place[4] == ModulePlace::Tray,
          "B2: N 6, a column for the sixth");
    Fixture reordered;
    reordered.kinds = {fourKinds()[3], fourKinds()[2], fourKinds()[1], fourKinds()[0]};
    reordered.add(1, "A");
    for (auto key : {kQ, kDuck, kClip, kL})
        reordered.module(1, key).state = ModuleState::Engaged;
    auto r = model(reordered, 2560);
    check(r.plan().x[0] < r.plan().x[1] && r.plan().x[1] < r.plan().x[2] && r.plan().x[2] < r.plan().x[3] &&
              r.plan().w[0] == 300 && r.plan().w[3] == 400 && r.moduleKey(0) == kL,
          "B2: a reordered list draws in its order");
    Fixture some = uniform(5);
    for (TrackId id = 1; id <= 5; ++id)
        some.module(id, kDuck) = ModuleInput{};
    some.module(3, kDuck).state = ModuleState::Dormant;
    some.module(3, kDuck).instance = 333;
    auto p = model(some);
    check(p.plan().place[1] == ModulePlace::Tray && p.moduleCell(2, 1).content == CellContent::DormantPill &&
              p.moduleCell(0, 1).content == CellContent::Refused,
          "B2: a kind only some rows can have: a pill there, refused elsewhere");
    Fixture nine;
    for (std::uint32_t k = 1; k <= 9; ++k)
        nine.kinds.push_back(kindOf(k + 10, "K", "k", ItemClass::View, false, false));
    nine.kinds.erase(nine.kinds.begin(), nine.kinds.begin() + 4);
    nine.add(1, "A");
    auto n9 = model(nine);
    check(n9.moduleKindCount() == kMaxModules && n9.stats().ignoredInputs >= 1, "B2: past the capacity, counted");
}

void cardCapture() {
    section("B3 a capture inside the card stays put while another module engages");
    Fixture f = eleven();
    auto m = model(f);
    m.setModuleLayout(ModuleLayout::Card, 0);
    const ItemRef l = m.moduleRef(3, kL);
    const CellGeo before = m.moduleCell(2, 3);
    const auto cap = m.beginCapture(CaptureKind::Drag, l, 10);
    check(cap.refusal == CaptureRefusal::None, "B3: captured ADI-L's cell on track 3");
    f.module(3, kDuck).state = ModuleState::Engaged;
    f.generation = 2;
    m.setRows(f.input(), 20);
    const CellGeo during = m.moduleCell(2, 3);
    check(during.x == before.x && during.width == before.width && m.moduleCell(2, 1).content == CellContent::Item,
          "B3: the ducking engages, ADI-L's cell does not move");
    m.endCapture(cap.ref, 30);
    check(m.plan().place[1] == ModulePlace::Column && m.plan().processorX == 292 &&
              m.moduleCell(2, 1).width == 260 && m.moduleCell(2, 1).x == 538,
          "B3: at the end the card is planned again: the ducking at its engaged width");
}

void refCommands() {
    section("M7 commands by ref for the accessibility layer");
    Fixture f = uniform(40);
    auto m = model(f);
    const RowRef r = m.rowRef(25);
    auto fx = m.focusRow(r, 10);
    check(fx.select == 25 && fx.takeFocus && m.focus().row == r && m.enlarged().track == 25, "M7: focusRow");
    m.advance(500);
    const auto at = *m.shownIndex(25);
    check(m.row(at).top >= 0 && m.row(at).top + m.row(at).height <= 600, "M7: focusRow reveals");
    fx = m.focusItem(m.moduleRef(10, kL), 600);
    check(fx.select == 10 && m.focus().item.module == kL, "M7: focusItem");
    m.advance(1200);
    fx = m.reveal(m.rowRef(39), 1200);
    m.advance(1500);
    const auto last = *m.shownIndex(39);
    check(!fx.refused && m.row(last).top + m.row(last).height <= 600 && m.focus().row.track == 10,
          "M7: reveal scrolls without moving the focus");
    fx = m.scrollTo(262, 1600);
    check(m.scroll() == 262 && !fx.refused, "M7: scrollTo");
    Fixture g = f;
    g.erase(25);
    g.generation = 2;
    m.setRows(g.input(), 1700);
    check(m.focusRow(r, 1800).refused && m.reveal(r, 1810).refused, "M7: a stale row is refused");
    const ItemRef staleItem = ItemRef{r, ItemPlace::Module, kQ, 0, 1};
    check(m.focusItem(staleItem, 1820).refused, "M7: a stale item is refused");
    check(m.moduleName(0) == "ADI-Q" && m.moduleName(1) == "Ducking" && m.moduleName(3) == "ADI-L" &&
              m.moduleName(kMaxModules).empty(),
          "M7: the module kinds' names come from the model, for pills, cells and the accessibility proxies");
}

void clockContract() {
    section("m6 the clock: a small step back is now, a big one is rebased");
    Fixture f = uniform(40);
    auto m = model(f);
    m.pointerMove(600, 300, false, 10'000'000);
    m.advance(10'000'030);
    m.advance(10'000'025); // a step back: treated as now
    check(!m.enlarged().valid(), "m6: a small step back does not run the dwell");
    m.advance(5000); // a new clock: everything pending moves with it
    check(m.stats().clockRebases == 1 && !m.enlarged().valid(), "m6: rebased, the dwell still pending");
    m.advance(5030);
    check(m.enlarged().valid(), "m6: and it completes 60 ms after it began");
    // a timer stamped by an event behind the clock counts from now, not from the stale time
    Fixture g = uniform(40);
    auto s = model(g);
    s.pointerMove(600, 300, false, 1000);
    s.advance(2000);
    const TrackId was = s.enlarged().track;
    s.pointerExit(1500); // 500 ms behind: treated as 2000
    s.advance(2299);
    check(was != 0 && s.enlarged().track == was, "m6: a leave stamped behind the clock still waits 300 ms from now");
    s.advance(2300);
    check(!s.enlarged().valid(), "m6: ... and fires then");
}

void lifetimes() {
    section("m17 focus and Max when their rows go (and m16: a capture freezes the heights, the captured row is not grown)");
    Fixture f = uniform(10);
    auto m = model(f);
    m.focusRow(m.rowRef(5), 10);
    m.maximise(m.rowRef(5), 20);
    Fixture g = f;
    g.erase(5);
    g.generation = 2;
    m.setRows(g.input(), 30);
    check(m.focus().row.track == 6 && !m.maximised().valid(), "m17: deleted: the focus moves to the row now in its place");
    m.setSearch("track 1", 40);
    check(m.focus().row.track == 10 || m.focus().row.track == 1, "m17: filtered out: the focus stays on a shown row");
    Fixture e = eleven();
    auto el = model(e);
    el.focusRow(el.rowRef(10), 10);
    el.toggleFold(el.rowRef(8), 20);
    check(el.focus().row.track == 8, "m17: folded away: the focus goes to the group");
    // m16: a press before the dwell starts a capture; the row is captured, not enlarged
    Fixture u = uniform(20);
    auto c = model(u);
    c.pointerMove(600, 300, false, 100);
    c.advance(1000);
    const TrackId hovered = c.enlarged().track;
    c.pointerMove(600, 50, false, 1000);
    const TrackId pressed = c.pointerRow().track;
    const auto cap = c.beginCapture(CaptureKind::Drag, c.moduleRef(pressed, kQ), 1010);
    c.advance(1500);
    check(c.enlarged().track == hovered && c.row(*c.shownIndex(pressed)).captured &&
              !c.row(*c.shownIndex(pressed)).enlarged,
          "m16: captured before its dwell: the heights freeze as they are, the captured row is not grown (win's placement)");
    c.endCapture(cap.ref, 1500);
    c.advance(1559);
    check(c.enlarged().track == hovered, "m16: at the end the heights wait for a fresh dwell");
    c.advance(1560);
    check(c.enlarged().track == pressed, "m16: then the row under the pointer");
}

void chipTiming() {
    section("m3 a chip click maximises by the base row's tier (win's placement), whatever the dwell");
    Fixture f = uniform(20);
    auto early = model(f);
    early.setBaseHeight(68, 0);
    early.pointerMove(20, 40, false, 100);
    pressChip(early, 0, 110); // before the dwell: the row is 68 (T2)
    check(early.maximised().track == 1, "m3: before the dwell it maximises");
    Fixture g = uniform(20);
    auto late = model(g);
    late.setBaseHeight(68, 0);
    late.pointerMove(20, 40, false, 100);
    late.advance(400); // after it: the row is 129 (T3)
    pressChip(late, 0, 400);
    check(late.maximised().track == 1, "m3: after the dwell too");
    // a double-click: its first click maximised, its second shows the devices and undoes that Max
    late.advance(800);
    const ChipGeo cg = late.chip(0);
    auto fx = late.pointerDown(cg.chip.x + 4, cg.chip.y + 4, Button::Left, Mods{}, 2, 800);
    late.pointerUp(cg.chip.x + 4, cg.chip.y + 4, 800);
    check(fx.showDevices.track == 1 && !late.maximised().valid(),
          "m3: the double-click shows the devices and restores the Max its first click began");
    Fixture h = uniform(20);
    auto tall = model(h);
    pressChip(tall, 0, 100); // base 100 is T3: no Max from a chip click
    check(!tall.maximised().valid(), "m3: at base 100 a chip click does not maximise");
}

// --- a seeded stream of every kind of event, checking what must always hold -------------------------

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    double real(double lo, double hi) {
        return lo + (hi - lo) * static_cast<double>(next() >> 11) / 9007199254740992.0;
    }
};

Fixture stressFixture() {
    Fixture f;
    for (TrackId id = 1; id <= 60; ++id) {
        const bool group = id % 10 == 1;
        f.add(id, "Track " + std::to_string(id), group ? 0 : ((id - 1) / 10) * 10 + 1, group ? 0u : 1u,
              group ? kGroupKind : kAudio);
        f.track(id).group = group;
        f.track(id).colour = static_cast<std::int32_t>(id % 7) - 1;
        f.track(id).roles = id == 2 ? role::Kick : id == 3 ? role::Bass : std::uint8_t{0};
        if (id % 3 == 0) {
            f.item(id, id * 10 + 1, static_cast<ItemClass>(id % 11), "Device " + std::to_string(id));
            f.item(id, id * 10 + 2, ItemClass::Plugin, "Plug-in " + std::to_string(id));
        }
        for (std::uint32_t key : {kQ, kDuck, kClip, kL}) {
            const auto state = static_cast<int>((id + key) % 5);
            f.module(id, key).state = state == 0 ? ModuleState::Absent : state == 1 ? ModuleState::Engaged : ModuleState::Dormant;
            if (state == 0)
                f.module(id, key).instance = 0;
        }
    }
    f.add(61, "Main", 0, 0, kMainKind);
    f.track(61).main = true;
    return f;
}

const char *broken(const StackModel &m, Rng &rng, double width, double height) {
    if (!std::isfinite(m.scroll()) || !std::isfinite(m.contentHeight()))
        return "a scroll or content height that is not finite";
    const Span v = m.visible(), r = m.realised();
    if (!v.empty && (v.first > v.last || v.last >= m.shownCount()))
        return "the visible span outside the rows";
    if (!v.empty && (r.empty || r.first > v.first || r.last < v.last))
        return "the realised span not covering the visible one";
    const double base = m.windowView().baseHeight;
    bool rows = true, items = true;
    m.forEachVisibleRow([&](const RowGeo &g) {
        rows = rows && std::isfinite(g.top) && std::isfinite(g.height) && g.height >= base - 1e-6 && m.live(g.ref);
        m.forEachChainItem(g.shown, [&](const ItemGeo &i) {
            items = items && std::isfinite(i.x) && i.width > 0 && (m.live(i.ref) || g.captured);
        });
        m.forEachModuleCell(g.shown, [&](const CellGeo &c) { items = items && std::isfinite(c.x) && c.width >= 0; });
    });
    if (!rows)
        return "a visible row not finite, below base or dead";
    if (!items)
        return "an item or cell not finite or dead";
    if (const auto c = m.capture(); c && !m.shownIndex(c->target.row.track))
        return "a captured row not shown";
    if (m.enlarged().valid() && !m.shownIndex(m.enlarged().track))
        return "an enlarged row not shown";
    if (m.maximised().valid() && !m.shownIndex(m.maximised().track))
        return "a maximised row not shown";
    if (m.focus().row.valid() && !m.shownIndex(m.focus().row.track))
        return "a focused row not shown";
    for (int k = 0; k < 4; ++k) {
        const Hit h = m.hitTest(rng.real(0, width), rng.real(0, height));
        if (h.row.valid() && (!m.live(h.row) || h.shown >= m.shownCount()))
            return "a hit on a dead or missing row";
    }
    const NavGeo n = m.navigator();
    if (n.shown && !n.lineSpan.empty && n.lineSpan.last >= m.shownCount())
        return "navigator lines past the rows";
    return nullptr;
}

// What the state is after an event, as one number: compared between runs.
std::uint64_t signature(const StackModel &m) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    const auto bits = [](double d) {
        std::uint64_t u = 0;
        std::memcpy(&u, &d, sizeof u);
        return u;
    };
    mix(static_cast<std::uint64_t>(m.enlarged().track));
    mix(static_cast<std::uint64_t>(m.maximised().track));
    mix(static_cast<std::uint64_t>(m.focus().row.track));
    mix(m.focus().item.module);
    mix(static_cast<std::uint64_t>(m.focus().item.device));
    mix(bits(m.scroll()));
    mix(m.shownCount());
    const Span v = m.visible();
    mix(v.first);
    mix(v.last);
    mix(v.empty ? 1u : 0u);
    m.forEachVisibleRow([&](const RowGeo &r) {
        mix(bits(r.top));
        mix(bits(r.height));
    });
    mix(bits(m.navigator().scroll));
    mix(m.capture() ? m.capture()->token : 0u);
    mix(m.pins().size());
    mix(static_cast<std::uint64_t>(m.windowView().baseHeight));
    mix(bits(m.plan().laneW));
    return h;
}

// One seeded stream of 4000 events. With frameStep > 0 the owner also calls advance every
// frameStep ms between events, which must change nothing an event sees.
const char *stream(std::uint64_t seed, int frameStep, std::vector<std::uint64_t> &trace) {
    const char *searches[6] = {"", "track 1", "a", "x", "renamed", "adi-l"};
    const char *firstBroken = nullptr;
    Rng rng{seed * 0x9E3779B97F4A7C15ull};
    Fixture f = stressFixture();
    const Constants c = seed % 3 == 0 ? Constants{}.scaled(1.25) : Constants{};
    double width = 1264, height = 600;
    StackModel m(c);
    m.setViewport(width, height, 0);
    m.setRows(f.input(), 0);
    if (seed % 2 == 0)
        m.setModuleLayout(ModuleLayout::Card, 0);
    TimeMs t = 0;
    const auto frames = [&](TimeMs from, TimeMs to) {
        if (frameStep > 0)
            for (TimeMs at = from + frameStep; at < to; at += frameStep)
                m.advance(at);
    };
    TrackId nextId = 100;
    DeviceId nextDevice = 5000;
    std::uint64_t nextInstance = 90000;
    std::uint32_t captured = 0; // the live capture's token, with its row's place when it began
    double capturedTop = 0, capturedHeight = 0;
    {
        for (int step = 0; step < 4000 && firstBroken == nullptr; ++step) {
            const TimeMs before = t;
            t += rng.below(40);
            frames(before, t);
            Mods mods;
            mods.shift = rng.below(4) == 0;
            mods.alt = rng.below(6) == 0;
            mods.ctrl = rng.below(8) == 0;
            const auto randomRow = [&]() -> TrackId {
                return m.shownCount() == 0 ? 0 : m.shownTrack(static_cast<std::size_t>(rng.below(static_cast<int>(m.shownCount()))));
            };
            switch (rng.below(18)) {
            case 0:
            case 1:
            case 2:
                m.pointerMove(rng.real(-40, width + 40), rng.real(-40, height + 40), rng.below(5) == 0, t);
                break;
            case 3:
                m.pointerDown(rng.real(0, width), rng.real(0, height), static_cast<Button>(rng.below(3)), mods,
                              1 + rng.below(2), t);
                break;
            case 4:
                m.pointerUp(rng.real(0, width), rng.real(0, height), t);
                break;
            case 5:
                m.pointerExit(t);
                break;
            case 6: {
                WheelDelta d;
                if (rng.below(2) == 0)
                    d.notches = static_cast<double>(rng.below(5) - 2);
                else {
                    d.pixelsY = rng.real(-200, 200);
                    d.pixelsX = rng.real(-100, 100);
                }
                m.wheel(rng.real(0, width), rng.real(0, height), d, mods, t);
                break;
            }
            case 7:
                m.key(static_cast<Key>(rng.below(12)), mods, t);
                break;
            case 8: {
                const TimeMs to = t + rng.below(400);
                frames(t, to);
                m.advance(t = to);
                break;
            }
            case 9: {
                const std::size_t i = static_cast<std::size_t>(rng.below(static_cast<int>(f.tracks.size())));
                const TrackId id = f.tracks[i].id;
                switch (rng.below(7)) {
                case 0:
                    f.tracks[i].name = f.keep("Renamed " + std::to_string(rng.below(1000)));
                    break;
                case 1: {
                    ModuleInput &mod = f.modules[i * f.kinds.size() + static_cast<std::size_t>(rng.below(4))];
                    mod.state = static_cast<ModuleState>(rng.below(3));
                    mod.instance = mod.state == ModuleState::Absent ? 0 : (rng.below(3) == 0 ? ++nextInstance : mod.instance + 1);
                    break;
                }
                case 2:
                    if (rng.below(2) == 0 || f.chains[i].empty())
                        f.item(id, ++nextDevice, static_cast<ItemClass>(rng.below(11)), "New device");
                    else
                        f.chains[i].erase(f.chains[i].begin());
                    break;
                case 3: {
                    const TrackId inserted = ++nextId;
                    f.insert(i, inserted, "Inserted " + std::to_string(inserted));
                    break;
                }
                case 4:
                    if (f.tracks.size() > 8 && !f.tracks[i].group && !f.tracks[i].main)
                        f.erase(id);
                    break;
                case 5:
                    f.tracks[i].inView = !f.tracks[i].inView;
                    break;
                default:
                    f.tracks[i].identity = f.tracks[i].identity == kAudio ? 0x31D1 : kAudio;
                    break;
                }
                ++f.generation;
                m.setRows(f.input(), t);
                break;
            }
            case 10:
                if (const auto cap = m.capture())
                    m.endCapture(*cap, t);
                else if (const TrackId id = randomRow(); id != 0) {
                    ItemRef target{m.rowRef(id), ItemPlace::None, 0, 0, m.rowRef(id).gen};
                    if (rng.below(2) == 0)
                        target = m.moduleRef(id, static_cast<std::uint32_t>(1 + rng.below(4)));
                    else if (const auto &chain = f.chains[f.at(id)]; !chain.empty() && rng.below(2) == 0)
                        target = m.chainRef(id, chain.front().device);
                    const auto began = m.beginCapture(static_cast<CaptureKind>(rng.below(3)), target, t);
                    if (began.refusal == CaptureRefusal::None) {
                        const RowGeo held = m.row(*m.shownIndex(id));
                        captured = began.ref.token;
                        capturedTop = held.top;
                        capturedHeight = held.height;
                    }
                }
                break;
            case 11:
                if (rng.below(4) == 0)
                    m.setSearchMode(static_cast<SearchMode>(rng.below(2)), t);
                else
                    m.setSearch(searches[rng.below(6)], t);
                break;
            case 12:
                if (rng.below(3) == 0)
                    m.setFilterPinned(rng.below(2) == 0, t);
                else if (const TrackId id = randomRow(); id != 0)
                    m.pin(m.rowRef(id), mods, t);
                break;
            case 13:
                m.toggleFold(m.rowRef(static_cast<TrackId>(1 + 10 * rng.below(6))), t);
                break;
            case 14:
                if (rng.below(2) == 0)
                    m.setBaseHeight(20 + rng.below(190), t);
                else
                    m.stepBaseHeight(rng.below(3) - 1, t);
                break;
            case 15:
                width = rng.real(300, 2600);
                height = rng.real(100, 1300);
                m.setViewport(width, height, t);
                break;
            case 16:
                switch (rng.below(6)) {
                case 0:
                    m.setModuleLayout(static_cast<ModuleLayout>(rng.below(2)), t);
                    break;
                case 1:
                    m.setRowLayout(static_cast<RowLayout>(rng.below(2)), t);
                    break;
                case 2:
                    m.setShowChain(rng.below(2) == 0, t);
                    break;
                case 3:
                    m.setShowDormant(rng.below(2) == 0, t);
                    break;
                case 4:
                    m.setNavigator(rng.below(3) != 0, rng.below(2) == 0, t);
                    break;
                default:
                    m.setAutoZoom(rng.below(3) != 0, t);
                    break;
                }
                break;
            default:
                if (const TrackId id = randomRow(); id != 0)
                    switch (rng.below(7)) {
                    case 0:
                        m.maximise(m.rowRef(id), t);
                        break;
                    case 1:
                        m.restore(t);
                        break;
                    case 2:
                        m.focusRow(m.rowRef(id), t);
                        break;
                    case 3:
                        m.reveal(m.rowRef(id), t);
                        break;
                    case 4:
                        m.scrollTo(rng.real(-100, 20000), t);
                        break;
                    case 5:
                        m.openFromChip(id, ItemKey{static_cast<std::uint32_t>(rng.below(5)), 0}, t);
                        break;
                    default:
                        m.advance(t); // an owner reads the export at the time it means
                        m.applyViewState(m.viewState(), t);
                        break;
                    }
                break;
            }
            firstBroken = broken(m, rng, width, height);
            // whatever happened, a live capture's row has not moved
            if (const auto cap = m.capture(); cap && cap->token == captured) {
                const RowGeo r = m.row(*m.shownIndex(cap->target.row.track));
                if (!near(r.top, capturedTop, 1e-6) || r.height != capturedHeight)
                    firstBroken = "a captured row moved";
            } else
                captured = 0;
            trace.push_back(signature(m));
        }
    }
    return firstBroken;
}

void stress() {
    section("seeded streams of every event: the invariants hold, and no frame rate is in them");
    std::size_t events = 0;
    const char *firstBroken = nullptr;
    std::vector<std::uint64_t> plain, framed;
    bool sameAcrossFrames = true;
    for (std::uint64_t seed = 1; seed <= 3 && firstBroken == nullptr; ++seed) {
        plain.clear();
        framed.clear();
        firstBroken = stream(seed, 0, plain);
        events += plain.size();
        if (firstBroken == nullptr && seed <= 2) {
            firstBroken = stream(seed, 7, framed);
            sameAcrossFrames = sameAcrossFrames && framed == plain;
            if (framed != plain)
                for (std::size_t i = 0; i < std::min(framed.size(), plain.size()); ++i)
                    if (framed[i] != plain[i]) {
                        std::printf("  seed %llu: the runs part after event %zu\n", static_cast<unsigned long long>(seed), i);
                        break;
                    }
        }
    }
    if (firstBroken != nullptr)
        std::printf("  broken after %zu events: %s\n", events, firstBroken);
    check(firstBroken == nullptr && events == 12000u, "12000 seeded events: every invariant holds, a capture never moves");
    check(sameAcrossFrames, "the same streams with a frame every 7 ms: the same state after every event");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a section name reaches the log before an abort would lose it
#if defined(_MSC_VER) && defined(_DEBUG)
    // The debug CRT's assertions and aborts go to stderr, never to a modal dialog: these tests
    // run unattended on the director's desktop, and a dialog there blocks the run and him.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    fisheyeAtBase100();
    fisheyeAtEveryDetent();
    capsAndFloor();
    maxRow();
    tiers();
    widths();
    widthBudget();
    positions();
    cells();
    itemFocusGeometry();
    laneAndChevrons();
    rowsAndScope();
    dwell();
    keyboardAtOnce();
    anchor();
    topSlack();
    noScrollWithButton();
    stillPointerEverywhere();
    holdOff();
    leaving();
    autoZoomOff();
    animation();
    scrolling();
    workCounter();
    incrementalRows();
    noAllocation();
    generations();
    capturedTargetStaysPut();
    refusals();
    rename();
    menuStaysPut();
    keys();
    escape();
    selectionIsRead();
    landing();
    navigatorGeometry();
    navigatorClick();
    filter();
    quickJump();
    folding();
    pins();
    reference();
    viewStateRoundTrip();
    adapter();
    actualChainAdapter();
    channelMembership();
    channelShortcutScope();
    channelScopeJumpPins();
    jumpCapturePublicationChanges();
    determinism();
    baseHeight();
    separateStates();
    constants();
    moduleKinds();
    cardCapture();
    refCommands();
    clockContract();
    lifetimes();
    chipTiming();
    stress();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
