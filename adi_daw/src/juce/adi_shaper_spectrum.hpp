// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/dsp/adi_shaper_scope.hpp"
#include "adi/dsp/spectrum.hpp"
#include <array>
#include <cmath>
#include <span>

namespace adi::ui {
// Message-thread display projection of the device input. A wide log column
// retains the strongest FFT bin in its interval. Narrow columns interpolate.
class AdiShaperSpectrum {
  public:
    static constexpr int columns = 150;
    std::span<const float> analyse(const dsp::AdiShaperScope::SpectrumFrame &input) {
        if (!(input.sampleRate > 0.) || !fft_.valid())
            return {};
        if (input.sampleRate != sampleRate_)
            prepare(input.sampleRate);
        fft_.analyse(input.mono, bins_);
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            const auto &cell = cells_[i];
            float peak = 0;
            if (cell.valid) {
                const auto k = cell.index;
                peak = std::lerp(bins_[k], bins_[std::min(k + 1, bins_.size() - 1)], cell.weight);
                for (auto j = cell.first; j < cell.end; ++j)
                    peak = std::max(peak, bins_[j]);
            }
            db_[i] = dsp::toDbfs(peak);
        }
        return db_;
    }

  private:
    struct Cell {
        std::size_t first{}, end{}, index{};
        float weight{};
        bool valid{};
    };
    static double frequency(double index) noexcept { return 20. * std::pow(1000., index / (columns - 1)); }
    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        const auto binHz = fft_.binHz(sampleRate);
        const auto last = static_cast<double>(bins_.size() - 1);
        for (int i = 0; i < columns; ++i) {
            auto &cell = cells_[static_cast<std::size_t>(i)];
            const double centre = frequency(i) / binHz;
            cell.valid = centre <= last;
            if (!cell.valid)
                continue;
            const double first = std::ceil(frequency(i - .5) / binHz);
            const double end = std::ceil(frequency(i + .5) / binHz);
            cell.first = static_cast<std::size_t>(std::min(first, last + 1));
            cell.end = static_cast<std::size_t>(std::min(end, last + 1));
            cell.index = static_cast<std::size_t>(centre);
            cell.weight = static_cast<float>(centre - static_cast<double>(cell.index));
        }
    }
    dsp::Spectrum fft_{dsp::AdiShaperScope::fftSize, dsp::Window::Hann};
    std::array<float, dsp::AdiShaperScope::fftSize / 2 + 1> bins_{};
    std::array<Cell, columns> cells_{};
    std::array<float, columns> db_{};
    double sampleRate_{};
};
} // namespace adi::ui
