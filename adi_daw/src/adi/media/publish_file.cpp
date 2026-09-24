// SPDX-License-Identifier: GPL-3.0-or-later
// All publication platform code lives here (ADR-0109).
#include "publish_file.hpp"
#include "blake3.hpp"
#include <array>
#include <fstream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h> // renamex_np, RENAME_EXCL; no CRT stream operations
#else
#include <sys/syscall.h>
#endif
#endif
namespace adi::media {
namespace fs = std::filesystem;
namespace {
std::error_code renameExclusive(const fs::path& source, const fs::path& target) {
#if defined(_WIN32)
    if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return {};
    const auto error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return std::make_error_code(std::errc::file_exists);
    return {static_cast<int>(error), std::system_category()};
#elif defined(__APPLE__)
    if (renamex_np(source.c_str(), target.c_str(), RENAME_EXCL) == 0) return {};
    return {errno, std::generic_category()};
#else
    // SYS_renameat2 is available on each Linux ABI we support; 1 is
    // RENAME_NOREPLACE. Older kernels/filesystems take the exclusive-copy path.
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), 1U) == 0) return {};
    return {errno, std::generic_category()};
#endif
}
struct ExclusiveFile {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit ExclusiveFile(const fs::path& path, std::error_code& error) {
        handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            error = (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) ? std::make_error_code(std::errc::file_exists) : std::error_code(static_cast<int>(code), std::system_category());
        }
    }
    ~ExclusiveFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void write(const char* data, std::size_t size) {
        while (size) {
            DWORD written = 0;
            if (!WriteFile(handle, data, static_cast<DWORD>(size), &written, nullptr) || written == 0) throw std::runtime_error("publication write failed");
            data += written; size -= written;
        }
    }
    void finish() {
        if (!FlushFileBuffers(handle)) throw std::runtime_error("publication flush failed");
        const auto old = handle; handle = INVALID_HANDLE_VALUE;
        if (!CloseHandle(old)) throw std::runtime_error("publication close failed");
    }
#else
    int handle = -1;
    explicit ExclusiveFile(const fs::path& path, std::error_code& error) {
        handle = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (handle < 0) error = {errno, std::generic_category()};
    }
    ~ExclusiveFile() { if (handle >= 0) ::close(handle); }
    void write(const char* data, std::size_t size) {
        while (size) {
            const auto written = ::write(handle, data, size);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("publication write failed");
            data += written; size -= static_cast<std::size_t>(written);
        }
    }
    void finish() {
        int result;
        do { result = ::fsync(handle); } while (result < 0 && errno == EINTR);
        if (result < 0) throw std::runtime_error("publication flush failed");
        const auto old = handle; handle = -1;
        if (::close(old) != 0) throw std::runtime_error("publication close failed");
    }
#endif
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;
};
struct Cleanup {
    const fs::path& target;
    bool owned = false;
    ~Cleanup() { if (owned) { std::error_code ignored; fs::remove(target, ignored); } }
};
}
PublishResult publishFile(const fs::path& source, const fs::path& target, PublishHooks hooks) noexcept {
    Cleanup cleanup{target};
    try {
        const auto error = hooks.rename ? hooks.rename(source, target) : renameExclusive(source, target);
        if (!error) return {PublishStatus::published, false, {}};
        if (error == std::errc::file_exists) return {PublishStatus::exists, false, error.message()};
        const auto expected = blake3File(source);
        if (!expected) return {PublishStatus::failed, true, "cannot hash staging file"};
        std::error_code createError;
        ExclusiveFile out(target, createError);
        if (createError) return {createError == std::errc::file_exists ? PublishStatus::exists : PublishStatus::failed, true, createError.message()};
        cleanup.owned = true;
        std::ifstream in(source, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read staging file");
        std::array<char, 65536> buffer{};
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto n = in.gcount();
            if (n > 0) out.write(buffer.data(), static_cast<std::size_t>(n));
        }
        if (!in.eof()) throw std::runtime_error("staging read failed");
        out.finish();
        if (hooks.afterCopy) hooks.afterCopy(target);
        const auto actual = blake3File(target);
        if (!actual || actual.hex != expected.hex) throw std::runtime_error("published copy hash mismatch");
        in.close();
        // Failure to remove a private staging name does not invalidate the
        // verified destination; its owning staging directory cleans it later.
        std::error_code ignored; fs::remove(source, ignored);
        cleanup.owned = false;
        return {PublishStatus::published, true, {}};
    } catch (const std::exception& e) { return {PublishStatus::failed, true, e.what()}; }
}
}
