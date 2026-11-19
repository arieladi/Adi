// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/render/queue.hpp"
#include "adi/media/publish_file.hpp"
#include <span>

namespace adi::render {
inline constexpr std::size_t kMaximumUserPresets = 128;
inline constexpr std::size_t kMaximumUserPresetsBytes = 4 * 1024 * 1024;
inline constexpr std::size_t kMaximumUserPresetNameBytes = 1024;
struct UserPreset { std::string name; Job job; };
struct UserPresetList { bool present = false; std::vector<UserPreset> presets; };
// Fixed ADI-specific key under the suite data root; names never become paths.
// Returns empty when appdata cannot locate a data folder. Does not create it.
[[nodiscard]] std::filesystem::path userPresetsFile();
class UserPresets {
public:
    // Explicit paths support tests/portable callers. Resolve relative file
    // paths once, at construction. Job paths inside presets remain literal data.
    explicit UserPresets(std::filesystem::path file = userPresetsFile());
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }
    // Owner-thread calls only. Absent is successful {false,{}}; malformed,
    // unsupported and unreadable files refuse completely and are never moved.
    [[nodiscard]] std::optional<UserPresetList> load(std::string& error) const;
    // Explicit WHOLE-list replacement, not a merge or stale-snapshot CAS.
    // Validates current disk bytes under the publication lease. No enqueue,
    // rendering, mask application or project rebinding. No copy fallback.
    // Hooks are diagnostic only; validateTarget is always our strict guard.
    // Published with warning is success; inspect durabilityConfirmed separately.
    [[nodiscard]] media::PublishResult save(std::span<const UserPreset>, media::ReplaceHooks = {}) const;
private:
    std::filesystem::path file_;
};
} // namespace adi::render
