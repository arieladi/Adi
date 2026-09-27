// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI's compiled-in Pd externals -- ADR-0188 d8, ADR-0192 d1.
//
// ADR-0188 d8 says ADI's externals "are compiled into the engine and registered
// with libpd as built-ins", and that nothing loads from disk. `pd_engine.cpp`
// enforces the second half. This is the first: the table of setup functions the
// engine calls, and the one place an external announces itself.
//
// WHY REGISTRATION HAPPENS ONCE, BEFORE ANY INSTANCE EXISTS -- and what that
// does NOT buy, which was worth finding out. Read in `m_class.c` at the pinned
// commit, after a planted fault passed a test that said it should not:
//
//   * `pd_objectmaker` is ONE OBJECT FOR THE PROCESS (`m_class.c:27`), not one
//     per instance. What is per instance is the METHOD LIST hanging off each
//     class: `c->c_methods` is an array indexed by instance.
//   * `class_new` registers a creator with `class_addmethod(pd_objectmaker,
//     ...)`, and under PDINSTANCE `class_doaddmethod` loops
//     `for (i = 0; i < pd_ninstances; i++)` -- it adds the method to EVERY
//     instance that exists at that moment.
//   * `pdinstance_new` copies INSTANCE 0's method list into each new
//     instance's slot.
//
// Instance 0 always exists and is always in that loop, so **a class registered
// at any moment reaches every instance, before it and after it**. Registering
// early is therefore NOT what makes the externals reachable -- Pd would have
// managed either way.
//
// What it buys is DETERMINISM, which is the reason that survives. Every device
// sees the same vocabulary from the moment it opens, and no patch can be opened
// against a half-built table. That matters here more than elsewhere: ADR-0177
// fix 3 says what a patch can do is knowable from its TEXT, and a set of
// built-in objects that depended on when a device happened to load would make
// the same patch mean two things in one session. `add` refuses after
// `registerAll` for exactly that reason, and for no reason to do with Pd.
//
// HOW AN EXTERNAL ANNOUNCES ITSELF. It writes `ADI_PD_BUILTIN` beside its own
// setup function, in its own file. Nothing here is edited to add one, which is
// the point: the externals belong to whoever owns that DSP (ADR-0192 d6 gives
// the color-bass pair to win_codex), and this file is in `src/juce/**`, which
// that agent does not touch.
//
// THE LINKER TRAP THAT COMES WITH SELF-REGISTRATION, and how it is avoided. A
// translation unit in a static library whose symbols nothing references is
// dropped, taking its registrar with it -- the external would simply not exist,
// with no error anywhere. The externals are therefore built as a CMake OBJECT
// library (`adi_pd_builtins`) whose objects are always linked, rather than
// added to `adi_core`'s source list. See CMakeLists.txt.

#pragma once

#include <cstddef>

namespace adi::device {

/// One compiled-in external: the name a patch writes in a box, and the setup
/// function that registers its class with Pd.
struct PdBuiltin {
    const char* name = nullptr;
    void (*setup)() = nullptr;
};

class PdBuiltins {
public:
    /// Declares a compiled-in external. **Before `PdRuntime::initialise`** --
    /// ordinarily from a static registrar, which runs before `main`.
    ///
    /// False, and nothing declared, when: the name or the function is null;
    /// the name is already declared; or Pd has already been initialised, which
    /// is the ordering rule above and is refused rather than half applied.
    static bool add(const char* name, void (*setup)()) noexcept;

    /// How many are declared, and the name of each, in declaration order.
    [[nodiscard]] static std::size_t count() noexcept;
    [[nodiscard]] static const char* nameAt(std::size_t index) noexcept;

    /// True once `registerAll` has run. `add` is refused from then on.
    [[nodiscard]] static bool registered() noexcept;

    /// Calls every declared setup function, in declaration order, exactly once
    /// per process. `PdRuntime::initialise` calls it; nothing else should.
    /// Returns how many ran, which is 0 on a second call.
    static std::size_t registerAll() noexcept;

private:
    PdBuiltins() = delete;
};

}  // namespace adi::device

/// Declares a compiled-in external from the file that defines it:
///
///     extern "C" void adi_combchord_tilde_setup(void);
///     ADI_PD_BUILTIN(adi.combchord~, adi_combchord_tilde_setup)
///
/// The name is written unquoted so it reads as it does in a patch box. It runs
/// before `main`, which is before `PdRuntime::initialise`, which is the order
/// the header above requires.
#define ADI_PD_BUILTIN(boxname, setupfn)                                       \
    namespace {                                                                \
    const bool adi_pd_builtin_##setupfn =                                      \
        ::adi::device::PdBuiltins::add(#boxname, &setupfn);                    \
    }
