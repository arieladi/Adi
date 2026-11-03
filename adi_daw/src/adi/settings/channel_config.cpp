// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/channel_config.hpp"

#include <string>

namespace adi::settings {
namespace {

constexpr const char* kKey = "audio.channelConfig";

bool readRows(const Value& side, const char* which, bool stereo, const std::string& where,
              std::vector<ChannelConfig::Mono>* monos, std::vector<ChannelConfig::Stereo>* stereos,
              std::string& why) {
    const auto it = side.find(which);
    if (it == side.end() || !it->is_array()) {
        why = where + " lists " + which + " channels";
        return false;
    }
    std::set<int> seen;
    for (const auto& j : *it) {
        if (!j.is_object() || !j.contains("ch") || !j.at("ch").is_number_integer() ||
            !j.contains("on") || !j.at("on").is_boolean()) {
            why = where + ": a " + which + " channel is its number and its switch";
            return false;
        }
        const int ch = j.at("ch").get<int>();
        if (ch < 1) { why = where + ": channels are numbered from 1"; return false; }
        if (!seen.insert(ch).second) {
            why = where + ": " + which + " channel " + std::to_string(ch) + " is listed twice";
            return false;
        }
        std::string name;
        if (j.contains("name")) {
            if (!j.at("name").is_string()) { why = where + ": a channel's name is text"; return false; }
            name = j.at("name").get<std::string>();
        }
        if (stereo) stereos->push_back({ch, j.at("on").get<bool>(), name});
        else monos->push_back({ch, j.at("on").get<bool>(), name});
    }
    return true;
}

bool readSide(const Value& record, const char* side, std::vector<ChannelConfig::Mono>& monos,
              std::vector<ChannelConfig::Stereo>& stereos, std::string& why) {
    const auto it = record.find(side);
    if (it == record.end() || !it->is_object()) {
        why = std::string("a channel configuration has ") + side;
        return false;
    }
    return readRows(*it, "mono", false, side, &monos, nullptr, why) &&
           readRows(*it, "stereo", true, side, nullptr, &stereos, why);
}

Value sideToJson(const std::vector<ChannelConfig::Mono>& monos, const std::vector<ChannelConfig::Stereo>& stereos) {
    Value mono = Value::array(), stereo = Value::array();
    for (const auto& m : monos) {
        Value j = {{"ch", m.channel}, {"on", m.on}};
        if (!m.name.empty()) j["name"] = m.name;
        mono.push_back(std::move(j));
    }
    for (const auto& s : stereos) {
        Value j = {{"ch", s.left}, {"on", s.on}};
        if (!s.name.empty()) j["name"] = s.name;
        stereo.push_back(std::move(j));
    }
    return {{"mono", std::move(mono)}, {"stereo", std::move(stereo)}};
}

std::set<int> active(const std::vector<ChannelConfig::Mono>& monos, const std::vector<ChannelConfig::Stereo>& stereos) {
    std::set<int> out;
    for (const auto& m : monos) if (m.on) out.insert(m.channel);
    for (const auto& s : stereos) if (s.on) { out.insert(s.left); out.insert(s.left + 1); }
    return out;
}

}  // namespace

std::set<int> ChannelConfig::activeInputs() const { return active(monoInputs, stereoInputs); }
std::set<int> ChannelConfig::activeOutputs() const { return active(monoOutputs, stereoOutputs); }

ChannelConfig defaultChannelConfig(int inputs, int outputs) {
    ChannelConfig c;
    for (int ch = 1; ch <= inputs; ++ch) c.monoInputs.push_back({ch, ch <= 2, ""});
    for (int ch = 1; ch + 1 <= inputs; ch += 2) c.stereoInputs.push_back({ch, ch == 1, ""});
    for (int ch = 1; ch <= outputs; ++ch) c.monoOutputs.push_back({ch, ch <= 2, ""});
    for (int ch = 1; ch + 1 <= outputs; ch += 2) c.stereoOutputs.push_back({ch, ch == 1, ""});
    return c;
}

std::string displayName(const ChannelConfig::Mono& m) {
    return m.name.empty() ? std::to_string(m.channel) + " (mono)" : m.name;
}

std::string displayName(const ChannelConfig::Stereo& s) {
    return s.name.empty() ? std::to_string(s.left) + "/" + std::to_string(s.left + 1) + " (stereo)" : s.name;
}

Value toJson(const ChannelConfig& c) {
    return {{"inputs", sideToJson(c.monoInputs, c.stereoInputs)},
            {"outputs", sideToJson(c.monoOutputs, c.stereoOutputs)}};
}

std::optional<ChannelConfig> channelConfigFromJson(const Value& j, std::string& why) {
    if (!j.is_object()) { why = "a channel configuration is a record"; return std::nullopt; }
    ChannelConfig c;
    if (!readSide(j, "inputs", c.monoInputs, c.stereoInputs, why) ||
        !readSide(j, "outputs", c.monoOutputs, c.stereoOutputs, why))
        return std::nullopt;
    return c;
}

bool checkChannelConfigs(const Value& table, std::string& why) {
    if (!table.is_object()) { why = std::string(kKey) + " is a record per device"; return false; }
    for (const auto& [device, record] : table.items()) {
        std::string inner;
        if (!channelConfigFromJson(record, inner)) {
            why = std::string(kKey) + "[" + device + "]: " + inner;
            return false;
        }
    }
    return true;
}

std::optional<ChannelConfig> channelConfig(const AppSettings& s, std::string_view device) {
    const Value table = s.get(kKey);
    if (!table.is_object()) return std::nullopt;
    const auto it = table.find(std::string(device));
    if (it == table.end()) return std::nullopt;
    std::string why;
    return channelConfigFromJson(*it, why);
}

bool setChannelConfig(AppSettings& s, std::string_view device, const ChannelConfig& c,
                      const ChangeContext& who, std::string& why) {
    Value table = s.get(kKey);
    if (!table.is_object()) table = Value::object();
    table[std::string(device)] = toJson(c);
    return s.set(kKey, table, who, why);
}

}  // namespace adi::settings
