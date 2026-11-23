// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/mcu_faders.hpp"

#include <cstdio>

namespace {
using namespace adi::engine::mcu;
using Kind = FaderEventKind;
int checks = 0, failures = 0;

void check(bool good, const char *message) {
    ++checks;
    if (!good) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

void wireMessages() {
    static_assert(encodeFader(7, 0x237f) == MidiPacket{0xe7, 0x7f, 0x46});
    static_assert(decodeFader(MidiPacket{0xe8, 0x7f, 0x7f}) == FaderEvent{Kind::Move, 8, 16383});
    bool exact = true;
    for (std::size_t strip = 0; strip < faderCount; ++strip) {
        for (std::uint16_t value = 0; value <= maximumFader; ++value) {
            const auto wire = encodeFader(strip, value);
            exact = exact && wire && decodeFader(*wire) == FaderEvent{Kind::Move, strip, value};
        }
    }
    check(exact, "all fourteen-bit fader positions round-trip on every channel and Main");
    check(decodeFader(MidiPacket{0x90, 0x68, 0x7f}) == FaderEvent{Kind::Touch, 0, 0} &&
              decodeFader(MidiPacket{0x90, 0x70, 0}) == FaderEvent{Kind::Release, 8, 0} &&
              decodeFader(MidiPacket{0x80, 0x69, 1}) == FaderEvent{Kind::Release, 1, 0},
          "touch notes and both release encodings retain their strip");
    const std::array<std::uint8_t, 2> partial{0xe0, 0};
    check(!encodeFader(9, 0) && !encodeFader(0, 16384) && !decodeFader(partial) &&
              !decodeFader(MidiPacket{0xe0, 0x80, 0}) && !decodeFader(MidiPacket{0xe9, 0, 0}) &&
              !decodeFader(MidiPacket{0x91, 0x68, 127}) &&
              !decodeFader(MidiPacket{0xb0, 0x10, 0x40}),
          "invalid lengths, data bytes and unrelated MCU messages are refused");
}

void feedbackAndTouch() {
    FaderFeedback unit;
    const auto older = unit.motor(2, 1222), newer = unit.motor(2, 1223);
    check(older == encodeFader(2, 1222) && newer == encodeFader(2, 1223) &&
              !unit.motor(2, 1223),
          "distinct motor targets produce complete packets and unchanged targets are suppressed");
    if (!older || !newer)
        return;
    check(!unit.receive(*newer) && !unit.receive(*older) && !unit.motor(2, 1223),
          "late intermediate servo acknowledgments never produce duplicate user moves");
    check(unit.receive(MidiPacket{0x90, 0x6a, 127}) == FaderEvent{Kind::Touch, 2, 0} &&
              !unit.receive(MidiPacket{0x90, 0x6a, 127}) && !unit.motor(2, 1400),
          "touch transitions are deduplicated and a touched fader receives no motor command");
    check(unit.receive(*newer) == FaderEvent{Kind::Move, 2, 1223} &&
              unit.receive(*encodeFader(2, 1400)) == FaderEvent{Kind::Move, 2, 1400},
          "touched fader movement reaches the host even at a previous motor target");
    check(unit.receive(MidiPacket{0x90, 0x6a, 0}) == FaderEvent{Kind::Release, 2, 0} &&
              !unit.motor(2, 1400),
          "release keeps the last physical reading for feedback diffing");
    const auto resumed = unit.motor(2, 1550);
    check(resumed && !unit.receive(*resumed) &&
              unit.receive(*encodeFader(2, 1450)) == FaderEvent{Kind::Move, 2, 1450},
          "motor movement resumes after release, then unrelated physical motion passes");
}

void separateStrips() {
    FaderFeedback unit;
    const auto left = unit.motor(0, 1000), main = unit.motor(8, 1000);
    check(left && main && !unit.receive(*main) &&
              unit.receive(*encodeFader(1, 1000)) == FaderEvent{Kind::Move, 1, 1000} &&
              !unit.receive(*left), "identical positions never consume another fader's echo");
    bool sent = true;
    for (std::uint16_t value = 0; value < 40; ++value)
        sent = sent && unit.motor(3, value).has_value();
    check(sent && !unit.receive(*encodeFader(3, 37)) && !unit.receive(*encodeFader(3, 39)) &&
              !unit.motor(3, 39), "bounded echo history keeps the newest targets under feedback pressure");
}
} // namespace

int main() {
    wireMessages();
    feedbackAndTouch();
    separateStrips();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
