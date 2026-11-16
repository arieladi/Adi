// Copyright (C) 2026 - zsliu98
// This file is part of ZLEqualizer
//
// ZLEqualizer is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License Version 3 as published by the Free Software Foundation.
//
// ZLEqualizer is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License along with ZLEqualizer. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <span>
#include <vector>
#include <algorithm>
#include <type_traits>

#include "tdf_base.hpp"
#include "../iir/iir.hpp"
#include "../coeff/ivantsov_coeff.hpp"
#include "../coeff/dynamic_peak_coeff.hpp"
#include "../coeff/automated_cut_coeff.hpp"

namespace adi_eq_dsp::filter {
    /**
     * an IIR filter which processes audio on the real-time thread
     * @tparam FloatType the float type of input audio buffer
     * @tparam kFilterSize the number of cascading filters
     */
    template <typename FloatType, size_t kFilterSize>
    class TDF final : public IIR<kFilterSize> {
    public:
        TDF() :
            IIR<kFilterSize>() {
        }

        void reset() override {
            std::ranges::fill(s1s_, static_cast<FloatType>(0));
            std::ranges::fill(s2s_, static_cast<FloatType>(0));
        }

        void prepare(const double sample_rate, const size_t num_channels, const size_t) override {
            IIR<kFilterSize>::prepareSampleRate(sample_rate);
            s1s_.assign(num_channels * kFilterSize, static_cast<FloatType>(0));
            s2s_.assign(num_channels * kFilterSize, static_cast<FloatType>(0));
        }

        /**
         * process the incoming audio buffer
         * @param buffer
         * @param num_samples
         */
        template <bool bypass = false>
        void process(std::span<FloatType*> buffer, const size_t num_samples) {
            if (this->current_filter_num_ == 0) {
                return;
            }
            const auto order = (this->c_filter_type_ == kFlatTilt || this->c_filter_type_ == kFlatGain)
                                   ? 0
                                   : this->c_order_;
            if (this->c_freq_.isSmoothing() || this->c_gain_.isSmoothing() || this->c_q_.isSmoothing()) {
                if constexpr (std::is_same_v<FloatType, double>) {
                    if (order == 2 && this->c_filter_type_ == kLowPass && this->current_filter_num_ == 1) {
                        processAutomatedCut<bypass>(buffer, num_samples);
                        return;
                    }
                }
                if (order == 2) {
                    processTDF<2, bypass, true>(buffer, num_samples);
                } else if (order == 1) {
                    processTDF<1, bypass, true>(buffer, num_samples);
                } else {
                    processTDF<0, bypass, true>(buffer, num_samples);
                }
            } else {
                if (order == 2) {
                    processTDF<2, bypass, false>(buffer, num_samples);
                } else if (order == 1) {
                    processTDF<1, bypass, false>(buffer, num_samples);
                } else {
                    processTDF<0, bypass, false>(buffer, num_samples);
                }
            }
        }

        template <int order, bool bypass = false, bool smooth = false>
        void processTDF(std::span<FloatType*> buffer, const size_t num_samples, const size_t sample_offset = 0) {
            for (size_t i = sample_offset; i < sample_offset + num_samples; ++i) {
                if constexpr (smooth) {
                    this->c_freq_.getNext();
                    this->c_gain_.getNext();
                    this->c_q_.getNext();
                    updateCoeffs();
                }
                for (size_t channel = 0; channel < buffer.size(); ++channel) {
                    if constexpr (bypass) {
                        processSample<order>(channel, buffer[channel][i]);
                    } else {
                        buffer[channel][i] = processSample<order>(channel, buffer[channel][i]);
                    }
                }
            }
        }

        template <bool bypass = false>
        bool processDynamicPeak(std::span<FloatType*> buffer, const FloatType* gains, const size_t num_samples) {
            if constexpr (!std::is_same_v<FloatType, double>) {
                return false;
            } else {
                if (this->c_filter_type_ != kPeak || this->c_order_ != 2 || this->current_filter_num_ != 1) {
                    return false;
                }
                DynamicPeakCoefficients::Block coefficients;
                for (size_t offset = 0; offset < num_samples; offset += DynamicPeakCoefficients::capacity) {
                    const auto count = std::min(DynamicPeakCoefficients::capacity, num_samples - offset);
                    DynamicPeakCoefficients::generate({ gains + offset, count }, this->cache_.data(), coefficients);
                    processCoefficientBlock<bypass>(buffer, offset, count, coefficients);
                }
                return true;
            }
        }

