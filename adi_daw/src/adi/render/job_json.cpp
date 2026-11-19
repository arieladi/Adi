// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/job_json.hpp"
#include "adi/media/media_ops.hpp"
#include <nlohmann/json.hpp>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace adi::render {
namespace {
using Json = nlohmann::json;
[[noreturn]] void fail(const std::string& at, const std::string& reason) {
    throw std::invalid_argument("render job " + at + ": " + reason);
}
void require(bool valid, const std::string& at, const std::string& reason) {
    if (!valid) fail(at, reason);
}
void fields(const Json& object, const std::string& at,
            std::initializer_list<std::string_view> expected) {
    require(object.is_object(), at, "expected an object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const auto key : expected) if (it.key() == key) { known = true; break; }
        require(known, at, "unknown field " + it.key().substr(0, 80));
    }
    for (const auto key : expected)
        require(object.contains(key), at, "missing field " + std::string(key));
}
std::string text(const Json& value, const std::string& at) {
    require(value.is_string(), at, "expected a string");
    return value.get<std::string>();
}
bool boolean(const Json& value, const std::string& at) {
    require(value.is_boolean(), at, "expected a boolean");
    return value.get<bool>();
}
std::int64_t signedInteger(const Json& value, const std::string& at) {
    require(value.is_number_integer(), at, "expected an exact JSON integer");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        require(number <= static_cast<std::uint64_t>(INT64_MAX), at, "integer exceeds int64");
        return static_cast<std::int64_t>(number);
    }
    return value.get<std::int64_t>();
}
std::uint64_t unsignedInteger(const Json& value, const std::string& at) {
    require(value.is_number_integer(), at, "expected an exact JSON integer");
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    const auto number = value.get<std::int64_t>();
    require(number >= 0, at, "expected a nonnegative integer");
    return static_cast<std::uint64_t>(number);
}
std::int32_t int32(const Json& value, const std::string& at) {
    const auto number = signedInteger(value, at);
    require(number >= INT32_MIN && number <= INT32_MAX, at, "integer exceeds int32");
    return static_cast<std::int32_t>(number);
}
double real(const Json& value, const std::string& at) {
    require(value.is_number(), at, "expected a number");
    const double number = value.get<double>();
    require(std::isfinite(number), at, "number must be finite");
    return number;
}
template<class Enum>
Enum enumeration(const Json& value, const std::string& at,
                 std::initializer_list<std::pair<std::string_view, Enum>> choices) {
    const auto name = text(value, at);
    for (const auto& [label, item] : choices) if (label == name) return item;
    fail(at, "unknown enum value");
}
std::string pathText(const std::filesystem::path& path, const std::string& at) {
    // Preserve the native spelling, including Windows verbatim/UNC prefixes.
    // generic_u8string would rewrite their separators during persistence.
    const auto utf8 = path.u8string();
    const std::string value(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    require(value.find('\0') == std::string::npos, at, "path contains NUL");
    return value;
}
std::filesystem::path path(const Json& value, const std::string& at) {
    const auto bytes = text(value, at);
    require(bytes.find('\0') == std::string::npos, at, "path contains NUL");
    return media::pathFromUtf8(bytes);
}
Source source(const Json& value, const std::string& at) {
    fields(value, at, {"trackId", "tap"});
    return {signedInteger(value.at("trackId"), at + ".trackId"),
            enumeration<engine::OfflineTap>(value.at("tap"), at + ".tap",
                {{"preFader", engine::OfflineTap::PreFader}, {"postFader", engine::OfflineTap::PostFader}})};
}
Json source(const Source& value) {
    return {{"trackId", value.trackId}, {"tap", value.tap == engine::OfflineTap::PreFader ? "preFader" : "postFader"}};
}
// Follow the scanner's streaming duplicate-key guard, then parse a DOM. This
// avoids the DOM callback parser's repeated parent-array scans.
struct Guard final : nlohmann::json_sax<Json> {
    std::vector<std::set<std::string>> objects;
    unsigned depth = 0;
    std::string error;
    bool refuse(std::string why) { error = std::move(why); return false; }
    bool enter() { return ++depth <= 16 || refuse("nesting exceeds 16 containers"); }
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(Json::number_integer_t) override { return true; }
    bool number_unsigned(Json::number_unsigned_t) override { return true; }
    bool number_float(Json::number_float_t value, const Json::string_t&) override {
        return std::isfinite(value) || refuse("nonfinite number");
    }
    bool string(Json::string_t&) override { return true; }
    bool binary(Json::binary_t&) override { return refuse("binary JSON is unsupported"); }
    bool start_object(std::size_t) override { if (!enter()) return false; objects.emplace_back(); return true; }
    bool key(Json::string_t& name) override {
        return objects.back().insert(name).second || refuse("duplicate field " + name.substr(0, 80));
    }
    bool end_object() override { objects.pop_back(); --depth; return true; }
    bool start_array(std::size_t) override { return enter(); }
    bool end_array() override { --depth; return true; }
    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception&) override {
        return refuse("invalid JSON at byte " + std::to_string(position));
    }
};
void serializable(const Job& job) {
    validateJob(job);
    require(!job.session.progressiveDecoder, "$.session", "runtime decoder service is not serializable");
    require(!job.session.externalTempoBpm, "$.session", "live external tempo is not a render option");
    require(std::isfinite(job.normalization.targetLufs) && std::isfinite(job.normalization.truePeakCeilingDb),
            "$.normalization", "stored numbers must be finite even when normalization is off");
    for (const auto weight : job.normalization.channelWeights)
        require(std::isfinite(weight), "$.normalization.channelWeights", "stored weights must be finite");
}
} // namespace

