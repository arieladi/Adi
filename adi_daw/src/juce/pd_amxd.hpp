// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `.amxd` translator, steps 1 and 2 -- DEV-16 (ADR-0203 d8: an addition at P2).
//
// A Max for Live device file is a small chunked container -- `ampf` with the
// device kind, `meta`, then `ptch` holding the Max patcher as JSON -- and the
// patcher is boxes and patch lines. This reads it and translates it into a Pd
// patch for DEV-10's user-patch device, or refuses it naming every box it
// cannot translate.
//
// THE PRE-FLIGHT IS THE TRANSLATION TABLE. A Max box is allowed when this file
// has a Pd translation for it whose behaviour is the same, and only then; the
// Pd text that comes out must then pass PD4's allowed-object walk
// (pd_object_walk.hpp) against the running build. Nothing is translated by
// resemblance: where Max and Pd differ in a way a patch would hear, the
// difference is made explicit in the Pd patch, or the box is refused with the
// reason. Made explicit (step 2):
//   * Max's INTEGERS. [+ 1], [/ 2], [> 0], [pack] and [unpack] with their
//     default "0 0", [trigger]'s `i` and the number box work on ints in Max;
//     Pd has only floats. Each int inlet goes through [i] first, and integer
//     division through [i] after, so [+ 1] fed 2.7 gives 3 as in Max.
//   * SUBPATCHERS. [p name] becomes [pd name]; an inlet carries a signal
//     ([inlet~]) when the parent feeds it one, an outlet when it is fed one.
//   * [line~] (step 3). Max chains a list of (target, ms) pairs; Pd's
//     [vline~] takes one segment and reads a third number as a delay. So a
//     [pd max-line~] walks the pairs and schedules each with the delay
//     accumulated so far.
// Refused, for later steps: [cycle~]'s phase inlet (an offset in Max, a reset
// in Pd), [line~]'s ramp-time inlet and end bang, float modulo, bpatchers,
// MIDI devices and instruments; and
// not read at all: frozen devices (`mx@c`) and Live's own encrypted devices
// (`ciph`).
//
// MAX'S RIGHT-TO-LEFT ORDER, MADE EXPLICIT. When a Max control outlet feeds
// several boxes, Max sends to them right to left by the destination's
// position. Pd's order for one outlet is the order the connections were made,
// which a translator does not control. So every control outlet with more than
// one connection goes through a [t a a ...] whose rightmost outlet feeds the
// rightmost destination -- the order is then in the patch, not in the
// connection list. Equal x positions: lower box first (an assumption to be
// read from Max; logged).
//
// A `live.dial` (or `live.numbox`, `live.slider`) becomes `[adi.param]` with
// the parameter's range, default, unit and curve, ids 1, 2, ... in box order.
// `[plugin~]`/`[plugout~]` become `[adc~]`/`[dac~]`, which is where DEV-10
// reads a patch's ports. Max's `---name` (device-local) sends become `$0-name`.
//
// No libpd; builds everywhere.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace adi::device {

enum class AmxdKind : std::uint8_t { AudioEffect, MidiEffect, Instrument, Unknown };

struct AmxdRefusedBox {
    std::string id;        ///< Max's box id, "obj-3"
    std::string maxClass;  ///< `maxclass`, or the first word of a newobj's text
    std::string text;      ///< the box text as Max shows it
    std::string reason;
};

struct AmxdTranslation {
    AmxdKind kind = AmxdKind::Unknown;
    /// Why the file was not read at all; empty when it was.
    std::string error;
    /// Boxes with no translation. Empty and no error: `pdPatch` is the device.
    std::vector<AmxdRefusedBox> refused;
    std::string pdPatch;
    /// Parameters made from live.* controls, in id order: their long names.
    std::vector<std::string> parameters;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && refused.empty(); }
};

/// Reads an `.amxd` file's bytes and translates its patcher.
[[nodiscard]] AmxdTranslation translateAmxd(std::span<const std::uint8_t> bytes);

/// Translates a Max patcher's JSON (the `ptch` chunk) for a device of `kind`.
[[nodiscard]] AmxdTranslation translateMaxPatcher(std::string_view json, AmxdKind kind);

}  // namespace adi::device
