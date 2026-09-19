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

std::string renderPosition(std::int64_t ticks, const std::vector<Meter>& meters) {
    const Meter kDefault{};                       // 4/4 from zero
    const Meter* cur = &kDefault;
    std::int64_t bars_before = 0;                 // bars completed before `cur`
    std::int64_t seg_start = 0;

    // Walk the segments. Bar numbering ACCUMULATES across signature changes:
    // dividing the whole position by the current meter is correct up to the
    // first change and wrong after it -- the same shape of bug as integrating
    // tempo with a single bpm, which the engine tests already guard against.
    for (std::size_t i = 0; i < meters.size(); ++i) {
        const Meter& m = meters[i];
        if (m.start_ticks > ticks) break;
        if (i > 0) {
            const std::int64_t bt = cur->barTicks();
            if (bt > 0) bars_before += (m.start_ticks - seg_start) / bt;
        }
        cur = &m;
        seg_start = m.start_ticks;
    }

    const std::int64_t bar_ticks = cur->barTicks();
    if (bar_ticks <= 0) return "0|0|0";           // a 0 denominator: total, not UB

    const std::int64_t into = ticks - seg_start;
    std::int64_t bar = bars_before + into / bar_ticks;
    std::int64_t rem = into % bar_ticks;
    if (rem < 0) { --bar; rem += bar_ticks; }     // floor, so negatives behave

    const std::int64_t beat_ticks =
        kWhole / static_cast<std::int64_t>(cur->denominator);
    const std::int64_t beat = beat_ticks > 0 ? rem / beat_ticks : 0;
    const std::int64_t tick = beat_ticks > 0 ? rem % beat_ticks : rem;

    return std::to_string(bar + 1) + "|" + std::to_string(beat + 1) + "|"
         + std::to_string(tick);
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

namespace {

/// The body of a quoted string, without the quotes. `escape_slash` is set when
/// the result goes inside a designator, where an unescaped `/` would forge a
/// path boundary.
void appendEscaped(std::string& out, std::string_view s, bool escape_slash) {
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
                if (escape_slash && sc.cp == '/') appendHex(out, sc.cp, "\\u{");
                else if (mustEscape(sc.cp))       appendHex(out, sc.cp, "\\u{");
                else                              out.append(s.substr(i, sc.len));
        }
        i += sc.len;
    }
}

}  // namespace

std::string quoteString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    appendEscaped(out, s, /*escape_slash=*/false);
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
// Designators (TEXT-PROJECTION 6.2 - 6.4)
// ---------------------------------------------------------------------------

std::string_view roleRoot(TrackRole r) {
    switch (r) {
        case TrackRole::Track:  return "trk";
        case TrackRole::Return: return "ret";
        case TrackRole::Vca:    return "vca";
        case TrackRole::Master: return "master";
        case TrackRole::Global: return "glob";
    }
    return "trk";
}

TrackRole roleOfKind(std::string_view kind) {
    if (kind == "return") return TrackRole::Return;
    if (kind == "master") return TrackRole::Master;
    if (kind == "vca")    return TrackRole::Vca;
    // The timeline's global lanes. SPEC 6.1 lists these as track kinds, but
    // they carry no audio and nothing sends to them, so keeping them out of
    // /trk leaves that rank space to the tracks a user actually addresses.
    if (kind == "marker" || kind == "tempo" || kind == "signature"
        || kind == "chord" || kind == "arranger" || kind == "transposition")
        return TrackRole::Global;
    // audio, midi, instrument, group, folder, video -- and anything a newer
    // writer invents. Guessing Track keeps the projection total across a
    // version boundary; refusing would lose a file we can otherwise render.
    return TrackRole::Track;
}

std::string designator(const std::vector<std::string>& segments) {
    std::string out;
    out += '"';
    for (const auto& seg : segments) {
        out += '/';
        appendEscaped(out, seg, /*escape_slash=*/true);
    }
    out += '"';
    return out;
}

std::string unresolved(std::string_view kind) {
    std::string out = "\"!unresolved(";
    appendEscaped(out, kind, /*escape_slash=*/true);
    out += ")\"";
    return out;
}

