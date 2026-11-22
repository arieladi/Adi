// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/plugin_scanner.hpp"

namespace adi::scan {

struct CatalogueLimits {
    std::size_t maximumModules = 65536;
    std::size_t maximumPlugins = kMaximumPlugins;
    std::size_t maximumTextBytes = 64U * 1024U * 1024U;
};

/// A null snapshot is not by itself permission to discover. Only Missing and
/// validated Legacy permit an owner's documented pre-cache discovery policy;
/// Invalid (including foreign/future/corrupt stores) must fail closed.
enum class CatalogueReadStatus { Invalid, Missing, Legacy, Ready };

struct CatalogueModule {
    std::string format, canonicalPath, stamp;
    std::int64_t updatedUtc = 0;
    std::vector<Plugin> plugins;
};

/// Stored metadata at one database snapshot, NOT a Ready device or a current
/// filesystem/build validation. No candidate is statted, loaded or executed.
/// Common packet/identity validation is complete. VST3 Plugin.data XML MUST
/// still pass the host's format validator before catalogue installation; then
/// rebind descriptions to an original absolute saved hint for JUCE identifiers.
struct Catalogue {
    std::vector<CatalogueModule> modules; // sorted by (format, canonicalPath)
    std::size_t pluginCount = 0;
    /// Lookup by an already canonical key; no filesystem work/alias guessing.
    /// Borrowed until this snapshot is mutated/destroyed, no live SQL lookup.
    [[nodiscard]] const CatalogueModule* find(std::string_view format,
        std::string_view canonicalPath) const noexcept;
};

/// OWNER-only, read-only SQLite connection + one read transaction. Does not
/// create/migrate a registry, open a Scanner/helper, insert Pending, or walk
/// search roots. Reads success-only v3 as well as v3 with typed failures.
/// Pending/manual/automatic blocks and every typed failure exclude a positive
/// row at this snapshot. A later block requires a new snapshot/use-time policy.
/// Empty legacy packets are negative, so excluded. Corruption or exhausted
/// caller bounds refuses the WHOLE snapshot, never a successful prefix.
/// Absolute path required. A missing file or structurally validated v1/v2 ADI
/// schema returns null with its typed status; neither creates/migrates a file.
/// Foreign/newer/malformed registries return Invalid. Callback refusal leaves
/// both diagnostics and optional status untouched.
/// Limits: 1..65536 modules, 1..65536 total plugins, 1..64 MiB aggregate stored
/// text (including packet/path/stamp); packets also retain their 8 MiB limit.
std::optional<Catalogue> readCatalogue(const std::filesystem::path& registry,
    std::string& error, const CatalogueLimits& limits = {},
    CatalogueReadStatus* status = nullptr);

} // namespace adi::scan