std::string jobToJson(const Job& job) {
    serializable(job);
    try {
        const auto& spec = job.session;
        const auto& norm = job.normalization;
        Json result{{"format", "adi-render-job"}, {"version", 1},
            {"project", pathText(job.project, "$.project")}, {"directory", pathText(job.directory, "$.directory")},
            {"nameTemplate", job.nameTemplate}, {"beginSample", job.beginSample}, {"endSample", job.endSample},
            {"session", {{"channels", spec.channels}, {"sampleRate", spec.sampleRate}, {"maxFrames", spec.maxFrames},
                {"precision", spec.precision == engine::Precision::Float32 ? "float32" : "float64"},
                {"performanceState", spec.performanceState}, {"delayCompensation", spec.delayCompensation},
                {"exporting", spec.exporting}, {"offline", spec.offline}}},
            {"source", source(job.source)}, {"regionId", job.regionId ? Json(*job.regionId) : Json(nullptr)},
            {"encoding", {{"format", job.encoding.format == audio::WavFormat::Pcm16 ? "pcm16" :
                job.encoding.format == audio::WavFormat::Pcm24 ? "pcm24" : "float32"},
                {"dither", job.encoding.dither == audio::WavDither::None ? "none" :
                job.encoding.dither == audio::WavDither::Triangular ? "triangular" : "rectangular"},
                {"ditherSeed", job.encoding.ditherSeed}}},
            {"normalization", {{"mode", norm.mode == NormalizeMode::Off ? "off" :
                norm.mode == NormalizeMode::Peak ? "peak" : "loudness"}, {"targetLufs", norm.targetLufs},
                {"truePeakCeilingDb", norm.truePeakCeilingDb}, {"channelWeights", norm.channelWeights}}},
            {"niStems", nullptr}};
        if (job.niStems) {
            Json stems = Json::array();
            for (const auto& stem : job.niStems->stems)
                stems.push_back({{"source", source(stem.source)}, {"name", stem.name}, {"color", stem.color}});
            result["niStems"] = {{"bitRate", job.niStems->bitRate}, {"stems", std::move(stems)}};
        }
        auto encoded = result.dump();
        require(encoded.size() <= kMaximumJobJsonBytes, "$", "JSON exceeds 1 MiB");
        return encoded;
    } catch (const Json::exception&) { fail("$", "cannot encode valid UTF-8 JSON"); }
    catch (const std::system_error&) { fail("$", "path cannot be represented as UTF-8"); }
}

