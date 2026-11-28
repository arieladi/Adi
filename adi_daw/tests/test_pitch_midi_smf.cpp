// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/pitch_midi_smf.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
using namespace adi;
using namespace adi::clip_edit;
namespace smf = interchange::smf;
constexpr auto quarter = textproj::kPPQ;
int checks = 0, failures = 0;

void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

ExpressionPoint point(std::int64_t tick, float value) {
    ExpressionPoint result{};
    result.time_ticks = tick;
    result.value = value;
    result.curve = static_cast<std::uint8_t>(Curve::Linear);
    return result;
}

PitchMidiProjection fixture() {
    PitchMidiProjection result;
    result.positionTicks = 17 * quarter;
    result.lengthTicks = 2 * quarter;
    NoteRecord voice{};
    voice.channel = 3;
    voice.key = 57;
    voice.start_ticks = quarter / 8;
    voice.dur_ticks = quarter / 2 + quarter / 64;
    voice.note_id = 1;
    voice.vel_on = 89;
    voice.vel_off = 45;
    voice.tuning_cents = -100;
    result.notes.push_back(voice);
    voice.key = 60;
    voice.start_ticks += voice.dur_ticks;
    voice.dur_ticks = quarter - voice.start_ticks;
    voice.note_id = 2;
    voice.tuning_cents = 200;
    result.notes.push_back(voice);
    result.pitchCurves = {{1, {point(0, -1), point(quarter / 4, 0.5f), point(result.notes[0].dur_ticks, 2)}},
                         {2, {point(0, 0), point(voice.dur_ticks, 0)}}};
    return result;
}

bool message(const smf::Event& event, std::uint64_t tick, std::uint8_t status,
             std::uint8_t first, std::uint8_t second) {
    return event.tick == tick && event.status == status && event.data == smf::Bytes{first, second};
}

std::uint16_t word(const smf::Event& event) {
    return static_cast<std::uint16_t>(event.data[0] | (event.data[1] << 7));
}

void roundTrip() {
    const auto projected = fixture();
    const auto encoded = encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Continuous, 2, {4, 600000});
    check(static_cast<bool>(encoded), "export constructs format zero bytes");
    if (!encoded) return;
    const auto decoded = smf::read(*encoded.data);
    const auto* file = std::get_if<smf::File>(&decoded);
    check(file && file->format == 0 && std::holds_alternative<smf::Ppq>(file->division) &&
          std::get<smf::Ppq>(file->division).ticksPerQuarter == 4 && file->chunks.size() == 1 &&
          std::holds_alternative<smf::Track>(file->chunks.front()), "SMF parser reads one PPQ track");
    if (!file || file->chunks.size() != 1 || !std::holds_alternative<smf::Track>(file->chunks.front())) return;
    const auto& events = std::get<smf::Track>(file->chunks.front()).events;
    check(events.size() > 15 && events.front().tick == 0 && events.front().status == 0xff &&
          events.front().metaType == 0x51 && events.front().data == smf::Bytes{9, 39, 192},
          "explicit tempo encodes exactly at zero");
    if (events.size() <= 15) return;
    constexpr std::array<std::array<std::uint8_t, 2>, 6> setup{{{101, 0}, {100, 0}, {6, 2},
                                                               {38, 0}, {101, 127}, {100, 127}}};
    bool rangeSetup = true;
    for (std::size_t i = 0; i < setup.size(); ++i)
        rangeSetup = message(events[1 + i], 0, 0xb3, setup[i][0], setup[i][1]) && rangeSetup;
    check(rangeSetup && message(events[7], 0, 0xe3, 0, 64),
          "RPN zero sensitivity and RPN Null precede center on the note channel");
    check(message(events[8], 1, 0xe3, 0, 0) && message(events[9], 1, 0x93, 57, 89),
          "half-tick rounds upward and negative full bend reaches wire word zero before the attack");

    const auto release = std::find_if(events.begin(), events.end(),
        [](const smf::Event& event) { return message(event, 3, 0x83, 57, 45); });
    check(release != events.end() && events.end() - release >= 4 && release != events.begin() &&
          (release - 1)->tick == 3 && (release - 1)->status == 0xe3 && word(*(release - 1)) == 12287 &&
          message(*(release + 1), 3, 0xe3, 0, 64) && message(*(release + 2), 3, 0xe3, 127, 127) &&
          message(*(release + 3), 3, 0x93, 60, 89),
          "coincident movement, note-off, center, full positive bend and next attack retain wire order");
    check(message(events[events.size() - 3], 4, 0x83, 60, 45) &&
          message(events[events.size() - 2], 4, 0xe3, 0, 64) &&
          events.back().tick == 8 && events.back().status == 0xff && events.back().metaType == 0x2f,
          "last note releases and centers before a clip-local four-tick trailing rest");
    const auto again = smf::write(*file);
    check(std::holds_alternative<smf::Bytes>(again) && std::get<smf::Bytes>(again) == *encoded.data,
          "production reader and writer round trip byte-for-byte");

    const auto plain = encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 4, {480});
    check(static_cast<bool>(plain), "static bend exports at standard PPQ with no explicit tempo");
    if (!plain) return;
    const auto staticDecoded = smf::read(*plain.data);
    const auto* staticFile = std::get_if<smf::File>(&staticDecoded);
    check(staticFile && std::get<smf::Track>(staticFile->chunks.front()).events.size() == 16 &&
          message(std::get<smf::Track>(staticFile->chunks.front()).events[2], 0, 0xb3, 6, 4) &&
          message(std::get<smf::Track>(staticFile->chunks.front()).events[7], 60, 0xe3, 0, 48),
          "no tempo event and four-semitone sensitivity maps static -1 semitone to word 6144 at tick 60");
}

void rejectedInputs() {
    auto projected = fixture();
    check(!encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, {0}) &&
          !encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, {32768}),
          "PPQ reserves the standard SMF division high bit");
    check(!encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, {480, 0}) &&
          !encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, {480, 0x1000000}),
          "tempo requires a nonzero three-byte value");
    check(!encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 1, {480}),
          "the export refuses a clipped pitch bend");
    auto tooShort = projected;
    tooShort.notes.front().dur_ticks = 1;
    const auto collapsed = encodePitchMidiChannelBendSmf(tooShort, PitchMidiBendMode::Static, 2, {4});
    check(!collapsed && collapsed.detail && collapsed.detail->find("higher SMF PPQ") != std::string::npos,
          "a collapsed note refuses rather than writing a zero-length MIDI voice");
    projected.lengthTicks = std::int64_t{1} << 50;
    const auto tooLong = encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, {32767});
    check(!tooLong && tooLong.detail && tooLong.detail->find("variable-length quantity") != std::string::npos,
          "a delta beyond four SMF VLQ bytes reports the codec error without output");
    projected.notes.front().vel_on = 0;
    check(!encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, {480}),
          "zero velocity would encode a note-off and is refused");
    std::stop_source stop;
    stop.request_stop();
    const PitchMidiSmfOptions options{480, {}, stop.get_token()};
    check(encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, options).problem ==
          PitchMidiProblem::Cancelled, "pre-cancelled encoding returns before building bytes");
    PitchMidiProblem callbackProblem;
    {
        audio::CallbackScope scope;
        callbackProblem = encodePitchMidiChannelBendSmf(projected, PitchMidiBendMode::Static, 2, options).problem;
    }
    check(callbackProblem == PitchMidiProblem::Realtime, "SMF encoding refuses callback execution");
}

} // namespace

int main() {
    roundTrip();
    rejectedInputs();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
