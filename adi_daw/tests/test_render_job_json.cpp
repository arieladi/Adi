// SPDX-License-Identifier: GPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/render/job_json.hpp"
#include "adi/media/media_ops.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace {
using namespace adi;
using namespace adi::render;
using Json = nlohmann::json;
int checks = 0, failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
template<class Action>
void refuses(Action action, const char* reason) {
    try { action(); check(false, reason); }
    catch (const std::invalid_argument& error) {
        check(std::string(error.what()).find(reason) != std::string::npos, error.what());
    }
}
Job fixture() {
    Job job;
    job.project = media::pathFromUtf8("relative/\xD7\x90\xD7\x93\xD7\x99 \xF0\x9F\x8E\xB5.adi");
    job.directory = media::pathFromUtf8("../exports/Caf\xC3\xA9");
    job.nameTemplate = "{{mix}} {project}-{group}-{track}-{region}-{bpm}-{counter}";
    job.beginSample = 137; job.endSample = 240001;
    return job;
}
bool same(const Source& a, const Source& b) { return a.trackId == b.trackId && a.tap == b.tap; }
bool same(const Job& a, const Job& b) {
    if (a.project.native() != b.project.native() || a.directory.native() != b.directory.native() ||
        a.nameTemplate != b.nameTemplate || a.beginSample != b.beginSample || a.endSample != b.endSample ||
        a.regionId != b.regionId || !same(a.source, b.source) ||
        a.session.channels != b.session.channels || a.session.sampleRate != b.session.sampleRate ||
        a.session.maxFrames != b.session.maxFrames || a.session.precision != b.session.precision ||
        a.session.performanceState != b.session.performanceState || a.session.delayCompensation != b.session.delayCompensation ||
        a.session.exporting != b.session.exporting || a.session.offline != b.session.offline ||
        a.session.progressiveDecoder != b.session.progressiveDecoder || a.session.externalTempoBpm != b.session.externalTempoBpm ||
        a.encoding.format != b.encoding.format || a.encoding.dither != b.encoding.dither ||
        a.encoding.ditherSeed != b.encoding.ditherSeed || a.normalization.mode != b.normalization.mode ||
        a.normalization.targetLufs != b.normalization.targetLufs ||
        a.normalization.truePeakCeilingDb != b.normalization.truePeakCeilingDb ||
        a.normalization.channelWeights != b.normalization.channelWeights ||
        a.niStems.has_value() != b.niStems.has_value()) return false;
    if (a.niStems) {
        if (a.niStems->bitRate != b.niStems->bitRate || a.niStems->stems.size() != b.niStems->stems.size()) return false;
        for (std::size_t i = 0; i < a.niStems->stems.size(); ++i) {
            const auto& x = a.niStems->stems[i]; const auto& y = b.niStems->stems[i];
            if (!same(x.source, y.source) || x.name != y.name || x.color != y.color) return false;
        }
    }
    return true;
}
void roundTrips() {
    auto job = fixture();
    auto encoded = jobToJson(job);
    const auto schema = Json::parse(encoded);
    check(schema.at("format") == "adi-render-job" && schema.at("version") == 1 &&
          schema.at("regionId").is_null() && schema.at("niStems").is_null(), "explicit format/version and nullable options");
    check(schema.at("source").at("tap") == "postFader" && schema.at("encoding").at("format") == "pcm24" &&
          schema.at("session").at("precision") == "float32", "stable named enum values are not C++ ordinals");
    check(same(job, jobFromJson(encoded)), "default supported job round-trips every input");
    auto verbatim = job;
    verbatim.project = media::pathFromUtf8("\\\\?\\C:\\session\\unicode-\xD7\x90.adi");
    verbatim.directory = media::pathFromUtf8("\\\\server\\share\\output");
    check(same(verbatim, jobFromJson(jobToJson(verbatim))), "native path spelling and verbatim/UNC separators survive unchanged");
    job.source = {INT64_MAX, engine::OfflineTap::PreFader}; job.regionId = INT64_MAX;
    job.beginSample = INT64_MIN; job.endSample = INT64_MAX; // ignored when resolving a region
    job.session = {4, 96000, 257, engine::Precision::Float64};
    job.session.performanceState = false; job.session.delayCompensation = false;
    job.session.exporting = true; job.session.offline = true;
    job.encoding = {audio::WavFormat::Pcm16, audio::WavDither::Rectangular, UINT64_MAX};
    job.normalization = {NormalizeMode::Loudness, -19.75, -2.25, {1, 0, 1.41, 2}};
    check(same(job, jobFromJson(jobToJson(job))), "all nondefault scalar/vector options and exact signed/unsigned limits survive");
    const auto exact = Json::parse(jobToJson(job));
    check(exact.at("encoding").at("ditherSeed").get<std::uint64_t>() == UINT64_MAX &&
          exact.at("beginSample").get<std::int64_t>() == INT64_MIN, "large integers are emitted without a double conversion");
    Queue queue; const auto id = queue.add(jobFromJson(jobToJson(job)));
    check(id == 1 && queue.snapshot().size() == 1 && same(queue.snapshot()[0].job, job),
          "decoded job is admitted through the actual queue without rendering");
    for (const auto format : {audio::WavFormat::Pcm16, audio::WavFormat::Pcm24, audio::WavFormat::Float32})
        for (const auto dither : {audio::WavDither::None, audio::WavDither::Triangular, audio::WavDither::Rectangular})
            for (const auto mode : {NormalizeMode::Off, NormalizeMode::Peak, NormalizeMode::Loudness}) {
                job.encoding.format = format; job.encoding.dither = dither; job.normalization.mode = mode;
                check(same(job, jobFromJson(jobToJson(job))), "each supported encoding/dither/normalization value round-trips");
            }
    job = fixture(); job.niStems.emplace(); job.niStems->bitRate = 320000;
    for (int i = 1; i <= 4; ++i) {
        job.niStems->stems.push_back({{i, i % 2 ? engine::OfflineTap::PreFader : engine::OfflineTap::PostFader},
                                    "Stem \"" + std::to_string(i) + "\n\xD7\x90", i % 2 ? "#12aBef" : ""});
        check(same(job, jobFromJson(jobToJson(job))), "NI one through four sources retain order, tap, metadata and codec rate");
    }
    test::TempDirectory dir{"render_job_json", "pure"};
    job = fixture(); job.project = dir.path() / "missing.adi"; job.directory = dir.path() / "missing-output";
    check(same(job, jobFromJson(jobToJson(job))) && std::filesystem::is_empty(dir.path()),
          "saving/loading a job neither reads a project nor creates output or state files");
}
void invalidJson() {
    const auto good = jobToJson(fixture());
    const auto doc = Json::parse(good);
    const auto changed = [&](const char* pointer, const Json& value, const char* reason) {
        auto invalid = doc; invalid[Json::json_pointer(pointer)] = value;
        refuses([&] { (void)jobFromJson(invalid.dump()); }, reason);
    };
    for (const char* parent : {"", "/session", "/source", "/encoding", "/normalization"})
        changed((std::string(parent) + "/typo").c_str(), 1, "unknown field typo");
    for (const char* key : {"project", "session", "regionId", "niStems", "normalization"}) {
        auto missing = doc; missing.erase(key);
        refuses([&] { (void)jobFromJson(missing.dump()); }, "missing field");
    }
    changed("/format", "other", "unsupported format");
    changed("/version", UINT64_MAX, "unsupported version");
    changed("/version", 1.0, "exact JSON integer");
    changed("/session/channels", UINT64_MAX, "int64");
    changed("/session/maxFrames", INT64_MAX, "int32");
    changed("/session/precision", "float16", "unknown enum");
    changed("/session/offline", 1, "boolean");
    changed("/source/tap", "dry", "unknown enum");
    changed("/source/trackId", -1, "invalid direct stem source");
    changed("/source/trackId", static_cast<std::uint64_t>(INT64_MAX) + 1, "int64");
    changed("/encoding/format", 1, "string");
    changed("/encoding/format", "mp3", "unknown enum");
    changed("/encoding/dither", "shaped", "unknown enum");
    changed("/encoding/ditherSeed", -1, "nonnegative");
    changed("/encoding/ditherSeed", 9007199254740992.0, "exact JSON integer");
    changed("/normalization/mode", "auto", "unknown enum");
    changed("/normalization/targetLufs", "-14", "number");
    changed("/normalization/channelWeights", Json::array({true}), "number");
    changed("/project", std::string("path\0hidden", 11), "NUL");
    changed("/endSample", 0, "range");
    changed("/regionId", 0, "positive");
    changed("/session/sampleRate", 48000.5, "sample rate");
    changed("/nameTemplate", "{unknown}", "unknown export name token");
    refuses([&] { (void)jobFromJson("{\"pro\\u006aect\":\"duplicate\"," + good.substr(1)); }, "duplicate field project");
    auto nested = good; const auto at = nested.find("\"encoding\":{");
    check(at != std::string::npos, "nested duplicate fixture finds canonical object");
    nested.insert(at + std::string("\"encoding\":{").size(), "\"dither\":\"none\",");
    refuses([&] { (void)jobFromJson(nested); }, "duplicate field dither");
    refuses([&] { (void)jobFromJson(good + " {}"); }, "invalid JSON");
    refuses([&] { (void)jobFromJson(std::string(17, '[') + "0" + std::string(17, ']')); }, "nesting");
    refuses([&] { (void)jobFromJson(std::string(kMaximumJobJsonBytes + 1, ' ')); }, "1 MiB");
    refuses([&] { (void)jobFromJson("{\"x\":1e9999}"); }, "invalid JSON");
    refuses([&] { (void)jobFromJson("{\"x\":\"\xFF\"}"); }, "invalid JSON");
    auto ni = fixture(); ni.niStems = NiStems{{{{1, engine::OfflineTap::PostFader}, "one", ""}}, 256000};
    auto value = Json::parse(jobToJson(ni)); value["niStems"]["stems"][0]["source"]["typo"] = 1;
    refuses([&] { (void)jobFromJson(value.dump()); }, "unknown field typo");
    value = Json::parse(jobToJson(ni)); value["niStems"]["bitRate"] = UINT64_MAX;
    refuses([&] { (void)jobFromJson(value.dump()); }, "uint32");
    value = Json::parse(jobToJson(ni)); value["niStems"]["stems"] = Json::array();
    refuses([&] { (void)jobFromJson(value.dump()); }, "one to four");
}
void invalidJobs() {
    auto job = fixture();
    job.source.tap = static_cast<engine::OfflineTap>(99);
    refuses([&] { (void)jobToJson(job); }, "invalid direct stem source");
    job = fixture(); job.encoding.format = static_cast<audio::WavFormat>(99);
    refuses([&] { (void)jobToJson(job); }, "unsupported export WAV");
    job = fixture(); job.normalization.targetLufs = std::numeric_limits<double>::quiet_NaN();
    refuses([&] { (void)jobToJson(job); }, "finite");
    job = fixture(); job.normalization.channelWeights = {std::numeric_limits<double>::infinity()};
    refuses([&] { (void)jobToJson(job); }, "finite");
    job = fixture(); job.session.externalTempoBpm = 120;
    refuses([&] { (void)jobToJson(job); }, "live external tempo");
    char unused = 0;
    job = fixture(); job.session.progressiveDecoder = reinterpret_cast<audio::ProgressiveDecoder*>(&unused);
    refuses([&] { (void)jobToJson(job); }, "runtime decoder");
    job = fixture(); job.nameTemplate = "\xFF";
    refuses([&] { (void)jobToJson(job); }, "UTF-8");
    job = fixture(); job.directory = media::pathFromUtf8(std::string("out\0hidden", 10));
    refuses([&] { (void)jobToJson(job); }, "NUL");
    job = fixture(); job.normalization.channelWeights.assign(kMaximumJobJsonBytes / 2, 0.0);
    refuses([&] { (void)jobToJson(job); }, "1 MiB");
}
} // namespace
int main() {
    try { roundTrips(); invalidJson(); invalidJobs(); }
    catch (const std::exception& error) { check(false, error.what()); }
    std::printf("render Job JSON: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
