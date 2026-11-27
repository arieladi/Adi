// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_midi_smf.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"

#include <string>
#include <utility>

namespace adi::clip_edit {
namespace {
namespace smf = interchange::smf;

std::uint64_t smfTick(std::int64_t tick, std::uint16_t ppq) noexcept {
    constexpr auto projectPpq = static_cast<std::uint64_t>(textproj::kPPQ);
    const auto local = static_cast<std::uint64_t>(tick);
    // Split before multiplying so the project tick limit fits on all platforms.
    return local / projectPpq * ppq + (local % projectPpq * ppq + projectPpq / 2) / projectPpq;
}

} // namespace

PitchMidiResult<interchange::smf::Bytes> encodePitchMidiChannelBendSmf(
    const PitchMidiProjection& projection, PitchMidiBendMode mode, int rangeSemitones,
    const PitchMidiSmfOptions& options) {
    if (audio::onAudioThread) return {PitchMidiProblem::Realtime, {}, {}};
    if (options.stop.stop_requested()) return {PitchMidiProblem::Cancelled, {}, {}};
    if (options.ticksPerQuarter == 0 || options.ticksPerQuarter > 0x7fff)
        return {PitchMidiProblem::Edits, {}, "SMF PPQ must be 1 through 32767"};
    if (options.microsecondsPerQuarter &&
        (*options.microsecondsPerQuarter == 0 || *options.microsecondsPerQuarter > 0xffffff))
        return {PitchMidiProblem::Edits, {}, "SMF tempo must be 1 through 16777215 microseconds per quarter"};

    auto bends = projectPitchMidiChannelBend(projection, mode, rangeSemitones, options.stop);
    if (!bends) return {bends.problem, {}, std::move(bends.detail)};
    for (const auto& note : projection.notes) {
        if (options.stop.stop_requested()) return {PitchMidiProblem::Cancelled, {}, {}};
        if (note.key > 127 || note.vel_on == 0 || note.vel_on > 127 || note.vel_off > 127)
            return {PitchMidiProblem::Edits, {}, "invalid MIDI key or note velocity for SMF"};
        if (smfTick(note.start_ticks, options.ticksPerQuarter) ==
            smfTick(note.start_ticks + note.dur_ticks, options.ticksPerQuarter))
            return {PitchMidiProblem::Edits, {}, "a projected note needs a higher SMF PPQ for positive duration"};
    }

    smf::Track track;
    track.events.reserve(bends.data->events.size() + projection.notes.size() * 2 + 8);
    if (options.microsecondsPerQuarter) {
        const auto tempo = *options.microsecondsPerQuarter;
        track.events.push_back({0, 0xff, 0x51, {static_cast<std::uint8_t>(tempo >> 16),
            static_cast<std::uint8_t>(tempo >> 8), static_cast<std::uint8_t>(tempo)}});
    }
    const auto add = [&track, channel = bends.data->channel](std::uint64_t tick, std::uint8_t status,
                                                            std::uint8_t first, std::uint8_t second) {
        track.events.push_back({tick, static_cast<std::uint8_t>(status | channel), 0, {first, second}});
    };
    // MIDI RPN 0,0 is Pitch Bend Sensitivity. Data Entry is semitones and cents.
    add(0, 0xb0, 101, 0);
    add(0, 0xb0, 100, 0);
    add(0, 0xb0, 6, static_cast<std::uint8_t>(bends.data->rangeSemitones));
    add(0, 0xb0, 38, 0);
    add(0, 0xb0, 101, 127);
    add(0, 0xb0, 100, 127);

    using Kind = PitchMidiBendEvent::Kind;
    auto note = projection.notes.begin();
    for (const auto& bend : bends.data->events) {
        if (options.stop.stop_requested()) return {PitchMidiProblem::Cancelled, {}, {}};
        const auto tick = smfTick(bend.tick, options.ticksPerQuarter);
        if (bend.kind == Kind::ReleaseCenter) add(tick, 0x80, note->key, note->vel_off);
        add(tick, 0xe0, static_cast<std::uint8_t>(bend.word & 0x7f), static_cast<std::uint8_t>(bend.word >> 7));
        if (bend.kind == Kind::Attack) add(tick, 0x90, note->key, note->vel_on);
        if (bend.kind == Kind::ReleaseCenter) ++note;
    }
    track.events.push_back({smfTick(projection.lengthTicks, options.ticksPerQuarter), 0xff, 0x2f, {}});
    smf::File file;
    file.division = smf::Ppq{options.ticksPerQuarter};
    file.chunks.emplace_back(std::move(track));
    auto encoded = smf::write(file);
    if (options.stop.stop_requested()) return {PitchMidiProblem::Cancelled, {}, {}};
    if (const auto* error = std::get_if<smf::Error>(&encoded))
        return {PitchMidiProblem::Edits, {}, "cannot encode pitch MIDI as SMF: " +
            std::string(smf::issueName(error->issue))};
    return {PitchMidiProblem::None, std::get<smf::Bytes>(std::move(encoded)), {}};
}

} // namespace adi::clip_edit
