// SPDX-License-Identifier: GPL-3.0-or-later
//
// The curve formulas of SPEC §6.3.2 (ADR-0159): the properties the SPEC
// requires, over a grid of x and tension, and the golden table it prints.
#include "adi/engine/curves.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace adi::engine;

int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", name.c_str()); }
}

constexpr std::uint8_t kHold = 0, kLinear = 1, kExp = 2, kLog = 3, kS = 4, kBezier = 5;
constexpr std::array<std::uint8_t, 5> kShaped{kLinear, kExp, kLog, kS, kBezier};
const char* name(std::uint8_t c) {
    static const char* names[] = {"hold", "linear", "exp", "log", "s-curve", "bezier"};
    return c <= 5 ? names[c] : "?";
}

// x on a dyadic grid, where 1 - x and 2x are exact, so the reflections can be
// checked bit for bit; tension on a grid that includes both ends and 0.
std::vector<double> xs() {
    std::vector<double> v;
    for (int i = 0; i <= 1024; ++i) v.push_back(i / 1024.0);
    return v;
}
std::vector<double> tensions() {
    std::vector<double> v;
    for (int i = -64; i <= 64; ++i) v.push_back(i / 64.0);
    v.push_back(0.3); v.push_back(-0.7); v.push_back(0.999999); v.push_back(-1e-9);
    return v;
}

void endpoints() {
    bool shape = true, value = true;
    const std::array<std::array<double, 2>, 6> pairs{{{0.1, 0.7}, {0.7, 0.1}, {-3.3, 1e-9}, {0.2, 0.30000000000000004},
                                                      {1e10, -7.77}, {1.0 / 3.0, 2.0 / 3.0}}};
    for (std::uint8_t c = 0; c <= 5; ++c)
        for (double t : tensions()) {
            shape = shape && curveShape(c, 0.0, t) == 0.0 && curveShape(c, 1.0, t) == 1.0;
            for (const auto& p : pairs)
                value = value && curveValue(p[0], p[1], 0.0, c, t) == p[0] && curveValue(p[0], p[1], 1.0, c, t) == p[1];
        }
    check(shape, "a: u(0) = 0 and u(1) = 1 exactly, every shape and tension");
    check(value, "a: a segment starts at v0 and ends at v1 bit for bit");
    bool beyond = true;
    for (std::uint8_t c = 0; c <= 5; ++c)
        beyond = beyond && curveValue(2.0, 5.0, -0.5, c, 0.4) == 2.0 && curveValue(2.0, 5.0, 1.5, c, 0.4) == 5.0;
    check(beyond, "x outside [0, 1] gives the nearer endpoint");
}

void monotonic() {
    for (std::uint8_t c = 0; c <= 5; ++c) {
        bool up = true, down = true, inRange = true;
        for (double t : tensions()) {
            double prev = -1.0, prevDown = std::numeric_limits<double>::infinity();
            for (int i = 0; i <= 4096; ++i) {
                const double x = i / 4096.0;
                const double u = curveShape(c, x, t);
                up = up && u >= prev;
                inRange = inRange && u >= 0.0 && u <= 1.0;
                prev = u;
                const double v = curveValue(3.0, -2.0, x, c, t);   // a falling segment
                down = down && v <= prevDown;
                prevDown = v;
            }
        }
        check(up && inRange, std::string("b: ") + name(c) + " is monotonic in [0, 1] for every tension in [-1, 1]");
        check(down, std::string("b: ") + name(c) + " falls monotonically from v0 > v1");
    }
}

void tensionZero() {
    for (std::uint8_t c : kShaped) {
        bool exact = true;
        for (double x : xs()) exact = exact && curveShape(c, x, 0.0) == x;
        for (int i = 0; i < 1000; ++i) {
            const double x = (i + 0.37) / 1000.0;   // not dyadic
            exact = exact && curveShape(c, x, 0.0) == x;
        }
        check(exact, std::string("c: tension 0 is exactly linear for ") + name(c));
    }
}

