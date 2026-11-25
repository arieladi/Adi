// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi_shaper_drive.hpp"
#include "adi_shaper_fold_tables.hpp"

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

// Five-point derivatives keep the rapidly changing compensation near zero smooth.
constexpr double gainSlope(const std::array<double, 65>& v, std::size_t i) noexcept {
    if (i == 0) return (-25*v[0] + 48*v[1] - 36*v[2] + 16*v[3] - 3*v[4]) / 12;
    if (i == 1) return (-3*v[0] - 10*v[1] + 18*v[2] - 6*v[3] + v[4]) / 12;
    if (i == 64) return (25*v[64] - 48*v[63] + 36*v[62] - 16*v[61] + 3*v[60]) / 12;
    if (i == 63) return (3*v[64] + 10*v[63] - 18*v[62] + 6*v[61] - v[60]) / 12;
    return (v[i-2] - 8*v[i-1] + 8*v[i+1] - v[i+2]) / 12;
}

constexpr auto shapingSegments(const std::array<double, 65>& values) noexcept {
    std::array<GainSegment, 64> segments{};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const double a = gainSlope(values, i), b = gainSlope(values, i+1);
        const double delta = values[i+1] - values[i];
        segments[i] = {values[i], a, 3*delta - 2*a - b, a + b - 2*delta};
    }
    return segments;
}

constexpr auto sineSegments = shapingSegments(drive_detail::sineGain);
constexpr auto triangleSegments = shapingSegments(drive_detail::triangleGain);
constexpr auto sawSegments = shapingSegments(drive_detail::sawGain);

constexpr auto roundedSegments = shapingSegments(drive_detail::roundedGain);
constexpr auto powerSegments = shapingSegments(drive_detail::powerGain);

