// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/interchange/otio_time.hpp"
#include "adi/interchange/otio_timeline.hpp"

#include <cstdio>
#include <limits>

namespace {
using namespace adi::interchange::otio;
using adi::time_base::Rounding;
using Json = nlohmann::json;
int checks = 0, failures = 0;

void check(bool pass, const char *message) {
    ++checks;
    if (!pass) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}

Json wireTime(const Json &value, const Json &rate) {
    return Json{{"OTIO_SCHEMA", "RationalTime.1"}, {"value", value}, {"rate", rate}};
}

void parseTime() {
    const auto wire = Json::parse(R"({"OTIO_SCHEMA":"RationalTime.1","value":9007199254740993,"rate":24})");
    const auto huge = readTime(wire);
    check(huge && huge->valueNumerator == 9'007'199'254'740'993 && huge->valueDenominator == 1 &&
              writeTime(*huge) == wire, "JSON integer ticks survive a read/write beyond binary64 precision");
    const auto fractional = readTime(wireTime(-12.75, 12.5));
    check(fractional && fractional->valueNumerator == -51 && fractional->valueDenominator == 4 &&
              fractional->rate.numerator == 25 && fractional->rate.denominator == 2 &&
              toNanoseconds(*fractional, Rounding::Exact) == -1'020'000'000,
          "fractional frame and time-base decimals convert as reduced ratios");
    check(fractional && writeTime(*fractional) == wireTime(-12.75, 12.5),
          "representable fractional times keep their exact rational value");
    const auto exponent = readTime(wireTime(2.5e-10, 0.5));
    check(exponent && exponent->valueNumerator == 1 && exponent->valueDenominator == 4'000'000'000 &&
              toNanoseconds(*exponent, Rounding::Nearest) == 1 &&
              !toNanoseconds(*exponent, Rounding::Exact), "decimal exponents and half-nanosecond rounding");
    check(!readTime(Json::array()) && !readTime(Json{{"OTIO_SCHEMA", "RationalTime.2"}, {"rate", 24}, {"value", 0}}) &&
              !readTime(wireTime("9", 24)) && !readTime(wireTime(9, false)) &&
              !readTime(wireTime(9, 0)) && !readTime(wireTime(9, -24)) &&
              !readTime(wireTime(9, std::numeric_limits<double>::infinity())),
          "only supported schemas and finite positive numeric time bases are accepted");
    check(!readTime(wireTime(std::uint64_t{1} << 63, 24)) &&
              !readTime(wireTime(1e20, 24)) && !writeTime({1, 3, {24, 1}}),
          "out-of-domain input and lossy fractional serialization are refused");
}

void ntscAndLimits() {
    const auto alias = readTime(wireTime(30'000, 29.97));
    check(alias && alias->rate.numerator == 30'000 && alias->rate.denominator == 1001 &&
              toNanoseconds(*alias, Rounding::Exact) == 1'001'000'000'000,
          "common NTSC shorthand uses exact 30000/1001 over long durations");
    const auto positive = toNanoseconds({1, 1, {30'000, 1001}}, Rounding::Nearest);
    const auto negative = toNanoseconds({-1, 1, {30'000, 1001}}, Rounding::Floor);
    check(positive == 33'366'667 && negative == -33'366'667 &&
              !toNanoseconds({1, 1, {30'000, 1001}}, Rounding::Exact),
          "frame conversion honors explicit rounding for fractional nanoseconds");
    const auto written = writeTime({2, 2, {60'000, 2002}});
    const auto back = written ? readTime(*written) : std::nullopt;
    check(back && back->rate.numerator == 30'000 && back->rate.denominator == 1001 &&
              back->valueNumerator == 1 && back->valueDenominator == 1,
          "writer canonicalizes common factors without losing NTSC rate");
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    check(toNanoseconds({minimum, 1, {1'000'000'000, 1}}, Rounding::Exact) == minimum &&
              toNanoseconds({maximum, 1, {1'000'000'000, 1}}, Rounding::Exact) == maximum &&
              !toNanoseconds({maximum, 1, {1, 1}}, Rounding::Exact),
          "cross-cancellation preserves both signed endpoints and rejects output overflow");
    check(toNanoseconds({maximum, maximum, {4'000'000'000, 1}}, Rounding::Floor) == 0 &&
              !toNanoseconds({1, maximum, {4'000'000'000, 1}}, Rounding::Exact) &&
              !toNanoseconds({1, 1, {1, 0}}, Rounding::Exact),
          "reduce factors before multiplying and reject unsupported representational bounds");
}

void ranges() {
    const auto document = Json::parse(R"({
       "OTIO_SCHEMA":"TimeRange.1",
       "start_time":{"OTIO_SCHEMA":"RationalTime.1","value":-48,"rate":24},
       "duration":{"OTIO_SCHEMA":"RationalTime.1","value":1001,"rate":1000}
    })");
    const auto range = readRange(document);
    check(range && writeRange(*range) == document &&
              toNanoseconds(range->start, Rounding::Exact) == -2'000'000'000 &&
              toNanoseconds(range->duration, Rounding::Exact) == 1'001'000'000,
          "mixed-rate ranges retain their individual time bases and signed source start");
    auto malformed = document;
    malformed["duration"]["value"] = -1;
    check(!readRange(malformed) && !writeRange({{0, 1, {24, 1}}, {-1, 1, {24, 1}}}),
          "negative duration is rejected at both range boundaries");
    malformed = document;
    malformed.erase("start_time");
    check(!readRange(malformed) && writeRange({}).has_value(),
          "partial ranges are refused and zero length is representable");
}

Json wireRange(int start, int duration, const Json &rate) {
    return Json{{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", wireTime(start, rate)},
                {"duration", wireTime(duration, rate)}};
}

Json linearDocument() {
    const Json gap{{"OTIO_SCHEMA", "Gap.1"}, {"source_range", wireRange(0, 8, 24)}};
    const Json legacy{{"OTIO_SCHEMA", "Clip.1"}, {"name", "Recorder"},
                      {"source_range", wireRange(-15, 24, 24)}, {"enabled", false},
                      {"media_reference", Json{{"OTIO_SCHEMA", "MissingReference.1"}}}};
    const Json media{{"OTIO_SCHEMA", "ExternalReference.1"}, {"target_url", "file:///media/a%20b.wav"},
                     {"available_range", wireRange(120, 1001, 29.97)}};
    const Json modern{{"OTIO_SCHEMA", "Clip.2"}, {"name", "Source"}, {"source_range", nullptr},
                      {"media_references", Json{{"backup", Json{{"OTIO_SCHEMA", "MissingReference.1"}}},
                                                 {"capture/1~", media}}},
                      {"active_media_reference_key", "capture/1~"}};
    const Json audio{{"OTIO_SCHEMA", "Track.1"}, {"kind", "Audio"}, {"name", "A1"},
                     {"children", Json::array({gap, legacy, modern})}};
    const Json video{{"OTIO_SCHEMA", "Track.1"}, {"name", "V1"}, {"children", Json::array()}};
    return Json{{"OTIO_SCHEMA", "Timeline.1"}, {"name", "Live room"}, {"global_start_time", wireTime(86400, 24)},
                {"tracks", Json{{"OTIO_SCHEMA", "Stack.1"}, {"children", Json::array({audio, video})}}}};
}

void expectTimelineError(const Json &wire, TimelineIssue issue, const std::string &path,
                         const char *message) {
    const auto parsed = readLinearTimeline(wire);
    const auto *error = std::get_if<TimelineError>(&parsed);
    check(error && error->issue == issue && error->path == path, message);
}

void linearTimeline() {
    auto wire = linearDocument();
    const auto parsed = readLinearTimeline(wire);
    wire.clear();
    const auto *timeline = std::get_if<LinearTimeline>(&parsed);
    check(timeline && timeline->name == "Live room" && timeline->globalStart &&
              timeline->globalStart->valueNumerator == 86400 && timeline->tracks.size() == 2,
          "capture timeline name, time origin and ordered tracks independently of the source DOM");
    if (!timeline || timeline->tracks.size() != 2)
        return;
    const auto &audio = timeline->tracks[0];
    const auto &video = timeline->tracks[1];
    check(audio.kind == TrackKind::Audio && audio.name == "A1" && audio.items.size() == 3 &&
              video.kind == TrackKind::Video && video.items.empty(),
          "read serial tracks and default an omitted OTIO track kind to video");
    if (audio.items.size() != 3)
        return;
    const auto &gap = audio.items[0];
    const auto &legacy = audio.items[1];
    const auto &modern = audio.items[2];
    check(gap.kind == ItemKind::Gap && gap.source.duration.valueNumerator == 8 &&
              legacy.kind == ItemKind::Clip && legacy.media == MediaKind::Missing && !legacy.enabled &&
              legacy.source.start.valueNumerator == -15 && legacy.source.duration.valueNumerator == 24,
          "gaps, disabled legacy clips and signed source trims preserve their exact ranges");
    check(modern.media == MediaKind::External && modern.targetUrl == "file:///media/a%20b.wav" &&
              modern.source.start.valueNumerator == 120 && modern.source.duration.valueNumerator == 1001 &&
              modern.source.duration.rate.numerator == 30'000 && modern.source.duration.rate.denominator == 1001,
          "Clip.2 selects active media and uses its exact available range for an untrimmed source");
    auto inactive = linearDocument();
    inactive["tracks"]["enabled"] = false;
    const auto disabled = readLinearTimeline(inactive);
    const auto *disabledTimeline = std::get_if<LinearTimeline>(&disabled);
    check(disabledTimeline && disabledTimeline->tracks.size() == 2 &&
              !disabledTimeline->tracks[0].enabled && !disabledTimeline->tracks[1].enabled,
          "the stack's enabled state applies to every track");
}

void unsupportedTimelines() {
    auto wire = linearDocument();
    wire["tracks"]["children"][0]["children"][1]["OTIO_SCHEMA"] = "Transition.1";
    expectTimelineError(wire, TimelineIssue::UnsupportedSchema, "/tracks/children/0/children/1/OTIO_SCHEMA",
                        "timing-bearing unsupported items get their exact location");
    wire = linearDocument();
    wire["tracks"]["children"][1]["source_range"] = wireRange(0, 1, 24);
    expectTimelineError(wire, TimelineIssue::UnsupportedTrim, "/tracks/children/1/source_range",
                        "composition trimming cannot silently alter track placement");
    wire = linearDocument();
    wire["tracks"]["children"][0]["children"][1]["effects"] = Json::array({Json{{"OTIO_SCHEMA", "FreezeFrame.1"}}});
    expectTimelineError(wire, TimelineIssue::UnsupportedEffects, "/tracks/children/0/children/1/effects",
                        "unmodelled effects refuse a misleading time projection");
    wire = linearDocument();
    wire["tracks"]["children"][0]["children"][2]["media_references"].erase("capture/1~");
    expectTimelineError(wire, TimelineIssue::MissingMediaReference,
                        "/tracks/children/0/children/2/media_references/capture~11~0",
                        "missing active reference reports an escaped JSON Pointer");
    wire = linearDocument();
    wire["tracks"]["children"][0]["children"][2]["media_references"]["capture/1~"].erase("available_range");
    expectTimelineError(wire, TimelineIssue::MissingSourceRange,
                        "/tracks/children/0/children/2/media_references/capture~11~0/available_range",
                        "a clip without either usable duration source is refused");
    wire = linearDocument();
    wire["tracks"]["children"][0]["children"][0]["source_range"]["duration"]["value"] = -1;
    expectTimelineError(wire, TimelineIssue::InvalidTime, "/tracks/children/0/children/0/source_range",
                        "malformed exact time carries the clip or gap path");
}

void writeTimeline() {
    const auto original = readLinearTimeline(linearDocument());
    const auto *timeline = std::get_if<LinearTimeline>(&original);
    check(timeline != nullptr, "prepare a native linear model for serialization");
    if (!timeline)
        return;
    const auto written = writeLinearTimeline(*timeline);
    const auto *wire = std::get_if<Json>(&written);
    check(wire != nullptr, "serialize a native linear model as a canonical OTIO timeline");
    if (!wire)
        return;
    const auto &tracks = wire->at("tracks").at("children");
    const auto &items = tracks.at(0).at("children");
    check(wire->at("OTIO_SCHEMA") == "Timeline.1" && tracks.at(0).at("kind") == "Audio" &&
              items.at(0).at("OTIO_SCHEMA") == "Gap.1" && items.at(1).at("OTIO_SCHEMA") == "Clip.2" &&
              items.at(1).at("media_references").at("DEFAULT_MEDIA").at("OTIO_SCHEMA") == "MissingReference.1" &&
              items.at(2).at("active_media_reference_key") == "DEFAULT_MEDIA",
          "canonical wire uses a modern clip with explicit active external or missing media references");
    const auto decoded = readLinearTimeline(Json::parse(wire->dump()));
    const auto *roundTrip = std::get_if<LinearTimeline>(&decoded);
    check(roundTrip && roundTrip->tracks.size() == 2 && roundTrip->tracks[0].items.size() == 3 &&
              !roundTrip->tracks[0].items[1].enabled &&
              roundTrip->tracks[0].items[2].targetUrl == "file:///media/a%20b.wav" &&
              roundTrip->tracks[0].items[2].source.duration.rate.denominator == 1001 &&
              roundTrip->tracks[0].items[2].source.duration.valueNumerator == 1001 &&
              roundTrip->globalStart && roundTrip->globalStart->valueNumerator == 86400,
          "serialized linear structure round-trips exact times, ordering, track state and URL");

    auto altered = *timeline;
    altered.tracks[0].items[2].media = MediaKind::Unsupported;
    auto refused = writeLinearTimeline(altered);
    auto *error = std::get_if<TimelineError>(&refused);
    check(error && error->issue == TimelineIssue::UnsupportedMedia &&
              error->path == "/tracks/children/0/children/2/media_references",
          "the writer reports unlowered media rather than serializing an empty placeholder");
    altered = *timeline;
    altered.tracks[0].items[0].source.duration = {1, 3, {24, 1}};
    refused = writeLinearTimeline(altered);
    error = std::get_if<TimelineError>(&refused);
    check(error && error->issue == TimelineIssue::InvalidTime &&
              error->path == "/tracks/children/0/children/0/source_range",
          "an unrepresentable rational refuses the whole document with its source path");
}
} // namespace

int main() {
    parseTime();
    ntscAndLimits();
    ranges();
    linearTimeline();
    unsupportedTimelines();
    writeTimeline();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
