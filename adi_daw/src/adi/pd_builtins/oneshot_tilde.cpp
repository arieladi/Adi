// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/pd_builtins/oneshot_midi.hpp"
#include "adi/pd_builtins/oneshot_sample.hpp"
#include "adi/pd_builtins/sample_bridge.hpp"
#include "juce/pd_builtins.hpp"
extern "C" {
#include "m_pd.h"
}
#include <bit>
#include <cmath>

namespace {
t_class* klass = nullptr;
struct State {
    adi::device::OneShot core;
    adi::device::OneShotPdMidi midi;
    adi::engine::TransportInfo transport{};
};
struct Object {
    t_object object;
    State* state;
    std::int32_t slot;
    t_outlet* dropped;
};
void* create(t_symbol*, int argc, t_atom* argv) {
    auto* x = reinterpret_cast<Object*>(pd_new(klass));
    x->state = new State;
    x->state->transport.bpm = 120;
    const float slot = argc ? atom_getfloat(argv) : 1;
    x->slot = std::isfinite(slot) && slot >= 1 && slot < 2147483648.f &&
              std::floor(slot) == slot ? static_cast<std::int32_t>(slot) : 1;
    outlet_new(&x->object, &s_signal);
    outlet_new(&x->object, &s_signal);
    x->dropped = outlet_new(&x->object, &s_float);
    return x;
}
void destroy(Object* x) { delete x->state; }
t_int* perform(t_int* w) {
    auto* x = reinterpret_cast<Object*>(w[1]);
    auto& state = *x->state;
    const auto* buffer = adi::device::PdSampleBlock::get(x->slot);
    const auto* sample = dynamic_cast<const adi::device::PdOneShotSample*>(buffer);
    float* output[]{reinterpret_cast<float*>(w[2]), reinterpret_cast<float*>(w[3])};
    adi::engine::NodeIo io{};
    io.out = output;
    io.channels = 2;
    io.frames = static_cast<std::int32_t>(w[4]);
    io.events = state.midi.events();
    io.transport = &state.transport;
    if (state.midi.needsVoiceReset()) state.core.resetVoices();
    state.core.processPrepared(io, sample ? sample->prepared.get() : nullptr,
                               buffer ? buffer->seq : 0);
    state.midi.consumed();
    return w + 5;
}
void dsp(Object* x, t_signal** signal) {
    x->state->core.prepare(signal[0]->s_sr, signal[0]->s_n);
    x->state->midi.reset();
    t_int args[]{reinterpret_cast<t_int>(x), reinterpret_cast<t_int>(signal[0]->s_vec),
                 reinterpret_cast<t_int>(signal[1]->s_vec), static_cast<t_int>(signal[0]->s_n)};
    dsp_addv(perform, 4, args);
}
void parameter(Object* x, t_symbol*, int count, t_atom* atoms) {
    if (count != 2) return;
    const float id = atom_getfloat(atoms), value = atom_getfloat(atoms + 1);
    if (!std::isfinite(id) || std::floor(id) != id || id < 0 ||
        id >= static_cast<float>(adi::device::OneShot::Count) || !std::isfinite(value)) return;
    const auto* p = x->state->core.paramAt(static_cast<std::int32_t>(id));
    x->state->core.setParam(p->id, adi::device::ParamValue::fromNormalized(
        (static_cast<double>(value) - p->minReal) / (p->maxReal - p->minReal)));
}
void note(Object* x, t_floatarg key, t_floatarg velocity, t_floatarg channel) {
    x->state->midi.note(key, velocity, channel);
}
void bend(Object* x, t_floatarg value, t_floatarg channel) { x->state->midi.bend(value, channel); }
void tempo(Object* x, t_floatarg value) {
    if (std::isfinite(value) && value > 0) x->state->transport.bpm = value;
}
void reset(Object* x) {
    x->state->core.resetVoices();
    x->state->midi.reset();
}
// An explicit query makes overflow observable without logging on the callback.
void drops(Object* x) { outlet_float(x->dropped, static_cast<t_float>(x->state->midi.dropped())); }
} // namespace
extern "C" void adi_oneshot_tilde_setup() {
    klass = class_new(gensym("adi.oneshot~"), std::bit_cast<t_newmethod>(&create),
        reinterpret_cast<t_method>(destroy), sizeof(Object), CLASS_DEFAULT, A_GIMME, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(dsp), gensym("dsp"), A_CANT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(parameter), gensym("p"), A_GIMME, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(note), gensym("note"), A_FLOAT, A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(bend), gensym("bend"), A_FLOAT, A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(tempo), gensym("tempo"), A_FLOAT, A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(reset), gensym("reset"), A_NULL);
    class_addmethod(klass, reinterpret_cast<t_method>(drops), gensym("drops"), A_NULL);
}
ADI_PD_BUILTIN(adi.oneshot~, adi_oneshot_tilde_setup)
