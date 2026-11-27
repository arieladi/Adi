// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/midi_clip_file.hpp"
#include "adi/controller_lanes.hpp"
#include "adi/engine/clip_details.hpp"
#include "adi/history.hpp"
#include "adi/clip_image.hpp"
#include "adi/store.hpp"
#include "adi/textproj.hpp"
#include "temp_directory.hpp"

#include <cstdio>
#include <initializer_list>
#include <array>
#include <fstream>

namespace {
using namespace adi::interchange::smf;
int checks = 0, failures = 0;

void check(bool pass, const char *message) {
    ++checks;
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

Bytes singleTrack(std::initializer_list<std::uint8_t> body) {
    Bytes source{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 1, 0xe0,
                 'M', 'T', 'r', 'k', 0, 0, 0, static_cast<std::uint8_t>(body.size())};
    source.insert(source.end(), body.begin(), body.end());
    return source;
}

void expectIssue(const Bytes &source, Issue expected, std::size_t offset) {
    const auto result = read(source);
    const auto *error = std::get_if<Error>(&result);
    const auto name = issueName(expected);
    check(error && error->issue == expected && error->offset == offset, name.data());
}

void losslessWire() {
    const Bytes source{
        'M', 'T', 'h', 'd', 0, 0, 0, 8, 0, 1, 0, 2, 0xe3, 80, 0xca, 0xfe,
        'x', 'R', 'a', 'w', 0, 0, 0, 3, 0, 0xff, 0x90,
        'M', 'T', 'r', 'k', 0, 0, 0, 34,
        0x80, 0, 0x90, 60, 70,
        0x81, 0, 62, 0,
        0, 0xff, 0x7e, 0x80, 2, 0x81, 0x99,
        0, 0xf0, 3, 0x7d, 0x20, 0xf7,
        0, 0xf7, 2, 0xf8, 0xfa,
        0, 0xc3, 0x19,
        0, 0xff, 0x2f, 0,
        'x', 'e', 'm', 'p', 0, 0, 0, 0,
        'M', 'T', 'r', 'k', 0, 0, 0, 4, 0, 0xff, 0x2f, 0};
    const auto parsed = read(source);
    const auto *file = std::get_if<File>(&parsed);
    check(file != nullptr, "read a multitrack file with opaque header and interspersed chunks");
    if (!file)
        return;
    const auto *smpte = std::get_if<Smpte>(&file->division);
    check(file->format == 1 && smpte && smpte->rateCode == -29 && smpte->ticksPerFrame == 80 &&
              file->headerExtra == Bytes{0xca, 0xfe}, "retain exact SMPTE rate code and extra header bytes");
    check(file->chunks.size() == 4 &&
              std::get<UnknownChunk>(file->chunks[0]).data == Bytes{0, 0xff, 0x90} &&
              std::get<UnknownChunk>(file->chunks[2]).data.empty(),
          "preserve unknown chunks in their original positions including empty chunks");
    const auto &events = std::get<Track>(file->chunks[1]).events;
    check(events.size() == 7 && events[1].tick == 128 && events[1].runningStatus &&
              events[1].status == 0x90 && events[1].data == Bytes{62, 0},
          "running status keeps decoded status and the original wire decision");
    check(events[2].metaType == 0x7e && events[2].data == Bytes{0x81, 0x99} &&
              events[3].data == Bytes{0x7d, 0x20, 0xf7} && events[4].data == Bytes{0xf8, 0xfa} &&
              events[5].status == 0xc3 && events[5].data == Bytes{0x19},
          "opaque meta, sysex, escaped messages and one-byte channel events retain payloads");
    const auto written = write(*file);
    const auto *bytes = std::get_if<Bytes>(&written);
    check(bytes && *bytes == source, "round trip reproduces file bytes including wider original VLQs");
}

void largeTicksAndEdits() {
    File file;
    file.division = Ppq{9600};
    Track track;
    constexpr std::uint64_t delta = 0x0fffffff;
    for (std::uint64_t i = 0; i < 32; ++i)
        track.events.push_back(Event{i * delta, 0x81, 0, {64, 0}, i != 0});
    track.events.push_back(Event{32 * delta, 0xff, 0x2f, {}, false, 1});
    file.chunks.emplace_back(track);
    const auto written = write(file);
    const auto *bytes = std::get_if<Bytes>(&written);
    check(bytes != nullptr, "writing grows a preserved delta width when an edited value requires it");
    if (!bytes)
        return;
    const auto parsed = read(*bytes);
    const auto *result = std::get_if<File>(&parsed);
    check(result && std::get<Ppq>(result->division).ticksPerQuarter == 9600 &&
              std::get<Track>(result->chunks.front()).events.back().tick == 32 * delta &&
              std::get<Track>(result->chunks.front()).events.back().deltaWidth == 4,
          "preserve source PPQ and absolute ticks beyond 32 bits");
    std::get<Track>(file.chunks.front()).events.back().tick = 0;
    const auto invalid = write(file);
    const auto *error = std::get_if<Error>(&invalid);
    check(error && error->issue == Issue::InvalidTickOrder, "reject writer events with decreasing ticks");
}

void malformed() {
    expectIssue({}, Issue::MissingHeader, 0);
    expectIssue(singleTrack({0x81}), Issue::VlqTruncated, 23);
    expectIssue(singleTrack({0x80, 0x80, 0x80, 0x80, 0}), Issue::VlqTooLong, 22);
    expectIssue(singleTrack({0, 0x40, 0}), Issue::MissingStatus, 23);
    expectIssue(singleTrack({0, 0xf1, 0}), Issue::InvalidStatus, 23);
    expectIssue(singleTrack({0, 0x90, 1}), Issue::EventDataTruncated, 24);
    expectIssue(singleTrack({0, 0x90, 1, 0xff}), Issue::InvalidChannelData, 24);
    expectIssue(singleTrack({0, 0xff, 0x80}), Issue::InvalidMetaType, 24);
    expectIssue(singleTrack({0, 0xf7, 4, 1}), Issue::EventDataTruncated, 25);
    expectIssue(singleTrack({0, 0xff, 0x2f, 1, 0}), Issue::InvalidEndOfTrack, 26);
    expectIssue(singleTrack({0, 0xff, 0x2f, 0, 0}), Issue::DataAfterEndOfTrack, 26);
    expectIssue(singleTrack({0, 0xd0, 64}), Issue::MissingEndOfTrack, 25);
    expectIssue(singleTrack({0, 0xd0, 64, 0, 0xff, 0x01, 0, 0, 64}), Issue::MissingStatus, 30);
    auto bad = singleTrack({0, 0xff, 0x2f, 0});
    bad[11] = 2;
    expectIssue(bad, Issue::TrackCountMismatch, 10);
    bad[11] = 1;
    bad[12] = 0xe4;
    expectIssue(bad, Issue::InvalidDivision, 12);
    bad[12] = 1;
    bad[21] = 5;
    expectIssue(bad, Issue::ChunkDataTruncated, 22);
    bad[21] = 4;
    bad.push_back('x');
    expectIssue(bad, Issue::ChunkHeaderTruncated, 26);

    const auto wire = singleTrack({0, 0x90, 60, 70, 0, 0xff, 0x2f, 0});
    bool allRejected = true;
    for (std::size_t size = 0; size < wire.size(); ++size)
        allRejected = allRejected && std::holds_alternative<Error>(read(std::span(wire).first(size)));
    check(allRejected, "every proper prefix of a valid file is bounded and rejected");
}
void clipInterchange() {
    adi::test::TempDirectory folder("smf", "clip");
    File file;
    file.format = 1;
    file.division = Ppq{480};
    file.chunks.emplace_back(Track{{{0, 0xff, 0x51, {0x06, 0x1a, 0x80}}, {0, 0xff, 0x2f, {}}}});
    file.chunks.emplace_back(Track{{{120, 0x92, 0, {60, 111}}, {120, 0xe2, 0, {0, 0}},
        {240, 0xb2, 0, {1, 32}}, {240, 0xe2, 0, {37, 73}}, {360, 0x82, 0, {60, 7}},
        {360, 0xe2, 0, {0, 64}}, {420, 0xe2, 0, {127, 127}}, {480, 0xff, 0x2f, {}}}});
    file.chunks.emplace_back(Track{{{0, 0x94, 0, {67, 92}}, {480, 0x94, 0, {67, 0}}, {480, 0xff, 0x2f, {}}}});
    const auto wire = write(file);
    const auto* bytes = std::get_if<Bytes>(&wire);
    check(bytes != nullptr, "write an SMF1 import fixture");
    if (bytes == nullptr)
        return;
    const auto path = folder.path() / "split.mid";
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        check(stream.good(), "write the MIDI file");
    }
    const auto imported = adi::interchange::readMidiClipFile(path);
    check(imported.error.empty() && imported.tracks.size() == 2, "SMF1 imports musical tracks separately and ignores its conductor track");
    if (imported.tracks.size() != 2)
        return;

    adi::StoreError error{};
    auto store = adi::Store::createNewProject(folder.path() / "midi.adi", error);
    check(store != nullptr, "create the import destination");
    if (!store)
        return;
    adi::clip_edit::CopyIds ids(store->db());
    const auto first = ids.next("clips"), second = ids.next("clips");
    constexpr auto beat = adi::textproj::kPPQ;
    const std::array requests{
        adi::interchange::createImportedMidiClip(ids, imported.tracks[0], 2, first, beat, "Piano"),
        adi::interchange::createImportedMidiClip(ids, imported.tracks[1], 3, second, beat, "Bass"),
    };
    check(adi::OpJournal(*store).commit(requests).ok, "import both tracks in one project transaction");
    const auto firstDetails = adi::engine::readClipDetails(*store, first);
    const auto secondDetails = adi::engine::readClipDetails(*store, second);
    check(firstDetails.clip && secondDetails.clip && firstDetails.clip->posTicks == beat &&
              firstDetails.clip->lengthTicks == beat && firstDetails.clip->notes.size() == 1 &&
              secondDetails.clip->notes.size() == 1 && secondDetails.clip->notes[0].key == 67,
          "the two clips keep their time, track and pitches");
    if (!firstDetails.clip || firstDetails.clip->notes.empty())
        return;
    const auto &note = firstDetails.clip->notes[0];
    check(note.startTicks == beat / 4 && note.durationTicks == beat / 2 && note.channel == 2 &&
              note.velocity == 111 && note.releaseVelocity == 7, "import preserves note timing, channel and velocities");
    const auto lanes = adi::controllers::readLanes(*store, first);
    check(lanes.ok && lanes.lanes.size() == 2 && lanes.lanes[0].points.size() == 1 &&
              lanes.lanes[0].controller == 1 && lanes.lanes[0].points[0].timeTicks == beat / 2 &&
              lanes.lanes[0].points[0].value == 32. / 127.,
          "the mod wheel lands in an editable clip controller lane");
    check(lanes.ok && lanes.lanes.size() == 2 && lanes.lanes[1].points.size() == 4 &&
              lanes.lanes[1].channel == 2 && lanes.lanes[1].controller == adi::controllers::kPitchBend &&
              lanes.lanes[1].points[0].value == 0. && lanes.lanes[1].points[1].timeTicks == beat / 2 &&
              lanes.lanes[1].points[2].value == .5 && lanes.lanes[1].points[3].value == 1.,
          "SMF channel pitch wheel imports into a normalized editable lane");

    std::string exportError;
    const auto exported = adi::interchange::encodeMidiClipFile(*store, first, exportError);
    const auto decoded = read(exported);
    const auto* output = std::get_if<File>(&decoded);
    check(exportError.empty() && output && output->format == 0 && std::get<Ppq>(output->division).ticksPerQuarter == 96 &&
              output->chunks.size() == 1, "export writes one Type 0 track at the Live export resolution");
    if (output) {
        const auto &events = std::get<Track>(output->chunks[0]).events;
        const auto message = [&](std::size_t index, std::uint64_t tick, std::uint8_t status, const Bytes &data) {
            return events[index].tick == tick && events[index].status == status && events[index].data == data;
        };
        check(events.size() == 8 && message(0, 24, 0xe2, {0, 0}) && message(1, 24, 0x92, {60, 111}) &&
                  message(2, 48, 0xb2, {1, 32}) && message(3, 48, 0xe2, {37, 73}) &&
                  message(4, 72, 0xe2, {0, 64}) && message(5, 72, 0x82, {60, 7}) &&
                  message(6, 84, 0xe2, {127, 127}) && events[7].tick == 96 && events[7].metaType == 0x2f,
              "export preserves all 14 pitch bits, controller bytes, and controller-first ordering");
    }
    check(adi::History(*store).undo().ok && !adi::engine::readClipDetails(*store, first).clip &&
              !adi::engine::readClipDetails(*store, second).clip, "one Undo removes the entire imported file");
}
} // namespace

int main() {
    losslessWire();
    largeTicksAndEdits();
    clipInterchange();
    malformed();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
