// SPDX-License-Identifier: GPL-3.0-or-later
//
// Canonical text projection — the pure renderers. See textproj.hpp for why
// these are separated from anything that touches a database.

#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <system_error>

namespace adi::textproj {
namespace {

// --- numbers ---------------------------------------------------------------

/// Pin the shape std::to_chars leaves open. Two libraries agreeing today is not
/// the same as the format being specified: `-0` and `1e+308` are both valid
/// shortest round-trip renderings, and both are wrong for a canonical form.
std::string canonicaliseFloat(std::string s) {
    const std::size_t e = s.find('e');

    if (e == std::string::npos) {
        // No exponent: guarantee a decimal point so a float never renders as a
        // bare integer, and so -0.0 is visibly distinct from 0.0.
        if (s.find('.') == std::string::npos) s += ".0";
        return s;
    }

    std::string mant = s.substr(0, e);
    std::string exp  = s.substr(e + 1);
    if (mant.find('.') == std::string::npos) mant += ".0";

    bool neg = false;
    if (!exp.empty() && (exp[0] == '+' || exp[0] == '-')) {
        neg = exp[0] == '-';
        exp.erase(0, 1);
    }
    const std::size_t nz = exp.find_first_not_of('0');
    exp = (nz == std::string::npos) ? "0" : exp.substr(nz);

    return mant + "e" + (neg ? "-" : "") + exp;
}

template <typename T>
std::string shortestRoundTrip(T v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";

    std::array<char, 64> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if (res.ec != std::errc{}) return "nan";   // unreachable for finite T
    return canonicaliseFloat(std::string(buf.data(), res.ptr));
}

// --- UTF-8 -----------------------------------------------------------------

/// Decode one scalar. Returns the scalar and its length, or length 0 when the
/// bytes at `i` are not a valid, shortest-form, non-surrogate encoding.
struct Scalar { std::uint32_t cp; std::size_t len; };

Scalar decodeUtf8(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const std::size_t avail = s.size() - i;

    auto cont = [&](std::size_t k) {
        return k < avail && (static_cast<unsigned char>(s[i + k]) & 0xC0u) == 0x80u;
    };
    auto bits = [&](std::size_t k) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    };

    if (b0 < 0x80u) return {b0, 1};
    if ((b0 & 0xE0u) == 0xC0u && cont(1)) {
        const std::uint32_t cp = ((b0 & 0x1Fu) << 6) | bits(1);
        return cp >= 0x80u ? Scalar{cp, 2} : Scalar{0, 0};          // overlong
    }
    if ((b0 & 0xF0u) == 0xE0u && cont(1) && cont(2)) {
        const std::uint32_t cp = ((b0 & 0x0Fu) << 12) | (bits(1) << 6) | bits(2);
        if (cp < 0x800u) return {0, 0};                              // overlong
        if (cp >= 0xD800u && cp <= 0xDFFFu) return {0, 0};           // surrogate
        return {cp, 3};
    }
    if ((b0 & 0xF8u) == 0xF0u && cont(1) && cont(2) && cont(3)) {
        const std::uint32_t cp = ((b0 & 0x07u) << 18) | (bits(1) << 12)
                               | (bits(2) << 6) | bits(3);
        return (cp >= 0x10000u && cp <= 0x10FFFFu) ? Scalar{cp, 4} : Scalar{0, 0};
    }
    return {0, 0};
}

/// The MUST-escape set. See textproj.hpp for why bidi controls are in it.
bool mustEscape(std::uint32_t cp) {
    if (cp == '"' || cp == '\\') return true;
    if (cp <= 0x1Fu || cp == 0x7Fu) return true;                 // C0 + DEL
    if (cp >= 0x80u && cp <= 0x9Fu) return true;                 // C1
    if (cp == 0x2028u || cp == 0x2029u || cp == 0xFEFFu) return true;
    if (cp == 0x061Cu || cp == 0x200Eu || cp == 0x200Fu) return true;
    if (cp >= 0x202Au && cp <= 0x202Eu) return true;
    if (cp >= 0x2066u && cp <= 0x2069u) return true;
    return false;
}

void appendHex(std::string& out, std::uint32_t v, const char* open) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    out += open;
    char tmp[8];
    int n = 0;
    do { tmp[n++] = kHex[v & 0xFu]; v >>= 4; } while (v != 0);
    while (n > 0) out += tmp[--n];
    out += '}';
}

}  // namespace

// ---------------------------------------------------------------------------

std::string renderF64(double v) { return shortestRoundTrip(v); }
std::string renderF32(float v)  { return shortestRoundTrip(v); }

std::string renderDuration(std::int64_t ticks) {
    // Total by construction: dur_ticks > 0 is a spec rule for writers, not a
    // guarantee about a file we are handed.
    if (ticks <= 0) return std::to_string(ticks) + "t";

    const std::int64_t g = std::gcd(ticks, kWhole);
    const std::int64_t num = ticks / g;
    const std::int64_t den = kWhole / g;

    if (den > kMaxDenominator) return std::to_string(ticks) + "t";
    return std::to_string(num) + "/" + std::to_string(den);
}

bool isBareSafe(std::string_view s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size();) {
        const Scalar sc = decodeUtf8(s, i);
        if (sc.len == 0) return false;
        if (mustEscape(sc.cp)) return false;
        // Structural characters of the grammar, plus space.
        if (sc.cp == ' ' || sc.cp == '/' || sc.cp == '#' || sc.cp == '~'
            || sc.cp == '@' || sc.cp == '?') return false;
        i += sc.len;
    }
    return true;
}

std::string quoteString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (std::size_t i = 0; i < s.size();) {
        const Scalar sc = decodeUtf8(s, i);
        if (sc.len == 0) {
            // Not valid UTF-8. Escaping the raw byte keeps the projector total
            // and injective; U+FFFD would silently merge two distinct names.
            appendHex(out, static_cast<unsigned char>(s[i]), "\\x{");
            ++i;
            continue;
        }
        switch (sc.cp) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (mustEscape(sc.cp)) appendHex(out, sc.cp, "\\u{");
                else                   out.append(s.substr(i, sc.len));
        }
        i += sc.len;
    }
    out += '"';
    return out;
}

std::string renderLabelToken(std::string_view s) {
    return isBareSafe(s) ? std::string(s) : quoteString(s);
}

std::vector<std::string> assignLabels(const std::vector<std::string>& base_labels) {
    const std::size_t n = base_labels.size();
    std::vector<std::string> out(n);

    // Empty base label -> `#rank`, at the member's 0-based rank in the
    // container's already-fixed order.
    for (std::size_t i = 0; i < n; ++i)
        out[i] = base_labels[i].empty() ? ("#" + std::to_string(i)) : base_labels[i];

    // Count, then suffix every member of a colliding group -- the first
    // included, so a collision reads as a collision.
    std::map<std::string, int> total;
    for (const auto& l : out) ++total[l];

    std::map<std::string, int> seen;
    for (std::size_t i = 0; i < n; ++i) {
        const auto it = total.find(out[i]);
        if (it->second > 1) {
            const int k = ++seen[out[i]];
            out[i] += "~" + std::to_string(k);
        }
    }
    return out;
}

}  // namespace adi::textproj
