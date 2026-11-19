// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper.hpp"
#include <array>
#include <cmath>
#include <cstdio>
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
}
int main() try { ringAndCurve(); return 0; }
catch (const std::exception& e) { std::fprintf(stderr,"%s\n", e.what()); return 1; }
