// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/device_presets.hpp"
#include "adi/media/media_ops.hpp"
#include "temp_directory.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
namespace preset = adi::preset;
using Json = nlohmann::json;
int checks = 0, failures = 0;
void check(bool result, const char *message) {
    ++checks;
    if (!result) {
        ++failures;
        std::printf("FAIL %s\n", message);
    }
}
template <class Action> bool refuses(Action action) {
    try {
        action();
        return false;
    } catch (const std::exception &error) {
        return *error.what() != '\0';
    }
}
void copyJson(const fs::path &output, const Json &document) {
    std::ofstream file(output, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << document;
    file.close();
}
void portablePreset() {
    const adi::test::TempDirectory temp("device_presets", "files");
    const auto library = temp.path() / "User Library";
    const preset::Identity device{"internal", "org.adidaw.builtin.oneshot", std::nullopt};
    preset::Preset original;
    original.device = device;
    original.deviceName = "OneShot";
    original.name = "AUX .{preset}/" "\xE2\x99\xAB";
    original.parameters = {{"17", .25}, {"33", 1.0}};
    original.state = {{"state", {std::byte{0}, std::byte{0xff}, std::byte{0x23}, std::byte{0x10}}},
                      {"sample:1", {std::byte{0xff}, std::byte{0}}}};
    check(preset::list(library).empty(), "SC-85 an absent User Library has no presets");
    const auto first = preset::write(library, original);
    const auto key = preset::deviceKey(device);
    const auto fileName = adi::media::pathUtf8(first.filename());
    check(first.parent_path() == library / "Presets" / key && first.extension() == ".adipreset" &&
              fileName.starts_with("_AUX .") && fileName.find('_', 1) != std::string::npos &&
              fileName.find("{preset}") != std::string::npos,
          "SC-85 a reserved Windows name and separator are made portable without expanding braces");
    const auto loaded = preset::read(first);
    check(loaded.device == device && loaded.name == original.name && loaded.deviceName == original.deviceName &&
              loaded.parameters == original.parameters && loaded.state == original.state,
          "SC-85 readable preset round-trips normalized values, Unicode, and arbitrary state bytes");
    auto altered = original;
    altered.parameters["17"] = .75;
    const auto second = preset::write(library, altered);
    check(second != first && adi::media::pathUtf8(second.stem()).ends_with(" (2)") &&
              preset::read(first).parameters == original.parameters &&
              preset::read(second).parameters == altered.parameters,
          "SC-85 a duplicate name publishes another file and keeps the previous file intact");

    const auto broken = first.parent_path() / "broken.adipreset";
    std::ofstream(broken) << "not a preset";
    std::ofstream(first.parent_path() / ".adi-preset-stage.tmp") << "{}";
    const auto entries = preset::list(library);
    check(entries.size() == 3 && std::ranges::any_of(entries, [&](const auto &entry) {
              return entry.deviceKey == key && entry.name == "broken" && entry.path == broken;
          }),
          "SC-85 listing groups filenames by device without loading their state");
    check(refuses([&] { (void)preset::read(broken); }), "SC-85 malformed listed data refuses on open");
    check(std::ranges::none_of(fs::directory_iterator(first.parent_path()), [](const auto &entry) {
              return entry.is_directory() &&
                     adi::media::pathUtf8(entry.path().filename()).starts_with(".adi-preset-");
          }),
          "SC-85 exclusive publication cleans its own staging directories");
    auto invalid = original;
    invalid.parameters["17"] = std::numeric_limits<double>::quiet_NaN();
    check(refuses([&] { (void)preset::write(library, invalid); }) && preset::list(library).size() == entries.size(),
          "SC-85 an invalid parameter cannot publish a partial preset");
}
void externalPreset() {
    const adi::test::TempDirectory temp("device_presets", "validation");
    const auto library = temp.path() / "User Library";
    preset::Preset plug;
    plug.device = {"vst3", "0123456789ABCDEF0123456789ABCDEF", std::numeric_limits<std::int64_t>::max()};
    plug.deviceName = "Example device";
    plug.name = "Example preset";
    plug.parameters.emplace("12", .5);
    plug.state.emplace("chunk", std::vector{std::byte{0xff}});
    const auto file = preset::write(library, plug);
    check(preset::read(file).device == plug.device && preset::read(file).state == plug.state,
          "SC-85 opaque VST3 state and full signed shell identity round-trip");
    auto clap = plug;
    clap.device.format = "clap";
    clap.device.shellId.reset();
    check(preset::deviceKey(clap.device) != preset::deviceKey(plug.device) &&
              preset::read(preset::write(library, clap)).device == clap.device,
          "SC-85 the plug-in format distinguishes otherwise equal identities");
    auto unshelled = plug.device;
    unshelled.shellId.reset();
    check(preset::deviceKey(unshelled) != preset::deviceKey(plug.device),
          "SC-85 shell identifiers distinguish devices with the same UID");

    Json baseline;
    {
        std::ifstream source(file, std::ios::binary);
        source >> baseline;
    }
    const auto copy = temp.path() / "standalone.adipreset";
    copyJson(copy, baseline);
    check(preset::read(copy).state == plug.state, "SC-85 the file opens independently of its User Library");
    const auto test = [&](auto edit, const char *message) {
        auto document = baseline;
        edit(document);
        copyJson(copy, document);
        check(refuses([&] { (void)preset::read(copy); }), message);
    };
    test([](Json &j) { j["version"] = 2; }, "SC-85 an unknown file version refuses");
    test([](Json &j) { j["device"]["uid"] = "another-device"; }, "SC-85 the key must match the device identity");
    test([](Json &j) { j["parameters"][0]["norm"] = 1.5; }, "SC-85 non-normalized data refuses");
    test([](Json &j) { const auto parameter = j["parameters"][0]; j["parameters"].push_back(parameter); },
         "SC-85 repeated parameter identities refuse");
    test([](Json &j) { const auto role = j["state"][0]; j["state"].push_back(role); },
         "SC-85 repeated state roles refuse");
    test([](Json &j) { j["state"][0]["base64"] = "AA=="; }, "SC-85 state bytes must match the BLAKE3");
    test([](Json &j) { j["state"][0]["base64"] = "/x=="; }, "SC-85 noncanonical base64 padding refuses");
    const auto wrongFolder = library / "Presets" / preset::deviceKey(clap.device) / "wrong.adipreset";
    copyJson(wrongFolder, baseline);
    check(refuses([&] { (void)preset::read(wrongFolder); }),
          "SC-85 a preset listed under a different device is refused on open");
}
} // namespace

int main() {
    try {
        portablePreset();
        externalPreset();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("device presets: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
