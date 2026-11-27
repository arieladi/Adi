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
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
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
// The record with one switch of port `id` set: the value the Settings write back whole. A port's `name`, when
// given, is kept with it (`midi.port.name`), so the record can be found again if the identifier changes.
inline settings::Value withMidiPortSwitch(settings::Value ports, const std::string &id, const char *field, bool on,
                                          const std::string &name = {}) {
    if (!ports.is_object())
        ports = settings::Value::object();
    auto &record = ports[id];
    if (!record.is_object())
        record = settings::Value::object();
    record[field] = on;
    if (!name.empty())
        record["name"] = name;
    return ports;
}
// A port as the system lists it now: its key in `midi.ports` ("in:" or "out:" and the system's identifier) and
// its name.
struct MidiPortListing {
    std::string id, name;
};
// The `midi.ports` record against the ports listed now (N61): each listed port's record keeps the port's name
// (`midi.port.name`), and a port with no record of its own adopts the record of the same direction and name that
// another identifier holds (one whose port is gone, or now lists another device), re-keyed to the port's new
// identifier, as a keyboard on another USB socket keeps its switches. A record of its own is one at its identifier
// that names it, or names nothing yet. Only an unambiguous match moves: one such record and one such port. Two
// ports of one name (two keyboards of a kind) stay apart, a port only adopts its own name, and a record at
// an identifier that now lists another device keeps its name, so its own device can still find it. Every move is
// read from the record as it was, so two devices that swapped sockets both find theirs. The new value, or nothing
// when the record already agrees. An initial Track value applies only to an input with no possible
// saved match. Save both on and off so preference changes leave the port's own Track switch alone.
inline std::optional<settings::Value> reconcileMidiPorts(const settings::Value &ports,
                                                         const std::vector<MidiPortListing> &listed,
                                                         std::optional<bool> trackForNewInputs = std::nullopt) {
    if (!ports.is_object())
        return std::nullopt;
    const auto direction = [](const std::string &id) { return id.substr(0, id.find(':') + 1); };
    const auto nameAt = [](const settings::Value &value, const std::string &id) -> std::optional<std::string> {
        const auto it = value.find(id);
        if (it == value.end() || !it->is_object())
            return std::nullopt; // no record
        const auto named = it->find("name");
        return named != it->end() && named->is_string() ? named->get<std::string>() : std::string();
    };
    const auto own = [&](const settings::Value &value, const MidiPortListing &port) {
        const auto name = nameAt(value, port.id);
        return name && (name->empty() || *name == port.name);
    };
    const auto listedAt = [&](const std::string &id) -> const MidiPortListing * {
        for (const auto &p : listed)
            if (p.id == id)
                return &p;
        return nullptr;
    };
    std::vector<std::pair<std::string, std::string>> moves; // from, to
    for (const auto &port : listed) {
        if (port.name.empty() || own(ports, port))
            continue;
        const auto same = [&](const std::string &id, const std::string &name) {
            return direction(id) == direction(port.id) && name == port.name;
        };
        std::vector<std::string> held;
        for (auto it = ports.begin(); it != ports.end(); ++it) {
            const auto name = nameAt(ports, it.key());
            if (it.key() == port.id || !name || !same(it.key(), *name))
                continue;
            const auto *there = listedAt(it.key());
            if (there == nullptr || !own(ports, *there)) // its port is gone, or lists another device
                held.push_back(it.key());
        }
        const auto unrecorded = std::count_if(listed.begin(), listed.end(), [&](const MidiPortListing &p) {
            return same(p.id, p.name) && !own(ports, p);
        });
        if (held.size() == 1 && unrecorded == 1)
            moves.emplace_back(held.front(), port.id);
    }
    auto out = ports;
    for (const auto &[from, to] : moves)
        out.erase(from);
    for (const auto &[from, to] : moves)
        out[to] = ports.at(from);
    bool changed = !moves.empty();
    for (const auto &port : listed) {
        const auto name = nameAt(out, port.id);
        if (port.name.empty() || !name || !name->empty())
            continue; // no record, or one that already names a device
        out[port.id]["name"] = port.name;
        changed = true;
    }
    if (trackForNewInputs.has_value()) {
        const auto records = ports.items();
        for (const auto &port : listed) {
            if (!port.id.starts_with("in:") || out.contains(port.id))
                continue;
            const bool previousName = !port.name.empty() && std::any_of(records.begin(), records.end(),
                [&](const auto &entry) {
                    return entry.key().starts_with("in:") && nameAt(ports, entry.key()) == port.name;
                });
            if (previousName)
                continue;
            auto &record = out[port.id];
            record["track"] = *trackForNewInputs;
            if (!port.name.empty())
                record["name"] = port.name;
            changed = true;
        }
    }
    if (!changed)
        return std::nullopt;
    return out;
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
template <std::size_t Capacity = 1024, class Event = MidiPortEvent> class MidiEventRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "a power of two");

  public:
    bool push(const Event &e) noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[w & (Capacity - 1)] = e;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(Event &out) noexcept {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;
        out = slots_[r & (Capacity - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    // Consumer only. Drop the queued prefix in bounded time while the producer stays live.
    void discardPending() noexcept { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::array<Event, Capacity> slots_{};
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
