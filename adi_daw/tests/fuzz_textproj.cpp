// SPDX-License-Identifier: GPL-3.0-or-later
//
// libFuzzer harness for the canonical text projection — ADR-0007.
//
// The reader (fuzz_blob.cpp) parses bytes it did not write, so its property is
// memory safety. This one is different: the projection's inputs come from our
// own database, so crashing is the least interesting thing that can go wrong.
// What can actually go wrong is that the output stops being canonical, and a
// projection that is merely *usually* canonical silently breaks ADR-0021's
// replay oracle rather than failing it.
//
// So the properties here are about the OUTPUT, not about memory:
//
//   P1  PERMUTATION INVARIANCE. Present the same collection in a different
//       input order -- which is what a different rowid assignment, insertion
//       history or query plan looks like -- and the output sequence must be
//       identical. This is the anti-leak property stated directly.
//   P2  K1 DOMINANCE. Refinement may only break ties, never reorder across
//       them. Two members whose declared keys differ must appear in key order
//       no matter what the reference graph says.
//   P3  VALID PERMUTATION. The result is every input index exactly once.
//   P4  IDEMPOTENCE. Running twice on the same input gives the same answer.
//   P5  STATUS HONESTY. Ambiguous is reported when, and only when, a tie
//       survived that K4 could not resolve -- which, with no renderer passed,
//       is any tie at all.
//   P7  PATH INTEGRITY. A designator contains exactly one `/` per segment,
//       for any segment bytes. A name that can contribute a literal `/` can
//       forge a path boundary and make a send appear to target another track.
//   P8  MEDIA PREFIXES. Uniform length, and they separate exactly as well as
//       the whole hashes do -- no more, which would be diff noise, and no
//       less, which would merge two media files.
//   P6  ESCAPE CLOSURE. A quoted string contains no raw LF, no raw control
//       character and no bidi control, for ANY input bytes. Nesting is by
//       indentation with no closing delimiter, so a name that can emit a raw
//       newline can forge a block; a name that can emit U+202E can make a diff
//       hunk display as something other than what it says.
//
// A bug I shipped in the first draft of the ordering code is the reason P2 is
// here and the reason P1 alone would not be enough: seeding refinement from a
// three-valued comparison collapsed every distinct integer key into one colour.
// That output was still deterministic and still permutation-invariant. It was
// simply weaker than specified, and no property about determinism alone would
// ever have noticed.

#include "adi/textproj.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace {

using namespace adi::textproj;

/// A deterministic reader over the fuzz input. Deliberately not a PRNG: every
/// decision has to be a function of the input bytes so a crash reproduces from
/// the artifact alone.
class Bytes {
public:
    Bytes(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}

    std::uint8_t u8() { return i_ < n_ ? d_[i_++] : 0; }
    std::size_t below(std::size_t bound) { return bound ? u8() % bound : 0; }
    bool empty() const { return i_ >= n_; }

    std::int64_t i64() {
        std::uint64_t v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8) | u8();
        return static_cast<std::int64_t>(v);
    }
    double f64() {
        std::uint64_t bits = 0;
        for (int k = 0; k < 8; ++k) bits = (bits << 8) | u8();
        double out = 0.0;
        std::memcpy(&out, &bits, sizeof out);
        return out;   // NaN, -0.0 and denormals are all wanted here
    }
    std::string text(std::size_t max_len) {
        const std::size_t len = below(max_len + 1);
        std::string s;
        s.reserve(len);
        for (std::size_t k = 0; k < len; ++k) s.push_back(static_cast<char>(u8()));
        return s;   // arbitrary bytes, including invalid UTF-8
    }

private:
    const std::uint8_t* d_;
    std::size_t n_;
    std::size_t i_ = 0;
};

constexpr std::size_t kMaxMembers = 12;   // K4 is factorial; keep runs fast
constexpr std::size_t kMaxKeys    = 3;

SortKey makeKey(Bytes& b) {
    switch (b.u8() % 4) {
        case 0:  return SortKey::integer(b.i64());
        case 1:  return SortKey::real(b.f64());
        case 2:  return SortKey::text(b.text(8));
        default: return SortKey::null();
    }
}

std::vector<Member> buildCollection(Bytes& b) {
    const std::size_t n = 1 + b.below(kMaxMembers);
    const std::size_t nkeys = b.below(kMaxKeys + 1);

    std::vector<Member> ms(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < nkeys; ++k) ms[i].keys.push_back(makeKey(b));
        // A small skeleton alphabet makes ties common, which is the interesting
        // region: a collection where everything is distinct never reaches K3.
        ms[i].skeleton = std::string(1, static_cast<char>('a' + (b.u8() % 3)));
    }
    // Edges. Both directions matter -- two members identical in every field are
    // still distinguishable by what cites them.
    const std::size_t edges = b.below(n * 2 + 1);
    for (std::size_t e = 0; e < edges; ++e) {
        const std::size_t from = b.below(n), to = b.below(n);
        ms[from].out_refs.push_back(static_cast<std::uint32_t>(to));
        ms[to].in_refs.push_back(static_cast<std::uint32_t>(from));
    }
    return ms;
}

