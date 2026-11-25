// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi_shaper_drive.hpp"

#include <algorithm>
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
}

void AdiShaperDrive::setMode(Mode mode) noexcept {
    if (mode_ == mode) return;
    mode_ = mode;
    previousDrive_ = -1;
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

template<AdiShaperDrive::Mode Style>
double AdiShaperDrive::shape(double z, Channel& channel) const noexcept {
    const double previous = channel.lastZ;
    channel.lastZ = z;
    if (previousDrive_ == 0.0f) return z;
    if constexpr (Style == Mode::Rectify) {
        return std::clamp(std::abs(z + 1.0) - 1.0, -1.0, 1.0);
    } else if constexpr (Style == Mode::Saturate) {
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

template<int Factor>
AdiShaperDrive::StereoSum AdiShaperDrive::oversample(const Stereo* inputHistory) noexcept {
    constexpr std::size_t taps = 16 * Factor + 1;
    const auto upsampled = interpolate<Factor>(inputHistory);
    const bool fading = driveFade_ > 0.0 && driveFade_ < 1.0;
    // Advance one complete set of phases. The mirrored ring has a whole
    // number of sets, so a set never crosses its end.
    constexpr int ringSize = inputs * Factor;
    wetHead_ = wetHead_ == 0 ? ringSize - Factor : wetHead_ - Factor;
    const auto firstPhase = static_cast<std::size_t>(wetHead_) + Factor - 1;
    // Select the transfer once for the whole set of oversampled phases.
    const auto writePhases = [&]<Mode Style>() noexcept {
        for (std::size_t p = 0; p < Factor; ++p) {
            Stereo wet{};
            for (std::size_t c=0; c<2; ++c) {
                const double input = upsampled[p][c];
                double shaped = shape<Style>(input * pre_, channels_[c]) * post_;
                if (fading) {
                    const double zeroDrive = (input * inputGain_) * outputGain_;
                    shaped = zeroDrive + driveFade_ * (shaped - zeroDrive);
                }
                wet[c] = static_cast<float>(shaped);
            }
            wet_[firstPhase - p] = wet_[firstPhase - p + ringSize] = wet;
        }
    };
    switch (mode_) {
    case Mode::HardClip: writePhases.template operator()<Mode::HardClip>(); break;
    case Mode::Rectify: writePhases.template operator()<Mode::Rectify>(); break;
    default: writePhases.template operator()<Mode::Saturate>(); break;
    }

    const Stereo* history = wet_.data() + firstPhase;
    constexpr std::size_t middle = taps / 2;
    FilterPair base{};
    for (std::size_t j = 0; j < middle; ++j) {
        const auto pair = FilterPair::read(history[j]) + FilterPair::read(history[taps - 1 - j]);
        base = base + pair * decimate_[j];
    }
    base = base + FilterPair::read(history[middle]) * decimate_[middle];
    return base.result();
}

void AdiShaperDrive::process(float& left, float& right, float driveNormalized) noexcept {
    if (!std::isfinite(driveNormalized)) driveNormalized = std::max(0.0f,previousDrive_);
    if (driveNormalized != previousDrive_) tune(std::clamp(driveNormalized,0.0f,1.0f));
    inputHead_ = inputHead_ == 0 ? inputs-1 : inputHead_-1;
    const auto inputHead = static_cast<std::size_t>(inputHead_);
    input_[inputHead] = input_[inputHead+inputs] = {left, right};
    const Stereo* in = input_.data()+inputHead_;
    auto base = factor_ == 4 ? oversample<4>(in) : oversample<2>(in);
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
