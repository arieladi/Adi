// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/pd_builtins/adi_shaper_transport.hpp"
#include "juce/pd_builtins.hpp"
extern "C" {
#include "m_pd.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <type_traits>

namespace {
using Core = adi::dsp::AdiShaper;
t_class* shaperClass = nullptr;
struct Object {
    t_object object;
    t_float signal;
    Core* core;
    t_outlet* latency;
};

void* create() {
    auto* x = reinterpret_cast<Object*>(pd_new(shaperClass));
    x->core = new (std::nothrow) Core;
    if (!x->core) {
        pd_free(&x->object.ob_pd);
        return nullptr;
    }
    inlet_new(&x->object, &x->object.ob_pd, &s_signal, &s_signal);
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    x->latency = outlet_new(&x->object, &s_float);
    return x;
}
void destroy(Object* x) { delete x->core; }
void reportLatency(Object* x) { outlet_float(x->latency, static_cast<t_float>(x->core->latency())); }

template <typename Sample>
void render(Core& core, const Sample* left, const Sample* right, Sample* outLeft,
            Sample* outRight, std::size_t count) noexcept {
    if constexpr (std::is_same_v<Sample, float>) {
        core.process(left, right, outLeft, outRight, count);
    } else {
        // A double-sample Pd build uses the same float DSP as the host.
        std::array<float, 64> l, r;
        for (std::size_t at = 0; at < count;) {
            const auto n = std::min(l.size(), count - at);
            for (std::size_t i = 0; i < n; ++i) {
                l[i] = static_cast<float>(left[at + i]);
                r[i] = static_cast<float>(right[at + i]);
            }
            core.process(l.data(), r.data(), l.data(), r.data(), n);
            std::copy_n(l.data(), n, outLeft + at);
            std::copy_n(r.data(), n, outRight + at);
            at += n;
        }
    }
}
t_int* perform(t_int* args) {
    auto* x = reinterpret_cast<Object*>(args[1]);
    render(*x->core, reinterpret_cast<const t_sample*>(args[2]),
           reinterpret_cast<const t_sample*>(args[3]), reinterpret_cast<t_sample*>(args[4]),
           reinterpret_cast<t_sample*>(args[5]), static_cast<std::size_t>(args[6]));
    return args + 7;
}
void dsp(Object* x, t_signal** signals) {
    x->core->prepare(signals[0]->s_sr);
    reportLatency(x);
    t_int args[]{reinterpret_cast<t_int>(x), reinterpret_cast<t_int>(signals[0]->s_vec),
                 reinterpret_cast<t_int>(signals[1]->s_vec), reinterpret_cast<t_int>(signals[2]->s_vec),
                 reinterpret_cast<t_int>(signals[3]->s_vec), static_cast<t_int>(signals[0]->s_n)};
    dsp_addv(perform, 6, args);
}
void parameter(Object* x, t_symbol*, int n, t_atom* atoms) {
    if (n != 2 || atoms[0].a_type != A_FLOAT || atoms[1].a_type != A_FLOAT) return;
    const auto id = atom_getfloat(atoms);
    if (!std::isfinite(id) || id < 0 || id >= static_cast<t_float>(Core::Count) || std::trunc(id) != id) return;
    const int previousLatency = x->core->latency();
    x->core->set(static_cast<Core::Param>(static_cast<int>(id)),
                 static_cast<float>(atom_getfloat(atoms + 1)));
    if (previousLatency != x->core->latency()) reportLatency(x);
}
void transport(Object* x, t_symbol*, int n, t_atom* atoms) {
    if (n != 7) return;
    for (int i = 0; i < n; ++i)
        if (atoms[i].a_type != A_FLOAT || !std::isfinite(atom_getfloat(atoms + i))) return;
    adi::device::applyShaperTransport(*x->core, atom_getfloat(atoms) != 0, atom_getfloat(atoms + 1),
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
