// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/smf.hpp"

#include <cstdio>
#include <initializer_list>

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
} // namespace

int main() {
    losslessWire();
    largeTicksAndEdits();
    malformed();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
