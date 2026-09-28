// SPDX-License-Identifier: GPL-3.0-or-later
// Cepstral factorization: Julius O. Smith, Introduction to Digital Filters,
// https://www.dsprelated.com/freebooks/filters/Conversion_Minimum_Phase.html
#include "adi/dsp/colorcab.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>

namespace adi::dsp {
namespace {
using Complex = std::complex<double>;
// Radix-2 DFT, exp(-i*2*pi*k*n/N), inverse divided by N. Local to the
// off-thread builder: no dependency or hidden audio-thread FFT planning.
void fft(std::vector<Complex>& a, bool inverse) {
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        auto bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const Complex step = std::polar(1.0, (inverse ? 2 : -2) * std::numbers::pi / static_cast<double>(length));
        for (std::size_t i = 0; i < n; i += length) {
            Complex w = 1;
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + length / 2] * w;
                a[i + j] = u + v;
                a[i + j + length / 2] = u - v;
                w *= step;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= static_cast<double>(n);
}
}
ColorCabDesign buildColorCab(std::span<const float> sample, double sampleRate, ColorCabOptions o) {
    if (sample.empty() || !std::isfinite(sampleRate) || sampleRate < 44100 || sampleRate > 768000
        || o.size < 64 || o.size > ColorCabKernel::capacity
        || !std::isfinite(o.gamma) || o.gamma < 0 || o.gamma > 1
        || !std::isfinite(o.smoothingOctaves) || o.smoothingOctaves < 0 || o.smoothingOctaves > 2
        || !std::isfinite(o.pitchSemitones) || std::abs(o.pitchSemitones) > 24)
        throw std::invalid_argument("Color Cab: invalid sample, rate or design options");
    double peak = 0;
    for (float x : sample) {
        if (!std::isfinite(x)) throw std::invalid_argument("Color Cab: nonfinite sample");
        peak = std::max(peak, std::abs(static_cast<double>(x)));
    }
    // Eight times the maximum FIR size separates cepstral circular aliasing
    // from truncation. 8192/48k = 171 ms analysis frames, half-overlapped Hann.
    constexpr std::size_t n = 8 * ColorCabKernel::capacity, half = n / 2;
    ColorCabDesign result;
    result.sampleRate = sampleRate;
    result.kernel.size = o.size;
    result.target.resize(half + 1);
    if (peak == 0) return result;
    std::vector<double> spectrum(half + 1, 0), window(n);
    std::vector<Complex> data(n);
    for (std::size_t j = 0; j < n; ++j)
        window[j] = (1 - std::cos(2 * std::numbers::pi * static_cast<double>(j) / n)) / 2;
    // Center the first window on sample 0, and visit every half-window to
    // include short files and the final tail. Input peak normalization avoids
    // overflow; the final spectral normalization removes file level anyway.
    for (std::size_t center = 0; center < sample.size() + half; center += half) {
        for (std::size_t j = 0; j < n; ++j) {
            const auto index = static_cast<std::ptrdiff_t>(center) + static_cast<std::ptrdiff_t>(j)
                             - static_cast<std::ptrdiff_t>(half);
            data[j] = index >= 0 && static_cast<std::size_t>(index) < sample.size()
                    ? sample[static_cast<std::size_t>(index)] / peak * window[j] : 0;
        }
        fft(data, false);
        for (std::size_t k = 0; k <= half; ++k) spectrum[k] += std::abs(data[k]);
    }
    const double maxBin = *std::max_element(spectrum.begin(), spectrum.end());
    // -80 dB magnitude floor (10^(-80/20)): keeps log finite and makes
    // extreme spectral nulls explicit rather than manufacturing infinities.
    constexpr double floor = 1e-4;
    for (auto& x : spectrum) x = std::pow(std::max(floor, x / maxBin), o.gamma);
    std::vector<double> smooth(half + 1);
    const double band = std::exp2(o.smoothingOctaves / 2);
    for (std::size_t k = 0; k <= half; ++k) {
        if (k == 0 || o.smoothingOctaves == 0) { smooth[k] = spectrum[k]; continue; }
        // Integrate with df/f weights: uniform weighting on log2 frequency,
        // edges at f*2^(+-width/2). Fractional bin overlap avoids stair steps.
        const double lo = static_cast<double>(k) / band, hi = static_cast<double>(k) * band;
        const auto first = static_cast<std::size_t>(std::max(1.0, std::floor(lo)));
        const auto last = std::min(half, static_cast<std::size_t>(std::ceil(hi)));
        double sum = 0, weight = 0;
        for (auto j = first; j <= last; ++j) {
            const double left = std::max(lo, static_cast<double>(j) - 0.5);
            const double right = std::min(hi, static_cast<double>(j) + 0.5);
            if (right <= left) continue;
            const double w = std::log(right / left);
            sum += spectrum[j] * w;
            weight += w;
        }
        smooth[k] = weight > 0 ? sum / weight : spectrum[k];
    }
    const double ratio = std::exp2(o.pitchSemitones / 12);
    for (std::size_t k = 0; k <= half; ++k) {
        const double source = static_cast<double>(k) / ratio;
        // Extend the edge value outside the measured band, never wrap it.
        const auto a = static_cast<std::size_t>(std::min(source, static_cast<double>(half)));
        const auto b = std::min(a + 1, half);
        result.target[k] = smooth[a] + (smooth[b] - smooth[a]) * (source - static_cast<double>(a));
    }
    const double targetPeak = *std::max_element(result.target.begin(), result.target.end());
    for (auto& x : result.target) x /= targetPeak;
    for (std::size_t k = 0; k < n; ++k) data[k] = std::log(result.target[k <= half ? k : n - k]);
    fft(data, true); // real cepstrum of log magnitude
    // Causal cepstrum: retain DC/Nyquist, double positive quefrencies, zero
    // negative ones. exp(DFT(c)) is the minimum-phase spectral factor.
    for (std::size_t k = 1; k < half; ++k) data[k] *= 2;
    for (std::size_t k = half + 1; k < n; ++k) data[k] = 0;
    fft(data, false);
    for (auto& x : data) x = std::exp(x);
    fft(data, true);
    for (std::size_t k = 0; k < o.size; ++k) {
        // Causal half-cosine tail window: first half unchanged, final tap zero.
        // Windowing/truncation approximates the ideal minimum-phase factor;
        // test_colorcab measures both magnitude error and energy concentration.
        const double t = k < o.size / 2 ? 0 : static_cast<double>(k - o.size / 2)
                                                     / static_cast<double>(o.size - 1 - o.size / 2);
        result.kernel.taps[k] = data[k].real() * (1 + std::cos(std::numbers::pi * t)) / 2;
    }
    return result;
}
void ColorCab::prepare(double sampleRate) noexcept {
    const double rate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 44100.0, 768000.0) : 48000;
    // ADR-0192's 20 ms, rounded to the nearest sample.
    fadeSamples_ = static_cast<std::size_t>(std::llround(rate * 0.020));
    current_ = {};
    current_.taps[0] = 1;
    next_ = current_;
    reset();
}
void ColorCab::reset() noexcept {
    history_.fill(0);
    write_ = 0;
    // Reset is called with audio stopped; finish any accepted transition.
    if (remaining_) current_ = next_;
    remaining_ = 0;
}
void ColorCab::setMix(double mix) noexcept {
    mix_ = std::isfinite(mix) ? std::clamp(mix, 0.0, 1.0) : 1;
}
bool ColorCab::setKernel(const ColorCabKernel& kernel) noexcept {
    if (remaining_ || kernel.size == 0 || kernel.size > ColorCabKernel::capacity) return false;
    for (std::size_t i = 0; i < kernel.size; ++i)
        if (!std::isfinite(kernel.taps[i]) || std::abs(kernel.taps[i]) > 1) return false;
    next_ = kernel;
    remaining_ = fadeSamples_;
    return true;
}
double ColorCab::convolve(const ColorCabKernel& kernel) const noexcept {
    double sum = 0;
    auto index = write_;
    for (std::size_t k = 0; k < kernel.size; ++k) {
        sum += kernel.taps[k] * history_[index];
        index = index == 0 ? history_.size() - 1 : index - 1;
    }
    return sum;
}
void ColorCab::process(const float* input, float* output, std::size_t frames) noexcept {
    for (std::size_t n = 0; n < frames; ++n) {
        const double x = std::isfinite(input[n]) ? input[n] : 0;
        history_[write_] = std::abs(x) < 1e-30 ? 0 : x; // -600 dB tail floor
        double wet = convolve(current_);
        if (remaining_) {
            const double alpha = static_cast<double>(fadeSamples_ - remaining_ + 1) / static_cast<double>(fadeSamples_);
            wet += alpha * (convolve(next_) - wet);
            if (--remaining_ == 0) current_ = next_;
        }
        output[n] = static_cast<float>((1 - mix_) * x + mix_ * wet);
        if (++write_ == history_.size()) write_ = 0;
    }
}
} // namespace adi::dsp
