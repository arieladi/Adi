// Copyright (C) 2026 - zsliu98
// This file is part of ZLEqualizer
//
// ZLEqualizer is free software: you can redistribute it and/or modify it under the terms of the GNU Affero General Public License Version 3 as published by the Free Software Foundation.
//
// ZLEqualizer is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License along with ZLEqualizer. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "zlp_definitions.hpp"

#include "../dsp/filter/empty_filter/empty.hpp"
#include "../dsp/filter/dynamic_filter/dynamic_tdf.hpp"
#include "../dsp/filter/dynamic_filter/dynamic_svf.hpp"
#include "../dsp/filter/dynamic_filter/dynamic_parallel.hpp"
#include "../dsp/filter/gain_compensation/gain_compensation.hpp"

#include "../dsp/filter/fir_filter/match_correction/match_calculator.hpp"
#include "../dsp/filter/fir_filter/mixed_correction/mixed_calculator.hpp"
#include "../dsp/filter/fir_filter/zero_correction/zero_calculator.hpp"

#include "stereo_fir_processor.hpp"

#include "../dsp/analyzer/analyzer_base/analyzer_sender_base.hpp"
// #include "../dsp/eq_match/eq_match_analyzer.hpp"
#include "../dsp/splitter/inplace_ms_splitter.hpp"
#include "../dsp/histogram/histogram.hpp"

#include "../dsp/loudness/lufs_matcher.hpp"
#include "../dsp/gain/gain.hpp"

#include "../dsp/delay/integer_delay.hpp"

#include "../chore/thread/notifier.hpp"
#include "../../../../Source/FractionalSlope.h"

namespace adi_eq_zlp {
    template <typename T, std::size_t N, typename... Args, std::size_t... I>
    constexpr std::array<T, N> make_array_of_impl(std::index_sequence<I...>, Args&&... args) {
        return {{(static_cast<void>(I), T(std::forward<Args>(args)...))...}};
    }

    template <typename T, std::size_t N, typename... Args>
    constexpr std::array<T, N> make_array_of(Args&&... args) {
        return make_array_of_impl<T, N>(std::make_index_sequence<N>{}, std::forward<Args>(args)...);
    }

    class Controller final {
    public:
        static constexpr size_t kFilterSize = 16;

        struct LatencyObserver {
            void* context = nullptr;
            void (*changed)(void*, int) noexcept = nullptr;
        };
        explicit Controller(LatencyObserver observer);
        Controller() : Controller(LatencyObserver{}) {}
        // Prepared FIR plan getters; no coefficient or signal state mutation.
        int preparedCorrectionLatency(FilterStructure structure) const noexcept;
        int preparedDelayLatency(double seconds) const noexcept;

        void prepare(double sample_rate, size_t max_num_samples);

        template <bool bypass = false>
        void process(std::array<double*, 2> main_pointers,
                     std::array<double*, 2> side_pointers,
                     size_t num_samples);

        void setFilterStructure(const FilterStructure filter_structure) {
            filter_structure_.store(filter_structure, std::memory_order::relaxed);
            to_update_.signal();
        }

        void setFilterStatus(const size_t idx, const FilterStatus filter_status) {
            filter_status_[idx].store(filter_status, std::memory_order::relaxed);
            to_update_status_.signal();
            to_update_.signal();
        }

        void setLRMS(const size_t idx, const FilterStereo filter_stereo) {
            lrms_[idx].store(filter_stereo, std::memory_order::relaxed);
            to_update_lrms_.signal();
            to_update_.signal();
        }

        void setDynamicON(const size_t idx, const bool dynamic_on) {
            dynamic_on_[idx].store(dynamic_on, std::memory_order::relaxed);
            to_update_dynamic_.signal();
            to_update_.signal();
        }

        void setSpectralBand(const size_t idx, const bool enabled) {
            if (spectral_band_[idx].exchange(enabled, std::memory_order::relaxed) != enabled) {
                to_update_status_.signal();
                to_update_dynamic_.signal();
                empty_update_flags_[idx].signal();
                to_update_.signal();
            }
        }

