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
void movingResolution() {
    // Stereo markers measured at 49152 Hz with an abrupt resolution change.
    // Both channels share the contour and retain their own signed audio history.
    struct Marker {
        float bitsBefore, bitsAfter, leftBefore, rightBefore, left, right;
        double expectedLeft, expectedRight;
    };
    constexpr Marker markers[] {
        {.25f, .75f, .375f, -.625f, -.21875f, .15625f, .084458889, -.225830705},
        {.25f, .75f, .20f, -.20f, .07f, -.07f, .067350056, -.067350059}, // Same integer codes.
        {.75f, .25f, .375f, -.625f, -.21875f, .15625f, .063210541, -.192970153},
        {.75f, .25f, -.4375f, .875f, .8125f, -.3125f, .169565353, .277776586},
        {1.f, .125f, .375f, -.625f, -.21875f, .15625f, .004737242, -.260156898},
        {.125f, 1.f, -.4375f, .875f, .8125f, -.3125f, .8125, -.3125}
    };
    Crush band;
    check(band.prepare(49152), "prepare resolution markers");
    for (const auto& m : markers) {
        band.reset();
        UndoHighPass leftMeter(49152), rightMeter(49152);
        float l = m.leftBefore, r = m.rightBefore;
        band.processStereo(l, r, m.bitsBefore, 1.f);
        leftMeter.decode(l);
        rightMeter.decode(r);
        l = m.left;
        r = m.right;
        band.processStereo(l, r, m.bitsAfter, 1.f);
        near(leftMeter.decode(l), m.expectedLeft, 2e-6, "moving resolution left");
        near(rightMeter.decode(r), m.expectedRight, 2e-6, "moving resolution right");
    }

    // An offset of two integer codes in both samples leaves the random crossing
    // weight identical between ears. Recover it from the left output and check
    // the right output against its independently known physical levels.
    band.reset();
    band.setDither(true);
    UndoHighPass leftMeter(49152), rightMeter(49152);
    float l = -1.f, r = 1.f;
    band.processStereo(l, r, 0.f, 1.f);
    leftMeter.decode(l);
    rightMeter.decode(r);
    constexpr double newStep = 1.0 / 7.999794;
    constexpr double levelGain = .96875;
    l = static_cast<float>(newStep);
    r = static_cast<float>(3 * newStep);
    band.processStereo(l, r, .5f, 1.f);
    const double weight = (leftMeter.decode(l) / levelGain + 1.0) / (newStep + 1.0);
    near(rightMeter.decode(r), levelGain * (1.0 + weight * (3 * newStep - 1.0)),
         2e-6, "stereo dither while resolution changes");
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
    // Nonfinite controls select full height. Finite controls clamp to [0, 1].
    Crush reference;
    check(reference.prepare(48000), "prepare control comparison");
    constexpr auto inf = std::numeric_limits<float>::infinity();
    constexpr float heights[]{-inf, -1.f, .5f, 2.f, inf, std::numeric_limits<float>::quiet_NaN()};
    constexpr float expected[]{1.f, 0.f, .5f, 1.f, 1.f, 1.f};
    for (std::size_t i = 0; i < std::size(heights); ++i) {
        const float s = (i % 2 == 0) ? .77f : -.43f;
        near(mono(band, s, heights[i], heights[i]), mono(reference, s, expected[i], expected[i]),
             0., "out-of-range control keeps the reference history");
    }
    band.reset();
    quantizer(band);movingResolution();clock(band);longClock(band);sustainedStereoClock();combined(band);dither(band);quantizerBias();
    trebleSteps();trebleContours();trebleTransfer();trebleHistory();
    jitterStatistics(); jitterCap(); jitterBypass(); jitterAndDither();
    prefilterReadings();prefilterEndpoints();movingPrefilter();
    fullRatePlateau(); flatOrCappedHold();
    check(!band.prepare(std::numeric_limits<double>::infinity()),"invalid sample rate");
    near(mono(band,.321f,.1f,.1f),.321f,0,"invalid-rate dry signal");
    std::puts("ADI-Shaper Crush resolution, antialiasing, hold clock, dither, and DC response passed.");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"%s\n",e.what());return 1;
}