    private:
        template <bool bypass = false>
        void processAutomatedCut(std::span<FloatType*> buffer, const size_t num_samples) {
            std::array<double, AutomatedCutCoefficients::capacity> frequencies {}, qualities {};
            AutomatedCutCoefficients::Block coefficients;
            for (size_t offset = 0; offset < num_samples; offset += AutomatedCutCoefficients::capacity) {
                if (!this->isFreqQSmoothing() && !this->c_gain_.isSmoothing()) {
                    processTDF<2, bypass, false>(buffer, num_samples - offset, offset);
                    break;
                }
                const auto count = std::min(AutomatedCutCoefficients::capacity, num_samples - offset);
                for (size_t sample = 0; sample < count; ++sample) {
                    frequencies[sample] = this->c_freq_.getNext();
                    this->c_gain_.getNext();
                    qualities[sample] = this->c_q_.getNext();
                }
                AutomatedCutCoefficients::generate({ frequencies.data(), count }, { qualities.data(), count },
                    this->sample_rate_, coefficients);
                processCoefficientBlock<bypass>(buffer, offset, count, coefficients);
            }
        }

        template <bool bypass>
        void processCoefficientBlock(std::span<FloatType*> buffer, size_t offset, size_t count,
                                     const DynamicPeakCoefficients::Block& coefficients) {
                      size_t channel = 0;
#if HWY_TARGET != HWY_SCALAR
                      namespace vector = hwy::HWY_NAMESPACE;
                      const vector::CappedTag<double, 2> tag;
                      if (vector::Lanes(tag) == 2) {
                          for (; channel + 1 < buffer.size(); channel += 2) {
                              std::array<double, 2> firstStates { s1s_[channel * kFilterSize], s1s_[(channel + 1) * kFilterSize] };
                              std::array<double, 2> secondStates { s2s_[channel * kFilterSize], s2s_[(channel + 1) * kFilterSize] };
                              auto stateOne = vector::LoadU(tag, firstStates.data());
                              auto stateTwo = vector::LoadU(tag, secondStates.data());
                              auto* left = buffer[channel] + offset;
                              auto* right = buffer[channel + 1] + offset;
                              const auto step = [&](auto input, auto denominatorOne, auto denominatorTwo,
                                                    auto numeratorZero, auto numeratorOne, auto numeratorTwo) {
                                  const auto output = vector::Add(vector::Mul(input, numeratorZero), stateOne);
                                  stateOne = vector::Add(vector::Sub(vector::Mul(input, numeratorOne),
                                                                    vector::Mul(output, denominatorOne)), stateTwo);
                                  stateTwo = vector::Sub(vector::Mul(input, numeratorTwo), vector::Mul(output, denominatorTwo));
                                  return output;
                              };
                              size_t sample = 0;
                              for (; sample + 1 < count; sample += 2) {
                                  const auto leftInput = vector::LoadU(tag, left + sample);
                                  const auto rightInput = vector::LoadU(tag, right + sample);
                                  const auto denominatorOne = vector::LoadU(tag, coefficients[0].data() + sample);
                                  const auto denominatorTwo = vector::LoadU(tag, coefficients[1].data() + sample);
                                  const auto numeratorZero = vector::LoadU(tag, coefficients[2].data() + sample);
                                  const auto numeratorOne = vector::LoadU(tag, coefficients[3].data() + sample);
                                  const auto numeratorTwo = vector::LoadU(tag, coefficients[4].data() + sample);
                                  const auto first = step(vector::InterleaveLower(leftInput, rightInput),
                                      vector::Broadcast<0>(denominatorOne), vector::Broadcast<0>(denominatorTwo),
                                      vector::Broadcast<0>(numeratorZero), vector::Broadcast<0>(numeratorOne), vector::Broadcast<0>(numeratorTwo));
                                  const auto second = step(vector::InterleaveUpper(tag, leftInput, rightInput),
                                      vector::Broadcast<1>(denominatorOne), vector::Broadcast<1>(denominatorTwo),
                                      vector::Broadcast<1>(numeratorZero), vector::Broadcast<1>(numeratorOne), vector::Broadcast<1>(numeratorTwo));
                                  if constexpr (!bypass) {
                                      vector::StoreU(vector::InterleaveLower(first, second), tag, left + sample);
                                      vector::StoreU(vector::InterleaveUpper(tag, first, second), tag, right + sample);
                                  }
                              }
                              if (sample < count) {
                                  const std::array<double, 2> input { left[sample], right[sample] };
                                  const auto output = step(vector::LoadU(tag, input.data()),
                                      vector::Set(tag, coefficients[0][sample]), vector::Set(tag, coefficients[1][sample]),
                                      vector::Set(tag, coefficients[2][sample]), vector::Set(tag, coefficients[3][sample]),
                                      vector::Set(tag, coefficients[4][sample]));
                                  if constexpr (!bypass) {
                                      std::array<double, 2> result;
                                      vector::StoreU(output, tag, result.data());
                                      left[sample] = result[0];
                                      right[sample] = result[1];
                                  }
                              }
                              vector::StoreU(stateOne, tag, firstStates.data());
                              vector::StoreU(stateTwo, tag, secondStates.data());
                              s1s_[channel * kFilterSize] = firstStates[0];
                              s1s_[(channel + 1) * kFilterSize] = firstStates[1];
                              s2s_[channel * kFilterSize] = secondStates[0];
                              s2s_[(channel + 1) * kFilterSize] = secondStates[1];
                          }
                      }
#endif
                      for (; channel < buffer.size(); ++channel) {
                        auto stateOne = s1s_[channel * kFilterSize];
                        auto stateTwo = s2s_[channel * kFilterSize];
                        auto* samples = buffer[channel] + offset;
                        for (size_t sample = 0; sample < count; ++sample) {
                            const auto input = samples[sample];
                            const auto output = input * coefficients[2][sample] + stateOne;
                            stateOne = input * coefficients[3][sample] - output * coefficients[0][sample] + stateTwo;
                            stateTwo = input * coefficients[4][sample] - output * coefficients[1][sample];
                            if constexpr (!bypass) {
                                samples[sample] = output;
                            }
                        }
                        s1s_[channel * kFilterSize] = stateOne;
                        s2s_[channel * kFilterSize] = stateTwo;
                    }
                    for (size_t index = 0; index < coefficients.size(); ++index) {
                        this->coeffs_[0][index] = coefficients[index][count - 1];
                    }
        }