/// Re-present the same collection in a different input order, rewriting every
/// edge so the graph is unchanged. `perm[p]` is the old index now at position p.
std::vector<Member> repermute(const std::vector<Member>& src,
                              const std::vector<std::uint32_t>& perm) {
    std::vector<std::uint32_t> where(src.size());
    for (std::uint32_t p = 0; p < perm.size(); ++p) where[perm[p]] = p;

    std::vector<Member> out;
    out.reserve(src.size());
    for (std::uint32_t p = 0; p < perm.size(); ++p) {
        Member m = src[perm[p]];
        for (auto& r : m.out_refs) r = where[r];
        for (auto& r : m.in_refs)  r = where[r];
        out.push_back(std::move(m));
    }
    return out;
}

/// The members in output order, identified by their ORIGINAL index, so two
/// runs over differently-ordered inputs are directly comparable.
std::vector<std::uint32_t> identitySequence(const OrderResult& r,
                                            const std::vector<std::uint32_t>& perm) {
    std::vector<std::uint32_t> out;
    out.reserve(r.order.size());
    for (std::uint32_t pos : r.order) out.push_back(perm[pos]);
    return out;
}

void checkOrdering(Bytes& b) {
    const std::vector<Member> base = buildCollection(b);
    const std::size_t n = base.size();

    std::vector<std::uint32_t> identity(n);
    std::iota(identity.begin(), identity.end(), 0u);

    const OrderResult r0 = canonicalOrder(base);

    // P3 -- a valid permutation of the input.
    if (r0.order.size() != n) __builtin_trap();
    {
        std::vector<std::uint32_t> sorted = r0.order;
        std::sort(sorted.begin(), sorted.end());
        if (sorted != identity) __builtin_trap();
    }

    // P4 -- idempotent.
    if (canonicalOrder(base).order != r0.order) __builtin_trap();

    // P5 -- the status says what actually happened. This harness never passes
    // a renderer, so K4 cannot resolve anything: ANY surviving tie is
    // unresolvable here, not just one that blew the bound. (This assertion was
    // itself wrong on the first run -- it only knew about the bound -- and the
    // fuzzer caught that too, one crash after catching the real leak.)
    const bool tied = r0.largest_tied_class > 1;
    if (tied != (r0.status == OrderStatus::Ambiguous)) __builtin_trap();

    // P2 -- refinement may only break ties, never reorder across them. Walk the
    // output and require the declared keys to be non-decreasing.
    for (std::size_t p = 1; p < n; ++p) {
        const auto& prev = base[r0.order[p - 1]].keys;
        const auto& cur  = base[r0.order[p]].keys;
        const std::size_t m = std::min(prev.size(), cur.size());
        int cmp = 0;
        for (std::size_t k = 0; k < m && cmp == 0; ++k) cmp = prev[k].compare(cur[k]);
        if (cmp == 0 && prev.size() != cur.size()) cmp = prev.size() < cur.size() ? -1 : 1;
        if (cmp > 0) __builtin_trap();
    }

    // P1 -- the property the whole module exists for. Try several input orders
    // drawn from the fuzz input rather than one, so a bug that only shows on a
    // particular shuffle is reachable.
    const std::vector<std::uint32_t> base_ids = identitySequence(r0, identity);
    for (int attempt = 0; attempt < 4 && !b.empty(); ++attempt) {
        std::vector<std::uint32_t> perm = identity;
        // A Fisher-Yates shuffle driven entirely by the input bytes.
        for (std::size_t i = n; i > 1; --i)
            std::swap(perm[i - 1], perm[b.below(i)]);

        const std::vector<Member> shuffled = repermute(base, perm);
        const OrderResult r1 = canonicalOrder(shuffled);

        if (r1.order.size() != n) __builtin_trap();
        if (r1.largest_tied_class != r0.largest_tied_class) __builtin_trap();
        if (r1.status != r0.status) __builtin_trap();

        // Only an exact result is claimed to be canonical. When a class blew
        // the K4 bound the projector said so, and comparing orders there would
        // be testing a promise nobody made.
        if (r0.status == OrderStatus::Exact
            && identitySequence(r1, perm) != base_ids) __builtin_trap();
    }
}

