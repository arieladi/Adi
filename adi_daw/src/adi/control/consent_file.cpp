// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/consent_file.hpp"

#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <vector>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace adi::control::detail {

struct PrivateFileLock {
#if defined(_WIN32)
    PrivateFileLock(HANDLE folder, HANDLE guard) : folder_(folder), guard_(guard) {}
    ~PrivateFileLock() {
        CloseHandle(guard_);
        CloseHandle(folder_);
    }
#else
    PrivateFileLock(int folder, int guard) : folder_(folder), guard_(guard) {}
    ~PrivateFileLock() {
        (void)::close(guard_);
        (void)::close(folder_);
    }
#endif
    PrivateFileLock(const PrivateFileLock&) = delete;
    PrivateFileLock& operator=(const PrivateFileLock&) = delete;

private:
#if defined(_WIN32)
    HANDLE folder_, guard_;
#else
    int folder_, guard_;
#endif
};

namespace {

namespace fs = std::filesystem;

bool usablePath(const fs::path& file) {
    return file.has_parent_path() && file.has_filename() &&
           file.filename() != "." && file.filename() != "..";
}

template<typename Read>
std::optional<std::string> readBounded(std::size_t maximum, Read read) {
    std::string bytes;
    std::array<char, 4096> block{};
    while (true) {
        const auto count = read(block);
        if (!count || *count > maximum - bytes.size()) return std::nullopt;
        if (*count == 0) return bytes;
        bytes.append(block.data(), *count);
    }
}

fs::path stageName(fs::path name) {
#if defined(_WIN32)
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = ::getpid();
#endif
    static std::atomic<std::uint64_t> sequence{0};
    name += ".stage-" + std::to_string(pid) + "-" +
            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return name;
}

#if defined(_WIN32)

struct NativeFile {
    explicit NativeFile(HANDLE openHandle) : handle(openHandle) {}
    ~NativeFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    explicit operator bool() const { return handle != INVALID_HANDLE_VALUE; }
    HANDLE handle;
};

bool missing(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// ADI's local data parent may inherit a normal profile ACL. This leaf and its
// files carry an explicit, inheritable DACL for the current user alone.
class PrivateSecurity {
public:
    bool init() {
        HANDLE rawToken = INVALID_HANDLE_VALUE;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return false;
        const NativeFile token(rawToken);
        DWORD tokenSize = 0;
        (void)GetTokenInformation(token.handle, TokenUser, nullptr, 0, &tokenSize);
        if (tokenSize < sizeof(TOKEN_USER)) return false;
        token_.resize(tokenSize);
        if (!GetTokenInformation(token.handle, TokenUser, token_.data(), tokenSize, &tokenSize)) return false;
        user_ = reinterpret_cast<const TOKEN_USER*>(token_.data())->User.Sid;
        const DWORD aclSize = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
                              GetLengthSid(user_);
        acl_.resize(aclSize);
        auto* acl = reinterpret_cast<ACL*>(acl_.data());
        if (!InitializeAcl(acl, aclSize, ACL_REVISION) ||
            !AddAccessAllowedAceEx(acl, ACL_REVISION, CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE,
                                   FILE_ALL_ACCESS, user_) ||
            !InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor_, user_, FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor_, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) return false;
        attributes_.nLength = static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES));
        attributes_.lpSecurityDescriptor = &descriptor_;
        return true;
    }

    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }

    bool verifies(HANDLE file, bool directory) const {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory ||
            (!directory && info.nNumberOfLinks != 1)) return false;
        PSID owner = nullptr;
        PACL acl = nullptr;
        struct Descriptor {
            PSECURITY_DESCRIPTOR value = nullptr;
            ~Descriptor() { if (value) LocalFree(value); }
        } desc;
        if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            &owner, nullptr, &acl, nullptr, &desc.value) != ERROR_SUCCESS ||
            !owner || !EqualSid(owner, user_) || !acl || acl->AceCount != 1) return false;
        void* rawAce = nullptr;
        if (!GetAce(acl, 0, &rawAce) ||
            static_cast<const ACE_HEADER*>(rawAce)->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
        return EqualSid(&ace->SidStart, user_) != 0;
    }

