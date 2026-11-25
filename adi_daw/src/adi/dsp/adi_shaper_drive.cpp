// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi_shaper_drive.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace adi::dsp {
namespace {
// Each lane is an independent channel. Accumulating across channels keeps
// the tap order, including double precision, identical on both paths.
struct FilterPair {
#if defined(__SSE2__) || defined(_M_X64)
    __m128d v;
    static FilterPair read(const std::array<float, 2>& frame) noexcept {
        const auto packed = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(frame.data()));
        return {_mm_cvtps_pd(_mm_castsi128_ps(packed))};
    }
    std::array<double, 2> result() const noexcept {
        std::array<double, 2> frame;
        _mm_storeu_pd(frame.data(), v);
        return frame;
    }
    FilterPair operator+(FilterPair b) const noexcept { return {_mm_add_pd(v, b.v)}; }
    FilterPair operator-(FilterPair b) const noexcept { return {_mm_sub_pd(v, b.v)}; }
    FilterPair operator*(double k) const noexcept { return {_mm_mul_pd(v, _mm_set1_pd(k))}; }
#else
    std::array<double, 2> v;
    static FilterPair read(const std::array<float, 2>& frame) noexcept { return {{frame[0], frame[1]}}; }
    std::array<double, 2> result() const noexcept { return v; }
    FilterPair operator+(FilterPair b) const noexcept { return {{v[0] + b.v[0], v[1] + b.v[1]}}; }
    FilterPair operator-(FilterPair b) const noexcept { return {{v[0] - b.v[0], v[1] - b.v[1]}}; }
    FilterPair operator*(double k) const noexcept { return {{v[0] * k, v[1] * k}}; }
#endif
};

constexpr std::array<double,33> clipGain{
    1,.78461232,.62661652,.51224817,.43081371,.37351289,.33318401,.30438928,.28330746,
    .26739481,.25499985,.24505308,.23685424,.22993587,.22397934,.21876220,.21412594,
    .20995502,.20616371,.20268703,.19947508,.19648884,.19369730,.19107558,.18860350,
    .18626423,.18404393,.18193085,.17991513,.17798820,.17614264,.17437210,.17267093};
constexpr std::array<double,33> rectGain{
    1,.88494267,.78740162,.70462111,.63451861,.57512017,.52486357,.48241614,.44655615,
    .41626222,.39068521,.36903397,.35063857,.33494174,.32148527,.30987649,.29980891,
    .29100422,.28327260,.27642702,.27033272,.26487023,.25995288,.25549255,.25143078,
    .24771312,.24429436,.24113551,.23820631,.23548045,.23293363,.23054671,.22830192};
constexpr std::array<double,33> powerGain{
    1.000000000, .994110707, .977123053, .950077226, .914723820, .873256774, .827949747, .780772872,
    .733473067, .687447899, .643805400, .603281143, .566191929, .532739893, .502892567, .476462156,
    .453167693, .432745441, .414868582, .399215211, .385495538, .373425750, .362772923, .353347100,
    .344987458, .337515698, .330815593, .324779425, .319319607, .314360289, .309836888, .305694557,
    .301886563};

// The gain knots are fixed. Only the position within a segment changes as
// the contour moves, so build its Hermite coefficients once.
struct GainSegment {
    double value, slope, quadratic, cubic;
};

constexpr auto gainSegments(const std::array<double, 33>& values) noexcept {
    std::array<GainSegment, 32> segments{};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const double left = values[i], right = values[i + 1];
        const double a = i > 0 ? (right - values[i - 1]) * .5 : (4 * right - 3 * left - values[2]) * .5;
        const double b = i < 31 ? (values[i + 2] - left) * .5 : (3 * right - 4 * left + values[30]) * .5;
        const double delta = right - left;
        segments[i] = {left, a, 3 * delta - 2 * a - b, a + b - 2 * delta};
    }
    return segments;
}

constexpr auto clipSegments = gainSegments(clipGain);
constexpr auto rectSegments = gainSegments(rectGain);
constexpr auto powerSegments = gainSegments(powerGain);

double compensation(const std::array<GainSegment, 32>& segments, float amount) noexcept {
    const double pos = 32.0 * amount;
    const auto i = static_cast<std::size_t>(std::min(31, static_cast<int>(pos)));
    const double t = pos - static_cast<double>(i);
    const auto& s = segments[i];
    return s.value + t * (s.slope + t * (s.quadratic + t * s.cubic));
}

