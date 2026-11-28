// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/clap_block_wire.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace adi::engine;
using namespace adi::engine::clap_wire;

bool check(bool result, const char* reason) {
    if (!result) std::fprintf(stderr, "%s\n", reason);
    return result;
}

Block sourceBlock() {
    Block block;
    block.sequence = 0x1122334455667788ULL;
    block.channels = 2;
    block.frames = 8;
    block.audio = {0.0f, -0.0f, 0.125f, -0.125f, 0.5f, -0.5f, 0.9f, 1.0f,
                   0.0f, 0.2f,   0.3f,    0.4f, 0.5f,  0.6f, 0.7f,  0.8f};
    DeviceTransport position;
    position.sample = 12000;
    position.bar = 3;
    position.quarterNotes = 12.25;
    position.bpm = 123.125;
    position.numerator = 7;
    position.denominator = 8;
    position.barStart = 12;
    position.playing = true;
    position.recording = true;
    position.looping = true;
    position.loopBegin = 6000;
    position.loopEnd = 18000;
    position.loopBeginQuarterNotes = 12;
    position.loopEndQuarterNotes = 13;
    position.nextChange = 12512;
    block.transport = position;

    // Same port, channel, pitch and frame, with independent fractional notes.
    block.events = {
        {.kind = Kind::NoteOn, .frame = 2, .flags = 1, .noteId = 101, .port = 3, .channel = 4, .key = 69, .value = .85},
        {.kind = Kind::NoteOn, .frame = 2, .noteId = 102, .port = 3, .channel = 4, .key = 69, .value = .31},
        {.kind = Kind::NoteExpression, .frame = 2, .noteId = 102, .port = 3, .channel = 4, .key = 69,
            .selector = 2, .value = 0.183647123456789},
        {.kind = Kind::NoteExpression, .frame = 2, .flags = 2, .noteId = 101, .port = 3, .channel = 4, .key = 69,
            .selector = 2, .value = -0.127839123456789},
        {.kind = Kind::ParamValue, .frame = 4, .selector = 0x81233451, .value = -20.1234567891011},
        {.kind = Kind::ParamMod, .frame = 4, .noteId = 101, .port = 3, .channel = 4, .key = 69,
            .selector = 0x81233451, .value = .023456789123456},
        {.kind = Kind::NormalizedParamValue, .frame = 4, .selector = 0x81233451, .value = .414123456789},
        {.kind = Kind::NormalizedParamMod, .frame = 4, .selector = 0x81233451, .value = -.14123456789},
        {.kind = Kind::Controller, .frame = 5, .port = 0, .channel = 4, .selector = 74, .value = .123456789123456},
        {.kind = Kind::Midi1, .frame = 5, .port = 65535, .midi = {0x007f4594}},
        {.kind = Kind::Midi2, .frame = 5, .port = 1, .midi = {0x41944500, 0xfefffeae, 0, 0}},
        {.kind = Kind::NoteOff, .frame = 7, .noteId = 102, .port = 3, .channel = 4, .key = 69, .value = .11},
        {.kind = Kind::NoteChoke, .frame = 7, .noteId = 101},
        {.kind = Kind::NoteEnd, .frame = 7, .noteId = 101, .port = 3, .channel = 4, .key = 69},
        {.kind = Kind::ParamGestureBegin, .frame = 7, .selector = 0x81233451},
        {.kind = Kind::ParamGestureEnd, .frame = 7, .selector = 0x81233451},
    };
    return block;
}

} // namespace

