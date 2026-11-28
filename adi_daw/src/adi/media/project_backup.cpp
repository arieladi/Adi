// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/media/project_backup.hpp"

#include "adi/media/media_ops.hpp"

#include <cstdio>
#include <system_error>

namespace adi::media {

ProjectBackupResult backupProject(const std::filesystem::path& project,
                                  const std::filesystem::path& directory,
                                  std::chrono::local_seconds localTime,
                                  const ArchiveControl& control) {
    namespace fs = std::filesystem;
    using namespace std::chrono;
    if (control.stop.stop_requested()) {
        return {{false, {}, {}, true}, {}};
    }
    if (project.stem().empty()) {
        return {{false, "Project backup requires a saved project"}, {}};
    }
    std::error_code error;
    if (directory.empty() || !fs::is_directory(directory, error)) {
        return {{false, "Choose an existing backup directory"}, {}};
    }
    const auto dayStart = floor<days>(localTime);
    if (dayStart < local_days{year{0}/January/1} || dayStart >= local_days{year{10000}/January/1}) {
        return {{false, "Project backup date is outside the file-name range"}, {}};
    }
    const year_month_day date{dayStart};
    const hh_mm_ss timeOfDay{localTime - dayStart};
    char suffix[32]{};
    std::snprintf(suffix, sizeof(suffix), " [%04d-%02u-%02u %02d%02d%02d].zip",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()), static_cast<int>(timeOfDay.hours().count()),
                  static_cast<int>(timeOfDay.minutes().count()),
                  static_cast<int>(timeOfDay.seconds().count()));
    auto filename = project.stem();
    filename += pathFromUtf8(suffix);
    const auto destination = fs::absolute(directory / filename, error);
    if (error) {
        return {{false, "Cannot resolve the project backup destination"}, {}};
    }
    return {collectExport(project, destination, control), destination};
}

} // namespace adi::media
