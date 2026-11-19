// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/scan_process.hpp"
#include "adi/plugin_registry.hpp"
#include <memory>
#include <optional>
#include <string_view>

namespace adi::scan {
struct Plugin {
    std::string id, name, vendor, version, features, data;
    bool instrument = false;
};
inline constexpr std::size_t kMaximumPlugins = 65536;
inline constexpr std::size_t kMaximumPacket = 8U * 1024U * 1024U;
inline constexpr unsigned kPacketSchema = 1;

/// Owner protocol APIs: strict schema 1, exact format/path, no duplicate keys,
/// ids or unknown fields, bounded tokens/depth/string bytes. No partial result.
std::optional<std::vector<Plugin>> decodeMetadata(std::string_view packet,
    const std::string& format, const std::string& path, std::string& error);
std::optional<std::string> encodeMetadata(const std::string& format,
    const std::string& path, const std::vector<Plugin>& plugins, std::string& error);

std::filesystem::path defaultScannerExecutable(); // sibling of current executable
/// Enumerate a plug-in format without loading its modules. Returns sorted,
/// canonical UTF-8 paths, and follows native links without repeating folders.
std::vector<std::string> findCandidates(const std::vector<std::string>& roots,
                                        std::string_view extension);
std::int64_t systemUtcMicros() noexcept;
struct Config {
    std::filesystem::path executable; // empty => sibling adi_plugin_scanner[.exe]
    std::filesystem::path registry;   // empty => appdata::pluginRegistryFile()
    ProcessOptions process;
    std::int64_t (*clock)() noexcept = systemUtcMicros;
};
enum class State { Ready, Quarantined, Unavailable, Cancelled, Failed, Invalid, CallbackRefused };
struct Result {
    State state = State::Invalid;
    bool cacheHit = false; // validated success packet; does not mean instantiated
    bool failureCacheHit = false; // no child; matching deterministic/two-strike failure
    unsigned failureStrikes = 0; // consecutive stable-stamp crashes/timeouts, capped at two
    std::string cacheWarning; // no reliable bounded stamp: fresh child result is not cached
    std::string path, error; // canonical request path; error is an owned diagnostic
    std::vector<Plugin> plugins;
};
struct Failure { std::string path, error; State state = State::Failed; };
struct Report {
    bool complete = true, cancelled = false;
    std::vector<Failure> failures;
};

/// Optional format-specific owner validation BEFORE success removes quarantine.
/// VST3 requires this to validate its serialized PluginDescriptions. Context is
/// borrowed through probe; discard any staged values unless Result is Ready.
using Validator = bool(*)(const std::vector<Plugin>&, const std::string&,
                          void*, std::string&);
class Scanner {
public:
    static std::unique_ptr<Scanner> open(const Config&, std::string& error);
    /// Canonicalize/stamp an existing file/bundle, commit pending, validate a
    /// matching cached packet or launch one helper, then token-check completion.
    /// The stamp is checked again after validation. Never loads a
    /// plug-in in this process, and never falls back in process. Owner only.
    Result probe(const std::string& format, const std::filesystem::path& path,
                 Validator validate = nullptr, void* context = nullptr);
    /// Explicit owner retry/re-enable, resolving existing aliases exactly as
    /// probe does. Clears manual/legacy/automatic blocks and both cache kinds.
    /// Missing paths refuse; callers holding a stored canonical missing key may
    /// use PluginRegistry::unblockScan directly. Never runs a child itself.
    bool retry(const std::string& format, const std::filesystem::path& path, std::string& error);
private:
    Scanner(Config config, std::unique_ptr<PluginRegistry> registry, std::string buildHash,
            std::uintmax_t helperSize, std::filesystem::file_time_type helperTime);
    Config config_;
    std::unique_ptr<PluginRegistry> registry_;
    std::string helperBuildHash_;
    std::uintmax_t helperSize_ = 0;
    std::filesystem::file_time_type helperTime_;
};
} // namespace adi::scan
