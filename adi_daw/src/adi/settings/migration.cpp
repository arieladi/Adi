// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/settings/migration.hpp"

#include <cstdint>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace adi::settings {

std::span<const MigrationStep> settingsMigrationSteps() noexcept { return {}; }

MigrationResult migrateSettingsDocument(const Value& input, appdata::App app,
    int targetSchema, std::span<const MigrationStep> steps) {
    const auto fail=[](MigrationStatus status, std::string why) {
        return MigrationResult{status, {}, std::move(why)};
    };
    if (!input.is_object())
        return fail(MigrationStatus::InvalidDocument, "the settings document is not an object");
    if (input.contains("app") && input.at("app") != appdata::displayName(app))
        return fail(MigrationStatus::ForeignApp, "the settings destination belongs to another application");
    if (targetSchema < 1)
        return fail(MigrationStatus::InvalidPlan, "the target settings schema must be positive");
    std::uint64_t source = 1; // Schema-less files predate an explicit schema field.
    if (const auto schema = input.find("schema"); schema != input.end()) {
        if (!schema->is_number_integer() ||
            (!schema->is_number_unsigned() && schema->get<std::int64_t>() < 0))
            return fail(MigrationStatus::InvalidDocument, "the settings schema must be a non-negative integer");
        source = schema->get<std::uint64_t>();
    }
    // A newer writer may have changed the rest of the shape. Leave it untouched.
    if (source > static_cast<std::uint64_t>(targetSchema))
        return {MigrationStatus::NewerSchema, input, "settings from a newer build are read-only"};
    if (!input.contains("values") || !input.at("values").is_object())
        return fail(MigrationStatus::InvalidDocument, "the settings document has no values object");
    // This is a trusted, compiled table, but reject a broken registration rather
    // than choosing one of two possible transformations or allowing a cycle.
    constexpr std::size_t maximumSteps = 256;
    if (steps.size() > maximumSteps)
        return fail(MigrationStatus::InvalidPlan, "the settings migration table exceeds its bound");
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        if (step.from < 0 || step.from == std::numeric_limits<int>::max() ||
            step.to != step.from + 1 || step.apply == nullptr)
            return fail(MigrationStatus::InvalidPlan, "settings migrations must be callable adjacent schema steps");
        for (std::size_t j = 0; j < i; ++j)
            if (steps[j].from == step.from)
                return fail(MigrationStatus::InvalidPlan, "ambiguous settings migration from schema " + std::to_string(step.from));
    }
    std::vector<const MigrationStep*> route;
    for (int version = static_cast<int>(source); version < targetSchema; ++version) {
        const MigrationStep* next = nullptr;
        for (const auto& step : steps)
            if (step.from == version) { next = &step; break; }
        if (!next)
            return fail(MigrationStatus::MissingStep, "no settings migration from schema " + std::to_string(version) +
                " to " + std::to_string(version + 1));
        route.push_back(next);
    }
    Value migrated = input;
    for (const auto* step : route) {
        std::string why;
        try {
            if (!step->apply(migrated.at("values"), why))
                return fail(MigrationStatus::StepFailed, "settings migration from schema " + std::to_string(step->from) +
                    " failed" + (why.empty() ? std::string{} : ": " + why));
            if (!migrated.at("values").is_object())
                return fail(MigrationStatus::StepFailed, "settings migration produced a non-object values field");
        } catch (const std::exception& error) {
            return fail(MigrationStatus::StepFailed, "settings migration threw: " + std::string(error.what()));
        } catch (...) {
            return fail(MigrationStatus::StepFailed, "settings migration threw an unknown exception");
        }
        migrated["schema"] = step->to;
    }
    return {route.empty() ? MigrationStatus::Current : MigrationStatus::Migrated, std::move(migrated), {}};
}

} // namespace adi::settings
