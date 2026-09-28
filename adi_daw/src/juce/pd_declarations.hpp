// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading `[adi.param]` and `[adi.array]` out of a patch -- ADR-0177, ADR-0183.
//
// ONE SCANNER, TWO SCHEMAS, and that is the point rather than a convenience.
// The director's instruction is that the two objects "move as identical
// siblings"; the way to make that true is for there to be one tokeniser, one
// `$0` check, one id rule and one problem list, with the schemas differing only
// where they must. Two parsers that agree today are two parsers that disagree
// after the next edit.
//
// THE PARSE IS STATIC. ADR-0177 fix 3: the DAW reads declarations from the
// stored patch TEXT, off the audio thread, WITHOUT RUNNING PD. That is what
// makes the declared set change only through `device.loadState` -- a patch is
// device state (ADR-0145 d8), and a change to it is one op that undo reverses.
// A DAW that learned its parameters by opening the patch in Pd would change the
// project with no op at all, which ADR-0003 forbids and which the panel
// document named as the fatal flaw of the original proposal.
//
// So nothing here includes z_libpd.h, and none of it needs libpd to be present.
//
// THE PD FILE FORMAT, only as much as is needed:
//   * a record ends at an unescaped `;`, and may span lines;
//   * `\;`, `\,` and `\$` are escapes -- the generator writes `\$0`, so the
//     token this sees is `$0`;
//   * atoms are whitespace-separated;
//   * `#X obj <x> <y> <class> <args...>` is an object box.
// Subpatches (`#N canvas` ... `#X restore`) are scanned like any other part of
// the file: ADR-0177 allows declarations in the device's patch or its
// subpatches, because those share its `$0`. An abstraction does not, and cannot
// appear in this file anyway.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace adi::device {

/// How a parameter's knob maps to its value. ADR-0177 decision 1.
enum class PdCurve : std::uint8_t {
    Lin,
    Log,        ///< frequencies; requires min > 0
    Exponent,   ///< a positive number in the declaration, as M4L's
    Int,        ///< whole steps
    Toggle,     ///< 0 or 1
    Menu,       ///< min 0, and max + 1 items follow the name
};

/// Why a declaration was not taken. The name a problem list shows is
/// "pd.duplicate_id" and so on, as `DecodeProblem` does it.
///
/// **Every one of these means the declaration is IGNORED and the device still
/// loads** (ADR-0177 decision 2). A patch with one bad parameter is not a
/// broken device; it is a device with one fewer parameter and a problem to
/// show. Refusing to load it would lose the other forty.
enum class PdDeclProblem : std::uint8_t {
    NotDollarZero,   ///< the first argument is not `$0`, so no receive name can be built
    BadId,           ///< not a positive integer below 2^31
    DuplicateId,     ///< an id already declared; the LATER one is ignored
    WrongArity,      ///< too few or too many arguments
    BadNumber,       ///< a numeric field that is not a number
    BadRange,        ///< min >= max, or a default outside it
    BadCurve,        ///< not one of the names, and not a positive number
    LogNeedsPositiveMin,  ///< `log` with min <= 0 has no mapping
    MenuItemCount,   ///< a menu whose item count is not max + 1, or whose min is not 0
    BadLength,       ///< an array length that is not a positive integer within bounds
    BadRate,         ///< an array rate that is not a positive, finite, bounded number
};
[[nodiscard]] const char* toString(PdDeclProblem) noexcept;

struct PdDeclProblemReport {
    PdDeclProblem problem = PdDeclProblem::WrongArity;
    /// Which object box in the file, counting from 1. Not a line number: a
    /// record may span lines, and the box index is what a patch author can
    /// find by counting objects.
    int box = 0;
    std::string detail;
};

/// `[adi.param $0 <id> <min> <max> <default> <unit> <curve> <name> [<item>...]]`
struct PdParamDecl {
    std::int32_t id = 0;
    double min = 0.0, max = 1.0, def = 0.0;
    std::string unit;                  ///< `-` in the patch becomes empty here
    PdCurve curve = PdCurve::Lin;
    double exponent = 1.0;             ///< meaningful only when curve == Exponent
    std::string name;                  ///< underscores are already spaces
    std::vector<std::string> items;    ///< menu only
    int box = 0;
};

/// `[adi.array $0 <id> <length> <rate> <min> <max> <unit> <name>]`
///
/// The extra fields over ADR-0183's first sketch are ADR-0177 fix 2 applied to
/// arrays: "the declaration is complete". A renderer given only a length and a
/// rate has to guess the value range, and a guess about a dB floor is a picture
/// that is wrong in a way nobody can see. The patch knows; it says.
struct PdArrayDecl {
    std::int32_t id = 0;
    std::int32_t length = 0;           ///< cells
    double rate = 0.0;                 ///< how often the patch writes it, in Hz
    double min = 0.0, max = 1.0;       ///< the value range, in `unit`
    std::string unit;
    std::string name;
    int box = 0;

