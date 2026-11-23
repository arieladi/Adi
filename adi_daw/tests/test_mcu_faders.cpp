// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/mcu_faders.hpp"
#include "adi/engine/hui_wire.hpp"
#include "adi/engine/mackie_vpots.hpp"
#include "adi/engine/mcu_lcd.hpp"

#include <algorithm>
#include <cstdio>

namespace {
namespace hui = adi::engine::hui;
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

void huiFaders() {
    using HuiKind = hui::FaderEventKind;
    static_assert(hui::encodeFader(7, 0x237f) ==
                  hui::FaderPackets{hui::MidiPacket{0xb0, 0x07, 0x46},
                                    hui::MidiPacket{0xb0, 0x27, 0x7f}});
    hui::FaderDecoder decoder;
    bool exact = true;
    for (std::size_t strip = 0; strip < hui::faderCount; ++strip) {
        for (std::uint16_t value = 0; value <= hui::maximumFader; ++value) {
            const auto wire = hui::encodeFader(strip, value);
            exact = exact && wire && !decoder.receive((*wire)[0]) &&
                    decoder.receive((*wire)[1]) == hui::FaderEvent{HuiKind::Move, strip, value};
        }
    }
    check(exact, "all fourteen-bit positions round-trip across eight HUI strips");
    check(!decoder.receive(hui::MidiPacket{0xb0, 0x20, 0x11}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x00, 0x03}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x06, 0x09}) &&
              decoder.receive(hui::MidiPacket{0xb0, 0x20, 0x11}) ==
                  hui::FaderEvent{HuiKind::Move, 0, 0x191} &&
              decoder.receive(hui::MidiPacket{0xb0, 0x26, 0x04}) ==
                  hui::FaderEvent{HuiKind::Move, 6, 0x484} &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x26, 0x04}),
          "HUI interleaved fader pairs are isolated and unsolicited fine bytes are ignored");
    check(!decoder.receive(hui::MidiPacket{0xb0, 0x2f, 0x40}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x0f, 6}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x2f, 0x41}) &&
              decoder.receive(hui::MidiPacket{0xb0, 0x2f, 0x40}) ==
                  hui::FaderEvent{HuiKind::Touch, 6} &&
              decoder.receive(hui::MidiPacket{0xb0, 0x2f, 0}) ==
                  hui::FaderEvent{HuiKind::Release, 6} &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x0f, 8}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x2f, 0x40}),
          "only port zero of a HUI channel zone reports fader touch");
    check(!hui::encodeFader(8, 0) && !hui::encodeFader(0, 16384) &&
              !decoder.receive(std::array<std::uint8_t, 2>{0xb0, 0}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x80, 0}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0, 0x80}) &&
              !decoder.receive(hui::MidiPacket{0xb1, 0, 3}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x20, 4}),
          "HUI rejects invalid packets, extra strips and other MIDI channels");
    const bool prepared = !decoder.receive(hui::MidiPacket{0xb0, 5, 4}) &&
                          !decoder.receive(hui::MidiPacket{0xb0, 0x0f, 5});
    decoder.reset();
    check(prepared && !decoder.receive(hui::MidiPacket{0xb0, 0x25, 3}) &&
              !decoder.receive(hui::MidiPacket{0xb0, 0x2f, 0x40}),
          "reconnecting discards unfinished HUI faders and the previous button zone");
}

void huiHeartbeat() {
    hui::Heartbeat heartbeat;
    constexpr hui::MidiPacket ping{0x90, 0, 0};
    check(heartbeat.poll(50) == ping && !heartbeat.poll(50) &&
              !heartbeat.poll(1049) && heartbeat.poll(1050) == ping,
          "HUI sends the initial host ping and waits one second between pings");
    check(heartbeat.poll(7000) == ping && !heartbeat.poll(7000) &&
              !heartbeat.poll(7999) && heartbeat.poll(8000) == ping &&
              heartbeat.poll(12) == ping && !heartbeat.poll(12),
          "late polls and clock resets recover with a single HUI host ping");
    heartbeat.reset();
    check(heartbeat.poll(13) == ping && !heartbeat.poll(13),
          "a reconnected HUI port pings immediately");
}

