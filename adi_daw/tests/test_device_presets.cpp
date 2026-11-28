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
              loaded.parameters == original.parameters && loaded.state == original.state && !loaded.panel,
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
    test([](Json &j) { j["panel"] = "12"; }, "SC-299 a panel must list its controls in an array");
    test([](Json &j) { j["panel"] = Json::array({12}); }, "SC-299 a numeric panel ID is refused");
    test([](Json &j) { j["panel"] = Json::array({""}); }, "SC-299 an empty panel ID is refused");
    test([](Json &j) { j["panel"] = Json::array({std::string("x\0y", 3)}); }, "SC-299 a panel ID with a NUL is refused");
    test([](Json &j) { j["panel"] = Json::array({"12", "17", "12"}); }, "SC-299 duplicate panel IDs are refused");
    const auto wrongFolder = library / "Presets" / preset::deviceKey(clap.device) / "wrong.adipreset";
    copyJson(wrongFolder, baseline);
    check(refuses([&] { (void)preset::read(wrongFolder); }),
          "SC-85 a preset listed under a different device is refused on open");
}
void defaultPreset() {
    const adi::test::TempDirectory temp("device_presets", "defaults");
    const auto library = temp.path() / "User Library";
    preset::Preset values;
    values.device = {"vst3", "0123456789ABCDEF0123456789ABCDEF", 1};
    values.deviceName = "Example device";
    values.name = "Example Default";
    values.parameters = {{"3", .5}, {"22", .75}};
    values.state = {{"chunk", {std::byte{0xff}, std::byte{0}, std::byte{0x12}}}};
    values.panel = std::vector<std::string>{"22", "3", "13"};
    const auto originalPanel = values.panel;
    check(!preset::readDefault(library, values.device), "SC-299 a missing User Library has no default");
    const auto namedFile = preset::write(library, values);
    check(!preset::readDefault(library, values.device) && preset::read(namedFile).panel == values.panel,
          "SC-299 a named preset retains panel order and is independent of the default");

    const auto defaultFile = preset::writeDefault(library, values);
    const auto stored = preset::readDefault(library, values.device);
    check(defaultFile == library / "Defaults" / "Devices" / (preset::deviceKey(values.device) + ".adipreset") &&
              stored && stored->device == values.device && stored->parameters == values.parameters &&
              stored->state == values.state && stored->panel == values.panel,
          "SC-299 the stable default retains parameters, opaque state and ordered panel controls");

    auto neighbor = values;
    neighbor.device.shellId = 2;
    check(!preset::readDefault(library, neighbor.device), "SC-299 equal plug-in UIDs with different shells have different defaults");
    const auto neighborFile = preset::writeDefault(library, neighbor);
    const auto otherStored = preset::readDefault(library, neighbor.device);
    check(neighborFile != defaultFile && otherStored && otherStored->device == neighbor.device,
          "SC-299 a second shell writes an independent default");
    const auto entries = preset::list(library);
    check(entries.size() == 1 && entries.front().path == namedFile, "SC-299 defaults do not enter the Browser's named list");

    values.name = "A different default name";
    values.parameters["22"] = .125;
    values.panel = std::vector<std::string>{};
    const auto replacement = preset::writeDefault(library, values);
    const auto updated = preset::readDefault(library, values.device);
    check(replacement == defaultFile && updated && updated->name == values.name && updated->parameters == values.parameters &&
              updated->panel && updated->panel->empty() && preset::read(namedFile).panel == originalPanel,
          "SC-299 resaving atomically replaces the same file and records an intentionally empty panel");
    auto invalid = values;
    invalid.panel = std::vector<std::string>{"12", "12"};
    check(refuses([&] { (void)preset::writeDefault(library, invalid); }) && preset::read(defaultFile).panel == values.panel,
          "SC-299 an invalid save leaves the prior default intact");

    fs::copy_file(neighborFile, defaultFile, fs::copy_options::overwrite_existing);
    check(refuses([&] { (void)preset::readDefault(library, values.device); }),
          "SC-299 a valid preset at another device's path is refused by exact identity");
    check(preset::writeDefault(library, values) == defaultFile && preset::read(defaultFile).device == values.device,
          "SC-299 saving again repairs a misfiled default");
}
} // namespace

int main() {
    try {
        portablePreset();
        externalPreset();
        defaultPreset();
    } catch (const std::exception &error) {
        check(false, error.what());
    }
    std::printf("device presets: %d checks, %d failures\n", checks, failures);
    return failures != 0 ? 1 : 0;
}
