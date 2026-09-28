// SPDX-License-Identifier: GPL-3.0-or-later
//
// dsp::Spectrum -- ADR-0183 d7, ADR-0195 d5.
//
// THE POINT OF THESE TESTS IS THE WRAPPER, not PFFFT. PFFFT is a twenty-year
// lineage of FFTPACK and does not need us to check that it transforms; what
// needs checking is the three lines that turn its output into magnitudes,
// because its real packing is unusual and getting it wrong produces a
// plausible picture rather than an obvious failure:
//
//     out[0] is DC and out[1] is NYQUIST -- not a complex pair.
//
// So the reference here is a DIRECT DFT at small N, written from the
// definition in four lines with no packing at all. If the wrapper and the
// definition disagree, the wrapper is wrong.

#include "adi/dsp/spectrum.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace adi::dsp;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL  %s\n", what.c_str()); }
}
void near(double got, double want, double tol, const std::string& what) {
    ++g_checks;
    if (!(std::fabs(got - want) <= tol)) {
        ++g_failures;
        std::printf("  FAIL  %s\n          got %.6f, want %.6f +- %g\n",
                    what.c_str(), got, want, tol);
    }
}
void section(const char* s) { std::printf("[%s]\n", s); }

constexpr double kPi = 3.14159265358979323846;

/// The definition, with no packing and no library: X[k] = sum x[n] e^-i2pikn/N,
/// then the same window and the same calibration the wrapper uses.
std::vector<double> referenceDft(const std::vector<float>& frame, Window w) {
    const auto n = static_cast<std::int32_t>(frame.size());
    std::vector<float> win(static_cast<std::size_t>(n));
    fillWindow(w, win);
    double sumw = 0.0;
    for (float v : win) sumw += v;
    const double cal = sumw > 0.0 ? 2.0 / sumw : 1.0;

    std::vector<double> mag(static_cast<std::size_t>(n / 2 + 1), 0.0);
    for (std::int32_t k = 0; k <= n / 2; ++k) {
        double re = 0.0, im = 0.0;
        for (std::int32_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(frame[static_cast<std::size_t>(i)]) *
                             static_cast<double>(win[static_cast<std::size_t>(i)]);
            const double a = 2.0 * kPi * k * i / n;
            re += x * std::cos(a);
            im -= x * std::sin(a);
        }
        // DC and Nyquist have no negative-frequency partner to be summed with,
        // so they take half the calibration -- the same rule the wrapper
        // applies, stated here independently rather than copied from it.
        const double half = (k == 0 || k == n / 2) ? 0.5 : 1.0;
        mag[static_cast<std::size_t>(k)] = std::sqrt(re * re + im * im) * cal * half;
    }
    return mag;
}

std::vector<float> sine(std::int32_t n, double bin, double amp) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (std::int32_t i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * bin * i / n));
    return v;
}

// ---------------------------------------------------------------------------

void testAgainstTheDefinition() {
    section("ADR-0195 d5 -- the wrapper against a direct DFT: scaling and bin order");

    for (std::int32_t n : {32, 64, 96}) {
        Spectrum s(n);
        check(s.valid(), "a spectrum of " + std::to_string(n) + " points is built");
        if (!s.valid()) continue;
        check(s.bins() == n / 2 + 1, "with N/2 + 1 bins");

        // A signal with energy EVERYWHERE, so every bin is compared and not
        // just the one a tone happens to land in -- including DC and Nyquist,
        // which is where the packing bug would hide.
        std::vector<float> frame(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i);
            frame[static_cast<std::size_t>(i)] = static_cast<float>(
                0.3 * std::sin(2.0 * kPi * 3.0 * t / n) +
                0.2 * std::cos(2.0 * kPi * 7.5 * t / n) +
                0.1 * ((i % 2) ? -1.0 : 1.0) +      // Nyquist
                0.05);                               // DC
        }

        std::vector<float> got(static_cast<std::size_t>(s.bins()), -1.f);
        s.analyse(frame, got);
        const auto want = referenceDft(frame, s.window());

        double worst = 0.0;
        std::int32_t worstBin = -1;
        for (std::int32_t k = 0; k < s.bins(); ++k) {
            const double d = std::fabs(static_cast<double>(got[static_cast<std::size_t>(k)]) -
                                       want[static_cast<std::size_t>(k)]);
            if (d > worst) { worst = d; worstBin = k; }
        }
        check(worst < 1e-5,
              "every bin matches the definition at N=" + std::to_string(n) +
              "\n          worst bin " + std::to_string(worstBin) +
              ", off by " + std::to_string(worst));
    }
}

