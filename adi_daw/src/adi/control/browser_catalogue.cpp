// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/control/browser_catalogue.hpp"

#include "adi/builtin_catalog.hpp"
#include "adi/device_presets.hpp"
#include "adi/ui/plugin_listing.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace adi::control {
namespace {

// MCP can put the same result in structuredContent and in a JSON-encoded
// text block. Leave room for both representations under the socket cap.
constexpr std::size_t kPageByteBudget = 64 * 1024;

Json deviceIdentity(const preset::Identity& identity) {
    Json result{{"format", identity.format}, {"uid", identity.uid}};
    if (identity.shellId) result["shellId"] = *identity.shellId;
    return result;
}

std::string_view pluginRole(ui::PluginEntry::Kind kind) {
    using Kind = ui::PluginEntry::Kind;
    switch (kind) {
        case Kind::Instrument: return "instrument";
        case Kind::Effect: return "effect";
        case Kind::MidiEffect: return "midi_effect";
        case Kind::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace

void BrowserCatalogue::add(Json row, std::string_view name, std::string_view vendor,
                           std::string_view deviceName) {
    const auto bytes = dumpJson(row).size() + 1;
    rows_.push_back({std::move(row), ui::lowerCase(name), ui::lowerCase(vendor), ui::lowerCase(deviceName), bytes});
}

BrowserCatalogue::BrowserCatalogue(const ui::PluginListing& listing, std::span<const preset::Entry> presets) {
    std::map<std::string, std::vector<const preset::Entry*>> presetsByDevice;
    for (const auto& entry : presets) presetsByDevice[entry.deviceKey].push_back(&entry);

    const auto addDevice = [&](const preset::Identity& identity, Json row, std::string_view name,
                               std::string_view vendor = {}) {
        Json device = deviceIdentity(identity);
        row.update(device);
        const auto key = preset::deviceKey(identity);
        row["deviceKey"] = key;
        add(std::move(row), name, vendor);
        const auto linked = presetsByDevice.find(key);
        if (linked == presetsByDevice.end()) return;
        for (const auto* preset : linked->second) {
            add({{"kind", "preset"}, {"name", preset->name}, {"deviceKey", key}, {"device", device}},
                preset->name, vendor, name);
        }
        presetsByDevice.erase(linked);
    };

    for (const auto& builtin : builtinCatalog()) {
        addDevice({"internal", std::string(builtin.uid), std::nullopt},
                  {{"kind", "device"}, {"name", builtin.name}, {"category", builtin.category},
                   {"group", builtin.group}, {"role", builtin.instrument ? "instrument" : "effect"},
                   {"available", builtin.available}}, builtin.name);
    }
    for (const auto& group : ui::pluginGroups(listing, {})) {
        for (const auto* plugin : group.entries) {
            addDevice({plugin->format, plugin->uid, plugin->shellId},
                      {{"kind", "plugin"}, {"name", plugin->name}, {"vendor", plugin->vendor},
                       {"version", plugin->version}, {"role", pluginRole(plugin->kind)}, {"available", true}},
                      plugin->name, plugin->vendor);
        }
    }
}

Json BrowserCatalogue::page(std::string_view query, std::size_t offset, std::size_t limit) const {
    const auto filter = ui::lowerCase(query);
    limit = std::clamp<std::size_t>(limit, 1, kMaxLimit);
    Json entries = Json::array();
    std::size_t total = 0;
    std::size_t bytes = 0;
    bool full = false;
    for (const auto& row : rows_) {
        if (row.name.find(filter) == std::string::npos && row.vendor.find(filter) == std::string::npos &&
            row.deviceName.find(filter) == std::string::npos)
            continue;
        if (total++ < offset || full) continue;
        if (entries.size() == limit || row.bytes > kPageByteBudget - bytes) {
            full = true;
            continue;
        }
        bytes += row.bytes;
        entries.push_back(row.data);
    }
    if (offset < total && entries.empty()) throw std::length_error("a catalogue row exceeds the page byte limit");
    const auto next = offset + entries.size();
    Json result{{"offset", offset}, {"total", total}, {"entries", std::move(entries)}};
    if (next < total) result["nextOffset"] = next;
    return result;
}

} // namespace adi::control
