// SPDX-License-Identifier: GPL-3.0-or-later
// All publication platform code lives here (ADR-0109).
#include "publish_file.hpp"
#include "blake3.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <stdio.h> // renamex_np, RENAME_EXCL; no CRT stream operations
#include <sys/mount.h>
#else
#include <sys/syscall.h>
#if defined(__linux__)
#include <linux/magic.h>
#include <sys/vfs.h>
#endif
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
class CopyStage {
public:
    explicit CopyStage(const fs::path& parent) {
        static std::atomic<unsigned long long> sequence{0};
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            auto candidate = parent / (".adi-publish-" + std::to_string(now) + "-" +
                                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            auto file = candidate / "copy";
            std::error_code error;
#if defined(_WIN32)
            const bool created = fs::create_directory(candidate, error);
#else
            const bool created = ::mkdir(candidate.c_str(), 0700) == 0;
            if (!created) error = {errno, std::generic_category()};
#endif
            if (created) {
                directory_ = std::move(candidate);
                path = std::move(file);
                return;
            }
            if (error && error != std::errc::file_exists)
                throw fs::filesystem_error("reserve publication staging", candidate, error);
        }
        throw std::runtime_error("cannot reserve publication staging");
    }
    ~CopyStage() {
        std::error_code ignored;
        if (createdCopy) fs::remove(path, ignored);
        fs::remove(directory_, ignored);
    }
    CopyStage(const CopyStage&) = delete;
    CopyStage& operator=(const CopyStage&) = delete;
    fs::path path;
    bool createdCopy = false;
private:
    fs::path directory_;
};
}
namespace {
std::error_code nativeError() noexcept {
#if defined(_WIN32)
    return {static_cast<int>(GetLastError()),std::system_category()};
#else
    return {errno,std::generic_category()};
#endif
}
#if defined(_WIN32)
using NativeFile=HANDLE;
const NativeFile invalidFile=INVALID_HANDLE_VALUE;
#else
using NativeFile=int;
constexpr NativeFile invalidFile=-1;
#endif
void closeNative(NativeFile file) noexcept {
#if defined(_WIN32)
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
#else
    if(file>=0)(void)::close(file);
#endif
}
#if defined(_WIN32)
constexpr bool unsupportedWideIdentity(DWORD error) noexcept {
    // The information class and buffer size are fixed and valid. Unsupported
    // classes/drivers may report INVALID_PARAMETER as well as NOT_SUPPORTED.
    return error==ERROR_INVALID_FUNCTION || error==ERROR_NOT_SUPPORTED ||
        error==ERROR_INVALID_PARAMETER || error==ERROR_CALL_NOT_IMPLEMENTED;
}
constexpr std::uint64_t identityHalf(const unsigned char* bytes) noexcept {
    std::uint64_t value=0;
    for(unsigned i=0;i<8;++i)value|=static_cast<std::uint64_t>(bytes[i])<<(8*i);
    return value;
}
constexpr std::optional<FileIdentity> legacyIdentity(std::uint64_t volume,std::uint64_t index) noexcept {
    if(index==0 || index==std::numeric_limits<std::uint64_t>::max())return {};
    return FileIdentity{volume,index,0,FileIdentityScheme::WindowsLegacy64};
}
static_assert(!legacyIdentity(1,0) && !legacyIdentity(1,std::numeric_limits<std::uint64_t>::max()));
static_assert(legacyIdentity(1,42)->file==42 && legacyIdentity(1,42)->fileHigh==0);
static_assert(unsupportedWideIdentity(ERROR_NOT_SUPPORTED) && unsupportedWideIdentity(ERROR_INVALID_PARAMETER) &&
              unsupportedWideIdentity(ERROR_INVALID_FUNCTION) && unsupportedWideIdentity(ERROR_CALL_NOT_IMPLEMENTED));
static_assert(!unsupportedWideIdentity(ERROR_ACCESS_DENIED) && !unsupportedWideIdentity(ERROR_INVALID_HANDLE) &&
              !unsupportedWideIdentity(ERROR_READ_FAULT) && !unsupportedWideIdentity(ERROR_SHARING_VIOLATION));
constexpr unsigned char identityByteFixture[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static_assert(identityHalf(identityByteFixture)==0x0807060504030201ULL &&
              identityHalf(identityByteFixture+8)==0x100f0e0d0c0b0a09ULL);
std::optional<FileIdentity> windowsIdentityOf(NativeFile file,std::error_code& error,bool rejectReparse=false) noexcept {
    // The legacy query supplies attributes only unless the wide query is
    // explicitly unsupported. Its ReFS index may be a colliding sentinel.
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)){error=nativeError();return {};}
    if((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0){error=std::make_error_code(std::errc::is_a_directory);return {};}
    if(rejectReparse && (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)!=0){
        error=std::make_error_code(std::errc::operation_not_permitted);return {};
    }
    FILE_ID_INFO wide{};
    if(GetFileInformationByHandleEx(file,FileIdInfo,&wide,sizeof(wide))){
        error.clear();return FileIdentity{wide.VolumeSerialNumber,identityHalf(wide.FileId.Identifier),
            identityHalf(wide.FileId.Identifier+8),FileIdentityScheme::Windows128};
    }
    const auto wideError=GetLastError();
    if(!unsupportedWideIdentity(wideError)){error={static_cast<int>(wideError),std::system_category()};return {};}
    const auto result=legacyIdentity(info.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow);
    if(!result){error=std::make_error_code(std::errc::operation_not_supported);return {};}
    error.clear();return result;
}
std::optional<FileRevision> windowsRevisionOf(NativeFile file, std::error_code& error) noexcept {
    const auto identity=windowsIdentityOf(file,error,true);
    if(!identity)return {};
    FILE_BASIC_INFO times{};
    FILE_STANDARD_INFO dimensions{};
    if(!GetFileInformationByHandleEx(file,FileBasicInfo,&times,sizeof(times)) ||
       !GetFileInformationByHandleEx(file,FileStandardInfo,&dimensions,sizeof(dimensions))){error=nativeError();return {};}
    if(dimensions.EndOfFile.QuadPart<0){error=std::make_error_code(std::errc::invalid_argument);return {};}
    error.clear();
    return FileRevision{*identity,static_cast<std::uint64_t>(dimensions.EndOfFile.QuadPart),
                        {times.LastWriteTime.QuadPart,0},{times.ChangeTime.QuadPart,0}};
}
#else
std::optional<FileRevision> statRevision(const struct stat& info, std::error_code& error) noexcept {
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }
#if defined(__APPLE__)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    error.clear();
    return FileRevision{{static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)},
                        static_cast<std::uint64_t>(info.st_size),
                        {static_cast<std::int64_t>(modified.tv_sec), static_cast<std::int64_t>(modified.tv_nsec)},
                        {static_cast<std::int64_t>(changed.tv_sec), static_cast<std::int64_t>(changed.tv_nsec)}};
}
#endif
std::optional<FileIdentity> identityOf(NativeFile file,std::error_code& error) noexcept {
#if defined(_WIN32)
    return windowsIdentityOf(file,error);
#else
    struct stat info{};
    if(::fstat(file,&info)!=0){error=nativeError();return {};}
    if(!S_ISREG(info.st_mode)){error=std::make_error_code(std::errc::invalid_argument);return {};}
    error.clear();return FileIdentity{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
#endif
}
std::optional<FileIdentity> identityAt(const fs::path& path,std::error_code& error) noexcept {
#if defined(_WIN32)
    const auto file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
#else
    // Closing any extra descriptor for a SQLite database can release all
    // POSIX record locks held by this process. Identity queries never open it.
    struct stat info{};
    if(::stat(path.c_str(),&info)!=0){error=nativeError();return {};}
    if(!S_ISREG(info.st_mode)){error=std::make_error_code(std::errc::invalid_argument);return {};}
    error.clear();return FileIdentity{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
#endif
#if defined(_WIN32)
    if(file==invalidFile){error=nativeError();return {};}
    const auto result=identityOf(file,error);closeNative(file);return result;
#endif
}
#if !defined(_WIN32)
NativeFile openLeaseInode(const FileIdentity& identity,std::error_code& error) {
    const fs::path directory=fs::path("/tmp")/("adi-project-leases-"+std::to_string(static_cast<std::uint64_t>(::getuid())));
    if(::mkdir(directory.c_str(),0700)!=0 && errno!=EEXIST){error=nativeError();return invalidFile;}
    struct stat info{};
    if(::lstat(directory.c_str(),&info)!=0){error=nativeError();return invalidFile;}
    if(!S_ISDIR(info.st_mode) || info.st_uid!=::getuid() || (info.st_mode&0077)!=0){error=std::make_error_code(std::errc::permission_denied);return invalidFile;}
    const auto parent=::open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(parent<0){error=nativeError();return invalidFile;}
    const auto name=std::to_string(identity.volume)+"-"+std::to_string(identity.file)+".lock";
    const auto result=::openat(parent,name.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(result<0)error=nativeError();else error.clear();
    (void)::close(parent);
    // Never unlink a lease inode: a concurrent opener may already own its FD.
    return result;
}
#endif
std::error_code lockNative(NativeFile file,bool exclusive) noexcept {
#if defined(_WIN32)
    // Outside SQLite's lock bytes and maximum possible database length. Byte
    // locking here never obstructs actual SQLite header/page reads or writes.
    OVERLAPPED range{};range.OffsetHigh=0x10000000u;
    const DWORD flags=LOCKFILE_FAIL_IMMEDIATELY|(exclusive?LOCKFILE_EXCLUSIVE_LOCK:0u);
    if(!LockFileEx(file,flags,0,1,0,&range))return nativeError();
#else
    if(::flock(file,(exclusive?LOCK_EX:LOCK_SH)|LOCK_NB)!=0)return nativeError();
#endif
    return {};
}
void unlockNative(NativeFile file) noexcept {
#if defined(_WIN32)
    OVERLAPPED range{};range.OffsetHigh=0x10000000u;(void)UnlockFileEx(file,0,1,0,&range);
#else
    (void)::flock(file,LOCK_UN);
#endif
}
struct NativeOwner {
    NativeFile value=invalidFile;
    ~NativeOwner(){closeNative(value);}
};
std::optional<FileRevision> revisionOf(NativeFile file, std::error_code& error) noexcept {
#if defined(_WIN32)
    return windowsRevisionOf(file, error);
#else
    struct stat info{};
    if (::fstat(file, &info) != 0) {
        error = nativeError();
        return {};
    }
    return statRevision(info, error);
#endif
}
bool cacheableRevision(NativeFile file, const FileRevision& revision) noexcept {
    // Storage with missing, coarse or nonlocal change times cannot vouch for
    // a digest after an in-place edit that restores the modified time.
#if defined(_WIN32)
    if (!revision.changed[0]) return false;
    std::array<wchar_t, 32> type{};
    if (!GetVolumeInformationByHandleW(file, nullptr, 0, nullptr, nullptr, nullptr,
                                       type.data(), static_cast<DWORD>(type.size()))) return false;
    const std::wstring_view name(type.data());
    return name == L"NTFS" || name == L"ReFS";
#elif defined(__APPLE__)
    struct statfs volume{};
    if (!revision.changed[1] || ::fstatfs(file, &volume) != 0 || !(volume.f_flags & MNT_LOCAL)) return false;
    const std::string_view name(volume.f_fstypename);
    return name == "apfs" || name == "hfs";
#elif defined(__linux__)
    struct statfs volume{};
    if (!revision.changed[1] || ::fstatfs(file, &volume) != 0) return false;
    const auto type = static_cast<std::uint32_t>(volume.f_type);
    return type == EXT4_SUPER_MAGIC || type == XFS_SUPER_MAGIC || type == BTRFS_SUPER_MAGIC ||
           type == OVERLAYFS_SUPER_MAGIC || type == TMPFS_MAGIC;
#else
    (void)file;
    (void)revision;
    return false;
#endif
}
std::ptrdiff_t readDigestChunk(void* context, std::span<char> buffer) noexcept {
    const auto file = *static_cast<const NativeFile*>(context);
#if defined(_WIN32)
    DWORD bytes = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) return -1;
    return static_cast<std::ptrdiff_t>(bytes);
#else
    ssize_t bytes;
    do { bytes = ::read(file, buffer.data(), buffer.size()); } while (bytes < 0 && errno == EINTR);
    return bytes;
#endif
}
std::error_code syncStaged(const fs::path& path) noexcept {
    NativeOwner file;
#if defined(_WIN32)
    file.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.value==invalidFile)return nativeError();
    if(!FlushFileBuffers(file.value))return nativeError();
#else
    file.value=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if(file.value==invalidFile)return nativeError();
#if defined(__APPLE__)
    int result;
    do {result=::fcntl(file.value,F_FULLFSYNC);}while(result!=0 && errno==EINTR);
#else
    int result;
    do {result=::fsync(file.value);}while(result!=0 && errno==EINTR);
#endif
    if(result!=0)return nativeError();
#endif
    return {};
}
std::error_code syncFolder(const fs::path& path) noexcept {
    NativeOwner directory;
#if defined(_WIN32)
    directory.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(directory.value==invalidFile)return nativeError();
    if(!FlushFileBuffers(directory.value))return nativeError();
#else
    directory.value=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory.value==invalidFile)return nativeError();
    int result;do {result=::fsync(directory.value);}while(result!=0 && errno==EINTR);
    if(result!=0)return nativeError();
#endif
    return {};
}
bool transientRename(const std::error_code& error) noexcept {
#if defined(_WIN32)
    return error.category()==std::system_category() &&
        (error.value()==ERROR_ACCESS_DENIED || error.value()==ERROR_SHARING_VIOLATION || error.value()==ERROR_LOCK_VIOLATION);
#else
    (void)error;return false;
#endif
}
bool unsupportedRename(const std::error_code& error) noexcept {
    if (error == std::errc::operation_not_supported || error == std::errc::invalid_argument ||
        error == std::errc::function_not_supported) return true;
#if defined(_WIN32)
    return error.category() == std::system_category() &&
        (error.value() == ERROR_NOT_SUPPORTED || error.value() == ERROR_INVALID_FUNCTION ||
         error.value() == ERROR_INVALID_PARAMETER);
#else
    return false;
#endif
}
std::error_code classicReplace(const fs::path& source, const fs::path& target) noexcept {
#if defined(_WIN32)
    if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {};
    return {static_cast<int>(GetLastError()), std::system_category()};
#else
    std::error_code error;
    fs::rename(source, target, error);
    return error;
#endif
}
PublishResult publishedResult(const fs::path& target,const FileIdentity& identity,
                              std::error_code (*sync)(const fs::path&)=nullptr) {
    PublishResult result{PublishStatus::published,false,{}};result.identity=identity;
    try {
        const auto folder=target.has_parent_path()?target.parent_path():fs::current_path();
        const auto error=sync?sync(folder):syncFolder(folder);
        result.durabilityConfirmed=!error;
        if(error)result.warning="file published; directory sync could not be confirmed: "+error.message();
    }catch(...){/* Publication already happened: never report an unchanged failure. */}
    return result;
}
PublishResult portableResult(const fs::path& target, const FileIdentity& identity,
                             const ReplaceHooks& hooks, std::string warning = {}) {
    auto result = publishedResult(target, identity, hooks.syncDirectory);
    result.usedFallback = true;
    if (!warning.empty()) {
        try {
            if (!result.warning.empty()) warning += "\n" + result.warning;
            result.warning = std::move(warning);
        } catch (...) {}
    }
    return result;
}
}
std::error_code renameReplacing(const fs::path& source,const fs::path& target) noexcept {
#if defined(_WIN32)
    // replaceFile renames while it holds the destination's exclusive lease
    // handle. A classic replacing rename (MoveFileExW, std::filesystem::rename)
    // refuses any target with an open handle, even one sharing delete, so it
    // can never succeed under that lease. POSIX semantics replace a target
    // whose every open handle shares delete -- our lease -- and still refuse
    // one held by SQLite or a Store's shared lease, which do not.
    try {
        NativeOwner file;
        file.value=CreateFileW(source.c_str(),DELETE|SYNCHRONIZE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file.value==invalidFile)return nativeError();
        const auto absolute=fs::absolute(target);
        const std::wstring& name=absolute.native();
        const auto bytes=name.size()*sizeof(wchar_t);
        if(bytes>0xFFFFFFF0u)return std::make_error_code(std::errc::filename_too_long);
        // FILE_RENAME_INFO with its Flags member, laid out by hand so the
        // build does not depend on the SDK's _WIN32_WINNT-gated union.
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+bytes);
        auto* info=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        const DWORD flags=0x1u /*FILE_RENAME_FLAG_REPLACE_IF_EXISTS*/|0x2u /*FILE_RENAME_FLAG_POSIX_SEMANTICS*/;
        std::memcpy(buffer.data(),&flags,sizeof flags);
        info->RootDirectory=nullptr;
        info->FileNameLength=static_cast<DWORD>(bytes);
        std::memcpy(info->FileName,name.c_str(),bytes);
        constexpr auto fileRenameInfoEx=static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
        if(SetFileInformationByHandle(file.value,fileRenameInfoEx,info,static_cast<DWORD>(buffer.size())))return {};
        const auto error=nativeError();
        // Before Windows 10 1709, or on a volume without POSIX rename (SMB
        // shares, FAT/exFAT): the classic rename is no way out, since it
        // refuses the destination replaceFile's own lease holds open. Report
        // that once, not as a transient refusal to retry; the old bytes and
        // the stage stay as they were.
        if(error.value()==ERROR_INVALID_PARAMETER || error.value()==ERROR_NOT_SUPPORTED ||
           error.value()==ERROR_INVALID_FUNCTION)return std::make_error_code(std::errc::operation_not_supported);
        return error;
    }catch(...){return std::make_error_code(std::errc::not_enough_memory);}
