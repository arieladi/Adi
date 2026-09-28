// SPDX-License-Identifier: GPL-3.0-or-later
//
// Spectral masking between two tracks -- ADR-0195 d5, AI-AGENT §5.
//
// ONE MEASURE, TWO CALLERS, AND THAT IS WHY IT LIVES HERE. The analyser's
// big-window overlay highlights where one track buries another; the agent's
// `analyze.masking` returns "band-overlap between two tracks over time". If
// those were written separately, a producer would see a highlight the agent
// would not mention, or be told about masking the picture does not show -- and
// neither would be wrong, because there would be two definitions of masking in
// one program. So this is a PURE FUNCTION with tests, and the view and the
// agent are both callers. Nothing here knows what a track is, what a colour is,
// or that a window exists.
//
// WHAT MASKING IS, AND THE THREE CHOICES THIS MAKES
//
//   1. **The bands are auditory, not musical.** Masking is a property of the
//      cochlea's filters, whose widths follow the ERB scale -- about 35 Hz at
//      100 Hz and 565 Hz at 5 kHz. Third-octave bands are uniform in log
//      frequency and match neither end: too narrow low down, too wide high up.
//      A third-octave measure would report masking at 5 kHz that a listener
//      hears as two separate sounds, and miss it at 100 Hz where they merge.
//
//   2. **Spreading is ASYMMETRIC, and that asymmetry is the whole point.** A
//      loud sound masks frequencies ABOVE it far more than below -- the upward
//      spread of masking. A bass guitar buries a kick's low mids; the kick does
//      not equally bury the bass's top. A symmetric measure -- anything built
//      on "how much do these two spectra overlap" -- gets the direction of
//      every real masking problem wrong half the time. The slopes here are the
//      shape every psychoacoustic model uses: steep downward, shallow upward.
//
//   3. **The answer is per band AND one number.** The overlay needs the bands,
//      to highlight where; the agent needs something it can put in a sentence.
//      `worstExcessDb` alone would call a 2 dB problem across the whole
//      spectrum quieter than a 20 dB notch nobody hears, so the single number
//      is `maskedFraction`: the share of the masked track's own energy that
//      sits under the masker. It is 0 when nothing is buried and 1 when all of
//      it is, and it is weighted by where that track's energy actually is.
//
// WHAT THIS IS NOT. It is not a masking THRESHOLD model: it does not claim a
// band is inaudible, only that one track dominates another there by so many
// dB. Audibility needs absolute level, the listener's system and a calibrated
// scale, and a DAW has none of the three. ADR-0195 leaves the highlight's
// threshold open "once there is something to look at", and this reports the
// number that threshold will be applied to.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace adi::dsp {

/// The ERB-spaced band layout a pair of spectra is measured in.
///
/// Built once for a given FFT size and sample rate, then reused: the band
/// edges do not change while the analyser runs, and recomputing logarithms per
/// frame on a worker that has 60 of them a second is waste with no upside.
class MaskingBands {
public:
    /// `fftBins` is the number of MAGNITUDE bins -- `fftSize / 2 + 1` -- and
    /// `sampleRate` what they were taken at. `bandsPerErb` of 1 gives roughly
    /// one band per auditory filter; more gives a finer picture of the same
    /// shape.
    MaskingBands(std::int32_t fftBins, double sampleRate, double bandsPerErb = 1.0);

    [[nodiscard]] std::int32_t count() const noexcept {
        return static_cast<std::int32_t>(centres_.size());
    }
    /// The band a bin falls in, or -1 for bins outside the analysed range.
    [[nodiscard]] std::int32_t bandOf(std::int32_t bin) const noexcept;
    [[nodiscard]] double centreHz(std::int32_t band) const noexcept;
    /// Band centres in ERB-rate (Cam) units -- the scale the spreading slopes
    /// below are quoted per, which is why they are kept rather than recomputed.
    [[nodiscard]] double centreCam(std::int32_t band) const noexcept;

    /// Sums `magnitudes` (linear, not dB) into per-band ENERGY.
    /// `out` is resized to `count()`.
    void accumulate(std::span<const float> magnitudes, std::vector<double>& out) const;

private:
    std::vector<double> centres_, centresCam_;
    std::vector<std::int32_t> binToBand_;
};

/// How one spectrum masks another.
struct MaskingResult {
    /// Per band, in dB: how far the masker's spread energy is above the
    /// masked track's own. Positive means the masker dominates there.
    /// Silence on both sides gives 0 rather than a hole.
    std::vector<float> excessDb;
    /// The band with the largest excess, and its value. -1 and 0 when there is
    /// nothing to measure.
    std::int32_t worstBand = -1;
    float worstExcessDb = 0.f;
    /// The share of the MASKED track's energy sitting in bands where the
    /// excess is above the threshold: 0 when none of it is buried, 1 when all
    /// of it is. This is the number a sentence can carry.
    float maskedFraction = 0.f;
};

/// Spreading slopes, in dB per ERB. Both are attenuations, so both positive.
struct MaskingSlopes {
    /// Toward HIGHER frequencies. Shallow: this is the upward spread of
    /// masking, the direction in which a loud sound reaches furthest.
    double upward = 10.0;
    /// Toward LOWER frequencies. Steep: a sound masks very little below
    /// itself.
    double downward = 27.0;
};

/// Measures how `maskerEnergy` masks `maskedEnergy`, both per band from
/// `MaskingBands::accumulate` and both linear energy rather than dB.
///
/// `thresholdDb` is how far the masker must exceed the masked track before a
/// band counts toward `maskedFraction`. ADR-0195 leaves the view's threshold
/// open; 0 dB -- "the masker is simply louder here" -- is the neutral default
/// and the one the tests pin.
[[nodiscard]] MaskingResult measureMasking(std::span<const double> maskerEnergy,
                                           std::span<const double> maskedEnergy,
                                           const MaskingBands& bands,
                                           MaskingSlopes slopes = {},
                                           double thresholdDb = 0.0);

/// The ERB-rate (Cam) of a frequency: `21.4 * log10(1 + 0.00437 f)`.
/// Exposed because the band layout and any caller drawing an ERB axis must
/// agree, and two copies of a constant are one copy too many.
[[nodiscard]] double erbRate(double hz) noexcept;
/// Its inverse.
[[nodiscard]] double erbRateToHz(double cam) noexcept;

}  // namespace adi::dsp
