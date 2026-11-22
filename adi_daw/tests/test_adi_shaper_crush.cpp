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
std::array<float,2> stereoMarker(int frame) {
    if (frame < 20) return {};
    return {static_cast<float>(frame * 71 % 251 - 125) / 256.f,
            static_cast<float>((frame * 13 + 49) % 251 - 125) / 256.f};
}

void prefilterReadings() {
    // Left and right were measured together on signed, independent inputs.
    // A full plateau at 96 kHz begins before the first change of its curve.
    struct Reading {
        double rate;
        float bits, clock;
        bool flat, switched;
        int frame;
        double left, right;
    };
    constexpr Reading readings[]{
        {44100, .67f, .43f, true, false, 101, .024926441, .018489731},
        {48000, .31f, .72f, true, false, 318, .065135881, -.198494330},
        {96000, 1.f, .985f, true, false, 101, -.206670254, -.199953899},
        {96000, 1.f, 1.f, false, false, 578, .000291218, -.361458153},
        {48000, 1.f, .61f, true, true, 169, -.003104904, .205336869},
        {48000, 1.f, .61f, true, true, 293, .016960168, -.008124054}};
    for (const auto& reading : readings) {
        Crush band;
        check(band.prepare(reading.rate), "prefilter prepare");
        std::array<float,2> signal{};
        for (int i=0; i<=reading.frame; ++i) {
            signal = stereoMarker(i);
            band.setPrefilter(!reading.switched || i < 137 || i >= 271);
            band.processStereo(signal[0], signal[1], reading.bits, reading.clock, reading.flat);
        }
        near(signal[0], reading.left, .00012, "prefilter measured left");
        near(signal[1], reading.right, .00012, "prefilter measured right");
    }
}

void prefilterEndpoints() {
    Crush wet, dry;
    check(wet.prepare(96000) && dry.prepare(96000), "prepare endpoint bands");
    wet.setPrefilter(true);
    const float belowFull = std::nextafter(1.f, 0.f);
    double difference = 0;
    for (int i=0; i<512; ++i) {
        auto a = stereoMarker(i), b = a;
        const float rate = i < 256 ? 1.f : belowFull;
        wet.processStereo(a[0], a[1], .51f, rate);
        dry.processStereo(b[0], b[1], .51f, rate);
        if (i < 256) check(a == b, "flat full-rate curve skips prefilter");
        else difference += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]);
    }
    check(difference > 1., "nearest rate below full engages prefilter");
    wet.reset();
    dry.reset();
    // At full rate in a moving curve the hold is transparent. The pole
    // arithmetic and the gain cap can be checked without fractional clock drift.
    std::array<std::array<float,2>,8> memory{};
    for (int i=0; i<4096; ++i) {
        auto a = stereoMarker(i), b = a;
        for (auto& state : memory) {
            for (unsigned ch=0; ch<2; ++ch) {
                const float delta = .999686062335968f * (b[ch] - state[ch]);
                b[ch] = state[ch] + delta;
                state[ch] = b[ch] + delta;
            }
        }
        wet.processStereo(a[0], a[1], 1.f, 1.f, false);
        dry.processStereo(b[0], b[1], 1.f, 1.f);
        check(a == b, "moving full-rate pole arithmetic");
    }
    auto healthy = wet;
    float bad = std::numeric_limits<float>::quiet_NaN(), right = .13f;
    float left = .36f, expectedRight = right;
    wet.processStereo(bad, right, 1.f, .61f);
    healthy.processStereo(left, expectedRight, 1.f, .61f);
    wet.setPrefilter(false);
    check(wet.finishBlock() && !wet.finishBlock(), "repair inactive filter memories once");
    wet.setPrefilter(true);
    for (int i=0; i<256; ++i) {
        auto a = stereoMarker(i), b = a;
        wet.processStereo(a[0], a[1], .4f, .63f);
        healthy.processStereo(b[0], b[1], .4f, .63f);
        check(std::isfinite(a[0]) && a[1] == b[1], "repair preserves healthy stereo channel");
    }
}

void movingPrefilter() {
    Crush band;
    check(band.prepare(48000), "prepare moving prefilter");
    band.setPrefilter(true);
    struct Marker { int frame; double left, right; };
    constexpr std::array<Marker,6> measured{{
        {502, -.0473070815, -.1108339280}, {507, -.0533162579, -.2486526668},
        {512, -.0380109735, -.0081906626}, {999, -.0275315363, .2218676060},
        {1000, -.0724930093, .2527466416}, {1063, -.0009904406, -.0232571177}}};
    std::size_t next = 0;
    for (int i=0; i<=measured.back().frame; ++i) {
        // Sampled heights of a three-level curve, including its two cubic edges.
        float height = i < 500 ? .34f : (i < 1000 ? .83f : .44f);
        if (i == 499) height = .33439885312f;
        if (i == 999) height = .83445805568f;
        auto signal = stereoMarker(i);
        band.processStereo(signal[0], signal[1], .31f, height, false);
        if (i == measured[next].frame) {
            near(signal[0], measured[next].left, .00015, "moving prefilter left");
            near(signal[1], measured[next].right, .00015, "moving prefilter right");
            ++next;
        }
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
} // namespace
int main() try {
    Crush band;
    check(band.prepare(48000),"prepare band");
    quantizer(band);clock(band);longClock(band);combined(band);dither(band);
    prefilterReadings();prefilterEndpoints();movingPrefilter();
    check(!band.prepare(std::numeric_limits<double>::infinity()),"invalid sample rate");
    near(mono(band,.321f,.1f,.1f),.321f,0,"invalid-rate dry signal");
    std::puts("ADI-Shaper Crush resolution, antialiasing, hold clock, dither, and DC response passed.");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());return 1;
}
