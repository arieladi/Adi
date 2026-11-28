// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/midi_clip_file.hpp"

#include "adi/controller_lanes.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/clip_image.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <stdexcept>

namespace adi::interchange {
namespace {
constexpr std::int64_t maxTick = std::int64_t{1} << 50;
constexpr auto maxMidiFileBytes = 32 * 1024 * 1024;
constexpr std::size_t maxStreamEvents = 1u << 20;
// Ableton's single-clip export grid, independent of the source file's division.
constexpr std::int64_t exportPpq = 96;

std::string codecError(const smf::Error& error) {
    return "MIDI file: " + std::string(smf::issueName(error.issue)) + " at byte " + std::to_string(error.offset) + ".";
}

std::int64_t projectTicks(std::uint64_t tick, std::uint16_t ppq) {
    const auto whole = tick / ppq;
    if (whole > static_cast<std::uint64_t>(maxTick / textproj::kPPQ))
        throw std::runtime_error("The MIDI file exceeds the Arrangement time range.");
    const auto fraction = static_cast<std::int64_t>(tick % ppq) * textproj::kPPQ;
    const auto result = static_cast<std::int64_t>(whole) * textproj::kPPQ + (fraction + ppq / 2) / ppq;
    if (result >= maxTick)
        throw std::runtime_error("The MIDI file exceeds the Arrangement time range.");
    return result;
}

std::uint64_t fileTicks(std::int64_t tick) {
    return static_cast<std::uint64_t>((tick * exportPpq + textproj::kPPQ / 2) / textproj::kPPQ);
}

MidiFileTrack importTrack(const smf::Track& track, std::uint16_t ppq) {
    MidiFileTrack result;
    std::map<std::pair<int, int>, std::deque<std::size_t>> held;
    if (track.events.size() > maxStreamEvents)
        throw std::runtime_error("The MIDI track contains too many events.");
    for (const auto& event : track.events) {
        const auto tick = projectTicks(event.tick, ppq);
        result.length = tick;
        const int kind = event.status >> 4;
        const int channel = event.status & 15;
        if (kind == 0x9 && event.data[1] != 0) {
            const auto key = event.data[0];
            NoteRecord note{};
            note.start_ticks = tick;
            note.note_id = result.notes.size() + 1;
            note.key = key;
            note.channel = static_cast<std::uint8_t>(channel);
            note.vel_on = event.data[1];
            note.probability = 10000;
            held[{channel, key}].push_back(result.notes.size());
            result.notes.push_back(note);
        } else if (kind == 0x8 || kind == 0x9) {
            const auto address = std::pair<int, int>{channel, event.data[0]};
            const auto pending = held.find(address);
            if (pending == held.end() || pending->second.empty())
                continue;
            auto& note = result.notes[pending->second.front()];
            pending->second.pop_front();
            note.dur_ticks = std::max<std::int64_t>(1, tick - note.start_ticks);
            note.vel_off = kind == 0x8 ? event.data[1] : 0;
        } else if (kind == 0xb || kind == 0xe) {
            const bool pitch = kind == 0xe;
            const int controller = pitch ? controllers::kPitchBend : event.data[0];
            const auto word = static_cast<std::uint16_t>((static_cast<unsigned>(event.data[1]) << 7) | event.data[0]);
            const auto value = pitch ? controllers::pitchBendFromWord(word) : static_cast<double>(event.data[1]) / 127.0;
            auto& lane = result.controllers[{channel, controller}];
            if (!lane.empty() && lane.back().time == tick) {
                lane.back().value = value;
            } else {
                AutomationPoint point{};
                point.time = tick;
                point.point_id = lane.size() + 1;
                point.value = value;
                point.curve = static_cast<std::uint8_t>(Curve::Hold);
                lane.push_back(point);
            }
        }
    }
    for (auto& note : result.notes) {
        if (note.dur_ticks == 0)
            note.dur_ticks = std::max<std::int64_t>(1, result.length - note.start_ticks);
        result.length = std::max(result.length, note.start_ticks + note.dur_ticks);
    }
    for (const auto& [address, lane] : result.controllers) {
        (void)address;
        result.length = std::max(result.length, lane.back().time + 1);
    }
    return result;
}

Payload streamBytes(std::span<const std::byte> bytes) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return Payload::binary(std::vector<std::uint8_t>(data, data + bytes.size()));
}

int order(const smf::Event& event) {
    const auto type = event.status >> 4;
    if (type == 0xb || type == 0xe)
        return 0;
    return event.status < 0x90 ? 1 : 2;
}
} // namespace

