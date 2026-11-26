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
//     An ordinary open can probe one installed saved hint after a catalogue miss.
//     Explicit CacheOnly prohibits that probe. Legacy root discovery is retained
//     for a fresh loader with no registry or a validated v1/v2 schema.
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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace adi::device {

/// Future discovery inputs, separate from the last published catalogue.
struct PluginSearchPaths {
    juce::FileSearchPath vst3;
    std::vector<std::string> clap;
};

enum class DiscoveryMode { Incremental, CacheOnly, Refresh };
enum class DiscoveryOutcome { Published, Missing, Legacy, Failed, Cancelled, Stale };

enum class DiscoveryPhase { Idle, Queued, ReadingCache, Vst3, Clap, Reconciling, Ready };
struct DiscoveryProgress {
    std::uint64_t job = 0;
    DiscoveryPhase phase = DiscoveryPhase::Idle;
    bool cancellationRequested = false;
    bool hasCandidateTotal = false;
    std::size_t completed = 0;
    std::size_t total = 0;
};

enum class DiscoveryStart { Started, Busy, Invalid, Unavailable, CallbackRefused };
struct DiscoveryAdmission {
    DiscoveryStart status = DiscoveryStart::Invalid;
    std::uint64_t job = 0;
    std::string error;
};

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
    ~JuceDeviceLoader();

    /// Format conventions only; no directory enumeration or module loading.
    [[nodiscard]] PluginSearchPaths defaultSearchPaths() const;
    /// Owner only. Replaces future roots without scanning or discarding the
    /// accepted catalogue/instances. An admitted discovery keeps its own roots.
    /// Callback calls are refused before allocation; identical input is a no-op.
    void setSearchPaths(const PluginSearchPaths&);

    /// A directory searched by explicit discovery, ahead of the defaults.
    /// Adding a root does not invalidate or bypass a hydrated snapshot.
    void addSearchPath(const juce::File& dir);

    /// Explicit owner scan. Only a fresh loader with no cache-capable registry
    /// retains legacy lazy root discovery for a miss without a usable hint.
    void scan();

    /// Synchronous owner-side operation. Incremental enumerates configured roots
    /// and reuses the isolated scanner/cache. Refresh enumerates the same roots
    /// with fresh probes for cached successes/rejections, preserving blocks.
    /// These explicit modes choose their cache policy over Config::cacheMode.
    /// CacheOnly reads stored metadata
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
    /// Ordinary opens can later probe one installed saved hint on a miss.
    /// Hydration itself never checks installed plug-ins or launches the helper.
    /// Does not claim current filesystem freshness or runtime readiness.
    bool hydrateCatalogue(std::string& error);

    /// Owner configuration for the dedicated helper/shared quarantine. Empty
    /// paths use app defaults. A change invalidates admission and metadata.
    void setScanConfig(const scan::Config&);

    /// Use one owner worker for read-only startup hydration, Incremental
    /// discovery or explicit Refresh. A zero worker override uses half the
    /// reported hardware threads, bounded to 1..4. The job owns cancellation
    /// through cancelDiscovery.
    /// After this API admits a job, runtime misses report NeedsDiscovery until
    /// the owner supplies catalogue metadata. Existing hits stay available.
    [[nodiscard]] DiscoveryAdmission startDiscovery(DiscoveryMode, unsigned workers = 0);
    [[nodiscard]] DiscoveryProgress discoveryProgress() const;
    bool cancelDiscovery(std::uint64_t job) noexcept;

    /// Nonblocking. Returns a result only when the matching job has finished.
    /// Successful results replace both format catalogues after staging. A job
    /// invalidated by a settings change reports Stale and keeps the current catalogue.
    [[nodiscard]] std::shared_ptr<const DiscoveryResult> adoptDiscovery(std::uint64_t job);
    [[nodiscard]] std::shared_ptr<const DiscoveryCatalogue> catalogue() const noexcept;

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
        std::int64_t scanBlocksLifted = 0; ///< blocked plug-ins a Rescan tried again (SC-319)
        std::int64_t needsDiscovery = 0; ///< unresolved catalogue misses requiring discovery
        std::int64_t moved = 0;   ///< VST3s found by name and class ID at another path (SC-324)
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] Vst3Host& vst3() noexcept { return vst3_; }
    [[nodiscard]] ClapHost& clap() noexcept { return clap_; }

    /// One line per plugin the last scan found, both formats: for `--list`,
    /// and for the day a browser row needs the same thing.
    [[nodiscard]] std::vector<std::string> knownPlugins() const;

private:
    struct DiscoveryJob;
    std::shared_ptr<const DiscoveryResult> discoverOwned(DiscoveryMode, bool allowLegacyFallback,
        DiscoveryJob* job = nullptr);
    void invalidateDiscovery() noexcept;
    std::unique_ptr<DeviceInstance> make(const engine::DeviceRequest& rq, std::string& err);
    std::unique_ptr<DeviceInstance> makeVst3(const engine::DeviceRequest& rq, std::string& err);
    std::unique_ptr<DeviceInstance> makeClap(const engine::DeviceRequest& rq, std::string& err);

    Vst3Host vst3_;
    ClapHost clap_;
    std::unique_ptr<juce::KnownPluginList> known_ = std::make_unique<juce::KnownPluginList>();
    std::vector<ClapPluginRef> clapCatalogue_;
    std::shared_ptr<const PluginSearchPaths> searchPaths_ = std::make_shared<PluginSearchPaths>();
    bool scanned_ = false;
    bool hydrationAttempted_ = false;
    bool cacheOnly_ = false;
    std::string hydrationError_;
    Stats stats_;
    scan::Config scanConfig_;
    scan::Report scanReport_;
    std::shared_ptr<const DiscoveryCatalogue> publishedCatalogue_;
    std::uint64_t discoveryGeneration_ = 0;
    std::uint64_t nextDiscoveryJob_ = 1;
    bool managedDiscovery_ = false;
    std::unique_ptr<DiscoveryJob> discoveryJob_;
};

}  // namespace adi::device