#else
    std::error_code error;fs::rename(source,target,error);return error;
#endif
}
std::optional<FileIdentity> readFileIdentity(const fs::path& path,std::error_code& error) noexcept {
    if(audio::onAudioThread){error=std::make_error_code(std::errc::operation_not_permitted);return {};}
    return identityAt(path,error);
}
std::optional<FileRevision> readFileRevision(const fs::path& path,std::error_code& error) noexcept {
    if(audio::onAudioThread){error=std::make_error_code(std::errc::operation_not_permitted);return {};}
#if defined(_WIN32)
    NativeOwner file;
    file.value=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file.value==invalidFile){error=nativeError();return {};}
    return revisionOf(file.value, error);
#else
    struct stat info{};
    if(::lstat(path.c_str(),&info)!=0){error=nativeError();return {};}
    return statRevision(info, error);
#endif
}
FileRevisionDigest blake3FileWithRevision(const fs::path& path, std::stop_token stop) noexcept {
    if (audio::onAudioThread) return {{{}, HashError::open}, std::nullopt};
    if (stop.stop_requested()) return {{{}, HashError::cancelled}, std::nullopt};
    std::error_code error;
    const auto initial = readFileRevision(path, error);
    if (!initial) return {blake3File(path, stop), std::nullopt};

    NativeOwner file;
#if defined(_WIN32)
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
#else
    file.value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
#endif
    if (file.value == invalidFile) return {{{}, HashError::open}, std::nullopt};
    const auto before = revisionOf(file.value, error);
    if (before != initial) return {{{}, HashError::read}, std::nullopt};
    auto digest = detail::blake3Read(&file.value, readDigestChunk, stop);
    if (!digest) return {std::move(digest), std::nullopt};
    const bool cacheable = cacheableRevision(file.value, *before);
    if (revisionOf(file.value, error) != before || readFileRevision(path, error) != before)
        return {{{}, HashError::read}, std::nullopt};
    return {std::move(digest), cacheable ? before : std::nullopt};
}
struct ProjectFileLease::Impl {
    NativeFile file=invalidFile;
    fs::path path;
    FileIdentity id;
    bool created=false;
    bool exclusive=false;
    ~Impl(){closeNative(file);}
};
ProjectFileLease::ProjectFileLease(std::unique_ptr<Impl> value):impl_(std::move(value)){}
ProjectFileLease::~ProjectFileLease()=default;
std::shared_ptr<ProjectFileLease> ProjectFileLease::open(const fs::path& path,bool create,bool exclusive,std::error_code& error) noexcept {
    if(audio::onAudioThread){error=std::make_error_code(std::errc::operation_not_permitted);return {};}
    try {
        auto value=std::make_unique<Impl>();value->path=fs::absolute(path).lexically_normal();
#if defined(_WIN32)
        const DWORD sharing=FILE_SHARE_READ|FILE_SHARE_WRITE|(exclusive?FILE_SHARE_DELETE:0u);
        value->file=CreateFileW(value->path.c_str(),create?GENERIC_READ|GENERIC_WRITE:GENERIC_READ,
            sharing,nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(value->file==invalidFile){error=nativeError();return {};}
        const auto identity=identityOf(value->file,error);if(!identity)return {};
#else
        NativeOwner createdProject;
        std::optional<FileIdentity> identity;
        if(create){
            createdProject.value=::open(value->path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0666);
            if(createdProject.value==invalidFile){error=nativeError();return {};}
            identity=identityOf(createdProject.value,error);
        }else identity=identityAt(value->path,error);
        if(!identity)return {};
        value->file=openLeaseInode(*identity,error);
        if(value->file==invalidFile){
            // The lease folder is unusable (wrong owner or mode, /tmp full or
            // read-only, out of descriptors). Nobody has adopted the empty
            // file we just created -- no lease was ever possible -- so take it
            // back while the name still names it; otherwise a retry at this
            // path would fail as AlreadyExists. A lock refused later is
            // handled below (DF-E12).
            struct stat info{};
            if(create && ::stat(value->path.c_str(),&info)==0 &&
               FileIdentity{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)}==*identity)
                (void)::unlink(value->path.c_str());
            return {};
        }
#endif
        error=lockNative(value->file,exclusive);
#if !defined(_WIN32)
        if(error && create) {
            // DF-E12 (a): a lease is keyed on the inode's number, and a file just made with O_EXCL is a new inode, so
            // no lease on it can be rightful yet. A lock refused there belongs to the file that number named before:
            // ext4 hands a freed inode number to the next new file at once, and a replace keeps the lease of the
            // inode its rename freed until its folder sync returns. Wait that out, for a bound.
            using namespace std::chrono_literals;
            auto pause=2ms;
            const auto until=std::chrono::steady_clock::now()+2s;
            while(error && (error==std::errc::resource_unavailable_try_again || error==std::errc::operation_would_block) &&
                  std::chrono::steady_clock::now()<until) {
                std::this_thread::sleep_for(pause);
                pause=std::min(pause*2,std::chrono::milliseconds(50));
                error=lockNative(value->file,exclusive);
            }
        }
        if(error) {
            // DF-E12 (b): no lease was taken on the empty file we created. Where the lock failed for any reason but a
            // holder, take the file back while the name still names it, as when the lease folder is unusable; a retry
            // at this path would otherwise fail as AlreadyExists. A lease still held after the wait above is no stale
            // one (a replace now lets go of a freed inode's lease at its rename): it is another process's replace
            // that has adopted our new file, and the file is left to it.
            const bool held=error==std::errc::resource_unavailable_try_again || error==std::errc::operation_would_block;
            struct stat info{};
            if(create && !held && ::stat(value->path.c_str(),&info)==0 &&
               FileIdentity{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)}==*identity)
                (void)::unlink(value->path.c_str());
            return {};
        }
#else
        if(error)return {};
#endif
        // A replacement that won the open-to-lock interval must not redirect
        // this Store onto a different inode after we acquired the old one.
        const auto current=identityAt(value->path,error);
        if(!current || *current!=*identity){if(!error)error=std::make_error_code(std::errc::state_not_recoverable);return {};}
        value->id=*identity;value->created=create;value->exclusive=exclusive;error.clear();
        return std::shared_ptr<ProjectFileLease>(new ProjectFileLease(std::move(value)));
    }catch(const fs::filesystem_error& exception){error=exception.code();return {};}
    catch(...){error=std::make_error_code(std::errc::not_enough_memory);return {};}
}
std::shared_ptr<ProjectFileLease> ProjectFileLease::openShared(const fs::path& path,std::error_code& error) noexcept {return open(path,false,false,error);}
std::shared_ptr<ProjectFileLease> ProjectFileLease::createShared(const fs::path& path,std::error_code& error) noexcept {return open(path,true,false,error);}
std::shared_ptr<ProjectFileLease> ProjectFileLease::openExclusive(const fs::path& path,std::error_code& error) noexcept {return open(path,false,true,error);}
FileIdentity ProjectFileLease::identity()const noexcept{return impl_->id;}
const fs::path& ProjectFileLease::path()const noexcept{return impl_->path;}
bool ProjectFileLease::removeExclusive(const FileRevision& expected, std::error_code& error,
                                       const std::function<std::error_code()>& validate) noexcept {
    if(audio::onAudioThread || !impl_->exclusive || impl_->file==invalidFile){
        error=std::make_error_code(std::errc::operation_not_permitted);
        return false;
    }
    try {
#if defined(_WIN32)
        // The existing lease shares delete. A second handle pins the name and excludes writers
        // and competing delete handles until this file has been marked for deletion.
        NativeOwner removable;
        removable.value=CreateFileW(impl_->path.c_str(),DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,
            nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(removable.value==invalidFile){error=nativeError();return false;}
#endif
        if(validate && (error=validate()))return false;
#if defined(_WIN32)
        const auto current=windowsRevisionOf(removable.value,error);
#else
        const auto current=readFileRevision(impl_->path,error);
#endif
        if(current!=expected || expected.identity!=impl_->id){
            if(!error)error=std::make_error_code(std::errc::state_not_recoverable);
            return false;
        }
#if defined(_WIN32)
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile=TRUE;
        if(!SetFileInformationByHandle(removable.value,FileDispositionInfo,&disposition,sizeof(disposition))){
            error=nativeError();
            return false;
        }
        closeNative(impl_->file);
        impl_->file=invalidFile;
        return true;
#else
        return fs::remove(impl_->path,error);
#endif
    }catch(const fs::filesystem_error& exception){error=exception.code();return false;}
    catch(...){error=std::make_error_code(std::errc::io_error);return false;}
}
bool ProjectFileLease::removeCreated(std::error_code& error,
                                     const std::function<std::error_code()>& validate) noexcept {
    if(audio::onAudioThread || !impl_->created){error=std::make_error_code(std::errc::operation_not_permitted);return false;}
    try {
        // A second real reader can have opened the fully seeded file before a
        // later factory failure. Never remove that reader's active project.
        unlockNative(impl_->file);
        if((error=lockNative(impl_->file,true)))return false;
        const auto current=identityAt(impl_->path,error);
        if(!current || *current!=impl_->id){if(!error)error=std::make_error_code(std::errc::state_not_recoverable);return false;}
#if defined(_WIN32)
        NativeOwner removable;
        removable.value=CreateFileW(impl_->path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(removable.value==invalidFile){error=nativeError();return false;}
        unlockNative(impl_->file);
        if((error=lockNative(removable.value,true)))return false;
#endif
        if(validate && (error=validate()))return false;
#if defined(_WIN32)
        closeNative(impl_->file);impl_->file=invalidFile;
#endif
        const bool removed=fs::remove(impl_->path,error);return removed && !error;
    }catch(const fs::filesystem_error& exception){error=exception.code();return false;}
    catch(...){error=std::make_error_code(std::errc::io_error);return false;}
}
PublishResult replaceFile(const fs::path& selectedSource,const fs::path& selectedTarget,
                          ReplaceHooks hooks, ReplacePolicy policy) noexcept {
    if(audio::onAudioThread)return {};
    try {
        const auto source=fs::absolute(selectedSource).lexically_normal();
        const auto target=fs::absolute(selectedTarget).lexically_normal();
        std::error_code error;
        const auto sourceStatus=fs::symlink_status(source,error);
        if(error || !fs::is_regular_file(sourceStatus))return {PublishStatus::failed,false,"staging file is not a regular closed file"};
        const auto sourceLease=ProjectFileLease::openExclusive(source,error);
        if(!sourceLease)return {PublishStatus::failed,false,"staging file is open or inaccessible: "+error.message()};
        const auto sourceId=std::optional<FileIdentity>(sourceLease->identity());
        const auto syncError=hooks.syncFile?hooks.syncFile(source):syncStaged(source);
        if(syncError)return {PublishStatus::failed,false,"sync staging file: "+syncError.message()};
        for(unsigned selection=0;selection<8;++selection){
            error.clear();const auto status=fs::symlink_status(target,error);
            const bool absent=error==std::errc::no_such_file_or_directory || (!error && status.type()==fs::file_type::not_found);
            if(error && !absent)return {PublishStatus::failed,false,error.message()};
            if(absent && hooks.expectedTarget)return {PublishStatus::failed,false,"expected replacement destination is missing"};
            std::shared_ptr<ProjectFileLease> lease;
            if(!absent){
                if(!fs::is_regular_file(status))return {PublishStatus::failed,false,"destination is not a regular file"};
                lease=ProjectFileLease::openExclusive(target,error);
                if(!lease)return {PublishStatus::failed,false,"destination is in use or inaccessible: "+error.message()};
                if(hooks.expectedTarget && lease->identity()!=*hooks.expectedTarget)
                    return {PublishStatus::failed,false,"destination identity changed since preflight"};
                if(lease->identity()==*sourceId)return {PublishStatus::failed,false,"source and destination identify the same file"};
            }
            if(hooks.validateTarget && (error=hooks.validateTarget(target)))return {PublishStatus::failed,false,"destination validation: "+error.message()};
            HashResult previous;
            if (!absent && policy == ReplacePolicy::PortableSettings) {
                previous = blake3File(target);
                if (!previous) return {PublishStatus::failed, false, "Cannot read the previous settings for portable replacement"};
            }
            if(absent){
                error=hooks.rename?hooks.rename(source,target):renameExclusive(source,target);
                if(error==std::errc::file_exists)continue;
            }else{
                for(unsigned attempt=0;;++attempt){
                    if(hooks.expectedTarget) {
                        const auto currentStatus=fs::symlink_status(target,error);
                        if(error || !fs::is_regular_file(currentStatus))
                            return {PublishStatus::failed,false,"replacement destination is no longer an ordinary file"};
                        if(attempt>0 && hooks.validateTarget && (error=hooks.validateTarget(target)))
                            return {PublishStatus::failed,false,"destination validation: "+error.message()};
                    }
                    const auto current=identityAt(target,error);
                    if(!current || *current!=lease->identity())return {PublishStatus::failed,false,"destination changed during replacement"};
                    error=hooks.rename?hooks.rename(source,target):renameReplacing(source,target);
                    const bool retry=error && (hooks.retryable?hooks.retryable(error):transientRename(error));
                    if(!retry || attempt==7)break;
                    const unsigned delay=std::min(200u,10u<<attempt);
                    if(hooks.retryWait)hooks.retryWait(delay);else std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<std::int64_t>(delay)));
                }
            }
            if (error && unsupportedRename(error) && policy != ReplacePolicy::LeaseOnly) {
                const auto currentStage = identityAt(source, error);
                if (!currentStage || *currentStage != *sourceId)
                    return {PublishStatus::failed, false, "Staging file changed before portable publication"};
                if (absent) {
                    fs::create_hard_link(source, target, error);
                    if (!error) {
                        std::error_code ignored;
                        fs::remove(source, ignored);
                        return portableResult(target, *sourceId, hooks);
                    }
                    if (error == std::errc::file_exists)
                        return {PublishStatus::exists, true, "Destination appeared before portable publication"};
                    if (policy == ReplacePolicy::PortableBundle || !unsupportedRename(error))
                        return {PublishStatus::failed, true, "This folder cannot publish an atomic save. Choose another folder. " + error.message()};
                    if (hooks.validateFallback && (error = hooks.validateFallback(target)))
                        return {PublishStatus::failed, true, "Destination validation: " + error.message()};
                    error.clear();
                    const auto current = fs::symlink_status(target, error);
                    const bool stillAbsent = error == std::errc::no_such_file_or_directory ||
                                             (!error && current.type() == fs::file_type::not_found);
                    if (!stillAbsent) return {PublishStatus::failed, true, "Destination changed before portable publication"};
                    std::string warning = "Saved with portable rename. Destination validation and publication were separate.";
#if defined(_WIN32)
                    error = renameExclusive(source, target);
#else
                    error = classicReplace(source, target);
#endif
                    if (error) return {PublishStatus::failed, true, "Portable publication: " + error.message()};
                    return portableResult(target, *sourceId, hooks, std::move(warning));
                }
                if (policy == ReplacePolicy::PortableSettings) {
                    const auto expected = lease->identity();
                    lease.reset();
                    if (hooks.validateFallback && (error = hooks.validateFallback(target)))
                        return {PublishStatus::failed, true, "Destination validation: " + error.message()};
                    if (!fs::is_regular_file(fs::symlink_status(target, error)))
                        return {PublishStatus::failed, true, "Destination changed before portable replacement"};
                    const auto current = identityAt(target, error);
                    if (!current || *current != expected)
                        return {PublishStatus::failed, true, "Destination changed before portable replacement"};
                    const auto after = blake3File(target);
                    if (!after || after.hex != previous.hex)
                        return {PublishStatus::failed, true, "Destination bytes changed before portable replacement"};
                    std::string warning = "Saved with portable rename. Destination validation and publication were separate.";
                    error = classicReplace(source, target);
                    if (error) return {PublishStatus::failed, true, "Portable replacement: " + error.message()};
                    return portableResult(target, *sourceId, hooks, std::move(warning));
                }
                return {PublishStatus::failed, false, "This volume cannot replace the bundle atomically. Choose a new name."};
            }
            if(error==std::errc::operation_not_supported)
                return {PublishStatus::failed,false,"replace file: this volume cannot replace a file in place; choose a new name"};
            if(error)return {PublishStatus::failed,false,"replace file: "+error.message()};
            // DF-E12: the rename freed the old target's inode, so its lease guards nothing now. Holding it through
            // the folder sync below (of no fixed length) would refuse a new file given that inode number meanwhile.
            lease.reset();
            return publishedResult(target,*sourceId,hooks.syncDirectory);
        }
        return {PublishStatus::failed,false,"destination kept changing during replacement"};
    }catch(const std::exception& exception){return {PublishStatus::failed,false,exception.what()};}
}