template<std::size_t N>
double compensation(const std::array<GainSegment, N>& segments, float amount) noexcept {
    const double pos = static_cast<double>(N) * amount;
    const auto i = std::min(N-1, static_cast<std::size_t>(pos));
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

double floorMagnitude(double x) noexcept {
    // The ordinary audio range fits in a signed integer. Truncation is floor for magnitudes.
    return x < 2147483647.0 ? static_cast<double>(static_cast<int>(x)) : std::floor(x);
}

double softNegative(double t) noexcept {
    if (t > 32.0) return 1.0;
    const double sum = 1.0+t*(1.0+t*(1.0+t*(1.0+t*(1.0+t))));
    return 1.0-1.0/sum;
}

using PowerFrame = std::array<double, 2>;
using PowerPair = std::array<PowerFrame, 2>;

double fourfoldPower(double input, double exponent) noexcept {
    if (input == 0.0 || !std::isfinite(input)) return input;
    int octave = 0;
    const double m = 2.0 * std::frexp(std::abs(input), &octave);
    const double log = 2.8883353978185062 + m * (-2.5209116522284445 + m *
        (1.481314907244546 + m * (-.4657850221838716 + m * .05966033255383041)));
    const double power = exponent * (static_cast<double>(octave - 1) + (m - 1.0) * log);
    const int floorPower = static_cast<int>(std::floor(power));
    const double f = power - static_cast<double>(floorPower);
    const double exp = .99992521 + f * (.69583320 + f * (.22606810 + f * .07802395));
    return std::copysign(std::ldexp(exp, floorPower), input);
}

// Each stereo pair shares its exponent.
template<int Factor>
PowerPair signedPowers(const PowerPair& z, double firstExponent, double secondExponent) noexcept {
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
    // The exponent is 1/(1+6*d*d), hence stays in [1/7, 1]. Keeping the fourfold
    // input magnitude in [2^-120, 2^120] leaves a normal float scale at both ends.
    // A three-band input reached 5.96044736e-8 at the upper edge of the old low-level fallback.
    // Larger finite magnitudes (up to the double input limit), tiny nonzero values
    // and nonfinite lanes go through the scalar path. Twofold keeps its original limits.
    const auto range = _mm_and_ps(_mm_cmpge_ps(x, _mm_set1_ps(Factor == 4 ? 0x1p-120f : 0x1p-24f)),
                                  _mm_cmple_ps(x, _mm_set1_ps(Factor == 4 ? 0x1p+120f : 0x1p+24f)));
    // A nonzero double can round to zero on conversion to float. Preserve
    // those inputs with scalar power, including when denormals are flushed.
    const int quietMask = _mm_movemask_ps(quiet);
    int fastMask = _mm_movemask_ps(range);
    if (quietMask != 0) {
        const int exactZero = _mm_movemask_pd(_mm_cmpeq_pd(a, _mm_setzero_pd())) |
                              (_mm_movemask_pd(_mm_cmpeq_pd(b, _mm_setzero_pd())) << 2);
        fastMask &= ~(quietMask & ~exactZero);
    }
    // Keep the healthy lanes on their usual numeric path when another lane
    // needs scalar recovery. Give the vector polynomial a safe argument there.
    const auto safe = _mm_or_ps(_mm_and_ps(range, x), _mm_andnot_ps(range, one));
    const auto bits = _mm_castps_si128(safe);
    __m128 powered;
    if constexpr (Factor == 4) {
        // Signed power at fourfold oversampling uses a binary mantissa in [1,2).
        // Input coefficients come from fitting independent alternating signed plateaus.
        // The output cubic uses the fractional part above floor(power), even for negative powers.
        const auto mantissaBits = _mm_or_si128(_mm_and_si128(bits, _mm_set1_epi32(0x7fffff)),
                                              _mm_set1_epi32(0x3f800000));
        const auto mantissa = _mm_castsi128_ps(mantissaBits);
        const auto integerLog = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_srli_epi32(bits, 23), _mm_set1_epi32(127)));
        auto log = _mm_set1_ps(.05966033255383041f);
        log = _mm_add_ps(_mm_set1_ps(-.4657850221838716f), _mm_mul_ps(mantissa, log));
        log = _mm_add_ps(_mm_set1_ps(1.481314907244546f), _mm_mul_ps(mantissa, log));
        log = _mm_add_ps(_mm_set1_ps(-2.5209116522284445f), _mm_mul_ps(mantissa, log));
        log = _mm_add_ps(_mm_set1_ps(2.8883353978185062f), _mm_mul_ps(mantissa, log));
        const auto log2x = _mm_add_ps(integerLog, _mm_mul_ps(_mm_sub_ps(mantissa, one), log));
        const auto power = _mm_mul_ps(_mm_set_ps(static_cast<float>(secondExponent), static_cast<float>(secondExponent),
                                                 static_cast<float>(firstExponent), static_cast<float>(firstExponent)), log2x);
        const auto truncated = _mm_cvttps_epi32(power);
        const auto below = _mm_castps_si128(_mm_cmplt_ps(power, _mm_cvtepi32_ps(truncated)));
        const auto floorPower = _mm_add_epi32(truncated, below);
        const auto fraction = _mm_sub_ps(power, _mm_cvtepi32_ps(floorPower));
        auto exp = _mm_set1_ps(.07802395f);
        exp = _mm_add_ps(_mm_set1_ps(.22606810f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(.69583320f), _mm_mul_ps(fraction, exp));
        exp = _mm_add_ps(_mm_set1_ps(.99992521f), _mm_mul_ps(fraction, exp));
        const auto scale = _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(floorPower, _mm_set1_epi32(127)), 23));
        powered = _mm_mul_ps(exp, scale);
    } else {
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
        const auto power = _mm_mul_ps(_mm_set_ps(static_cast<float>(secondExponent), static_cast<float>(secondExponent),
                                                static_cast<float>(firstExponent), static_cast<float>(firstExponent)), log2x);
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
        powered = _mm_mul_ps(exp, scale);
    }
    const auto result = _mm_or_ps(_mm_and_ps(input, sign), _mm_andnot_ps(quiet, powered));
    _mm_storeu_pd(out[0].data(), _mm_cvtps_pd(result));
    _mm_storeu_pd(out[1].data(), _mm_cvtps_pd(_mm_movehl_ps(result, result)));
    if (fastMask == 15) return out;
#else
    constexpr int fastMask = 0;
#endif
    for (std::size_t i = 0; i < z.size(); ++i) {
        for (std::size_t c = 0; c < z[i].size(); ++c) {
            if ((fastMask & (1 << (2*i+c))) != 0) continue;
            const double exponent = i == 0 ? firstExponent : secondExponent;
            if constexpr (Factor == 4) out[i][c] = fourfoldPower(z[i][c], exponent);
            else out[i][c] = std::copysign(std::pow(std::abs(z[i][c]), exponent), z[i][c]);
        }
    }
    return out;
}

