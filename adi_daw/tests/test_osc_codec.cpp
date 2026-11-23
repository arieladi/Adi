// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: unit tests for the self-contained OSC 1.0 decoder (osc_codec).

#include "adi/osc_codec.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace adi::osc;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what.c_str());
    }
}

void section(const char* name) {
    std::printf("[%s]\n", name);
}

// Local packet-builder helpers for tests (encoding stays outside osc_codec).
void appendU32BE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

void appendI32BE(std::vector<std::uint8_t>& out, std::int32_t v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    appendU32BE(out, bits);
}

void appendU64BE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    appendU32BE(out, static_cast<std::uint32_t>(v >> 32U));
    appendU32BE(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFULL));
}

void appendF32BE(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    appendU32BE(out, bits);
}

void appendOscString(std::vector<std::uint8_t>& out, std::string_view s) {
    for (char c : s) {
        out.push_back(static_cast<std::uint8_t>(c));
    }
    out.push_back(0U);
    while ((out.size() % 4U) != 0U) {
        out.push_back(0U);
    }
}

void appendOscBlob(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> data) {
    appendI32BE(out, static_cast<std::int32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    while ((out.size() % 4U) != 0U) {
        out.push_back(0U);
    }
}

std::vector<std::uint8_t> makeBundle(std::uint64_t timeTag,
                                     const std::vector<std::vector<std::uint8_t>>& elements) {
    std::vector<std::uint8_t> out;
    const char hdr[] = "#bundle";
    for (std::size_t i = 0; i < 8U; ++i) {
        out.push_back(static_cast<std::uint8_t>(hdr[i]));
    }
    appendU64BE(out, timeTag);
    for (const auto& elem : elements) {
        appendI32BE(out, static_cast<std::int32_t>(elem.size()));
        out.insert(out.end(), elem.begin(), elem.end());
    }
    return out;
}

std::vector<std::uint8_t> makeSimpleMessage(std::string_view addr, float v) {
    std::vector<std::uint8_t> out;
    appendOscString(out, addr);
    appendOscString(out, ",f");
    appendF32BE(out, v);
    return out;
}

void testValidMessages() {
    section("valid messages");

    // Zero arguments.
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/adi/transport/play");
        appendOscString(raw, ",");
        const auto r = decode(raw);
        check(r.ok(), "zero-arg message decodes");
        check(r.packet && r.packet->isMessage(), "zero-arg is message");
        if (r.packet && r.packet->asMessage()) {
            check(r.packet->asMessage()->address == "/adi/transport/play", "address matches");
            check(r.packet->asMessage()->args.empty(), "zero args");
        }
    }

    // All four OSC 1.0 argument types: i, f, s, b (plus edge padding lengths).
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/adi/track/1/test");
        appendOscString(raw, ",iiffssbb");
        appendI32BE(raw, -42);
        appendI32BE(raw, (std::numeric_limits<std::int32_t>::min)());
        appendF32BE(raw, 0.75f);
        appendF32BE(raw, -12.5f);
        appendOscString(raw, "");       // 1 NUL + 3 pad
        appendOscString(raw, "abcd");   // 4 bytes + 4 NUL pad
        const std::vector<std::uint8_t> blobEmpty{};
        const std::vector<std::uint8_t> blob5{0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
        appendOscBlob(raw, blobEmpty);
        appendOscBlob(raw, blob5);

        const auto r = decode(raw);
        check(r.ok(), "mixed ifsb message decodes");
        if (r.ok() && r.packet->asMessage()) {
            const auto& m = *r.packet->asMessage();
            check(m.args.size() == 8U, "8 arguments decoded");
            check(std::get<std::int32_t>(m.args[0]) == -42, "int32 -42");
            check(std::get<std::int32_t>(m.args[1]) == (std::numeric_limits<std::int32_t>::min)(),
                  "int32 min");
            check(std::get<float>(m.args[2]) == 0.75f, "float 0.75");
            check(std::get<float>(m.args[3]) == -12.5f, "float -12.5");
            check(std::get<std::string>(m.args[4]).empty(), "empty string");
            check(std::get<std::string>(m.args[5]) == "abcd", "4-char string");
            check(std::get<Blob>(m.args[6]).empty(), "empty blob");
            const auto& b5 = std::get<Blob>(m.args[7]);
            check(b5.size() == 5U && b5[0] == std::byte{0x01} && b5[4] == std::byte{0x05},
                  "5-byte blob bytes");
        }
    }

    // String padding boundaries: lengths 1, 2, 3, 4.
    for (std::size_t len = 1U; len <= 4U; ++len) {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/s");
        appendOscString(raw, ",s");
        std::string s(len, 'x');
        appendOscString(raw, s);
        const auto r = decode(raw);
        check(r.ok() && r.packet->asMessage() &&
                  std::get<std::string>(r.packet->asMessage()->args[0]) == s,
              "string padding len=" + std::to_string(len));
    }

    // Blob padding boundaries: lengths 0, 1, 2, 3, 4.
    for (std::size_t len = 0U; len <= 4U; ++len) {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/b");
        appendOscString(raw, ",b");
        std::vector<std::uint8_t> payload(len, 0xABU);
        appendOscBlob(raw, payload);
        const auto r = decode(raw);
        check(r.ok() && r.packet->asMessage() &&
                  std::get<Blob>(r.packet->asMessage()->args[0]).size() == len,
              "blob padding len=" + std::to_string(len));
    }
}

