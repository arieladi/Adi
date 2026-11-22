// SPDX-License-Identifier: GPL-3.0-or-later
// [adi.compressor~]: Live 12's Compressor core (dsp/live_compressor.hpp) as a
// stereo Pd external. `p <index> <value>` sets a control in its own unit;
// `latency` reports the lookahead in samples on the third outlet.
#include "adi/dsp/live_compressor.hpp"
#include "juce/pd_builtins.hpp"
#include "adi/pd_builtins/float_signals.hpp"
extern "C" {
#include "m_pd.h"
}
#include <cmath>
namespace {
t_class *klass = nullptr;
struct Object {
    t_object object;
    t_float signal;
    adi::dsp::LiveCompressor *core;
    t_outlet *latency;
    adi::device::FloatSignals *signals;
};
void report(Object *x) { outlet_float(x->latency, static_cast<t_float>(x->core->latency())); }
void *create() {
    auto *x = reinterpret_cast<Object *>(pd_new(klass));
    x->signals = new adi::device::FloatSignals;
    x->core = new adi::dsp::LiveCompressor;
    inlet_new(&x->object, &x->object.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    x->latency = outlet_new(&x->object, &s_float);
    return x;
}
void destroy(Object *x) {
    delete x->core;
    delete x->signals;
}
t_int *perform(t_int *w) {
    auto *x = reinterpret_cast<Object *>(w[1]);
    x->core->process(reinterpret_cast<const float *>(w[2]), reinterpret_cast<const float *>(w[3]),
                     reinterpret_cast<float *>(w[4]), reinterpret_cast<float *>(w[5]),
                     static_cast<std::size_t>(w[6]));
    return w + 7;
}
void dsp(Object *x, t_signal **s) {
    x->signals->begin(s, 2, 2);
    x->core->prepare(s[0]->s_sr);
    report(x);
    t_int a[] = {reinterpret_cast<t_int>(x), x->signals->at(0), x->signals->at(1),
                 x->signals->at(2),          x->signals->at(3), static_cast<t_int>(s[0]->s_n)};
    dsp_addv(perform, 6, a);
    x->signals->end();
}
void parameter(Object *x, t_symbol *, int n, t_atom *a) {
    if (n != 2) {
        return;
    }
    const auto id = atom_getfloat(a);
    if (!std::isfinite(id) || id < 0 || id >= static_cast<float>(adi::dsp::LiveCompressor::Count)) {
        return;
    }
    x->core->set(static_cast<adi::dsp::LiveCompressor::Param>(static_cast<int>(id)),
                 static_cast<float>(atom_getfloat(a + 1)));
    report(x);
}
} // namespace
extern "C" void adi_compressor_tilde_setup() {
    klass = class_new(gensym("adi.compressor~"), reinterpret_cast<t_newmethod>(create),
                      reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_NULL);
    CLASS_MAINSIGNALIN(klass, Object, signal);
    class_addmethod(klass, reinterpret_cast<t_method>(report), gensym("latency"), A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(parameter), gensym("p"), A_GIMME, A_NULL);
}
ADI_PD_BUILTIN(adi.compressor ~, adi_compressor_tilde_setup)