MidiFileTracks readMidiClipFile(const std::filesystem::path& path) {
    try {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        const auto size = static_cast<std::streamoff>(in.tellg());
        if (size <= 0 || size > maxMidiFileBytes)
            throw std::runtime_error("The MIDI file is empty, unreadable or too large.");
        smf::Bytes bytes(static_cast<std::size_t>(size));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Could not read the MIDI file.");
        auto decoded = smf::read(bytes);
        if (const auto* error = std::get_if<smf::Error>(&decoded))
            return {{}, codecError(*error)};
        const auto& file = std::get<smf::File>(decoded);
        const auto* division = std::get_if<smf::Ppq>(&file.division);
        if (division == nullptr)
            throw std::runtime_error("MIDI files with SMPTE time division are not supported for Arrangement import.");
        MidiFileTracks result;
        for (const auto& chunk : file.chunks) {
            if (const auto* wire = std::get_if<smf::Track>(&chunk)) {
                auto track = importTrack(*wire, division->ticksPerQuarter);
                if (!track.notes.empty() || !track.controllers.empty())
                    result.tracks.push_back(std::move(track));
            }
        }
        if (result.tracks.empty())
            result.error = "The MIDI file has no notes or MIDI controllers to import.";
        return result;
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

OpRequest createImportedMidiClip(clip_edit::CopyIds& ids, const MidiFileTrack& source, std::int64_t track,
                                 std::int64_t clip, std::int64_t position, const std::string& name) {
    OpRequest request;
    request.opType = "capture.createMidi";
    request.label = "Import MIDI File";
    request.targetKind = "clip";
    request.targetId = clip;
    request.payload = {{"clip", {{"id", clip}, {"track", track}, {"kind", "midi"}, {"name", name},
                                 {"pos", position}, {"length", source.length}}},
                       {"controllers", Payload::array()}};
    if (!source.notes.empty()) {
        const auto bytes = writeStream<NoteRecord>(FourCC::Notes, source.notes, 1, StreamFlags::SortedByTime);
        request.payload["notes"] = {{"id", ids.next("event_streams")}, {"bytes", streamBytes(bytes)}};
    }
    for (const auto& [address, points] : source.controllers) {
        const auto bytes = writeStream<AutomationPoint>(FourCC::Controller, points, 1, StreamFlags::SortedByTime);
        request.payload["controllers"].push_back({{"id", ids.next("event_streams")}, {"channel", address.first},
            {"controller", address.second}, {"bytes", streamBytes(bytes)}});
    }
    return request;
}

smf::Bytes encodeMidiClipFile(const Store& store, std::int64_t clip, std::string& error) {
    try {
        const auto result = engine::readClipDetails(store, clip);
        if (!result.clip || result.clip->kind != "midi")
            throw std::runtime_error("Select one MIDI clip to export.");
        const auto& source = *result.clip;
        if (!source.notesProblem.empty())
            throw std::runtime_error(source.notesProblem);
        const auto start = source.startMarkerTicks;
        const auto length = source.lengthTicks.value_or(0);
        if (start < -maxTick || start > maxTick || length <= 0 || length > maxTick)
            throw std::runtime_error("The MIDI clip has an invalid length or start marker.");
        const auto end = source.endMarkerTicks.value_or(start + length);
        if (end <= start || end > 2 * maxTick || end - start > maxTick)
            throw std::runtime_error("The MIDI clip has an invalid end marker.");
        const auto lanes = controllers::readLanes(store, clip);
        if (!lanes.ok)
            throw std::runtime_error(lanes.error);
        smf::Track track;
        auto last = fileTicks(end - start);
        for (const auto& note : source.notes) {
            if (!note.active || note.startTicks >= end || note.durationTicks <= 0 ||
                note.startTicks < -maxTick || note.durationTicks > maxTick)
                continue;
            const auto from = std::max(start, note.startTicks);
            const auto to = std::min(end, note.startTicks + note.durationTicks);
            if (to <= from)
                continue;
            const auto on = fileTicks(from - start);
            const auto off = std::max(on + 1, fileTicks(to - start));
            last = std::max(last, off);
            track.events.push_back({on, static_cast<std::uint8_t>(0x90 | note.channel), 0, {note.key, note.velocity}});
            track.events.push_back({off, static_cast<std::uint8_t>(0x80 | note.channel), 0, {note.key, note.releaseVelocity}});
        }
        for (const auto& lane : lanes.lanes) {
            if (!lane.error.empty())
                throw std::runtime_error(lane.error);
            if (!lane.channel || *lane.channel < 0 || *lane.channel > 15 ||
                !lane.controller || !controllers::validController(*lane.controller))
                continue;
            for (const auto& point : lane.points) {
                if (point.timeTicks < start || point.timeTicks >= end)
                    continue;
                if (*lane.controller == controllers::kPitchBend) {
                    const auto word = static_cast<unsigned>(std::lround(controllers::pitchBendWordExact(point.value)));
                    track.events.push_back({fileTicks(point.timeTicks - start), static_cast<std::uint8_t>(0xe0 | *lane.channel), 0,
                                            {static_cast<std::uint8_t>(word & 0x7f), static_cast<std::uint8_t>(word >> 7)}});
                    continue;
                }
                const auto value = static_cast<std::uint8_t>(std::lround(std::clamp(point.value, 0.0, 1.0) * 127.0));
                track.events.push_back({fileTicks(point.timeTicks - start), static_cast<std::uint8_t>(0xb0 | *lane.channel), 0,
                                        {static_cast<std::uint8_t>(*lane.controller), value}});
            }
        }
        std::stable_sort(track.events.begin(), track.events.end(), [](const auto& a, const auto& b) {
            return a.tick == b.tick ? order(a) < order(b) : a.tick < b.tick;
        });
        track.events.push_back({last, 0xff, 0x2f, {}});
        smf::File file;
        file.division = smf::Ppq{exportPpq};
        file.chunks.emplace_back(std::move(track));
        auto encoded = smf::write(file);
        if (const auto* issue = std::get_if<smf::Error>(&encoded))
            throw std::runtime_error(codecError(*issue));
        error.clear();
        return std::get<smf::Bytes>(std::move(encoded));
    } catch (const std::exception& exception) {
        error = exception.what();
        return {};
    }
}

} // namespace adi::interchange
