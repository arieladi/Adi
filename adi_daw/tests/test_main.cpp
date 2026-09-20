// SPDX-License-Identifier: GPL-3.0-or-later
//
// Step 4 tests. No framework: these are asserts about a binary format, and a
// dependency would have to earn its place. Revisit if this grows past one file.

#include "adi/blob.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>


// A synthetic record type with TWO released sizes. Every real type has exactly
// one so far, which makes ADR-0008's "older, narrower writer" branch unreachable
// for them -- correct, but untested. This keeps it covered until a real type
// gains a v2, at which point this can go.
#pragma pack(push, 1)
struct FakeV2Record {
    std::int64_t a;   // 0
    std::int64_t b;   // 8
    std::uint64_t c;  // 16
    std::uint64_t d;  // 24
    std::uint64_t e;  // 32   <- absent in the 40-byte v1
    std::uint64_t f;  // 40   <- absent in the 40-byte v1
};
#pragma pack(pop)
static_assert(sizeof(FakeV2Record) == 48);

namespace adi {
template <> struct StreamTraits<FakeV2Record> {
    static constexpr std::size_t released[] = {40, 48};
};
}  // namespace adi

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void section(const char* s) { std::printf("[%s]\n", s); }

using namespace adi;

// --- layout ----------------------------------------------------------------
// The sizes are static_asserts in blob.hpp, so reaching this code at all proves
// them. What is checked here is that the bytes on the wire actually carry what
// we think they do, at the offsets SPEC 6.3.1 names.
void testLayout() {
    section("layout");
    NoteRecord n{};
    n.start_ticks = 5765760;
    n.dur_ticks = 2882880;
    n.note_id = 42;
    n.key = 60;
    n.vel_on = 100;
    n.vel_off = 64;
    n.channel = 3;
    n.flags = NoteFlags::HasExpression;
    n.probability = 8000;
    n.tuning_cents = -13.75f;

    std::byte raw[sizeof(NoteRecord)];
    std::memcpy(raw, &n, sizeof n);

    std::int64_t start = 0;
    std::uint64_t id = 0;
    std::uint8_t key = 0;
    std::uint16_t prob = 0;
    float cents = 0.0f;
    std::memcpy(&start, raw + 0, 8);
    std::memcpy(&id, raw + 16, 8);
    std::memcpy(&key, raw + 24, 1);
    std::memcpy(&prob, raw + 30, 2);
    std::memcpy(&cents, raw + 32, 4);

    check(start == 5765760, "start_ticks at byte 0");
    check(id == 42, "note_id at byte 16");
    check(key == 60, "key at byte 24");
    check(prob == 8000, "probability at byte 30");
    check(cents == -13.75f, "tuning_cents at byte 32");
}

// --- round trip -------------------------------------------------------------
void testRoundTrip() {
    section("round trip");
    std::vector<NoteRecord> in;
    for (int i = 0; i < 1000; ++i) {
        NoteRecord n{};
        n.start_ticks = static_cast<std::int64_t>(i) * 1441440;  // 16ths at ADI_PPQ
        n.dur_ticks = 1441440;
        n.note_id = static_cast<std::uint64_t>(i) + 1;
        n.key = static_cast<std::uint8_t>(36 + (i % 60));
        n.vel_on = static_cast<std::uint8_t>(1 + (i % 127));
        n.vel_off = 64;
        n.channel = static_cast<std::uint8_t>(i % 16);
        n.probability = static_cast<std::uint16_t>((i * 7) % 10001);
        n.tuning_cents = static_cast<float>(i % 200) - 100.0f;
        in.push_back(n);
    }

    const auto blob = writeStream<NoteRecord>(FourCC::Notes, in);
    check(blob.size() == 16 + 1000 * 40, "blob is header + count*rec_size");

    StreamReader<NoteRecord> r(blob, FourCC::Notes);
    check(r.ok(), "reader accepts its own output");
    check(r.count() == 1000, "count survives");
    check(r.recSize() == 40, "rec_size is 40");
    check(!r.hasUnknownTail(), "no unknown tail for a same-version blob");

    const auto out = r.all();
    check(out.has_value(), "all() succeeds within the cap");
    check(out->size() == in.size(), "record count matches");
    check(std::memcmp(in.data(), out->data(), in.size() * sizeof(NoteRecord)) == 0,
          "1000 notes round-trip byte-identically");
}