        void setFractionalSlope(const size_t idx, const double slope) {
            const auto value = std::clamp(std::isfinite(slope) ? slope : -1.0, -1.0, 96.0);
            if (fractional_slope_[idx].exchange(value, std::memory_order::relaxed) != value) {
                empty_update_flags_[idx].signal();
                to_update_.signal();
            }
        }

        void setAdditionalLatency(const int samples) {
            const auto value = std::max(0, samples);
            if (additional_latency_.exchange(value, std::memory_order::relaxed) != value) {
                publishLatency();
            }
        }

        int getBaseLatency() const {
            return correction_latency_.load(std::memory_order::relaxed)
                + delay_latency_.load(std::memory_order::relaxed);
        }

        void setDynamicBypass(const size_t idx, const bool dynamic_bypass) {
            dynamic_bypass_[idx].store(dynamic_bypass, std::memory_order::relaxed);
            to_update_.signal();
        }

        void setDynamicRelative(const size_t idx, const bool is_relative) {
            dynamic_th_relative_[idx].store(is_relative, std::memory_order::relaxed);
            dynamic_th_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicSwap(const size_t idx, const bool is_swap) {
            dynamic_swap_[idx].store(is_swap, std::memory_order::relaxed);
            to_update_.signal();
        }

        void setDynamicLearn(const size_t idx, const bool is_learn) {
            dynamic_th_learn_[idx].store(is_learn, std::memory_order::relaxed);
            dynamic_th_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicThreshold(const size_t idx, const double threshold) {
            dynamic_threshold_[idx].store(threshold, std::memory_order::relaxed);
            dynamic_th_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicKnee(const size_t idx, const double knee) {
            dynamic_knee_[idx].store(knee, std::memory_order::relaxed);
            dynamic_th_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicAttack(const size_t idx, const double attack) {
            dynamic_attack_[idx].store(attack, std::memory_order::relaxed);
            dynamic_ar_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicRelease(const size_t idx, const double release) {
            dynamic_release_[idx].store(release, std::memory_order::relaxed);
            dynamic_ar_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicSmooth(const size_t idx, const double smooth) {
            dynamic_smooth_[idx].store(smooth, std::memory_order::relaxed);
            dynamic_smooth_update_[idx].signal();
            dynamic_extra_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicRMSLength(const size_t idx, const double rms_length) {
            dynamic_rms_length_[idx].store(rms_length, std::memory_order::relaxed);
            dynamic_rms_length_update_[idx].signal();
            dynamic_extra_update_[idx].signal();
            to_update_.signal();
        }

        void setDynamicRMSMix(const size_t idx, const double rms_mix) {
            dynamic_rms_mix_[idx].store(rms_mix, std::memory_order::relaxed);
            dynamic_rms_mix_update_[idx].signal();
            dynamic_extra_update_[idx].signal();
            to_update_.signal();
        }

        std::array<adi_eq_dsp::filter::Empty, kBandNum>& getEmptyFilters() {
            return emptys_;
        }

        std::array<adi_eq_dsp::filter::Empty, kBandNum>& getSideEmptyFilters() {
            return side_emptys_;
        }

        void setEditorON(const bool editor_on) {
            editor_on_.store(editor_on, std::memory_order::relaxed);
            to_update_ui_.signal();
            to_update_.signal();
        }

        void setMatchBypassON(const bool is_on) {
            match_bypass_on_.store(is_on, std::memory_order::relaxed);
            to_update_ui_.signal();
            to_update_.signal();
        }

        auto& getAnalyzerSender() {
            return analyzer_sender_;
        }

        double getLearnedThreshold(const size_t idx) const {
            return learned_thresholds_[idx].load(std::memory_order::relaxed);
        }

        double getLearnedKnee(const size_t idx) const {
            return learned_knees_[idx].load(std::memory_order::relaxed);
        }

        std::array<adi_eq_chore::thread::Notifier, kBandNum>& getEmptyUpdateFlags() {
            return empty_update_flags_;
        }

        std::array<adi_eq_chore::thread::Notifier, kBandNum>& getSideEmptyUpdateFlags() {
            return side_empty_update_flags_;
        }

        auto& getUpdateFlag() {
            return to_update_;
        }

        double getCurrentGain(const size_t idx) const {
            return current_gains_[idx].load(std::memory_order::relaxed);
        }

        double getSideLoudness(const size_t idx) const {
            return dynamic_side_loudness_display_[idx].load(std::memory_order::relaxed);
        }

        void setSoloWholeIdx(const size_t idx) {
            solo_whole_idx_.store(idx, std::memory_order::relaxed);
            to_update_solo_.signal();
            to_update_.signal();
        }

        bool isSoloActive() const {
            return solo_whole_idx_.load(std::memory_order::relaxed) < 2 * kBandNum;
        }

        void setSoloGain(const double gain) {
            solo_gain_db_.store(gain, std::memory_order::relaxed);
            to_update_solo_gain_.signal();
            to_update_.signal();
        }

        double getSoloGain() const {
            return solo_gain_db_.load(std::memory_order::relaxed);
        }

        void resetSoloGain() {
            solo_gain_db_.store(0., std::memory_order::relaxed);
            to_reset_solo_gain_.signal();
            to_update_solo_gain_.signal();
            to_update_.signal();
        }

        void setMakeupGain(const double gain) {
            makeup_gain_linear_.store(adi_eq_dsp::chore::decibelsToGain(gain), std::memory_order::relaxed);
            to_update_makeup_.signal();
            to_update_output_.signal();
            to_update_.signal();
        }

        void setSGCON(const bool f) {
            sgc_on_.store(f, std::memory_order::relaxed);
            to_update_output_.signal();
            to_update_.signal();
        }

        void setLoudnessMatchON(const bool f) {
            loudness_matcher_on_.store(f, std::memory_order::relaxed);
            to_update_output_.signal();
            to_update_.signal();
        }

        double getLUFSMatcherDiff() const {
            return loudness_matcher_.getDiff();
        }

        void setAGCON(const bool f) {
            agc_on_.store(f, std::memory_order::relaxed);
            to_update_output_.signal();
            to_update_.signal();
        }

        void setPhaseFlipON(const bool f) {
            phase_flip_on_.store(f, std::memory_order::relaxed);
            to_update_output_.signal();
            to_update_.signal();
        }

        void setDelay(const double delay_second) {
            delay_second_.store(delay_second, std::memory_order::relaxed);
            to_update_delay_.signal();
            to_update_output_.signal();
            to_update_.signal();
        }

        double getDisplayedGain() const {
            return displayed_gain_.load(std::memory_order::relaxed);
        }

    private:
        LatencyObserver latency_observer_;
        adi_eq_chore::thread::Notifier to_update_{false};
        // filter structure
        std::atomic<FilterStructure> filter_structure_{};
        FilterStructure c_filter_structure_{adi_eq_zlp::FilterStructure::kMinimum};
        // filter status
        std::array<std::atomic<FilterStatus>, kBandNum> filter_status_{};
        std::array<FilterStatus, kBandNum> c_filter_status_{};
        std::array<std::atomic<bool>, kBandNum> spectral_band_{};
        std::array<bool, kBandNum> c_spectral_band_{};
        std::atomic<int> additional_latency_{0};
        std::array<std::atomic<double>, kBandNum> fractional_slope_{};
        std::array<bool, kBandNum> c_fractional_active_{};
        std::array<ADISharedEqFractionalSlope, kBandNum> fractional_filters_{};
        double fractional_sample_rate_{48000.0};
        bool initializing_fractional_{false};
        std::vector<size_t> not_off_total_{};
        adi_eq_chore::thread::Notifier to_update_status_{false};
        // filter l/r/m/s
        std::array<std::atomic<FilterStereo>, kBandNum> lrms_{};
        std::array<FilterStereo, kBandNum> c_lrms_{};
        adi_eq_chore::thread::Notifier to_update_lrms_{false};
        bool is_lr_on_{false}, is_ms_on_{false};
        // not off indices for stereo/l/r/m/s
        std::array<std::vector<size_t>, 5> not_off_indices_{};
        // empty filters for holding atomic parameters
        std::array<adi_eq_dsp::filter::Empty, kBandNum> emptys_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> empty_update_flags_{};
        std::array<adi_eq_dsp::filter::Empty, kBandNum> side_emptys_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> side_empty_update_flags_{};
        std::array<adi_eq_dsp::filter::FilterParameters, kBandNum> filter_paras_{};
        std::array<adi_eq_dsp::filter::FilterParameters, kBandNum> side_filter_paras_{};
        // dynamic handlers
        std::array<adi_eq_dsp::filter::DynamicSideHandler<double>, kBandNum> dynamic_side_handlers_
            = make_array_of<adi_eq_dsp::filter::DynamicSideHandler<double>, kBandNum>();
        // dynamic TDF filters
        std::array<adi_eq_dsp::filter::DynamicTDF<double, kFilterSize>, kBandNum> tdf_filters_
            = [&]<size_t... Is>(std::index_sequence<Is...>) {
                return std::array{
                    adi_eq_dsp::filter::DynamicTDF<double, kFilterSize>{std::get<Is>(dynamic_side_handlers_)}...
                };
            }(std::make_index_sequence<std::tuple_size_v<decltype(dynamic_side_handlers_)>>());
        // dynamic SVF filters
        std::array<adi_eq_dsp::filter::DynamicSVF<double, kFilterSize>, kBandNum> svf_filters_
            = [&]<size_t... Is>(std::index_sequence<Is...>) {
                return std::array{
                    adi_eq_dsp::filter::DynamicSVF<double, kFilterSize>{std::get<Is>(dynamic_side_handlers_)}...
                };
            }(std::make_index_sequence<std::tuple_size_v<decltype(dynamic_side_handlers_)>>());
        // dynamic parallel filters
        std::array<adi_eq_dsp::filter::DynamicParallel<double, kFilterSize>, kBandNum> parallel_filters_
            = [&]<size_t... Is>(std::index_sequence<Is...>) {
                return std::array{
                    adi_eq_dsp::filter::DynamicParallel<double, kFilterSize>{std::get<Is>(dynamic_side_handlers_)}...
                };
            }(std::make_index_sequence<std::tuple_size_v<decltype(dynamic_side_handlers_)>>());
        // side-buffer
        std::array<std::vector<double>, 2> side_buffers{};
        std::vector<double*> side_copy_pointers_{};
        // side-chain filters
        std::array<adi_eq_dsp::filter::TDF<double, kFilterSize / 2>, kBandNum> side_filters_{};
        // corrections
        bool c_correction_enabled_{false};
        bool force_update_correction_{false};
        // update indices
        std::array<bool, kBandNum> res_update_flags_{};
        // correction on indices for stereo/l/r/m/s (might duplicate)
        std::atomic<int> correction_latency_{0};
        bool to_update_correction_indices_{false};
        std::vector<size_t> correction_on_total_{};
        std::array<std::vector<size_t>, 5> correction_on_indices_{};
        size_t correction_mask_{0};
        // filters for calculating prototype response and biquad response
        std::array<adi_eq_dsp::filter::Ideal<float, kFilterSize>, kBandNum> res_ideals_{};
        std::array<adi_eq_dsp::filter::TDF<float, kFilterSize>, kBandNum> res_tdfs_{};
        // match correction
        std::unique_ptr<adi_eq_dsp::fft::RFFT<float>> match_fft_;
        adi_eq_dsp::filter::MatchCalculator<kBandNum, kFilterSize> match_calculator_;
        StereoFIRProcessor<double> match_stereo_fir_{match_fft_, 9, 2};
        // mixed correction
        std::unique_ptr<adi_eq_dsp::fft::RFFT<float>> mixed_fft_;
        adi_eq_dsp::filter::MixedCalculator<kBandNum, kFilterSize> mixed_calculator_;
        StereoFIRProcessor<double> mixed_stereo_fir_{mixed_fft_, 10, 16};
        // linear phase (zero phase) correction
        std::unique_ptr<adi_eq_dsp::fft::RFFT<float>> zero_fft_;
        adi_eq_dsp::filter::ZeroCalculator<kBandNum, kFilterSize> zero_calculator_;
        StereoFIRProcessor<double> zero_stereo_fir_{zero_fft_, 13, 0};

        // filter dynamic flags
        std::array<std::atomic<bool>, kBandNum> dynamic_on_{};
        std::array<std::atomic<bool>, kBandNum> dynamic_bypass_{};
        std::array<bool, kBandNum> c_dynamic_on_{};
        adi_eq_chore::thread::Notifier to_update_dynamic_{false};
        // filter dynamic swap flags
        std::array<std::atomic<bool>, kBandNum> dynamic_swap_{};
        // dynamic related parameters
        std::array<adi_eq_chore::thread::Notifier, kBandNum> dynamic_th_update_{};
        std::array<std::atomic<bool>, kBandNum> dynamic_th_relative_{};
        std::array<bool, kBandNum> c_dynamic_th_relative_{};
        std::array<std::atomic<bool>, kBandNum> dynamic_th_learn_{};
        std::array<bool, kBandNum> c_dynamic_th_learn_{};
        std::array<std::atomic<double>, kBandNum> dynamic_threshold_{};
        std::array<double, kBandNum> c_dynamic_threshold_{};
        std::array<std::atomic<double>, kBandNum> dynamic_knee_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> dynamic_ar_update_{};
        std::array<std::atomic<double>, kBandNum> dynamic_attack_{};
        std::array<std::atomic<double>, kBandNum> dynamic_release_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> dynamic_extra_update_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> dynamic_smooth_update_{};
        std::array<std::atomic<double>, kBandNum> dynamic_smooth_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> dynamic_rms_length_update_{};
        std::array<std::atomic<double>, kBandNum> dynamic_rms_length_{};
        std::array<adi_eq_chore::thread::Notifier, kBandNum> dynamic_rms_mix_update_{};
        std::array<std::atomic<double>, kBandNum> dynamic_rms_mix_{};
        // histogram for dynamic auto-threshold
        double hist_unit_decay_{1.0}, slow_hist_unit_decay_{1.0};
        std::array<double, 3> hist_percentiles_{0.1, 0.5, 0.9};
        std::array<double, 3> hist_target_temp_{};
        std::array<double, 3> hist_results_{};
        std::array<adi_eq_dsp::histogram::Histogram<double>, kBandNum> histograms_
            = make_array_of<adi_eq_dsp::histogram::Histogram<double>, kBandNum>(-80.0, 0.0, static_cast<size_t>(80));
        std::array<adi_eq_dsp::histogram::Histogram<double>, kBandNum> slow_histograms_
            = make_array_of<adi_eq_dsp::histogram::Histogram<double>, kBandNum>(-80.0, 0.0, static_cast<size_t>(80));
        std::array<std::atomic<double>, kBandNum> learned_thresholds_{};
        std::array<std::atomic<double>, kBandNum> learned_knees_{};
        // array to hold dynamic gains
        std::array<std::atomic<double>, kBandNum> current_gains_{};
        std::array<std::atomic<double>, kBandNum> dynamic_side_loudness_display_{};

        std::array<std::vector<double>, 2> pre_main_buffers_{};
        std::array<double*, 2> pre_main_pointers_{};

        std::atomic<bool> editor_on_{false};
        bool c_editor_on_{false};
        std::atomic<bool> match_bypass_on_{false};
        bool c_match_bypass_on_{false};
        adi_eq_chore::thread::Notifier to_update_ui_{false};

        adi_eq_dsp::analyzer::AnalyzerSenderBase<double, 3> analyzer_sender_{};
        // solo related
        adi_eq_dsp::filter::TDF<double, kFilterSize / 2> solo_filter_;
        std::array<std::vector<double>, 2> solo_buffers_{};
        std::array<double*, 2> solo_pointers_{};
        adi_eq_chore::thread::Notifier to_update_solo_{false};
        std::atomic<size_t> solo_whole_idx_{2 * kBandNum};
        bool c_solo_on_{false};
        bool c_solo_side_{false};
        size_t c_solo_idx_{0};
        adi_eq_chore::thread::Notifier to_reset_solo_gain_{false};
        adi_eq_chore::thread::Notifier to_update_solo_gain_{false};
        std::atomic<double> solo_gain_db_{0.};
        adi_eq_dsp::gain::Gain<double> solo_gain_{};
        // static gain compensation
        adi_eq_chore::thread::Notifier to_update_output_{false};
        std::atomic<bool> sgc_on_{false};
        bool c_sgc_on_{false};
        std::array<double, kBandNum> sgc_values_{};
        double c_sgc_gain_linear_{1.};
        adi_eq_dsp::gain::Gain<double> sgc_gain_{};
        // auto gain compensation
        std::atomic<bool> agc_on_{false};
        bool c_agc_on_{false};
        double pre_square_sum_{1.};
        double c_agc_gain_linear_{1.};
        // loudness matcher
        std::atomic<bool> loudness_matcher_on_{false};
        bool c_loudness_matcher_on_{false};
        adi_eq_dsp::loudness::LUFSMatcher<double> loudness_matcher_{true};
        // makeup gain
        adi_eq_chore::thread::Notifier to_update_makeup_{false};
        std::atomic<double> makeup_gain_linear_{};
        double c_makeup_gain_linear_{};
        adi_eq_dsp::gain::Gain<double> output_gain_{};
        std::atomic<double> displayed_gain_{1.};
        // phase flip
        std::atomic<bool> phase_flip_on_{false};
        bool c_phase_flip_on_{false};
        // lookahead
        std::atomic<double> delay_second_{0.};
        adi_eq_chore::thread::Notifier to_update_delay_{false};
        std::atomic<int> delay_latency_{0};
        bool c_delay_on_{false};
        adi_eq_dsp::delay::IntegerDelay<double> delay_{};

        void prepareBuffer();

        void prepareUIStatus();

        void prepareStatus();

        void prepareDynamics();

        void prepareOneBandDynamics(size_t i);

        void prepareLRMS();

        void prepareFilters();

        adi_eq_dsp::filter::FilterParameters getMainFilterParameters(size_t index) const;

        void prepareCorrectionIndices();

        void prepareCorrection();

        void prepareOutput();

        void prepareDynamicParameters();

        void publishLatency() noexcept;

        template <typename DynamicFilterArrayType, bool should_check_parallel = false, bool should_be_parallel = false>
        void processDynamic(DynamicFilterArrayType& dynamic_filters,
                            std::array<double*, 2> main_pointers,
                            std::array<double*, 2> side_pointers,
                            size_t num_samples);

        template <typename DynamicFilterArrayType, bool should_check_parallel, bool should_be_parallel>
        void processOneChannelDynamic(DynamicFilterArrayType& dynamic_filters,
                                      size_t lrms_idx,
                                      std::span<double*> main_pointers,
                                      std::span<double*> side_pointers1,
                                      std::span<double*> side_pointers2,
                                      size_t num_samples);

        template <bool bypass = false, bool dynamic_on = false, bool dynamic_bypass = false,
                  typename DynamicFilterArrayType>
        void processOneBandDynamic(DynamicFilterArrayType& dynamic_filters,
                                   size_t i,
                                   std::span<double*> main_pointers,
                                   std::span<double*> side_pointers,
                                   size_t num_samples);

        template <bool is_pre>
        void processParallelPrePost(std::span<double*> main_pointers, size_t num_samples);

        template <bool is_pre>
        void processParallelOneBandPrePost(size_t i, std::span<double*> main_pointers, size_t num_samples);

        void processCorrections(StereoFIRProcessor<double>& processor, std::span<double*> main_pointers,
                                size_t num_samples, bool bypass);

        template <bool force>
        void updateSoloFilter(adi_eq_dsp::filter::FilterParameters paras);

        void updateSGC();

        void updateOutputGain();
    };

    extern template void Controller::process<true>(std::array<double*, 2>, std::array<double*, 2>, size_t);

    extern template void Controller::process<false>(std::array<double*, 2>, std::array<double*, 2>, size_t);
}
