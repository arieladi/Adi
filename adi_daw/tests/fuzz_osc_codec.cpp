// SPDX-License-Identifier: GPL-3.0-or-later
//
// libFuzzer harness for the OSC 1.0 packet decoder — SRF-09.
//
// THE PROPERTY UNDER TEST
//
//   No untrusted byte sequence causes a crash, hang, out-of-bounds read,
//   undefined behaviour, or violation of the decoder's structural invariants.
//
// Built automatically by CMakeLists.txt when ADI_BUILD_FUZZERS=ON against
// src/adi/osc_codec.cpp alone.

#include "adi/osc_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace {

using namespace adi::osc;

void verifyPacket(const Packet& pkt,
                  const DecodeLimits& limits,
                  std::size_t bundleDepth,
                  std::size_t& elementCount) {
    if (const auto* msg = pkt.asMessage()) {
        if (msg->address.empty() || msg->address[0] != '/') {
            __builtin_trap();
        }
        if (msg->args.size() > limits.maxArguments) {
            __builtin_trap();
        }
        for (const auto& arg : msg->args) {
            if (const auto* s = std::get_if<std::string>(&arg)) {
                volatile std::size_t n = s->size();
                (void)n;
            } else if (const auto* b = std::get_if<Blob>(&arg)) {
                volatile std::size_t n = b->size();
                (void)n;
            }
        }
        return;
    }

    const auto* bun = pkt.asBundle();
    if (!bun) {
        __builtin_trap();
    }
    const std::size_t nextDepth = bundleDepth + 1U;
    if (nextDepth > limits.maxBundleDepth) {
        __builtin_trap();
    }
    for (const auto& elem : bun->elements) {
        ++elementCount;
        if (elementCount > limits.maxElements) {
            __builtin_trap();
        }
        verifyPacket(elem, limits, nextDepth, elementCount);
    }
}

void exercise(std::span<const std::uint8_t> input, const DecodeLimits& limits) {
    const DecodeResult r = decode(input, limits);
    if (!r.ok()) {
        if (r.error == DecodeError::None || r.packet.has_value()) {
            __builtin_trap();
        }
        return;
    }
    if (r.error != DecodeError::None || !r.packet.has_value()) {
        __builtin_trap();
    }
    if (input.empty() || input.size() > limits.maxPacketBytes || (input.size() % 4U) != 0U) {
        __builtin_trap();
    }

    std::size_t elementCount = 0U;
    verifyPacket(*r.packet, limits, 0U, elementCount);

    const auto flat = flattenMessages(*r.packet);
    for (const Message* m : flat) {
        if (!m || m->address.empty() || m->address[0] != '/') {
            __builtin_trap();
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);

    // Default production limits.
    exercise(input, DecodeLimits{});

    // Tight limits to exercise cap refusals on small fuzzer inputs.
    DecodeLimits tight;
    tight.maxPacketBytes = 64;
    tight.maxBundleDepth = 2;
    tight.maxElements = 4;
    tight.maxArguments = 4;
    exercise(input, tight);

    return 0;
}
