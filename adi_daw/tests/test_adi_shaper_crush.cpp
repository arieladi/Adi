// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_crush.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using Crush = adi::dsp::AdiShaperCrush;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void near(double actual, double expected, double threshold, const char* message) {
    if (!(std::abs(actual - expected) <= threshold)) {
        std::fprintf(stderr, "%s: %.9g versus %.9g, tolerance %.9g\n", message, actual, expected, threshold);
        throw std::runtime_error(message);
    }
}
float mono(Crush& band, float source, float bits, float rate) {
    float l = source, r = source;
    band.processStereo(l, r, bits, rate);
    check(l == r && std::isfinite(l), "matched stereo");
    return l;
}
struct UndoHighPass {
    explicit UndoHighPass(double rate) : pole(std::exp(-2 * std::numbers::pi * 10 / rate)) {}
    double decode(double sample) {
        const double value = (sample + integral) * (2.0 / (1.0 + pole));
        integral += (1.0 - pole) * sample;
        return value;
    }
    double pole, integral = 0;
};
void quantizer(Crush& band) {
    // Input steps, transition value and settled level measured independently.
    constexpr std::array<std::array<float, 3>, 9> cases{{
        {0, 0, 0}, {.05f, 0, 0}, {.65f, .1875f, .75f}, {.45f, .5625f, 0},
        {.60f, .5f, .75f}, {-.30f, .0833333f, 0}, {.65f, .118421f, .75f},
        {-.65f, 0, -.75f}, {1.30f, .25f, .75f}}};
    band.reset();
    UndoHighPass inverse(48000);
    for (const auto& row : cases) {
        for (int i = 0; i < 24; ++i)
            near(inverse.decode(mono(band, row[0], 0, 1)), row[i ? 2 : 1], .00002, "quantized transition");
    }
    struct Threshold { float height, input, firstLevel; };
    constexpr std::array<Threshold, 5> steps{{
        {.125f, .18182373f, .3027403f}, {.25f, .11114502f, .19878945f},
        {.5f, .06256104f, .12109686f}, {.75f, .03381348f, .06735006f},
        {.99f, .00002106f, .00004209f}}};
    for (const auto& s : steps) {
        band.reset();
        UndoHighPass inv(48000);
        for(int i=0;i<5;++i) static_cast<void>(inv.decode(mono(band, s.input, s.height, 1)));
        near(inv.decode(mono(band, s.input, s.height, 1)), s.firstLevel, .000015, "settled level height");
    }
    // Positive low-level steps between the nominal and measured first thresholds.
    for (auto [height, input] : std::array<std::array<float, 2>, 2>{{
             {.125f, .1818200f}, {.25f, .1111125f}}}) {
        band.reset();
        for (int i=0; i<16; ++i)
            near(mono(band, input, height, 1), 0, 1e-8, "measured first threshold");
    }
}
void clock(Crush& band) {
    for (auto [index, first, settled] : std::array<std::array<float, 3>, 2>{{
             {73, .126574f, .251569f}, {74, .124995f, .248431f}}}) {
        band.reset();
        UndoHighPass inv(48000);
        for (int i=0; i<78; ++i) {
            const auto value = inv.decode(mono(band, i==static_cast<int>(index) ? .5f : 0.f, 1, .25f));
            if (i == 74) near(value, first, .0004, "fractional capture boundary");
            if (i == 75) near(value, settled, .0004, "interpolated source");
        }
    }
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        check(band.prepare(rate), "prepare");
        UndoHighPass inv(rate);
        const int period = static_cast<int>(std::floor(rate / 8.82));
        for(int i=0;i<=period+1;++i) {
            const double value=inv.decode(mono(band,.5f,1,0));
            if(i<period-1) near(value,0,1e-7,"silence before first hold event");
            if(i==period+1) near(value,.5,.00005,"lowest hold clock");
        }
        band.reset();
        for (int i=0;i<3;++i) {
            const auto actual=mono(band, i==0 ? 1.f : 0.f,1,1);
            const double pole=std::exp(-2*std::numbers::pi*10/rate);
            near(actual, i==0 ? (1+pole)/2 : (1+pole)/2*(pole-1)*std::pow(pole,i-1), 2e-7,
                 "full resolution DC response");
        }
    }
}
void longClock(Crush& band) {
    // A marked sample on a live DC input exposes the fractional hold time.
    // Independent captures contain 0.5, 2 and 6 seconds of pre-roll.
    struct Marker { float height; int frame; double first, held, tolerance; };
    constexpr Marker markers[]{
        {.25f, 24062, .12490189, .24299639, .007},
        {.375f, 96026, .12491496, .24347907, .004},
        {.5f, 96015, .06728267, .41987836, .00001},
        {.5f, 288015, .12481332, .25966132, .00001},
        {.75f, 24003, .00201411, .49797773, .004}
    };
    check(band.prepare(48000), "prepare long clock test");
    constexpr float background = .0625f;
    for (const auto& mark : markers) {
        band.reset();
        UndoHighPass inverse(48000);
        for (int i=0; i<=mark.frame+1; ++i) {
            const float input = i == mark.frame ? background + .5f : background;
            const double value = inverse.decode(mono(band, input, 1, mark.height)) - background;
            if (i == mark.frame) near(value, mark.first, mark.tolerance, "long fractional wrap");
            if (i == mark.frame+1) near(value, mark.held, mark.tolerance, "long fractional capture");
        }
    }
}
void combined(Crush& band) {
    // Shared input and observed left samples from a 48 kHz held-out render.
    constexpr std::array<float, 32> input{.33664951f,-.35934982f,.40007362f,.01105044f,.69700485f,
        .10975637f,.90608668f,.53747797f,.69246209f,.54697335f,-.09147993f,.02921500f,-.40685076f,
        -.12430046f,-.06589244f,.59903276f,-.18613397f,-.83111858f,.18032891f,-.34436998f,
        -.31528822f,.02110452f,.16092943f,-.33413336f,-.91907758f,.65704197f,.89739114f,-.73366845f,
        .37742418f,.55638677f,-.19605464f,.41704029f};
    band.prepare(48000);
    for(int i=0;i<300;++i) mono(band,0,.39f,.61f);
    auto copy=band;
    for(std::size_t i=0;i<input.size();++i) {
        float l=input[i],r=0;
        band.processStereo(l,r,.39f,.61f);
        check(r==0,"right channel remains silent");
        near(l,mono(copy,input[i],.39f,.61f),0,"value-copy preserves clock and step history");
        if(i==5) near(l,.27304488,.001,"quantized signal feeds hold");
        if(i==15) near(l,-.19938470,.001,"second combined reading");
        if(i==31) near(l,.24919967,.001,"combined fractional transition");
    }
}
void dither(Crush& band) {
    band.setDither(true);
    for(auto [level, expected] : std::array<std::array<float,2>,4>{{
            {0,0},{.1f,.02688f},{.25f,.13546f},{.5f,.29945f}}}) {
        band.reset();
        double e=0;
        for(int i=0;i<32768;++i) {
            const auto y=mono(band,level,0,1);
            if(i>=2048)e+=static_cast<double>(y)*y;
        }
        near(std::sqrt(e/(32768-2048)),expected, level==0 ? 0 : .011,"dither RMS");
    }
    band.reset();const auto x=mono(band,.49f,0,1);
    band.reset();near(x,mono(band,.49f,0,1),0,"repeatable dither reset");
}