void testFullScaleSineIsZeroDbfs() {
    section("ADR-0183 d7 -- a full-scale sine reads 0.00 dBFS, at every size");

    // THE NUMBER EVERYTHING ELSE IS READ AGAINST. It took weeks to get right in
    // the Max for Live build, and it is one line here only because that work
    // decided what the line should be.
    for (std::int32_t n : {256, 1024, 2048, 4096}) {
        Spectrum s(n);
        check(s.valid(), "N=" + std::to_string(n) + " is a size PFFFT takes");
        if (!s.valid()) continue;

        // ON a bin centre: off-centre is scalloping loss, which is a property
        // of any windowed FFT and not of this calibration.
        const std::int32_t bin = n / 8;
        const auto frame = sine(n, bin, 1.0);
        std::vector<float> mag(static_cast<std::size_t>(s.bins()), 0.f);
        s.analyse(frame, mag);

        const double db = toDbfs(mag[static_cast<std::size_t>(bin)]);
        near(db, 0.0, 0.01, "a full-scale sine at N=" + std::to_string(n));
    }

    // Half scale is -6.02 dB, and that is the check that the calibration is a
    // SCALE and not a constant someone tuned until one case read zero.
    Spectrum s(1024);
    const auto half = sine(1024, 128, 0.5);
    std::vector<float> mag(static_cast<std::size_t>(s.bins()), 0.f);
    s.analyse(half, mag);
    near(toDbfs(mag[128]), -6.0206, 0.01, "half scale is -6.02 dBFS");

    const auto tenth = sine(1024, 128, 0.1);
    s.analyse(tenth, mag);
    near(toDbfs(mag[128]), -20.0, 0.01, "a tenth is -20 dBFS");
}

void testDcAndNyquist() {
    section("ADR-0195 d5 -- DC and Nyquist, where the packing bug would hide");

    Spectrum s(1024);
    std::vector<float> mag(static_cast<std::size_t>(s.bins()), 0.f);

    // A FULL-SCALE DC OFFSET READS 0 dBFS, not +6. DC has no
    // negative-frequency partner, so without the half it would read twice as
    // loud as the sine it is being compared with.
    std::vector<float> dc(1024, 1.f);
    s.analyse(dc, mag);
    near(toDbfs(mag[0]), 0.0, 0.01, "a full-scale DC offset");
    // AND BIN 1 READS 0 dBFS TOO, which surprised me and is correct. A Hann
    // window is 0.5 - 0.5cos, so its own transform is 0.5N at bin 0 with
    // 0.25N at bins +-1: the skirt IS 6 dB down in raw terms. But bin 0 is
    // calibrated as DC -- halved, because DC has no negative-frequency partner
    // -- while bin 1 is calibrated as a sinusoid, which is not. The two
    // factors cancel exactly.
    //
    // It is a consequence of using two calibrations for two kinds of bin, it
    // only appears for pure DC, which is not a musical signal, and it is
    // asserted here so that nobody later "fixes" it into a wrong number.
    near(toDbfs(mag[1]), 0.0, 0.05,
         "and bin 1 reads the same, because the window's -6 dB skirt and DC's "
         "half-calibration cancel");
    check(toDbfs(mag[2]) < -100.f, "while bin 2, which the window does not reach, is silent");

    // Alternating +-1 IS Nyquist, and it must land in the LAST bin rather than
    // in bin 1 -- which is where PFFFT actually stores it.
    std::vector<float> nyq(1024);
    for (std::size_t i = 0; i < nyq.size(); ++i) nyq[i] = (i % 2) ? -1.f : 1.f;
    s.analyse(nyq, mag);
    near(toDbfs(mag[static_cast<std::size_t>(s.bins() - 1)]), 0.0, 0.01,
         "full-scale Nyquist, in the LAST bin");
    check(toDbfs(mag[0]) < -100.f, "and not in DC, which is where a naive unpack puts it");
    near(toDbfs(mag[static_cast<std::size_t>(s.bins() - 2)]), 0.0, 0.05,
         "with the same cancellation on the bin below it, Nyquist being "
         "unsplit for the same reason DC is");
}