// --- ADR-0008 forward compatibility ------------------------------------------
// The contract that lets us add a per-note field later without breaking every
// build already in the field. The most important test in this file.
void testStriding() {
    section("ADR-0008 striding");

    // A NEWER writer: our fields plus 8 bytes we know nothing about.
    constexpr std::uint16_t kFutureRec = 48;
    std::vector<std::byte> future(16 + 3 * kFutureRec, std::byte{0});
    StreamHeader h{};
    h.fourcc = static_cast<std::uint32_t>(FourCC::Notes);
    h.version = 2;
    h.rec_size = kFutureRec;
    h.count = 3;
    h.flags = StreamFlags::SortedByTime;
    std::memcpy(future.data(), &h, sizeof h);

    for (int i = 0; i < 3; ++i) {
        NoteRecord n{};
        n.start_ticks = 1000 * (static_cast<std::int64_t>(i) + 1);
        n.note_id = static_cast<std::uint64_t>(i) + 100;
        n.key = static_cast<std::uint8_t>(60 + i);
        std::memcpy(future.data() + 16 + static_cast<std::size_t>(i) * kFutureRec, &n, sizeof n);
        const std::uint64_t marker = 0xDEADBEEFCAFEF00Dull;  // the unknown tail
        std::memcpy(future.data() + 16 + static_cast<std::size_t>(i) * kFutureRec + 40, &marker, 8);
    }

    StreamReader<NoteRecord> rf(future, FourCC::Notes);
    check(rf.ok(), "newer, wider records are readable");
    check(rf.recSize() == kFutureRec, "stride comes from the header, not sizeof");
    check(rf.hasUnknownTail(), "unknown tail is detected so callers preserve bytes");
    check(rf.count() == 3, "count correct with a wider stride");
    check(rf.at(1)->note_id == 101 && rf.at(1)->key == 61,
          "fields read correctly despite the unknown tail");
    check(rf.at(2)->start_ticks == 3000, "third record found at the right stride");

    // An OLDER writer: narrower records. Absent fields take zero defaults.
    constexpr std::uint16_t kOldRec = 40;  // a released size; see StreamTraits
    std::vector<std::byte> old(16 + 2 * kOldRec, std::byte{0});
    StreamHeader ho{};
    ho.fourcc = static_cast<std::uint32_t>(FourCC::Notes);
    ho.version = 1;
    ho.rec_size = kOldRec;
    ho.count = 2;
    std::memcpy(old.data(), &ho, sizeof ho);
    for (int i = 0; i < 2; ++i) {
        NoteRecord n{};
        n.start_ticks = 500 * (static_cast<std::int64_t>(i) + 1);
        n.key = static_cast<std::uint8_t>(40 + i);
        std::memcpy(old.data() + 16 + static_cast<std::size_t>(i) * kOldRec, &n, kOldRec);
    }

    StreamReader<NoteRecord> ro(old, FourCC::Notes);
    check(ro.ok(), "older, narrower records are readable");
    check(!ro.hasUnknownTail(), "a narrower blob has no unknown tail");
    check(ro.at(1)->key == 41, "known fields read from a narrow record");
    check(ro.at(1)->tuning_cents == 0.0f, "absent field takes its zero default");
}

// --- malformed input ---------------------------------------------------------
void testErrors() {
    section("malformed input");
    const std::vector<std::byte> tiny(8, std::byte{0});
    check(StreamReader<NoteRecord>(tiny, FourCC::Notes).error() == StreamError::TooShort,
          "blob shorter than its header is rejected");

    const std::vector<NoteRecord> none;
    const auto empty = writeStream<NoteRecord>(FourCC::Notes, none);
    check(StreamReader<NoteRecord>(empty, FourCC::Automation).error() == StreamError::BadFourCC,
          "wrong fourcc is rejected, not misread");

    const std::vector<NoteRecord> one(1);
    auto trunc = writeStream<NoteRecord>(FourCC::Notes, one);
    trunc.resize(trunc.size() - 4);
    check(StreamReader<NoteRecord>(trunc, FourCC::Notes).error() == StreamError::Truncated,
          "truncated body is rejected");

    auto zero = writeStream<NoteRecord>(FourCC::Notes, one);
    const std::uint16_t z = 0;
    std::memcpy(zero.data() + 6, &z, 2);
    check(StreamReader<NoteRecord>(zero, FourCC::Notes).error() == StreamError::ZeroRecSize,
          "rec_size 0 is rejected rather than dividing by zero");
}