double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    for (int i = 1; i <= 20; ++i) {
        term *= x*x / (4.0*i*i);
        sum += term;
    }
    return sum;
}

double softNegative(double t) noexcept {
    if (t > 32.0) return 1.0;
    const double sum = 1.0+t*(1.0+t*(1.0+t*(1.0+t*(1.0+t))));
    return 1.0-1.0/sum;
}

using PowerFrame = std::array<double, 2>;
using PowerPair = std::array<PowerFrame, 2>;

// The four oversampled inputs share one exponent. Fit log2 on a mantissa
// centered at one and exp2 within half a unit of an exact power of two.
PowerPair signedPowers(const PowerPair& z, double exponent) noexcept {
    PowerPair out{};
#if defined(__SSE2__) || defined(_M_X64)
    const auto a = _mm_loadu_pd(z[0].data());
    const auto b = _mm_loadu_pd(z[1].data());
    const auto input = _mm_movelh_ps(_mm_cvtpd_ps(a), _mm_cvtpd_ps(b));
    const auto sign = _mm_set1_ps(-0.0f);
    const auto magnitude = _mm_andnot_ps(sign, input);
    const auto quiet = _mm_cmpeq_ps(magnitude, _mm_setzero_ps());
    const auto one = _mm_set1_ps(1.0f);
    const auto x = _mm_or_ps(magnitude, _mm_and_ps(quiet, one));
    // Restrict the float logarithm to a 48-octave window. Outside it the
    // scalar path retains double precision for the integer part of log2(x).
    const auto range = _mm_and_ps(_mm_cmpge_ps(x, _mm_set1_ps(0x1p-24f)),
                                  _mm_cmple_ps(x, _mm_set1_ps(0x1p+24f)));
    // A nonzero double can round to zero on conversion to float. Preserve
    // those inputs with scalar power, including when denormals are flushed.
    const int quietMask = _mm_movemask_ps(quiet);
    const bool representableZeros = quietMask == 0 || quietMask ==
        (_mm_movemask_pd(_mm_cmpeq_pd(a, _mm_setzero_pd())) |
        (_mm_movemask_pd(_mm_cmpeq_pd(b, _mm_setzero_pd())) << 2));
    if (_mm_movemask_ps(range) == 15 && representableZeros) {
        const auto bits = _mm_castps_si128(x);
        constexpr auto sqrtHalfBits = std::bit_cast<int>(std::numbers::sqrt2_v<float> * .5f);
        const auto relative = _mm_sub_epi32(bits, _mm_set1_epi32(sqrtHalfBits));
        const auto e = _mm_cvtepi32_ps(_mm_srai_epi32(relative, 23));
        const auto mantissa = _mm_sub_epi32(bits, _mm_and_si128(relative, _mm_set1_epi32(~0x7fffff)));
        const auto r = _mm_sub_ps(_mm_castsi128_ps(mantissa), one);
        auto log = _mm_set1_ps(0.1681865911f);
        log = _mm_add_ps(_mm_set1_ps(-0.2679638705f), _mm_mul_ps(r, log));
        log = _mm_add_ps(_mm_set1_ps(0.2961195572f), _mm_mul_ps(r, log));
        log = _mm_add_ps(_mm_set1_ps(-0.3595244555f), _mm_mul_ps(r, log));
        log = _mm_add_ps(_mm_set1_ps(0.4806131255f), _mm_mul_ps(r, log));
        log = _mm_add_ps(_mm_set1_ps(-0.7213601786f), _mm_mul_ps(r, log));
        log = _mm_add_ps(_mm_set1_ps(1.442696523f), _mm_mul_ps(r, log));
        const auto log2x = _mm_add_ps(e, _mm_mul_ps(r, log));
        const auto power = _mm_mul_ps(_mm_set1_ps(static_cast<float>(exponent)), log2x);
        const auto half = _mm_or_ps(_mm_and_ps(power, sign), _mm_set1_ps(.5f));
        const auto nearest = _mm_cvttps_epi32(_mm_add_ps(power, half));
        const auto fraction = _mm_sub_ps(power, _mm_cvtepi32_ps(nearest));
        auto exp = _mm_set1_ps(0.0001545316294f);
        exp = _mm_add_ps(_mm_set1_ps(0.001339086336f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(0.009618082557f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(0.05550357114f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(0.2402265076f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(0.693147188f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(1.f), _mm_mul_ps(fraction, exp));
        const auto scale = _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(nearest, _mm_set1_epi32(127)), 23));
        const auto result = _mm_or_ps(_mm_and_ps(input, sign), _mm_andnot_ps(quiet, _mm_mul_ps(exp, scale)));
        _mm_storeu_pd(out[0].data(), _mm_cvtps_pd(result));
        _mm_storeu_pd(out[1].data(), _mm_cvtps_pd(_mm_movehl_ps(result, result)));
        return out;
    }
#endif
    for (std::size_t i = 0; i < z.size(); ++i)
        for (std::size_t c = 0; c < z[i].size(); ++c)
            out[i][c] = std::copysign(std::pow(std::abs(z[i][c]), exponent), z[i][c]);
    return out;
}

double clipArea(double z) noexcept {
    return std::abs(z) <= 1.0 ? .5*z*z : std::abs(z)-.5;
}
} // namespace

AdiShaperDrive::AdiShaperDrive() noexcept { prepare(48000.0); }

void AdiShaperDrive::prepare(double sampleRate) noexcept {
    if (!(sampleRate > 0 && std::isfinite(sampleRate))) return;
    if (sampleRate_ != sampleRate) {
        sampleRate_ = sampleRate;
        factor_ = sampleRate < 70000.0 ? 4 : 2;
        taps_ = 16*factor_+1;
        const double cutoff = factor_ == 4 ? .44 : .48;
        constexpr double beta = 8.5;
        const double windowScale = 1.0/besselI0(beta);
        double sum = 0;
        for (int i=0; i<taps_; ++i) {
            const double offset = static_cast<double>(i-8*factor_)/factor_;
            const double s = 2.0*std::numbers::pi*cutoff*offset;
            const double t = offset/8;
            const double h = (s == 0 ? 1.0 : std::sin(s)/s)
                           * besselI0(beta*std::sqrt(std::max(0.0,1.0-t*t)))*windowScale;
            decimate_[static_cast<std::size_t>(i)] = static_cast<float>(h);
            sum += h;
        }
        interpolate_ = {};
        const auto factor = static_cast<std::size_t>(factor_), taps = static_cast<std::size_t>(taps_);
        for (std::size_t i=0; i<taps; ++i) {
            decimate_[i] = static_cast<float>(decimate_[i]/sum);
            interpolate_[i%factor][i/factor] = static_cast<float>(factor_)*decimate_[i];
        }
        if (factor_ == 4) {
            for (std::size_t j = 0; j < inputs / 2; ++j) {
                const double a = interpolate_[1][j], b = interpolate_[3][j];
                oddSum_[j] = .5 * (a + b);
                oddDifference_[j] = .5 * (a - b);
            }
        }
        dcRate_ = 1.0-std::exp(-2.0*std::numbers::pi*10.0/sampleRate);
    }
    reset();
}

void AdiShaperDrive::reset() noexcept {
    channels_ = {};
    input_ = {};
    wet_ = {};
    inputHead_ = wetHead_ = 0;
    powerHead_ = driveDelay;
}

void AdiShaperDrive::setMode(Mode mode) noexcept {
    if (mode_ == mode) return;
    mode_ = mode;
    powerHead_ = driveDelay;
    previousDrive_ = -1;
}

void AdiShaperDrive::setPowerSaturation(bool enabled) noexcept {
    if (powerSaturation_ == enabled) return;
    powerSaturation_ = enabled;
    if (mode_ == Mode::Saturate && previousDrive_ >= 0.0f) {
        const float latest = powerHead_ < driveDelay
            ? powerAmounts_[(powerHead_ + driveDelay - 1) % driveDelay] : previousDrive_;
        tune(latest);
    }
    powerHead_ = driveDelay;
}

void AdiShaperDrive::setTrims(float inputNormalized, float outputNormalized) noexcept {
    if (inputNormalized != inputTrim_ && std::isfinite(inputNormalized)) {
        inputTrim_ = std::clamp(inputNormalized,0.0f,1.0f);
        inputGain_ = std::exp2((inputTrim_-0.5)*(inputTrim_ < 0.5f ? 4.0 : 12.0));
        previousDrive_ = -1;
    }
    if (outputNormalized != outputTrim_ && std::isfinite(outputNormalized)) {
        outputTrim_ = std::clamp(outputNormalized,0.0f,1.0f);
        outputGain_ = std::exp2((outputTrim_-0.5)*(outputTrim_ < 0.5f ? 12.0 : 4.0));
        previousDrive_ = -1;
    }
}

void AdiShaperDrive::tune(float amount) noexcept {
    previousDrive_ = amount;
    const bool power = powerSaturation_ && mode_ == Mode::Saturate;
    if (power) {
        const double d = amount;
        powerExponent_ = 1.0 / (1.0 + 6.0 * d * d);
        pre_ = inputGain_;
        post_ = outputGain_ * compensation(powerSegments, amount);
        // The power curve approaches the linear transfer continuously.
        driveFade_ = 0.0;
        return;
    }
    const double a = 1.0+amount, square=a*a, fourth=square*square;
    const bool rect = mode_ == Mode::Rectify;
    pre_ = inputGain_ * (rect ? fourth : fourth*fourth);
    post_ = outputGain_ * compensation(rect ? rectSegments : clipSegments, amount);
    // Bring in the nonlinear transfer below the first nonzero compensation
    // knot. A smoothstep joins both existing paths with zero blend slope.
    constexpr double firstKnot = 1.0 / 32.0;
    const double x = std::min(1.0, static_cast<double>(amount) / firstKnot);
    driveFade_ = x * x * (3.0 - 2.0 * x);
}

double AdiShaperDrive::transfer(double z) const noexcept {
    if (mode_ == Mode::HardClip) return std::clamp(z,-1.0,1.0);
    if (mode_ == Mode::Rectify) return std::clamp(std::abs(z+1.0)-1.0,-1.0,1.0);
    return z < 0 ? -softNegative(-z) : z/(1.0+(2.0/3.0)*z);
}

double AdiShaperDrive::shape(double z, Channel& channel) const noexcept {
    const double previous = channel.lastZ;
    channel.lastZ = z;
    if (previousDrive_ == 0.0f) return z;
    if (mode_ != Mode::HardClip) return transfer(z);
    // Integrate the hard clip along the segment traversed in this substep.
    if (z >= 1 && previous >= 1) return 1;
    if (z <= -1 && previous <= -1) return -1;
    if (std::abs(z) <= 1 && std::abs(previous) <= 1) return .5*(z+previous);
    const double delta = z-previous;
    return std::abs(delta) > 1.0e-7 ? (clipArea(z)-clipArea(previous))/delta
                                    : std::clamp(.5*(z+previous),-1.0,1.0);
}

// The center and half-rate phases are symmetric. In fourfold interpolation
// the two odd phases are reversals of each other, sharing a sum and difference.
template<int Factor>
AdiShaperDrive::Phases AdiShaperDrive::interpolate(const Stereo* history) const noexcept {
    FilterPair center{}, half{}, oddSum{}, oddDifference{};
    for (std::size_t j = 0; j < inputs / 2; ++j) {
        const auto a = FilterPair::read(history[j]);
        const auto b = FilterPair::read(history[inputs - 2 - j]);
        const auto sum = a + b;
        center = center + (a + FilterPair::read(history[inputs - 1 - j])) * interpolate_[0][j];
        half = half + sum * interpolate_[Factor / 2][j];
        if constexpr (Factor == 4) {
            oddSum = oddSum + sum * oddSum_[j];
            oddDifference = oddDifference + (a - b) * oddDifference_[j];
        }
    }
    center = center + FilterPair::read(history[inputs / 2]) * interpolate_[0][inputs / 2];
    Phases out{};
    out[0] = center.result();
    out[Factor / 2] = half.result();
    if constexpr (Factor == 4) {
        out[1] = (oddSum + oddDifference).result();
        out[3] = (oddSum - oddDifference).result();
    }
    return out;
}

template<int Factor, bool Power>
AdiShaperDrive::StereoSum AdiShaperDrive::oversample(const Stereo* inputHistory) noexcept {
    constexpr std::size_t taps = 16 * Factor + 1;
    const auto upsampled = interpolate<Factor>(inputHistory);
    const bool fading = driveFade_ > 0.0 && driveFade_ < 1.0;
    Phases shapedPower{};
    if constexpr (Power) {
        for (std::size_t p = 0; p < Factor; p += 2) {
            const PowerPair phaseInputs{{
                {upsampled[p][0] * pre_, upsampled[p][1] * pre_},
                {upsampled[p + 1][0] * pre_, upsampled[p + 1][1] * pre_}}};
            const auto output = previousDrive_ == 0.f ? phaseInputs : signedPowers(phaseInputs, powerExponent_);
            shapedPower[p] = output[0];
            shapedPower[p + 1] = output[1];
        }
        channels_[0].lastZ = upsampled[Factor - 1][0] * pre_;
        channels_[1].lastZ = upsampled[Factor - 1][1] * pre_;
    }
    FilterPair base{};
    for (std::size_t p=0; p<Factor; ++p) {
        wetHead_ = wetHead_ == 0 ? static_cast<int>(taps) - 1 : wetHead_ - 1;
        const auto wetHead = static_cast<std::size_t>(wetHead_);
        Stereo wet{};
        if constexpr (Power) {
            wet = {static_cast<float>(shapedPower[p][0] * post_), static_cast<float>(shapedPower[p][1] * post_)};
        } else {
            for (std::size_t c = 0; c < 2; ++c) {
                const double input = upsampled[p][c];
                double shaped = shape(input * pre_, channels_[c]) * post_;
                if (fading) {
                    const double zeroDrive = (input * inputGain_) * outputGain_;
                    shaped = zeroDrive + driveFade_ * (shaped - zeroDrive);
                }
                wet[c] = static_cast<float>(shaped);
            }
        }
        wet_[wetHead] = wet_[wetHead+taps] = wet;
        if (p == 0) {
            const Stereo* history = wet_.data() + wetHead_;
            constexpr std::size_t middle = taps / 2;
            for (std::size_t j = 0; j < middle; ++j) {
                const auto pair = FilterPair::read(history[j]) + FilterPair::read(history[taps - 1 - j]);
                base = base + pair * decimate_[j];
            }
            base = base + FilterPair::read(history[middle]) * decimate_[middle];
        }
    }
    return base.result();
}

void AdiShaperDrive::process(float& left, float& right, float driveNormalized) noexcept {
    const bool power = powerSaturation_ && mode_ == Mode::Saturate;
    if (!std::isfinite(driveNormalized))
        driveNormalized = power && powerHead_ < driveDelay
            ? powerAmounts_[(powerHead_ + driveDelay - 1) % driveDelay] : std::max(0.0f, previousDrive_);
    if (power) {
        const float requested = std::clamp(driveNormalized, 0.0f, 1.0f);
        if (powerHead_ == driveDelay) {
            powerAmounts_.fill(requested);
            powerHead_ = 0;
        }
        driveNormalized = powerAmounts_[powerHead_];
        powerAmounts_[powerHead_] = requested;
        powerHead_ = (powerHead_ + 1) % driveDelay;
    }
    if (driveNormalized != previousDrive_) {
        const auto amount = std::clamp(driveNormalized,0.0f,1.0f);
        if (amount != previousDrive_) tune(amount);
    }
    inputHead_ = inputHead_ == 0 ? inputs-1 : inputHead_-1;
    const auto inputHead = static_cast<std::size_t>(inputHead_);
    input_[inputHead] = input_[inputHead+inputs] = {left, right};
    const Stereo* in = input_.data()+inputHead_;
    auto base = power ? (factor_ == 4 ? oversample<4, true>(in) : oversample<2, true>(in))
                        : (factor_ == 4 ? oversample<4, false>(in) : oversample<2, false>(in));
    for (std::size_t c=0; c<channels_.size(); ++c) {
        auto& dc = channels_[c].dc;
        const double high=base[c]-dc;
        dc+=dcRate_*high;
        if (std::abs(dc)<1.0e-28) dc=0;
        base[c]=high*(1.0-0.5*dcRate_);
    }
    left=static_cast<float>(base[0]);
    right=static_cast<float>(base[1]);
}

bool AdiShaperDrive::finishBlock() noexcept {
    bool repaired = false;
    const auto head = static_cast<std::size_t>(wetHead_);
    for (std::size_t ch = 0; ch < channels_.size(); ++ch) {
        auto& state = channels_[ch];
        bool bad = !std::isfinite(state.lastZ) || !std::isfinite(state.dc);
        // The decimator has seen everything except phases after the last
        // base-rate sample. Check those too before moving to the next block.
        for (int phase = 1; phase < factor_; ++phase)
            bad |= !std::isfinite(wet_[head + static_cast<std::size_t>(phase - 1)][ch]);
        if (bad) {
            state = {};
            for (auto& sample : input_) sample[ch] = 0.0f;
            for (auto& sample : wet_) sample[ch] = 0.0f;
            repaired = true;
        }
    }
    return repaired;
}

} // namespace adi::dsp