void testValidBundlesAndTimeTags() {
    section("valid bundles and time tags");

    // Empty bundle with immediate time tag.
    {
        const auto raw = makeBundle(1ULL, {});
        const auto r = decode(raw);
        check(r.ok() && r.packet->isBundle(), "empty immediate bundle decodes");
        if (r.ok() && r.packet->asBundle()) {
            const auto& b = *r.packet->asBundle();
            check(b.timeTag.isImmediate(), "timeTag is immediate");
            check(b.timeTag.seconds() == 0U && b.timeTag.fraction() == 1U, "immediate fields");
            check(b.elements.empty(), "empty elements");
            check(flattenMessages(*r.packet).empty(), "flatten empty bundle");
        }
    }

    // Nested bundle with explicit NTP time tag and depth-first ordering.
    {
        const std::uint64_t tag = (0x83AA7E80ULL << 32U) | 0x80000000ULL;
        const auto m1 = makeSimpleMessage("/adi/track/1/volume", 0.25f);
        const auto m2 = makeSimpleMessage("/adi/track/2/volume", 0.50f);
        const auto m3 = makeSimpleMessage("/adi/master/volume", 0.75f);
        const auto inner = makeBundle(1ULL, {m2});
        const auto outer = makeBundle(tag, {m1, inner, m3});

        const auto r = decode(outer);
        check(r.ok() && r.packet->isBundle(), "nested bundle decodes");
        if (r.ok() && r.packet->asBundle()) {
            const auto& b = *r.packet->asBundle();
            check(!b.timeTag.isImmediate(), "explicit time tag not immediate");
            check(b.timeTag.seconds() == 0x83AA7E80U, "NTP seconds");
            check(b.timeTag.fraction() == 0x80000000U, "NTP fraction");
            check(b.elements.size() == 3U, "outer has 3 elements");

            const auto flat = flattenMessages(*r.packet);
            check(flat.size() == 3U, "flatten returns 3 messages");
            if (flat.size() == 3U) {
                check(flat[0]->address == "/adi/track/1/volume", "flatten[0]");
                check(flat[1]->address == "/adi/track/2/volume", "flatten[1]");
                check(flat[2]->address == "/adi/master/volume", "flatten[2]");
            }
        }
    }
}

