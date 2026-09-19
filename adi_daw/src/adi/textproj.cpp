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

// ---------------------------------------------------------------------------
// Ordering (TEXT-PROJECTION 7)
// ---------------------------------------------------------------------------

SortKey SortKey::integer(std::int64_t v) { SortKey k; k.tag_ = Tag::Int;  k.i_ = v; return k; }
SortKey SortKey::real(double v)          { SortKey k; k.tag_ = Tag::Real; k.d_ = v; return k; }
SortKey SortKey::text(std::string v)     { SortKey k; k.tag_ = Tag::Text; k.s_ = std::move(v); return k; }
SortKey SortKey::null()                  { return SortKey{}; }

std::string SortKey::seedToken() const {
    switch (tag_) {
        case Tag::Null: return "n";
        case Tag::Int:  return "i" + std::to_string(i_);
        case Tag::Real: {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &d_, sizeof bits);   // the bit pattern, so that
            return "r" + std::to_string(bits);      // -0.0 and +0.0 differ
        }
        case Tag::Text: return "t" + std::to_string(s_.size()) + ":" + s_;
    }
    return "n";
}

int SortKey::compare(const SortKey& o) const {
    if (tag_ != o.tag_)
        return static_cast<int>(tag_) < static_cast<int>(o.tag_) ? -1 : 1;
    switch (tag_) {
        case Tag::Null: return 0;
        case Tag::Int:  return i_ < o.i_ ? -1 : (i_ > o.i_ ? 1 : 0);
        case Tag::Text: {
            const int c = s_.compare(o.s_);
            return c < 0 ? -1 : (c > 0 ? 1 : 0);
        }
        case Tag::Real: {
            // A total order over every bit pattern a REAL column can hold.
            // `<` is not one: NaN compares false against everything and -0.0 ==
            // +0.0, so a comparator built on it is not a strict weak ordering
            // and std::sort on it is undefined behaviour, not merely wrong.
            const bool an = std::isnan(d_), bn = std::isnan(o.d_);
            if (an || bn) return an && bn ? 0 : (an ? 1 : -1);   // NaN sorts last
            if (d_ < o.d_) return -1;
            if (d_ > o.d_) return 1;
            // Equal by value: separate -0.0 from +0.0 by sign bit.
            const bool as = std::signbit(d_), bs = std::signbit(o.d_);
            return as == bs ? 0 : (as ? -1 : 1);
        }
    }
    return 0;
}

namespace {

int compareKeys(const std::vector<SortKey>& a, const std::vector<SortKey>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (const int c = a[i].compare(b[i])) return c;
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

/// Map each distinct signature to its rank among the sorted distinct
/// signatures. The rank is therefore a function of the signature set alone --
/// never a counter incremented in traversal order, which would make the colour
/// depend on the order we happened to visit members in.
std::vector<std::size_t> rankOf(const std::vector<std::string>& sigs) {
    std::vector<std::string> distinct = sigs;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());

    std::vector<std::size_t> out(sigs.size());
    for (std::size_t i = 0; i < sigs.size(); ++i)
        out[i] = static_cast<std::size_t>(
            std::lower_bound(distinct.begin(), distinct.end(), sigs[i]) - distinct.begin());
    return out;
}

std::string joinSorted(const std::vector<std::uint32_t>& refs,
                       const std::vector<std::size_t>& colour) {
    std::vector<std::size_t> cs;
    cs.reserve(refs.size());
    for (std::uint32_t r : refs)
        if (r < colour.size()) cs.push_back(colour[r]);
    std::sort(cs.begin(), cs.end());          // a multiset, not a sequence
    std::string out;
    for (std::size_t c : cs) { out += std::to_string(c); out += ','; }
    return out;
}

}  // namespace

