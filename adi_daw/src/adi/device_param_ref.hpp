// SPDX-License-Identifier: GPL-3.0-or-later
//
// Canonical parsing for device automation `param_ref` values (SPEC §6.3.3,
// ADR-0165). A device lane names its parameter by the plug-in's numeric id
// in canonical unsigned decimal: digits only, no sign, no leading zero ("0"
// allowed), at most 4294967295. Readers still accept numeric non-canonical
// spellings such as "001" from older or third-party files and map them to
// their numeric value.

#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace adi {

/// Parse `text` as a decimal uint32 device parameter id. Returns true for any
/// non-empty ASCII-digit string whose numeric value fits in uint32_t (including
/// legacy leading zeros such as "001"); sets `*canonical` to true iff `text` is
/// in canonical form ("0" or [1-9][0-9]*).
[[nodiscard]] inline bool parseDeviceParamRef(std::string_view text, std::uint32_t& id,
                                              bool* canonical = nullptr) noexcept {
    if (canonical != nullptr) *canonical = false;
    if (text.empty()) return false;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(ch - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) return false;
    }
    id = static_cast<std::uint32_t>(value);
    if (canonical != nullptr) *canonical = text.size() == 1 || text[0] != '0';
    return true;
}

/// True iff `text` is a canonical unsigned decimal uint32 device parameter id.
[[nodiscard]] inline bool isCanonicalDeviceParamRef(std::string_view text) noexcept {
    std::uint32_t id = 0;
    bool canonical = false;
    return parseDeviceParamRef(text, id, &canonical) && canonical;
}

}  // namespace adi
