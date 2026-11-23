// SPDX-License-Identifier: GPL-3.0-or-later
//
// The allowed-object walk -- PD4 (DEV-09, DEV-10, DEV-11, DEV-16).
//
// WHAT IT ANSWERS: before a patch is opened, can every object box in it be
// made by THIS build, and if not, which boxes, where? A user patch (DEV-10),
// a patch edited in Pd (DEV-11) and a translated `.amxd` (DEV-16) all pass
// through it, so the answer has to name the box an author would find: the
// file (the patch or one of its abstractions), the subpatch, the class and
// the box's number counting object boxes from 1, as `parsePdDeclarations`
// counts them.
//
// THE WALK IS STATIC, like the declaration parse (ADR-0177 fix 3): it reads
// the patch TEXT and the abstractions the device stores, and runs no Pd. What
// it needs from Pd is only the VOCABULARY, the class names the running build
// can create, and that is read once from the build itself
// (`PdRuntime::creatableClasses`, pd_vocabulary.cpp) rather than written down
// here: a list in this file would be a second truth that drifts the first
// time an external is added or Pd is re-pinned (plugdata's approach, PD4).
//
// RECURSIVE THROUGH SUBPATCHES AND ABSTRACTIONS. A subpatch (`#N canvas` ...
// `#X restore ... pd <name>`) is part of the same file and is walked in place.
// An object whose class is not in the vocabulary but names one of the stored
// abstractions is walked as that abstraction, once per abstraction however
// often it is used; `[clone]` names its abstraction as an argument and is
// followed the same way. An abstraction that reaches itself is refused
// (Pd would recurse until its depth guard), and so is a class name Pd only
// learns at run time (`$1` in a class position), because nothing static can
// say what it will be.
//
// No libpd here, so it builds on every ABI.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace adi::device {

/// The class names a build of Pd can create.
class PdVocabulary {
public:
    PdVocabulary() = default;
    explicit PdVocabulary(std::set<std::string, std::less<>> names) : names_(std::move(names)) {}
    [[nodiscard]] bool contains(std::string_view name) const { return names_.find(name) != names_.end(); }
    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }
    [[nodiscard]] const std::set<std::string, std::less<>>& names() const noexcept { return names_; }

private:
    std::set<std::string, std::less<>> names_;
};

enum class PdObjectRefusal : std::uint8_t {
    Unknown,       ///< not in the vocabulary and not a stored abstraction
    DynamicClass,  ///< a `$` in the class position: decided only when Pd runs
    Recursive,     ///< an abstraction that contains itself, directly or not
    TooDeep,       ///< nesting past kMaxDepth (subpatches and abstractions)
    Malformed,     ///< a `#X restore` with no canvas open, or a canvas never closed
    PathNamed,     ///< an absolute, drive-letter or `..` path in the class position
};
[[nodiscard]] const char* toString(PdObjectRefusal) noexcept;

/// One box that cannot be made.
struct PdRefusedObject {
    PdObjectRefusal reason = PdObjectRefusal::Unknown;
    std::string className;
    /// "" for the device's patch, else the abstraction's name as stored.
    std::string file;
    /// The subpatch path inside `file`: "" at the top, else "pd lfo/pd shape".
    std::string subpatch;
    /// The object box in `file`, counting from 1 in file order.
    int box = 0;
};

struct PdObjectWalk {
    std::vector<PdRefusedObject> refused;
    /// Stored abstractions the patch uses, in first-use order.
    std::vector<std::string> abstractions;
    /// Object boxes looked at, abstractions counted once each.
    std::size_t objects = 0;
    [[nodiscard]] bool allowed() const noexcept { return refused.empty(); }
    /// "Unknown object 'foo' in pd lfo, box 3" and so on, one line each.
    [[nodiscard]] std::vector<std::string> describe() const;
};

/// Nesting allowed, subpatches and abstractions together.
inline constexpr int kPdWalkMaxDepth = 64;

/// Abstractions as the device stores them (DEV-10's 'abstractions' role):
/// name to patch text. "voice" and "voice.pd" both name the same one.
using PdAbstractions = std::map<std::string, std::string, std::less<>>;

/// Walks `patchText` and every abstraction it reaches. Message thread or a
/// worker; allocates; runs no Pd.
[[nodiscard]] PdObjectWalk walkPdObjects(std::string_view patchText,
                                         const PdAbstractions& abstractions,
                                         const PdVocabulary& vocabulary);

}  // namespace adi::device