private:
    std::vector<std::byte> token_, acl_;
    PSID user_ = nullptr;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

constexpr DWORD readShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

bool makeDirectory(const fs::path& folder, PrivateSecurity& security) {
    if (folder.has_parent_path()) fs::create_directories(folder.parent_path());
    return CreateDirectoryW(folder.c_str(), security.attributes()) || GetLastError() == ERROR_ALREADY_EXISTS;
}

HANDLE openDirectory(const fs::path& folder) {
    // Deny replacing the checked directory while using child paths.
    return CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

bool canReplace(const fs::path& target, const PrivateSecurity& security) {
    const NativeFile existing(CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                         readShare, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return existing ? security.verifies(existing.handle, false) : missing(GetLastError());
}

PrivateFileStatus fileStatus(const fs::path& file) {
    PrivateSecurity security;
    if (!security.init()) return PrivateFileStatus::Unavailable;
    const NativeFile directory(openDirectory(file.parent_path()));
    if (!directory)
        return missing(GetLastError()) ? PrivateFileStatus::Missing : PrivateFileStatus::Unavailable;
    if (!security.verifies(directory.handle, true)) return PrivateFileStatus::Unavailable;
    const NativeFile existing(CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                         readShare, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!existing)
        return missing(GetLastError()) ? PrivateFileStatus::Missing : PrivateFileStatus::Unavailable;
    return security.verifies(existing.handle, false) ? PrivateFileStatus::Private : PrivateFileStatus::Unavailable;
}

std::optional<std::string> readFile(const fs::path& file, std::size_t maximum) {
    PrivateSecurity security;
    if (!security.init()) return std::nullopt;
    const NativeFile directory(openDirectory(file.parent_path()));
    if (!directory) return missing(GetLastError()) ? std::optional<std::string>("") : std::nullopt;
    if (!security.verifies(directory.handle, true)) return std::nullopt;

    const NativeFile input(CreateFileW(file.c_str(), GENERIC_READ, readShare, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!input) return missing(GetLastError()) ? std::optional<std::string>("") : std::nullopt;
    if (!security.verifies(input.handle, false)) return std::nullopt;
    return readBounded(maximum, [&](auto& block) -> std::optional<std::size_t> {
        DWORD count = 0;
        if (!ReadFile(input.handle, block.data(), static_cast<DWORD>(block.size()), &count, nullptr))
            return std::nullopt;
        return static_cast<std::size_t>(count);
    });
}

bool writeFile(const fs::path& file, std::string_view contents) {
    const auto folder = file.parent_path();
    PrivateSecurity security;
    if (!security.init() || !makeDirectory(folder, security)) return false;
    const NativeFile directory(openDirectory(folder));
    if (!directory || !security.verifies(directory.handle, true)) return false;

    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto stage = folder / stageName(file.filename());
        bool ready = false;
        {
            const NativeFile output(CreateFileW(stage.c_str(), GENERIC_WRITE, 0, security.attributes(),
                                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!output) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
                return false;
            }
            DWORD written = 0;
            ready = WriteFile(output.handle, contents.empty() ? "" : contents.data(),
                              static_cast<DWORD>(contents.size()), &written, nullptr) &&
                    static_cast<std::size_t>(written) == contents.size() && FlushFileBuffers(output.handle);
        }
        if (ready && canReplace(file, security) &&
            MoveFileExW(stage.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        (void)DeleteFileW(stage.c_str());
        return false;
    }
    return false;
}

bool eraseFile(const fs::path& file) {
    PrivateSecurity security;
    if (!security.init()) return false;
    const NativeFile directory(openDirectory(file.parent_path()));
    if (!directory) return missing(GetLastError());
    if (!security.verifies(directory.handle, true) || !canReplace(file, security)) return false;
    return DeleteFileW(file.c_str()) || missing(GetLastError());
}

PrivateFileLockAttempt tryLock(const fs::path& file, bool create) {
    const auto folder = file.parent_path();
    PrivateSecurity security;
    if (!security.init() || (create && !makeDirectory(folder, security))) return {};
    NativeFile directory(openDirectory(folder));
    if (!directory || !security.verifies(directory.handle, true)) return {};

    auto lock = file;
    lock += ".lock";
    NativeFile guard(CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, security.attributes(),
                                  create ? OPEN_ALWAYS : OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!guard || !security.verifies(guard.handle, false)) return {};
    OVERLAPPED range{};
    if (!LockFileEx(guard.handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &range)) return {nullptr, GetLastError() == ERROR_LOCK_VIOLATION};
    auto held = std::make_shared<PrivateFileLock>(directory.handle, guard.handle);
    directory.handle = guard.handle = INVALID_HANDLE_VALUE;
    return {std::move(held), false};
}

#else

struct NativeFile {
    explicit NativeFile(int fd) : handle(fd) {}
    ~NativeFile() { if (handle >= 0) (void)::close(handle); }
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    explicit operator bool() const { return handle >= 0; }
    int handle;
};

bool verifies(const struct stat& status, bool directory) {
    return status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0 &&
           (directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode) && status.st_nlink == 1);
}

bool verifies(int fd, bool directory) {
    struct stat status{};
    return ::fstat(fd, &status) == 0 && verifies(status, directory);
}

bool makeDirectory(const fs::path& folder) {
    if (folder.has_parent_path()) fs::create_directories(folder.parent_path());
    return ::mkdir(folder.c_str(), 0700) == 0 || errno == EEXIST;
}

int openDirectory(const fs::path& folder) {
    return ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

bool canReplace(int directory, const fs::path& name) {
    struct stat status{};
    if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) return errno == ENOENT;
    return verifies(status, false);
}

PrivateFileStatus fileStatus(const fs::path& file) {
    const NativeFile directory(openDirectory(file.parent_path()));
    if (!directory)
        return errno == ENOENT ? PrivateFileStatus::Missing : PrivateFileStatus::Unavailable;
    if (!verifies(directory.handle, true)) return PrivateFileStatus::Unavailable;
    struct stat status{};
    if (::fstatat(directory.handle, file.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? PrivateFileStatus::Missing : PrivateFileStatus::Unavailable;
    return verifies(status, false) ? PrivateFileStatus::Private : PrivateFileStatus::Unavailable;
}

std::optional<std::string> readFile(const fs::path& file, std::size_t maximum) {
    const auto folder = file.parent_path();
    const NativeFile directory(openDirectory(folder));
    if (!directory) return errno == ENOENT ? std::optional<std::string>("") : std::nullopt;
    if (!verifies(directory.handle, true)) return std::nullopt;

    const auto name = file.filename();
    const NativeFile input(::openat(directory.handle, name.c_str(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!input) return errno == ENOENT ? std::optional<std::string>("") : std::nullopt;
    if (!verifies(input.handle, false)) return std::nullopt;
    return readBounded(maximum, [&](auto& block) -> std::optional<std::size_t> {
        ssize_t count;
        do { count = ::read(input.handle, block.data(), block.size()); } while (count < 0 && errno == EINTR);
        if (count < 0) return std::nullopt;
        return static_cast<std::size_t>(count);
    });
}

bool syncFile(int fd, bool directory) {
    int status;
#if defined(__APPLE__)
    if (!directory) {
        do { status = ::fcntl(fd, F_FULLFSYNC); } while (status < 0 && errno == EINTR);
        return status == 0;
    }
#else
    (void)directory;
#endif
    do { status = ::fsync(fd); } while (status < 0 && errno == EINTR);
    return status == 0;
}

bool writeAll(int fd, std::string_view contents) {
    while (!contents.empty()) {
        const auto count = ::write(fd, contents.data(), contents.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        contents.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

bool writeFile(const fs::path& file, std::string_view contents) {
    const auto folder = file.parent_path();
    if (!makeDirectory(folder)) return false;
    const NativeFile directory(openDirectory(folder));
    if (!directory || !verifies(directory.handle, true)) return false;

    const auto name = file.filename();
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto stage = stageName(name);
        const NativeFile output(::openat(directory.handle, stage.c_str(),
                                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (!output) {
            if (errno == EEXIST) continue;
            return false;
        }
        if (writeAll(output.handle, contents) && syncFile(output.handle, false) &&
            canReplace(directory.handle, name) &&
            ::renameat(directory.handle, stage.c_str(), directory.handle, name.c_str()) == 0)
            return syncFile(directory.handle, true);
        (void)::unlinkat(directory.handle, stage.c_str(), 0);
        return false;
    }
    return false;
}

bool eraseFile(const fs::path& file) {
    const auto folder = file.parent_path();
    const NativeFile directory(openDirectory(folder));
    if (!directory) return errno == ENOENT;
    if (!verifies(directory.handle, true)) return false;

    const auto name = file.filename();
    if (!canReplace(directory.handle, name)) return false;
    if (::unlinkat(directory.handle, name.c_str(), 0) != 0) return errno == ENOENT;
    return syncFile(directory.handle, true);
}

PrivateFileLockAttempt tryLock(const fs::path& file, bool create) {
    const auto folder = file.parent_path();
    if (create && !makeDirectory(folder)) return {};
    NativeFile directory(openDirectory(folder));
    if (!directory || !verifies(directory.handle, true)) return {};

    auto lock = file.filename();
    lock += ".lock";
    const int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (create ? O_CREAT : 0);
    NativeFile guard(::openat(directory.handle, lock.c_str(), flags, 0600));
    if (!guard || !verifies(guard.handle, false)) return {};
    int status;
    do { status = ::flock(guard.handle, LOCK_EX | LOCK_NB); } while (status < 0 && errno == EINTR);
    if (status != 0) return {nullptr, errno == EWOULDBLOCK || errno == EAGAIN};
    auto held = std::make_shared<PrivateFileLock>(directory.handle, guard.handle);
    directory.handle = guard.handle = -1;
    return {std::move(held), false};
}

#endif

}  // namespace

fs::path consentFilePath() {
    const auto data = appdata::pathsFor(appdata::App::Daw).data;
    return data.empty() ? fs::path{} : data / "consent-adi" / "ledger.json";
}

std::optional<std::string> readConsentFile(const fs::path& file, std::size_t maximum) {
    if (audio::onAudioThread) return std::nullopt;
    try {
        if (usablePath(file)) return readFile(file, std::min(maximum, kConsentFileMaxBytes));
    } catch (...) {
    }
    return std::nullopt;
}

bool writeConsentFile(const fs::path& file, std::string_view contents) {
    if (audio::onAudioThread || contents.size() > kConsentFileMaxBytes) return false;
    try {
        return usablePath(file) && writeFile(file, contents);
    } catch (...) {
        return false;
    }
}

bool withConsentFileLock(const fs::path& file, const std::function<void()>& action) {
    if (audio::onAudioThread || !action) return false;
    try {
        const auto lock = lockPrivateFile(file, true);
        if (!lock.lock) return false;
        action();
        return true;
    } catch (...) {
        return false;
    }
}

PrivateFileStatus privateFileStatus(const fs::path& file) {
    if (audio::onAudioThread) return PrivateFileStatus::Unavailable;
    try {
        if (usablePath(file)) return fileStatus(file);
    } catch (...) {
    }
    return PrivateFileStatus::Unavailable;
}

PrivateFileLockAttempt lockPrivateFile(const fs::path& file, bool create) {
    if (audio::onAudioThread) return {};
    try {
        if (usablePath(file)) return tryLock(file, create);
    } catch (...) {
    }
    return {};
}

bool removePrivateFile(const fs::path& file) {
    if (audio::onAudioThread) return false;
    try {
        return usablePath(file) && eraseFile(file);
    } catch (...) {
        return false;
    }
}

}  // namespace adi::control::detail