std::vector<std::string> mediaPrefixes(const std::vector<std::string>& hashes) {
    std::size_t longest = 0;
    for (const auto& h : hashes) longest = (std::max)(longest, h.size());

    // Grow in steps of 4 from the minimum until every DISTINCT hash has a
    // distinct prefix. Equal hashes stay equal -- idx_media_hash is not unique,
    // so that case is legal and is left to assignLabels.
    std::size_t len = kMediaPrefixMin;
    for (; len < longest; len += 4) {
        std::vector<std::string> pre;
        pre.reserve(hashes.size());
        for (const auto& h : hashes) pre.push_back(h.substr(0, (std::min)(len, h.size())));

        std::vector<std::string> full = hashes, cut = pre;
        std::sort(full.begin(), full.end());
        full.erase(std::unique(full.begin(), full.end()), full.end());
        std::sort(cut.begin(), cut.end());
        cut.erase(std::unique(cut.begin(), cut.end()), cut.end());
        if (full.size() == cut.size()) break;    // the prefix separates as well
    }                                            // as the whole hash does

    std::vector<std::string> out;
    out.reserve(hashes.size());
    for (const auto& h : hashes) out.push_back(h.substr(0, (std::min)(len, h.size())));
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
            } else if (render == nullptr) {
                // Found by the fuzzer, on its first campaign, as a violation of
                // the one property this module exists to guarantee.
                //
                // The sorts below are `stable_sort`, and stable means a tie
                // keeps its INPUT order -- which is rowid order, which is the
                // leak. K4 is what removes that dependence, and K4 needs to
                // render to do it. Without a renderer a surviving tie is simply
                // not resolvable: refinement is incomplete (two members can
                // share a refined colour and still render differently), so we
                // cannot certify that swapping them is unobservable.
                //
                // Reporting Ambiguous is the honest answer. The alternative --
                // leaving them in input order and calling it Exact -- is how a
                // projection ends up merely usually canonical, which breaks
                // ADR-0021's oracle silently instead of failing it.
                res.status = OrderStatus::Ambiguous;
            } else {
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

// ---------------------------------------------------------------------------
// The pipeline (TEXT-PROJECTION 9.1)
// ---------------------------------------------------------------------------

namespace {

struct Pass {
    const Tree& tree;
    std::vector<std::string> skeleton;                 // per node
    std::vector<std::vector<std::size_t>> ordered;     // per node, children in order
    std::vector<std::string> label;                    // per node, after ~k / #n
    std::vector<std::vector<std::string>> path;        // per node, designator segments
    std::vector<std::string> designator_of;            // per node, quoted
    OrderStatus status = OrderStatus::Exact;
    std::size_t largest_tied = 1;

    explicit Pass(const Tree& t)
        : tree(t), skeleton(t.nodes.size()), ordered(t.nodes.size()),
          label(t.nodes.size()), path(t.nodes.size()),
          designator_of(t.nodes.size()) {}

    // --- pass 1 ------------------------------------------------------------
    // Bottom-up, because a node's skeleton contains its children's skeletons,
    // so the children have to be ordered before the parent can be rendered.
    // Cross-references are `?`: that is the whole trick, and it is why this
    // terminates when a definition in terms of the final text would not.
    void buildSkeletons(std::size_t n, std::vector<char>& visiting) {
        if (!skeleton[n].empty()) return;
        if (visiting[n]) {                      // a containment cycle; the
            skeleton[n] = "<cycle>";            // schema does not forbid one
            return;
        }
        visiting[n] = 1;

        const Node& node = tree.nodes[n];
        for (std::size_t c : node.children) buildSkeletons(c, visiting);

        // Order each child COLLECTION separately -- clips among clips, devices
        // among devices. A clip and a device never compete for a position, and
        // their designators differ by selector anyway.
        std::map<std::string, std::vector<std::size_t>> by_kind;
        for (std::size_t c : node.children) by_kind[tree.nodes[c].kind].push_back(c);

        std::vector<std::size_t> flat;
        for (auto& [kind, group] : by_kind) {
            std::vector<Member> ms;
            ms.reserve(group.size());
            for (std::size_t c : group) {
                Member m;
                m.keys = tree.nodes[c].keys;
                m.skeleton = skeleton[c];
                ms.push_back(std::move(m));
            }
            // Edges between siblings let refinement speak. A reference to a
            // non-sibling cannot separate two siblings at this level, and is
            // left to K2, which already contains it as `?` in both.
            for (std::size_t i = 0; i < group.size(); ++i) {
                for (const Ref& r : tree.nodes[group[i]].refs) {
                    const auto at = std::find(group.begin(), group.end(), r.target);
                    if (at == group.end()) continue;
                    const auto j = static_cast<std::uint32_t>(at - group.begin());
                    ms[i].out_refs.push_back(j);
                    ms[j].in_refs.push_back(static_cast<std::uint32_t>(i));
                }
            }

            const OrderResult res = canonicalOrder(ms);
            if (res.status == OrderStatus::Ambiguous) status = OrderStatus::Ambiguous;
            largest_tied = (std::max)(largest_tied, res.largest_tied_class);
            for (std::uint32_t idx : res.order) flat.push_back(group[idx]);
        }
        ordered[n] = flat;

        std::string sk = node.kind;
        sk += '\x1f';
        sk += node.base_label;
        for (const auto& a : node.attrs) { sk += '\x1f'; sk += a; }
        for (const Ref& r : node.refs) { sk += '\x1f'; sk += r.role; sk += "=?"; }
        for (std::size_t c : flat) { sk += '\x1e'; sk += skeleton[c]; }
        skeleton[n] = sk;

        visiting[n] = 0;
    }

    // --- pass 2 ------------------------------------------------------------
    // Top-down. Every order is fixed now, so labels are determined, and a
    // label plus a parent path is a designator.
    void assign(const std::vector<std::size_t>& group,
                const std::vector<std::string>& parent_path) {
        std::map<std::string, std::vector<std::size_t>> by_kind;
        for (std::size_t c : group) by_kind[tree.nodes[c].kind].push_back(c);

        for (auto& [kind, members] : by_kind) {
            std::vector<std::string> bases;
            bases.reserve(members.size());
            for (std::size_t c : members) bases.push_back(tree.nodes[c].base_label);
            const std::vector<std::string> labels = assignLabels(bases);

            for (std::size_t i = 0; i < members.size(); ++i) {
                const std::size_t c = members[i];
                label[c] = labels[i];

                std::vector<std::string> p = parent_path;
                if (!tree.nodes[c].selector.empty()) p.push_back(tree.nodes[c].selector);
                p.push_back(labels[i]);
                path[c] = p;
                designator_of[c] = designator(p);

                assign(ordered[c], p);
            }
        }
    }

    // --- render ------------------------------------------------------------
    void emit(std::size_t n, int depth, std::string& out) const {
        const Node& node = tree.nodes[n];
        const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');

        out += pad;
        out += node.kind;
        out += ' ';
        out += renderLabelToken(label[n]);
        out += '\n';

        for (const auto& a : node.attrs) { out += pad; out += "  "; out += a; out += '\n'; }

        for (const Ref& r : node.refs) {
            out += pad;
            out += "  ";
            out += r.role;
            out += " -> ";
            out += (r.target == Ref::kDangling || r.target >= tree.nodes.size())
                   ? unresolved(r.target_kind)
                   : designator_of[r.target];
            out += '\n';
        }

        for (std::size_t c : ordered[n]) emit(c, depth + 1, out);
    }
};

}  // namespace

Projection project(const Tree& tree) {
    Pass p(tree);
    Projection out;

    std::vector<char> visiting(tree.nodes.size(), 0);
    for (std::size_t r : tree.roots) p.buildSkeletons(r, visiting);

    // The roots are a collection too, and they are ordered by the same rules.
    {
        std::vector<Member> ms;
        ms.reserve(tree.roots.size());
        for (std::size_t r : tree.roots) {
            Member m;
            m.keys = tree.nodes[r].keys;
            m.skeleton = p.skeleton[r];
            ms.push_back(std::move(m));
        }
        for (std::size_t i = 0; i < tree.roots.size(); ++i) {
            for (const Ref& r : tree.nodes[tree.roots[i]].refs) {
                const auto at = std::find(tree.roots.begin(), tree.roots.end(), r.target);
                if (at == tree.roots.end()) continue;
                const auto j = static_cast<std::uint32_t>(at - tree.roots.begin());
                ms[i].out_refs.push_back(j);
                ms[j].in_refs.push_back(static_cast<std::uint32_t>(i));
            }
        }
        const OrderResult res = canonicalOrder(ms);
        if (res.status == OrderStatus::Ambiguous) p.status = OrderStatus::Ambiguous;
        p.largest_tied = (std::max)(p.largest_tied, res.largest_tied_class);

        std::vector<std::size_t> roots_in_order;
        roots_in_order.reserve(tree.roots.size());
        for (std::uint32_t idx : res.order) roots_in_order.push_back(tree.roots[idx]);

        p.assign(roots_in_order, {});
        for (std::size_t r : roots_in_order) p.emit(r, 0, out.text);
    }

    out.status = p.status;
    out.largest_tied_class = p.largest_tied;
    out.designators = p.designator_of;
    return out;
}

}  // namespace adi::textproj
