// SPDX-License-Identifier: GPL-3.0-or-later
//
// The registry against docs/SETTINGS-CATALOGUE.md, the Settings Reference's
// 207 rows (ADR-0156). Every row is in exactly one of two lists:
//
//   answered   the row is a setting ADI has: the registry keys that answer it
//   absent     the row is deliberately not a setting, with the kind and reason
//
// A row is named "<section> / <setting>": the section as the catalogue heads
// it ("3. Audio"), and the setting cell up to its first " (". tests/
// test_settings.cpp reads the catalogue and proves the partition.

#pragma once

#include <string>
#include <vector>

namespace adi::settings {

struct AnsweredRow {
    std::string row;
    std::vector<std::string> keys;   // each in the registry
};

enum class Absence {
    Rejected,      // REJECTED in the catalogue
    Note,          // NOTE: listed so the user knows, nothing to set
    NotASetting,   // decided, but a behaviour, an action, a display or a fixed rule
    Backlog,       // arrives with its feature
    Wish,          // WISH: not planned; nothing to set until it is
};
[[nodiscard]] const char* toString(Absence);

struct AbsentRow {
    std::string row;
    Absence kind;
    std::string reason;
};

[[nodiscard]] const std::vector<AnsweredRow>& answeredRows();
[[nodiscard]] const std::vector<AbsentRow>& absentRows();

}  // namespace adi::settings