void checkRenderers(Bytes& b) {
    // P6 -- escape closure. For ANY bytes, the quoted form must be safe to put
    // on a line of a format whose nesting has no closing delimiter.
    const std::string raw = b.text(64);
    const std::string q = quoteString(raw);

    if (q.size() < 2 || q.front() != '"' || q.back() != '"') __builtin_trap();
    for (std::size_t i = 1; i + 1 < q.size(); ++i) {
        const auto c = static_cast<unsigned char>(q[i]);
        if (c == '\n' || c == '\r' || c == '\t') __builtin_trap();  // would forge a line
        if (c < 0x20 || c == 0x7F) __builtin_trap();                // raw control
    }
    // The bidi controls, in their UTF-8 form, must not survive into the output.
    for (const char* bidi : {"\xE2\x80\xAE", "\xE2\x80\xAD", "\xE2\x80\xAB",
                             "\xE2\x81\xA6", "\xE2\x81\xA9", "\xE2\x80\x8F"})
        if (q.find(bidi) != std::string::npos) __builtin_trap();

    // A bare token is only offered when it needs no escaping at all, so it must
    // equal its input exactly.
    if (isBareSafe(raw) && renderLabelToken(raw) != raw) __builtin_trap();

    // Durations are total and never emit anything a parser could mistake.
    const std::string d = renderDuration(b.i64());
    if (d.empty()) __builtin_trap();
    for (char c : d)
        if (!((c >= '0' && c <= '9') || c == '/' || c == 't' || c == '-')) __builtin_trap();

    // Floats: finite values always carry a `.` or an `e`, so one can never be
    // mistaken for an integer and -0.0 is distinguishable from 0.0.
    const double v = b.f64();
    const std::string f = renderF64(v);
    if (f.empty()) __builtin_trap();
    if (f.find('+') != std::string::npos) __builtin_trap();
    const bool finite = f != "nan" && f != "inf" && f != "-inf";
    if (finite && f.find('.') == std::string::npos && f.find('e') == std::string::npos)
        __builtin_trap();
}

void checkDesignators(Bytes& b) {
    // P7 -- PATH INTEGRITY. A designator is one quoted string whose segments
    // are separated by `/`, and segment content is attacker-influenced (it is
    // a track name). So the number of literal `/` in the output must equal the
    // number of segments, for ANY segment bytes: a name that can contribute a
    // literal `/` can forge a path boundary and make a send appear to target a
    // different track than it does.
    const std::size_t count = b.below(5);
    std::vector<std::string> segs;
    segs.reserve(count);
    for (std::size_t i = 0; i < count; ++i) segs.push_back(b.text(12));

    const std::string d = designator(segs);
    if (d.size() < 2 || d.front() != '"' || d.back() != '"') __builtin_trap();

    std::size_t slashes = 0;
    for (std::size_t i = 1; i + 1 < d.size(); ++i) {
        const auto c = static_cast<unsigned char>(d[i]);
        if (c == '/') ++slashes;
        // Escape closure applies inside a designator too.
        if (c == '\n' || c == '\r' || c == '\t') __builtin_trap();
        if (c < 0x20 || c == 0x7F) __builtin_trap();
    }
    if (slashes != count) __builtin_trap();
    for (const char* bidi : {"\xE2\x80\xAE", "\xE2\x81\xA6", "\xE2\x80\x8F"})
        if (d.find(bidi) != std::string::npos) __builtin_trap();

    // P8 -- MEDIA PREFIXES. Uniform length; equal hashes give equal prefixes;
    // and a prefix separates exactly as well as the whole hash does, because
    // anything less would silently merge two media files and anything more is
    // noise in every diff that cites one.
    const std::size_t n = b.below(6);
    std::vector<std::string> hashes;
    hashes.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Hex-shaped, with deliberate shared leading runs so collisions at the
        // 16-digit minimum are actually reachable.
        static const char kHex[] = "0123456789abcdef";
        const std::size_t shared = b.below(24);
        std::string h;
        for (std::size_t k = 0; k < 32; ++k)
            h.push_back(kHex[(k < shared ? k : k + b.u8()) % 16]);
        hashes.push_back(std::move(h));
    }
    const std::vector<std::string> pre = mediaPrefixes(hashes);
    if (pre.size() != hashes.size()) __builtin_trap();
    for (std::size_t i = 0; i < pre.size(); ++i) {
        if (!pre.empty() && pre[i].size() != pre[0].size()) __builtin_trap();
        if (hashes[i].compare(0, pre[i].size(), pre[i]) != 0) __builtin_trap();
        for (std::size_t j = 0; j < i; ++j) {
            if ((hashes[i] == hashes[j]) != (pre[i] == pre[j])) __builtin_trap();
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Bytes b(data, size);
    checkRenderers(b);
    checkDesignators(b);
    checkOrdering(b);
    return 0;
}
