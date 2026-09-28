// SPDX-License-Identifier: GPL-3.0-or-later
//
// See masking.hpp for why the bands are auditory and the spreading asymmetric.

#include "adi/dsp/masking.hpp"

#include <algorithm>
#include <cmath>

namespace adi::dsp {
namespace {

/// Below this, energy is treated as silence. -200 dB in energy terms: far
/// under any dither floor, and finite, so a log never sees zero.
constexpr double kFloor = 1e-20;

double toDb(double energy) noexcept {
    return 10.0 * std::log10(energy < kFloor ? kFloor : energy);
}

}  // namespace

double erbRate(double hz) noexcept {
    if (hz < 0.0) hz = 0.0;
    return 21.4 * std::log10(1.0 + 0.00437 * hz);
}

double erbRateToHz(double cam) noexcept {
    return (std::pow(10.0, cam / 21.4) - 1.0) / 0.00437;
}

MaskingBands::MaskingBands(std::int32_t fftBins, double sampleRate, double bandsPerErb) {
    if (fftBins < 2 || sampleRate <= 0.0) return;
    if (!(bandsPerErb > 0.0)) bandsPerErb = 1.0;

    const double nyquist = sampleRate * 0.5;
    // 20 Hz up, not 0: below it there is no hearing to mask, and the ERB scale
    // is compressing so hard down there that the first band would swallow a
    // quarter of the others.
    const double loHz = 20.0;
    const double hiHz = std::min(nyquist, 20000.0);
    if (hiHz <= loHz) return;

    const double loCam = erbRate(loHz);
    const double hiCam = erbRate(hiHz);
    const double step = 1.0 / bandsPerErb;
    const auto n = static_cast<std::int32_t>(std::floor((hiCam - loCam) / step));
    if (n < 1) return;

    centres_.reserve(static_cast<std::size_t>(n));
    centresCam_.reserve(static_cast<std::size_t>(n));
    for (std::int32_t b = 0; b < n; ++b) {
        const double cam = loCam + (static_cast<double>(b) + 0.5) * step;
        centresCam_.push_back(cam);
        centres_.push_back(erbRateToHz(cam));
    }

    // Bin -> band, resolved once. A bin belongs to the band whose ERB-rate
    // interval contains it, so every bin lands in exactly one band and no
    // energy is counted twice or lost between bands.
    binToBand_.assign(static_cast<std::size_t>(fftBins), -1);
    const double binHz = nyquist / static_cast<double>(fftBins - 1);
    for (std::int32_t bin = 0; bin < fftBins; ++bin) {
        const double hz = static_cast<double>(bin) * binHz;
        if (hz < loHz || hz > hiHz) continue;
        const double cam = erbRate(hz);
        auto band = static_cast<std::int32_t>(std::floor((cam - loCam) / step));
        if (band < 0) band = 0;
        if (band >= n) band = n - 1;
        binToBand_[static_cast<std::size_t>(bin)] = band;
    }
}

std::int32_t MaskingBands::bandOf(std::int32_t bin) const noexcept {
    if (bin < 0 || bin >= static_cast<std::int32_t>(binToBand_.size())) return -1;
    return binToBand_[static_cast<std::size_t>(bin)];
}

double MaskingBands::centreHz(std::int32_t band) const noexcept {
    if (band < 0 || band >= count()) return 0.0;
    return centres_[static_cast<std::size_t>(band)];
}

double MaskingBands::centreCam(std::int32_t band) const noexcept {
    if (band < 0 || band >= count()) return 0.0;
    return centresCam_[static_cast<std::size_t>(band)];
}

void MaskingBands::accumulate(std::span<const float> magnitudes,
                              std::vector<double>& out) const {
    out.assign(static_cast<std::size_t>(count()), 0.0);
    const auto bins = static_cast<std::int32_t>(
        std::min(magnitudes.size(), binToBand_.size()));
    for (std::int32_t bin = 0; bin < bins; ++bin) {
        const std::int32_t band = binToBand_[static_cast<std::size_t>(bin)];
        if (band < 0) continue;
        // ENERGY, so magnitudes are squared before they are summed. Summing
        // magnitudes would make two half-amplitude partials in one band read
        // as loud as one full-amplitude partial, which is not what either of
        // them sounds like.
        const double m = static_cast<double>(magnitudes[static_cast<std::size_t>(bin)]);
        out[static_cast<std::size_t>(band)] += m * m;
    }
}

MaskingResult measureMasking(std::span<const double> maskerEnergy,
                             std::span<const double> maskedEnergy,
                             const MaskingBands& bands,
                             MaskingSlopes slopes,
                             double thresholdDb) {
    MaskingResult r;
    const std::int32_t n = bands.count();
    if (n <= 0 || maskerEnergy.size() < static_cast<std::size_t>(n) ||
        maskedEnergy.size() < static_cast<std::size_t>(n)) {
        return r;
    }
    if (!(slopes.upward > 0.0)) slopes.upward = 10.0;
    if (!(slopes.downward > 0.0)) slopes.downward = 27.0;

    // THE SPREAD MASKER, in dB. For each band, the loudest thing the masker
    // puts there once every band's energy has been allowed to reach it, losing
    // `upward` dB per ERB going up and `downward` dB per ERB going down.
    //
    // A MAXIMUM, not a sum: this asks "how loud is the masker here", and two
    // distant partials that each reach a band at -30 dB do not together make
    // -24 dB of masking. Summing would also make the measure depend on the
    // band count, since finer bands would contribute more terms for the same
    // audio.
    std::vector<double> spreadDb(static_cast<std::size_t>(n), -1e9);
    for (std::int32_t src = 0; src < n; ++src) {
        const double srcDb = toDb(maskerEnergy[static_cast<std::size_t>(src)]);
        if (srcDb <= toDb(kFloor)) continue;
        const double srcCam = bands.centreCam(src);
        for (std::int32_t dst = 0; dst < n; ++dst) {
            const double d = bands.centreCam(dst) - srcCam;
            const double loss = d >= 0.0 ? d * slopes.upward : -d * slopes.downward;
            const double reaching = srcDb - loss;
            double& best = spreadDb[static_cast<std::size_t>(dst)];
            if (reaching > best) best = reaching;
        }
    }

    r.excessDb.assign(static_cast<std::size_t>(n), 0.f);
    double total = 0.0, buried = 0.0;
    for (std::int32_t b = 0; b < n; ++b) {
        const double own = maskedEnergy[static_cast<std::size_t>(b)];
        const double ownDb = toDb(own);
        const double mask = spreadDb[static_cast<std::size_t>(b)];

        // Both silent is not masking, it is nothing. Reporting the difference
        // of two floors would put a highlight on an empty part of the
        // spectrum, which is the one place a producer cannot act on it.
        const bool quiet = own < kFloor && mask <= toDb(kFloor);
        const double excess = quiet ? 0.0 : mask - ownDb;
        r.excessDb[static_cast<std::size_t>(b)] = static_cast<float>(excess);

        if (own < kFloor) continue;   // no energy of its own to bury
        total += own;
        if (excess > thresholdDb) buried += own;

        if (r.worstBand < 0 || excess > static_cast<double>(r.worstExcessDb)) {
            r.worstBand = b;
            r.worstExcessDb = static_cast<float>(excess);
        }
    }
    // Weighted by where the masked track's energy IS: a track with nothing in
    // a band cannot be buried there, and counting bands rather than energy
    // would let thirty empty high bands outvote the one that holds the part.
    r.maskedFraction = total > 0.0 ? static_cast<float>(buried / total) : 0.f;
    return r;
}

}  // namespace adi::dsp
