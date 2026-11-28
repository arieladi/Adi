// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/render/queue.hpp"
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace adi { class Store; }
namespace adi::render {
inline constexpr const char* kSavedJobsKey = "export.savedJobs";
inline constexpr std::size_t kMaximumSavedJobs = 128;
inline constexpr std::size_t kMaximumSavedJobsBytes = 4 * 1024 * 1024;
struct SavedJobs {
    bool present = false; // absent differs from an explicitly saved empty list
    std::vector<Job> jobs;
};
// Owner thread, open file-backed Store with an absolute path. Layer 3 only:
// no journal, queue, render,
// project snapshot, ID relocation or file publication. Callback calls refuse
// without touching error. Otherwise error is cleared on entry.
// Load validates the entire versioned row, then binds ONLY Job.project to the
// current owning Store.path(). Save As/copies retain their project-local IDs.
// No partial result; an absent row is successful {false,{}}. Read-only load is OK.
[[nodiscard]] std::optional<SavedJobs> loadSavedJobs(const Store&, std::string& error);
// All incoming Job.project spellings must equal Store.path(). Replace the list
// atomically, including an explicit empty list. Existing invalid/unsupported
// data refuses overwrite. Requires writable Store and no enclosing transaction.
[[nodiscard]] bool saveSavedJobs(Store&, std::span<const Job>, std::string& error);
} // namespace adi::render
