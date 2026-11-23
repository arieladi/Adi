// SPDX-License-Identifier: GPL-3.0-or-later
//
// SRF-09: self-contained OSC 1.0 packet decoder.

#include "adi/osc_codec.hpp"

#include <cstring>
#include <string_view>

namespace adi::osc {

namespace {

constexpr std::size_t kAlign = 4U;
constexpr std::size_t kBundleHeaderBytes = 8U;
constexpr std::size_t kTimeTagBytes = 8U;
constexpr std::size_t kMinBundleBytes = kBundleHeaderBytes + kTimeTagBytes;

[[nodiscard]] std::uint8_t byteAt(std::span<const std::byte> data, std::size_t index) noexcept {
    return std::to_integer<std::uint8_t>(data[index]);
}

[[nodiscard]] std::uint32_t readU32BE(std::span<const std::byte> data,
                                      std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(byteAt(data, offset)) << 24U) |
           (static_cast<std::uint32_t>(byteAt(data, offset + 1U)) << 16U) |
           (static_cast<std::uint32_t>(byteAt(data, offset + 2U)) << 8U) |
           static_cast<std::uint32_t>(byteAt(data, offset + 3U));
}

[[nodiscard]] std::int32_t readI32BE(std::span<const std::byte> data,
                                     std::size_t offset) noexcept {
    const std::uint32_t bits = readU32BE(data, offset);
    std::int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] float readF32BE(std::span<const std::byte> data, std::size_t offset) noexcept {
    const std::uint32_t bits = readU32BE(data, offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] std::uint64_t readU64BE(std::span<const std::byte> data,
                                      std::size_t offset) noexcept {
    const std::uint64_t hi = readU32BE(data, offset);
    const std::uint64_t lo = readU32BE(data, offset + 4U);
    return (hi << 32U) | lo;
}

[[nodiscard]] DecodeResult makeError(DecodeError err, std::string detail) {
    DecodeResult r;
    r.error = err;
    r.detail = std::move(detail);
    return r;
}

/// Read an OSC NUL-terminated 4-byte-aligned string from `data` at `offset`.
[[nodiscard]] DecodeError readOscString(std::span<const std::byte> data,
                                        std::size_t& offset,
                                        std::string& out,
                                        std::string& detail) {
    std::size_t nulPos = offset;
    while (nulPos < data.size() && byteAt(data, nulPos) != 0U) {
        ++nulPos;
    }
    if (nulPos >= data.size()) {
        detail = "unterminated OSC string";
        return DecodeError::Truncated;
    }

    const std::size_t lenWithNul = (nulPos - offset) + 1U;
    const std::size_t rem = lenWithNul % kAlign;
    const std::size_t paddedLen = rem == 0U ? lenWithNul : (lenWithNul + (kAlign - rem));
    if (paddedLen > data.size() - offset) {
        detail = "OSC string padding extends past packet";
        return DecodeError::Truncated;
    }

    // Every padding byte after the terminating NUL must be zero.
    for (std::size_t i = offset + lenWithNul; i < offset + paddedLen; ++i) {
        if (byteAt(data, i) != 0U) {
            detail = "non-zero padding byte in OSC string";
            return DecodeError::InvalidPadding;
        }
    }

    out.assign(reinterpret_cast<const char*>(data.data() + offset), nulPos - offset);
    offset += paddedLen;
    return DecodeError::None;
}

[[nodiscard]] bool isValidAddress(std::string_view address) noexcept {
    if (address.empty() || address[0] != '/') {
        return false;
    }
    for (char c : address) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x21U || uc > 0x7EU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasBundlePrefix(std::span<const std::byte> data) noexcept {
    static constexpr char kHeader[kBundleHeaderBytes] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    if (data.size() < kBundleHeaderBytes) {
        return false;
    }
    for (std::size_t i = 0U; i < kBundleHeaderBytes; ++i) {
        if (byteAt(data, i) != static_cast<std::uint8_t>(kHeader[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] DecodeResult decodeMessage(std::span<const std::byte> data,
                                         const DecodeLimits& limits) {
    std::size_t offset = 0U;
    Message msg;
    std::string detail;

    if (const auto err = readOscString(data, offset, msg.address, detail);
        err != DecodeError::None) {
        return makeError(err, std::move(detail));
    }
    if (!isValidAddress(msg.address)) {
        return makeError(DecodeError::InvalidAddress,
                         "OSC address must start with '/' and use printable non-space ASCII");
    }

    if (offset >= data.size()) {
        return makeError(DecodeError::MissingTypeTag, "OSC message is missing its type tag string");
    }

    std::string typeTags;
    if (const auto err = readOscString(data, offset, typeTags, detail);
        err != DecodeError::None) {
        return makeError(err, std::move(detail));
    }
    if (typeTags.empty() || typeTags[0] != ',') {
        return makeError(DecodeError::MissingTypeTag, "OSC type tag string must start with ','");
    }

    const std::size_t argCount = typeTags.size() - 1U;
    if (argCount > limits.maxArguments) {
        return makeError(DecodeError::TooManyArguments, "OSC message exceeds argument limit");
    }
    msg.args.reserve(argCount);

    for (std::size_t i = 1U; i < typeTags.size(); ++i) {
        const char tag = typeTags[i];
        switch (tag) {
            case 'i': {
                if (data.size() - offset < 4U) {
                    return makeError(DecodeError::Truncated, "truncated int32 argument");
                }
                msg.args.emplace_back(readI32BE(data, offset));
                offset += 4U;
                break;
            }
            case 'f': {
                if (data.size() - offset < 4U) {
                    return makeError(DecodeError::Truncated, "truncated float32 argument");
                }
                msg.args.emplace_back(readF32BE(data, offset));
                offset += 4U;
                break;
            }
            case 's': {
                std::string strArg;
                if (const auto err = readOscString(data, offset, strArg, detail);
                    err != DecodeError::None) {
                    return makeError(err, std::move(detail));
                }
                msg.args.emplace_back(std::move(strArg));
                break;
            }
            case 'b': {
                if (data.size() - offset < 4U) {
                    return makeError(DecodeError::Truncated, "truncated blob length");
                }
                const std::int32_t signedLen = readI32BE(data, offset);
                offset += 4U;
                if (signedLen < 0) {
                    return makeError(DecodeError::InvalidBlobSize, "negative OSC blob length");
                }
                const auto blobLen = static_cast<std::size_t>(signedLen);
                const std::size_t remaining = data.size() - offset;
                if (blobLen > remaining) {
                    return makeError(DecodeError::Truncated, "OSC blob extends past packet");
                }
                const std::size_t rem = blobLen % kAlign;
                const std::size_t pad = rem == 0U ? 0U : (kAlign - rem);
                if (pad > remaining - blobLen) {
                    return makeError(DecodeError::Truncated,
                                     "OSC blob padding extends past packet");
                }
                for (std::size_t p = 0U; p < pad; ++p) {
                    if (byteAt(data, offset + blobLen + p) != 0U) {
                        return makeError(DecodeError::InvalidPadding,
                                         "non-zero padding byte in OSC blob");
                    }
                }
                Blob blob(data.begin() + static_cast<std::ptrdiff_t>(offset),
                          data.begin() + static_cast<std::ptrdiff_t>(offset + blobLen));
                msg.args.emplace_back(std::move(blob));
                offset += blobLen + pad;
                break;
            }
            default:
                return makeError(DecodeError::UnsupportedTypeTag,
                                 std::string("unsupported OSC type tag '") + tag + "'");
        }
    }

    if (offset != data.size()) {
        return makeError(DecodeError::TrailingBytes, "unexpected trailing bytes after OSC message");
    }

    DecodeResult r;
    r.packet = Packet{std::move(msg)};
    return r;
}

[[nodiscard]] DecodeResult decodeElement(std::span<const std::byte> data,
                                         const DecodeLimits& limits,
                                         std::size_t bundleDepth,
                                         std::size_t& totalElements);

[[nodiscard]] DecodeResult decodeBundle(std::span<const std::byte> data,
                                        const DecodeLimits& limits,
                                        std::size_t bundleDepth,
                                        std::size_t& totalElements) {
    if (bundleDepth >= limits.maxBundleDepth) {
        return makeError(DecodeError::BundleDepthExceeded,
                         "OSC bundle nesting exceeds maximum depth");
    }
    if (data.size() < kBundleHeaderBytes) {
        return makeError(DecodeError::Truncated, "truncated OSC bundle header");
    }
    if (!hasBundlePrefix(data)) {
        return makeError(DecodeError::InvalidBundleHeader, "invalid OSC bundle header");
    }
    if (data.size() < kMinBundleBytes) {
        return makeError(DecodeError::Truncated, "truncated OSC bundle time tag");
    }

    Bundle bundle;
    bundle.timeTag = TimeTag{readU64BE(data, kBundleHeaderBytes)};

    std::size_t offset = kMinBundleBytes;
    while (offset < data.size()) {
        if (totalElements >= limits.maxElements) {
            return makeError(DecodeError::TooManyElements,
                             "OSC bundle exceeds maximum element count");
        }
        if (data.size() - offset < 4U) {
            return makeError(DecodeError::Truncated, "truncated OSC bundle element size");
        }
        const std::int32_t signedElemSize = readI32BE(data, offset);
        offset += 4U;
        if (signedElemSize <= 0 || (signedElemSize % static_cast<std::int32_t>(kAlign)) != 0) {
            return makeError(DecodeError::InvalidBundleElementSize,
                             "OSC bundle element size must be positive and 4-byte aligned");
        }
        const auto elemSize = static_cast<std::size_t>(signedElemSize);
        if (elemSize > data.size() - offset) {
            return makeError(DecodeError::Truncated, "OSC bundle element extends past packet");
        }

        ++totalElements;
        auto sub = decodeElement(data.subspan(offset, elemSize), limits, bundleDepth + 1U,
                                 totalElements);
        if (!sub.ok()) {
            return sub;
        }
        bundle.elements.push_back(std::move(*sub.packet));
        offset += elemSize;
    }

    DecodeResult r;
    r.packet = Packet{std::move(bundle)};
    return r;
}

[[nodiscard]] DecodeResult decodeElement(std::span<const std::byte> data,
                                         const DecodeLimits& limits,
                                         std::size_t bundleDepth,
                                         std::size_t& totalElements) {
    if (data.empty()) {
        return makeError(DecodeError::EmptyPacket, "empty OSC packet");
    }
    if ((data.size() % kAlign) != 0U) {
        return makeError(DecodeError::MisalignedPacket, "OSC packet size is not 4-byte aligned");
    }

    const std::uint8_t first = byteAt(data, 0U);
    if (first == static_cast<std::uint8_t>('#')) {
        return decodeBundle(data, limits, bundleDepth, totalElements);
    }
    if (first == static_cast<std::uint8_t>('/')) {
        return decodeMessage(data, limits);
    }
    return makeError(DecodeError::InvalidAddress,
                     "OSC packet must begin with '/' (message) or '#bundle'");
}

void collectMessages(const Packet& packet, std::vector<const Message*>& out) {
    if (const auto* msg = packet.asMessage()) {
        out.push_back(msg);
        return;
    }
    if (const auto* bun = packet.asBundle()) {
        for (const auto& elem : bun->elements) {
            collectMessages(elem, out);
        }
    }
}

}  // namespace

std::string_view toString(DecodeError err) noexcept {
    switch (err) {
        case DecodeError::None: return "none";
        case DecodeError::EmptyPacket: return "empty_packet";
        case DecodeError::PacketTooLarge: return "packet_too_large";
        case DecodeError::MisalignedPacket: return "misaligned_packet";
        case DecodeError::Truncated: return "truncated";
        case DecodeError::InvalidAddress: return "invalid_address";
        case DecodeError::InvalidPadding: return "invalid_padding";
        case DecodeError::MissingTypeTag: return "missing_type_tag";
        case DecodeError::UnsupportedTypeTag: return "unsupported_type_tag";
        case DecodeError::TooManyArguments: return "too_many_arguments";
        case DecodeError::InvalidBlobSize: return "invalid_blob_size";
        case DecodeError::TrailingBytes: return "trailing_bytes";
        case DecodeError::InvalidBundleHeader: return "invalid_bundle_header";
        case DecodeError::InvalidBundleElementSize: return "invalid_bundle_element_size";
        case DecodeError::BundleDepthExceeded: return "bundle_depth_exceeded";
        case DecodeError::TooManyElements: return "too_many_elements";
    }
    return "unknown";
}

DecodeResult decode(std::span<const std::byte> bytes, const DecodeLimits& limits) {
    if (bytes.empty()) {
        return makeError(DecodeError::EmptyPacket, "empty OSC packet");
    }
    if (bytes.size() > limits.maxPacketBytes) {
        return makeError(DecodeError::PacketTooLarge, "OSC packet exceeds maximum byte size");
    }
    if ((bytes.size() % kAlign) != 0U) {
        return makeError(DecodeError::MisalignedPacket, "OSC packet size is not 4-byte aligned");
    }

    std::size_t totalElements = 0U;
    return decodeElement(bytes, limits, 0U, totalElements);
}

DecodeResult decode(std::span<const std::uint8_t> bytes, const DecodeLimits& limits) {
    return decode(std::as_bytes(bytes), limits);
}

std::vector<const Message*> flattenMessages(const Packet& packet) {
    std::vector<const Message*> out;
    collectMessages(packet, out);
    return out;
}

}  // namespace adi::osc
