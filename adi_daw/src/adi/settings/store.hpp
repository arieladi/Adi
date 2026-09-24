// SPDX-License-Identifier: GPL-3.0-or-later
//
// App-scope settings: one JSON file per application (ADR-0149, ADR-0152).
//
//   <appdata config>/settings.json
//   {"schema": 1, "app": "ADI DAW", "values": {"audio.bufferSize": 256, ...}}
//
// Four promises, each with a test that plants its breach:
//   * ADI DAW, ADI Live and aDiJ never read each other's file (ADR-0145 d10):
//     a file naming another application is refused and never overwritten.
//   * A key this build does not know -- a newer build's -- survives a save.
//   * A file that does not parse is moved aside, never overwritten silently,
//     and the application starts from defaults.
//   * Only App-scope settings are written here. A Project setting is a row in
//     the .adi and changes through its op; the store refuses it by name.
//
// Every change can be logged (ADR-0125 d3): one JSON line per change, with
// the time, who, the key, before and after. An agent's change is ALWAYS
// logged; a person's is logged when the log is enabled, which it is by
// default -- the recommendation ADR-0125 left open, decided in ADR-0152.

#pragma once

#include "adi/appdata.hpp"
#include "adi/settings/registry.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace adi::settings {

inline constexpr int kSettingsSchema = 1;

enum class LoadStatus {
    Defaults,      // no file yet: defaults
    Loaded,        // read
    CorruptKept,   // did not parse; moved aside to `keptAside`, defaults in use
    ForeignApp,    // names another application; untouched, defaults in use, saving refused
    NewerSchema,   // written by a newer build: read, its unknown keys kept
};
[[nodiscard]] const char* toString(LoadStatus);

enum class Actor { User, Agent };

/// Who made a change and when. The time is passed in, never read from a
/// clock here, so tests and logs are exact.
struct ChangeContext {
    Actor actor = Actor::User;
    std::string detail;        // the model and version for the agent
    std::int64_t timeUtc = 0;  // microseconds
};

class AppSettings {
public:
    /// This application's own file: appdata::pathsFor(app).config/settings.json.
    [[nodiscard]] static AppSettings forApp(appdata::App app);

    /// An explicit file, for tests and for a portable install.
    AppSettings(appdata::App app, std::filesystem::path file);

    [[nodiscard]] LoadStatus status() const { return status_; }
    [[nodiscard]] const std::filesystem::path& file() const { return file_; }
    [[nodiscard]] const std::optional<std::filesystem::path>& keptAside() const { return aside_; }
    [[nodiscard]] appdata::App app() const { return app_; }

    /// The value in force: the stored one if it is legal for this build, the
    /// registry's default otherwise. Unknown keys return null.
    [[nodiscard]] Value get(const std::string& key) const;

    /// Sets an App-scope value. Refuses unknown keys, Project and Device
    /// scope (naming the op or the device), and illegal values. Logs the
    /// change when the log is on. Does not save.
    bool set(const std::string& key, const Value& value, const ChangeContext& who, std::string& why);

    /// Back to the registry default, for one key or a whole page (the page's
    /// Defaults button, ADR-0125 d3's recovery).
    bool reset(const std::string& key, const ChangeContext& who, std::string& why);
    void resetPage(const std::string& page, const ChangeContext& who);

    /// Writes the file atomically (a temporary beside it, then a rename).
    /// Keys this build does not know are written back exactly as read.
    /// Refused for a file that names another application.
    bool save(std::string& why) const;

    /// The raw stored values, unknown keys included -- what save writes.
    [[nodiscard]] const Value& stored() const { return values_; }

    /// The change log beside the file (settings-changes.jsonl); on by default.
    void setLogUserChanges(bool on) { logUsers_ = on; }
    [[nodiscard]] std::filesystem::path changeLogFile() const;

private:
    bool write(const std::string& key, const Value& value, const ChangeContext& who, std::string& why);
    void log(const std::string& key, const Value& before, const Value& after, const ChangeContext& who) const;

    appdata::App app_;
    std::filesystem::path file_;
    LoadStatus status_ = LoadStatus::Defaults;
    std::optional<std::filesystem::path> aside_;
    int schema_ = kSettingsSchema;
    Value values_ = Value::object();
    bool logUsers_ = true;
};

// --- the agent's pipeline (ADR-0125 d1-d3) -----------------------------------------------

enum class AgentTier { Observe, Propose, Apply };

struct AgentResult {
    bool ok = false;
    std::string why;
};

/// The one path by which the agent changes an application setting. Apply
/// tier only; the setting must be on the whitelist (`agentMayChange`), and
/// never a path, the audio page, privacy or the agent's own tier -- those are
/// refused even if someone marks them. Every accepted change is logged.
/// There is no undo (d3): recovery is a bundle or the page's Defaults button.
AgentResult agentSet(AppSettings&, AgentTier tier, const std::string& key, const Value& value,
                     const std::string& detail, std::int64_t timeUtc);

/// The structural half of the whitelist, checked by agentSet and by the
/// registry test: a setting the agent may NEVER touch, however it is marked.
[[nodiscard]] bool agentForbidden(const Setting&);

// --- presets (R-04) ---------------------------------------------------------------------

/// A named set of pages and the values those pages held. Whole when it holds
/// every page, partial otherwise; applying one changes ITS pages only.
struct Preset {
    std::string name;
    std::vector<std::string> pages;
    Value values = Value::object();   // key -> value, App scope only
};

[[nodiscard]] Preset makePreset(const AppSettings&, std::string name, std::vector<std::string> pages);

struct PresetResult {
    int applied = 0;
    std::vector<std::string> skipped;   // keys refused: not on the preset's pages, unknown, illegal
};
PresetResult applyPreset(AppSettings&, const Preset&, const ChangeContext& who);

[[nodiscard]] Value toJson(const Preset&);
[[nodiscard]] std::optional<Preset> presetFromJson(const Value&, std::string& why);

}  // namespace adi::settings
