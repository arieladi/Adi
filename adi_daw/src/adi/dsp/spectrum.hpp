// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one definition of a spectrum in ADI -- ADR-0183 d7, ADR-0195 d5.
//
// TWO PATHS COMPUTE THE SAME NUMBERS, which is the whole reason this file
// exists. The analyser's Pd patch publishes a spectrum through `[adi.array]`;
// the big window's overlay re-analyses other tracks on a worker, in C++, from
// their engine taps. If those two carried their own idea of what a decibel is,
// a producer would see one track's curve sit above another's because of the
// window it happened to be measured with. So the window, its coherent gain and
// the meaning of 0 dBFS are defined HERE, once, and the Pd side is generated
// from these same constants (tools/gen_pd_patches.py).
//
// WHAT 0 dBFS MEANS, exactly: a full-scale sine -- amplitude 1.0, any
// frequency, at any FFT size and with any window in this file -- reads 0.00.
// Not a full-scale square, not a DC offset of 1.0. That choice is the one the
// Max for Live build settled on and it is the one every analyser a producer
// has used agrees with (ADR-0183 d7). It is asserted, at several sizes,
// because it is the number everything else is read against.
//
// WHY THE CORRECTION IS COHERENT GAIN AND NOT ENERGY. A window scales a
// sinusoid's peak bin by sum(w)/N -- its coherent gain -- and scales broadband
// noise by a different factor, sqrt(sum(w^2)/N). Correcting by coherent gain
// makes TONES read true, which is what a spectrum analyser is looked at for: a
// producer checks whether a 1 kHz tone is at -12 dBFS, not whether a band of
// noise integrates correctly. The other convention is right for noise floors
// and wrong for everything a mix engineer does.
//
// DC AND NYQUIST ARE CALIBRATED DIFFERENTLY, and it shows. A real sinusoid
// splits its energy between +f and -f, so the correction doubles it back; DC
// and Nyquist have no partner and take half of that. The result is that a
// full-scale DC offset reads 0.00 dBFS -- its amplitude, which is what an
// analyser is asked for -- and NOT +6. The visible consequence is that a Hann
// window's skirt beside DC reads 0.00 too: the skirt is 6 dB down in raw terms
// and bin 1 is calibrated as a sinusoid, and the two cancel. It appears only
// for pure DC, which is not a musical signal, and it is asserted in the tests
// so that nobody later corrects it into a wrong number.
//
// PFFFT, NOT AN FFT OF OUR OWN. bungee -- the warp engine ADR-0188 d2 chose --
// already carries PFFFT, so ADI will link it when warp lands; a second FFT
// would be two of them in one binary. It is pinned in its own right at the
// commit bungee uses (ADR-0024), and nothing outside this file sees its API:
// its real-transform packing is unusual enough that letting it leak would put
// the same three-line unpacking bug in every caller.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace adi::dsp {

enum class Window : std::uint8_t {
    /// The default, and what the Pd side is generated with. Its sidelobes fall
    /// at 18 dB/octave, which is what makes two tones a few bins apart
    /// readable as two.
    Hann,
    /// No window: the raw frame. Only for tests that want the transform's own
    /// behaviour with nothing applied to it.
    Rectangular,
};

/// Fills `out` with the window, `out.size()` points long. Periodic, not
/// symmetric: the point at the end would repeat the point at the start, and an
/// overlapping analysis would then double-count it.
void fillWindow(Window w, std::span<float> out) noexcept;

/// sum(w) / n -- the factor a window scales a sinusoid's peak bin by.
[[nodiscard]] double coherentGain(Window w, std::int32_t n) noexcept;

/// dBFS from a CALIBRATED linear magnitude, floored rather than -inf: a log of
/// zero is not a number a renderer can plot, and every spectrum has silent
/// bins.
[[nodiscard]] float toDbfs(float calibratedMagnitude) noexcept;
inline constexpr float kDbFloor = -200.f;

/// One FFT size's worth of state: the PFFFT setup, the window, and the
/// scratch. Build it once and reuse it; `analyse` allocates nothing.
class Spectrum {
public:
    /// `fftSize` must be a multiple of `2 * pffft_simd_size()^2` -- 32 on
    /// every SIMD build -- AND factor into 2s, 3s and 5s. 1024, 2048 and 4096
    /// are all fine; 1000 fails the multiple and 224 fails the factorisation.
    ///
    /// **Both are refused, and neither crashes.** That matters because PFFFT
    /// itself does not refuse the first kind: it asserts, so a bad size takes
    /// the process down in a debug build and is undefined with NDEBUG. The
    /// multiple is checked before PFFFT is called; `valid()` is false either
    /// way and `analyse` then does nothing rather than pretending.
    explicit Spectrum(std::int32_t fftSize = 2048, Window w = Window::Hann);
    ~Spectrum();
    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;

    [[nodiscard]] bool valid() const noexcept { return setup_ != nullptr; }
    [[nodiscard]] std::int32_t size() const noexcept { return n_; }
    /// N/2 + 1: DC, the N/2 - 1 bins between, and Nyquist.
    [[nodiscard]] std::int32_t bins() const noexcept { return n_ / 2 + 1; }
    [[nodiscard]] Window window() const noexcept { return window_; }
    [[nodiscard]] double binHz(double sampleRate) const noexcept {
        return n_ > 0 ? sampleRate / static_cast<double>(n_) : 0.0;
    }

    /// Windows `frame` (`size()` samples), transforms it, and writes `bins()`
    /// CALIBRATED linear magnitudes to `out` -- 1.0 for a full-scale sine at
    /// that bin. Shorter spans are refused rather than read past; a longer
    /// `frame` has its first `size()` samples taken.
    void analyse(std::span<const float> frame, std::span<float> out) noexcept;

private:
    std::int32_t n_ = 0;
    Window window_ = Window::Hann;
    void* setup_ = nullptr;      ///< PFFFT_Setup*, opaque so the API stays in
    float* in_ = nullptr;        ///< aligned: PFFFT requires 16 bytes
    float* out_ = nullptr;
    std::vector<float> win_;
    float calibration_ = 1.f;    ///< 2 / sum(w): see the header comment
};

}  // namespace adi::dsp
