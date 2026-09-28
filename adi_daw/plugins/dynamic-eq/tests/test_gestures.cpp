// SPDX-License-Identifier: AGPL-3.0-only
#include "Gestures.hpp"
#include <cstdio>
#include <limits>
using namespace adi::eq;
int checks = 0, failures = 0;
void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
int main() {
    Band b;
    auto run = [&](Kind k, Modifiers m = {}, double x = 1, double y = 1, double w = 1, Axis a = Axis::None) {
        return gesture(b, {k, x, y, w, m, a});
    };
    auto r = run(Kind::Drag);
    check(near(r.band.frequency, 2000) && near(r.band.gain, 1) && near(r.band.q, b.q),
          "row 1 bell frequency/gain");
    for (auto s : {Shape::LowShelf, Shape::HighShelf}) {
        b.shape = s;
        check(near(run(Kind::Drag).band.gain, 1), "row 1 shelves gain");
    }
    for (auto s : {Shape::LowPass, Shape::HighPass, Shape::Notch, Shape::BandPass}) {
        b.shape = s;
        r = run(Kind::Drag);
        check(near(r.band.q, b.q * 2) && near(r.band.gain, 0), "row 1 no-gain shapes Q");
    }
    b = Band{};
    check(near(run(Kind::Wheel).band.q, b.q * 2), "row 2 wheel Q");
    check(near(run(Kind::Drag, {true}).band.q, b.q * 2), "row 3 Ctrl/Cmd vertical Q");
    r = run(Kind::Drag, {false, true}, 2, 1);
    check(r.axis == Axis::Frequency && near(r.band.gain, 0) && near(r.band.frequency, 4000),
          "row 4 Alt frequency lock");
    r = run(Kind::Drag, {false, true}, 1, 2);
    check(r.axis == Axis::Vertical && near(r.band.frequency, 1000) && near(r.band.gain, 2),
          "row 4 Alt gain lock");
    r = run(Kind::Drag, {true, true}, 1, 2);
    check(near(r.band.q, b.q * 4) && near(r.band.frequency, 1000), "row 4 Alt Ctrl Q lock");
    check(near(run(Kind::Drag, {false, true}, 1, 20, 0, Axis::Frequency).band.gain, 0),
          "lock persists on direction reversal");
    check(near(run(Kind::Drag, {false, false, true}).band.gain, .1), "row 5 Shift drag fine");
    check(near(run(Kind::Wheel, {false, false, true}).band.q, b.q * std::exp2(.1)), "row 5 Shift wheel fine");
    r = run(Kind::Wheel, {false, true});
    check(near(r.band.range, 1) && r.band.dynamic && near(r.band.gain, 0), "row 6 Alt wheel range");
    check(near(run(Kind::Wheel, {true}).band.gain, 1), "row 7 Ctrl wheel gain");
    b.range = -6;
    r = run(Kind::Wheel, {true, true});
    check(near(r.band.gain, 1) && near(r.band.range, -7) && near(r.band.gain + r.band.range, -6),
          "row 8 linked target invariant");
    b.gain = 30;
    b.range = -6;
    r = run(Kind::Wheel, {true, true});
    check(near(r.band.gain, 30) && near(r.band.range, -6), "linked gain boundary");
    b = Band{};
    check(run(Kind::Click, {false, true}).band.bypass, "row 9 Alt click bypass");
    r = run(Kind::Click, {true, true});
    check(r.band.shape == Shape::LowShelf && !r.band.bypass, "row 10 Ctrl Alt click shape precedence");
    r = run(Kind::Click, {false, true, true});
    check(r.band.slope == 2 && !r.band.bypass, "row 11 Alt Shift click slope precedence");
    check(run(Kind::DoubleClick).action == Action::EditValues, "row 12 exact values");
    check(run(Kind::RightClick).action == Action::Menu, "row 13 band menu");
    r = run(Kind::Create);
    check(r.action == Action::CreateBand && !r.band.dynamic, "row 14 drag curve create");
    r = run(Kind::Create, {false, true});
    check(r.action == Action::CreateBand && r.band.dynamic && near(r.band.gain, 1) && near(r.band.range, -1),
          "row 14 Alt create dynamic without axis lock");
    b.slope = 6;
    b.shape = Shape::FlatGain;
    check(run(Kind::Click, {true, true}).band.shape == Shape::Bell, "shape cycle wraps");
    check(run(Kind::Click, {false, true, true}).band.slope == 0, "slope cycle wraps");
    b = Band{};
    r = run(Kind::Drag, {}, 1e6, 1e6);
    check(r.band.frequency == 160000 && r.band.gain == 30, "finite output after extreme drag");
    r = run(Kind::Drag, {}, std::numeric_limits<double>::quiet_NaN());
    check(r.band.frequency == b.frequency, "reject malformed delta");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