double clipArea(double z) noexcept {
    return std::abs(z) <= 1.0 ? .5*z*z : std::abs(z)-.5;
}
} // namespace

AdiShaperDrive::AdiShaperDrive() noexcept { prepare(48000.0); }

void AdiShaperDrive::Filters::preparePhases(int factor, int taps) noexcept {
    const auto count = static_cast<std::size_t>(factor);
    up = {};
    for (std::size_t i = 0; i < static_cast<std::size_t>(taps); ++i)
        up[i % count][i / count] = static_cast<float>(factor) * down[i];
    if (factor == 4) {
        for (std::size_t j = 0; j < inputs / 2; ++j) {
            const double a = up[1][j], b = up[3][j];
            oddSum[j] = .5 * (a + b);
            oddDifference[j] = .5 * (a - b);
        }
    }
}

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
            legacyFilters_.down[static_cast<std::size_t>(i)] = static_cast<float>(h);
            sum += h;
        }
        const auto taps = static_cast<std::size_t>(taps_);
        for (std::size_t i=0; i<taps; ++i) {
            legacyFilters_.down[i] = static_cast<float>(legacyFilters_.down[i]/sum);
            const auto edge = std::min(i, taps - 1 - i);
            shapingFilters_.down[i] = static_cast<float>(factor_ == 4 ? drive_detail::fourfoldTaps[edge]
                                                                   : drive_detail::twofoldTaps[edge]);
        }
        legacyFilters_.preparePhases(factor_, taps_);
        shapingFilters_.preparePhases(factor_, taps_);
        dcRate_ = 1.0-std::exp(-2.0*std::numbers::pi*10.0/sampleRate);
    }
    reset();
}

void AdiShaperDrive::reset() noexcept {
    channels_ = {};
    input_ = {};
    wet_ = {};
    inputHead_ = wetHead_ = 0;
    strengthHead_ = controlDelay;
}

void AdiShaperDrive::updateResponse() noexcept {
    Response next = Response::Saturate;
    switch (fold_) {
    case Fold::Sine: next = Response::Sine; break;
    case Fold::Triangle: next = Response::Triangle; break;
    case Fold::Saw: next = Response::Saw; break;
    default:
        switch (mode_) {
        case Mode::HardClip: next = roundedClip_ ? Response::Rounded : Response::HardClip; break;
        case Mode::Rectify: next = Response::Rectify; break;
        default: next = powerSaturation_ ? Response::Power : Response::Saturate; break;
        }
    }
    if (response_ != next) {
        response_ = next;
        strengthHead_ = controlDelay;
        previousDrive_ = -1;
    }
}

void AdiShaperDrive::setMode(Mode mode) noexcept {
    if (mode_ == mode) return;
    mode_ = mode;
    updateResponse();
}

void AdiShaperDrive::setFold(Fold fold) noexcept {
    if (fold_ == fold) return;
    fold_ = fold;
    updateResponse();
}

void AdiShaperDrive::setRoundedClip(bool enabled) noexcept {
    if (roundedClip_ == enabled) return;
    roundedClip_ = enabled;
    updateResponse();
}

void AdiShaperDrive::setPowerSaturation(bool enabled) noexcept {
    if (powerSaturation_ == enabled) return;
    powerSaturation_ = enabled;
    updateResponse();
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
    current_.active = amount != 0.0f;
    const double d = amount;
    if (response_ == Response::Rounded) {
        const double octaves = 8.0 * d;
        double gain = 1.0;
        if (current_.active && factor_ == 4) {
            const int whole = static_cast<int>(octaves);
            const double f = octaves - whole;
            const double mantissa = .99992521 + f * (.69583320 + f * (.22606810 + f * .07802395));
            gain = static_cast<double>(1 << whole) * mantissa;
        } else if (current_.active) {
            gain = std::exp2(octaves);
        }
        current_.pre = inputGain_ * gain;
        current_.post = outputGain_ * compensation(roundedSegments, amount);
        current_.knee = .85 + .12 * d;
        const double width = 1.0 - current_.knee;
        current_.kneeSquared = width * width;
        current_.kneeOffset = 1.0 - 2.0 * current_.knee;
        return;
    }
    if (response_ == Response::Power) {
        current_.exponent = 1.0 / (1.0 + 6.0 * d * d);
        current_.pre = inputGain_;
        current_.post = outputGain_ * compensation(powerSegments, amount);
        return;
    }
    if (fold_ != Fold::Off) {
        if (fold_ == Fold::Saw) {
            current_.pre = inputGain_ * (1.0 + (16.0 * std::numbers::sqrt2 - 1.0) * d * d);
            current_.post = outputGain_ * compensation(sawSegments, amount);
        } else {
            const double a = 1.0 + (fold_ == Fold::Sine ? 1.7987877 : 1.5) * d;
            const double square = a * a;
            current_.pre = inputGain_ * (square * square);
            current_.post = outputGain_ * compensation(fold_ == Fold::Sine ? sineSegments : triangleSegments, amount);
        }
        return;
    }
    const double a = 1.0+amount, square=a*a, fourth=square*square;
    const bool rect = mode_ == Mode::Rectify;
    current_.pre = inputGain_ * (rect ? fourth : fourth*fourth);
    current_.post = outputGain_ * compensation(rect ? rectSegments : clipSegments, amount);
    // Bring in the nonlinear transfer below the first nonzero compensation
    // knot. A smoothstep joins both existing paths with zero blend slope.
    constexpr double firstKnot = 1.0 / 32.0;
    const double x = std::min(1.0, static_cast<double>(amount) / firstKnot);
    driveFade_ = x * x * (3.0 - 2.0 * x);
}