void reflections() {
    bool logIsExp = true, sSym = true;
    for (double t : tensions())
        for (double x : xs()) {
            logIsExp = logIsExp && curveShape(kLog, x, t) == 1.0 - curveShape(kExp, 1.0 - x, t);
            // Each pair {x, 1 - x} is exact from its lower half; from the upper
            // half 1 - (1 - a) may differ from a by an ulp, which is float, not shape.
            sSym = sSym && (x <= 0.5 ? curveShape(kS, 1.0 - x, t) == 1.0 - curveShape(kS, x, t)
                                     : std::fabs(curveShape(kS, 1.0 - x, t) - (1.0 - curveShape(kS, x, t))) <= 0x1p-52);
        }
    check(logIsExp, "d: log(x, t) = 1 - exp(1 - x, t), bit for bit on the dyadic grid");
    check(sSym, "e: s-curve is symmetric, s(1 - x) = 1 - s(x): exact for x <= 1/2");
    bool bend = curveShape(kExp, 0.5, 1.0) < 0.5 && curveShape(kExp, 0.5, -1.0) > 0.5 &&
                curveShape(kLog, 0.5, 1.0) > 0.5 && curveShape(kS, 0.25, 1.0) < 0.25 &&
                curveShape(kS, 0.25, -1.0) > 0.25 && curveShape(kBezier, 0.5, 1.0) < 0.5;
    check(bend, "positive tension bends the way the name says: exp slow-start, log fast-start, s steep middle, bezier convex");
}

void bezierControlPoint() {
    // At t = +1 the control point is the corner (1, 0): y = (1 - sqrt(1 - x))^2.
    // At t = -1 it is (0, 1): y = 1 - (1 - sqrt(x))^2... reflected: 1 - (1 - sqrt(1 - (1 - x)))^2.
    bool closed = true;
    for (int i = 1; i < 64; ++i) {
        const double x = i / 64.0;
        const double plus = (1.0 - std::sqrt(1.0 - x)) * (1.0 - std::sqrt(1.0 - x));
        const double minus = 1.0 - (1.0 - std::sqrt(x)) * (1.0 - std::sqrt(x));
        closed = closed && std::fabs(curveShape(kBezier, x, 1.0) - plus) < 1e-14 &&
                 std::fabs(curveShape(kBezier, x, -1.0) - minus) < 1e-14;
    }
    check(closed, "f: bezier's control point is (0.5 + t/2, 0.5 - t/2): the corner (1, 0) at t = +1, (0, 1) at t = -1");
    bool mirrored = true;
    for (double t : tensions())
        for (int i = 1; i < 64; ++i) {
            const double x = i / 64.0;
            mirrored = mirrored && std::fabs(curveShape(kBezier, x, -t) - (1.0 - curveShape(kBezier, 1.0 - x, t))) < 1e-12;
        }
    check(mirrored, "f: bezier at -t is bezier at t reflected through the centre");
}

void hold() {
    bool ok = true;
    for (double t : tensions())
        for (double x : xs()) {
            const double v = curveValue(-4.0, 9.0, x, kHold, t);
            ok = ok && v == (x < 1.0 ? -4.0 : 9.0);
        }
    check(ok, "g: hold is v0 on [x0, x1) and v1 at x1");
    check(curveValue(-4.0, 9.0, std::nextafter(1.0, 0.0), kHold, 0.0) == -4.0, "g: hold is still v0 one ulp before x1");
}

