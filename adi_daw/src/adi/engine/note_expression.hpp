// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mapping ADI's per-note expression onto VST3's, and the arithmetic that does
// it. **No JUCE and no VST3 headers**, so this tests on all seven ABIs.
//
// The split is deliberate and it is the same one `device_core.hpp` uses. The
// interesting part of the VST3 event path is the CONVERSION — which dimension
// becomes which type id, and what the normalised value is — and getting that
// wrong is silent: the plugin receives a well-formed event carrying the wrong
// number, and the note bends by the wrong amount rather than not at all.
// Filling in Steinberg's structs is the boring part and is the only part that
// needs their headers.
//
// `vst3_events.hpp` static_asserts every constant here against the real SDK,
// so a value that drifts is a compile error in the JUCE job rather than a
// wrong pitch on somebody's Continuum.

#pragma once

#include "adi/blob.hpp"

#include <cstdint>

namespace adi::engine {

/// VST3's predefined note-expression type ids, from `ivstnoteexpression.h`.
///
/// Repeated here rather than included, so the conversion is testable without
/// a plugin SDK. `vst3_events.hpp` asserts each one against Steinberg's enum.
enum class Vst3NoteExprType : std::uint32_t {
    Volume     = 0,
    Pan        = 1,
    Tuning     = 2,
    Vibrato    = 3,
    Expression = 4,
    Brightness = 5,
    Text       = 6,
    Phoneme    = 7,
    /// Not a type: "this dimension has no VST3 equivalent".
    None       = 0xFFFFFFFFu,
};

/// VST3's tuning range. From the SDK's own comment:
///
///     kTuningTypeID, plain range [0 = -120.0, 0.5 none, 1 = +120.0]
///     plain = 240 * (norm - 0.5) and norm = plain / 240 + 0.5
///
/// Ten octaves each way. MPE's per-note range is ±48 semitones, so a
/// full-scale MPE bend lands at 0.7 rather than at 1.0 — which is correct and
/// is worth stating, because a conversion that mapped MPE's extreme onto
/// VST3's extreme would transpose by two and a half octaves at full bend.
inline constexpr double kVst3TuningSemitoneRange = 240.0;

/// Which VST3 type carries this ADI dimension.
///
/// Pitch → Tuning is the only one that is truly canonical. Pressure and
/// Timbre are MPE's Z and Y, and VST3 has no dimension named for either: the
/// convention hosts have settled on is Expression for Z and Brightness for Y,
/// which matches what MPE's own specification says those axes mean. A plugin
/// that publishes custom types through `INoteExpressionController` should be
/// preferred over these defaults where it does — that negotiation is not
/// built, and this is the fallback it would fall back to.
[[nodiscard]] constexpr Vst3NoteExprType vst3TypeFor(ExpressionDim d) noexcept {
    switch (d) {
        case ExpressionDim::Pitch:    return Vst3NoteExprType::Tuning;
        case ExpressionDim::Pressure: return Vst3NoteExprType::Expression;
        case ExpressionDim::Timbre:   return Vst3NoteExprType::Brightness;
        case ExpressionDim::Gain:     return Vst3NoteExprType::Volume;
        case ExpressionDim::Pan:      return Vst3NoteExprType::Pan;
    }
    return Vst3NoteExprType::None;
}

/// Semitones to VST3's normalised tuning value.
///
/// `norm = plain / 240 + 0.5`, clamped. The clamp matters: a plugin handed a
/// value outside 0..1 is in undefined territory per the SDK, and MPE's range
/// is configurable — somebody setting ±127 semitones on a controller would
/// otherwise push past VST3's ±120 and produce a value no plugin is required
/// to survive.
[[nodiscard]] constexpr double semitonesToVst3Tuning(double semitones) noexcept {
    const double n = semitones / kVst3TuningSemitoneRange + 0.5;
    return n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
}

/// The inverse, for reading a plugin's own automation of a note expression.
[[nodiscard]] constexpr double vst3TuningToSemitones(double norm) noexcept {
    return kVst3TuningSemitoneRange * (norm - 0.5);
}

/// Convert one ADI expression value into the VST3 normalised value for its
/// type. Returns false when the dimension has no VST3 equivalent, so a caller
/// cannot silently send a Volume event for something it did not mean.
[[nodiscard]] constexpr bool toVst3NoteExpression(ExpressionDim d, double value,
                                                  Vst3NoteExprType& typeOut,
                                                  double& normOut) noexcept {
    typeOut = vst3TypeFor(d);
    if (typeOut == Vst3NoteExprType::None) return false;

    if (typeOut == Vst3NoteExprType::Tuning) {
        // The one dimension that is NOT already normalised. ADI carries pitch
        // in semitones because that is what the music means; VST3 wants it as
        // a fraction of ten octaves.
        normOut = semitonesToVst3Tuning(value);
        return true;
    }

    // Pressure, Timbre, Pan and Gain are already 0..1 in ADI. Clamped for the
    // same reason as above.
    normOut = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    return true;
}

}  // namespace adi::engine
