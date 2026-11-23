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
    // The first crossing at the minimum height includes rate-specific
    // accumulation rounding. These indices and transitions came from DC steps.
    struct Minimum { int rate, firstFrame; double transition; };
    for (const auto [rate, firstFrame, transition] : std::array<Minimum, 3>{{
             {44100, 5000, .34711317}, {48000, 5441, .02380869}, {96000, 10885, .21713978}}}) {
        check(band.prepare(rate), "prepare");
        UndoHighPass inv(rate);
        for(int i=0;i<=firstFrame+1;++i) {
            const double value=inv.decode(mono(band,.5f,1,0));
            if(i<firstFrame) near(value,0,1e-7,"silence before first hold event");
            if(i==firstFrame) near(value,transition,.000005,"lowest hold clock transition");
            if(i==firstFrame+1) near(value,.5,.000005,"lowest hold clock");
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

void sustainedStereoClock() {
    // After more than two seconds on a steady contour, two signed input
    // frames reveal the transition and the held value in each channel.
    // Subtract the pre-marker DC level recovered on the same render.
    struct Marker {
        int rate, frame;
        float height;
        std::array<double, 2> transition, held;
    };
    constexpr Marker cases[]{
        {96000, 229771, .073000014f, {.01710536, .14570030}, {.03611648, .30763352}},
        {44100, 102540, .281310022f, {.00197571, .14995156}, {.00447023, .33927978}},
        {48000, 112731, .407000005f, {-.04772568, .11906427}, {-.22996955, .57371955}},
        {96000, 225527, .992123961f, {.03317799, .13986813}, {.06590689, .27784311}},
        // These five physical heights distinguish a separately rounded
        // log-frequency product from a fused multiply-add.
        {48000, 112691, .389999986f, {.00388830, .14949775}, {.00871396, .33503605}},
        {48000, 112800, .527122974f, {-.04516685, .13817329}, {-.16693585, .51068588}},
        {96000, 225467, .711719036f, {-.03190773, .15050797}, {-.09248112, .43623114}},
        {44100, 103641, .889999986f, {-.02148362, .15249778}, {-.05636793, .40011794}},
        {48000, 112887, .921000004f, {-.02725117, .15178435}, {-.07522165, .41897165}}
    };
    for (const auto& mark : cases) {
        Crush band;
        band.prepare(mark.rate);
        UndoHighPass left(mark.rate), right(mark.rate);
        std::array<double, 2> background{};
        for (int i = 0; i <= mark.frame + 1; ++i) {
            float l = .0625f, r = -.03125f;
            if (i == mark.frame - 1) { l = .625f; r = -.25f; }
            if (i == mark.frame) { l = -.375f; r = .75f; }
            band.processStereo(l, r, 1, mark.height);
            const std::array<double, 2> decoded{left.decode(l), right.decode(r)};
            if (i == mark.frame - 2) background = decoded;
            if (i < mark.frame) continue;
            const auto& expected = i == mark.frame ? mark.transition : mark.held;
            for (std::size_t ch = 0; ch < decoded.size(); ++ch)
                near(decoded[ch] - background[ch], expected[ch], .000005, "sustained stereo hold clock");
        }
    }
    Crush extreme;
    check(extreme.prepare(std::numeric_limits<double>::max()), "finite high sample rate");
    near(mono(extreme, .5f, 1, 0), 0., 0., "sub-resolution increment holds silence");
    check(extreme.prepare(1), "finite low sample rate");
    check(std::isfinite(mono(extreme, .5f, 1, .999f)), "low sample rate caps the increment");
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

std::array<float, 2> endpointMarker(int offset) {
    constexpr std::array<std::array<float, 2>, 3> markers{{
        {.8f, -.1f}, {-.25f, .6f}, {.16f, -.42f}}};
    if (offset < 0 || offset >= static_cast<int>(markers.size())) return {};
    return markers[static_cast<std::size_t>(offset)];
}

void fullRatePlateau() {
    // A signed pair at the end of the plateau distinguishes the passed sample
    // from the fractional capture that the next, slower frames reveal.
    struct Case { int preroll; double heldL, heldR; };
    constexpr Case cases[]{{0, .11061388, .35959082}, {8192, .42338427, .15107722}};
    for (const auto& test : cases) {
        Crush band;
        band.prepare(49152);
        UndoHighPass left(49152), right(49152);
        for (int frame = 0; frame <= test.preroll + 119; ++frame) {
            const int position = frame % 8192;
            const int stimulus = frame - test.preroll;
            auto [l, r] = endpointMarker(stimulus - 116);
            const float height = position >= 67 && position < 118 ? 1.f : .65f;
            band.processStereo(l, r, 1, height, false);
            const double a = left.decode(l), b = right.decode(r);
            if (stimulus == 117) {
                near(a, -.25, .00001, "full moving plateau passes left");
                near(b, .6, .00001, "full moving plateau passes right");
            } else if (stimulus == 118 || stimulus == 119) {
                const double tolerance = test.preroll == 0 ? .00002 : .001;
                near(a, test.heldL, tolerance, "crossing an endpoint retains the clock and left latch");
                near(b, test.heldR, tolerance, "crossing an endpoint retains the clock and right latch");
            }
        }
    }
}

void flatOrCappedHold() {
    // At 32 kHz both flat settings pass full-rate audio. Only the literal
    // full-height setting bypasses the latch. The sub-full setting still clocks.
    struct Case { float height; double firstL, firstR, nextL, nextR; };
    constexpr Case cases[]{{1.f, -.25, .6, -.09885536, .22944373},
                           {.97f, .45492853, .13004768, .27775367, -.02162898}};
    for (const auto& test : cases) {
        Crush band;
        band.prepare(32000);
        UndoHighPass left(32000), right(32000);
        for (int frame = 0; frame <= 1026; ++frame) {
            auto [l, r] = endpointMarker(frame - 1022);
            const float h = frame >= 512 && frame < 1024 ? test.height : .65f;
            band.processStereo(l, r, 1, h);
            const double a = left.decode(l), b = right.decode(r);
            constexpr double tolerance = .0003; // Accumulated frequency error before frame 512.
            if (frame == 1024 || frame == 1025) {
                near(a, frame == 1024 ? test.firstL : test.nextL, tolerance, "flat/capped left re-entry");
                near(b, frame == 1024 ? test.firstR : test.nextR, tolerance, "flat/capped right re-entry");
            } else if (frame == 1026) {
                near(a, .07451977, tolerance, "both paths resume the same left phase");
                near(b, -.19561441, tolerance, "both paths resume the same right phase");
            }
        }
    }
}
} // namespace
int main() try {
    Crush band;
    check(band.prepare(48000),"prepare band");
    quantizer(band);clock(band);longClock(band);sustainedStereoClock();combined(band);dither(band);
    fullRatePlateau(); flatOrCappedHold();
    check(!band.prepare(std::numeric_limits<double>::infinity()),"invalid sample rate");
    near(mono(band,.321f,.1f,.1f),.321f,0,"invalid-rate dry signal");
    std::puts("ADI-Shaper Crush resolution, antialiasing, hold clock, dither, and DC response passed.");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());return 1;
}
