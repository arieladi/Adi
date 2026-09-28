// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/pd_builtins/sample_bridge.hpp"
#include "adi/pd_builtins/colorcab_sample.hpp"
#include "juce/pd_builtins.hpp"
extern "C" {
#include "m_pd.h"
}
#include <algorithm>
#include <bit>
#include <cmath>
namespace {
t_class* klass = nullptr;
struct Object {
    t_object object; t_float signal;
    adi::dsp::ColorCab* core;
    std::int32_t slot;
    std::uint64_t sequence;
    double rate;
};
void* create(t_symbol*, int argc, t_atom* argv) {
    auto* x = reinterpret_cast<Object*>(pd_new(klass));
    x->core = new adi::dsp::ColorCab;
    const float id = argc ? atom_getfloat(argv) : 1;
    x->slot = std::isfinite(id) && id >= 1 && id < 2147483648.f ? static_cast<std::int32_t>(id) : 1;
    x->sequence = 0; x->rate = 0;
    outlet_new(&x->object, &s_signal);
    return x;
}
void destroy(Object* x) { delete x->core; }
t_int* perform(t_int* w) {
    auto* x = reinterpret_cast<Object*>(w[1]);
    const auto* sample = dynamic_cast<const adi::device::PdColorCabSample*>(adi::device::PdSampleBlock::get(x->slot));
    if (sample && sample->seq != x->sequence && sample->design.sampleRate == x->rate &&
        x->core->setKernel(sample->design.kernel)) x->sequence = sample->seq;
    x->core->process(reinterpret_cast<const float*>(w[2]), reinterpret_cast<float*>(w[3]),
                     static_cast<std::size_t>(w[4]));
    return w + 5;
}
void dsp(Object* x, t_signal** s) {
    x->rate = s[0]->s_sr; x->sequence = 0;
    x->core->prepare(x->rate);
    // dsp_add reads pointer-sized t_int varargs; an int frame count leaves
    // the upper stack word undefined on Win64. Use its typed vector API.
    t_int args[] = {reinterpret_cast<t_int>(x), reinterpret_cast<t_int>(s[0]->s_vec),
                    reinterpret_cast<t_int>(s[1]->s_vec), static_cast<t_int>(s[0]->s_n)};
    dsp_addv(perform, 4, args);
}
void mix(Object* x, t_floatarg f) { x->core->setMix(f); }
}
extern "C" void adi_colorcab_tilde_setup() {
    // Pd stores a type-erased constructor and calls it with the A_GIMME ABI.
    klass = class_new(gensym("adi.colorcab~"), std::bit_cast<t_newmethod>(&create),
        reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_GIMME, A_NULL);
    CLASS_MAINSIGNALIN(klass, Object, signal);
    class_addmethod(klass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(mix), gensym("mix"), A_FLOAT, A_NULL);
}
ADI_PD_BUILTIN(adi.colorcab~, adi_colorcab_tilde_setup)
