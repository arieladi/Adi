// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/hybrid_reverb.hpp"
#include "adi/pd_builtins/hybrid_reverb_sample.hpp"
#include "adi/pd_builtins/sample_bridge.hpp"
#include "juce/pd_builtins.hpp"
extern "C" {
#include "m_pd.h"
}
#include <cmath>
namespace {
t_class *klass = nullptr;
struct Object {
    t_object object;
    t_float signal;
    adi::dsp::HybridReverb *core;
    t_outlet *latency;
};
void report(Object *x) { outlet_float(x->latency, adi::dsp::HybridReverb::latency); }
void *create() {
    auto *x = reinterpret_cast<Object *>(pd_new(klass));
    x->core = new adi::dsp::HybridReverb;
    inlet_new(&x->object, &x->object.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    x->latency = outlet_new(&x->object, &s_float);
    return x;
}
void destroy(Object *x) { delete x->core; }
t_int *perform(t_int *w) {
    auto *x = reinterpret_cast<Object *>(w[1]);
    const auto *sample =
        dynamic_cast<const adi::device::PdHybridReverbSample *>(adi::device::PdSampleBlock::get(1));
    x->core->process(reinterpret_cast<const float *>(w[2]), reinterpret_cast<const float *>(w[3]),
                     reinterpret_cast<float *>(w[4]), reinterpret_cast<float *>(w[5]),
                     static_cast<std::size_t>(w[6]), sample ? &sample->impulse : nullptr);
    return w + 7;
}
void dsp(Object *x, t_signal **s) {
    x->core->prepare(s[0]->s_sr);
    report(x);
    t_int a[] = {reinterpret_cast<t_int>(x),           reinterpret_cast<t_int>(s[0]->s_vec),
                 reinterpret_cast<t_int>(s[1]->s_vec), reinterpret_cast<t_int>(s[2]->s_vec),
                 reinterpret_cast<t_int>(s[3]->s_vec), static_cast<t_int>(s[0]->s_n)};
    dsp_addv(perform, 6, a);
}
void parameter(Object *x, t_symbol *, int argc, t_atom *argv) {
    if (argc != 2)
        return;
    const auto id = atom_getfloat(argv);
    if (!std::isfinite(id) || id < 0 || id >= static_cast<float>(adi::dsp::HybridReverb::Count))
        return;
    x->core->set(static_cast<adi::dsp::HybridReverb::Param>(static_cast<int>(id)),
                 atom_getfloat(argv + 1));
    report(x);
}
void tempo(Object *x, t_floatarg f) { x->core->tempo(f); }
} // namespace
extern "C" void adi_hybrid_reverb_tilde_setup() {
    klass = class_new(gensym("adi.hybridreverb~"), reinterpret_cast<t_newmethod>(create),
                      reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_NULL);
    CLASS_MAINSIGNALIN(klass, Object, signal);
    class_addmethod(klass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(parameter), gensym("p"), A_GIMME, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(tempo), gensym("tempo"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(report), gensym("latency"), A_NULL);
}
ADI_PD_BUILTIN(adi.hybridreverb ~, adi_hybrid_reverb_tilde_setup)
