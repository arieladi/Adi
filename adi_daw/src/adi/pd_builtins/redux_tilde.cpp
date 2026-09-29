// SPDX-License-Identifier: MIT
#include "adi/dsp/redux.hpp"
#include "juce/pd_builtins.hpp"
extern "C" {
#include "m_pd.h"
}
namespace {
t_class* klass=nullptr;
struct Object { t_object object; t_float signal; adi::dsp::Redux* core; };
void* create() {
    auto* x=reinterpret_cast<Object*>(pd_new(klass));
    x->core=new adi::dsp::Redux;
    inlet_new(&x->object,&x->object.ob_pd,&s_signal,&s_signal);
    outlet_new(&x->object,&s_signal); outlet_new(&x->object,&s_signal);
    return x;
}
void destroy(Object* x) { delete x->core; }
t_int* perform(t_int* w) {
    auto* x=reinterpret_cast<Object*>(w[1]);
    x->core->process(reinterpret_cast<const float*>(w[2]),reinterpret_cast<const float*>(w[3]),
        reinterpret_cast<float*>(w[4]),reinterpret_cast<float*>(w[5]),static_cast<std::size_t>(w[6]));
    return w+7;
}
void dsp(Object* x,t_signal** s) {
    x->core->prepare(s[0]->s_sr);
    t_int args[]={reinterpret_cast<t_int>(x),reinterpret_cast<t_int>(s[0]->s_vec),
        reinterpret_cast<t_int>(s[1]->s_vec),reinterpret_cast<t_int>(s[2]->s_vec),
        reinterpret_cast<t_int>(s[3]->s_vec),static_cast<t_int>(s[0]->s_n)};
    dsp_addv(perform,6,args);
}
void rate(Object* x,t_floatarg v) { x->core->setRate(v); }
void jitter(Object* x,t_floatarg v) { x->core->setJitter(v); }
void bits(Object* x,t_floatarg v) { x->core->setBits(v); }
void shape(Object* x,t_floatarg v) { x->core->setShape(v); }
void pre(Object* x,t_floatarg v) { x->core->setPre(v); }
void postFilter(Object* x,t_floatarg v) { x->core->setPost(v); }
void octave(Object* x,t_floatarg v) { x->core->setOctave(v); }
void dc(Object* x,t_floatarg v) { x->core->setDcShift(v); }
void mix(Object* x,t_floatarg v) { x->core->setMix(v); }
}
extern "C" void adi_redux_tilde_setup() {
    klass=class_new(gensym("adi.redux~"),reinterpret_cast<t_newmethod>(create),
        reinterpret_cast<t_method>(destroy),sizeof(Object),CLASS_DEFAULT,A_NULL);
    CLASS_MAINSIGNALIN(klass,Object,signal);
    class_addmethod(klass,reinterpret_cast<t_method>(dsp),gensym("dsp"),A_CANT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(rate),gensym("rate"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(jitter),gensym("jitter"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(bits),gensym("bits"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(shape),gensym("shape"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(pre),gensym("pre"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(postFilter),gensym("post"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(octave),gensym("octave"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(dc),gensym("dc"),A_FLOAT,A_NULL);
    class_addmethod(klass,reinterpret_cast<t_method>(mix),gensym("mix"),A_FLOAT,A_NULL);
}
ADI_PD_BUILTIN(adi.redux~, adi_redux_tilde_setup)