void lcdWire() {
    LcdFrame frame;
    const bool formatted = frame.setStrip(0, 0, "Long label") &&
                           frame.setStrip(1, 7, "R\t");
    LcdFeedback output(LcdDevice::Main);
    const auto first = output.prepare(frame);
    check(formatted && first[0] && first[1], "an unknown MCU display prepares both full rows");
    if (!first[0] || !first[1])
        return;
    const auto top = first[0]->bytes(), bottom = first[1]->bytes();
    constexpr std::array<std::uint8_t, 7> header{0xf0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00};
    check(top.size() == 64 && bottom.size() == 64 &&
              std::ranges::equal(header, top.first(header.size())) &&
              top.back() == 0xf7 && bottom.back() == 0xf7 && bottom[6] == 0x38,
          "standard MCU display wire messages use the proper row addresses and SysEx command");
    constexpr std::array<std::uint8_t, 7> abbreviated{'L', 'o', 'n', 'g', ' ', 'l', ' '};
    check(std::ranges::equal(abbreviated, top.subspan(7, abbreviated.size())) &&
              bottom[7 + 49] == 'R' && bottom[7 + 50] == '?' && bottom[7 + 55] == ' ',
          "labels are six printable ASCII cells with a seven-byte pitch");
    output.sent(*first[0]);
    const auto retry = output.prepare(frame);
    check(!retry[0] && retry[1] && std::ranges::equal(retry[1]->bytes(), bottom),
          "a row whose send failed remains dirty independently of a confirmed row");
    output.sent(*first[1]);
    check(!output.prepare(frame)[0] && !output.prepare(frame)[1], "unchanged LCD frames send nothing");

    check(frame.setStrip(0, 0, "Lo") && frame.setStrip(0, 1, "Next") &&
              !frame.setStrip(0, lcdStrips, "invalid") && !frame.setLine(lcdRows, "invalid"),
          "valid label changes succeed and out-of-bounds display writes are refused");
    const auto updated = output.prepare(frame);
    constexpr std::array<std::uint8_t, 17> patch{0xf0, 0, 0, 0x66, 0x14, 0x12, 2,
                                                 ' ', ' ', ' ', ' ', ' ', 'N', 'e', 'x', 't', 0xf7};
    check(updated[0] && !updated[1] && std::ranges::equal(updated[0]->bytes(), patch),
          "coalesced row output changes the minimum span and clears shorter labels");
    if (updated[0])
        output.sent(*updated[0]);
    check(!output.prepare(frame)[0], "successful partial patches become the new feedback baseline");
    output.reset();
    const auto reconnected = output.prepare(frame);
    check(reconnected[0] && reconnected[1] && reconnected[0]->bytes().size() == 64,
          "reconnect discards the previous display baseline");

    LcdFeedback extender(LcdDevice::Extender);
    extender.sent(*first[0]);
    const auto extension = extender.prepare(frame);
    check(extension[0] && extension[1] && extension[0]->bytes()[4] == 0x15 &&
              extension[1]->bytes()[4] == 0x15,
          "MCU Extender uses its own SysEx device ID and baseline");
}