    public:
        template <int order>
        FloatType processSample(const size_t channel, FloatType sample) {
            const size_t channel_offset = channel * kFilterSize;
            if constexpr (order == 1) {
                const auto& coeff{IIR<kFilterSize>::coeffs_[0]};
                auto& s1{s1s_[channel_offset]};
                const auto output = sample * coeff[2] + s1;
                s1 = (sample * coeff[3]) - (output * coeff[0]);
                sample = output;
                return sample;
            }
            if constexpr (order == 2) {
                const auto& coeff{IIR<kFilterSize>::coeffs_[0]};
                auto& s1{s1s_[channel_offset]};
                auto& s2{s2s_[channel_offset]};
                const auto output = sample * coeff[2] + s1;
                s1 = (sample * coeff[3]) - (output * coeff[0]) + s2;
                s2 = (sample * coeff[4]) - (output * coeff[1]);
                sample = output;
                return sample;
            }
            for (size_t filter_idx = 0; filter_idx < this->current_filter_num_; ++filter_idx) {
                const auto& coeff{IIR<kFilterSize>::coeffs_[filter_idx]};
                auto& s1{s1s_[channel_offset + filter_idx]};
                auto& s2{s2s_[channel_offset + filter_idx]};
                const auto output = sample * coeff[2] + s1;
                s1 = (sample * coeff[3]) - (output * coeff[0]) + s2;
                s2 = (sample * coeff[4]) - (output * coeff[1]);
                sample = output;
            }
            return sample;
        }

        /**
         * update filter coefficients
         */
        void updateCoeffs() override {
            const auto next_freq = this->c_freq_.getCurrent();
            const auto next_gain = this->c_gain_.getCurrent();
            const auto next_q = this->c_q_.getCurrent();
            this->current_filter_num_ = FilterDesign::updateCoeffs<IvantsovCoeff>(
                this->c_filter_type_, this->c_order_,
                next_freq, this->sample_rate_,
                next_gain, next_q, this->coeffs_);
        }

        void cacheDynPara() {
            cacheDyn(this->c_filter_type_, this->c_order_, this->c_freq_.getTarget(),
                     this->sample_rate_, this->c_q_.getTarget());
        }

        void cacheDyn(const FilterType filterType, const size_t n,
                      const double f, const double fs, const double q0) {
            FilterDesign::updateCache<IvantsovCoeff>(filterType, n, f, fs, q0, this->cache_.data());
        }

        void updateGainLinear(const FloatType g_linear_sqrt) {
            FilterDesign::updateGainLinear<IvantsovCoeff>(this->c_filter_type_, this->c_order_,
                                                          g_linear_sqrt, this->cache_.data(), this->coeffs_);
        }

    private:
        std::vector<FloatType> s1s_{};
        std::vector<FloatType> s2s_{};
    };
}
