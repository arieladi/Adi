// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The MIDI ports (SRF-05; Live 12 manual 41.6 "MIDI Ports", the Tempo & MIDI Settings): what the port host
// (`src/juce/app_midi_ports.*`) needs that has no JUCE in it, so the tests run it as it is.
//
// - A port's switches as the Settings keep them: one record per port under its identifier in
//   `midi.ports` (the registry's home for the `midi.port.*` rows), read with the rows' defaults.
// - The events a port delivers, each timestamped, through one lock-free ring per port: one producer
//   (the port's own input thread) and one consumer (the engine's callback, or the window's frame). No
//   lock, no allocation once made.
#pragma once
#include "adi/settings/registry.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace adi::ui {
// Live's MIDI Ports table, one row per port and direction: Track (the port feeds tracks), Sync and
// Remote (it may be MIDI-mapped), and ADI's MPE.
struct MidiPortSwitches {
    bool track = false, sync = false, remote = false, mpe = false;
    friend bool operator==(const MidiPortSwitches &, const MidiPortSwitches &) = default;
};
// A switch's default: the registry's `midi.port.<field>` row (false where the row is missing).
inline bool midiPortDefault(const char *field) {
    const auto *row = settings::find(std::string("midi.port.") + field);
    return row != nullptr && row->defaultValue.is_boolean() && row->defaultValue.get<bool>();
}
// Port `id`'s switches from the `midi.ports` record: its own fields where set, the rows' defaults
// elsewhere.
inline MidiPortSwitches midiPortSwitches(const settings::Value &ports, const std::string &id) {
    MidiPortSwitches out{midiPortDefault("track"), midiPortDefault("sync"), midiPortDefault("remote"),
                         midiPortDefault("mpe")};
    if (!ports.is_object())
        return out;
    const auto it = ports.find(id);
    if (it == ports.end() || !it->is_object())
        return out;
    const auto read = [&](const char *field, bool &into) {
        if (const auto f = it->find(field); f != it->end() && f->is_boolean())
            into = f->get<bool>();
    };
    read("track", out.track);
    read("sync", out.sync);
    read("remote", out.remote);
    read("mpe", out.mpe);
    return out;
}
// The record with one switch of port `id` set: the value the Settings write back whole.
inline settings::Value withMidiPortSwitch(settings::Value ports, const std::string &id, const char *field, bool on) {
    if (!ports.is_object())
        ports = settings::Value::object();
    auto &record = ports[id];
    if (!record.is_object())
        record = settings::Value::object();
    record[field] = on;
    return ports;
}
// One MIDI 1.0 message as a port delivered it: up to three bytes (system exclusive is not carried),
// when it arrived in milliseconds of the host's high-resolution clock, and which port it came from.
struct MidiPortEvent {
    double timeMs = 0;
    std::uint32_t port = 0;
    std::uint8_t group = 0; // the UMP group: one of a multi-port device's jacks
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t size = 0;
};
// One producer, one consumer; `Capacity` a power of two. `push` refuses when full (the event is lost
// and counted), `pop` returns false when empty. Neither allocates, locks or waits.
template <std::size_t Capacity = 1024> class MidiEventRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "a power of two");

  public:
    bool push(const MidiPortEvent &e) noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[w & (Capacity - 1)] = e;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(MidiPortEvent &out) noexcept {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;
        out = slots_[r & (Capacity - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::array<MidiPortEvent, Capacity> slots_{};
    std::atomic<std::uint64_t> write_{0}, read_{0}, dropped_{0};
};
// What a MIDI 1.0 status byte's message carries: its length in bytes (1 to 3), or 0 for what a ring does
// not carry (system exclusive and its end, an undefined status, a data byte with no status).
inline std::uint8_t midiMessageSize(std::uint8_t status) noexcept {
    if (status < 0x80)
        return 0;
    switch (status & 0xf0) {
    case 0xc0:
    case 0xd0:
        return 2;
    case 0xf0:
        break;
    default:
        return 3;
    }
    switch (status) {
    case 0xf1:
    case 0xf3:
        return 2;
    case 0xf2:
        return 3;
    case 0xf6:
    case 0xf8:
    case 0xfa:
    case 0xfb:
    case 0xfc:
    case 0xfe:
    case 0xff:
        return 1;
    default:
        return 0; // 0xf0, 0xf7, and the undefined 0xf4, 0xf5, 0xf9, 0xfd
    }
}
// A Universal MIDI Packet's first word, received in the MIDI 1.0 protocol, as the MIDI 1.0 message it
// carries: message type 1 (system common and real time) and 2 (MIDI 1.0 channel voice) have their status
// and two data bytes in the word's low 24 bits. Other types (utility, system exclusive, MIDI 2.0, data)
// carry nothing a ring holds: false.
inline bool midiFromUmpWord(std::uint32_t word, MidiPortEvent &out) noexcept {
    const auto type = word >> 28;
    if (type != 1 && type != 2)
        return false;
    const auto status = static_cast<std::uint8_t>((word >> 16) & 0xff);
    const auto size = midiMessageSize(status);
    if (size == 0)
        return false;
    out.bytes = {status, static_cast<std::uint8_t>((word >> 8) & 0x7f), static_cast<std::uint8_t>(word & 0x7f)};
    for (std::size_t i = size; i < 3; ++i)
        out.bytes[i] = 0;
    out.size = size;
    out.group = static_cast<std::uint8_t>((word >> 24) & 0x0f);
    return true;
}
} // namespace adi::ui