void finite() {
    const double big = std::numeric_limits<double>::max();
    const double inf = std::numeric_limits<double>::infinity();
    bool ok = true;
    const std::array<double, 7> tens{-1e300, -1.0, -0.2, 0.0, 0.7, 1.0, 1e300};
    const std::array<std::array<double, 2>, 5> ends{{{-big, big}, {big, -big}, {0.0, 1e-300}, {-1e308, 1e308}, {5.0, 5.0}}};
    for (std::uint8_t c = 0; c <= 7; ++c)   // 6 and 7: curves a reader refuses; the evaluator stays total
        for (double t : tens)
            for (const auto& e : ends)
                for (int i = 0; i <= 256; ++i) {
                    const double x = i / 256.0;
                    ok = ok && std::isfinite(curveValue(e[0], e[1], x, c, t)) && std::isfinite(curveShape(c, x, t));
                }
    check(ok, "h: finite for every finite input, the double range's ends included");
    bool clamped = true;
    for (std::uint8_t c : kShaped)
        for (int i = 1; i < 16; ++i) {
            const double x = i / 16.0;
            clamped = clamped && curveShape(c, x, 7.0) == curveShape(c, x, 1.0) && curveShape(c, x, -1e300) == curveShape(c, x, -1.0);
        }
    check(clamped, "h: tension is clamped to [-1, 1]");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check(curveShape(kExp, 0.25, nan) == 0.25 && curveValue(1.0, 2.0, nan, kExp, 0.5) == 1.0 &&
              std::isfinite(curveValue(1.0, 2.0, 0.5, kS, inf)),
          "a NaN tension reads as 0, a NaN x as the start, an infinite tension as +1");
    check(curveShape(9, 0.3, 0.8) == 0.3, "a curve above 5 draws as linear (a reader refuses it first)");
}

// SPEC §6.3.2's golden table. A formula change fails here, not in a listening
// session. 1e-12 allows each platform's libm its last bits in expm1 and sqrt.
void golden() {
    struct Row { std::uint8_t c; double t; double u[3]; };
    const std::array<Row, 16> rows{{
        {kExp, 1.0, {0.004628041293, 0.030653430032, 0.177004945950}},
        {kExp, 0.5, {0.044782800838, 0.150979557211, 0.402811752901}},
        {kExp, -0.5, {0.597188247099, 0.849020442789, 0.955217199162}},
        {kExp, -1.0, {0.822995054050, 0.969346569968, 0.995371958707}},
        {kLog, 1.0, {0.822995054050, 0.969346569968, 0.995371958707}},
        {kLog, 0.5, {0.597188247099, 0.849020442789, 0.955217199162}},
        {kLog, -0.5, {0.044782800838, 0.150979557211, 0.402811752901}},
        {kLog, -1.0, {0.004628041293, 0.030653430032, 0.177004945950}},
        {kS, 1.0, {0.015326715016, 0.5, 0.984673284984}},
        {kS, 0.5, {0.075489778606, 0.5, 0.924510221394}},
        {kS, -0.5, {0.424510221394, 0.5, 0.575489778606}},
        {kS, -1.0, {0.484673284984, 0.5, 0.515326715016}},
        {kBezier, 1.0, {0.017949192431, 0.085786437627, 0.25}},
        {kBezier, 0.5, {0.104248688935, 0.263932022500, 0.517949192431}},
        {kBezier, -0.5, {0.482050807569, 0.736067977500, 0.895751311065}},
        {kBezier, -1.0, {0.75, 0.914213562373, 0.982050807569}},
    }};
    for (const auto& r : rows) {
        bool ok = true;
        const double x[3] = {0.25, 0.5, 0.75};
        for (int i = 0; i < 3; ++i) ok = ok && std::fabs(curveShape(r.c, x[i], r.t) - r.u[i]) < 1e-11;
        char label[96];
        std::snprintf(label, sizeof label, "golden: %s at tension %+.1f matches SPEC 6.3.2's table", name(r.c), r.t);
        check(ok, label);
    }
    check(std::fabs(curveShape(kExp, 0.5, 1.0) - 1.0 / (1.0 + std::sqrt(1000.0))) < 1e-15,
          "golden: exp at +1 and x = 1/2 is 1 / (1 + sqrt 1000): the 1000:1 range");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    endpoints();
    monotonic();
    tensionZero();
    reflections();
    bezierControlPoint();
    hold();
    finite();
    golden();
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
