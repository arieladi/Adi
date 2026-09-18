// SPDX-License-Identifier: GPL-3.0-or-later
//
// libFuzzer harness for StreamReader — SPEC §6.3.
//
// StreamReader is the only component in the tree that parses bytes it did not
// write. A .adi arrives truncated by a full volume, cut short by a failed sync,
// carried across a version boundary, or handed over by someone who would like it
// to be interesting. Four bugs were found here by hand (ADR-0023); this exists
// to find the fifth.
//
// THE PROPERTY UNDER TEST
//
//   No input causes a crash, a hang, or a read outside the buffer.
//
// Not "parses correctly". Most random bytes are not a valid stream and MUST be
// rejected — rejection is a pass, not a finding. The only verdicts that count
// are a sanitiser report, a timeout, or one of the invariant checks below.
//
// The invariants are deliberately few and all of them are about memory safety or
// self-consistency, never about content:
//
//   * a reader that is !ok() yields nothing from at() and nothing from all()
//   * at(i) is nullopt for every i >= count(), and never faults for any i
//   * all() and at() agree, record for record
//   * the accessors agree with each other about whether the reader succeeded
//
// BUILD
//   cmake -S adi_daw -B build -DADI_BUILD_FUZZERS=ON   (clang only; see CMakeLists)
//   ./build/adi_fuzz_blob -runs=100000 tests/fuzz_corpus
//
// Apple clang ships no libFuzzer runtime; use Homebrew LLVM or a Linux clang.

#include "adi/blob.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace adi;

/// `count` is a u32, so a hostile header can ask for four billion records. The
/// property is "no hang", so the harness must not honour that: it probes the
/// interesting indices and a bounded prefix instead of walking the whole stream.
constexpr std::uint32_t kProbePrefix = 64;

/// Small enough that a single input cannot make the fuzzer's RSS the finding.
/// The default cap (kMaxRecordsAtOnce, 8.4M) is a policy the unit tests own;
/// what matters here is that all() and at() agree and that neither reads out of
/// bounds, which a small cap exercises just as well and thousands of times
/// faster.
constexpr std::uint32_t kAllCap = 4096;

/// Exercise every entry point of the reader for one record type.
template <typename Rec>
void exercise(std::span<const std::byte> blob, FourCC expected) {
    const StreamReader<Rec> r(blob, expected);

    // Total accessors: legal to call in any state. Reading them is the point —
    // the old operator[] segfaulted precisely because a caller could reach a
    // reader's header without its body.
    const bool okay = r.ok();
    const auto err = r.error();
    const std::uint32_t n = r.count();
    const std::uint16_t rec = r.recSize();
    const std::uint16_t ver = r.version();
    const bool tail = r.hasUnknownTail();
    const StreamHeader hdr = r.header();
    (void)err; (void)ver; (void)hdr;

    // Self-consistency, not content.
    if (!okay) {
        if (n != 0) __builtin_trap();            // count() must fold to 0
        if (rec != 0) __builtin_trap();          // so must recSize()
        if (tail) __builtin_trap();              // and hasUnknownTail()
        if (r.at(0).has_value()) __builtin_trap();
        if (r.all(kAllCap).has_value()) __builtin_trap();
        return;
    }

    // Out of range in every direction, including the boundary and the extremes.
    // None of these may fault, and all must be nullopt.
    if (r.at(n).has_value()) __builtin_trap();
    if (n < (std::numeric_limits<std::uint32_t>::max)() &&
        r.at(n + 1).has_value()) __builtin_trap();
    if (r.at((std::numeric_limits<std::uint32_t>::max)()).has_value()) __builtin_trap();

    // A bounded prefix of the records that should exist. Every one must be
    // readable; the sanitiser decides whether the read was in bounds.
    const std::uint32_t probe = n < kProbePrefix ? n : kProbePrefix;
    for (std::uint32_t i = 0; i < probe; ++i) {
        const std::optional<Rec> rec_i = r.at(i);
        if (!rec_i.has_value()) __builtin_trap();   // i < count() must yield one
        volatile const unsigned char* p =
            reinterpret_cast<const unsigned char*>(&*rec_i);
        volatile unsigned char sink = 0;
        for (std::size_t b = 0; b < sizeof(Rec); ++b) sink = static_cast<unsigned char>(sink ^ p[b]);
        (void)sink;                                  // force the bytes to be read
    }

    // all() must agree with at(), or one of them is walking the buffer wrongly.
    if (const auto v = r.all(kAllCap)) {
        if (v->size() != n) __builtin_trap();
        const std::uint32_t m = n < kProbePrefix ? n : kProbePrefix;
        for (std::uint32_t i = 0; i < m; ++i) {
            const auto one = r.at(i);
            if (!one.has_value()) __builtin_trap();
            if (std::memcmp(&(*v)[i], &*one, sizeof(Rec)) != 0) __builtin_trap();
        }
    } else if (n <= kAllCap) {
        __builtin_trap();                            // within cap: must succeed
    }

    // Re-serialising what we just read is what a save does, and it is the path
    // ADR-0008 warns loses an unknown tail. Bounded so the fuzzer does not spend
    // its time in the allocator.
    if (n > 0 && n <= 256) {
        if (const auto v = r.all(kAllCap)) {
            const auto out = writeStream<Rec>(expected, *v);
            const StreamReader<Rec> back(out, expected);
            if (!back.ok()) __builtin_trap();
            if (back.count() != n) __builtin_trap();
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> blob{
        reinterpret_cast<const std::byte*>(data), size};

    // Every input is offered to all three readers. Two of the three will reject
    // on fourcc, which is itself worth exercising: BadFourCC is the state that
    // used to leave a populated header beside a null body, and it is the state
    // the old operator[] segfaulted in.
    exercise<NoteRecord>(blob, FourCC::Notes);
    exercise<AutomationPoint>(blob, FourCC::Automation);
    exercise<ExpressionPoint>(blob, FourCC::Expression);
    return 0;
}
