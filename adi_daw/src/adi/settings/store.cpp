// SPDX-License-Identifier: GPL-3.0-or-later

#include "adi/settings/store.hpp"
#include "adi/media/publish_file.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace adi::settings {
namespace {

const char* actorName(Actor a) { return a == Actor::Agent ? "agent" : "user"; }

/// The switch for a person's changes in the log (A6-20, ADR-0203).
constexpr const char* kLogChangesKey = "settings.logChanges";

/// The table of interface zooms, per window and per display (N4).
constexpr const char* kWindowZoomKey = "window.zoom";
constexpr const char* kNewerSchemaReadOnly = "settings from a newer build are read-only";

bool newerSchema(const Value& doc) {
    const auto version = doc.find("schema");
    if (version == doc.end() || !version->is_number_integer()) return false;
    if (version->is_number_unsigned())
        return version->get<std::uint64_t>() > static_cast<std::uint64_t>(kSettingsSchema);
    return version->get<std::int64_t>() > kSettingsSchema;
}

bool readText(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// Each save owns its staging name even when two instances write side by side.
class SettingsStage {
public:
    explicit SettingsStage(const std::filesystem::path& parent) {
        static std::atomic<std::uint64_t> sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto folder = parent / (".adi-settings-" + std::to_string(tick) + "-" +
                                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            auto file = folder / "settings.json";
            if (std::filesystem::create_directory(folder)) {
                folder_ = std::move(folder);
                file_ = std::move(file);
                return;
            }
        }
        throw std::runtime_error("cannot reserve a private settings stage");
    }
    ~SettingsStage() {
        std::error_code ignored;
        std::filesystem::remove(file_, ignored);
        std::filesystem::remove(folder_, ignored);
    }
    SettingsStage(const SettingsStage&) = delete;
    SettingsStage& operator=(const SettingsStage&) = delete;
    const std::filesystem::path& file() const { return file_; }
private:
    std::filesystem::path folder_, file_;
};

std::optional<std::string> readSettingsSnapshot(const std::filesystem::path& file) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(file, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found)
        return std::nullopt;
    if (error || !std::filesystem::is_regular_file(status))
        throw std::runtime_error("cannot inspect the settings destination");
    std::string text;
    if (!readText(file, text)) throw std::runtime_error("cannot read the settings destination");
    return text;
}

Value readSettingsDocument(const std::optional<std::string>& snapshot, appdata::App app) {
    if (!snapshot) return {{"values", Value::object()}};
    auto doc = Value::parse(*snapshot, nullptr, false);
    if (doc.is_object() && newerSchema(doc)) throw std::runtime_error(kNewerSchemaReadOnly);
    if (!doc.is_object() || !doc.contains("values") || !doc.at("values").is_object() ||
        (doc.contains("schema") && !doc.at("schema").is_number_integer()))
        throw std::runtime_error("the settings destination is incomplete or invalid");
    if (doc.contains("app") && doc.at("app") != appdata::displayName(app))
        throw std::runtime_error("the settings destination belongs to another application");
    return doc;
}

void writeSettingsStage(const SettingsStage& stage, std::string_view bytes) {
    std::ofstream out(stage.file(), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write the settings stage");
    out << bytes;
    out.close();
    if (!out) throw std::runtime_error("cannot finish the settings stage");
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
    const bool object = !doc.is_discarded() && doc.is_object();
    if (object && doc.contains("app") && doc.at("app") != appdata::displayName(app_)) {
        status_ = LoadStatus::ForeignApp;
        return;
    }
    const bool hasValues = object && doc.contains("values") && doc.at("values").is_object();
    if (object && newerSchema(doc)) {
        // Read familiar values when available. The rest of a newer document
        // may have a different shape and belongs to the writing version.
        if (hasValues) values_ = doc.at("values");
        status_ = LoadStatus::NewerSchema;
        return;
    }
    const bool shaped = hasValues &&
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
    values_ = doc.at("values");
    status_ = LoadStatus::Loaded;
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
                        std::string& why, bool removeOverride) {
    if (status_ == LoadStatus::NewerSchema) { why = kNewerSchemaReadOnly; return false; }
    if (status_ == LoadStatus::ForeignApp) { why = "settings belong to another application"; return false; }
    const Setting* s = find(key);
    if (!s) { why = "no setting named '" + key + "'"; return false; }
    if (s->scope == Scope::Project) {
        why = key + " belongs to the project; change it with the op " + s->op;
        return false;
    }
    if (s->scope == Scope::Device) { why = key + " belongs to a device"; return false; }
    if (!validate(*s, value, why)) return false;
    const Value before = get(key);
    // Read before the write, so the change that turns the log off is the log's
    // last line and explains its own silence (A6-20, ADR-0203).
    const bool logged = who.actor != Actor::User || get(kLogChangesKey) != false;
    pendingKeys_.insert(key);
    if (removeOverride) values_.erase(key);
    else values_[key] = value;
    if (logged && before != value) log(key, before, value, who);
    return true;
}

bool AppSettings::set(const std::string& key, const Value& value, const ChangeContext& who,
                      std::string& why) {
    return write(key, value, who, why);
}

bool AppSettings::reset(const std::string& key, const ChangeContext& who, std::string& why) {
    const Setting* s = find(key);
    if (!s) { why = "no setting named '" + key + "'"; return false; }
    return write(key, s->defaultValue, who, why, true);
}

void AppSettings::resetPage(const std::string& page, const ChangeContext& who) {
    for (const auto& s : registry()) {
        if (s.page != page || s.scope != Scope::App) continue;
        std::string ignored;
        write(s.key, s.defaultValue, who, ignored, true);
    }
}

bool AppSettings::save(std::string& why) {
    if (file_.empty()) { why = "this machine gives no settings folder"; return false; }
    if (status_ == LoadStatus::NewerSchema) { why = kNewerSchemaReadOnly; return false; }
    if (status_ == LoadStatus::ForeignApp) {
        why = file_.string() + " belongs to another application and is not overwritten";
        return false;
    }
    try {
        const auto destination = std::filesystem::absolute(file_);
        std::filesystem::create_directories(destination.parent_path());
        const auto previous = readSettingsSnapshot(destination);
        auto doc = readSettingsDocument(previous, app_);
        doc["schema"] = kSettingsSchema;
        doc["app"] = appdata::displayName(app_);
        auto& merged = doc.at("values");
        if (!previous) merged = values_;
        for (const auto& key : pendingKeys_) {
            const auto value = values_.find(key);
            if (value == values_.end()) merged.erase(key);
            else merged[key] = *value;
        }
        SettingsStage stage(destination.parent_path());
        writeSettingsStage(stage, doc.dump(2) + '\n');
        std::string backupWarning;
        media::ReplaceHooks hooks;
        hooks.validateTarget = [&](const std::filesystem::path& target) -> std::error_code {
            // Another writer can replace the document before this lease was
            // taken. Never publish a merge or a backup of those stale bytes.
            if (readSettingsSnapshot(target) != previous)
                throw std::runtime_error("settings changed during save; retry");
            if (!previous) return {};
            auto backup = target;
            backup += ".bak";
            SettingsStage backupStage(target.parent_path());
            writeSettingsStage(backupStage, *previous);
            auto saved = media::replaceFile(backupStage.file(), backup);
            if (saved.status != media::PublishStatus::published)
                throw std::runtime_error("cannot save the previous settings: " + saved.error);
            backupWarning = std::move(saved.warning);
            if (readSettingsSnapshot(target) != previous)
                throw std::runtime_error("settings changed during backup; retry");
            return {};
        };
        auto published = media::replaceFile(stage.file(), destination, std::move(hooks));
        if (published.status != media::PublishStatus::published) {
            why = std::move(published.error);
            return false;
        }
        values_ = std::move(merged);
        pendingKeys_.clear();
        quietUntil_.reset();
        retryAfter_.reset();
        retryDelay_ = std::chrono::seconds{1};
        why = std::move(published.warning);
        if (!backupWarning.empty()) {
            if (!why.empty()) why += '\n';
            why += "settings backup: " + backupWarning;
        }
        return true;
    } catch (const std::exception& error) {
        why = error.what();
        return false;
    }
}

void AppSettings::saveSoon(SaveClock::time_point now) noexcept {
    if (!pendingKeys_.empty()) quietUntil_ = now + std::chrono::milliseconds{500};
}

std::optional<AppSettings::SaveAttempt> AppSettings::tick(SaveClock::time_point now) {
    if (!quietUntil_ || now < *quietUntil_ || (retryAfter_ && now < *retryAfter_)) return {};
    auto attempt = flush();
    if (attempt && !attempt->saved) {
        retryAfter_ = now + retryDelay_;
        retryDelay_ = std::min(retryDelay_ * 2, std::chrono::seconds{30});
    }
    return attempt;
}

std::optional<AppSettings::SaveAttempt> AppSettings::flush() {
    if (pendingKeys_.empty()) return {};
    std::string diagnostic;
    const bool saved = save(diagnostic);
    return SaveAttempt{saved, std::move(diagnostic)};
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
           s.page == "Audio" || s.page == "Engine" || s.page == "Plug-Ins" || s.section == "Privacy" ||
           s.page == "AI";
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

std::string windowZoomKey(std::string_view window, std::string_view display) {
    return std::string(window) + "@" + std::string(display);
}

std::optional<double> windowZoom(const AppSettings& s, std::string_view window, std::string_view display) {
    const Value table = s.get(kWindowZoomKey);
    if (!table.is_object()) return std::nullopt;
    const auto it = table.find(windowZoomKey(window, display));
    if (it == table.end() || !it->is_number()) return std::nullopt;
    return it->get<double>();
}

bool setWindowZoom(AppSettings& s, std::string_view window, std::string_view display, double zoom,
                   const ChangeContext& who, std::string& why) {
    Value table = s.get(kWindowZoomKey);
    if (!table.is_object()) table = Value::object();
    table[windowZoomKey(window, display)] = zoom;
    return s.set(kWindowZoomKey, table, who, why);
}

}  // namespace adi::settings
