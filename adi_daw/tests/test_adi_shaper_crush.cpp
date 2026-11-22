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
struct TrebleTap { int frame; double left, right; };
void trebleSteps() {
    // Signed stereo steps captured with full-speed hold and minimum resolution.
    constexpr std::array<std::array<TrebleTap, 3>, 2> taps{{
        {{{16, -.003092475, .005335633}, {24, -.221224740, .224369004}, {32, -.366270751, .370653480}}},
        {{{16, -.043584999, .075199805}, {24, -.377605557, .379767567}, {32, -.293384731, .326458722}}}
    }};
    Crush band;
    check(band.prepare(48000), "prepare treble steps");
    for (std::size_t setting = 0; setting < taps.size(); ++setting) {
        band.setTreble(setting == 0 ? 0.f : .3f);
        band.reset();
        std::size_t next = 0;
        for (int i = 0; next < taps[setting].size(); ++i) {
            float l = i >= 16 && i < 32 ? -.71f : .43f;
            float r = i >= 16 && i < 32 ? .91f : -.38f;
            band.processStereo(l, r, 0.f, 1.f);
            const auto& tap = taps[setting][next];
            if (i == tap.frame) {
                near(l, tap.left, 2e-6, "treble step left");
                near(r, tap.right, 2e-6, "treble step right");
                ++next;
            }
        }
    }
    // The eighth impulse-tail sample distinguishes the 350 Hz split at each rate.
    struct Tail { double rate, left, right; };
    constexpr Tail tails[] {{44100, .009381539, -.004573001}, {96000, .005349674, -.002607681}};
    for (const auto& tap : tails) {
        check(band.prepare(tap.rate), "prepare treble impulse");
        band.setTreble(.25f);
        float l = .6f, r = -.31f;
        for (int i = 0; i <= 8; ++i) {
            if (i != 0) l = r = 0.f;
            band.processStereo(l, r, .3f, 1.f);
        }
        near(l, tap.left, 2e-6, "treble impulse tail left");
        near(r, tap.right, 2e-6, "treble impulse tail right");
    }
}
void trebleContours() {
    // Resolution and hold change at distinct sample positions. These source
    // readings include the existing DC filter after the time-varying shelf.
    constexpr TrebleTap taps[] {
        {24, -.166701570, .328490376}, {56, -.356131196, .408803135},
        {79, .150631368, -.468152434}, {94, .208758622, -.565110445}
    };
    constexpr std::array<int, 2> inputSteps{23, 65};
    constexpr std::array<float, 3> left{.61f, -.73f, .29f};
    constexpr std::array<float, 3> right{-.35f, .82f, -.65f};
    Crush band;
    check(band.prepare(49152), "prepare moving treble");
    band.setTreble(.37f);
    std::size_t step = 0, next = 0;
    for (int i = 0; i <= 94; ++i) {
        if (step < inputSteps.size() && i == inputSteps[step]) ++step;
        float l = left[step], r = right[step];
        if (i == 54 || i == 55) l = r = 0.f;
        const float bits = i < 55 ? .25f : .75f;
        const float hold = i < 71 ? 1.f : .65f;
        band.processStereo(l, r, bits, hold);
        if (i == taps[next].frame) {
            near(l, taps[next].left, 2e-5, "moving treble left");
            near(r, taps[next].right, 2e-5, "moving treble right");
            ++next;
        }
    }
}
void trebleTransfer() {
    // Reconstruct the input to the shelf from a neutral device. Independent
    // measurements at 48 kHz give this one-pole impulse response. The gain
    // follows the product of the two contours even at fractional hold rates.
    Crush neutral, toned;
    check(neutral.prepare(48000) && toned.prepare(48000), "prepare treble transfer");
    toned.setTreble(.37f);
    UndoHighPass neutralMeter(48000), tonedMeter(48000);
    double previous = 0, filtered = 0;
    for (int i = 0; i < 4096; ++i) {
        const float bits = .1f + .8f * static_cast<float>(i % 37) / 36.f;
        const float hold = .2f + .7f * static_cast<float>(i % 61) / 60.f;
        const float input = static_cast<float>(.53 * std::sin(.13 * i) + .34 * std::cos(.379 * i));
        const double source = neutralMeter.decode(mono(neutral, input, bits, hold));
        filtered = .022398277 * (source + previous) + .955203446 * filtered;
        previous = source;
        const double gain = .37 + .63 * bits * hold;
        const double expected = filtered + gain * (source - filtered);
        near(tonedMeter.decode(mono(toned, input, bits, hold)), expected, 2e-6,
             "treble transfer before DC removal");
    }
}
void trebleHistory() {
    Crush neutral, active;
    check(neutral.prepare(48000) && active.prepare(48000), "prepare treble automation");
    active.setTreble(0.f);
    UndoHighPass neutralMeter(48000), activeMeter(48000);
    for (int i = 0; i < 32; ++i) {
        neutralMeter.decode(mono(neutral, .75f, 0.f, 1.f));
        activeMeter.decode(mono(active, .75f, 0.f, 1.f));
    }
    neutral.setTreble(0.f);
    near(neutralMeter.decode(mono(neutral, -.65f, 0.f, 1.f)),
         activeMeter.decode(mono(active, -.65f, 0.f, 1.f)), 2e-6, "neutral treble retains filter memory");
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
} // namespace
int main() try {
    Crush band;
    check(band.prepare(48000),"prepare band");
    quantizer(band);clock(band);longClock(band);combined(band);dither(band);
    trebleSteps();trebleContours();trebleTransfer();trebleHistory();
    check(!band.prepare(std::numeric_limits<double>::infinity()),"invalid sample rate");
    near(mono(band,.321f,.1f,.1f),.321f,0,"invalid-rate dry signal");
    std::puts("ADI-Shaper Crush resolution, antialiasing, hold clock, dither, and DC response passed.");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());return 1;
}