void testOtherStreams() {
    section("automation + expression");
    std::vector<AutomationPoint> ap(50);
    for (std::size_t i = 0; i < ap.size(); ++i) {
        ap[i].time = static_cast<std::int64_t>(i) * 720720;
        ap[i].value = static_cast<double>(i) / 49.0;
        ap[i].curve = static_cast<std::uint8_t>(Curve::Linear);
        ap[i].point_id = i + 1;
    }
    const auto ab = writeStream<AutomationPoint>(FourCC::Automation, ap);
    StreamReader<AutomationPoint> ar(ab, FourCC::Automation);
    check(ar.ok() && ar.count() == 50, "automation stream round-trips");
    check(ar.at(49)->value == 1.0, "f64 value survives exactly at the endpoint");
    check(ar.recSize() == 32, "automation rec_size is 32");

    std::vector<ExpressionPoint> ep(200);
    for (std::size_t i = 0; i < ep.size(); ++i) {
        ep[i].time_ticks = static_cast<std::int64_t>(i) * 1000;
        ep[i].value = static_cast<float>(i) * 0.005f;
    }
    const auto eb = writeStream<ExpressionPoint>(FourCC::Expression, ep);
    StreamReader<ExpressionPoint> er(eb, FourCC::Expression);
    check(er.ok() && er.count() == 200, "expression stream round-trips");
    check(er.recSize() == 24, "expression rec_size is 24");
}


// --- hardening: every accessor is total -------------------------------------
// Each check here is a bug that mac found in the first cut of this reader, and
// each one is a file a user could be handed: truncated by a full volume, cut
// short by a failed sync, or written by someone hostile.
void testHardening() {
    section("hardening");

    // 1. at() on a FAILED reader. The old operator[] memcpy'd from a null
    //    span -- reproduced as a segfault, exit 139.
    const std::vector<NoteRecord> one(1);
    const auto good = writeStream<NoteRecord>(FourCC::Notes, one);
    StreamReader<NoteRecord> wrongKind(good, FourCC::Automation);
    check(!wrongKind.ok(), "wrong fourcc still fails");
    check(!wrongKind.at(0).has_value(), "at() on a failed reader returns nullopt, not a segfault");
    check(wrongKind.count() == 0, "count() on a failed reader is 0");
    check(!wrongKind.all().has_value(), "all() on a failed reader returns nullopt");

    // 2. at() out of range. Previously read ~40MB past a 96-byte buffer and
    //    returned the garbage silently.
    StreamReader<NoteRecord> r(good, FourCC::Notes);
    check(r.ok() && r.count() == 1, "one-record blob reads back");
    check(r.at(0).has_value(), "index 0 is in range");
    check(!r.at(1).has_value(), "index == count is out of range");
    check(!r.at(1000000).has_value(), "far out-of-range index returns nullopt");
    check(!r.at(0xFFFFFFFFu).has_value(), "UINT32_MAX index returns nullopt");

    // 3. The 32-bit size_t overflow. count*rec_size == 2^32 wrapped to 0 in
    //    size_t arithmetic, so `need` became 16 and a header-only blob passed
    //    while count() reported 131072. Now computed in u64, so this is
    //    rejected on every ABI -- Truncated on LP64/LLP64, and on ILP32 either
    //    Truncated or TooLarge. What matters is that it is never Ok.
    {
        std::vector<std::byte> hdrOnly(16, std::byte{0});
        StreamHeader h{};
        h.fourcc = static_cast<std::uint32_t>(FourCC::Notes);
        h.version = 1;
        h.rec_size = 32768;
        h.count = 131072;  // 131072 * 32768 == 2^32
        std::memcpy(hdrOnly.data(), &h, sizeof h);
        StreamReader<NoteRecord> ovf(hdrOnly, FourCC::Notes);
        check(!ovf.ok(), "count*rec_size == 2^32 on a 16-byte blob is rejected");
        check(ovf.error() == StreamError::Truncated || ovf.error() == StreamError::TooLarge,
              "the overflow fixture fails for a length reason, not by chance");
        check(ovf.count() == 0, "a rejected blob reports no records");
    }

    // 4. A rec_size that lands mid-field. 29 copies one byte of the 2-byte
    //    flags at offset 28 and leaves the other zero, so flags was silently
    //    0x00EF where the writer wrote 0xBEEF, with error() == Ok. ADR-0008
    //    promises absent fields take a zero default; half a field is neither.
    {
        constexpr std::uint16_t kTorn = 29;
        std::vector<std::byte> torn(16 + kTorn, std::byte{0});
        StreamHeader h{};
        h.fourcc = static_cast<std::uint32_t>(FourCC::Notes);
        h.version = 1;
        h.rec_size = kTorn;
        h.count = 1;
        std::memcpy(torn.data(), &h, sizeof h);
        StreamReader<NoteRecord> t(torn, FourCC::Notes);
        check(t.error() == StreamError::RecSizeUnknown,
              "a rec_size matching no released version is rejected, not torn");
        check(!t.at(0).has_value(), "a torn record is not readable");
    }

    // 5. all() amplification cap. rec_size may be far smaller than sizeof(Rec),
    //    so a modest blob can ask for a very large vector.
    {
        std::vector<NoteRecord> many(1000);
        const auto blob = writeStream<NoteRecord>(FourCC::Notes, many);
        StreamReader<NoteRecord> rr(blob, FourCC::Notes);
        check(rr.all(1000).has_value(), "all() succeeds at exactly the cap");
        check(!rr.all(999).has_value(), "all() refuses above the cap rather than allocating");
        check(rr.at(999).has_value(), "at() still reaches every record when all() refuses");
    }
}