template<AdiShaperDrive::Response Style>
double AdiShaperDrive::shape(double z, Channel& channel) const noexcept {
    const double previous = channel.lastZ;
    channel.lastZ = z;
    if (previousDrive_ == 0.0f) return z;
    if constexpr (Style == Response::Rectify) {
        return std::clamp(std::abs(z + 1.0) - 1.0, -1.0, 1.0);
    } else if constexpr (Style == Response::Saturate) {
        return z < 0 ? -softNegative(-z) : z / (1.0 + (2.0 / 3.0) * z);
    } else {
        // Integrate the hard clip along the segment traversed in this substep.
        if (z >= 1 && previous >= 1) return 1;
        if (z <= -1 && previous <= -1) return -1;
        if (std::abs(z) <= 1 && std::abs(previous) <= 1) return .5*(z+previous);
        const double delta = z-previous;
        return std::abs(delta) > 1.0e-7 ? (clipArea(z)-clipArea(previous))/delta
                                        : std::clamp(.5*(z+previous),-1.0,1.0);
    }
}

// The center and half-rate phases are symmetric. In fourfold interpolation
// the two odd phases are reversals of each other, sharing a sum and difference.
template<int Factor>
AdiShaperDrive::Phases AdiShaperDrive::interpolate(const Stereo* history, const Filters& filters) const noexcept {
    // The signed power makes even sub-nanounit endpoint residue audible on isolated impulses.
    // Its interpolator has an exact zero at the outer pair of the base-aligned phase.
    const double outer = response_ == Response::Power ? 0.0 : filters.up[0][0];
    FilterPair center{}, half{}, oddSum{}, oddDifference{};
    for (std::size_t j = 0; j < inputs / 2; ++j) {
        const auto a = FilterPair::read(history[j]);
        const auto b = FilterPair::read(history[inputs - 2 - j]);
        const auto sum = a + b;
        center = center + (a + FilterPair::read(history[inputs - 1 - j])) * (j == 0 ? outer : filters.up[0][j]);
        half = half + sum * filters.up[Factor / 2][j];
        if constexpr (Factor == 4) {
            oddSum = oddSum + sum * filters.oddSum[j];
            oddDifference = oddDifference + (a - b) * filters.oddDifference[j];
        }
    }
    center = center + FilterPair::read(history[inputs / 2]) * filters.up[0][inputs / 2];
    Phases out{};
    out[0] = center.result();
    out[Factor / 2] = half.result();
    if constexpr (Factor == 4) {
        out[1] = (oddSum + oddDifference).result();
        out[3] = (oddSum - oddDifference).result();
    }
    return out;
}

