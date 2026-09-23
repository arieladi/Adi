// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace adi::test {

// Test-only OS adapter: no platform branch is added to production/shared code.
inline auto processId() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

/// One test's scratch directory. Suite/PID/token separate concurrent processes;
/// the counter also separates repeated tokens in one process. Exclusive create
/// avoids taking over stale directories after PID reuse. Never delete on entry.
/// Declare this BEFORE any open files/stores so they close before its destructor.
class TempDirectory {
public:
    TempDirectory(const char* suite, const char* token) {
        const auto prefix = std::string("adi_test_") + suite + "_" +
                            std::to_string(processId()) + "_" + token + "_";
        const auto root = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto candidate = root / (prefix + std::to_string(
                serial_.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error("create test directory", candidate, error);
        }
        throw std::runtime_error("could not claim a unique test directory after 128 attempts");
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    inline static std::atomic<unsigned long> serial_{0};
    std::filesystem::path path_;
};

} // namespace adi::test
