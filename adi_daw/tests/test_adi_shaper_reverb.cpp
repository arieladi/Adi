// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/adi_shaper_reverb.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
using Room = adi::dsp::AdiShaperReverb;
using Stereo = std::array<float, 2>;
constexpr int rate = 48000, length = rate * 3;
int errors = 0;
void check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++errors;
    }
}

struct Levels {
    float send = 1, ret = 1;
    bool closeSend = false, closeReturn = false, clearTails = false;
};
std::vector<Stereo> impulse(Room& room, float width, float delay = 0, double bpm = 120,
                            Levels levels = {}) {
    room.reset();
    room.setWidth(width);
    room.setPredelay(delay, bpm);
    room.setClearTails(levels.clearTails);
    std::vector<Stereo> result(length);
    for (int n = 0; n < length; ++n) {
        float l = n == 0 ? .5f : 0.f, r = 0;
        room.processStereo(l, r, levels.closeSend && n >= 256 ? 0.f : levels.send,
                           levels.closeReturn && n >= 12000 && n < 18000 ? 0.f : levels.ret);
        result[static_cast<std::size_t>(n)] = {l, r};
    }
    return result;
}
} // namespace

int main() {
    Room room;
    float l = 1, r = 1;
    room.processStereo(l, r, 1, 1);
    check(l == 0 && r == 0, "unprepared room is silent");
    check(room.prepare(rate), "prepare stereo room");
    const auto base = impulse(room, .5f);
    std::size_t count = 0;
    double energy = 0, rightPeak = 0;
    for (const auto& frame : base) {
        if (frame[0] != 0) ++count;
        energy += frame[0] * frame[0];
        rightPeak = std::max(rightPeak, std::abs(static_cast<double>(frame[1])));
    }
    check(count > 500 && energy > .001 && rightPeak == 0, "authored FIR has a tail, independent L/R at neutral width");
    room.reset();
    room.setClearTails(true);
    bool resumesWet = true;
    for (int i = 0; i < length; ++i) {
        l = i == 0 ? .5f : 0.f;
        r = 0;
        room.processStereo(l, r, 1, 1, i >= 12000);
        const auto expected = i < 12000 ? Stereo{} : base[static_cast<std::size_t>(i)];
        resumesWet &= Stereo{l, r} == expected;
    }
    check(resumesWet && energy > 0, "inaudible wet mix keeps the tail even with Clear Tails on");
    check(impulse(room, .5f, 0, 120, {.closeSend = true}) == base, "closing send preserves the existing tail");
    const auto mono = impulse(room, 0);
    const auto attenuated = impulse(room, .5f, 0, 120, {.send = .25f, .ret = .5f});
    const auto muted = impulse(room, .5f, 0, 120, {.closeReturn = true});
    const auto cleared = impulse(room, .5f, 0, 120, {.closeReturn = true, .clearTails = true});
    bool width = true, gain = true, restores = true, clears = true;
    double laterEnergy = 0;
    for (int i = 0; i < length; ++i) {
        const auto n = static_cast<std::size_t>(i);
        const auto mid = .5f * base[n][0];
        width &= mono[n][0] == mid && mono[n][1] == mid;
        gain &= std::abs(attenuated[n][0] - .125f * base[n][0]) < 1e-8;
        restores &= muted[n][0] == (i >= 12000 && i < 18000 ? 0 : base[n][0]);
        clears &= cleared[n][0] == (i >= 12000 ? 0 : base[n][0]);
        if (i >= 18000) laterEnergy += base[n][0] * base[n][0];
    }
    check(width && gain, "independent linear send, return and mono width");
    check(restores && clears && laterEnergy > 0, "mute/reopen restores or clears the tail");
    for (const auto [p, tempo, offset] : std::array<std::array<double, 3>, 5>{{
             {.125, 120, 450}, {.25, 90, 1800}, {.5, 120, 7200}, {.75, 120, 12000}, {1., 90, 32000}}}) {
        const auto shifted = impulse(room, .5f, static_cast<float>(p), tempo);
        bool aligned = true;
        for (int i = 0; i < length; ++i) {
            const int prior = i - static_cast<int>(offset);
            aligned &= shifted[static_cast<std::size_t>(i)][0] == (prior < 0 ? 0 : base[static_cast<std::size_t>(prior)][0]);
        }
        check(aligned, "free and tempo-synced predelays give exact integer shifts");
    }
    room.setShape(.5f, 0, true);
    const auto gated = impulse(room, .5f);
    check(std::all_of(gated.begin() + rate / 4, gated.end(), [](const Stereo& x) { return x[0] == 0 && x[1] == 0; }),
          "shortest room gate fully decays");

    // Each reflection's pressure envelope falls by half over these intervals.
    // Compare the complete wet impulse with the independently shaped long tail.
    for (const auto [knob, halfLife] : std::array<std::array<double, 2>, 4>{{
             {0, .014}, {.25, .146}, {.5, .837}, {.75, 2.046}}}) {
        room.setShape(.5f, static_cast<float>(knob), false);
        const auto response = impulse(room, .5f);
        double squaredError = 0;
        for (int i = 0; i < length; ++i) {
            const auto n = static_cast<std::size_t>(i);
            const double expected = base[n][0] * std::exp2(-i / (rate * halfLife));
            const double error = response[n][0] - expected;
            squaredError += error * error;
        }
        check(std::sqrt(squaredError / energy) < .002, "room decay tracks its pressure half-life");
    }
    const auto energyTime = [](const std::vector<Stereo>& response) {
        double sum = 0, weighted = 0;
        for (std::size_t i = 0; i < response.size(); ++i) {
            const double power = static_cast<double>(response[i][0]) * response[i][0];
            sum += power;
            weighted += power * static_cast<double>(i);
        }
        return weighted / sum;
    };
    const auto nominalTime = energyTime(base);
    for (float size : {0.f, .25f, .75f, 1.f}) {
        room.setShape(size, 1, false);
        const double ratio = energyTime(impulse(room, .5f)) / nominalTime;
        const double expected = std::exp2(2. * size - 1.);
        check(std::abs(ratio / expected - 1.) < .08, "room size scales reflection times");
    }
    room.setShape(1, 1, false);
    const auto fullRoom = impulse(room, .5f);
    for (float decay : {0.f, .25f, .5f, .75f, 1.f}) {
        room.setShape(1, decay, true);
        const auto gatedRoom = impulse(room, .5f);
        const int start = static_cast<int>(std::lround(1.8 * decay * rate));
        // At maximum room size the two diffusers can carry a reflection by a
        // further 25 ms after the 200 ms fade reaches zero.
        const int end = start + rate * 225 / 1000;
        bool envelope = true, hasLateSignal = false;
        for (int i = 0; i < length; ++i) {
            const auto n = static_cast<std::size_t>(i);
            if (i < start) envelope &= gatedRoom[n] == fullRoom[n];
            if (i >= end) {
                envelope &= gatedRoom[n][0] == 0.f;
                hasLateSignal |= fullRoom[n][0] != 0.f;
            }
        }
        check(envelope && hasLateSignal, "room gate preserves the open interval and removes the late reflections");
    }
    room.reset();
    l = std::numeric_limits<float>::infinity();
    r = std::numeric_limits<float>::quiet_NaN();
    room.processStereo(l, r, 1, 1);
    bool finite = true;
    for (int i = 0; i < rate; ++i) {
        l = r = i < 500 ? std::numeric_limits<float>::max() : 0;
        room.processStereo(l, r, 1, 1);
        finite &= std::isfinite(l) && std::isfinite(r);
    }
    check(finite, "nonfinite excitation and finite overload leave the room output finite");
    return errors ? 1 : 0;
}
