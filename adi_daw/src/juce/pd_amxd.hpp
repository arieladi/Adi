// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `.amxd` translator, steps 1 to 9 -- DEV-16 (ADR-0203 d8: an addition at P2).
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
//     accumulated so far. Step 7: a time in the right inlet carries the next
//     lone number and then reverts to 0; the right outlet bangs when the
//     whole list has played (a [delay] that each new input reschedules); an
//     argument is the initial value.
//   * Step 4, where Pd has no box of the same name or the same contract:
//     [prepend]/[append] (a list then [list trim], so the first word is the
//     selector as in Max), [!- n] as (-x) + n, [gate n] as a prefix and a
//     [route], [selector~ n] as one [*~] per input summed, [slide~] as its
//     own recursion in [fexpr~], and [biquad~], whose a0 a1 a2 b1 b2 (with
//     the feedback SUBTRACTED) become Pd's fb1 fb2 ff1 ff2 ff3 (ADDED): a
//     coefficient list is reordered and its b's negated on the way in.
//     live.text (toggle mode), live.menu and live.tab are parameters too.
//   * Max's [%] is an INTEGER modulo whatever it is given (step 8): [% 2.5]
//     takes 2 as its divisor and truncates what it is fed, as [+ 1] does.
//   * [cycle~]'S PHASE INLET (step 6) offsets the phase in Max and resets it
//     in Pd's [osc~]: such a [cycle~] is [phasor~] -> [+~] -> [cos~].
//   * Step 9, from an audit of the table against Max's reference pages: a
//     bare [/~] divides by 1; a bare [t] has two int outlets; [gate] and
//     [selector~] clamp a choice past their count; a bang into [gate]
//     reports the open outlet; [loadbang] and live.thisdevice take a bang;
//     plugin~/plugout~'s channel arguments do not route; a message box's
//     "---" names are the device's, and its right inlet sets it; number
//     boxes keep their minimum, maximum and float format; and the fan-out
//     order follows the destination INLET's x, so one outlet into both
//     inlets of a box feeds the right one first. Refused: quoted symbols in
//     messages, a signal into [phasor~]'s right inlet (hard sync), unstable
//     biquad~ arguments, live.* Steps and their outlets past the first.
//   * BPATCHERS (step 5). One whose patcher is embedded becomes a [pd name]
//     as a [p] does; Max replaces #1 to #9 in its boxes by its arguments and
//     #0 by a name unique to the instance, and so does the translation (#0
//     as $0-bp-<its path>, so two instances never share a send). Its [p]s
//     take the same arguments; a bpatcher inside it takes its own. A # with
//     no argument is refused rather than guessed, as is a bpatcher naming a
//     patcher file outside the device.
// Refused, for later steps: a biquad~ whose coefficients change at run time
// while its smoothing is on (Max's default; how it smooths is not yet read),
// signal-rate
// biquad~ coefficients and slide~ times, live.text's button mode, bpatchers
// of a file outside the device, MIDI devices and instruments; and
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
