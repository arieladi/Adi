// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/echo.hpp"
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
    adi::dsp::Echo *core;
};
void *create() {
    auto *x = reinterpret_cast<Object *>(pd_new(klass));
    x->core = new adi::dsp::Echo;
    inlet_new(&x->object, &x->object.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    return x;
}
void destroy(Object *x) { delete x->core; }
t_int *perform(t_int *w) {
    auto *x = reinterpret_cast<Object *>(w[1]);
    x->core->process(reinterpret_cast<const float *>(w[2]), reinterpret_cast<const float *>(w[3]),
                     reinterpret_cast<float *>(w[4]), reinterpret_cast<float *>(w[5]),
                     static_cast<std::size_t>(w[6]));
    return w + 7;
}
void dsp(Object *x, t_signal **s) {
    x->core->prepare(s[0]->s_sr);
    t_int a[] = {reinterpret_cast<t_int>(x),           reinterpret_cast<t_int>(s[0]->s_vec),
                 reinterpret_cast<t_int>(s[1]->s_vec), reinterpret_cast<t_int>(s[2]->s_vec),
                 reinterpret_cast<t_int>(s[3]->s_vec), static_cast<t_int>(s[0]->s_n)};
    dsp_addv(perform, 6, a);
}
void parameter(Object *x, t_symbol *, int n, t_atom *a) {
    if (n != 2)
        return;
    const auto id = atom_getfloat(a);
    if (!std::isfinite(id) || id < 0 || id >= static_cast<float>(adi::dsp::Echo::Count))
        return;
    x->core->set(static_cast<adi::dsp::Echo::Param>(static_cast<int>(id)), atom_getfloat(a + 1));
}
} // namespace
void tempo(Object *x, t_floatarg v) { x->core->tempo(v); }
extern "C" void adi_echo_tilde_setup() {
    klass = class_new(gensym("adi.echo~"), reinterpret_cast<t_newmethod>(create),
                      reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_NULL);
    CLASS_MAINSIGNALIN(klass, Object, signal);
    class_addmethod(klass, reinterpret_cast<t_method>(tempo), gensym("tempo"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(parameter), gensym("p"), A_GIMME, A_NULL);
}
ADI_PD_BUILTIN(adi.echo ~, adi_echo_tilde_setup)
