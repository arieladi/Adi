// SPDX-License-Identifier: GPL-3.0-or-later
//
// Live's Channel Configuration (manual 17.2; the Audio page's Input Config and
// Output Config buttons): for each audio device, which mono channels and stereo
// pairs are in use and what they are called. The names belong to the device
// (17.2: "always associated with their respective audio device"), so the setting
// `audio.channelConfig` is one record per device under the device's name, kept
// with the application (N20 of the window's needs table).
//
// Channels are numbered from 1 as Live shows them; a pair is its left channel and
// the next. A device with no record uses Live's default: every channel and pair
// listed, 1, 2 and 1/2 on, the rest off.

#pragma once

#include "adi/settings/store.hpp"

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace adi::settings {

struct ChannelConfig {
    struct Mono {
        int channel = 1;
        bool on = false;
        std::string name;   // empty: Live's "1 (mono)"
        bool operator==(const Mono&) const = default;
    };
    struct Stereo {
        int left = 1;       // the pair is left and left + 1
        bool on = false;
        std::string name;   // empty: Live's "1/2 (stereo)"
        bool operator==(const Stereo&) const = default;
    };
    std::vector<Mono> monoInputs, monoOutputs;
    std::vector<Stereo> stereoInputs, stereoOutputs;
    bool operator==(const ChannelConfig&) const = default;

    /// Every channel an enabled mono or an enabled pair uses: what the device is
    /// opened with (JUCE's AudioDeviceSetup channel bits, 1-based here).
    [[nodiscard]] std::set<int> activeInputs() const;
    [[nodiscard]] std::set<int> activeOutputs() const;
};

/// Live's default for a device with that many inputs and outputs.
[[nodiscard]] ChannelConfig defaultChannelConfig(int inputs, int outputs);

/// What the chooser shows: the channel's own name, or Live's.
[[nodiscard]] std::string displayName(const ChannelConfig::Mono&);
[[nodiscard]] std::string displayName(const ChannelConfig::Stereo&);

[[nodiscard]] Value toJson(const ChannelConfig&);
/// Refuses a record of the wrong shape: a channel below 1, one listed twice, a
/// switch that is not a boolean, a name that is not text, a missing side.
[[nodiscard]] std::optional<ChannelConfig> channelConfigFromJson(const Value&, std::string& why);

/// The whole setting, device name -> record: the registry row's check.
[[nodiscard]] bool checkChannelConfigs(const Value& table, std::string& why);

/// The record kept for that device, or nothing: the window then starts from
/// defaultChannelConfig with the device's counts.
[[nodiscard]] std::optional<ChannelConfig> channelConfig(const AppSettings&, std::string_view device);

/// Keeps that device's record. The setting is validated whole, so a bad record is
/// refused and nothing changes. Does not save.
bool setChannelConfig(AppSettings&, std::string_view device, const ChannelConfig&,
                      const ChangeContext& who, std::string& why);

}  // namespace adi::settings
