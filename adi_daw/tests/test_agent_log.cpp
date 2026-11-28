// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/agent_log.hpp"
#include "adi/scan_process.hpp"
#include "temp_directory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <future>
#include <set>
#include <string>
#include <string_view>

namespace {

using namespace adi::control;
namespace fs = std::filesystem;
constexpr std::string_view kProject = "shared-project-id";
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view detail) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FAIL  %.*s\n", static_cast<int>(detail.size()), detail.data());
    }
}

std::string pathUtf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void testTimeline() {
    std::puts("[timeline order, persistence, and project isolation]");
    adi::test::TempDirectory temp("agent_log", "timeline");
    const auto path = temp.path() / "agent-log";
    AgentLog writer(path);
    const auto missing = writer.read(kProject);
    check(missing && missing->empty() && !fs::exists(path), "a missing timeline creates no file");

    const std::array events{
        AgentLogEvent{AgentLogKind::Request, "job-one", "Make this phrase quieter\nthen show me."},
        AgentLogEvent{AgentLogKind::Answer, "job-one", "I found the phrase."},
        AgentLogEvent{AgentLogKind::Proposal, "change-one", "Lower clip gain", "awaiting_review"},
        AgentLogEvent{AgentLogKind::ProposalOutcome, "change-one", {}, "applied", "user"},
        AgentLogEvent{AgentLogKind::ViewChange, "job-one", "show Vox"},
        AgentLogEvent{AgentLogKind::StopPress, {}, "Rejected one proposal", {}, "user", "keyboard"},
    };
    for (const auto& event : events) check(writer.append(kProject, event), "append a timeline event");
    check(writer.append("../different/project", {AgentLogKind::Request, "private", "Another project"}),
          "an identity with path syntax is treated only as a key");

    AgentLog reopened(path);
    const auto rows = reopened.read(kProject);
    check(rows && rows->size() == events.size(), "reopening reads only this project's history");
    if (rows && rows->size() == events.size()) {
        std::int64_t previous = 0;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto& row = (*rows)[i];
            const auto& event = events[i];
            check(row.sequence > previous && row.utcMicros > 0 && row.event.kind == event.kind &&
                      row.event.id == event.id && row.event.text == event.text && row.event.state == event.state &&
                      row.event.actor == event.actor && row.event.source == event.source,
                  "the event fields survive in order");
            previous = row.sequence;
        }
        const auto page = reopened.read(kProject, (*rows)[1].sequence, 2);
        check(page && page->size() == 2 && (*page)[0].event.kind == AgentLogKind::Proposal &&
                  (*page)[1].event.kind == AgentLogKind::ProposalOutcome,
              "pagination resumes strictly after the supplied sequence");
    }
    const auto other = reopened.read("../different/project");
    check(other && other->size() == 1 && other->front().event.id == "private",
          "the other project's log is independent");
    check(!writer.append({}, {AgentLogKind::Request}) &&
              !writer.append(kProject, {AgentLogKind::Answer, {}, std::string(1024 * 1024 + 1, 'x')}),
          "an empty project identity and oversized event are refused");

    std::size_t databases = 0;
    for (const auto& entry : fs::directory_iterator(path))
        if (entry.path().extension() == ".sqlite") ++databases;
    check(databases == 2 && !fs::exists(temp.path() / "different"),
          "each project has exactly one database with a safe filename");
}

int appendAsChild(const fs::path& directory, std::string_view child) {
    AgentLog log(directory);
    for (int number = 0; number < 8; ++number) {
        const auto id = std::string(child) + "-" + std::to_string(number);
        if (!log.append(kProject, {AgentLogKind::Request, id, "concurrent write"})) return 3;
    }
    return 0;
}

void testConcurrentProcesses(const fs::path& executable) {
    std::puts("[separate processes share one ordered timeline]");
    adi::test::TempDirectory temp("agent_log", "concurrent");
    const auto directory = temp.path() / "agent-log";
    const auto worker = [&](std::string id) {
        return adi::scan::runProcess(executable, {"--append-log", pathUtf8(directory), std::move(id)});
    };
    auto first = std::async(std::launch::async, worker, "first");
    auto second = std::async(std::launch::async, worker, "second");
    auto third = std::async(std::launch::async, worker, "third");
    const auto a = first.get();
    const auto b = second.get();
    const auto c = third.get();
    check(a.succeeded() && b.succeeded() && c.succeeded(), "all three child writers append successfully");
    const auto rows = AgentLog(directory).read(kProject);
    check(rows && rows->size() == 24, "competing processes lose no entries");
    if (!rows) return;
    std::set<std::string> ids;
    std::int64_t last = 0;
    bool increasing = true;
    for (const auto& row : *rows) {
        ids.insert(row.event.id);
        increasing = increasing && row.sequence > last;
        last = row.sequence;
    }
    check(ids.size() == 24 && increasing, "every appended entry has a distinct chronological position");
}

void testPrivateFiles() {
    std::puts("[owner-only storage refuses unsafe files]");
    adi::test::TempDirectory temp("agent_log", "privacy");
    const auto directory = temp.path() / "agent-log";
    AgentLog log(directory);
    const auto event = AgentLogEvent{AgentLogKind::Request, "job", "private project text"};
    const bool seeded = log.append(kProject, event);
    check(seeded, "write the log in a new private directory");
    if (!seeded) return;
    fs::path database;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().extension() == ".sqlite") database = entry.path();
    check(!database.empty(), "find the single log database");
    if (database.empty()) return;

#if !defined(_WIN32)
    const auto privateMode = [](const fs::path& path) {
        constexpr auto other = fs::perms::group_all | fs::perms::others_all;
        return (fs::status(path).permissions() & other) == fs::perms::none;
    };
    check(privateMode(directory) && privateMode(database), "the data directory and log have owner-only modes");
    fs::permissions(database, fs::perms::group_read, fs::perm_options::add);
    check(!log.read(kProject) && !log.append(kProject, event), "an insecure existing log is never read or written");
    fs::permissions(database, fs::perms::group_read, fs::perm_options::remove);
#endif

    const auto backup = directory / "retained.sqlite";
    fs::rename(database, backup);
    std::error_code error;
    fs::create_hard_link(backup, database, error);
    if (error) {
        fs::create_directory(database);
    }
    const auto length = fs::file_size(backup);
    check(!log.read(kProject) && !log.append(kProject, event) && fs::file_size(backup) == length,
          "a hard link or directory in place of the log cannot redirect it");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--append-log")
        return appendAsChild(fs::path(reinterpret_cast<const char8_t*>(argv[2])), argv[3]);
    try {
        const auto executable = fs::absolute(argv[0]);
        testTimeline();
        testConcurrentProcesses(executable);
        testPrivateFiles();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::printf("%s -- %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
