// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// DEV-09 (AR17): ADI's DSP cores take float. Built with PD_FLOATSIZE=64 (the
// 64-bit Processing Precision mode, ADR-0207 d2), Pd's signal vectors are
// double, and an external that hands them to a core as float* reads garbage.
// FloatSignals gives an external float views of its signals for one DSP chain:
// the inputs are converted into float scratch before its perform routine and
// the outputs back after it, both added to Pd's DSP chain around the external's
// own. Built with PD_FLOATSIZE=32 a view is the signal itself and nothing is
// added, so the float build pays nothing.
//
// Use, in the dsp method: begin(s, inputs, outputs), then the perform routine's
// dsp_addv with at(k) for each signal, then end(). Scratch is allocated in the
// dsp method (message thread, under Pd's lock), never in perform.
extern "C" {
#include "m_pd.h"
}
#include <cstddef>
#include <type_traits>
#include <vector>

namespace adi::device {

class FloatSignals {
  public:
    static constexpr bool kConverts = !std::is_same_v<t_sample, float>;

    void begin(t_signal** signals, int inputs, int outputs) {
        signals_ = signals;
        inputs_ = inputs;
        outputs_ = outputs;
        if constexpr (kConverts) {
            scratch_.resize(static_cast<std::size_t>(inputs + outputs));
            for (int i = 0; i < inputs + outputs; ++i)
                scratch_[static_cast<std::size_t>(i)].assign(
                    static_cast<std::size_t>(signals[i]->s_n), 0.f);
            for (int i = 0; i < inputs; ++i) add(toFloat, signals[i]->s_vec, i);
        }
    }

    /// The signal as the external's perform routine reads it, for dsp_addv.
    [[nodiscard]] t_int at(int index) const noexcept {
        if constexpr (kConverts)
            return reinterpret_cast<t_int>(scratch_[static_cast<std::size_t>(index)].data());
        else
            return reinterpret_cast<t_int>(signals_[index]->s_vec);
    }

    void end() {
        if constexpr (kConverts)
            for (int o = inputs_; o < inputs_ + outputs_; ++o) add(fromFloat, signals_[o]->s_vec, o);
    }

  private:
    void add(t_perfroutine routine, t_sample* signal, int index) {
        t_int args[] = {reinterpret_cast<t_int>(signal),
                        reinterpret_cast<t_int>(scratch_[static_cast<std::size_t>(index)].data()),
                        static_cast<t_int>(signals_[index]->s_n)};
        dsp_addv(routine, 3, args);
    }
    static t_int* toFloat(t_int* w) {
        const auto* in = reinterpret_cast<const t_sample*>(w[1]);
        auto* out = reinterpret_cast<float*>(w[2]);
        for (t_int i = 0; i < w[3]; ++i) out[i] = static_cast<float>(in[i]);
        return w + 4;
    }
    static t_int* fromFloat(t_int* w) {
        auto* out = reinterpret_cast<t_sample*>(w[1]);
        const auto* in = reinterpret_cast<const float*>(w[2]);
        for (t_int i = 0; i < w[3]; ++i) out[i] = static_cast<t_sample>(in[i]);
        return w + 4;
    }

    t_signal** signals_ = nullptr;
    int inputs_ = 0, outputs_ = 0;
    std::vector<std::vector<float>> scratch_;
};

} // namespace adi::device
