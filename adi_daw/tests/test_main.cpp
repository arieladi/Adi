// SPDX-License-Identifier: GPL-3.0-or-later
//
// Step 4 tests. No framework: these are asserts about a binary format, and a
// dependency would have to earn its place. Revisit if this grows past one file.

#include "adi/blob.hpp"

#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

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
    check(out.size() == in.size(), "record count matches");
    check(std::memcmp(in.data(), out.data(), in.size() * sizeof(NoteRecord)) == 0,
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
    check(rf[1].note_id == 101 && rf[1].key == 61,
          "fields read correctly despite the unknown tail");
    check(rf[2].start_ticks == 3000, "third record found at the right stride");

    // An OLDER writer: narrower records. Absent fields take zero defaults.
    constexpr std::uint16_t kOldRec = 32;  // no tuning_cents, no reserved
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
    check(ro[1].key == 41, "known fields read from a narrow record");
    check(ro[1].tuning_cents == 0.0f, "absent field takes its zero default");
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
    check(ar[49].value == 1.0, "f64 value survives exactly at the endpoint");
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

}  // namespace

int main() {
    std::printf("adi_tests -- SPEC 6.3 binary layouts\n\n");
    testLayout();
    testRoundTrip();
    testStriding();
    testErrors();
    testOtherStreams();
    std::printf("\n%s -- %d checks, %d failure(s)\n", g_failures ? "FAILED" : "PASS", g_checks,
                g_failures);
    return g_failures ? 1 : 0;
}
