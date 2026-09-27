// SPDX-License-Identifier: GPL-3.0-or-later
//
// See pd_declarations.hpp for why the parse is static and why there is one
// scanner rather than two.

#include "juce/pd_declarations.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace adi::device {
namespace {

/// One object box, already split into atoms with escapes resolved.
struct Record {
    std::vector<std::string> atoms;
    int box = 0;
};

/// Splits a patch into records and each record into atoms.
///
/// A record ends at an unescaped `;`. Escapes are taken literally: `\$0`
/// becomes `$0`, which is the token a declaration is written with and the one
/// the check below compares against.
std::vector<Record> scan(std::string_view text) {
    std::vector<Record> out;
    std::vector<std::string> atoms;
    std::string atom;
    bool escaped = false;
    int boxes = 0;

    const auto endAtom = [&] {
        if (!atom.empty()) { atoms.push_back(atom); atom.clear(); }
    };
    const auto endRecord = [&] {
        endAtom();
        if (!atoms.empty()) {
            // Only object boxes can declare. Everything else -- canvases,
            // connections, messages, comments -- is skipped here rather than
            // filtered later, so `box` counts what an author would count.
            // `#X obj <x> <y> <class> <args...>` -- the class is atom 4, after
            // the two coordinates, and the arguments start at 5.
            if (atoms.size() >= 5 && atoms[0] == "#X" && atoms[1] == "obj") {
                ++boxes;
                out.push_back(Record{atoms, boxes});
            }
            atoms.clear();
        }
    };

    for (const char c : text) {
        if (escaped) { atom.push_back(c); escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == ';') { endRecord(); continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { endAtom(); continue; }
        atom.push_back(c);
    }
    endRecord();
    return out;
}

bool parseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(v)) return false;
    out = v;
    return true;
}

bool parseId(const std::string& s, std::int32_t& out) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;   // positive, decimal, no sign
    }
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE || end != s.c_str() + s.size()) return false;
    if (v <= 0 || v >= 2147483648LL) return false;   // positive, below 2^31
    out = static_cast<std::int32_t>(v);
    return true;
}

/// `Cutoff_Freq` reads "Cutoff Freq" (ADR-0177 decision 1). One symbol in Pd,
/// because a space would end the atom.
std::string label(const std::string& s) {
    std::string out = s;
    std::replace(out.begin(), out.end(), '_', ' ');
    return out;
}

/// `-` means no unit, and is not a unit called "-".
std::string unitOf(const std::string& s) { return s == "-" ? std::string{} : s; }

}  // namespace

const char* toString(PdDeclProblem p) noexcept {
    switch (p) {
        case PdDeclProblem::NotDollarZero:       return "pd.not_dollar_zero";
        case PdDeclProblem::BadId:               return "pd.bad_id";
        case PdDeclProblem::DuplicateId:         return "pd.duplicate_id";
        case PdDeclProblem::WrongArity:          return "pd.wrong_arity";
        case PdDeclProblem::BadNumber:           return "pd.bad_number";
        case PdDeclProblem::BadRange:            return "pd.bad_range";
        case PdDeclProblem::BadCurve:            return "pd.bad_curve";
        case PdDeclProblem::LogNeedsPositiveMin: return "pd.log_needs_positive_min";
        case PdDeclProblem::MenuItemCount:       return "pd.menu_item_count";
        case PdDeclProblem::BadLength:           return "pd.bad_length";
        case PdDeclProblem::BadRate:             return "pd.bad_rate";
    }
    return "pd.unknown";
}

const PdParamDecl* PdDeclarations::param(std::int32_t id) const noexcept {
    for (const auto& p : params) if (p.id == id) return &p;
    return nullptr;
}
const PdArrayDecl* PdDeclarations::array(std::int32_t id) const noexcept {
    for (const auto& a : arrays) if (a.id == id) return &a;
    return nullptr;
}

std::string pdParamReceiveName(int dollarZero, std::int32_t id) {
    return std::to_string(dollarZero) + "-adi-" + std::to_string(id);
}
std::string pdArrayReceiveName(int dollarZero, std::int32_t id) {
    return std::to_string(dollarZero) + "-adiarr-" + std::to_string(id);
}

std::vector<PdExternalRequest> pdExternalRequests(std::string_view patchText) {
    std::vector<PdExternalRequest> out;
    for (const auto& r : scan(patchText)) {
        if (r.atoms[4] != "declare") continue;
        // `[declare -path a -lib b]` -- flags and their values alternate, and
        // a flag may legally end the box with nothing after it.
        for (std::size_t i = 5; i < r.atoms.size(); ++i) {
            const std::string& a = r.atoms[i];
            if (a != "-path" && a != "-stdpath" && a != "-lib" && a != "-stdlib") continue;
            PdExternalRequest req;
            req.flag = a;
            req.box = r.box;
            if (i + 1 < r.atoms.size() && !r.atoms[i + 1].empty() &&
                r.atoms[i + 1][0] != '-') {
                req.value = r.atoms[i + 1];
            }
            out.push_back(std::move(req));
        }
    }
    return out;
}

