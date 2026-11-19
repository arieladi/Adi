// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
namespace {
using Core = adi::dsp::AdiShaper;
using Bank = adi::dsp::AdiShaperWaveBank;
void require(bool result, const char* label) { if (!result) throw std::runtime_error(label); }
void node(Core& c, int slot, int key, Bank::Point p) {
    const int id = Bank::firstId - 1 + 2 * slot;
    c.set(static_cast<Core::Param>(id), static_cast<float>(Bank::address(key, p)));
    c.set(static_cast<Core::Param>(id + 1), static_cast<float>(Bank::value(p)));
}
void ringAndCurve() {
    Core core;
    core.prepare(48000);
    constexpr int midVolume = Bank::key(0, 1, 0);
    node(core, 0, midVolume, {0, 0, 1, Bank::Shape::Bend});
    node(core, 1, midVolume, {.5f, 1, 0, Bank::Shape::Hold});
    node(core, 2, midVolume, {.5f, .25f, 0, Bank::Shape::Line});
    node(core, 3, midVolume, {1, .25f, 0, Bank::Shape::Line});
    std::array<float, 64> left, right;
    left.fill(.5f); right.fill(-.25f);
    core.sync(1, false); // quarter of a four-quarter cycle, midway through the rising bend
    auto* scope = core.shaperScope();
    scope->select(0, 1);
    core.process(left.data(), right.data(), left.data(), right.data(), left.size());
    require(std::abs(left[0] - .125f) < .0001f && std::abs(right[63] + .0625f) < .0001f,
            "variable-time bent segment is heard on both channels");
    adi::dsp::AdiShaperScope::Frame f;
    require(scope->read(f) && std::abs(f.inputMax[128] - .125f) < .00001f &&
            std::abs(f.outputMax[128] - .03125f) < .0001f && std::abs(f.peakRight - .0625f) < .0001f,
            "capture separates pre- and post-gain signals under in-place processing");
    scope->suspend();
    left.fill(1); right.fill(1); core.sync(3, false);
    core.process(left.data(), right.data(), left.data(), right.data(), 64);
    require(std::abs(left[0] - .25f) < .00002f && !scope->read(f), "duplicate-X right limit and closed scope");
    for (int i = 0; i < 4; ++i) core.set(static_cast<Core::Param>(Bank::firstId - 1 + 2 * i), 0);
    left.fill(1); right.fill(1);
    core.process(left.data(), right.data(), left.data(), right.data(), 64);
    require(left[0] == 1 && right.back() == 1, "deleting the free curve restores legacy defaults");
}
void sourceCorners() {
    struct Point { int x; double y; int copies; };
    constexpr std::array frames{0, 1500, 3000, 6000, 9000, 12000, 15000, 18000, 21000, 23999};
    // At a 24,000-frame cycle, a constant input yields these measured gains.
    auto run = [&](std::initializer_list<Point> points, std::array<double,10> expected) {
        Bank bank;
        int slot = 0;
        for (auto p : points) {
            const Bank::Point point{p.x / 8192.f, static_cast<float>(p.y),
                                    static_cast<float>(2-p.copies), Bank::Shape::SourceCorner};
            const int a = Bank::address(0,point), b = Bank::value(point);
            const auto restored = Bank::unpack(a,b);
            require(restored.x == point.x && restored.shape == point.shape && restored.bend == point.bend,
                    "source corner X, multiplicity and mode survive two float words");
            bank.set(Bank::firstId+slot*2, static_cast<float>(a));
            bank.set(Bank::firstId+slot*2+1, static_cast<float>(b));
            ++slot;
        }
        bank.prepareBlock();
        for (std::size_t i=0; i<frames.size(); ++i) {
            const auto phase = static_cast<std::uint64_t>((frames[i]/24000.)*0x1p64);
            require(std::abs(Bank::at(bank.view(0),phase)-expected[i]) < (frames[i] == 23999 ? 7.e-5 : 2.e-5),
                    "periodic soft, medium and hard corners match the measured gain");
        }
    };
    run({{0, 0.22, 1}, {2048, 0.8, 1}, {4096, 0.05, 1}, {6144, 0.65, 1}}, {0.38833332, 0.4325521, 0.50333333, 0.57833332, 0.42541668, 0.27500001, 0.35666668, 0.47833335, 0.43458334, 0.38832086});
    run({{0, 0.22, 3}, {1531, 0.8, 1}, {5099, 0.05, 2}, {6878, 0.65, 1}}, {0.22, 0.40752965, 0.52888072, 0.56960535, 0.44779655, 0.25972122, 0.07922816, 0.37344444, 0.47141224, 0.22005942});
    run({{733, 0.22, 2}, {1531, 0.8, 3}, {5099, 0.05, 1}, {6878, 0.65, 3}}, {0.37397656, 0.26672608, 0.43150377, 0.69132566, 0.48659244, 0.32982442, 0.27550364, 0.41779411, 0.58908159, 0.37404826});
    run({{0, 0.22, 3}, {3250, 0.8, 1}, {3327, 0.05, 2}, {7291, 0.65, 1}}, {0.22, 0.31137231, 0.40084779, 0.54646719, 0.51079702, 0.16638428, 0.31377101, 0.433873, 0.47847888, 0.22011435});
    run({{0, 0.1, 1}, {1811, 0.7, 3}, {1811, 0.25, 2}, {4869, 0.51, 1}, {6911, 0.98, 2}}, {0.35713798, 0.35537657, 0.45339593, 0.27100772, 0.35980791, 0.46687624, 0.6099174, 0.80590034, 0.80353057, 0.35718793});
    Bank edge;
    constexpr std::array xValues{0,1811,1811,4869,6911}, copies{1,3,2,1,2};
    constexpr std::array levels{.1f,.7f,.25f,.51f,.98f};
    for (int i=0; i<5; ++i) {
        const Bank::Point pt{xValues[i]/8192.f,levels[i],static_cast<float>(2-copies[i]),Bank::Shape::SourceCorner};
        edge.set(Bank::firstId+2*i, static_cast<float>(Bank::address(0,pt)));
        edge.set(Bank::firstId+2*i+1, static_cast<float>(Bank::value(pt)));
    }
    edge.prepareBlock();
    constexpr std::array transit{.700137f,.717950f,.725976f,.678448f,.545396f,.393655f,.306327f,.289025f,.298323f};
    const auto trace = edge.view(0);
    for (std::size_t i=0; i<transit.size(); ++i) {
        const auto phase = static_cast<std::uint64_t>(((5300.+static_cast<double>(i))/24000.)*0x1p64);
        require(std::abs(Bank::at(trace,phase)-transit[i]) < 2.e-5f,
                "grid interpolation preserves the measured overshoot at a coincident hard and medium point");
    }
    for (int i=0; i<5; ++i) {
        const Bank::Point pt{xValues[i]/8192.f,levels[i],-1.f,Bank::Shape::SourceCorner};
        edge.set(Bank::firstId+2*i+1, static_cast<float>(Bank::value(pt)));
    }
    edge.prepareBlock();
    constexpr std::array hardCut{.699360f,.699489f,.699609f,.699681f,.699692f,.699675f,.250006f,.250033f,.250066f};
    const auto bothHard = edge.view(0);
    for (std::size_t i=0; i<hardCut.size(); ++i) {
        const auto phase = static_cast<std::uint64_t>(((5300.+static_cast<double>(i))/24000.)*0x1p64);
        require(std::abs(Bank::at(bothHard,phase)-hardCut[i]) < 1.e-5f,
                "hard duplicate clamps lookup taps to the last or first grid address on that side");
    }
    Bank flat;
    Bank::Point point{0.f,.6f,1.f,Bank::Shape::SourceCorner};
    for (int i = 0; i < Bank::slots; ++i) {
        point.x = static_cast<float>(i) / Bank::slots;
        flat.set(Bank::firstId+2*i, static_cast<float>(Bank::address(0,point)));
        flat.set(Bank::firstId+2*i+1, static_cast<float>(Bank::value(point)));
    }
    flat.prepareBlock();
    bool moving = true;
    require(flat.view(0).constant && flat.view(0).spans.empty() &&
            std::abs(Bank::at(flat.view(0),42,&moving)-.6f) < 1.e-5f && !moving,
            "a saturated pool of equal heights takes the stationary path");
    for (int i = 0; i < Bank::slots; ++i) {
        point.y = i % 2 == 0 ? 0.f : 1.f;
        point.bend = -1.f;
        point.x = static_cast<float>(i) / Bank::slots;
        flat.set(Bank::firstId+2*i, static_cast<float>(Bank::address(0,point)));
        flat.set(Bank::firstId+2*i+1, static_cast<float>(Bank::value(point)));
    }
    flat.prepareBlock();
    const auto filled = flat.view(0);
    for (int i = 0; i < Bank::slots; ++i) {
        const auto quarter = (static_cast<std::uint64_t>(i) << 55) + (std::uint64_t{1} << 53);
        require(std::abs(Bank::at(filled,quarter)-(i % 2 == 0 ? .25f : .75f)) < 1.e-6f,
                "all 1,536 repeated controls fit and return a linear hard edge");
    }
    Bank seam;
    auto add = [&](int slot, Bank::Point p) {
        seam.set(Bank::firstId+2*slot, static_cast<float>(Bank::address(0,p)));
        seam.set(Bank::firstId+2*slot+1, static_cast<float>(Bank::value(p)));
    };
    add(0, {0.f,1.f,-1.f,Bank::Shape::SourceCorner}); // previous cycle's endpoint
    add(1, {0.f,0.f,-1.f,Bank::Shape::SourceCorner});
    add(2, {.5f,.25f,-1.f,Bank::Shape::SourceCorner});
    seam.prepareBlock();
    const auto wrapped = seam.view(0);
    require(Bank::at(wrapped,0) == 0 && std::abs(Bank::at(wrapped,~std::uint64_t{0}) - (1.f-.75f/4096.f)) < 1.e-6f,
            "coincident hard corners at the wrap preserve right and left limits");
    add(1, {0.f,0.f,0.f,Bank::Shape::Hold});
    add(2, {.5f,1.f,1.f,Bank::Shape::SourceCorner});
    seam.prepareBlock();
    const auto mixed = seam.view(0);
    require(Bank::at(mixed,std::uint64_t{1} << 62,&moving) == 0 && !moving,
            "a legacy hold keeps its outgoing segment next to a source corner");
}
void timeUsesFreeContour() {
    Core core;
    core.prepare(48000);
    core.set(Core::TimeOn, 1);
    core.set(Core::timeParam(Core::Band::Mid, Core::TimeRange), 5);
    constexpr int key = Bank::key(static_cast<int>(Core::Stage::Time), 1, 0);
    node(core, 0, key, {0.f, 0.f, 0.f, Bank::Shape::Line});
    node(core, 1, key, {1.f, 1.f, 0.f, Bank::Shape::Hold});
    core.sync(4.0 * .319, false);
    std::array<float, 512> l{}, r{};
    for (int i = 0; i < 512; ++i) {
        l[i] = static_cast<float>(i) / 1024.f;
        r[i] = -.25f * l[i];
    }
    core.process(l.data(), r.data(), l.data(), r.data(), l.size());
    for (int i = 240; i < 512; ++i) {
        const float expected = static_cast<float>((i - ((1.0 - .319) * 240.0 - 1.0)) / 1024.0);
        require(std::abs(l[i] - expected) < 2e-6f && std::abs(r[i] + .25f * expected) < 2e-6f,
                "Time's free line overrides constant legacy heights and interpolates the history read");
    }
}

}
int main() try { ringAndCurve(); sourceCorners(); timeUsesFreeContour(); return 0; }
catch (const std::exception& e) { std::fprintf(stderr,"%s\n", e.what()); return 1; }
