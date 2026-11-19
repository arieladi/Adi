// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi_shaper_drive.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adi::dsp {
namespace {
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

double compensation(const std::array<double,33>& values, float amount) noexcept {
    const double pos = 32.0 * amount;
    const int i = std::min(31, static_cast<int>(pos));
    const double t = pos - i, left = values[i], right = values[i+1];
    const double a = i > 0 ? (right-values[i-1])*0.5 : (4*right-3*left-values[2])*0.5;
    const double b = i < 31 ? (values[i+2]-left)*0.5 : (3*right-4*left+values[30])*0.5;
    const double delta = right - left;
    return left + t * (a + t * (3*delta-2*a-b + t*(a+b-2*delta)));
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
            decimate_[i] = static_cast<float>(h);
            sum += h;
        }
        interpolate_ = {};
        for (int i=0; i<taps_; ++i) {
            decimate_[i] = static_cast<float>(decimate_[i]/sum);
            interpolate_[i%factor_][i/factor_] = static_cast<float>(factor_)*decimate_[i];
        }
        if (factor_ == 4) {
            for (int j = 0; j < inputs / 2; ++j) {
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
    post_ = outputGain_ * compensation(rect ? rectGain : clipGain,amount);
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
    Phases out{};
    for (int j = 0; j < inputs / 2; ++j) {
        for (std::size_t c = 0; c < 2; ++c) {
            const double a = history[j][c], b = history[inputs - 2 - j][c];
            out[0][c] += interpolate_[0][j] * (a + history[inputs - 1 - j][c]);
            out[Factor / 2][c] += interpolate_[Factor / 2][j] * (a + b);
            if constexpr (Factor == 4) {
                out[1][c] += oddSum_[j] * (a + b);
                out[3][c] += oddDifference_[j] * (a - b);
            }
        }
    }
    for (std::size_t c = 0; c < 2; ++c) {
        out[0][c] += static_cast<double>(interpolate_[0][inputs / 2]) * history[inputs / 2][c];
        if constexpr (Factor == 4) {
            const double sum = out[1][c], difference = out[3][c];
            out[1][c] = sum + difference;
            out[3][c] = sum - difference;
        }
    }
    return out;
}

void AdiShaperDrive::process(float& left, float& right, float driveNormalized) noexcept {
    if (!std::isfinite(driveNormalized)) driveNormalized = std::max(0.0f,previousDrive_);
    if (driveNormalized != previousDrive_) tune(std::clamp(driveNormalized,0.0f,1.0f));
    inputHead_ = inputHead_ == 0 ? inputs-1 : inputHead_-1;
    input_[inputHead_] = input_[inputHead_+inputs] = {left, right};
    const Stereo* in = input_.data()+inputHead_;
    const auto upsampled = factor_ == 4 ? interpolate<4>(in) : interpolate<2>(in);
    StereoSum base{};
    for (int p=0; p<factor_; ++p) {
        wetHead_ = wetHead_ == 0 ? taps_-1 : wetHead_-1;
        Stereo wet{};
        for (std::size_t c=0; c<2; ++c)
            wet[c] = static_cast<float>(shape(upsampled[p][c] * pre_, channels_[c]) * post_);
        wet_[wetHead_] = wet_[wetHead_+taps_] = wet;
        if (p == 0) {
            const Stereo* history = wet_.data() + wetHead_;
            const int middle = taps_ / 2;
            for (int j = 0; j < middle; ++j) {
                const double coefficient = decimate_[j];
                for (std::size_t c = 0; c < 2; ++c) {
                    const double pair = static_cast<double>(history[j][c]) + history[taps_ - 1 - j][c];
                    base[c] += coefficient * pair;
                }
            }
            for (std::size_t c = 0; c < 2; ++c)
                base[c] += static_cast<double>(decimate_[middle]) * history[middle][c];
        }
    }
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

} // namespace adi::dsp