void testTheWindow() {
    section("ADR-0183 d7 -- the window is periodic, and its coherent gain is summed");

    std::vector<float> w(8);
    fillWindow(Window::Hann, w);
    near(w[0], 0.0, 1e-6, "a Hann window starts at zero");
    near(w[4], 1.0, 1e-6, "and peaks at one in the middle");
    // PERIODIC, not symmetric: w[n-1] != w[0]. A symmetric window would repeat
    // its endpoint and an overlapping analysis would count that sample twice.
    check(w[7] > 0.1f, "and does NOT return to zero at the end -- it is periodic");

    near(coherentGain(Window::Hann, 1024), 0.5, 1e-6, "Hann's coherent gain is 0.5");
    near(coherentGain(Window::Rectangular, 1024), 1.0, 1e-9, "a rectangle's is 1.0");

    std::vector<float> r(16);
    fillWindow(Window::Rectangular, r);
    bool allOnes = true;
    for (float v : r) if (std::fabs(v - 1.f) > 1e-9f) allOnes = false;
    check(allOnes, "and a rectangular window is all ones");
}

void testRefusals() {
    section("ADR-0195 d5 -- sizes and spans it refuses rather than mangles");

    // TWO KINDS OF BAD SIZE, and only one of them is PFFFT's to refuse.
    //
    // 1000 is not a multiple of 32, and `pffft_new_setup` ASSERTS on that
    // rather than returning NULL -- it would abort the process here, and do
    // something undefined in a build with NDEBUG. The wrapper checks the
    // multiple before PFFFT sees it, which is most of why it exists.
    Spectrum notAMultiple(1000);
    check(!notAMultiple.valid(),
          "a size that is not a multiple of 32 is refused BY US -- PFFFT would "
          "have aborted the process");
    std::vector<float> junk(1000, 1.f), out(501, -1.f);
    notAMultiple.analyse(junk, out);
    check(out[0] < -0.5f, "and analysing through it writes nothing at all");

    // 224 IS a multiple of 32, so it reaches PFFFT -- and 224 = 2^5 * 7, which
    // it cannot factor, so it returns NULL as its header promises.
    Spectrum badFactors(224);
    check(!badFactors.valid(), "a size PFFFT cannot factor comes back NULL, and is refused too");

    check(Spectrum(1024).valid(), "and a good size is accepted");

    Spectrum s(64);
    std::vector<float> shortFrame(32, 1.f);
    std::vector<float> mag(static_cast<std::size_t>(s.bins()), -1.f);
    s.analyse(shortFrame, mag);
    check(mag[0] < -0.5f, "a frame shorter than the FFT is refused, not read past");

    std::vector<float> frame(64, 1.f), shortOut(4, -1.f);
    s.analyse(frame, shortOut);
    check(shortOut[0] < -0.5f, "and so is an output span too small for the bins");

    near(toDbfs(0.f), kDbFloor, 1e-6, "a silent bin is floored, not -inf");
    near(s.binHz(48000.0), 750.0, 1e-9, "64 bins of 48 kHz are 750 Hz apart");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_spectrum_tests -- the one definition of a spectrum\n\n");
    testAgainstTheDefinition();
    testFullScaleSineIsZeroDbfs();
    testDcAndNyquist();
    testTheWindow();
    testRefusals();
    std::printf("\n%s -- %d checks, %d failure(s)\n",
                g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
