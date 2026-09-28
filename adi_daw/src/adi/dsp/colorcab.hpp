// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace adi::dsp {
// ADR-0192 Size domain. Value object, safe to copy at a block boundary after
// the host has published it. The host owns synchronization, never this core.
struct ColorCabKernel {
    static constexpr std::size_t capacity = 1024;
    std::array<double, capacity> taps{};
    std::size_t size = 1;
};
struct ColorCabOptions {
    std::size_t size = 256;
    double gamma = 0.5;         // magnitude power: 0 flat, 1 original contrast
    double smoothingOctaves = 1.0 / 3; // full log-frequency averaging width
    double pitchSemitones = 0;
};
struct ColorCabDesign {
    ColorCabKernel kernel;
    // Positive-frequency target INCLUDING DC and Nyquist; normalized peak=1,
    // at uniformly spaced frequencies, useful as saved derived device state.
    std::vector<double> target;
    double sampleRate = 48000;
};
// Pure, deterministic, OFF AUDIO THREAD. Mono sample at the processing rate.
// Throws invalid_argument on empty/nonfinite input, invalid rate or options.
// A silent sample designs silence. Finite FIR accuracy depends on target
// smoothness: no fixed tap count can promise 1 dB for arbitrary narrow notches.
[[nodiscard]] ColorCabDesign buildColorCab(std::span<const float> sample,
                                          double sampleRate, ColorCabOptions options = {});
class ColorCab {
public:
    // Call off-thread before processing. Identity kernel on prepare.
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void setMix(double mix) noexcept;
    // Serialized with process. Copies a bounded value, no allocation. False
    // for invalid kernels or while fading: the caller retains/retries newest
    // request, so a rapid replacement can never interrupt a live crossfade.
    [[nodiscard]] bool setKernel(const ColorCabKernel& kernel) noexcept;
    [[nodiscard]] bool fading() const noexcept { return remaining_ != 0; }
    void process(const float* input, float* output, std::size_t frames) noexcept;
private:
    double convolve(const ColorCabKernel& kernel) const noexcept;
    std::array<double, ColorCabKernel::capacity> history_{};
    ColorCabKernel current_{}, next_{};
    std::size_t write_ = 0, fadeSamples_ = 960, remaining_ = 0;
    double mix_ = 1;
};
} // namespace adi::dsp
