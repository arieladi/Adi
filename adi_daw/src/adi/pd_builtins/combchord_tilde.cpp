// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/dsp/combchord.hpp"
#include "juce/pd_builtins.hpp"
extern "C" {
#include "m_pd.h"
}
#include <algorithm>
#include <cmath>

namespace {
t_class* klass = nullptr;
struct Object { t_object object; t_float signal; adi::dsp::CombChord* core; };
void* create() {
    auto* x = reinterpret_cast<Object*>(pd_new(klass));
    x->core = new adi::dsp::CombChord;
    outlet_new(&x->object, &s_signal);
    return x;
}
void destroy(Object* x) { delete x->core; }
t_int* perform(t_int* w) {
    auto* x = reinterpret_cast<Object*>(w[1]);
    x->core->process(reinterpret_cast<const float*>(w[2]), reinterpret_cast<float*>(w[3]),
                     static_cast<std::size_t>(w[4]));
    return w + 5;
}
void dsp(Object* x, t_signal** s) {
    x->core->prepare(s[0]->s_sr);
    // dsp_add reads pointer-sized t_int varargs; an int frame count leaves
    // the upper stack word undefined on Win64. Use its typed vector API.
    t_int args[] = {reinterpret_cast<t_int>(x), reinterpret_cast<t_int>(s[0]->s_vec),
                    reinterpret_cast<t_int>(s[1]->s_vec), static_cast<t_int>(s[0]->s_n)};
    dsp_addv(perform, 4, args);
}
void state(Object* x, t_floatarg f) {
    if (std::isfinite(f)) x->core->setState(static_cast<std::size_t>(std::clamp(f, 0.f, 7.f)));
}
void mode(Object* x, t_floatarg f) { x->core->setMode(f >= 0.5f ? adi::dsp::CombChord::Mode::Square : adi::dsp::CombChord::Mode::Saw); }
void decay(Object* x, t_floatarg f) { x->core->setDecay(f); }
void color(Object* x, t_floatarg f) { x->core->setColor(f); }
void mix(Object* x, t_floatarg f) { x->core->setMix(f); }
void output(Object* x, t_floatarg f) { x->core->setOutputDb(f); }
void chord(Object* x, t_symbol*, int argc, t_atom* argv) {
    if (argc != 7) return;
    const auto slot = atom_getfloat(argv);
    if (!std::isfinite(slot) || slot < 0 || slot > 7) return;
    adi::dsp::CombChord::Chord notes{};
    for (std::size_t i = 0; i < notes.size(); ++i) notes[i] = atom_getfloat(argv + i + 1);
    x->core->setChord(static_cast<std::size_t>(slot), notes);
}
}
extern "C" void adi_combchord_tilde_setup() {
    klass = class_new(gensym("adi.combchord~"), reinterpret_cast<t_newmethod>(create),
        reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_NULL);
    CLASS_MAINSIGNALIN(klass, Object, signal);
    class_addmethod(klass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(chord), gensym("chord"), A_GIMME, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(state), gensym("state"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(mode), gensym("mode"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(decay), gensym("decay"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(color), gensym("color"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(mix), gensym("mix"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(output), gensym("output"), A_FLOAT, A_NULL);
}
ADI_PD_BUILTIN(adi.combchord~, adi_combchord_tilde_setup)