std::error_code replaceCacheFile(const fs::path& source,const fs::path& target) noexcept {
    return classicReplace(source, target);
}
std::error_code syncPublishedDirectory(const fs::path& path) noexcept {
    if(audio::onAudioThread)return std::make_error_code(std::errc::operation_not_permitted);
    return syncFolder(path);
}
std::error_code removePublishedFile(const fs::path& path,const FileIdentity& expected) noexcept {
    if(audio::onAudioThread)return std::make_error_code(std::errc::operation_not_permitted);
#if defined(_WIN32)
    NativeOwner file;
    file.value=CreateFileW(path.c_str(),DELETE|FILE_READ_ATTRIBUTES,0,nullptr,OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file.value==invalidFile) {
        const auto error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return {};
        return {static_cast<int>(error),std::system_category()};
    }
    std::error_code identityError;
    const auto current=windowsIdentityOf(file.value,identityError,true);
    if(!current)return identityError;
    if(*current!=expected)return std::make_error_code(std::errc::state_not_recoverable);
    FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
    if(!SetFileInformationByHandle(file.value,FileDispositionInfo,&disposition,sizeof(disposition)))return nativeError();
    if(!CloseHandle(file.value))return nativeError();
    file.value=invalidFile;return {};
#else
    (void)path;(void)expected;
    return std::make_error_code(std::errc::operation_not_supported);
#endif
}
PublishResult publishFile(const fs::path& source, const fs::path& target, PublishHooks hooks,PublishPolicy policy) noexcept {
    if(audio::onAudioThread)return {};
    bool copying = false;
    try {
        std::error_code identityError;
        const auto sourceStatus=fs::symlink_status(source,identityError);
        if(identityError || !fs::is_regular_file(sourceStatus))
            return {PublishStatus::failed,false,"staging file is not a regular closed file"};
        auto sourceLease=ProjectFileLease::openExclusive(source,identityError);
        if(!sourceLease)return {PublishStatus::failed,false,"staging file is open or inaccessible: "+identityError.message()};
        const auto identity=std::optional<FileIdentity>(sourceLease->identity());
        const auto sameStage=[&] {
            identityError.clear();
            const auto status=fs::symlink_status(source,identityError);
            if(identityError || !fs::is_regular_file(status))return false;
            const auto current=identityAt(source,identityError);
            return current && *current==*identity;
        };
        unsigned retries=0;
        const auto retry=[&](const std::error_code& error) {
            if(!transientRename(error) || retries==7)return false;
            const unsigned delay=std::min(200u,10u<<retries++);
            if(hooks.retryWait)hooks.retryWait(delay);
            else std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<std::int64_t>(delay)));
            return true;
        };
        std::error_code synced;
        do {
            if(!sameStage())return {PublishStatus::failed,false,"staging file changed before sync"};
            synced=hooks.syncFile?hooks.syncFile(source):syncStaged(source);
        }while(synced && retry(synced));
        if(synced)return {PublishStatus::failed,false,"sync staging file: "+synced.message()};
        std::error_code error;
        do {
            if(!sameStage())return {PublishStatus::failed,false,"staging file changed before publication"};
            error=hooks.rename?hooks.rename(source,target):renameExclusive(source,target);
        }while(error && retry(error));
        if (!error) return publishedResult(target,*identity,hooks.syncDirectory);
        if (error == std::errc::file_exists) return {PublishStatus::exists, false, error.message()};
        if(policy==PublishPolicy::AtomicOnly)return {PublishStatus::failed,false,"atomic publication: "+error.message()};
        if(!sameStage())return {PublishStatus::failed,false,"staging file changed before copy fallback"};
        copying = true;
        const auto expected = blake3File(source);
        if (!expected) return {PublishStatus::failed, true, "cannot hash staging file"};
        const auto parent = target.has_parent_path() ? target.parent_path() : fs::current_path();
        CopyStage copy(parent);
        std::error_code createError;
        ExclusiveFile out(copy.path, createError);
        if (createError) return {PublishStatus::failed, true, createError.message()};
        copy.createdCopy = true;
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
        if (hooks.afterCopy) hooks.afterCopy(copy.path);
        const auto actual = blake3File(copy.path);
        if (!actual || actual.hex != expected.hex) throw std::runtime_error("published copy hash mismatch");
        const auto copyIdentity = identityAt(copy.path, identityError);
        if (!copyIdentity) throw std::runtime_error("verified copy identity unavailable: " + identityError.message());
        // The name is claimed exclusively in one filesystem operation. A file
        // created by another writer while we copied stays wholly that writer's.
        std::error_code linkError;
        fs::create_hard_link(copy.path, target, linkError);
        if (linkError == std::errc::file_exists) return {PublishStatus::exists, true, linkError.message()};
        if (linkError) return {PublishStatus::failed, true, "publish verified copy: " + linkError.message()};
        in.close();
        // Failure to remove a private staging name does not invalidate the
        // verified destination; its owning staging directory cleans it later.
        std::error_code ignored; fs::remove(source, ignored);
        sourceLease.reset(); // DF-E12: the staging inode is gone; its lease must not outlive it through the sync
        auto result = publishedResult(target, *copyIdentity, hooks.syncDirectory);
        result.usedFallback = true;
        return result;
    } catch (const std::exception& e) { return {PublishStatus::failed, copying, e.what()}; }
}
}
