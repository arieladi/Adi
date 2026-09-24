// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings registry: every setting the Settings window shows, typed.
// ADR-0125 (R-01 to R-07), ADR-0145, ADR-0152. docs/SETTINGS.md is the prose.
//
// One table, read by every page, the Find box (R-02), presets (R-04), bundles
// (R-05) and the agent's pipeline (R-07). A setting that is not here does not
// exist: the store refuses an unknown key from a caller, while keeping one a
// newer build wrote.
//
// Scope (R-03) decides who owns the value:
//   App      this application's settings.json (ADR-0149); the store writes it.
//   Project  a row in the .adi, changed only by an op. The registry names the
//            op and the store refuses to write it -- the file is not ours to
//            touch outside the op log (ADR-0003).
//   Device   a device's own state; listed for the badge, stored by the device.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace adi::settings {

using Value = nlohmann::json;

enum class Type { Bool, Int, Real, Text, Choice, Path, PathList };
enum class Scope { App, Project, Device };

[[nodiscard]] const char* toString(Type);
[[nodiscard]] const char* toString(Scope);

struct Setting {
    std::string key;        // "audio.bufferSize" -- permanent, like an op name
    Type type = Type::Bool;
    Scope scope = Scope::App;
    std::string page;       // "Audio", "Look & Feel": the left-hand list (R-01)
    std::string label;      // what the page shows
    std::string help;       // what the Find box searches with the label (R-02)
    Value defaultValue;
    std::vector<Value> choices;   // Type::Choice: the allowed values, in order
    double min = 0.0;             // Int and Real: inclusive range, when max > min
    double max = 0.0;
    std::string op;         // Project scope: the op that sets it (OPS.md 9)
    /// ADR-0125 d2: on the agent's whitelist. Off unless someone set it on
    /// purpose -- a new setting is never agent-writable by default.
    bool agentMayChange = false;
};

/// Every setting, in page order and then display order.
[[nodiscard]] const std::vector<Setting>& registry();
[[nodiscard]] const Setting* find(std::string_view key);

/// The pages, in the order the Settings window lists them (R-01).
[[nodiscard]] std::vector<std::string> pages();

/// R-02: every setting whose label, help text or key contains each word of
/// `query`, case-insensitively (ASCII folding; the labels are English today).
[[nodiscard]] std::vector<const Setting*> search(std::string_view query);

/// Is `v` a legal value for `s`? `why` says what is wrong when it is not.
[[nodiscard]] bool validate(const Setting& s, const Value& v, std::string& why);

}  // namespace adi::settings
