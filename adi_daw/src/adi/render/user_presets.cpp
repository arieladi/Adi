// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/user_presets.hpp"
#include "adi/render/job_json.hpp"
#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#include <share.h>
#endif

namespace adi::render {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
void require(bool valid, const char* reason) {
    if (!valid) throw std::invalid_argument(std::string("user render presets: ") + reason);
}
struct Guard final : nlohmann::json_sax<Json> {
    std::vector<std::set<std::string>> objects;
    unsigned depth = 0;
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(Json::number_integer_t) override { return true; }
    bool number_unsigned(Json::number_unsigned_t) override { return true; }
    bool number_float(Json::number_float_t x, const Json::string_t&) override { return std::isfinite(x); }
    bool string(Json::string_t&) override { return true; }
    bool binary(Json::binary_t&) override { return false; }
    bool start_object(std::size_t) override { if (++depth > 19) return false; objects.emplace_back(); return true; }
    bool key(Json::string_t& key) override { return objects.back().insert(key).second; }
    bool end_object() override { objects.pop_back(); --depth; return true; }
    bool start_array(std::size_t) override { return ++depth <= 19; }
    bool end_array() override { --depth; return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};
void name(const std::string& value) {
    require(value.size() <= kMaximumUserPresetNameBytes && value.find('\0') == std::string::npos,
            "preset name exceeds byte limit or contains NUL");
}
UserPresetList decode(std::string_view bytes) {
    require(bytes.size() <= kMaximumUserPresetsBytes, "file exceeds byte limit");
    require(bytes.find('\0') == std::string_view::npos, "literal NUL byte in file");
    Guard guard;
    require(Json::sax_parse(bytes.begin(), bytes.end(), &guard), "malformed, duplicate or over-deep JSON");
    const auto value = Json::parse(bytes.begin(), bytes.end());
    require(value.is_object() && value.size() == 4 && value.contains("format") && value.contains("version") &&
            value.contains("app") && value.contains("presets"), "unknown or missing envelope fields");
    require(value.at("format") == "adi-user-render-presets" && value.at("app") == "ADI", "unsupported format or application");
    require(value.at("version").is_number_integer() && value.at("version") == 1, "unsupported version");
    const auto& entries = value.at("presets");
    require(entries.is_array() && entries.size() <= kMaximumUserPresets, "invalid preset array or count limit");
    UserPresetList result{true, {}}; result.presets.reserve(entries.size());
    for (const auto& entry : entries) {
        require(entry.is_object() && entry.size() == 2 && entry.contains("name") && entry.contains("job") &&
                entry.at("name").is_string(), "unknown or invalid preset fields");
        auto label = entry.at("name").get<std::string>(); name(label);
        result.presets.push_back({std::move(label), jobFromJson(entry.at("job").dump())});
    }
    return result;
}
std::string encode(std::span<const UserPreset> presets) {
    require(presets.size() <= kMaximumUserPresets, "preset count limit");
    Json value{{"format", "adi-user-render-presets"}, {"version", 1}, {"app", "ADI"}, {"presets", Json::array()}};
    std::size_t size = value.dump().size();
    for (const auto& preset : presets) {
        name(preset.name);
        Json entry{{"name", preset.name}, {"job", Json::parse(jobToJson(preset.job))}};
        const auto extra = entry.dump().size() + (value.at("presets").empty() ? 0U : 1U);
        require(extra <= kMaximumUserPresetsBytes - size, "file exceeds byte limit");
        size += extra; value["presets"].push_back(std::move(entry));
    }
    auto bytes = value.dump();
    require(bytes.size() <= kMaximumUserPresetsBytes, "file exceeds byte limit");
    return bytes;
}
UserPresetList read(const fs::path& file) {
    require(!file.empty(), "no application data path");
    std::error_code error; const auto status = fs::symlink_status(file, error);
    if (error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found)) return {};
    if (error) throw fs::filesystem_error("read user presets", file, error);
    require(fs::is_regular_file(status), "destination is not a regular file");
#if defined(_WIN32)
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> input(_wfsopen(file.c_str(), L"rb", _SH_DENYNO), &std::fclose);
#else
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> input(std::fopen(file.c_str(), "rb"), &std::fclose);
#endif
    require(input != nullptr, "cannot open existing preset file");
    std::string bytes(kMaximumUserPresetsBytes + 1, '\0');
    const auto count = std::fread(bytes.data(), 1, bytes.size(), input.get());
    require(!std::ferror(input.get()), "cannot read complete preset file");
    require(count <= kMaximumUserPresetsBytes && std::feof(input.get()), "file exceeds byte limit or read is incomplete");
    bytes.resize(count);
    return decode(bytes);
}
std::error_code validateTarget(const fs::path& target) {
    try { (void)read(target); return {}; }
    catch (...) { return std::make_error_code(std::errc::invalid_argument); }
}
struct Stage {
    fs::path directory, file;
    explicit Stage(const fs::path& parent) {
        static std::atomic<unsigned long long> serial{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto candidate = parent / (".adi-render-presets-" + std::to_string(tick) + "-" + std::to_string(serial.fetch_add(1)));
            auto child = candidate / "payload.json"; // allocate before claiming the directory
            std::error_code error;
            if (fs::create_directory(candidate, error)) { directory = std::move(candidate); file = std::move(child); return; }
            if (error && error != std::errc::file_exists) throw fs::filesystem_error("reserve preset stage", candidate, error);
        }
        throw std::runtime_error("cannot reserve private preset stage");
    }
    ~Stage() noexcept {
        // Only our fixed child and exclusively created directory, never a
        // recursive removal or a public preset filename. Unexpected children stay.
        try { std::error_code ignored; fs::remove(file, ignored); fs::remove(directory, ignored); }
        catch (...) {} // private cleanup must never turn a published save into failure
    }
};
} // namespace

fs::path userPresetsFile() {
    const auto root = appdata::pathsFor(appdata::App::Daw).data;
    return root.empty() ? fs::path{} : root / "ADI" / "render-presets.json";
}
UserPresets::UserPresets(fs::path file) : file_(file.empty() ? fs::path{} : fs::absolute(file)) {}
std::optional<UserPresetList> UserPresets::load(std::string& error) const {
    if (audio::onAudioThread) return std::nullopt;
    error.clear();
    try { return read(file_); }
    catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}
media::PublishResult UserPresets::save(std::span<const UserPreset> presets, media::ReplaceHooks hooks) const {
    if (audio::onAudioThread) return {};
    try {
        require(!file_.empty(), "no application data path");
        const auto bytes = encode(presets);
        (void)read(file_); // refuse before even creating private staging
        fs::create_directories(file_.parent_path());
        Stage stage(file_.parent_path());
        {
            std::ofstream output(stage.file, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(output), "cannot create private preset payload");
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close(); require(static_cast<bool>(output), "cannot write and close private preset payload");
        }
        hooks.validateTarget = validateTarget; // callers cannot bypass the strict guard
        return media::replaceFile(stage.file, file_, hooks);
    } catch (const std::exception& exception) { return {media::PublishStatus::failed, false, exception.what()}; }
}
} // namespace adi::render