PdDeclarations parsePdDeclarations(std::string_view patchText) {
    PdDeclarations out;
    const auto records = scan(patchText);

    const auto fail = [&out](PdDeclProblem p, int box, std::string detail) {
        out.problems.push_back({p, box, std::move(detail)});
    };

    for (const auto& r : records) {
        const std::string& cls = r.atoms[4];
        const bool isParam = (cls == "adi.param");
        const bool isArray = (cls == "adi.array");
        if (!isParam && !isArray) continue;

        // Arguments after the class name.
        const std::vector<std::string> a(r.atoms.begin() + 5, r.atoms.end());

        // Shared, and shared deliberately: `$0` then the id, checked once for
        // both objects. ADR-0177 decision 1's reasoning is identical for an
        // array -- Pd's send/receive names are global within an instance, and
        // a renamed array must not orphan what points at it.
        if (a.empty() || a[0] != "$0") {
            fail(PdDeclProblem::NotDollarZero, r.box,
                 cls + " must be given $0 as its first argument");
            continue;
        }
        std::int32_t id = 0;
        if (a.size() < 2 || !parseId(a[1], id)) {
            fail(PdDeclProblem::BadId, r.box,
                 cls + " needs a positive integer id below 2^31");
            continue;
        }

        if (isArray) {
            // [adi.array $0 <id> <length> <rate> <min> <max> <unit> <name>]
            if (a.size() != 8) {
                fail(PdDeclProblem::WrongArity, r.box,
                     "adi.array takes $0, id, length, rate, min, max, unit, name");
                continue;
            }
            if (out.array(id) != nullptr) {
                fail(PdDeclProblem::DuplicateId, r.box,
                     "array id " + a[1] + " is already declared; this one is ignored");
                continue;
            }
            PdArrayDecl d;
            d.id = id;
            d.box = r.box;

            std::int32_t len = 0;
            if (!parseId(a[2], len) || len > PdArrayDecl::kMaxLength) {
                fail(PdDeclProblem::BadLength, r.box,
                     "length must be a positive integer up to " +
                         std::to_string(PdArrayDecl::kMaxLength));
                continue;
            }
            d.length = len;

            if (!parseNumber(a[3], d.rate) || d.rate <= 0.0 || d.rate > PdArrayDecl::kMaxRate) {
                fail(PdDeclProblem::BadRate, r.box,
                     "rate must be a positive number of hertz up to " +
                         std::to_string(static_cast<int>(PdArrayDecl::kMaxRate)));
                continue;
            }
            if (!parseNumber(a[4], d.min) || !parseNumber(a[5], d.max)) {
                fail(PdDeclProblem::BadNumber, r.box, "min and max must be numbers");
                continue;
            }
            if (!(d.min < d.max)) {
                fail(PdDeclProblem::BadRange, r.box, "min must be below max");
                continue;
            }
            d.unit = unitOf(a[6]);
            d.name = label(a[7]);
            out.arrays.push_back(std::move(d));
            continue;
        }

        // [adi.param $0 <id> <min> <max> <default> <unit> <curve> <name> [items]]
        if (a.size() < 8) {
            fail(PdDeclProblem::WrongArity, r.box,
                 "adi.param takes $0, id, min, max, default, unit, curve, name");
            continue;
        }
        if (out.param(id) != nullptr) {
            fail(PdDeclProblem::DuplicateId, r.box,
                 "param id " + a[1] + " is already declared; this one is ignored");
            continue;
        }
        PdParamDecl d;
        d.id = id;
        d.box = r.box;
        if (!parseNumber(a[2], d.min) || !parseNumber(a[3], d.max) ||
            !parseNumber(a[4], d.def)) {
            fail(PdDeclProblem::BadNumber, r.box, "min, max and default must be numbers");
            continue;
        }
        if (!(d.min < d.max)) {
            fail(PdDeclProblem::BadRange, r.box, "min must be below max");
            continue;
        }
        if (d.def < d.min || d.def > d.max) {
            fail(PdDeclProblem::BadRange, r.box, "the default is outside min..max");
            continue;
        }
        d.unit = unitOf(a[5]);

        const std::string& curve = a[6];
        if (curve == "lin")          d.curve = PdCurve::Lin;
        else if (curve == "log")     d.curve = PdCurve::Log;
        else if (curve == "int")     d.curve = PdCurve::Int;
        else if (curve == "toggle")  d.curve = PdCurve::Toggle;
        else if (curve == "menu")    d.curve = PdCurve::Menu;
        else {
            double e = 0.0;
            if (!parseNumber(curve, e) || e <= 0.0) {
                fail(PdDeclProblem::BadCurve, r.box,
                     "curve must be lin, log, int, toggle, menu, or a positive exponent");
                continue;
            }
            d.curve = PdCurve::Exponent;
            d.exponent = e;
        }
        if (d.curve == PdCurve::Log && d.min <= 0.0) {
            fail(PdDeclProblem::LogNeedsPositiveMin, r.box,
                 "a log curve needs min above 0");
            continue;
        }
        d.name = label(a[7]);

        if (d.curve == PdCurve::Menu) {
            // ADR-0177 decision 1: min is 0 and there are max + 1 items.
            const std::size_t want =
                (d.min == 0.0 && d.max >= 0.0) ? static_cast<std::size_t>(d.max) + 1 : 0;
            const std::size_t got = a.size() - 8;
            if (want == 0 || got != want) {
                fail(PdDeclProblem::MenuItemCount, r.box,
                     "a menu needs min 0 and max + 1 items; got " + std::to_string(got));
                continue;
            }
            for (std::size_t i = 8; i < a.size(); ++i) d.items.push_back(label(a[i]));
        } else if (a.size() > 8) {
            fail(PdDeclProblem::WrongArity, r.box,
                 "only a menu takes items after the name");
            continue;
        }
        out.params.push_back(std::move(d));
    }
    return out;
}

}  // namespace adi::device
