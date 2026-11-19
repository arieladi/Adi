// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/plugin_scanner.hpp"

namespace adi::scan {

struct CatalogueLimits {
    std::size_t maximumModules = 65536;
    std::size_t maximumPlugins = kMaximumPlugins;
    std::size_t maximumTextBytes = 64U * 1024U * 1024U;
};

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
/// Absolute path required; missing/foreign/newer registries fail with error.
/// Limits: 1..65536 modules, 1..65536 total plugins, 1..64 MiB aggregate stored
/// text (including packet/path/stamp); packets also retain their 8 MiB limit.
std::optional<Catalogue> readCatalogue(const std::filesystem::path& registry,
    std::string& error, const CatalogueLimits& limits = {});

} // namespace adi::scan
