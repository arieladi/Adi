// SPDX-License-Identifier: GPL-3.0-or-later
//
// See juce_device_loader.hpp. ADR-0122.

#include "juce/juce_device_loader.hpp"

#include <string>
#include <utility>

namespace adi::device {

JuceDeviceLoader::JuceDeviceLoader()
    : vst3Paths_(vst3_.defaultSearchPaths()), clapPaths_(ClapHost::defaultSearchPaths()) {}

void JuceDeviceLoader::addSearchPath(const juce::File& dir) {
    vst3Paths_.add(dir);
    clapPaths_.push_back(dir.getFullPathName().toStdString());
    scanned_ = false;
}

void JuceDeviceLoader::scan() {
    if(audio::onAudioThread)return;
    scanReport_=vst3_.scan(vst3Paths_,known_,scanConfig_);
    auto clapReport=clap_.scan(clapPaths_,scanConfig_);
    scanReport_.complete=scanReport_.complete && clapReport.complete;
    scanReport_.cancelled=scanReport_.cancelled || clapReport.cancelled;
    for(auto& failure:clapReport.failures)scanReport_.failures.push_back(std::move(failure));
    scanned_ = scanReport_.complete && !scanReport_.cancelled;
    ++stats_.scans;
}

engine::DeviceLoader JuceDeviceLoader::fn() {
    return [this](const engine::DeviceRequest& rq, std::string& err) { return make(rq, err); };
}

std::vector<std::string> JuceDeviceLoader::knownPlugins() const {
    std::vector<std::string> out;
    for (const juce::PluginDescription& d : known_.getTypes()) {
        const DeviceIdentity id = identityOf(d);
        out.push_back(id.format + "  " + id.uid + "  " + id.name + " -- " + id.vendor + " " +
                      id.version + "  (" + d.fileOrIdentifier.toStdString() + ")");
    }
    for (const ClapPluginRef& p : clap_.plugins())
        out.push_back("clap  " + p.id + "  " + p.name + " -- " + p.vendor + " " + p.version +
                      "  (" + p.bundlePath + ")" + (p.isInstrument ? "  [instrument]" : ""));
    return out;
}

std::unique_ptr<DeviceInstance> JuceDeviceLoader::make(const engine::DeviceRequest& rq,
                                                       std::string& err) {
    ++stats_.requests;
    if (rq.ref == nullptr) {
        ++stats_.failed;
        err = "no plugin reference";
        return nullptr;
    }
    const std::string& f = rq.ref->format;
    if (f == "vst3") return makeVst3(rq, err);
    if (f == "clap") return makeClap(rq, err);
    ++stats_.unhosted;
    err = (f == "internal") ? std::string("internal devices are not built yet")
                            : "format '" + f + "' is not hosted (ADR-0041)";
    return nullptr;
}

std::unique_ptr<DeviceInstance> JuceDeviceLoader::makeVst3(const engine::DeviceRequest& rq,
                                                           std::string& err) {
    const rows::PluginRef& ref = *rq.ref;
    juce::PluginDescription found;
    bool have = false;

    // The hint first: exact, and one file rather than a directory tree.
    if (!ref.pathHint.empty() && juce::File::isAbsolutePath(juce::String(ref.pathHint)) &&
        juce::File(juce::String(ref.pathHint)).exists()) {
        juce::KnownPluginList types;
        const auto report=vst3_.scanFile(juce::String::fromUTF8(ref.pathHint.c_str()),types,scanConfig_);
        if(!report.complete || !report.failures.empty()) {
            ++stats_.failed;err=report.failures.empty()?"VST3 path-hint scan incomplete":report.failures.front().error;
            return nullptr;
        }
        for (const auto& d : types.getTypes()) {
            if (identityOf(d).uid == ref.uid) {
                found = d;
                have = true;
                break;
            }
        }
    }
    if (!have) {
        if (!scanned_) scan();
        if(!scanned_){++stats_.failed;err="VST3 discovery unavailable or cancelled";return nullptr;}
        for (const juce::PluginDescription& d : known_.getTypes()) {
            if (identityOf(d).uid == ref.uid) {
                found = d;
                have = true;
                break;
            }
        }
    }
    if (!have) {
        ++stats_.notFound;
        err = "no VST3 with uid '" + ref.uid + "' in the search path (" +
              std::to_string(known_.getNumTypes()) + " known" +
              (ref.pathHint.empty() ? std::string() : "; hint '" + ref.pathHint + "'") + ")";
        return nullptr;
    }

    std::unique_ptr<DeviceInstance> dev = vst3_.makeDevice(found, rq.sampleRate, rq.maxFrames, err);
    if (dev == nullptr || !dev->loaded()) {
        // The host's placeholder is discarded: the session builds its own,
        // with the mirror and the bytes (ADR-0122 d4).
        ++stats_.failed;
        if (err.empty()) err = "the plugin did not instantiate";
        return nullptr;
    }
    ++stats_.vst3;
    return dev;
}

std::unique_ptr<DeviceInstance> JuceDeviceLoader::makeClap(const engine::DeviceRequest& rq,
                                                           std::string& err) {
    const rows::PluginRef& ref = *rq.ref;
    if (!scanned_) scan();
    if(!scanned_){++stats_.failed;err="CLAP discovery unavailable or cancelled";return nullptr;}
    // Discovery/quarantine resolves aliases. Preserve an older project's
    // preferred installed copy when its saved hint used a symlink or dots.
    std::string preferred=ref.pathHint;
    if(!preferred.empty()) {
        std::error_code error;
        const auto canonical=std::filesystem::canonical(
            std::filesystem::path(std::u8string(preferred.begin(),preferred.end())),error);
        if(!error){const auto bytes=canonical.u8string();preferred.assign(bytes.begin(),bytes.end());}
    }
    const ClapPluginRef* pick = nullptr;
    for (const ClapPluginRef& p : clap_.plugins()) {
        if (p.id != ref.uid) continue;
        if (pick == nullptr || p.bundlePath == preferred) pick = &p;
    }
    if (pick == nullptr) {
        ++stats_.notFound;
        err = "no CLAP with id '" + ref.uid + "' in the search path (" +
              std::to_string(clap_.plugins().size()) + " known)";
        return nullptr;
    }
    std::unique_ptr<DeviceInstance> dev = clap_.makeDevice(*pick, rq.sampleRate, rq.maxFrames, err);
    if (dev == nullptr || !dev->loaded()) {
        ++stats_.failed;
        if (err.empty()) err = "the plugin did not instantiate";
        return nullptr;
    }
    ++stats_.clap;
    return dev;
}

}  // namespace adi::device
