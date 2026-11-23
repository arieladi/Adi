// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CO-01: base-62 lexicographic fractional sibling keys (Rocicorp / Figcraft
// algorithm, PLAN C3). Keys compare directly with standard ASCII std::string
// `<`, refuse swapped or equal bounds, support balanced N-key subdivision,
// periodic respace, and longest-increasing-subsequence (LIS) key reuse.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adi::collab {

/// Return true if `key` is a canonical base-62 fractional index key.
[[nodiscard]] bool validateFractionalKey(std::string_view key) noexcept;

/// Generate a canonical fractional key strictly between `lower` and `upper`.
/// Returns nullopt if either bound is invalid, if `lower >= upper` (swapped or
/// equal bounds are refused), or if `upper` is the smallest representable key.
[[nodiscard]] std::optional<std::string> keyBetween(std::optional<std::string_view> lower,
                                                    std::optional<std::string_view> upper);

/// Generate `count` strictly ascending canonical keys between `lower` and `upper`.
/// Returns nullopt on invalid/swapped bounds.
[[nodiscard]] std::optional<std::vector<std::string>> nKeysBetween(
    std::optional<std::string_view> lower,
    std::optional<std::string_view> upper,
    std::size_t count);

/// Generate `count` evenly spaced short keys starting from the root ("a0", ...).
[[nodiscard]] std::vector<std::string> respaceKeys(std::size_t count);

/// Given the desired sibling order carrying each item's current key (empty
/// string for newly inserted items), keep the longest strictly increasing
/// subsequence of valid keys untouched and assign minimal fresh keys only to
/// moved or newly inserted entries.
[[nodiscard]] std::vector<std::string> reuseKeysForOrder(std::span<const std::string> currentKeys);

}  // namespace adi::collab
