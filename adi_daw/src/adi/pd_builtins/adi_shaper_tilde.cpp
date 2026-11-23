// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/pd_builtins/adi_shaper_transport.hpp"
#include "adi/pd_builtins/float_signals.hpp"
#include "juce/pd_builtins.hpp"
#include <cmath>
#include <new>

namespace {
using Core = adi::dsp::AdiShaper;
t_class* shaperClass = nullptr;
struct State {
    Core core;
    adi::device::FloatSignals signals;
};
struct Object {
    t_object object;
    t_float signal;
    State* state;
    t_outlet* latency;
};

void* create() {
    auto* x = reinterpret_cast<Object*>(pd_new(shaperClass));
    if (!x) return nullptr;
    x->state = new (std::nothrow) State;
    if (!x->state) {
        pd_free(&x->object.ob_pd);
        return nullptr;
    }
    inlet_new(&x->object, &x->object.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    x->latency = outlet_new(&x->object, &s_float);
    return x;
}
void destroy(Object* x) { delete x->state; }
void reportLatency(Object* x) { outlet_float(x->latency, static_cast<t_float>(x->state->core.latency())); }

t_int* perform(t_int* args) {
    auto* x = reinterpret_cast<Object*>(args[1]);
    x->state->core.process(reinterpret_cast<const float*>(args[2]), reinterpret_cast<const float*>(args[3]),
                           reinterpret_cast<float*>(args[4]), reinterpret_cast<float*>(args[5]),
                           static_cast<std::size_t>(args[6]));
    return args + 7;
}
void dsp(Object* x, t_signal** signals) {
    x->state->core.prepare(signals[0]->s_sr);
    reportLatency(x);
    auto& views = x->state->signals;
    try {
        views.begin(signals, 2, 2);
    } catch (const std::bad_alloc&) {
        pd_error(x, "adi.shaper~: insufficient memory for signal buffers");
        dsp_add_zero(signals[2]->s_vec, signals[2]->s_n);
        dsp_add_zero(signals[3]->s_vec, signals[3]->s_n);
        return;
    }
    t_int args[]{reinterpret_cast<t_int>(x), views.at(0), views.at(1), views.at(2), views.at(3),
                 static_cast<t_int>(signals[0]->s_n)};
    dsp_addv(perform, 6, args);
    views.end();
}
void parameter(Object* x, t_symbol*, int n, t_atom* atoms) {
    if (n != 2 || atoms[0].a_type != A_FLOAT || atoms[1].a_type != A_FLOAT) return;
    const auto id = atom_getfloat(atoms);
    if (!std::isfinite(id) || id < 0 || id >= static_cast<t_float>(Core::Count) || std::trunc(id) != id) return;
    auto& core = x->state->core;
    const int previousLatency = core.latency();
    core.set(static_cast<Core::Param>(static_cast<int>(id)), static_cast<float>(atom_getfloat(atoms + 1)));
    if (previousLatency != core.latency()) reportLatency(x);
}
void transport(Object* x, t_symbol*, int n, t_atom* atoms) {
    if (n != 7 && n != 9) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        if (atoms[i].a_type != A_FLOAT || !std::isfinite(atom_getfloat(atoms + i))) {
            return;
        }
    }
    if (n == 9) {
        // The host's exact position for this tick: whole quarter notes plus
        // the fraction, each exact in t_float, added in double.
        adi::device::applyShaperPosition(x->state->core, atom_getfloat(atoms) != 0,
            atom_getfloat(atoms + 1),
            static_cast<double>(atom_getfloat(atoms + 7)) + static_cast<double>(atom_getfloat(atoms + 8)));
        return;
    }
    adi::device::applyShaperTransport(x->state->core, atom_getfloat(atoms) != 0, atom_getfloat(atoms + 1),
        atom_getfloat(atoms + 2), atom_getfloat(atoms + 3), atom_getfloat(atoms + 4),
        atom_getfloat(atoms + 5), atom_getfloat(atoms + 6));
}
} // namespace

extern "C" void adi_shaper_tilde_setup() {
    shaperClass = class_new(gensym("adi.shaper~"), reinterpret_cast<t_newmethod>(create),
                            reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_NULL);
    CLASS_MAINSIGNALIN(shaperClass, Object, signal);
    class_addmethod(shaperClass, reinterpret_cast<t_method>(reportLatency), gensym("latency"), A_NULL);
    class_addmethod(shaperClass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(shaperClass, reinterpret_cast<t_method>(parameter), gensym("p"), A_GIMME, A_NULL);
    class_addmethod(shaperClass, reinterpret_cast<t_method>(transport), gensym("transport"), A_GIMME, A_NULL);
}
ADI_PD_BUILTIN(adi.shaper~, adi_shaper_tilde_setup)
