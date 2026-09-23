// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loader: a `plugin_refs` row becomes a live `Vst3Device` or `ClapDevice`.
// ADR-0122 d2 -- the second of the two things JUCE contributes to a session
// (the first is the callback, `DeviceBridge`).
//
// WHAT IT DECIDES, and the session does not:
//
//   * **How a row is matched to a binary.** `plugin_refs.uid` is the identity
//     (VST3: JUCE's identifier string, as `identityOf` records it; CLAP: the
//     descriptor's reverse-DNS id). `path_hint` is tried FIRST for VST3 -- a
//     file the project remembers is exact and costs one `findAllTypesForFile`
//     rather than a directory scan -- and is the tie-break for CLAP when one id
//     is installed twice. The scan runs once, lazily, on the first row that
//     needs it, and again only after a search path is added.
//   * **An instance that did not load is NOT returned.** Both hosts hand back a
//     `MissingDevice` rather than null on failure (ADR-0011), which is right
//     for a probe and wrong here: the SESSION builds the placeholder, because
//     only it has the `plugin_params` mirror and the `plugin_state` bytes to
//     put in it (ADR-0122 d4). So `loaded() == false` becomes null plus the
//     host's error, and every placeholder in a session is the session's.
//   * **A format we name but do not host is refused with the ADR that says so.**
//     `au`, `vst2`, `lv2`, `ladspa` (ADR-0041); `internal` until native
//     devices exist. The row still opens -- as a placeholder -- which is the
//     whole reason `plugin_refs.format` admits them (SPEC 7.4).
#pragma once

#include "adi/engine/session.hpp"
#include "juce/clap_host.hpp"
#include "juce/vst3_host.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adi::device {

class JuceDeviceLoader {
public:
    JuceDeviceLoader();

    /// A directory searched for both formats, ahead of the defaults. Adding
    /// one after a scan schedules another scan on the next request.
    void addSearchPath(const juce::File& dir);

    /// Explicit scan; `fn()` also scans lazily on the first row that needs one.
    void scan();

    /// The session's `DeviceLoader`. Valid while this object lives.
    [[nodiscard]] engine::DeviceLoader fn();

    struct Stats {
        std::int64_t requests = 0;
        std::int64_t vst3 = 0;       ///< instances handed out
        std::int64_t clap = 0;
        std::int64_t unhosted = 0;   ///< a format we do not host
        std::int64_t notFound = 0;   ///< hosted format, no binary matched the uid
        std::int64_t failed = 0;     ///< matched, and did not instantiate
        std::int64_t scans = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] Vst3Host& vst3() noexcept { return vst3_; }
    [[nodiscard]] ClapHost& clap() noexcept { return clap_; }

    /// One line per plugin the last scan found, both formats: for `--list`,
    /// and for the day a browser row needs the same thing.
    [[nodiscard]] std::vector<std::string> knownPlugins() const;

private:
    std::unique_ptr<DeviceInstance> make(const engine::DeviceRequest& rq, std::string& err);
    std::unique_ptr<DeviceInstance> makeVst3(const engine::DeviceRequest& rq, std::string& err);
    std::unique_ptr<DeviceInstance> makeClap(const engine::DeviceRequest& rq, std::string& err);

    Vst3Host vst3_;
    ClapHost clap_;
    juce::KnownPluginList known_;
    juce::FileSearchPath vst3Paths_;
    std::vector<std::string> clapPaths_;
    bool scanned_ = false;
    Stats stats_;
};

}  // namespace adi::device