// --- ADR-0008: an older, narrower released version --------------------------
void testNarrowerReleasedVersion() {
    section("ADR-0008 older writer");
    constexpr std::uint16_t kV1 = 40;  // a released size for FakeV2Record
    std::vector<std::byte> blob(16 + 2 * kV1, std::byte{0});
    StreamHeader h{};
    h.fourcc = static_cast<std::uint32_t>(FourCC::Notes);
    h.version = 1;
    h.rec_size = kV1;
    h.count = 2;
    std::memcpy(blob.data(), &h, sizeof h);
    for (int i = 0; i < 2; ++i) {
        FakeV2Record v{};
        v.a = 100 * (i + 1);
        v.d = 7;
        v.e = 0xDEAD;  // at offset 32: the LAST field of the 40-byte version
        v.f = 0xBEEF;  // at offset 40: beyond it, so it must NOT survive
        std::memcpy(blob.data() + 16 + static_cast<std::size_t>(i) * kV1, &v, kV1);
    }
    StreamReader<FakeV2Record> rd(blob, FourCC::Notes);
    check(rd.ok(), "a narrower RELEASED rec_size is accepted");
    check(rd.at(1)->a == 200, "fields inside the older record read correctly");
    check(rd.at(1)->d == 7, "last field of the older record reads correctly");
    check(rd.at(1)->e == 0xDEAD, "the last field inside the older record survives");
    check(rd.at(1)->f == 0, "a field the older version lacked takes its zero default");
}

}  // namespace

int main() {
    // Unbuffered, so the last line before a crash survives. On Windows a
    // crashing test binary loses its whole block-buffered stdout, and the
    // harness then prints a blank line where a failure should be -- which is
    // how adi_device_tests' 383 KB overrun looked like a harness glitch for
    // two runs before anyone ran the binary directly.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("adi_tests -- SPEC 6.3 binary layouts\n\n");
    testLayout();
    testRoundTrip();
    testStriding();
    testErrors();
    testOtherStreams();
    testHardening();
    testNarrowerReleasedVersion();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
