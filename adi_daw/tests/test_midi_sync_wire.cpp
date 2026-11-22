// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sync/midi_sync_wire.hpp"

#include <cstdio>
#include <tuple>

namespace {
using namespace adi::sync;
using adi::time_base::Timecode;
using MidiByte = std::array<std::uint8_t, 1>;
using QuarterFrame = std::array<std::uint8_t, 2>;
int checks = 0, failures = 0;

void check(bool pass, const char *message) {
    ++checks;
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

QuarterFrame quarter(unsigned piece, std::uint8_t value) {
    return {0xf1, static_cast<std::uint8_t>((piece << 4) | value)};
}

void clockWire() {
    static_assert(ticksPerClock * 24 == 5'765'760);
    static_assert(ticksPerSongPosition == 1'441'440);
    bool exact = true;
    for (std::uint16_t position = 0; position <= maximumSongPosition; ++position) {
        const auto tick = position * ticksPerSongPosition;
        const auto bytes = encodeSongPosition(tick);
        exact = exact && bytes && (*bytes)[1] == (position & 0x7f) &&
                (*bytes)[2] == (position >> 7) && decodeClock(*bytes) == ClockEvent{ClockKind::SongPosition, tick};
    }
    check(exact, "every 14-bit SPP reaches the exact musical tick");
    check(!encodeSongPosition(-ticksPerSongPosition) && !encodeSongPosition(ticksPerClock) &&
              !encodeSongPosition((maximumSongPosition + 1) * ticksPerSongPosition),
          "SPP rejects negative, off-grid and out-of-range positions before narrowing");
    check(decodeClock(MidiByte{0xf8}) == ClockEvent{ClockKind::Clock} &&
              decodeClock(MidiByte{0xfa}) == ClockEvent{ClockKind::Start} &&
              decodeClock(MidiByte{0xfb}) == ClockEvent{ClockKind::Continue} &&
              decodeClock(MidiByte{0xfc}) == ClockEvent{ClockKind::Stop}, "decode MIDI realtime transport");
    check(!decodeClock({}) && !decodeClock(MidiByte{0xf2}) &&
              !decodeClock(std::array<std::uint8_t, 3>{0xf2, 0x80, 0}) &&
              !decodeClock(QuarterFrame{0xf8, 0}) && !decodeClock(MidiByte{0xfe}),
          "malformed MIDI and other realtime statuses do not reach the transport");
}

void timecodeWire() {
    QuarterFrameDecoder decoder;
    // 01:02:03:04 at 29.97 drop-frame.
    constexpr std::array<std::uint8_t, 8> dropLabel{4, 0, 3, 0, 2, 0, 1, 4};
    bool completeOnly = true;
    for (unsigned piece = 0; piece < 7; ++piece) {
        completeOnly = completeOnly && !decoder.receive(quarter(piece, dropLabel[piece])) &&
                       !decoder.locked() && !decoder.receive(MidiByte{0xf8});
    }
    const auto forward = decoder.receive(quarter(7, dropLabel[7]));
    check(completeOnly && decoder.locked() && forward && forward->direction == MtcDirection::Forward &&
              forward->rate == MtcRate::Drop2997 && forward->label == Timecode{1, 2, 3, 4} &&
              forward->frame == 111'582, "lock only on all eight forward pieces and keep exact drop-frame position");
    check(!decoder.receive(quarter(0, 4)) && decoder.locked() &&
              !decoder.receive(quarter(2, 3)) && !decoder.locked() &&
              !decoder.receive(quarter(3, 0)), "a skipped piece unlocks and discards the unfinished label");
    for (unsigned piece = 8; piece > 1; --piece)
        completeOnly = completeOnly && !decoder.receive(quarter(piece - 1, dropLabel[piece - 1]));
    const auto reverse = decoder.receive(quarter(0, dropLabel[0]));
    check(completeOnly && reverse && reverse->direction == MtcDirection::Reverse &&
              forward && reverse->frame == forward->frame && decoder.locked(),
          "reverse quarter-frame order locks at the same exact label");
    check(!decoder.receive(quarter(7, 8)) && !decoder.locked(),
          "reserved bits invalidate a locked quarter-frame stream");

    // Drop-frame minute one skips labels 00:01:00:00 and 00:01:00:01.
    constexpr std::array<std::uint8_t, 8> missingLabel{1, 0, 0, 0, 1, 0, 0, 4};
    bool rejected = true;
    for (unsigned piece = 0; piece < 8; ++piece)
        rejected = rejected && !decoder.receive(quarter(piece, missingLabel[piece]));
    check(rejected && !decoder.locked(), "reject skipped drop-frame labels");
    for (const auto &[wireRate, expectedRate, expectedFrame] : {
             std::tuple{0U, MtcRate::Fps24, 24}, std::tuple{2U, MtcRate::Fps25, 25},
             std::tuple{6U, MtcRate::Fps30, 30}}) {
        decoder.reset();
        for (unsigned piece = 0; piece < 7; ++piece) {
            const auto part = decoder.receive(quarter(piece, piece == 2 ? 1 : 0));
            check(!part, "incomplete non-drop label is held");
        }
        const auto position = decoder.receive(quarter(7, static_cast<std::uint8_t>(wireRate)));
        check(position && position->rate == expectedRate && position->frame == expectedFrame,
              "MTC rate bits select exact integer frame positions");
    }
    decoder.reset();
    check(!decoder.locked() && !decoder.receive(quarter(4, 3)) &&
              !decoder.receive(QuarterFrame{0xf1, 0x80}), "port reset and malformed messages clear previous assembly");
}
} // namespace

int main() {
    clockWire();
    timecodeWire();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
