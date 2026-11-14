// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace adi::mix::limiter::programs
{
struct Controls
{
    float threshold_db;
    float attack_seconds;
    float release_seconds;
    float lookahead_fraction;
};

inline float f32(double value) { return static_cast<float>(value); }
inline float add32(double first, double second) { return f32(first + second); }
inline float mul32(double first, double second) { return f32(first * second); }
inline double clip(double value, double lower, double upper) { return std::min(upper, std::max(lower, value)); }
inline float maximum(double first, double second) { return f32(std::max(first, second)); }
inline float minimum(double first, double second) { return f32(std::min(first, second)); }
inline float power_curve(double value, double strength) { return f32(std::pow(value, f32(15.0 * strength + 1.0))); }
inline float exponential_curve(double value, double strength) { const auto factor = 15.0 * strength + 0.01; return f32(std::expm1(factor * value) / std::expm1(factor)); }
inline float linear_crossfade(double first, double second, double mix) { const auto weight = 0.5 * (mix + 1.0); return f32(first * (1.0 - weight) + second * weight); }

template <typename Engine, typename Frame, typename Control>
auto processTransparent(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto detector_1 = engine.detector(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 0.20000004768371582, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(detector_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto filter_1 = engine.filter(0, add32(0.0, mul32(detector_1, 1.0)), std::nullopt, std::optional<float>{f32(11.891783714294434)});
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(peak_hold_1, 1.0)), add32(0.0, mul32(filter_1, clip(add32(1.0, mul32(filter_1, -1.600000262260437)), 0.0, 100.0)))));
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto transfer_curve_1 = engine.capture(exponential_curve(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 2.4000000953674316)), 0.30000001192092896));
    const auto peak_hold_2 = engine.peak(1, add32(0.0, mul32(operator_1, 1.0)), clip(add32(0.20000003278255463, mul32(transfer_curve_1, 41.666660308837891)), 0.0, 50.0));
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(peak_hold_2, 1.0)), 0.0, clip(add32(0.0039999997243285179, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), clip(add32(0.0014007567660883069, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), clip(add32(0.0977325439453125, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.448760986328125)), 0.0, 1000.0))), 0.0, 100.0))), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.0);
    const auto delay_1 = engine.delay(0, add32(0.0, mul32(capacitor_3, 1.0)), clip(add32(add32(0.10000000149011612, mul32(0.099999904632568359, clip(add32(2.0833332538604736, mul32(transfer_curve_1, -0.0173611119389534)), 0.0, 208.33332824707031))), mul32(transfer_curve_1, 8.3333325164858252e-05)), 0.0, 50.0));
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(detector_2, 1.0)), clip(add32(add32(add32(0.0020000000949949026, mul32(f32(clip(add32(0.019999999552965164, mul32(transfer_curve_1, -3.2083337306976318)), 0.0, 1.0)), -0.099999994039535522)), mul32(capacitor_3, clip(add32(0.020753221586346626, mul32(f32(clip(add32(0.019999999552965164, mul32(transfer_curve_1, -3.2083337306976318)), 0.0, 1.0)), -1.0416669845581055)), 0.0, 41.666664123535156))), mul32(transfer_curve_1, 0.4166666567325592)), 0.0, 1000.0), clip(add32(add32(0.040000006556510925, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), mul32(capacitor_3, clip(add32(-0.016770677641034126, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.0083333337679505348)), -41.666664123535156, 0.0))), 0.0, 1000.0), 1.0, clip(add32(1.0, mul32(capacitor_3, clip(add32(-0.0058304676786065102, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.00083333341171965003)), -4.1666669845581055, 0.0))), 0.0, 1.0), 0.0, clip(add32(0.0, mul32(delay_1, clip(add32(0.1666666716337204, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), clip(add32(-0.002105324063450098, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.0056250006891787052)), -41.666667938232422, 0.0))), 0.0, 4.1666669845581055))), -1.0, 1.0));
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(detector_1, 1.0)), clip(add32(0.032000001519918442, mul32(transfer_curve_1, 0.11666666716337204)), 0.0, 1000.0), 0.0080000003799796104, 1.0, 1.0, 0.0, 0.0);
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(operator_1, 1.0)), add32(0.0, mul32(capacitor_1, clip(add32(1.0, mul32(capacitor_1, clip(add32(add32(-0.022411346435546875, mul32(capacitor_2, 0.41760331392288208)), mul32(transfer_curve_1, -0.034722223877906799)), -416.66665649414062, 0.0))), 0.0, 100.0)))));
    const auto convolver_1 = engine.convolve(0, add32(0.0, mul32(operator_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0.44999998807907104, 0.5, 1.0);
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(convolver_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0, 0);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

template <typename Engine, typename Frame, typename Control>
auto processPunchy(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto detector_1 = engine.detector(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(detector_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_2 = engine.peak(1, add32(0.0, mul32(detector_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(peak_hold_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 1.0)), 0.0, 1000.0), clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.0);
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(peak_hold_2, 1.0)), 0.099999994039535522, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 9.9999990463256836)), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.5);
    const auto mixer_1 = engine.capture(linear_crossfade(add32(0.0, mul32(capacitor_1, 1.0)), add32(0.0, mul32(capacitor_3, 1.0)), clip(add32(-1.0, mul32(capacitor_3, 1.6666665077209473)), -1.0, 1.0)));
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(peak_hold_1, 1.0)), add32(0.0, mul32(mixer_1, 1.0))));
    const auto transfer_curve_1 = engine.capture(power_curve(add32(0.0, mul32(capacitor_1, 1.0)), 0.30000001192092896));
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(operator_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 0.040000002831220627)), 0.0, 1000.0), clip(add32(0.0010000000474974513, mul32(transfer_curve_1, 0.57297384738922119)), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(add32(0.014999999664723873, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.029999995604157448)), mul32(capacitor_1, 0.041666667908430099)), -1.0, 1.0));
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(operator_1, 1.0)), add32(0.0, mul32(capacitor_2, 1.0))));
    const auto convolver_1 = engine.convolve(0, add32(0.0, mul32(operator_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0.80000001192092896, 0.5, 0.20000000298023224);
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(convolver_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0, 0);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

template <typename Engine, typename Frame, typename Control>
auto processDynamic(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto filter_2 = engine.filter(1, add32(0.0, mul32(trigger_input, 1.0)), std::optional<float>{f32(5.3219285011291504)}, std::optional<float>{f32(13.773138999938965)});
    const auto transfer_curve_1 = engine.capture(exponential_curve(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 2.4000000953674316)), 0.30000001192092896));
    const auto detector_3 = engine.detector(2, add32(0.0, mul32(filter_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, clip(add32(1.048277735710144, mul32(transfer_curve_1, 0.083333335816860199)), 1.0, 100.0), 180.0, 0, false);
    const auto peak_hold_3 = engine.peak(2, add32(0.0, mul32(detector_3, 1.0)), 3.0);
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(peak_hold_3, 1.0)), 0.0, 0.0055100000463426113, 1.0, 1.0, -0.00049999949987977743, 0.00099999899975955486);
    const auto detector_4 = engine.detector(3, add32(0.0, mul32(filter_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0010004043579102)), -60.0, 60.0), 1.0, 0.0, clip(add32(1.0371667146682739, mul32(transfer_curve_1, 0.041666675359010696)), 1.0, 100.0), 180.0, 0, false);
    const auto capacitor_4 = engine.envelope(3, add32(0.0, mul32(detector_4, 1.0)), 0.083649992942810059, clip(add32(0.08999999612569809, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), clip(add32(0.0099999997764825821, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.29999995231628418)), 0.0, 100.0))), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.0);
    const auto operator_3 = engine.capture(maximum(add32(0.0, mul32(capacitor_3, 1.0)), add32(0.0, mul32(capacitor_4, 1.0))));
    const auto convolver_2 = engine.convolve(1, add32(0.0, mul32(operator_3, 1.0)), 0.5, 2, 0.5, 0.19986110925674438, 1.0);
    const auto peak_hold_4 = engine.peak(3, add32(0.0, mul32(convolver_2, 1.0)), 0.20000003278255463);
    const auto capacitor_5 = engine.envelope(4, add32(0.0, mul32(peak_hold_4, 1.0)), 0.00011000000813510269, 0.001999999862164259, 0.029999999329447746, 0.7019999623298645, 0.0, 0.0);
    const auto delay_1 = engine.delay(0, add32(0.0, mul32(convolver_2, 1.0)), 0.10000000149011612);
    const auto vca_2 = engine.vca(1, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(delay_1, clip(add32(-1.0, mul32(capacitor_5, clip(add32(add32(-16.666666030883789, mul32(capacitor_5, clip(add32(31.250003814697266, mul32(transfer_curve_1, clip(add32(-0.72337955236434937, mul32(transfer_curve_1, -12.056326866149902)), -7233.7958984375, 0.0))), 0.0, 1736.111083984375))), mul32(transfer_curve_1, clip(add32(0.1736111044883728, mul32(transfer_curve_1, 2.8935186862945557)), 0.0, 1736.111083984375))), -416.66665649414062, 0.0))), -100.0, 0.0))), 0.0, 1, 0, 0);
    const auto vca_3 = engine.vca(2, add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), 0.0, 0.0, 1, 0, 0);
    const auto detector_1 = engine.detector(0, add32(0.0, mul32(vca_2, 1.0)), clip(add32(0.0, mul32(vca_3, 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(detector_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto filter_1 = engine.filter(0, add32(0.0, mul32(detector_1, 1.0)), std::nullopt, std::optional<float>{f32(11.891783714294434)});
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(peak_hold_1, 1.0)), add32(0.0, mul32(filter_1, clip(add32(1.0, mul32(filter_1, -1.600000262260437)), 0.0, 100.0)))));
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(vca_2, 1.0)), clip(add32(0.0, mul32(vca_3, 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_2 = engine.peak(1, add32(0.0, mul32(operator_1, 1.0)), clip(add32(0.20000003278255463, mul32(transfer_curve_1, 41.666660308837891)), 0.0, 50.0));
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(peak_hold_2, 1.0)), 0.0, clip(add32(0.019999999552965164, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), clip(add32(0.00019999999494757503, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), clip(add32(0.099999994039535522, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.5)), 0.0, 1000.0))), 0.0, 100.0))), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.00099999899975955486);
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(detector_2, 1.0)), clip(add32(add32(0.0, mul32(capacitor_2, clip(add32(0.020753221586346626, mul32(-0.01200103759765625, clip(add32(1.736111044883728, mul32(transfer_curve_1, -173.61105346679688)), 0.0, 173.61109924316406))), 0.0, 41.666664123535156))), mul32(transfer_curve_1, 0.041666667908430099)), 0.0, 1000.0), clip(add32(add32(0.060000002384185791, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.40000000596046448)), mul32(capacitor_2, clip(add32(-0.016770677641034126, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.0083333337679505348)), -41.666664123535156, 0.0))), 0.0, 1000.0), 1.0, clip(add32(1.0, mul32(capacitor_2, clip(add32(-0.0058304676786065102, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.00083333341171965003)), -4.1666669845581055, 0.0))), 0.0, 1.0), 0.0, 0.0);
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(operator_1, 1.0)), add32(0.0, mul32(capacitor_1, 1.0))));
    const auto convolver_1 = engine.convolve(0, add32(0.0, mul32(operator_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0.44999998807907104, 0.5, 0.80000001192092896);
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(vca_2, 1.0)), add32(0.0, mul32(convolver_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0, 0);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

template <typename Engine, typename Frame, typename Control>
auto processAllround(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_2 = engine.peak(1, add32(0.0, mul32(detector_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(peak_hold_2, 1.0)), 0.25000002980232239, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.0);
    const auto capacitor_4 = engine.envelope(3, add32(0.0, mul32(peak_hold_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 1.0)), 0.0, 1000.0), clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), -1.0, 1.0));
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(peak_hold_2, clip(add32(0.5, mul32(0.001201629638671875, clip(add32(0.0, mul32(capacitor_1, 868.0555419921875)), 0.0, 416.66665649414062))), 0.0, 100.0))), add32(0.0, mul32(capacitor_4, 1.0))));
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(operator_1, 1.0)), clip(add32(add32(0.0, mul32(capacitor_1, clip(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 1.6666666269302368)), 0.0, 41.666664123535156))), mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 0.020000005140900612)), 0.0, 1000.0), clip(add32(0.0, mul32(capacitor_1, 0.64270830154418945)), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(0.0099999997764825821, mul32(capacitor_1, 4.1666669845581055)), -1.0, 1.0));
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(capacitor_2, 1.0)), clip(add32(0.0, mul32(capacitor_1, clip(add32(add32(0.0, mul32(capacitor_1, -1.736111044883728)), mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 1.25)), 0.0, 41.666664123535156))), 0.0, 1000.0), clip(add32(0.0, mul32(capacitor_1, 0.4166666567325592)), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.0);
    const auto detector_1 = engine.detector(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 0.20000004768371582, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(detector_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(capacitor_3, 1.0)), add32(0.0, mul32(peak_hold_1, 1.0))));
    const auto convolver_1 = engine.convolve(0, add32(0.0, mul32(operator_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 2, 0.40000000596046448, 0.5, 0.61994445323944092);
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(convolver_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 0, 0);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

template <typename Engine, typename Frame, typename Control>
auto processAggressive(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto detector_1 = engine.smart(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0);
    const auto convolver_2 = engine.convolve(1, add32(0.0, mul32(detector_1, 1.0)), 0.0f, 1, 0.0f, 0.5f, 0.0f);
    const auto delay_1 = engine.delay(0, add32(0.0, mul32(convolver_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto detector_3 = engine.detector(2, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto capacitor_5 = engine.envelope(4, add32(0.0, mul32(detector_3, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 2.0)), 0.0, 1000.0), 0.0, 1.0, 1.0, 0.0, 0.05000000074505806);
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), mul32(detector_1, clip(add32(-16.25, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 25.0)), -250.0, 0.0))), -60.0, 60.0), 1.0, 0.0, clip(add32(1.0, mul32(capacitor_5, 208.33332824707031)), 1.0, 100.0), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), -1.0)), 0.0, 180.0), 0, false);
    const auto averaging_filter_1 = engine.average(0, add32(0.0, mul32(detector_2, 1.0)), clip(add32(5.0000004768371582, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 160.00003051757812)), 0.0, 50.0));
    const auto averaging_filter_2 = engine.average(1, add32(0.0, mul32(averaging_filter_1, 1.0)), clip(add32(5.0000004768371582, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 160.00003051757812)), 0.0, 50.0));
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(averaging_filter_2, 1.0)), 0.0025000001769512892, clip(add32(0.0024999999441206455, mul32(averaging_filter_2, clip(add32(0.4166666567325592, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 8.3333330154418945)), 0.0, 41.666664123535156))), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(0.019999999552965164, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.935999870300293)), -1.0, 1.0));
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(capacitor_1, 1.0)), 0.0025000001769512892, clip(add32(0.0, mul32(capacitor_1, clip(add32(0.041666667908430099, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.75000011920928955)), 0.0, 41.666664123535156))), 0.0, 1000.0), 1.0, 1.0, 0.0, 0.0);
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(capacitor_2, 1.0)), 0.0, clip(add32(add32(0.0, mul32(capacitor_1, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.83333331346511841)), 0.0, 41.666664123535156))), mul32(capacitor_2, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.83333331346511841)), 0.0, 41.666664123535156))), 0.0, 1000.0), 0.20000000298023224, 1.0, 0.0, 3.9999486034503207e-05);
    const auto delay_2 = engine.delay(1, add32(0.0, mul32(capacitor_3, 1.0)), clip(add32(39.999996185302734, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), -200.0)), 0.0, 50.0));
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(delay_2, 1.0)), add32(0.0, mul32(detector_1, 1.0))));
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(operator_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), clip(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 2.5000004768371582)), 0.0, 1.0)), 5.0)), 0.0, 5000.0))), 0.0, 50.0));
    const auto convolver_1 = engine.convolve(0, add32(0.0, mul32(peak_hold_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 2, 0.5, 0.20000000298023224, 1.0);
    const auto capacitor_4 = engine.envelope(3, add32(0.0, mul32(convolver_1, 1.0)), clip(add32(0.00050000008195638657, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 2.5000004768371582)), 0.0, 1.0)), -0.00049999996554106474)), 0.0, 1000.0), 0.0, 1.0, 1.0, 0.0, 0.0);
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(delay_1, 1.0)), add32(0.0, mul32(capacitor_4, 1.0))));
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(operator_2, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 1, 0);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

