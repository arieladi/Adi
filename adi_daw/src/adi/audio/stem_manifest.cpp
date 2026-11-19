// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/stem_manifest.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace adi::audio {
bool stemColorValid(const std::string &color) noexcept {
    return color.empty() || (color.size() == 7 && color.front() == '#' &&
                             std::all_of(color.begin() + 1, color.end(), [](char c) {
                                 return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                                        (c >= 'a' && c <= 'f');
                             }));
}
StemManifest parseStemManifest(const std::string &original) {
    if (original.empty() || original.size() > 1024 * 1024)
        throw std::runtime_error("NI Stems manifest is missing or excessive");
    const auto end = original.find_last_not_of(char{0});
    if (end == std::string::npos)
        throw std::runtime_error("NI Stems manifest is empty");
    const auto root =
        nlohmann::json::parse(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(end + 1),
                              [](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
                                  if (depth > 32)
                                      throw std::runtime_error("NI Stems manifest nesting exceeds limit");
                                  return true;
                              });
    if (!root.is_object() || !root.contains("version") || !root.at("version").is_number_integer() ||
        root.at("version") != 1)
        throw std::runtime_error("unsupported NI Stems manifest version");
    const auto &rows = root.at("stems");
    if (!rows.is_array() || rows.empty() || rows.size() > 4)
        throw std::runtime_error("NI Stems manifest requires one to four stems");
    StemManifest out;
    for (const auto &row : rows) {
        if (!row.is_object())
            throw std::runtime_error("NI Stems label must be an object");
        StemLabel label{row.value("name", std::string{}), row.value("color", std::string{})};
        if (label.name.size() > 1024 || !stemColorValid(label.color))
            throw std::runtime_error("invalid NI Stems name or RGB color");
        if (label.name.empty())
            label.name = "Stem " + std::to_string(out.stems.size() + 1);
        out.stems.push_back(std::move(label));
    }
    if (root.contains("mastering_dsp"))
        for (const auto *effect : {"compressor", "limiter"}) {
            const auto &dsp = root.at("mastering_dsp");
            if (!dsp.is_object())
                throw std::runtime_error("invalid NI mastering DSP metadata");
            if (dsp.contains(effect)) {
                const auto &object = dsp.at(effect);
                if (!object.is_object())
                    throw std::runtime_error("invalid NI mastering effect");
                const bool enabled = object.value("enabled", false);
                out.masteringEnabled = out.masteringEnabled || enabled;
            }
        }
    return out;
}
std::string makeStemManifest(const std::vector<StemLabel> &labels) {
    nlohmann::json stems = nlohmann::json::array();
    for (const auto &label : labels)
        stems.push_back({{"name", label.name}, {"color", label.color.empty() ? "#009e73" : label.color}});
    nlohmann::json root = {
        {"version", 1},
        {"stems", stems},
        {"mastering_dsp",
         {{"compressor",
           {{"ratio", 3},
            {"output_gain", 1},
            {"enabled", false},
            {"threshold", 0},
            {"attack", 0.003},
            {"input_gain", 1},
            {"release", 0.3},
            {"hp_cutoff", 300},
            {"dry_wet", 100}}},
          {"limiter", {{"release", 0.05}, {"threshold", 0}, {"ceiling", 0}, {"enabled", false}}}}}};
    const auto text = root.dump();
    (void)parseStemManifest(text);
    return text;
}
} // namespace adi::audio