void quantizerBias() {
    Crush band;
    band.prepare(48000);
    band.setBias(.68f);
    UndoHighPass leading(48000), trailing(48000);
    for (int i = 0; i < 128; ++i) {
        float l = 0.f, r = 0.f;
        band.processStereo(l, r, .19f, 1.f);
        leading.decode(l);
        trailing.decode(r);
    }
    // Signed stereo plateaus and the transition from each previous sample.
    // Each four-frame pair was measured independently at fixed resolution.
    struct Step { float l, r, firstL, firstR, settledL, settledR; };
    constexpr Step steps[]{
        {.19f,-.37f,.1797036f,-.0888537f,.2337050f,-.2337050f},
        {-.51f,.16f,-.0474146f,-.0108365f,-.4674099f,.2337050f},
        {.48f,.29f,.0733809f,.2337050f,.4674099f,.2337050f},
        {-.32f,-.63f,.1484880f,-.0716401f,-.2337050f,-.4674099f},
        {.71f,.51f,.2468490f,.0268247f,.7011149f,.4674099f},
        {-.68f,-.19f,.0925551f,.2200047f,-.4674099f,0.f}};
    for (const auto& s : steps) {
        for (int i = 0; i < 4; ++i) {
            float l = s.l, r = s.r;
            band.processStereo(l, r, .19f, 1.f);
            near(leading.decode(l), i == 0 ? s.firstL : s.settledL, 2e-6, "signed left bias");
            near(trailing.decode(r), i == 0 ? s.firstR : s.settledR, 2e-6, "signed right bias");
        }
    }
    band.prepare(48000);
    band.setBias(.8f);
    UndoHighPass changing(48000);
    for (int i = 0; i < 6; ++i) changing.decode(mono(band, .21f, 0.f, 1.f));
    band.setBias(0.f);
    // The preceding input occupied code coordinate .6092. The descending
    // interval to .35 spends .1092/.2592 above the positive threshold.
    near(changing.decode(mono(band, .35f, 0.f, 1.f)), .75 * .1092 / .2592, 2e-6,
         "bias automation retains the preceding sample's quantizer offset");
    near(changing.decode(mono(band, .35f, 0.f, 1.f)), 0., 2e-6, "the following centered sample settles");
    band.prepare(48000);
    band.setBias(.38f);
    band.setDither(true);
    UndoHighPass inverse(48000);
    double sum = 0., energy = 0.;
    for (int i = 0; i < 32768; ++i) {
        const auto q = inverse.decode(mono(band, .28f, 0.f, 1.f));
        if (i >= 4096) { sum += q; energy += q*q; }
    }
    const double mean = sum / 28672.;
    near(mean, .292408, .012, "biased dither mean");
    near(std::sqrt(energy / 28672. - mean * mean), .274116, .012, "biased dither spread");
    band.reset();
    bool silent = true;
    for (int i = 0; i < 4096; ++i) {
        float l = 0.f, r = i < 512 ? 0.f : static_cast<float>(i % 13 - 6) / 7.f;
        band.processStereo(l, r, 0.f, 1.f);
        silent &= l == 0.f && (i >= 512 || r == 0.f);
    }
    check(silent, "biased dither preserves a silent channel while the other channel crosses steps");
    Crush centered, explicitZero;
    centered.prepare(44100);
    explicitZero.prepare(44100);
    centered.setDither(true);
    explicitZero.setDither(true);
    explicitZero.setBias(0.f);
    for (int i = 0; i < 1024; ++i) {
        const float x = static_cast<float>(i % 37 - 17) / 19.f;
        const float bits = static_cast<float>(i % 13) / 12.f;
        check(mono(centered, x, bits, .61f) == mono(explicitZero, x, bits, .61f),
              "an explicit centered bias preserves dither and the fractional hold clock");
    }
    Crush full, pushed;
    full.prepare(96000);
    pushed.prepare(96000);
    pushed.setBias(1.f);
    pushed.setDither(true);
    for (int i = 0; i < 96; ++i) {
        const float x = static_cast<float>(i % 17 - 7) / 11.f;
        check(mono(full, x, 1.f, 1.f) == mono(pushed, x, 1.f, 1.f), "full resolution bypasses bias and dither");
    }
}
} // namespace
int main() try {
    Crush band;
    check(band.prepare(48000),"prepare band");
    quantizer(band);clock(band);longClock(band);combined(band);dither(band);quantizerBias();
    check(!band.prepare(std::numeric_limits<double>::infinity()),"invalid sample rate");
    near(mono(band,.321f,.1f,.1f),.321f,0,"invalid-rate dry signal");
    std::puts("ADI-Shaper Crush resolution, antialiasing, hold clock, dither, and DC response passed.");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());return 1;
}
