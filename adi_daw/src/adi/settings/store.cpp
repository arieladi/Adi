// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/store.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>

namespace adi::settings {
namespace {

const char* actorName(Actor a) { return a == Actor::Agent ? "agent" : "user"; }

bool readText(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// settings.json.corrupt-1, -2, ...: the first name not already taken.
std::filesystem::path asideName(const std::filesystem::path& file) {
    std::error_code ec;
    for (int n = 1;; ++n) {
        auto candidate = file;
        candidate += ".corrupt-" + std::to_string(n);
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
}

}  // namespace

const char* toString(LoadStatus s) {
    switch (s) {
        case LoadStatus::Defaults:    return "defaults";
        case LoadStatus::Loaded:      return "loaded";
        case LoadStatus::CorruptKept: return "corrupt, kept aside";
        case LoadStatus::ForeignApp:  return "another application's file";
        case LoadStatus::NewerSchema: return "written by a newer build";
    }
    return "?";
}

AppSettings AppSettings::forApp(appdata::App app) {
    const auto dir = appdata::pathsFor(app).config;
    return AppSettings(app, dir.empty() ? std::filesystem::path{} : dir / "settings.json");
}

AppSettings::AppSettings(appdata::App app, std::filesystem::path file)
    : app_(app), file_(std::move(file)) {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::exists(file_, ec)) return;   // Defaults

    std::string text;
    Value doc = Value::parse(readText(file_, text) ? text : std::string{}, nullptr,
                             /*allow_exceptions=*/false);
    const bool shaped = !doc.is_discarded() && doc.is_object() && doc.contains("values") &&
                        doc.at("values").is_object() &&
                        (!doc.contains("schema") || doc.at("schema").is_number_integer());
    if (!shaped) {
        // Never overwrite what we could not read: it may be a newer build's
        // format, or a user's hand edit one comma short of valid.
        const auto aside = asideName(file_);
        std::filesystem::rename(file_, aside, ec);
        if (!ec) aside_ = aside;
        status_ = LoadStatus::CorruptKept;
        return;
    }
    if (doc.contains("app") && doc.at("app") != appdata::displayName(app_)) {
        // ADR-0145 d10: another application's settings are not ours to read,
        // and not ours to overwrite either.
        status_ = LoadStatus::ForeignApp;
        return;
    }
    values_ = doc.at("values");
    schema_ = doc.value("schema", kSettingsSchema);
    status_ = schema_ > kSettingsSchema ? LoadStatus::NewerSchema : LoadStatus::Loaded;
}

Value AppSettings::get(const std::string& key) const {
    const Setting* s = find(key);
    if (!s) return nullptr;
    if (s->scope == Scope::App) {
        const auto it = values_.find(key);
        std::string why;
        if (it != values_.end() && validate(*s, *it, why)) return *it;
    }
    return s->defaultValue;
}

bool AppSettings::write(const std::string& key, const Value& value, const ChangeContext& who,
                        std::string& why) {
    const Setting* s = find(key);
    if (!s) { why = "no setting named '" + key + "'"; return false; }
    if (s->scope == Scope::Project) {
        why = key + " belongs to the project; change it with the op " + s->op;
        return false;
    }
    if (s->scope == Scope::Device) { why = key + " belongs to a device"; return false; }
    if (!validate(*s, value, why)) return false;
    const Value before = get(key);
    values_[key] = value;
    if (before != value) log(key, before, value, who);
    return true;
}

bool AppSettings::set(const std::string& key, const Value& value, const ChangeContext& who,
                      std::string& why) {
    return write(key, value, who, why);
}

bool AppSettings::reset(const std::string& key, const ChangeContext& who, std::string& why) {
    const Setting* s = find(key);
    if (!s) { why = "no setting named '" + key + "'"; return false; }
    return write(key, s->defaultValue, who, why);
}

void AppSettings::resetPage(const std::string& page, const ChangeContext& who) {
    for (const auto& s : registry()) {
        if (s.page != page || s.scope != Scope::App) continue;
        std::string ignored;
        write(s.key, s.defaultValue, who, ignored);
    }
}

bool AppSettings::save(std::string& why) const {
    if (file_.empty()) { why = "this machine gives no settings folder"; return false; }
    if (status_ == LoadStatus::ForeignApp) {
        why = file_.string() + " belongs to another application and is not overwritten";
        return false;
    }
    Value doc = Value::object();
    doc["schema"] = std::max(schema_, kSettingsSchema);   // never downgrade a newer file
    doc["app"] = appdata::displayName(app_);
    doc["values"] = values_;                              // unknown keys ride along

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    auto tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { why = "cannot write " + tmp.string(); return false; }
        out << doc.dump(2) << '\n';
        if (!out) { why = "writing " + tmp.string() + " failed"; return false; }
    }
    std::filesystem::rename(tmp, file_, ec);
    if (ec) {
        why = "cannot replace " + file_.string() + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::filesystem::path AppSettings::changeLogFile() const {
    return file_.empty() ? std::filesystem::path{} : file_.parent_path() / "settings-changes.jsonl";
}

void AppSettings::log(const std::string& key, const Value& before, const Value& after,
                      const ChangeContext& who) const {
    if (who.actor == Actor::User && !logUsers_) return;
    const auto path = changeLogFile();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    Value line = {{"time", who.timeUtc}, {"actor", actorName(who.actor)}, {"key", key},
                  {"before", before},    {"after", after}};
    if (!who.detail.empty()) line["detail"] = who.detail;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << line.dump() << '\n';
}

// --- the agent's pipeline ----------------------------------------------------------------

bool agentForbidden(const Setting& s) {
    // ADR-0125 d2's "never" list, as structure rather than as marks: a path,
    // anything on the audio page (device, driver, rate, block size, I/O, the
    // virtual device), plug-in folders and scanning, privacy, the agent's
    // own settings, and anything that is not an App setting at all.
    return s.scope != Scope::App || s.type == Type::Path || s.type == Type::PathList ||
           s.page == "Audio" || s.page == "Plug-ins" || s.page == "Privacy" || s.page == "AI";
}

AgentResult agentSet(AppSettings& store, AgentTier tier, const std::string& key, const Value& value,
                     const std::string& detail, std::int64_t timeUtc) {
    AgentResult r;
    if (tier != AgentTier::Apply) {
        r.why = "the agent changes settings at Apply tier only (ADR-0125 d1)";
        return r;
    }
    const Setting* s = find(key);
    if (!s) { r.why = "no setting named '" + key + "'"; return r; }
    if (!s->agentMayChange || agentForbidden(*s)) {
        r.why = key + " is not on the agent's whitelist (ADR-0125 d2)";
        return r;
    }
    r.ok = store.set(key, value, ChangeContext{Actor::Agent, detail, timeUtc}, r.why);
    return r;
}

// --- presets --------------------------------------------------------------------------------

Preset makePreset(const AppSettings& store, std::string name, std::vector<std::string> pages) {
    Preset p;
    p.name = std::move(name);
    p.pages = std::move(pages);
    for (const auto& s : registry()) {
        if (s.scope != Scope::App) continue;
        if (std::find(p.pages.begin(), p.pages.end(), s.page) == p.pages.end()) continue;
        p.values[s.key] = store.get(s.key);
    }
    return p;
}

PresetResult applyPreset(AppSettings& store, const Preset& p, const ChangeContext& who) {
    PresetResult r;
    for (const auto& [key, value] : p.values.items()) {
        const Setting* s = find(key);
        // A partial preset changes ITS pages and nothing else: a key from a
        // page it does not hold is skipped, even if the file carries it.
        if (!s || std::find(p.pages.begin(), p.pages.end(), s->page) == p.pages.end()) {
            r.skipped.push_back(key);
            continue;
        }
        std::string why;
        if (store.set(key, value, who, why)) ++r.applied;
        else r.skipped.push_back(key);
    }
    return r;
}

Value toJson(const Preset& p) {
    return {{"schema", kSettingsSchema}, {"name", p.name}, {"pages", p.pages}, {"values", p.values}};
}

std::optional<Preset> presetFromJson(const Value& j, std::string& why) {
    try {
        Preset p;
        p.name = j.at("name").get<std::string>();
        p.pages = j.at("pages").get<std::vector<std::string>>();
        p.values = j.at("values");
        if (!p.values.is_object()) { why = "a preset's values are an object"; return std::nullopt; }
        return p;
    } catch (const std::exception& e) {
        why = e.what();
        return std::nullopt;
    }
}

}  // namespace adi::settings
