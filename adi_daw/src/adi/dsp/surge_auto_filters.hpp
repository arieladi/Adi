/*
 * sst-filters - A header-only collection of SIMD filter
 * implementations by the Surge Synth Team
 *
 * Copyright 2019-2025, various authors, as described in the GitHub
 * transaction log.
 *
 * sst-filters is released under the Gnu General Public Licens
 * version 3 or later. Some of the filters in this package
 * originated in the version of Surge open sourced in 2018.
 *
 * All source in sst-filters available at
 * https://github.com/surge-synthesizer/sst-filters
 */

/*
 * An important note on licensing. sst-filters is, by and large,
 * GPL3 code, but Andy kindly made his entire work on this filter
 * available to everyone, so this header file is available
 * for you to copy in an MIT licensed context. You will have to
 * implement your own FastTan SSE or use tanh below, but basically
 * go ahead and use it however you want, in the same spirit as Andy's
 * sharing the work.
 */

// The MIT concession above applies to CytomicSVF only. This combined adaptation
// includes GPL-3.0-or-later K35 and Huov code and is distributed under GPL-3.0-or-later.
// Scalar adaptations of CytomicSVF, K35Filter and VintageLadders::Huov,
// sst-filters e92d93a92beabde03fa4ab767b285fa21c6608d6.
// Standard tan/tanh/exp replace SIMD fast approximations. ADI selects output taps.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
namespace adi::dsp::surge_auto {
struct SVF {
    double i1 = 0, i2 = 0;
    struct Out {
        double low, band, high, notch;
    };
    Out process(double x, double hz, double resonance, double fs) noexcept {
        const double g = std::tan(std::numbers::pi * std::clamp(hz / fs, 0., .475));
        const double k = 2 - 2 * std::clamp(resonance, 0., .98), a1 = 1 / (1 + g * (g + k)),
                     a2 = g * a1, a3 = g * a2;
        const double v3 = x - i2, v1 = a1 * i1 + a2 * v3, v2 = i2 + a2 * i1 + a3 * v3;
        i1 = 2 * v1 - i1;
        i2 = 2 * v2 - i2;
        return {v2, v1, x - k * v1 - v2, x - k * v1};
    }
};
// K35Filter.h's Odin-derived Sallen-Key topology and coefficient equations.
/**
 * This namespace contains an adaptation of the filter from
 * https://github.com/TheWaveWarden/odin2/blob/master/Source/audio/Filters/Korg35Filter.cpp
 */
struct K35 {
    double lz = 0, hz = 0, z2 = 0;
    static double low(double g, double x, double &z) noexcept {
        const double v = (x - z) * g, result = v + z;
        z = v + result;
        return result;
    }
    static double high(double g, double x, double &z) noexcept { return x - low(g, x, z); }
    double process(double x, double freq, double res, double drive, double fs, bool hp) noexcept {
        const double g = std::tan(std::numbers::pi * std::clamp(freq / fs, 0., .3)), gp1 = 1 + g,
                     G = g / gp1;
        const double k = std::clamp(res * 1.96, .01, 1.96), lb = hp ? 1 / gp1 : (k - k * G) / gp1,
                     hb = hp ? -G / gp1 : -1 / gp1, alpha = 1 / (1 - k * G + k * G * G);
        const double blend = std::min(drive, 1.);
        if (!hp) {
            const double y1 = low(G, x, lz), s35 = lb * z2 + hb * hz, uClean = alpha * (y1 + s35);
            const double u = uClean * (1 - blend) + std::tanh(uClean * drive) * blend;
            const double y = k * low(G, u, z2);
            high(G, y, hz);
            return y / k;
        }
        const double y1 = high(G, x, hz), s35 = hb * z2 + lb * lz, u = alpha * (y1 + s35),
                     clean = k * u;
        const double y = clean * (1 - blend) + std::tanh(clean * drive) * blend;
        low(G, high(G, y, z2), lz);
        return y / k;
    }
};
/*
** Huovilainen developed an improved and physically correct model of the Moog
** Ladder filter that builds upon the work done by Smith and Stilson. This model
** inserts nonlinearities inside each of the 4 one-pole sections on account of the
** smoothly saturating function of analog transistors. The base-emitter voltages of
** the transistors are considered with an experimental value of 1.22070313 which
** maintains the characteristic sound of the analog Moog. This model also permits
** self-oscillation for resonances greater than 1. The model depends on five
** hyperbolic tangent functions (tanh) for each sample, and an oversampling factor
** of two (preferably higher, if possible). Although a more faithful
** representation of the Moog ladder, these dependencies increase the processing
** time of the filter significantly. Lastly, a half-sample delay is introduced for
** phase compensation at the final stage of the filter.
**
** References: Huovilainen (2004), Huovilainen (2010), DAFX - Zolzer (ed) (2nd ed)
** Original implementation: Victor Lazzarini for CSound5
**
** Considerations for oversampling:
** http://music.columbia.edu/pipermail/music-dsp/2005-February/062778.html
** http://www.synthmaker.co.uk/dokuwiki/doku.php?id=tutorials:oversampling
*/
struct Ladder {
    std::array<double, 6> delay{};
    std::array<double, 4> stage{}, stageTanh{};
    double process(double x, double hz, double resonance, double fs, int poles,
                   bool highpass) noexcept {
        const double fc = std::clamp(hz / fs, 0., .3), fr = fc * .5, fc2 = fc * fc, fc3 = fc * fc2;
        const double fcr = 1.8730 * fc3 + .4995 * fc2 - .6490 * fc + .9988,
                     acr = -3.9364 * fc2 + 1.8409 * fc + .9968;
        const double tune = (1 - std::exp(-2 * std::numbers::pi * fr * fcr)) * 70,
                     resquad = 4 * resonance * acr;
        for (int sub = 0; sub < 2; ++sub) {
            double input = x - resquad * delay[5];
            stage[0] = delay[0] + tune * (std::tanh(input / 70) - stageTanh[0]);
            delay[0] = stage[0];
            for (std::size_t k = 1; k < 4; ++k) {
                input = stage[k - 1];
                stageTanh[k - 1] = std::tanh(input / 70);
                stage[k] = delay[k] + tune * (stageTanh[k - 1] -
                                              (k != 3 ? stageTanh[k] : std::tanh(delay[k] / 70)));
                delay[k] = stage[k];
            }
            delay[5] = (stage[3] + delay[4]) * .5;
            delay[4] = stage[3];
        }
        if (highpass)
            return poles == 2 ? x - 2 * stage[0] + stage[1]
                              : x - 4 * stage[0] + 6 * stage[1] - 4 * stage[2] + stage[3];
        return poles == 2 ? stage[1] : delay[5];
    }
};
} // namespace adi::dsp::surge_auto