template<int Factor, AdiShaperDrive::Response Style>
void AdiShaperDrive::writePhases(const Phases& phases, std::size_t head) noexcept {
    constexpr std::size_t ringSize = inputs * Factor;
#if defined(__SSE2__) || defined(_M_X64)
    if constexpr (Style == Response::Rounded || Style == Response::Triangle || Style == Response::Saw) {
        const auto sign = _mm_set1_pd(-0.0);
        const auto one = _mm_set1_pd(1.0);
        for (std::size_t p = 0; p < Factor; ++p) {
            const auto& t = p == 0 ? first_ : current_;
            const auto z = _mm_mul_pd(_mm_loadu_pd(phases[p].data()), _mm_set1_pd(t.pre));
            auto value = z;
            if (p == Factor-1) {
                _mm_store_sd(&channels_[0].lastZ, z);
                _mm_storeh_pd(&channels_[1].lastZ, z);
            }
            if (t.active) {
                const auto m = _mm_andnot_pd(sign, z);
                __m128d mag;
                if constexpr (Style == Response::Rounded) {
                    const auto mask = _mm_cmpgt_pd(m, _mm_set1_pd(t.knee));
                    mag = m;
                    if (_mm_movemask_pd(mask) != 0) {
                        const auto clipped = _mm_sub_pd(one, _mm_div_pd(_mm_set1_pd(t.kneeSquared),
                            _mm_add_pd(_mm_max_pd(m, _mm_set1_pd(t.knee)), _mm_set1_pd(t.kneeOffset))));
                        mag = _mm_or_pd(_mm_and_pd(mask, clipped), _mm_andnot_pd(mask, m));
                    }
                } else {
                    const auto units = Style == Response::Triangle ? _mm_mul_pd(m, _mm_set1_pd(.5)) : m;
                    // Conversion truncates nonnegative levels exactly. Keep the scalar reduction
                    // for levels beyond the integer range and for nonfinite inputs.
                    __m128d floor;
                    if (_mm_movemask_pd(_mm_cmplt_pd(units, _mm_set1_pd(2147483647.0))) == 3)
                        floor = _mm_cvtepi32_pd(_mm_cvttpd_epi32(units));
                    else {
                        std::array<double, 2> u;
                        _mm_storeu_pd(u.data(), units);
                        floor = _mm_set_pd(std::floor(u[1]), std::floor(u[0]));
                    }
                    if constexpr (Style == Response::Triangle) {
                        auto phase = _mm_sub_pd(m, _mm_add_pd(floor,floor));
                        mag = _mm_sub_pd(one, _mm_andnot_pd(sign, _mm_sub_pd(one,phase)));
                    } else mag = _mm_sub_pd(m,floor);
                }
                value = _mm_or_pd(_mm_and_pd(sign,z),_mm_andnot_pd(sign,mag));
            }
            const auto y = _mm_cvtpd_ps(_mm_mul_pd(value, _mm_set1_pd(t.post)));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(wet_[head-p].data()), _mm_castps_si128(y));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(wet_[head-p+ringSize].data()), _mm_castps_si128(y));
        }
    } else
#endif
    if constexpr (Style == Response::Power) {
        for (std::size_t p = 0; p < Factor; p += 2) {
            const Transfer& first = p == 0 ? first_ : current_;
            const PowerPair z{{
                {phases[p][0] * first.pre, phases[p][1] * first.pre},
                {phases[p+1][0] * current_.pre, phases[p+1][1] * current_.pre}}};
            const auto wet = first.active || current_.active ? signedPowers<Factor>(z, first.exponent, current_.exponent) : z;
            Stereo a{}, b{};
            for (std::size_t c = 0; c < 2; ++c) {
                a[c] = static_cast<float>((first.active ? wet[0][c] : z[0][c]) * first.post);
                b[c] = static_cast<float>((current_.active ? wet[1][c] : z[1][c]) * current_.post);
                channels_[c].lastZ = z[1][c];
            }
            wet_[head-p] = wet_[head-p+ringSize] = a;
            wet_[head-p-1] = wet_[head-p-1+ringSize] = b;
        }
    } else {
        const bool fading = driveFade_ > 0.0 && driveFade_ < 1.0;
        for (std::size_t p = 0; p < Factor; ++p) {
            Stereo wet{};
            for (std::size_t c = 0; c < 2; ++c) {
                const double input = phases[p][c];
                double shaped;
                if constexpr (Style == Response::Saturate || Style == Response::Rectify || Style == Response::HardClip) {
                    shaped = shape<Style>(input * current_.pre, channels_[c]) * current_.post;
                    if (fading) {
                        const double linear = (input * inputGain_) * outputGain_;
                        shaped = linear + driveFade_ * (shaped - linear);
                    }
                } else {
                    const Transfer& transfer = p == 0 ? first_ : current_;
                    const double z = input * transfer.pre;
                    channels_[c].lastZ = z;
                    double magnitude = std::abs(z);
                    if (transfer.active) {
                        if constexpr (Style == Response::Rounded) {
                            if (magnitude > transfer.knee)
                                magnitude = 1.0 - transfer.kneeSquared / (magnitude + transfer.kneeOffset);
                        } else if constexpr (Style == Response::Sine) {
                            if (magnitude > .5) magnitude = .5 + .5 * std::sin(2*magnitude - 1);
                        } else if constexpr (Style == Response::Triangle) {
                            const double phase = magnitude - 2*floorMagnitude(.5*magnitude);
                            magnitude = 1 - std::abs(1-phase);
                        } else {
                            magnitude -= floorMagnitude(magnitude);
                        }
                    }
                    shaped = std::copysign(magnitude, z) * transfer.post;
                }
                wet[c] = static_cast<float>(shaped);
            }
            wet_[head-p] = wet_[head-p+ringSize] = wet;
        }
    }
}

