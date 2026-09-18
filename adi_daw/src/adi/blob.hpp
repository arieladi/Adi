// SPDX-License-Identifier: GPL-3.0-or-later
//
// Binary stream layouts for the .adi core tier — SPEC §6.3.
//
// This header is the reason step 4 is C++ rather than a Python prototype. The
// spec makes exact claims about record sizes and field offsets; a scripting
// language has no struct layout to get wrong, so it cannot test them. Here
// every claim is a static_assert, and a mismatch is a compile error rather than
// a corrupt project file discovered by a user in a year.
//
// Two rules from the spec are load-bearing and are implemented here rather than
// left to callers:
//
//   ADR-0008  Readers MUST stride by the header's rec_size, never by
//             sizeof(their own struct). StreamReader does this; it is the only
//             supported way to walk a stream.
//   SPEC §6.3 All integers little-endian, floats IEEE 754.

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace adi {

// The format is little-endian on the wire. Every target we support (x86-64,
// arm64) is little-endian, so encode/decode is a memcpy. If this ever fails,
// the fix is byte-swapping accessors, not deleting the assert.
static_assert(std::endian::native == std::endian::little,
              "adi: big-endian hosts need byte-swapping accessors (SPEC 6.3)");

static_assert(sizeof(float) == 4 && sizeof(double) == 8, "adi: needs IEEE 754");

// ---------------------------------------------------------------------------
// Stream header — SPEC §6.3, 16 bytes, prefixes every core-tier BLOB.
// ---------------------------------------------------------------------------

enum class FourCC : std::uint32_t {
    Notes      = 0x544F4E41u,  // 'ANOT' little-endian
    Automation = 0x54554141u,  // 'AAUT'
    Expression = 0x50584541u,  // 'AEXP'
    Controller = 0x4C544341u,  // 'ACTL'
    Sysex      = 0x58595341u,  // 'ASYX'
    WarpMap    = 0x50525741u,  // 'AWRP'
};

struct StreamFlags {
    static constexpr std::uint32_t SortedByTime = 1u << 0;
    static constexpr std::uint32_t TimeIsNanos  = 1u << 1;
};

#pragma pack(push, 1)

struct StreamHeader {
    std::uint32_t fourcc;    // 0
    std::uint16_t version;   // 4
    std::uint16_t rec_size;  // 6  bytes per record IN THIS BLOB
    std::uint32_t count;     // 8
    std::uint32_t flags;     // 12
};

// ---------------------------------------------------------------------------
// ANOT — note record, v1, 40 bytes. SPEC §6.3.1.
// ---------------------------------------------------------------------------
struct NoteRecord {
    std::int64_t  start_ticks;   // 0   relative to clip origin
    std::int64_t  dur_ticks;     // 8   > 0
    std::uint64_t note_id;       // 16  stable within clip; 0 = unassigned
    std::uint8_t  key;           // 24  0..127
    std::uint8_t  vel_on;        // 25  1..127
    std::uint8_t  vel_off;       // 26  0..127
    std::uint8_t  channel;       // 27  0..15
    std::uint16_t flags;         // 28
    std::uint16_t probability;   // 30  0..10000 = 0.00%..100.00%
    float         tuning_cents;  // 32  -1200.0..+1200.0
    std::uint32_t reserved;      // 36  MUST be 0
};

struct NoteFlags {
    static constexpr std::uint16_t Muted         = 1u << 0;
    static constexpr std::uint16_t Selected      = 1u << 1;
    static constexpr std::uint16_t HasExpression = 1u << 2;
    static constexpr std::uint16_t TiedToNext    = 1u << 3;
    static constexpr std::uint16_t Ghost         = 1u << 4;
};

// ---------------------------------------------------------------------------
// AAUT — automation point, v1, 32 bytes. SPEC §6.3.3.
// ---------------------------------------------------------------------------
struct AutomationPoint {
    std::int64_t  time;      // 0   ticks or ns per the lane's time_base
    double        value;     // 8   in the lane's declared value_domain
    float         tension;   // 16
    std::uint8_t  curve;     // 20
    std::uint8_t  flags;     // 21
    std::uint16_t reserved;  // 22
    std::uint64_t point_id;  // 24
};

// ---------------------------------------------------------------------------
// AEXP — per-note expression point, v1, 24 bytes. SPEC §6.3.2.
// ---------------------------------------------------------------------------
struct ExpressionPoint {
    std::int64_t time_ticks;     // 0   relative to NOTE start
    float        value;          // 8
    float        tension;        // 12
    std::uint8_t curve;          // 16
    std::uint8_t flags;          // 17
    std::uint8_t reserved[6];    // 18
};

#pragma pack(pop)

enum class Curve : std::uint8_t {
    Hold = 0, Linear = 1, Exponential = 2, Logarithmic = 3, SCurve = 4, Bezier = 5,
};

enum class ExpressionDim : std::uint8_t {
    Pitch = 0, Pressure = 1, Timbre = 2, Gain = 3, Pan = 4,
    // >= 64 is plugin-defined
};

// ---------------------------------------------------------------------------
// The spec's size and offset claims, proved at compile time.
// ---------------------------------------------------------------------------

#define ADI_FIELD_AT(T, field, off) \
    static_assert(offsetof(T, field) == (off), \
                  #T "." #field " is not at offset " #off " — SPEC 6.3 disagrees")

static_assert(sizeof(StreamHeader) == 16, "stream header must be 16 bytes (SPEC 6.3)");
ADI_FIELD_AT(StreamHeader, fourcc,   0);
ADI_FIELD_AT(StreamHeader, version,  4);
ADI_FIELD_AT(StreamHeader, rec_size, 6);
ADI_FIELD_AT(StreamHeader, count,    8);
ADI_FIELD_AT(StreamHeader, flags,   12);

static_assert(sizeof(NoteRecord) == 40, "note record must be 40 bytes (SPEC 6.3.1)");
ADI_FIELD_AT(NoteRecord, start_ticks,   0);
ADI_FIELD_AT(NoteRecord, dur_ticks,     8);
ADI_FIELD_AT(NoteRecord, note_id,      16);
ADI_FIELD_AT(NoteRecord, key,          24);
ADI_FIELD_AT(NoteRecord, vel_on,       25);
ADI_FIELD_AT(NoteRecord, vel_off,      26);
ADI_FIELD_AT(NoteRecord, channel,      27);
ADI_FIELD_AT(NoteRecord, flags,        28);
ADI_FIELD_AT(NoteRecord, probability,  30);
ADI_FIELD_AT(NoteRecord, tuning_cents, 32);
ADI_FIELD_AT(NoteRecord, reserved,     36);

static_assert(sizeof(AutomationPoint) == 32, "automation point must be 32 bytes (SPEC 6.3.3)");
ADI_FIELD_AT(AutomationPoint, time,      0);
ADI_FIELD_AT(AutomationPoint, value,     8);
ADI_FIELD_AT(AutomationPoint, tension,  16);
ADI_FIELD_AT(AutomationPoint, curve,    20);
ADI_FIELD_AT(AutomationPoint, flags,    21);
ADI_FIELD_AT(AutomationPoint, reserved, 22);
ADI_FIELD_AT(AutomationPoint, point_id, 24);

static_assert(sizeof(ExpressionPoint) == 24, "expression point must be 24 bytes (SPEC 6.3.2)");
ADI_FIELD_AT(ExpressionPoint, time_ticks, 0);
ADI_FIELD_AT(ExpressionPoint, value,      8);
ADI_FIELD_AT(ExpressionPoint, tension,   12);
ADI_FIELD_AT(ExpressionPoint, curve,     16);
ADI_FIELD_AT(ExpressionPoint, flags,     17);
ADI_FIELD_AT(ExpressionPoint, reserved,  18);

#undef ADI_FIELD_AT

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/// Serialise records into a blob with the 16-byte header prepended.
template <typename Rec>
std::vector<std::byte> writeStream(FourCC cc, std::span<const Rec> records,
                                   std::uint16_t version = 1,
                                   std::uint32_t flags = StreamFlags::SortedByTime)
{
    StreamHeader h{};
    h.fourcc   = static_cast<std::uint32_t>(cc);
    h.version  = version;
    h.rec_size = static_cast<std::uint16_t>(sizeof(Rec));
    h.count    = static_cast<std::uint32_t>(records.size());
    h.flags    = flags;

    std::vector<std::byte> out(sizeof(StreamHeader) + records.size() * sizeof(Rec));
    std::memcpy(out.data(), &h, sizeof h);
    if (!records.empty())
        std::memcpy(out.data() + sizeof h, records.data(), records.size() * sizeof(Rec));
    return out;
}

// ---------------------------------------------------------------------------
// Reading — the ADR-0008 striding contract
// ---------------------------------------------------------------------------

enum class StreamError {
    Ok, TooShort, BadFourCC, ZeroRecSize, Truncated,
};

/// Human-readable form, for CLI output and test failures.
const char* toString(StreamError e);

/// Walks a stream by the header's rec_size, never by sizeof(Rec).
///
/// A record wider than this build knows is a NEWER version's: the extra tail
/// bytes are skipped on read and the blob must be preserved byte-for-byte on
/// save. A record narrower is an OLDER version's: missing fields take their
/// documented defaults (zero). Both cases are normal, not errors — that is the
/// whole point of ADR-0008.
template <typename Rec>
class StreamReader {
public:
    StreamReader(std::span<const std::byte> blob, FourCC expected) {
        if (blob.size() < sizeof(StreamHeader)) { err_ = StreamError::TooShort; return; }
        std::memcpy(&h_, blob.data(), sizeof h_);
        if (h_.fourcc != static_cast<std::uint32_t>(expected)) { err_ = StreamError::BadFourCC; return; }
        if (h_.rec_size == 0) { err_ = StreamError::ZeroRecSize; return; }

        const std::size_t need = sizeof(StreamHeader)
                               + static_cast<std::size_t>(h_.count) * h_.rec_size;
        if (blob.size() < need) { err_ = StreamError::Truncated; return; }

        body_ = blob.subspan(sizeof(StreamHeader),
                             static_cast<std::size_t>(h_.count) * h_.rec_size);
        err_  = StreamError::Ok;
    }

    [[nodiscard]] StreamError error()   const { return err_; }
    [[nodiscard]] bool        ok()      const { return err_ == StreamError::Ok; }
    [[nodiscard]] std::uint32_t count() const { return ok() ? h_.count : 0u; }
    [[nodiscard]] std::uint16_t recSize() const { return h_.rec_size; }
    [[nodiscard]] std::uint16_t version() const { return h_.version; }
    [[nodiscard]] const StreamHeader& header() const { return h_; }

    /// True when the blob was written by a build that knows more fields than we
    /// do. Callers that re-save MUST round-trip the original bytes rather than
    /// re-encoding from Rec, or the unknown tail is silently dropped.
    [[nodiscard]] bool hasUnknownTail() const { return ok() && h_.rec_size > sizeof(Rec); }

    [[nodiscard]] Rec operator[](std::uint32_t i) const {
        Rec r{};                                    // older/narrower -> zero defaults
        const std::size_t n = std::min<std::size_t>(h_.rec_size, sizeof(Rec));
        std::memcpy(&r, body_.data() + static_cast<std::size_t>(i) * h_.rec_size, n);
        return r;                                   // newer/wider  -> tail skipped
    }

    [[nodiscard]] std::vector<Rec> all() const {
        std::vector<Rec> v;
        v.reserve(count());
        for (std::uint32_t i = 0; i < count(); ++i) v.push_back((*this)[i]);
        return v;
    }

private:
    StreamHeader h_{};
    std::span<const std::byte> body_{};
    StreamError err_ = StreamError::TooShort;
};

}  // namespace adi
