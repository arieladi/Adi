// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "adi/appdata.hpp"
#include "adi/settings/registry.hpp"
#include <span>
#include <string>

namespace adi::settings {

// A step edits values only. It must be pure: no I/O or external side effects.
// The runner owns schema advancement and preserves the document's other fields.
struct MigrationStep {
    int from = 0;
    int to = 0;
    bool (*apply)(Value& values, std::string& why) = nullptr;
};

enum class MigrationStatus {
    Current, Migrated, NewerSchema, ForeignApp, InvalidDocument,
    InvalidPlan, MissingStep, StepFailed
};

struct MigrationResult {
    MigrationStatus status = MigrationStatus::InvalidDocument;
    Value document; // Populated only for Current/Migrated/NewerSchema.
    std::string why;
    [[nodiscard]] bool ok() const noexcept {
        return status == MigrationStatus::Current || status == MigrationStatus::Migrated;
    }
};

// No file is read or written. On failure the input remains unchanged and no
// partially migrated document is returned. A schema-less legacy file is schema
// 1, the first published settings format. Adjacent steps must be unambiguous;
// the complete route is checked before any callback is invoked.
[[nodiscard]] MigrationResult migrateSettingsDocument(const Value& input, appdata::App app,
    int targetSchema, std::span<const MigrationStep> steps);

// Schema 1 is current; no historical transformation is invented. Future schema
// changes add explicit adjacent steps here, with fixtures for their own policy.
[[nodiscard]] std::span<const MigrationStep> settingsMigrationSteps() noexcept;

} // namespace adi::settings
