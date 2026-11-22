// SPDX-License-Identifier: GPL-3.0-or-later
//
// PD4: the class names THIS build of Pd can create, read from the build.
//
// Pd makes an object box by sending the box's first atom to `pd_objectmaker`,
// a class whose methods are the creators, aliases included, each added by
// `class_new` or `class_addcreator` (m_class.c). So that class IS the
// vocabulary -- vanilla's objects, ADI's compiled-in externals (registered by
// PdBuiltins::registerAll before any instance exists) and nothing else, since
// ADI loads no external from disk (ADR-0188 d8). Abstractions are not in it:
// Pd looks for those on disk when no creator matches, which is what the walk
// (pd_object_walk.hpp) does with the device's stored abstractions instead.
//
// TWO PLACES, NOT ONE. `class_doaddmethod` files a method under its selector
// in the method table -- except bang, float, symbol, list and anything, which
// go to dedicated slots. So [float] and [list] are not in the table; their
// creators sit in pd_objectmaker's float and list slots (a first version that
// read only the table refused Resonators.pd for its [list] boxes). A slot
// counts when it differs from a class with nothing added -- a probe class
// with no creator, made once. The anything slot is Pd's abstraction loader,
// not a class, and is left out.
//
// Every instance carries the same table (`pdinstance_new` copies instance 0's
// and `class_doaddmethod` adds to all of them), so instance 0's is read, under
// its lock.

#include "juce/pd_engine.hpp"

extern "C" {
#include "z_libpd.h"
#include "m_imp.h"
}

namespace adi::device {

std::set<std::string, std::less<>> PdRuntime::creatableClasses() {
    std::set<std::string, std::less<>> names;
    std::string error;
    if (!initialise(error)) return names;
    libpd_set_instance(libpd_main_instance());
    sys_lock();
    // No creator: its slots are Pd's defaults. Made once; Pd never frees a class.
    static t_class* const probe =
        class_new(gensym("adi.vocabulary-probe"), nullptr, nullptr, sizeof(t_pd), CLASS_PD, A_NULL);
    const t_class* maker = pd_objectmaker;
#ifdef PDINSTANCE
    const t_methodentry* methods = maker->c_methods[pd_this->pd_instanceno];
#else
    const t_methodentry* methods = maker->c_methods;
#endif
    for (int i = 0; i < maker->c_nmethod; ++i)
        if (methods[i].me_name != nullptr) names.emplace(methods[i].me_name->s_name);
    if (probe != nullptr) {
        if (maker->c_bangmethod != probe->c_bangmethod) names.emplace("bang");
        if (maker->c_floatmethod != probe->c_floatmethod) names.emplace("float");
        if (maker->c_symbolmethod != probe->c_symbolmethod) names.emplace("symbol");
        if (maker->c_listmethod != probe->c_listmethod) names.emplace("list");
    }
    sys_unlock();
    return names;
}

}  // namespace adi::device
