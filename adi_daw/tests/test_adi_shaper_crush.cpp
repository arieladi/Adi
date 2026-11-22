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

void jitterStatistics() {
    // Capture intervals measured on a stationary 644 Hz contour, averaged over
    // many cycles. Each generator can use its own reproducible sequence.
    struct Reading { float jitter; double mean, deviation; };
    constexpr Reading readings[]{{.25f, 1.01, .26}, {.5f, 1.045, .92},
                                  {.75f, .987, 1.36}, {1.f, 1.056, 1.88}};
    constexpr double nominal = 74.497;
    for (const auto& reading : readings) {
        Crush band;
        band.prepare(48000);
        band.setJitter(reading.jitter);
        UndoHighPass inverse(48000);
        double previous = 0., sum = 0., squares = 0.;
        int last = -1, intervals = 0;
        bool changing = false;
        for (int i = 0; i < 192000; ++i) {
            const float ramp = static_cast<float>(i % 4093 - 2046) / 4096.f;
            const double sample = inverse.decode(mono(band, ramp, 1, .25f));
            const bool changed = std::abs(sample - previous) > 2.e-6;
            if (changed && !changing) {
                if (last >= 0) {
                    const double frames = i - last;
                    sum += frames;
                    squares += frames * frames;
                    ++intervals;
                }
                last = i;
            }
            previous = sample;
            changing = changed;
        }
        check(intervals > 2000, "enough jitter captures");
        const double mean = sum / intervals;
        near(mean / nominal, reading.mean, .022, "jitter interval mean");
        near(std::sqrt((squares / intervals - mean * mean) / nominal),
             reading.deviation, .095, "jitter interval spread");
    }
}

void jitterCap() {
    // A high-rate frame passes its stereo inputs but stores an interpolated
    // capture. The following zero-rate frame exposes that private held value.
    // These markers pin ADI's own repeatable seed and the fractional clock.
    struct Marker { int rate; float height; int frame; double left, right; };
    constexpr Marker cases[]{{32000, .99f, 16, .302016, -.373488},
                             {44100, .95f, 3, .687959, -.084031},
                             {48000, .95f, 3, .632062, -.125953},
                             {96000, .95f, 3, .316031, -.362977}};
    for (const auto& m : cases) {
        Crush band;
        band.setJitter(1);
        band.prepare(m.rate);
        UndoHighPass left(m.rate), right(m.rate);
        for (int k = 0; k <= m.frame + 1; ++k) {
            float l = k == m.frame - 1 ? .8f : 0.f;
            float r = k == m.frame ? -.6f : 0.f;
            band.processStereo(l, r, 1, m.height);
            const double a = left.decode(l), b = right.decode(r);
            if (k == m.frame) {
                near(a, 0., .00001, "saturated left passes");
                near(b, -.6, .00001, "saturated right passes");
            } else if (k == m.frame + 1) {
                near(a, m.left, .00001, "saturated left capture");
                near(b, m.right, .00001, "saturated right capture");
            }
        }
    }
}

void jitterBypass() {
    Crush a;
    a.prepare(48000);
    a.setJitter(1);
    for (int i = 0; i < 113; ++i) mono(a, 0, 1, .45f);
    auto resumed = a;
    for (int i = 0; i < 1024; ++i)
        near(mono(a, 0, 1, 1), 0, 0, "flat full-height bypass");
    for (int i = 0; i < 2048; ++i) {
        const float input = static_cast<float>((i * 29) % 113 - 56) / 113.f;
        near(mono(a, input, 1, .45f), mono(resumed, input, 1, .45f), 0,
             "flat full-height pauses the hold phase and timing draws");
    }
    a.reset();
    resumed.prepare(48000);
    Crush moving = a;
    bool differs = false;
    for (int i = 0; i < 256; ++i) {
        const float x = i % 3 == 0 ? .6f : -.25f;
        float l = x, r = x, l2 = x, r2 = x;
        moving.processStereo(l, r, 1, 1, false);
        resumed.processStereo(l2, r2, 1, 1, false);
        const float flat = mono(a, x, 1, 1);
        differs = differs || l != flat;
        check(l == l2 && r == r2, "reset and reprepare repeat the clock stream");
    }
    check(differs, "active contour jitters through the full-height plateau");
}

void jitterAndDither() {
    Crush both, quantizerOnly, clockOnly;
    both.prepare(48000);
    quantizerOnly.prepare(48000);
    clockOnly.prepare(48000);
    both.setJitter(.75f);
    clockOnly.setJitter(.75f);
    UndoHighPass beforeHold(48000);
    for (int i = 0; i < 8192; ++i) {
        const bool dithered = i < 512 || i >= 2048;
        both.setDither(dithered);
        quantizerOnly.setDither(dithered);
        const float height = i >= 3072 && i < 4096 ? 1.f : .45f;
        const float input = static_cast<float>((i * 79) % 253 - 126) / 180.f;
        const float quantized = static_cast<float>(beforeHold.decode(mono(quantizerOnly, input, .1f, 1)));
        near(mono(both, input, .1f, height), mono(clockOnly, quantized, 1, height),
             .000003, "timing and dither retain their separate positions");
    }
}
} // namespace
int main() try {
    Crush band;
    check(band.prepare(48000),"prepare band");
    quantizer(band);clock(band);longClock(band);combined(band);dither(band);
    jitterStatistics(); jitterCap(); jitterBypass(); jitterAndDither();
    check(!band.prepare(std::numeric_limits<double>::infinity()),"invalid sample rate");
    near(mono(band,.321f,.1f,.1f),.321f,0,"invalid-rate dry signal");
    std::puts("ADI-Shaper Crush resolution, antialiasing, hold clock, dither, and DC response passed.");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());return 1;
}