template <typename Engine, typename Frame, typename Control>
auto processModern(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto delay_1 = engine.delay(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 8.0000009536743164)), 0.0, 1.0)), 2.0000002384185791)), 0.0, 50.0));
    const auto detector_4 = engine.detector(3, add32(0.0, mul32(delay_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 1, false);
    const auto transfer_curve_1 = engine.capture(power_curve(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), clip(add32(1.2239997386932373, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), -0.57599997520446777)), 0.0, 2400.0))), clip(add32(0.079999998211860657, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), -0.54000002145767212)), 0.0, 1.0)));
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(detector_4, 1.0)), clip(add32(0.0, mul32(transfer_curve_1, clip(add32(4.1666665077209473, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 0.50000005960464478)), 0.0, 208.33332824707031))), 0.0, 50.0));
    const auto peak_hold_4 = engine.peak(3, add32(0.0, mul32(detector_4, 1.0)), clip(add32(0.0, mul32(transfer_curve_1, clip(add32(4.1666665077209473, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 0.50000005960464478)), 0.0, 208.33332824707031))), 0.0, 50.0));
    auto capacitor_1 = peak_hold_1;
    auto capacitor_3 = peak_hold_4;
    bool skipControls = false;
    if constexpr (requires { engine.neutralEnvelope(0, peak_hold_1); })
        skipControls = engine.neutralEnvelope(0, peak_hold_1) && engine.neutralEnvelope(2, peak_hold_4);
    if (!skipControls) {
        const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 6.0000004768371582, 1, true);
        const auto peak_hold_2 = engine.peak(1, add32(0.0, mul32(detector_2, 1.0)), clip(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 8.0000009536743164)), 0.0, 1.0)), 2.0000002384185791)), 0.0, 50.0));
        const auto capacitor_4 = engine.envelope(3, add32(0.0, mul32(peak_hold_2, 1.0)), 0.0, 0.043999996036291122, 1.0, 0.014583333395421505, 1.0, 0.024000015109777451);
        const auto detector_3 = engine.detector(2, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, clip(add32(0.0, mul32(capacitor_4, 0.23999999463558197)), 0.0, 30.0), clip(add32(7.9999995231628418, mul32(capacitor_4, -0.25)), 1.0, 100.0), 12.000000953674316, 1, true);
        const auto peak_hold_3 = engine.peak(2, add32(0.0, mul32(detector_3, 0.5)), clip(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 8.0000009536743164)), 0.0, 1.0)), 2.0000002384185791)), 0.0, 50.0));
        const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(peak_hold_3, 1.0)), 0.0, 0.067000009119510651, 1.0, 0.081472225487232208, 1.0, 0.0029999923426657915);
        capacitor_1 = engine.envelope(0, add32(0.0, mul32(peak_hold_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 0.25)), 0.0, 1000.0), clip(add32(add32(0.0010000000474974513, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.5)), mul32(capacitor_2, 1.0000001192092896)), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(0.0, mul32(capacitor_2, 0.10000001639127731)), -1.0, 1.0));
        capacitor_3 = engine.envelope(2, add32(0.0, mul32(peak_hold_4, 1.0)), clip(add32(0.25999999046325684, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 0.25)), 0.0, 1000.0), clip(add32(add32(0.5, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), mul32(capacitor_2, clip(add32(1.0000001192092896, mul32(capacitor_4, 2.0833330154418945)), 0.0, 41.666664123535156))), 0.0, 1000.0), 0.10669444501399994, 0.94819444417953491, 0.0, clip(add32(0.00099999899975955486, mul32(capacitor_2, clip(add32(0.10000001639127731, mul32(capacitor_4, 0.20833329856395721)), 0.0, 4.1666669845581055))), -1.0, 1.0));
    }
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(capacitor_1, 1.0)), add32(0.0, mul32(capacitor_3, 1.0))));
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(operator_1, -1.0)), clip(add32(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 8.0000009536743164)), 0.0, 1.0)), 2.0000002384185791)), mul32(transfer_curve_1, clip(add32(4.1666665077209473, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 0.50000005960464478)), 0.0, 208.33332824707031))), 0.0, 5.0), 0, 0, 1);
    const auto detector_1 = engine.smart(0, add32(0.0, mul32(vca_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 1);
    const auto vca_2 = engine.vca(1, add32(0.0, mul32(vca_1, 1.0)), add32(0.0, mul32(detector_1, -1.0)), 0.0, 0, 1, 1);
    return f32(add32(0.0, mul32(vca_2, 1.0)));
}


template <typename Engine, typename Frame, typename Control>
auto processBus(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto detector_1 = engine.smart(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 1);
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 1, false);
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(detector_2, 1.0)), 0.34999996423721313, clip(add32(1.008313876837974e-09, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.0)), 0.0, 1000.0), 1.0, 0.99900001287460327, 0.0, 0.0);
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(detector_2, 1.0)), add32(0.0, mul32(capacitor_1, 1.0))));
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(operator_1, 1.0)), 0.0, clip(add32(0.015000001527369022, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.05000000074505806)), 0.0, 1000.0), 1.0, 0.0, 0.0, 0.0);
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(detector_2, 1.0)), add32(0.0, mul32(capacitor_2, 1.0))));
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(operator_2, 1.0)), clip(add32(0.00031781481811776757, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), 0.30000001192092896)), 0.0, 1000.0), clip(add32(0.015000001527369022, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.5)), 0.0, 1000.0), 0.5, 0.75, 0.0, clip(add32(0.15000000596046448, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.10000000149011612)), -1.0, 1.0));
    const auto averaging_filter_1 = engine.average(0, add32(0.0, mul32(capacitor_3, 1.0)), 10.000000953674316);
    const auto delay_1 = engine.delay(0, add32(0.0, mul32(averaging_filter_1, 1.0)), clip(add32(48.0, mul32(f32(clip(f32(controls.attack_seconds), 0.0, 1000.0)), -50.0)), 0.0, 50.0));
    const auto operator_3 = engine.capture(maximum(add32(0.0, mul32(detector_1, 1.0)), add32(0.0, mul32(delay_1, 1.0))));
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(operator_3, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto convolver_1 = engine.convolve(0, add32(0.0, mul32(peak_hold_1, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 1.0, 0.5, 0.0);
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(convolver_1, -1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.lookahead_fraction), 0.0, 1.0)), 5.0)), 0.0, 5.0), 1, 1, 1);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