void testTruncatedMisalignedOversized() {
    section("truncated, misaligned, oversized packets");

    // Empty packet.
    {
        const std::vector<std::uint8_t> empty;
        const auto r = decode(empty);
        check(!r.ok() && r.error == DecodeError::EmptyPacket, "empty packet refused");
    }

    // Misaligned sizes.
    for (std::size_t sz : {1U, 2U, 3U, 5U, 7U, 13U}) {
        std::vector<std::uint8_t> raw(sz, static_cast<std::uint8_t>('/'));
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::MisalignedPacket,
              "misaligned size " + std::to_string(sz) + " refused");
    }

    // Oversized packet.
    {
        DecodeLimits lim;
        lim.maxPacketBytes = 32;
        std::vector<std::uint8_t> raw(36, 0U);
        raw[0] = static_cast<std::uint8_t>('/');
        const auto r = decode(raw, lim);
        check(!r.ok() && r.error == DecodeError::PacketTooLarge, "oversized packet refused");
    }

    // Every strict prefix of a rich valid message must be refused without OOB read.
    {
        std::vector<std::uint8_t> full;
        appendOscString(full, "/adi/track/1/device/1/param/2");
        appendOscString(full, ",ifsb");
        appendI32BE(full, 7);
        appendF32BE(full, 0.5f);
        appendOscString(full, "hello");
        const std::vector<std::uint8_t> b{1U, 2U, 3U, 4U, 5U};
        appendOscBlob(full, b);

        check(decode(full).ok(), "full message valid before truncation sweep");
        for (std::size_t cut = 0U; cut < full.size(); ++cut) {
            const auto r = decode(std::span<const std::uint8_t>(full.data(), cut));
            check(!r.ok(), "truncated message at " + std::to_string(cut) + " refused");
        }
    }

    // Every strict prefix of a nested bundle must be refused without OOB read,
    // except cut == 16 which is itself a complete empty bundle (#bundle\0 + timetag).
    {
        const auto inner = makeBundle(1ULL, {makeSimpleMessage("/adi/master/volume", 0.5f)});
        const auto full = makeBundle(42ULL, {inner});
        check(decode(full).ok(), "full bundle valid before truncation sweep");
        for (std::size_t cut = 0U; cut < full.size(); ++cut) {
            const auto r = decode(std::span<const std::uint8_t>(full.data(), cut));
            if (cut == 16U) {
                check(r.ok() && r.packet->isBundle() && r.packet->asBundle()->elements.empty() &&
                          r.packet->asBundle()->timeTag.raw == 42ULL,
                      "16-byte bundle prefix is a valid empty bundle");
            } else {
                check(!r.ok(), "truncated bundle at " + std::to_string(cut) + " refused");
            }
        }
    }
}

void testBundleDepthAndElementCaps() {
    section("bundle depth and element caps");

    auto nest = [](std::size_t depth) {
        std::vector<std::uint8_t> cur = makeSimpleMessage("/adi/master/volume", 0.5f);
        for (std::size_t d = 0U; d < depth; ++d) {
            cur = makeBundle(1ULL, {cur});
        }
        return cur;
    };

    // Default cap = 8: depth 8 succeeds, depth 9 fails.
    {
        const auto ok8 = decode(nest(8U));
        check(ok8.ok(), "depth 8 allowed under default cap 8");
        const auto fail9 = decode(nest(9U));
        check(!fail9.ok() && fail9.error == DecodeError::BundleDepthExceeded,
              "depth 9 refused with BundleDepthExceeded");
    }

    // Custom cap = 2: depth 2 succeeds, depth 3 fails.
    {
        DecodeLimits lim;
        lim.maxBundleDepth = 2;
        check(decode(nest(2U), lim).ok(), "depth 2 allowed under cap 2");
        const auto fail3 = decode(nest(3U), lim);
        check(!fail3.ok() && fail3.error == DecodeError::BundleDepthExceeded,
              "depth 3 refused under cap 2");
    }

    // Element count cap.
    {
        DecodeLimits lim;
        lim.maxElements = 3;
        const auto m = makeSimpleMessage("/adi/master/volume", 0.1f);
        check(decode(makeBundle(1ULL, {m, m, m}), lim).ok(), "3 elements allowed under cap 3");
        const auto r = decode(makeBundle(1ULL, {m, m, m, m}), lim);
        check(!r.ok() && r.error == DecodeError::TooManyElements,
              "4 elements refused under cap 3");
    }

    // Argument count cap.
    {
        DecodeLimits lim;
        lim.maxArguments = 2;
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/a");
        appendOscString(raw, ",iii");
        appendI32BE(raw, 1);
        appendI32BE(raw, 2);
        appendI32BE(raw, 3);
        const auto r = decode(raw, lim);
        check(!r.ok() && r.error == DecodeError::TooManyArguments,
              "3 args refused under cap 2");
    }
}

