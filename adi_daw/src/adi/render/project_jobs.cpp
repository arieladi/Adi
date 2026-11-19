// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/render/project_jobs.hpp"
#include "adi/render/job_json.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/media/media_ops.hpp"
#include "adi/store.hpp"
#include <SQLiteCpp/SQLiteCpp.h>
#include <sqlite3.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace adi::render {
namespace {
using Json = nlohmann::json;
void require(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(std::string("saved render jobs: ") + reason);
}
std::string ownerPath(const Store& store) {
    require(!store.path().empty() && store.path() != std::filesystem::path(":memory:"),
            "requires a file-backed project");
    require(store.path().is_absolute(), "open the owning project with an absolute path");
    const auto bytes = store.path().u8string();
    std::string result(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(result.find('\0') == std::string::npos, "project path contains NUL");
    return result;
}
// Guard before the DOM, so duplicate nested Job fields cannot be erased by
// parse/dump. The two envelope containers sit outside the Job codec's limit.
struct Guard final : nlohmann::json_sax<Json> {
    std::vector<std::set<std::string>> objects;
    unsigned depth = 0;
    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(Json::number_integer_t) override { return true; }
    bool number_unsigned(Json::number_unsigned_t) override { return true; }
    bool number_float(Json::number_float_t value, const Json::string_t&) override { return std::isfinite(value); }
    bool string(Json::string_t&) override { return true; }
    bool binary(Json::binary_t&) override { return false; }
    bool start_object(std::size_t) override {
        if (++depth > 18) return false;
        objects.emplace_back(); return true;
    }
    bool key(Json::string_t& name) override { return objects.back().insert(name).second; }
    bool end_object() override { objects.pop_back(); --depth; return true; }
    bool start_array(std::size_t) override { return ++depth <= 18; }
    bool end_array() override { --depth; return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};
SavedJobs decode(std::string_view bytes, const std::filesystem::path& currentOwner) {
    require(bytes.size() <= kMaximumSavedJobsBytes, "row exceeds byte limit");
    // The pinned JSON lexer treats a literal NUL as end-of-input. SQLite TEXT
    // can preserve that byte, so reject it before parsing instead of accepting
    // a valid prefix while silently discarding its trailing bytes.
    require(bytes.find('\0') == std::string_view::npos, "row contains a literal NUL byte");
    Guard guard;
    require(Json::sax_parse(bytes.begin(), bytes.end(), &guard), "malformed, duplicate or over-deep JSON");
    const auto value = Json::parse(bytes.begin(), bytes.end());
    require(value.is_object() && value.size() == 4 && value.contains("format") && value.contains("version") &&
            value.contains("ownerProject") && value.contains("jobs"), "unknown or missing envelope fields");
    require(value.at("format") == "adi-project-export-jobs", "unsupported format");
    const auto& version = value.at("version");
    require(version.is_number_integer() && version == 1, "unsupported version");
    const auto& storedOwner = value.at("ownerProject");
    require(storedOwner.is_string(), "ownerProject must be a path string");
    const auto owner = storedOwner.get<std::string>();
    require(!owner.empty() && owner.find('\0') == std::string::npos, "invalid stored owner path");
    const auto oldPath = media::pathFromUtf8(owner);
    require(oldPath != std::filesystem::path(":memory:"), "invalid stored owner path");
    const auto& jobs = value.at("jobs");
    require(jobs.is_array() && jobs.size() <= kMaximumSavedJobs, "invalid jobs array or job count limit");
    SavedJobs result{true, {}};
    result.jobs.reserve(jobs.size());
    for (const auto& item : jobs) {
        auto job = jobFromJson(item.dump());
        require(job.project.native() == oldPath.native(), "job belongs to a different stored project");
        job.project = currentOwner;
        validateJob(job);
        result.jobs.push_back(std::move(job));
    }
    return result;
}
SavedJobs read(const Store& store) {
    SQLite::Statement row(const_cast<SQLite::Database&>(store.db()),
        "SELECT typeof(value),length(CAST(value AS BLOB)),value FROM session_state WHERE key=?");
    row.bind(1, kSavedJobsKey);
    if (!row.executeStep()) return {};
    require(row.getColumn(0).getString() == "text", "stored row is not TEXT");
    const auto length = row.getColumn(1).getInt64();
    require(length >= 0 && static_cast<std::uint64_t>(length) <= kMaximumSavedJobsBytes, "row exceeds byte limit");
    return decode(row.getColumn(2).getString(), store.path());
}
std::string encode(const Store& store, std::span<const Job> jobs) {
    require(jobs.size() <= kMaximumSavedJobs, "job count limit");
    Json value{{"format", "adi-project-export-jobs"}, {"version", 1},
               {"ownerProject", ownerPath(store)}, {"jobs", Json::array()}};
    // Account for each encoded object and separating comma before accumulating
    // a large DOM. The final check includes all envelope syntax/path bytes.
    std::size_t size = value.dump().size();
    require(size <= kMaximumSavedJobsBytes, "row exceeds byte limit");
    for (const auto& job : jobs) {
        require(job.project.native() == store.path().native(), "job project must equal the owning Store path");
        const auto bytes = jobToJson(job);
        const auto extra = bytes.size() + (value.at("jobs").empty() ? 0U : 1U);
        require(extra <= kMaximumSavedJobsBytes - size, "row exceeds byte limit");
        size += extra;
        value["jobs"].push_back(Json::parse(bytes));
    }
    auto bytes = value.dump();
    require(bytes.size() <= kMaximumSavedJobsBytes, "row exceeds byte limit");
    return bytes;
}
} // namespace

std::optional<SavedJobs> loadSavedJobs(const Store& store, std::string& error) {
    if (audio::onAudioThread) return std::nullopt;
    error.clear();
    try {
        (void)ownerPath(store);
        return read(store);
    } catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}
bool saveSavedJobs(Store& store, std::span<const Job> jobs, std::string& error) {
    if (audio::onAudioThread) return false;
    error.clear();
    try {
        require(!store.readOnly(), "project is read-only");
        require(sqlite3_get_autocommit(store.db().getHandle()) != 0, "finish the enclosing SQL transaction first");
        const auto bytes = encode(store, jobs);
        SQLite::Transaction transaction(store.db(), SQLite::TransactionBehavior::IMMEDIATE);
        // Preserve data from a newer writer or damaged row until its explicit
        // recovery/migration is available. Validation and replacement share lock.
        (void)read(store);
        SQLite::Statement update(store.db(),
            "INSERT INTO session_state(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        update.bind(1, kSavedJobsKey); update.bind(2, bytes); update.exec();
        transaction.commit();
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
} // namespace adi::render