Job jobFromJson(std::string_view bytes) {
    require(bytes.size() <= kMaximumJobJsonBytes, "$", "JSON exceeds 1 MiB");
    require(bytes.find('\0') == std::string_view::npos, "$", "literal NUL in JSON document");
    Guard guard;
    const bool parsed = Json::sax_parse(bytes, &guard);
    require(parsed, "$", guard.error.empty() ? "invalid JSON" : guard.error);
    try {
        const auto value = Json::parse(bytes);
        fields(value, "$", {"format", "version", "project", "directory", "nameTemplate", "beginSample",
            "endSample", "session", "source", "regionId", "encoding", "normalization", "niStems"});
        require(text(value.at("format"), "$.format") == "adi-render-job", "$.format", "unsupported format");
        require(unsignedInteger(value.at("version"), "$.version") == 1, "$.version", "unsupported version");
        Job job;
        job.project = path(value.at("project"), "$.project");
        job.directory = path(value.at("directory"), "$.directory");
        job.nameTemplate = text(value.at("nameTemplate"), "$.nameTemplate");
        job.beginSample = signedInteger(value.at("beginSample"), "$.beginSample");
        job.endSample = signedInteger(value.at("endSample"), "$.endSample");
        const auto& spec = value.at("session");
        fields(spec, "$.session", {"channels", "sampleRate", "maxFrames", "precision", "performanceState",
                                    "delayCompensation", "exporting", "offline"});
        job.session.channels = int32(spec.at("channels"), "$.session.channels");
        job.session.sampleRate = real(spec.at("sampleRate"), "$.session.sampleRate");
        job.session.maxFrames = int32(spec.at("maxFrames"), "$.session.maxFrames");
        job.session.precision = enumeration<engine::Precision>(spec.at("precision"), "$.session.precision",
            {{"float32", engine::Precision::Float32}, {"float64", engine::Precision::Float64}});
        job.session.performanceState = boolean(spec.at("performanceState"), "$.session.performanceState");
        job.session.delayCompensation = boolean(spec.at("delayCompensation"), "$.session.delayCompensation");
        job.session.exporting = boolean(spec.at("exporting"), "$.session.exporting");
        job.session.offline = boolean(spec.at("offline"), "$.session.offline");
        job.source = source(value.at("source"), "$.source");
        if (!value.at("regionId").is_null()) job.regionId = signedInteger(value.at("regionId"), "$.regionId");
        const auto& encoding = value.at("encoding");
        fields(encoding, "$.encoding", {"format", "dither", "ditherSeed"});
        job.encoding.format = enumeration<audio::WavFormat>(encoding.at("format"), "$.encoding.format",
            {{"pcm16", audio::WavFormat::Pcm16}, {"pcm24", audio::WavFormat::Pcm24}, {"float32", audio::WavFormat::Float32}});
        job.encoding.dither = enumeration<audio::WavDither>(encoding.at("dither"), "$.encoding.dither",
            {{"none", audio::WavDither::None}, {"triangular", audio::WavDither::Triangular}, {"rectangular", audio::WavDither::Rectangular}});
        job.encoding.ditherSeed = unsignedInteger(encoding.at("ditherSeed"), "$.encoding.ditherSeed");
        const auto& norm = value.at("normalization");
        fields(norm, "$.normalization", {"mode", "targetLufs", "truePeakCeilingDb", "channelWeights"});
        job.normalization.mode = enumeration<NormalizeMode>(norm.at("mode"), "$.normalization.mode",
            {{"off", NormalizeMode::Off}, {"peak", NormalizeMode::Peak}, {"loudness", NormalizeMode::Loudness}});
        job.normalization.targetLufs = real(norm.at("targetLufs"), "$.normalization.targetLufs");
        job.normalization.truePeakCeilingDb = real(norm.at("truePeakCeilingDb"), "$.normalization.truePeakCeilingDb");
        require(norm.at("channelWeights").is_array(), "$.normalization.channelWeights", "expected an array");
        for (const auto& weight : norm.at("channelWeights"))
            job.normalization.channelWeights.push_back(real(weight, "$.normalization.channelWeights"));
        if (!value.at("niStems").is_null()) {
            const auto& ni = value.at("niStems");
            fields(ni, "$.niStems", {"bitRate", "stems"});
            const auto rate = unsignedInteger(ni.at("bitRate"), "$.niStems.bitRate");
            require(rate <= UINT32_MAX, "$.niStems.bitRate", "integer exceeds uint32");
            job.niStems.emplace(); job.niStems->bitRate = static_cast<std::uint32_t>(rate);
            require(ni.at("stems").is_array() && ni.at("stems").size() <= 4, "$.niStems.stems", "expected at most four stems");
            for (const auto& stem : ni.at("stems")) {
                fields(stem, "$.niStems.stems[]", {"source", "name", "color"});
                job.niStems->stems.push_back({source(stem.at("source"), "$.niStems.stems[].source"),
                    text(stem.at("name"), "$.niStems.stems[].name"), text(stem.at("color"), "$.niStems.stems[].color")});
            }
        }
        serializable(job);
        return job;
    } catch (const Json::exception&) { fail("$", "invalid JSON value"); }
}
} // namespace adi::render
