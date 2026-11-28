// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_presets.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/blake3.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/render/queue.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace adi::preset {
namespace fs = std::filesystem;
namespace {
using Json = nlohmann::json;
constexpr std::string_view kDocument = "adi.device-preset";
constexpr std::string_view kExtension = ".adipreset";
constexpr std::string_view kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
// Large enough for every slot of a 32-slot built-in Sampler, including base64
// expansion. Limit file input before handing it to the JSON parser.
constexpr std::uintmax_t kMaxFileBytes = 2ULL * 1024 * 1024 * 1024;

void requireOwner() {
    if (audio::onAudioThread)
        throw std::runtime_error("Device preset file access belongs off the audio thread");
}
bool validText(std::string_view text) {
    return !text.empty() && text.find('\0') == std::string_view::npos;
}
bool hashText(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
Json shellId(const Identity &identity) {
    return identity.shellId ? Json(*identity.shellId) : Json(nullptr);
}
std::string stateDigest(std::span<const std::byte> bytes) {
    const auto hash = media::blake3Bytes(bytes);
    if (!hash)
        throw std::runtime_error("Could not hash the device preset state");
    return hash.hex;
}
void checkName(const Preset &preset) {
    if (!validText(preset.name))
        throw std::runtime_error("A device preset needs a name");
    if (!validText(preset.deviceName))
        throw std::runtime_error("A device preset needs a device name");
}
void checkParameter(std::string_view name, double normalized) {
    if (!validText(name) || !std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0)
        throw std::runtime_error("A device preset holds an invalid normalized parameter");
}
void checkState(std::string_view role, std::span<const std::byte> bytes) {
    if (!validText(role) || bytes.empty() ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("A device preset holds an invalid state role or byte length");
}
void checkPanel(const std::vector<std::string> &ids) {
    std::set<std::string_view> seen;
    for (const auto &id : ids) {
        if (!validText(id))
            throw std::runtime_error("A device preset holds an invalid panel control ID");
        if (!seen.emplace(id).second)
            throw std::runtime_error("Duplicate device preset panel control ID");
    }
}
std::string encode(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const bool second = i + 1 < bytes.size();
        const bool third = i + 2 < bytes.size();
        const auto bits = (std::to_integer<unsigned>(bytes[i]) << 16u) |
                          (second ? std::to_integer<unsigned>(bytes[i + 1]) << 8u : 0u) |
                          (third ? std::to_integer<unsigned>(bytes[i + 2]) : 0u);
        text += kBase64[(bits >> 18u) & 63u];
        text += kBase64[(bits >> 12u) & 63u];
        text += second ? kBase64[(bits >> 6u) & 63u] : '=';
        text += third ? kBase64[bits & 63u] : '=';
    }
    return text;
}
unsigned decodeDigit(char c) {
    const auto digit = kBase64.find(c);
    if (digit == std::string_view::npos)
        throw std::runtime_error("Invalid base64 digit in device preset state");
    return static_cast<unsigned>(digit);
}
std::vector<std::byte> decode(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0)
        throw std::runtime_error("Invalid base64 length in device preset state");
    std::vector<std::byte> bytes;
    bytes.reserve((text.size() / 4) * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const auto a = decodeDigit(text[i]);
        const auto b = decodeDigit(text[i + 1]);
        const bool padThird = text[i + 2] == '=';
        const bool padFourth = text[i + 3] == '=';
        const auto c = padThird ? 0u : decodeDigit(text[i + 2]);
        const auto d = padFourth ? 0u : decodeDigit(text[i + 3]);
        if ((padThird && (!padFourth || (b & 15u) != 0u)) ||
            (padFourth && (i + 4 != text.size() || (c & 3u) != 0u)))
            throw std::runtime_error("Invalid base64 padding in device preset state");
        const auto bits = (a << 18u) | (b << 12u) | (c << 6u) | d;
        bytes.push_back(static_cast<std::byte>((bits >> 16u) & 255u));
        if (!padThird)
            bytes.push_back(static_cast<std::byte>((bits >> 8u) & 255u));
        if (!padFourth)
            bytes.push_back(static_cast<std::byte>(bits & 255u));
    }
    return bytes;
}
Json encodePreset(const Preset &preset, const std::string &key) {
    auto parameters = Json::array();
    for (const auto &[id, normalized] : preset.parameters) {
        checkParameter(id, normalized);
        parameters.push_back({{"id", id}, {"norm", normalized}});
    }
    auto state = Json::array();
    for (const auto &[role, bytes] : preset.state) {
        checkState(role, bytes);
        state.push_back({{"role", role}, {"blake3", stateDigest(bytes)}, {"base64", encode(bytes)}});
    }
    Json document{{"format", std::string(kDocument)}, {"version", 1}, {"name", preset.name}, {"deviceKey", key},
                  {"device", {{"format", preset.device.format}, {"uid", preset.device.uid},
                              {"shellId", shellId(preset.device)}, {"name", preset.deviceName}}},
                  {"parameters", std::move(parameters)}, {"state", std::move(state)}};
    if (preset.panel) {
        checkPanel(*preset.panel);
        document["panel"] = *preset.panel;
    }
    return document;
}
Preset decodePreset(const Json &json, const fs::path &file) {
    const auto &format = json.at("format");
    if (!format.is_string() || format.get_ref<const std::string &>() != kDocument ||
        !json.at("version").is_number_integer() || json.at("version") != 1)
        throw std::runtime_error("Unsupported ADI device preset version");
    const auto &device = json.at("device");
    Preset preset;
    preset.name = json.at("name").get<std::string>();
    preset.deviceName = device.at("name").get<std::string>();
    preset.device.format = device.at("format").get<std::string>();
    preset.device.uid = device.at("uid").get<std::string>();
    const auto &shell = device.at("shellId");
    if (!shell.is_null()) {
        if (!shell.is_number_integer() ||
            (shell.is_number_unsigned() &&
             shell.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
            throw std::runtime_error("Invalid device preset shell identity");
        preset.device.shellId = shell.get<std::int64_t>();
    }
    const auto key = deviceKey(preset.device);
    // A standalone preset can be read from any folder. Under the managed
    // Presets tree it must agree with the key by which the Browser listed it.
    const auto folder = file.parent_path();
    const auto folderKey = media::pathUtf8(folder.filename());
    if (json.at("deviceKey").get<std::string>() != key ||
        (folder.parent_path().filename() == "Presets" && hashText(folderKey) && folderKey != key))
        throw std::runtime_error("The device preset identity does not match its key");
    checkName(preset);
    const auto &parameters = json.at("parameters");
    const auto &state = json.at("state");
    if (!parameters.is_array() || !state.is_array())
        throw std::runtime_error("Invalid parameter or state list in device preset");
    if (const auto panel = json.find("panel"); panel != json.end() && !panel->is_null()) {
        if (!panel->is_array())
            throw std::runtime_error("Invalid panel control list in device preset");
        preset.panel = panel->get<std::vector<std::string>>();
        checkPanel(*preset.panel);
    }
    for (const auto &parameter : parameters) {
        const auto id = parameter.at("id").get<std::string>();
        const auto &value = parameter.at("norm");
        if (!value.is_number())
            throw std::runtime_error("A device preset holds a nonnumeric parameter");
        const auto normalized = value.get<double>();
        checkParameter(id, normalized);
        if (!preset.parameters.emplace(id, normalized).second)
            throw std::runtime_error("Duplicate device preset parameter");
    }
    for (const auto &role : state) {
        const auto name = role.at("role").get<std::string>();
        const auto &text = role.at("base64").get_ref<const std::string &>();
        auto bytes = decode(text);
        checkState(name, bytes);
        if (role.at("blake3").get<std::string>() != stateDigest(bytes))
            throw std::runtime_error("Device preset state hash does not match its bytes");
        if (!preset.state.emplace(name, std::move(bytes)).second)
            throw std::runtime_error("Duplicate device preset state role");
    }
    return preset;
}
void ensureDirectory(const fs::path &path) {
    fs::create_directory(path);
    if (!fs::is_directory(fs::symlink_status(path)))
        throw std::runtime_error("The preset folder is not an ordinary directory");
}
bool hasExtension(const fs::path &path) {
    auto ext = media::pathUtf8(path.extension());
    for (char &c : ext)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return ext == kExtension;
}
class PresetStage {
  public:
    explicit PresetStage(const fs::path &parent) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt) {
            auto candidate = parent / (".adi-preset-" + std::to_string(tick) + "-" +
                                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            if (fs::create_directory(candidate)) {
                folder_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Could not reserve a private device preset file");
    }
    ~PresetStage() {
        std::error_code ignored;
        fs::remove_all(folder_, ignored);
    }
    PresetStage(const PresetStage &) = delete;
    PresetStage &operator=(const PresetStage &) = delete;
    fs::path file() const { return folder_ / "preset.tmp"; }

  private:
    fs::path folder_;
};
void writeDocument(const fs::path &file, const Json &document) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << std::setw(2) << document << '\n';
    stream.close();
    if (fs::file_size(file) > kMaxFileBytes)
        throw std::runtime_error("The device preset file is too large to save");
}
fs::path defaultPath(const fs::path &library, const std::string &key) {
    return library / "Defaults" / "Devices" / (key + std::string(kExtension));
}
} // namespace

std::string deviceKey(const Identity &identity) {
    if ((identity.format != "internal" && identity.format != "clap" && identity.format != "vst3") ||
        !validText(identity.uid))
        throw std::runtime_error("Unsupported or missing device preset identity");
    const auto text = Json::array({identity.format, identity.uid, shellId(identity)}).dump();
    return stateDigest(std::as_bytes(std::span(text.data(), text.size())));
}
std::vector<Entry> list(const fs::path &library) {
    requireOwner();
    std::vector<Entry> entries;
    if (library.empty())
        return entries;
    const auto presets = library / "Presets";
    if (!fs::is_directory(fs::symlink_status(presets)))
        return entries;
    for (const auto &directory : fs::directory_iterator(presets)) {
        const auto key = media::pathUtf8(directory.path().filename());
        if (!hashText(key) || !fs::is_directory(directory.symlink_status()))
            continue;
        for (const auto &file : fs::directory_iterator(directory.path())) {
            if (hasExtension(file.path()) && fs::is_regular_file(file.symlink_status()))
                entries.push_back({key, media::pathUtf8(file.path().stem()), file.path()});
        }
    }
    std::ranges::sort(entries, [](const auto &left, const auto &right) {
        return std::tie(left.deviceKey, left.name, left.path) < std::tie(right.deviceKey, right.name, right.path);
    });
    return entries;
}
Preset read(const fs::path &file) {
    requireOwner();
    if (!hasExtension(file) || !fs::is_regular_file(fs::symlink_status(file)))
        throw std::runtime_error("Choose an ADI device preset file");
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("Could not open the device preset");
    const auto length = static_cast<std::streamoff>(stream.tellg());
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaxFileBytes)
        throw std::runtime_error("The device preset file is too large to open");
    stream.seekg(0);
    try {
        return decodePreset(Json::parse(stream), file);
    } catch (const Json::exception &error) {
        throw std::runtime_error(std::string("Invalid ADI device preset: ") + error.what());
    }
}
fs::path write(const fs::path &library, const Preset &preset) {
    requireOwner();
    if (library.empty())
        throw std::runtime_error("Set the User Library folder before saving presets");
    checkName(preset);
    const auto key = deviceKey(preset.device);
    render::NameContext names;
    names.project = preset.name;
    const auto stem = render::resolveName("{project}", names);
    const auto targetFor = [&](unsigned index) {
        const auto suffix = index <= 1 ? std::string() : " (" + std::to_string(index) + ")";
        auto name = media::pathFromUtf8(stem + suffix + std::string(kExtension));
        render::validateOutputName(name);
        return name;
    };
    const auto preferred = targetFor(1);
    const auto document = encodePreset(preset, key);
    fs::create_directories(library);
    const auto presets = library / "Presets";
    ensureDirectory(presets);
    const auto directory = presets / key;
    ensureDirectory(directory);
    const PresetStage stage(directory);
    writeDocument(stage.file(), document);
    for (unsigned index = 1; index <= 10000; ++index) {
        const auto target = directory / (index == 1 ? preferred : targetFor(index));
        const auto result = media::publishFile(stage.file(), target, {}, media::PublishPolicy::AtomicOnly);
        if (result.status == media::PublishStatus::published)
            return target;
        if (result.status != media::PublishStatus::exists)
            throw std::runtime_error("Could not save the device preset: " + result.error);
    }
    throw std::runtime_error("No unused name is available for the device preset");
}
std::optional<Preset> readDefault(const fs::path &library, const Identity &identity) {
    requireOwner();
    if (library.empty())
        return std::nullopt;
    const auto file = defaultPath(library, deviceKey(identity));
    const auto directory = file.parent_path();
    for (const auto &folder : {directory.parent_path(), directory}) {
        const auto status = fs::symlink_status(folder);
        if (!fs::exists(status))
            return std::nullopt;
        if (!fs::is_directory(status))
            throw std::runtime_error("The device defaults folder is not an ordinary directory");
    }
    if (!fs::exists(fs::symlink_status(file)))
        return std::nullopt;
    auto loaded = read(file);
    if (loaded.device != identity)
        throw std::runtime_error("The default preset does not match this device");
    return loaded;
}
fs::path writeDefault(const fs::path &library, const Preset &preset) {
    requireOwner();
    if (library.empty())
        throw std::runtime_error("Set the User Library folder before saving a device default");
    checkName(preset);
    const auto key = deviceKey(preset.device);
    const auto file = defaultPath(library, key);
    const auto directory = file.parent_path();
    const auto document = encodePreset(preset, key);
    fs::create_directories(library);
    ensureDirectory(directory.parent_path());
    ensureDirectory(directory);
    const PresetStage stage(directory);
    writeDocument(stage.file(), document);
    auto result = media::publishFile(stage.file(), file, {}, media::PublishPolicy::AtomicOnly);
    if (result.status == media::PublishStatus::exists)
        result = media::replaceFile(stage.file(), file);
    if (result.status != media::PublishStatus::published)
        throw std::runtime_error("Could not save the default device preset: " + result.error);
    return file;
}
} // namespace adi::preset
