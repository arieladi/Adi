// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_pitch.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>

namespace {
using Pitch = adi::dsp::AdiShaperPitch;
constexpr double tau = 2 * std::numbers::pi_v<double>;

void check(bool result, const char* message)
{
    if (!result) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

void unisonTiming()
{
    struct Reading { int rate; float grain; int arrival; float amplitude; };
    constexpr std::array<Reading, 11> readings{{
        {44100,.5f,575,.41229f}, {96000,.5f,1243,.44998f},
        {48000,0,31,.44998f}, {48000,.125f,68,.44547f}, {48000,.25f,179,.32259f},
        {48000,.375f,365,.44547f}, {48000,.5f,625,.44998f}, {48000,.625f,959,.44547f},
        {48000,.75f,1367,.32265f}, {48000,.875f,1850,.44547f}, {48000,1,2407,.44998f}
    }};
    static_assert(Pitch::latencySamples == 0);
    for (const auto& reading : readings) {
        Pitch pitch;
        check(pitch.prepare(reading.rate), "Pitch preparation failed");
        float peak = 0;
        int at = -1;
        double sum = 0;
        for (int n = 0; n <= reading.arrival + 20; ++n) {
            float left = n == 0 ? .5f : 0.0f, right = 0;
            pitch.processStereo(left, right, .5f, reading.grain);
            check(right == 0, "Impulse leaked into the silent channel");
            if (left > peak) { peak = left; at = n; }
            sum += left;
        }
        check(at == reading.arrival, "Incorrect physical grain delay at unison");
        check(std::abs(peak - reading.amplitude) < .002f, "Unexpected unison impulse crest");
        check(std::abs(sum - .5) < .002, "Unison impulse area");
    }
}

void shiftedTransient()
{
    // Place the click at the centre of a grain. Its surrounding taps expose
    // both fractional read speed and the upward pitch anti-alias filter.
    struct Reading { float height; std::array<float, 5> taps; };
    constexpr std::array<Reading, 5> readings{{
        {0, {.32259f,.42147f,.44998f,.40036f,.29104f}},
        {.25f, {.04920f,.32259f,.44998f,.29104f,.04003f}},
        {.5f, {-.03804f,.04920f,.44998f,.04003f,-.02491f}},
        {.75f, {-.00640f,.02001f,.22499f,.01311f,-.00245f}},
        {1, {0,.00337f,.11250f,.00129f,0}}
    }};
    for (const auto& reading : readings) {
        Pitch pitch;
        check(pitch.prepare(48000), "Pitch preparation failed");
        for (int n = 0; n <= 24722; ++n) {
            float left = n == 24095 ? .5f : 0.0f, right = -left;
            pitch.processStereo(left, right, reading.height, .5f);
            check(left == -right, "Pitch inverted-stereo cancellation");
            if (n >= 24718) {
                const auto i = static_cast<std::size_t>(n - 24718);
                if (std::abs(left - reading.taps[i]) >= .002) {
                    std::fprintf(stderr, "Height %.3f at frame %d: %.6f, expected %.6f\n",
                                 reading.height, n, left, reading.taps[i]);
                    check(false, "Shifted transient interpolation");
                }
            }
        }
    }
}

void sustainedPitch()
{
    struct Reading { int semitones; std::array<int, 4> bins; std::array<double, 4> amplitude; };
    constexpr std::array<Reading, 6> readings{{
        {-24,{93,152,236,178},{.16910,.05115,.15409,.04161}},
        {-12,{210,249,450,411},{.19731,.05243,.15568,.04162}},
        {-7,{288,314,593,567},{.19726,.05268,.15720,.04158}},
        {7,{675,637,1303,1341},{.19133,.04583,.15191,.03677}},
        {12,{909,831,1731,1809},{.19715,.04936,.14552,.04155}},
        {24,{1841,1608,3440,3673},{.17440,.04730,.13851,.03618}}
    }};
    for (const auto& reading : readings) {
        Pitch pitch;
        check(pitch.prepare(48000), "Pitch preparation failed");
        const float height = .5f + reading.semitones / 48.0f;
        std::array<double, 4> real{}, imag{};
        for (int n = 0; n < 96000; ++n) {
            const double angle = tau * n / 48000;
            float left = static_cast<float>(.25 * std::sin(angle * 443));
            float right = static_cast<float>(.20 * std::sin(angle * 877 + .33));
            pitch.processStereo(left, right, height, .5f);
            if (n < 48000) continue;
            const double hann = .5 - .5 * std::cos(tau * (n - 48000) / 47999);
            for (std::size_t i = 0; i < 4; ++i) {
                const double sample = hann * (i < 2 ? left : right);
                real[i] += sample * std::cos(angle * reading.bins[i]);
                imag[i] += sample * std::sin(angle * reading.bins[i]);
            }
        }
        for (std::size_t i = 0; i < 4; ++i) {
            const double amplitude = std::hypot(real[i], imag[i]) / 12000;
            if (std::abs(amplitude - reading.amplitude[i]) >= .00025) {
                std::fprintf(stderr, "%+d semitones at %d Hz: got %.6f, expected %.6f\n",
                             reading.semitones, reading.bins[i], amplitude, reading.amplitude[i]);
                check(false, "Grain sideband amplitude");
            }
        }
    }
}

void automationAndReset()
{
    Pitch pitch;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    check(!pitch.prepare(nan), "Invalid sample rate accepted");
    float left = .5f, right = -.25f;
    pitch.processStereo(left, right, nan, nan);
    check(left == .5f && right == -.25f, "Unprepared dry passthrough");
    check(pitch.prepare(48000), "Pitch preparation failed");
    for (int i = 0; i < 30000; ++i) {
        left = static_cast<float>(.2 * std::sin(i * .03));
        right = -left;
        const float height = static_cast<float>(.5 + .6 * std::sin(i * .002));
        const float grain = static_cast<float>(.5 + .6 * std::sin(i * .0007));
        const float bad = i % 701 == 0 ? nan : height;
        pitch.processStereo(left, right, bad, grain);
        check(std::isfinite(left) && std::abs(left) < .5f && left == -right,
              "Automated output stays finite and preserves phase");
    }
    pitch.reset();
    for (int i = 0; i < 6000; ++i) {
        left = right = i == 0 ? nan : 0.0f;
        pitch.processStereo(left, right, .75f, 1);
        check(left == 0 && right == 0, "Reset or invalid input leaked into history");
    }
}

void movingPitch()
{
    // One bar at 120 BPM: sweep from -12 to +12 semitones and back. The last
    // editable contour point is one grid step before the end of the bar.
    struct Reading { int frame; float left; float right; };
    constexpr std::array<Reading, 12> readings{{
        {4800,-.101121f,-.130829f},{12000,.078862f,-.036790f},{24000,-.223238f,.131159f},
        {36000,.238805f,.020459f},{48000,.088898f,-.135177f},{60000,-.170075f,.151924f},
        {72000,.089387f,.124940f},{84000,-.148971f,.080115f},{96000,-.164789f,-.109482f},
        {108000,.000153f,-.073348f},{120000,.232752f,.115357f},{132000,-.181606f,.013277f}
    }};
    Pitch pitch;
    check(pitch.prepare(48000), "Pitch preparation failed");
    std::size_t next = 0;
    for (int n = 0; n <= readings.back().frame; ++n) {
        const int position = n % 96000;
        const double height = position <= 48000 ? .25 + .5 * position / 48000
                            : std::max(.25, .75 - .5 * (position - 48000) / (48000.0 * 4095 / 4096));
        const double angle = tau * n / 48000;
        float left = static_cast<float>(.25 * std::sin(angle * 443));
        float right = static_cast<float>(.20 * std::sin(angle * 877 + .33));
        pitch.processStereo(left, right, static_cast<float>(height), .5f);
        if (n != readings[next].frame) continue;
        check(std::abs(left - readings[next].left) < .001f &&
              std::abs(right - readings[next].right) < .001f,
              "Audio-rate pitch sweep diverged");
        ++next;
    }
}
}

int main()
{
    unisonTiming();
    shiftedTransient();
    sustainedPitch();
    movingPitch();
    automationAndReset();
    std::puts("ADi-SHAPER cyclic pitch tests passed");
}