int main() {
    const auto input = sourceBlock();
    std::vector<std::byte> packet;
    std::string error;
    if (!encodeBlock(input, packet, error)) {
        std::fprintf(stderr, "encode failed: %s\n", error.c_str());
        return 1;
    }
    std::size_t packetSize = 0;
    bool okay = check(blockPacketSize(std::span(packet).first(kBlockHeaderBytes), packetSize, error) &&
                      packetSize == packet.size(), "framed size differs from encoded packet");
    okay &= check(packet[8] == std::byte{0x88} && packet[15] == std::byte{0x11},
                  "sequence is not little-endian");
    const std::array tuningBytes{std::byte{0x53}, std::byte{0x2d}, std::byte{0xa0}, std::byte{0xba},
                                 std::byte{0xbf}, std::byte{0x81}, std::byte{0xc7}, std::byte{0x3f}};
    okay &= check(packet.size() == 1088 && std::equal(tuningBytes.begin(), tuningBytes.end(), packet.begin() + 336),
                  "native v1 wire layout changed");

    Block decoded;
    if (!decodeBlock(packet, decoded, error)) {
        std::fprintf(stderr, "decode failed: %s\n", error.c_str());
        return 1;
    }
    okay &= check(decoded.sequence == input.sequence && decoded.channels == 2 && decoded.frames == 8,
                  "block identity or shape changed");
    okay &= check(decoded.audio == input.audio && std::signbit(decoded.audio[1]), "float PCM changed");
    okay &= check(decoded.events == input.events, "ordered events or voice address fields changed");
    okay &= check(std::bit_cast<std::uint64_t>(decoded.events[2].value) ==
                  std::bit_cast<std::uint64_t>(input.events[2].value) &&
                  std::bit_cast<std::uint64_t>(decoded.events[3].value) ==
                  std::bit_cast<std::uint64_t>(input.events[3].value), "same-key tuning lost precision");
    okay &= check(decoded.transport && decoded.transport->sample == 12000 && decoded.transport->bar == 3 &&
                  decoded.transport->bpm == 123.125 && decoded.transport->numerator == 7 &&
                  decoded.transport->denominator == 8 && decoded.transport->recording &&
                  decoded.transport->loopEnd == 18000 && decoded.transport->nextChange == 12512,
                  "transport metadata changed");
    std::vector<std::byte> reencoded;
    okay &= check(encodeBlock(decoded, reencoded, error) && reencoded == packet,
                  "roundtrip is not byte-stable");

    Block unchanged;
    unchanged.sequence = 7;
    for (std::size_t i = 0; i < packet.size(); ++i) {
        if (decodeBlock(std::span(packet).first(i), unchanged, error) || unchanged.sequence != 7) {
            std::fprintf(stderr, "accepted truncated packet at byte %zu\n", i);
            okay = false;
            break;
        }
    }
    for (const auto corruptOffset : {4u, 7u, 19u, 23u, 27u, 31u}) {
        auto corrupt = packet;
        corrupt[corruptOffset] = std::byte{0xff};
        okay &= check(!decodeBlock(corrupt, unchanged, error) && unchanged.sequence == 7,
                      "decoded an invalid header");
    }
    auto oversized = packet;
    oversized.push_back(std::byte{});
    okay &= check(!decodeBlock(oversized, unchanged, error), "accepted packet with trailing bytes");

    auto invalid = input;
    invalid.events[3].frame = 1;
    okay &= check(!encodeBlock(invalid, reencoded, error) && reencoded == packet,
                  "accepted reordered events or mutated output on failure");
    invalid = input;
    invalid.events[2].value = std::numeric_limits<double>::quiet_NaN();
    okay &= check(!encodeBlock(invalid, reencoded, error), "accepted nonfinite event");
    invalid.events[2].value = 121;
    okay &= check(!encodeBlock(invalid, reencoded, error), "accepted tuning outside the CLAP range");
    invalid = input;
    invalid.audio[3] = std::numeric_limits<float>::infinity();
    okay &= check(!encodeBlock(invalid, reencoded, error), "accepted nonfinite PCM");
    invalid = input;
    invalid.events[1].port = -1;
    okay &= check(!encodeBlock(invalid, reencoded, error), "accepted wildcard note-on port");
    invalid = input;
    invalid.transport->bpm = 0;
    okay &= check(!encodeBlock(invalid, reencoded, error), "accepted invalid tempo");

    Block synth;
    synth.sequence = 1;
    synth.frames = 64;
    okay &= check(encodeBlock(synth, reencoded, error) && decodeBlock(reencoded, decoded, error) &&
                  !decoded.transport && decoded.audio.empty() && decoded.events.empty(),
                  "zero-input instrument packet did not preserve shape");

    if (okay) std::printf("%zu truncations refused; %zu ordered events preserved; same-key tuning error 0 cents\n",
                          packet.size(), input.events.size());
    return okay ? 0 : 1;
}