template<int Factor>
AdiShaperDrive::StereoSum AdiShaperDrive::decimate(const Stereo* history, const Filters& filters) const noexcept {
    constexpr std::size_t taps = 16 * Factor + 1, middle = taps / 2;
    FilterPair base{};
    for (std::size_t j = 0; j < middle; ++j) {
        const auto pair = FilterPair::read(history[j]) + FilterPair::read(history[taps - 1 - j]);
        base = base + pair * filters.down[j];
    }
    return (base + FilterPair::read(history[middle]) * filters.down[middle]).result();
}

template<int Factor>
AdiShaperDrive::StereoSum AdiShaperDrive::render(const Stereo* input) noexcept {
    const auto& filters = usesDelayedControl() ? shapingFilters_ : legacyFilters_;
    const auto phases = interpolate<Factor>(input, filters);
    constexpr int ringSize = inputs * Factor;
    wetHead_ = wetHead_ == 0 ? ringSize - Factor : wetHead_ - Factor;
    const auto head = static_cast<std::size_t>(wetHead_) + Factor - 1;
    switch (response_) {
    case Response::HardClip: writePhases<Factor, Response::HardClip>(phases, head); break;
    case Response::Rectify: writePhases<Factor, Response::Rectify>(phases, head); break;
    case Response::Rounded: writePhases<Factor, Response::Rounded>(phases, head); break;
    case Response::Power: writePhases<Factor, Response::Power>(phases, head); break;
    case Response::Sine: writePhases<Factor, Response::Sine>(phases, head); break;
    case Response::Triangle: writePhases<Factor, Response::Triangle>(phases, head); break;
    case Response::Saw: writePhases<Factor, Response::Saw>(phases, head); break;
    default: writePhases<Factor, Response::Saturate>(phases, head); break;
    }
    return decimate<Factor>(wet_.data() + head, filters);
}

void AdiShaperDrive::process(float& left, float& right, float driveNormalized) noexcept {
    if (usesDelayedControl()) {
        if (!std::isfinite(driveNormalized))
            driveNormalized = strengthHead_ < controlDelay ? strengthHistory_[(strengthHead_ + controlDelay-1) % controlDelay] : 0.0f;
        driveNormalized = std::clamp(driveNormalized, 0.0f, 1.0f);
        if (strengthHead_ == controlDelay) {
            strengthHistory_.fill(driveNormalized);
            strengthHead_ = 0;
            lastDelayed_ = driveNormalized;
        }
        if (previousDrive_ != lastDelayed_) tune(lastDelayed_);
        // The phase on the base-rate grid precedes the remaining phases by one control frame.
        first_ = current_;
        const float delayed = strengthHistory_[strengthHead_];
        strengthHistory_[strengthHead_] = driveNormalized;
        strengthHead_ = (strengthHead_ + 1) % controlDelay;
        driveNormalized = delayed;
        lastDelayed_ = delayed;
    }
    if (!std::isfinite(driveNormalized)) driveNormalized = std::max(0.0f,previousDrive_);
    if (driveNormalized != previousDrive_) tune(std::clamp(driveNormalized,0.0f,1.0f));
    inputHead_ = inputHead_ == 0 ? inputs-1 : inputHead_-1;
    const auto inputHead = static_cast<std::size_t>(inputHead_);
    input_[inputHead] = input_[inputHead+inputs] = {left, right};
    const Stereo* in = input_.data()+inputHead_;
    auto base = factor_ == 4 ? render<4>(in) : render<2>(in);
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
