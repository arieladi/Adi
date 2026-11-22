// SPDX-License-Identifier: GPL-3.0-or-later
#include "overdrive.hpp"
#include "byod_ojd.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
namespace adi::dsp {
// DEV-22: Live 12.4 reports 5 samples for its Overdrive (the research's
// latency table), which points at an oversampled shaper; ADI's ran at the base
// rate, aliasing, and reported 0. The shaper now runs at 4x. The resampling
// filter is a 185-tap Kaiser-windowed sinc (cutoff 0.12 of the 4x rate,
// beta 10) made minimum phase (real cepstrum, 65 536 points), renormalised to
// unity DC gain: flat to 20 kHz at 48 kHz (-0.003 dB), at least 61 dB down
// from 0.135 of the 4x rate, and 10.03 taps of low-frequency group delay a
// pass, so 5.02 samples for both passes at the base rate. test_overdrive.cpp
// checks these properties.
const std::array<double, Overdrive::kTaps> &Overdrive::resamplingFilter() noexcept {
    static constexpr std::array<double, kTaps> kMinimumPhase{
    2.710216939710855e-05, 0.0001961339072871145, 0.0008228706601281655,
    0.002570554924841616, 0.006569437588613236, 0.014404103566569546,
    0.02785106884866368, 0.04830000036458087, 0.07591744844997031,
    0.10878444916274486, 0.14238799753083206, 0.16987783382864496,
    0.18334148307449066, 0.17599967243262993, 0.14477870199609558,
    0.0923589049352672, 0.02773096712295764, -0.03536233097163892,
    -0.0820044372107578, -0.10075294282877867, -0.08776843698047104,
    -0.04875179592345081, 0.00236357301503719, 0.04784541567622049,
    0.07230878948680816, 0.06830280776701926, 0.039082619872266,
    -0.002701108173144492, -0.04004171512843447, -0.058400811189098525,
    -0.051526470731213164, -0.023804864478204294, 0.011884647075703645,
    0.03990002319533528, 0.04865056694656018, 0.03549132256523932,
    0.007546643559023107, -0.02184497088518003, -0.03937679364044553,
    -0.037804768566308315, -0.018996091210250712, 0.007337754268172724,
    0.028508784781022874, 0.03492460378597268, 0.02445614706801982,
    0.003083800168107271, -0.018352038617055522, -0.029586916619125193,
    -0.025843389300383532, -0.009855805631105599, 0.009855717922921316,
    0.023453236948258176, 0.024671723846230308, 0.013700735072943273,
    -0.003273522387455267, -0.017474665129788406, -0.022045678978132574,
    -0.015350945833988697, -0.0014793420328592965, 0.012153518495971953,
    0.018735637857328156, 0.015449705253610377, 0.0046387861433129915,
    -0.00771417644406286, -0.01525858429329213, -0.014525241748647664,
    -0.006494573440357587, 0.004211329795718644, 0.011944596027395664,
    0.012992876958557366, 0.0073399771548918365, -0.0015986401086205279,
    -0.008987546599625017, -0.011166747764598348, -0.007445887963019175,
    -0.00022689349587415373, 0.006483194880086986, 0.009274020604523868,
    0.007047981208445134, 0.0013955473440104315, -0.004458144548551007,
    -0.007469153834338588, -0.00634126488474944, -0.002045074186162824,
    0.0028922841796924764, 0.0058474982788800905, 0.0054791609049412646,
    0.002307439060913335, -0.0017363905381875813, -0.004458019919322108,
    -0.004575579995196252, -0.0023003563353170167, 0.0009258610571260408,
    0.0033150048925028124, 0.003709041884616965, 0.002122538768063332,
    -0.00039115483155643835, -0.0024085484740266564, -0.00292810509502585,
    -0.0018519201441052752, 6.52143756433811e-05, 0.0017134452299399925,
    0.0022571755363117087, 0.0015456666687947832, 0.0001109814716082513,
    -0.0011975522795726124, -0.0017040148428844007, -0.0012441334225038725,
    -0.00018832506575103364, 0.0008241551255325806, 0.0012613516279847078,
    0.0009689928404082349, 0.0002030845178226995, -0.0005622171628626935,
    -0.0009185500348931983, -0.0007336825486540914, -0.00018435771056100517,
    0.0003821779127584926, 0.0006595415130723142, 0.0005417380414460182,
    0.0001514202855379314, -0.0002599599960218805, -0.0004674523449839124,
    -0.00039067706686231774, -0.00011562571267458405, 0.00017753214365685534,
    0.00032711791316804143, 0.0002752175342589587, 8.293287276987815e-05,
    -0.00012200819667138938, -0.00022592251161660787, -0.0001892140749589528,
    -5.591339994313159e-05, 8.445485345028888e-05, 0.00015382343214611046,
    0.0001266897566126986, 3.516119014865792e-05, -5.882261391380766e-05,
    -0.00010305785382652313, -8.23259256517254e-05, -2.020966735693997e-05,
    4.109383096698531e-05, 6.774802206807872e-05, 5.164445134066203e-05,
    1.011243902024759e-05, -2.864544664701489e-05, -4.35186442721966e-05,
    -3.101755643623612e-05, -3.7839269665353855e-06, 1.9783874873981698e-05,
    2.7150403912707073e-05, 1.7595914017559827e-05, 1.974286706289562e-07,
    -1.3417697378820272e-05, -1.630166603071477e-05, -9.20316428574309e-06,
    1.5228493712062577e-06, 8.837603697371173e-06, 9.28377897125396e-06,
    4.218564080590036e-06, -2.0692058381135924e-06, -5.571644764662964e-06,
    -4.888425395338478e-06, -1.4625378417921134e-06, 1.9553692439302856e-06,
    3.289836483445551e-06, 2.2535165695854776e-06, 9.771837689115554e-08,
    -1.5356369230611979e-06, -1.7437298613635878e-06, -7.635994906064242e-07,
    4.517269559661921e-07, 1.0254067093515147e-06, 7.246853403621012e-07,
    -1.9675176756583188e-08, -5.502060822492816e-07, -5.004841021815203e-07,
    -2.945762517557698e-08, 3.587152898250355e-07, 2.7455513276584004e-07,
    -1.441227391914612e-07, -2.639659233940806e-07, 2.0378080261933232e-07,
    -4.6780388632012784e-08, 3.448108060408799e-09,
    };
    return kMinimumPhase;
}
void Overdrive::prepare(double rate) noexcept {
    rate_ = std::isfinite(rate) ? std::clamp(rate, 8000., 384000.) : 48000.;
    lane_ = {};
    p_ = target_;
    smooth_ = std::exp(-1. / (.005 * rate_));
    envelope_ = std::exp(-1. / (.02 * rate_));
    dc_ = std::exp(-2 * std::numbers::pi * 20 / rate_);
}
void Overdrive::set(Param id, double value) noexcept {
    if (id < 0 || id >= Count || !std::isfinite(value))
        return;
    constexpr double lo[] = {50, .1, 0, 0, 0, 0}, hi[] = {12000, 6, 100, 100, 100, 100};
    target_[static_cast<std::size_t>(id)] = std::clamp(value, lo[id], hi[id]);
}
void Overdrive::process(const float *l, const float *r, float *ol, float *orr,
                        std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t p = 0; p < p_.size(); ++p)
            p_[p] = target_[p] + smooth_ * (p_[p] - target_[p]);
        const double omega = 2 * std::numbers::pi * std::min(p_[Frequency], rate_ * .45) / rate_;
        const double alpha = std::sin(omega) * std::sinh(std::log(2.) * .5 * p_[Bandwidth]);
        const double b0 = alpha / (1 + alpha), a1 = -2 * std::cos(omega) / (1 + alpha),
                     a2 = (1 - alpha) / (1 + alpha);
        const double tonePole =
            std::exp(-2 * std::numbers::pi *
                     std::min(400. * std::pow(40., p_[Tone] / 100), rate_ * .45) / rate_);
        const float drive = static_cast<float>(2 * std::pow(16., p_[Drive] / 100));
        const double dynamics = p_[Dynamics] / 100, mix = p_[Mix] / 100;
        const float input[] = {l ? l[i] : 0, r ? r[i] : 0};
        float *output[] = {ol, orr};
        for (std::size_t c = 0; c < 2; ++c) {
            auto &s = lane_[c];
            const double dry = std::isfinite(input[c]) ? input[c] : 0;
            const double band = b0 * dry + s.z1;
            s.z1 = -a1 * band + s.z2;
            s.z2 = -b0 * dry - a2 * band;
            // 4x around the shaper: polyphase interpolation of the band signal,
            // the shaper at the 4x rate, decimation at phase 0.
            const auto &h = resamplingFilter();
            s.bandAt = (s.bandAt + kPhases - 1) % kPhases;
            s.band[s.bandAt] = s.band[s.bandAt + kPhases] = band;
            double shaped = 0;
            for (int k = 0; k < kFactor; ++k) {
                double up = 0;
                for (int t = 0; kFactor * t + k < kTaps; ++t)
                    up += h[static_cast<std::size_t>(kFactor * t + k)] *
                          s.band[s.bandAt + static_cast<std::size_t>(t)];
                const double y = byod::ojd(static_cast<float>(up * kFactor), drive);
                s.shapedAt = (s.shapedAt + kTaps - 1) % kTaps;
                s.shaped[s.shapedAt] = s.shaped[s.shapedAt + kTaps] = y;
                if (k == 0)
                    for (std::size_t j = 0; j < static_cast<std::size_t>(kTaps); ++j)
                        shaped += h[j] * s.shaped[s.shapedAt + j];
            }
            s.tone = (1 - tonePole) * shaped + tonePole * s.tone;
            const double wet = s.tone - s.dcIn + dc_ * s.dcOut;
            s.dcIn = s.tone;
            s.dcOut = wet;
            s.inputPower = (1 - envelope_) * band * band + envelope_ * s.inputPower;
            s.outputPower = (1 - envelope_) * wet * wet + envelope_ * s.outputPower;
            const double preserving =
                std::clamp(std::sqrt((s.inputPower + 1e-12) / (s.outputPower + 1e-12)), 0., 4.);
            const double gain = 1 + dynamics * (preserving - 1);
            // The dry path waits the reported 5 samples, so Mix stays coherent.
            s.dry[s.dryAt] = dry;
            const double late = s.dry[(s.dryAt + s.dry.size() - 5) % s.dry.size()];
            s.dryAt = (s.dryAt + 1) % s.dry.size();
            output[c][i] = static_cast<float>(late + mix * (wet * gain - late));
        }
    }
}
} // namespace adi::dsp
