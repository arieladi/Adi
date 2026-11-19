// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loader: a `plugin_refs` row becomes a live VST3, CLAP or canonical built-in device.
// ADR-0122 d2 -- the second of the two things JUCE contributes to a session
// (the first is the callback, `DeviceBridge`).
//
// WHAT IT DECIDES, and the session does not:
//
//   * **How a row is matched to a binary.** `plugin_refs.uid` is the identity
//     (VST3: JUCE's identifier string, as `identityOf` records it; CLAP: the
//     descriptor's reverse-DNS id). VST3 checks the in-memory catalogue first.
//     Discovery at an unknown VST3 path_hint contributes its descriptions to
//     that catalogue. CLAP uses path_hint to choose between copies of one id.
//     A hydrated catalogue miss needs explicit discovery. Lazy discovery is
//     retained for a fresh loader with no registry or a validated v1/v2 schema.
//   * **An instance that did not load is NOT returned.** Both hosts hand back a
//     `MissingDevice` rather than null on failure (ADR-0011), which is right
//     for a probe and wrong here: the SESSION builds the placeholder, because
//     only it has the `plugin_params` mirror and the `plugin_state` bytes to
//     put in it (ADR-0122 d4). So `loaded() == false` becomes null plus the
//     host's error, and every placeholder in a session is the session's.
//   * **A format we name but do not host is refused with the ADR that says so.**
//     AU, VST2, LV2 and LADSPA (ADR-0041). Unsupported internal UIDs
//     also open as placeholders, which is the
//     whole reason `plugin_refs.format` admits them (SPEC 7.4).
#pragma once

#include "adi/engine/session.hpp"
#include "adi/audio/io_audit.hpp"
#include "juce/clap_host.hpp"
#include "juce/vst3_host.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace adi::device {

enum class DiscoveryMode { Incremental, CacheOnly };
enum class DiscoveryOutcome { Published, Missing, Legacy, Failed, Cancelled };

/// Owned format-validated metadata, not instantiated/restored/audio readiness.
/// Reuses the actual host metadata types; no second registry/wire schema.
struct DiscoveryCatalogue {
    std::vector<juce::PluginDescription> vst3;
    std::vector<ClapPluginRef> clap;
};
struct DiscoveryResult {
    DiscoveryMode mode = DiscoveryMode::CacheOnly;
    DiscoveryOutcome outcome = DiscoveryOutcome::Failed;
    scan::Report report;
    std::string error;
    /// New snapshot only for Published. Other outcomes retain the last
    /// accepted snapshot (possibly null), never a partially scanned prefix.
    std::shared_ptr<const DiscoveryCatalogue> catalogue;
};

class JuceDeviceLoader {
public:
    explicit JuceDeviceLoader(bool includeDefaultSearchPaths = true);

    /// A directory searched by explicit discovery, ahead of the defaults.
    /// Adding a root does not invalidate or bypass a hydrated snapshot.
    void addSearchPath(const juce::File& dir);

    /// Explicit owner scan. A fresh loader with no cache-capable registry retains legacy
    /// lazy discovery; after hydration, misses report NeedsDiscovery instead.
    void scan();

    /// Synchronous owner-side operation. Incremental enumerates configured roots
    /// and reuses the isolated scanner/cache. CacheOnly reads stored metadata
    /// without checking a helper or touching candidates. Both publish an owned
    /// const result, safe to retain after later operations or loader destruction.
    /// Explicit CacheOnly with missing/legacy storage never opts into lazy scan;
    /// hydrateCatalogue below preserves that separate compatibility policy.
    /// No job scheduling, Clean mode, runtime loading or automatic startup here.
    /// Callback refusal returns null before allocation or state mutation.
    [[nodiscard]] std::shared_ptr<const DiscoveryResult> discover(DiscoveryMode);

    /// Read-only owner hydration of stored metadata, independent of helper
    /// deployment. Stages all common/format validation before publication.
    /// Missing or validated v1/v2 registry on a fresh loader permits legacy discovery;
    /// only that later owner discovery migrates. Corrupt existing metadata
    /// fails closed for new external instances until a successful refresh/scan.
    /// A valid empty catalogue also disables implicit discovery. Publication
    /// replaces metadata only at this explicit boundary, not on every insert.
    /// Does not claim current filesystem freshness or runtime readiness.
    bool hydrateCatalogue(std::string& error);

    /// Owner configuration for the dedicated helper/shared quarantine. Empty
    /// paths use app defaults. Changing it invalidates the lazy discovery cache.
    void setScanConfig(const scan::Config& config) {
        if(audio::onAudioThread)return;
        scanConfig_=config;scanned_=false;known_->clear();clapCatalogue_.clear();
        hydrationAttempted_=false;catalogueHydrated_=false;hydrationError_.clear();
        publishedCatalogue_.reset();
    }
    /// Borrowed on the owner between jobs; copy/publish for another thread.
    [[nodiscard]] const scan::Report& lastScanReport() const noexcept { return scanReport_; }

    /// The session's `DeviceLoader`. Valid while this object lives.
    [[nodiscard]] engine::DeviceLoader fn();

    struct Stats {
        std::int64_t requests = 0;
        std::int64_t vst3 = 0;       ///< instances handed out
        std::int64_t clap = 0;
        std::int64_t builtin = 0;    ///< supported canonical built-ins
        std::int64_t unhosted = 0;   ///< a format we do not host
        std::int64_t notFound = 0;   ///< hosted format, no binary matched the uid
        std::int64_t failed = 0;     ///< matched, and did not instantiate
        std::int64_t scans = 0;
        std::int64_t catalogueReads = 0; ///< persistent metadata reads, never discovery
        std::int64_t needsDiscovery = 0; ///< cached snapshot misses; no implicit scan
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] Vst3Host& vst3() noexcept { return vst3_; }
    [[nodiscard]] ClapHost& clap() noexcept { return clap_; }

    /// One line per plugin the last scan found, both formats: for `--list`,
    /// and for the day a browser row needs the same thing.
    [[nodiscard]] std::vector<std::string> knownPlugins() const;

private:
    std::shared_ptr<const DiscoveryResult> discoverOwned(DiscoveryMode, bool allowLegacyFallback);
    std::unique_ptr<DeviceInstance> make(const engine::DeviceRequest& rq, std::string& err);
    std::unique_ptr<DeviceInstance> makeVst3(const engine::DeviceRequest& rq, std::string& err);
    std::unique_ptr<DeviceInstance> makeClap(const engine::DeviceRequest& rq, std::string& err);

    Vst3Host vst3_;
    ClapHost clap_;
    std::unique_ptr<juce::KnownPluginList> known_ = std::make_unique<juce::KnownPluginList>();
    std::vector<ClapPluginRef> clapCatalogue_;
    juce::FileSearchPath vst3Paths_;
    std::vector<std::string> clapPaths_;
    bool scanned_ = false;
    bool hydrationAttempted_ = false;
    bool catalogueHydrated_ = false;
    std::string hydrationError_;
    Stats stats_;
    scan::Config scanConfig_;
    scan::Report scanReport_;
    std::shared_ptr<const DiscoveryCatalogue> publishedCatalogue_;
};

}  // namespace adi::device