template <typename Engine, typename Frame, typename Control>
auto processSafe(Engine& engine, const Control& controls, const Frame& audio_input, const Frame& trigger_input)
{
    const auto detector_1 = engine.smart(0, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0);
    const auto delay_4 = engine.delay(3, add32(0.0, mul32(detector_1, 1.0)), clip(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.9999998807907104)), 0.0, 1.0)), 5.0)), 0.0, 50.0));
    const auto detector_2 = engine.detector(1, add32(0.0, mul32(trigger_input, 1.0)), clip(add32(0.0, mul32(f32(clip(f32(controls.threshold_db), -60.0, 60.0)), 1.0)), -60.0, 60.0), 1.0, 0.0, 100.0, 180.0, 0, false);
    const auto peak_hold_1 = engine.peak(0, add32(0.0, mul32(detector_2, 1.0)), clip(add32(add32(0.5, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.9999998807907104)), 0.0, 1.0)), 5.0)), mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 2.0)), 0.0, 50.0));
    const auto delay_1 = engine.delay(0, add32(0.0, mul32(peak_hold_1, 1.0)), clip(add32(49.5, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), -2.0)), 0.0, 50.0));
    const auto peak_hold_2 = engine.peak(1, add32(0.0, mul32(detector_2, 1.0)), 8.4999990463256836);
    const auto capacitor_1 = engine.envelope(0, add32(0.0, mul32(peak_hold_2, 1.0)), 0.020000001415610313, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.029999995604157448)), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 0.004999999888241291)), -1.0, 1.0));
    const auto delay_2 = engine.delay(1, add32(0.0, mul32(capacitor_1, 1.0)), 41.499996185302734);
    const auto operator_1 = engine.capture(maximum(add32(0.0, mul32(delay_1, 1.0)), add32(0.0, mul32(delay_2, 1.0))));
    const auto peak_hold_3 = engine.peak(2, add32(0.0, mul32(detector_2, 1.0)), 39.999996185302734);
    const auto capacitor_2 = engine.envelope(1, add32(0.0, mul32(peak_hold_3, 1.0)), clip(add32(0.41999995708465576, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), -0.040000002831220627)), 0.0, 1000.0), clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 2.0)), 0.0, 1000.0), 1.0, 1.0, 0.0, clip(add32(0.5, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 2.0)), -1.0, 1.0));
    const auto delay_3 = engine.delay(2, add32(0.0, mul32(capacitor_2, 1.0)), 10.000000953674316);
    const auto operator_2 = engine.capture(maximum(add32(0.0, mul32(operator_1, 1.0)), add32(0.0, mul32(delay_3, 1.0))));
    const auto averaging_filter_1 = engine.average(0, add32(0.0, mul32(operator_2, 1.0)), clip(add32(add32(0.5, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.9999998807907104)), 0.0, 1.0)), 5.0)), mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 2.0)), 0.0, 50.0));
    const auto capacitor_3 = engine.envelope(2, add32(0.0, mul32(averaging_filter_1, 1.0)), 0.0, 0.0, 1.0, 1.0, 0.0, 0.019999999552965164);
    const auto operator_3 = engine.capture(maximum(add32(0.0, mul32(delay_4, 1.0)), add32(0.0, mul32(capacitor_3, 1.0))));
    const auto vca_1 = engine.vca(0, add32(0.0, mul32(audio_input, 1.0)), add32(0.0, mul32(operator_3, 1.0)), clip(add32(0.0, mul32(f32(clip(add32(0.0, mul32(f32(clip(f32(controls.release_seconds), 0.0, 1000.0)), 1.9999998807907104)), 0.0, 1.0)), 5.0)), 0.0, 5.0), 0, 1, 0);
    return f32(add32(0.0, mul32(vca_1, 1.0)));
}

}