    /// A patch that declares a billion cells must not make the DAW allocate
    /// them. The same sanity bound `PdLatencyReceiver::maxSamples` is, and for
    /// the same reason (ADR-0085).
    static constexpr std::int32_t kMaxLength = 1 << 20;
    /// Faster than this is not a display, and the double buffer would spend
    /// its life being overwritten between reads.
    static constexpr double kMaxRate = 1000.0;
};

/// A `[declare]` in the patch that would reach outside the DAW's own files.
///
/// **ADR-0188 d8: ADI loads no Pd external from disk.** `[declare]` is the one
/// object that can defeat that from inside the patch text, and it has two
/// separate ways to do it:
///   * `-lib` / `-stdlib` load a binary external BY NAME at patch-open time;
///   * `-path` / `-stdpath` add a directory to the canvas's search path, and
///     Pd then resolves externals through it exactly as it resolves
///     abstractions.
/// Both are found here, in the same static parse and with Pd not running, so
/// the engine can refuse the patch before libpd ever sees it.
struct PdExternalRequest {
    std::string flag;    ///< `-path`, `-stdpath`, `-lib` or `-stdlib`
    std::string value;   ///< what followed it, or empty
    int box = 0;
};

/// `[adi.sample $0 <id> <name>]`
///
/// ADR-0192 d4: a sibling of `[adi.param]` and `[adi.array]`, with the same
/// fixed-id rule and for the same reason -- renaming a slot must not orphan the
/// media a project refers to.
///
/// **It declares no length, no rate and no range**, and that asymmetry is the
/// point rather than an omission. An array's shape is the patch's to choose,
/// because the patch fills it; a sample's shape is the FILE's, and the patch
/// finds out what it got. What the host puts in the slot is what the media
/// turned out to be.
struct PdSampleDecl {
    std::int32_t id = 0;
    std::string name;
    int box = 0;
};

struct PdDeclarations {
    std::vector<PdParamDecl> params;
    std::vector<PdArrayDecl> arrays;
    std::vector<PdSampleDecl> samples;
    std::vector<PdDeclProblemReport> problems;

    /// In file order, as ADR-0177 decision 3 requires for drawing the panel.
    [[nodiscard]] const PdParamDecl* param(std::int32_t id) const noexcept;
    [[nodiscard]] const PdArrayDecl* array(std::int32_t id) const noexcept;
    [[nodiscard]] const PdSampleDecl* sample(std::int32_t id) const noexcept;
};

/// Reads every `[adi.param]` and `[adi.array]` in a patch's text.
///
/// **Parameters, arrays and samples have THREE SEPARATE id spaces.** A parameter's id is
/// `plugin_params.param_id` (ADR-0177 decision 1) and an array is not a
/// parameter -- it is never automated, mapped or bound. Sharing one space
/// would make adding a display change what an automation lane points at.
[[nodiscard]] PdDeclarations parsePdDeclarations(std::string_view patchText);

/// Every `[declare]` in the patch that asks for a library or a search path.
///
/// Empty is the normal answer. A non-empty one is a patch `LibPdEngine` must
/// refuse to open: see `PdExternalRequest`. It is deliberately separate from
/// `parsePdDeclarations`, because a bad `[adi.param]` is ignored and the device
/// still loads (ADR-0177 d2), while this is not a declaration to ignore -- it
/// is a reason not to run the patch at all.
[[nodiscard]] std::vector<PdExternalRequest> pdExternalRequests(std::string_view patchText);

/// The receive name the host sends a parameter's value to: `<$0>-adi-<id>`,
/// which is what `adi.param.pd` listens on (ADR-0177 decision 5).
[[nodiscard]] std::string pdParamReceiveName(int dollarZero, std::int32_t id);
/// The array's counterpart: `<$0>-adiarr-<id>`. A different stem, so a patch
/// cannot address an array where a parameter is meant and have it half work.
[[nodiscard]] std::string pdArrayReceiveName(int dollarZero, std::int32_t id);

/// The receive name the host sends the transport to: `<$0>-aditr`, which is
/// what `adi.transport.pd` listens on (ADR-0188 d3). A third stem, for the
/// reason the second one exists: one list arriving where a parameter was meant
/// should fail outright rather than half work.
[[nodiscard]] std::string pdTransportReceiveName(int dollarZero);

}  // namespace adi::device