OrderResult canonicalOrder(const std::vector<Member>& members,
                           RenderFn render,
                           void* ctx,
                           std::size_t max_tied_class) {
    const std::size_t n = members.size();
    OrderResult res;
    res.order.resize(n);
    for (std::size_t i = 0; i < n; ++i) res.order[i] = static_cast<std::uint32_t>(i);
    if (n < 2) return res;

    // --- K1 and K2 ---------------------------------------------------------
    std::stable_sort(res.order.begin(), res.order.end(),
                     [&](std::uint32_t a, std::uint32_t b) {
                         if (const int c = compareKeys(members[a].keys, members[b].keys))
                             return c < 0;
                         return members[a].skeleton < members[b].skeleton;
                     });

    auto tiedWith = [&](std::uint32_t a, std::uint32_t b) {
        return compareKeys(members[a].keys, members[b].keys) == 0
               && members[a].skeleton == members[b].skeleton;
    };

    // --- K3: refinement to a fixed point -----------------------------------
    // Seed each member's colour from what K1 and K2 already separated, then
    // repeatedly fold in the multiset of neighbour colours, in BOTH directions.
    // Two tracks identical in every field are still distinguishable if
    // different things send to them, and only the in-edges carry that.
    std::vector<std::string> seed(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::string k;
        for (const auto& key : members[i].keys) { k += key.seedToken(); k += ':'; }
        seed[i] = k + '|' + members[i].skeleton;
    }
    std::vector<std::size_t> colour = rankOf(seed);

    for (std::size_t round = 0; round < n; ++round) {
        std::vector<std::string> sig(n);
        for (std::size_t i = 0; i < n; ++i)
            sig[i] = std::to_string(colour[i]) + '>' + joinSorted(members[i].out_refs, colour)
                                               + '<' + joinSorted(members[i].in_refs, colour);
        std::vector<std::size_t> next = rankOf(sig);
        if (next == colour) break;            // fixed point
        colour = std::move(next);
    }

    // Re-sort the K1/K2-tied runs by refined colour. Members that K1 and K2
    // already separated keep their positions: refinement may only break ties,
    // never reorder across them.
    std::stable_sort(res.order.begin(), res.order.end(),
                     [&](std::uint32_t a, std::uint32_t b) {
                         if (const int c = compareKeys(members[a].keys, members[b].keys))
                             return c < 0;
                         if (members[a].skeleton != members[b].skeleton)
                             return members[a].skeleton < members[b].skeleton;
                         return colour[a] < colour[b];
                     });

    // --- K4: permutation minimisation over each surviving class ------------
    std::size_t p = 0;
    while (p < n) {
        std::size_t q = p + 1;
        while (q < n && tiedWith(res.order[p], res.order[q])
                     && colour[res.order[p]] == colour[res.order[q]]) ++q;

        const std::size_t size = q - p;
        res.largest_tied_class = std::max(res.largest_tied_class, size);

        if (size > 1) {
            if (size > max_tied_class) {
                // Refusing beats inventing. An arbitrary order here would be a
                // leak wearing a canonical hat, and the caller must be able to
                // tell the difference.
                res.status = OrderStatus::Ambiguous;
            } else if (render != nullptr) {
                // Minimise over the OUTPUT, not over any property of the
                // members. That is what makes this canonical rather than a
                // choice of member: members that tie through K4 render
                // identically, so which is first is unobservable.
                std::vector<std::uint32_t> klass(res.order.begin() + static_cast<std::ptrdiff_t>(p),
                                                 res.order.begin() + static_cast<std::ptrdiff_t>(q));
                std::sort(klass.begin(), klass.end());

                std::vector<std::uint32_t> best_class, cand = res.order;
                std::string best;
                do {
                    for (std::size_t i = 0; i < size; ++i) cand[p + i] = klass[i];
                    std::string out = render(cand, ctx);
                    if (best.empty() && best_class.empty()) { best = std::move(out); best_class = klass; }
                    else if (out < best)                    { best = std::move(out); best_class = klass; }
                } while (std::next_permutation(klass.begin(), klass.end()));

                for (std::size_t i = 0; i < size; ++i) res.order[p + i] = best_class[i];
            }
        }
        p = q;
    }
    return res;
}

}  // namespace adi::textproj