void vpotWire() {
    using namespace adi::engine::mackie;
    using Protocol = VpotProtocol;
    using Mode = VpotMode;
    bool exact = true;
    for (std::size_t strip = 0; strip < vpotCount; ++strip) {
        for (int step = 1; step <= 63; ++step) {
            const auto magnitude = static_cast<std::uint8_t>(step);
            const auto flagged = static_cast<std::uint8_t>(magnitude | 0x40);
            const auto mcuId = static_cast<std::uint8_t>(0x10 + strip);
            const auto huiId = static_cast<std::uint8_t>(0x40 + strip);
            exact = exact &&
                decodeVpotTurn(Protocol::Mcu, VpotPacket{0xb0, mcuId, magnitude}) == VpotTurn{strip, step} &&
                decodeVpotTurn(Protocol::Mcu, VpotPacket{0xb0, mcuId, flagged}) == VpotTurn{strip, -step} &&
                decodeVpotTurn(Protocol::Hui, VpotPacket{0xb0, huiId, magnitude}) == VpotTurn{strip, -step} &&
                decodeVpotTurn(Protocol::Hui, VpotPacket{0xb0, huiId, flagged}) == VpotTurn{strip, step};
        }
    }
    check(exact, "all MCU and HUI rotary magnitudes preserve their opposite wire polarities");
    check(!decodeVpotTurn(Protocol::Mcu, VpotPacket{0xb0, 0x10, 0}) &&
              !decodeVpotTurn(Protocol::Hui, VpotPacket{0xb0, 0x40, 0x40}) &&
              !decodeVpotTurn(Protocol::Mcu, VpotPacket{0xb0, 0x18, 1}) &&
              !decodeVpotTurn(Protocol::Hui, VpotPacket{0xb0, 0x48, 1}) &&
              !decodeVpotTurn(Protocol::Mcu, VpotPacket{0xb1, 0x10, 1}) &&
              !decodeVpotTurn(Protocol::Hui, VpotPacket{0xb0, 0x40, 0xff}) &&
              !decodeVpotTurn(Protocol::Mcu, std::array<std::uint8_t, 2>{0xb0, 0x10}),
          "no-step, malformed and unrelated strip packets are filtered");
    check(encodeVpotRing(Protocol::Mcu, 7, {Mode::Dot, 11, false}) == VpotPacket{0xb0, 0x37, 0x0b} &&
              encodeVpotRing(Protocol::Mcu, 1, {Mode::BoostCut, 6, true}) == VpotPacket{0xb0, 0x31, 0x56} &&
              encodeVpotRing(Protocol::Mcu, 3, {Mode::Spread, 6, false}) == VpotPacket{0xb0, 0x33, 0x36} &&
              encodeVpotRing(Protocol::Hui, 3, {Mode::Wrap, 10, false}) == VpotPacket{0xb0, 0x13, 0x2a} &&
              encodeVpotRing(Protocol::Hui, 0, {Mode::Wrap, 0, false}) == VpotPacket{0xb0, 0x10, 0},
          "ring feedback emits each display mode and protocol address");
    check(!encodeVpotRing(Protocol::Mcu, 8, {}) &&
              !encodeVpotRing(Protocol::Mcu, 1, {Mode::Spread, 7}) &&
              !encodeVpotRing(Protocol::Hui, 1, {Mode::Wrap, 12}) &&
              !encodeVpotRing(Protocol::Hui, 1, {Mode::Dot, 4, true}) &&
              !encodeVpotRing(Protocol::Mcu, 0, {static_cast<Mode>(4), 1}),
          "unsupported display values cannot become wire packets");
    VpotFeedback mcu(Protocol::Mcu), huiOutput(Protocol::Hui);
    constexpr VpotRing centered{Mode::Dot, 6};
    const auto dirty = mcu.prepare(5, centered);
    check(dirty && dirty == mcu.prepare(5, centered), "uncertain ring sends retry");
    if (!dirty)
        return;
    mcu.sent(*dirty);
    huiOutput.sent(*dirty);
    check(!mcu.prepare(5, centered) && mcu.prepare(4, centered).has_value() &&
              huiOutput.prepare(5, centered).has_value(),
          "confirmed ring state is scoped to one protocol and strip");
    mcu.reset();
    check(mcu.prepare(5, centered) == dirty, "reconnect republishes otherwise unchanged rings");
}
} // namespace

int main() {
    wireMessages();
    feedbackAndTouch();
    separateStrips();
    huiFaders();
    huiHeartbeat();
    lcdWire();
    vpotWire();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
