// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/settings/store.hpp"
#include "juce/juce_device_loader.hpp"
#include <algorithm>
#include <optional>

namespace adi::device {
/// Resolve the existing source settings without filesystem probing. Injectable
/// defaults keep tests away from installed roots. Relative custom paths follow
/// the CLI's current-directory rule; empty entries are ignored.
inline std::optional<PluginSearchPaths> pluginSearchPathsFromSettings(
    const settings::AppSettings& settings,PluginSearchPaths defaults,
    const juce::File& relativeBase,std::string& error) {
    error.clear();
    if(!settings.get("plugins.useVst3SystemFolders").get<bool>())defaults.vst3=juce::FileSearchPath{};
    const auto append=[&](const char* key,bool vst3) {
        const auto entries=settings.get(key);
        for(const auto& value:entries) {
            const auto path=value.get<std::string>();
            if(path.empty())continue;
            if(path.find('\0')!=std::string::npos) {
                error=std::string(key)+" contains a folder with an embedded NUL";return false;
            }
            const auto text=juce::String::fromUTF8(path.c_str());
            const auto file=juce::File::isAbsolutePath(text)?juce::File(text):relativeBase.getChildFile(text);
            if(vst3)defaults.vst3.addIfNotAlreadyThere(file);
            else {
                const auto full=file.getFullPathName().toStdString();
                if(std::find(defaults.clap.begin(),defaults.clap.end(),full)==defaults.clap.end())defaults.clap.push_back(full);
            }
        }
        return true;
    };
    if(!append("plugins.vst3Folders",true) || !append("plugins.clapFolders",false))return {};
    return defaults;
}

/// Cheap owner-boundary adapter used at launch and after Settings changes.
/// Changes only future discovery inputs; it neither scans nor republishes.
inline bool configurePluginSearchPaths(JuceDeviceLoader& loader,
    const settings::AppSettings& settings,std::string& error) {
    if(audio::onAudioThread)return false;
    auto paths=pluginSearchPathsFromSettings(settings,loader.defaultSearchPaths(),
                                            juce::File::getCurrentWorkingDirectory(),error);
    if(!paths)return false;
    loader.setSearchPaths(*paths);return true;
}
} // namespace adi::device
