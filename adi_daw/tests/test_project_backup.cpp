// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/media_ops.hpp"
#include "adi/media/project_backup.hpp"
#include "adi/store.hpp"
#include "temp_directory.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main() {
    using namespace adi;
    using namespace std::chrono;
    namespace fs = std::filesystem;
    int checks = 0;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) {
            ++failures;
            std::printf("FAIL: %s\n", label);
        }
    };
    const auto readBytes = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    };
    const adi::test::TempDirectory temp("project_backup", "folder");
    const auto project = temp.path() / media::pathFromUtf8("Set \xc3\x89.adi");
    const auto folder = temp.path() / "Backup";
    fs::create_directory(folder);
    StoreError error;
    const auto store = Store::createNewProject(project, error);
    if (!store) {
        std::printf("Could not create a project fixture: %s\n", toString(error));
        return 1;
    }
    const local_seconds time = local_days{year{2024}/February/29} + hours{13} + minutes{9} +
                               seconds{5};
    std::vector<media::ArchivePhase> phases;
    const auto result = media::backupProject(project, folder, time,
                                               {{}, [&](auto update) { phases.push_back(update.phase); }});
    check(result.archive.ok &&
          result.destination == folder / media::pathFromUtf8("Set \xc3\x89 [2024-02-29 130905].zip"),
          "timestamp includes local leap-day date and seconds, preserving the project name");
    const auto archive = readBytes(result.destination);
    check(archive.starts_with("PK\x03\x04"), "published backup is a ZIP");
    check(!phases.empty() && phases.front() == media::ArchivePhase::Snapshot &&
          phases.back() == media::ArchivePhase::Publish, "report archive progress to the worker caller");
    const auto repeat = media::backupProject(project, folder, time);
    check(!repeat.archive.ok && repeat.destination == result.destination &&
          readBytes(result.destination) == archive, "same-second backup cannot overwrite the archive");

    std::stop_source stop;
    const auto cancelled = media::backupProject(project, folder, time + seconds{1},
                                                  {stop.get_token(), [&](auto update) {
        if (update.phase == media::ArchivePhase::WriteArchive) {
            stop.request_stop();
        }
    }});
    check(cancelled.archive.cancelled && !fs::exists(cancelled.destination),
          "cancellation before publication leaves no destination");
    check(!media::backupProject(project, folder / "missing", time).archive.ok &&
          !fs::exists(folder / "missing"), "backup requires the explicitly chosen existing directory");
    check(!media::backupProject(project, {}, time).archive.ok,
          "a dismissed folder picker supplies no implicit destination");
    check(fs::exists(project), "source stays open and present");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