void testMalformedFields() {
    section("malformed addresses, padding, tags, blobs, bundles");

    // Address not starting with '/'.
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "adi/master/volume");
        appendOscString(raw, ",f");
        appendF32BE(raw, 0.5f);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidAddress, "address without leading /");
    }

    // Address with space or control char.
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/adi/bad path");
        appendOscString(raw, ",");
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidAddress, "address with space refused");
    }

    // Non-zero padding byte in address string.
    {
        std::vector<std::uint8_t> raw = {'/', 'a', 0x00U, 0xFFU, ',', 0x00U, 0x00U, 0x00U};
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidPadding, "non-zero address pad refused");
    }

    // Non-zero padding byte in blob.
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/b");
        appendOscString(raw, ",b");
        appendI32BE(raw, 1);
        raw.push_back(0xAAU);
        raw.push_back(0x00U);
        raw.push_back(0x01U);  // bad pad
        raw.push_back(0x00U);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidPadding, "non-zero blob pad refused");
    }

    // Missing comma in type tag.
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/a");
        appendOscString(raw, "f");
        appendF32BE(raw, 0.5f);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::MissingTypeTag, "type tag without comma refused");
    }

    // Unsupported type tag.
    {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/a");
        appendOscString(raw, ",T");
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::UnsupportedTypeTag,
              "unsupported tag refused");
    }

    // Negative and overflowing blob sizes.
    for (std::int32_t badLen : {-1, -4, (std::numeric_limits<std::int32_t>::min)()}) {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/b");
        appendOscString(raw, ",b");
        appendI32BE(raw, badLen);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidBlobSize,
              "negative blob size refused");
    }
    for (std::int32_t hugeLen : {0x7FFFFFFF, 0x7FFFFFFC, 100}) {
        std::vector<std::uint8_t> raw;
        appendOscString(raw, "/b");
        appendOscString(raw, ",b");
        appendI32BE(raw, hugeLen);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::Truncated,
              "overlong blob size refused without overflow");
    }

    // Trailing garbage bytes after message.
    {
        auto raw = makeSimpleMessage("/adi/master/volume", 0.5f);
        appendU32BE(raw, 0xDEADBEEFU);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::TrailingBytes, "trailing bytes refused");
    }

    // Bad bundle header.
    {
        std::vector<std::uint8_t> raw = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 'X',
                                         0U,  0U,  0U,  0U,  0U,  0U,  0U,  1U};
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidBundleHeader,
              "bad #bundle header refused");
    }

    // Invalid bundle element sizes (0, negative, misaligned, past end).
    for (std::int32_t badSize : {0, -4, 5}) {
        std::vector<std::uint8_t> raw = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0U};
        appendU64BE(raw, 1ULL);
        appendI32BE(raw, badSize);
        appendU32BE(raw, 0U);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::InvalidBundleElementSize,
              "invalid bundle element size refused");
    }
    {
        std::vector<std::uint8_t> raw = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0U};
        appendU64BE(raw, 1ULL);
        appendI32BE(raw, 64);
        appendU32BE(raw, 0U);
        const auto r = decode(raw);
        check(!r.ok() && r.error == DecodeError::Truncated,
              "bundle element extending past packet refused");
    }
}

}  // namespace

int main() {
    testValidMessages();
    testValidBundlesAndTimeTags();
    testTruncatedMisalignedOversized();
    testBundleDepthAndElementCaps();
    testMalformedFields();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
