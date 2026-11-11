// SPDX-License-Identifier: GPL-3.0-or-later
// All publication platform code lives here (ADR-0109).
#include "publish_file.hpp"
#include "blake3.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
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
#include <sys/file.h>
#include <sys/stat.h>
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
std::optional<FileIdentity> identityOf(NativeFile file,std::error_code& error) noexcept {
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)){error=nativeError();return {};}
    if((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0){error=std::make_error_code(std::errc::is_a_directory);return {};}
    error.clear();return FileIdentity{static_cast<std::uint64_t>(info.dwVolumeSerialNumber),
        (static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow};
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
std::error_code renameReplacing(const fs::path& source,const fs::path& target) noexcept {
#if defined(_WIN32)
    if(MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return {};
    return nativeError();
#else
    std::error_code error;fs::rename(source,target,error);return error;
#endif
}
bool transientRename(const std::error_code& error) noexcept {
#if defined(_WIN32)
    return error.category()==std::system_category() &&
        (error.value()==ERROR_ACCESS_DENIED || error.value()==ERROR_SHARING_VIOLATION || error.value()==ERROR_LOCK_VIOLATION);
#else
    (void)error;return false;
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
}
struct ProjectFileLease::Impl {
    NativeFile file=invalidFile;
    fs::path path;
    FileIdentity id;
    bool created=false;
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
        if(value->file==invalidFile)return {};
#endif
        if((error=lockNative(value->file,exclusive)))return {};
        // A replacement that won the open-to-lock interval must not redirect
        // this Store onto a different inode after we acquired the old one.
        const auto current=identityAt(value->path,error);
        if(!current || *current!=*identity){if(!error)error=std::make_error_code(std::errc::state_not_recoverable);return {};}
        value->id=*identity;value->created=create;error.clear();
        return std::shared_ptr<ProjectFileLease>(new ProjectFileLease(std::move(value)));
    }catch(const fs::filesystem_error& exception){error=exception.code();return {};}
    catch(...){error=std::make_error_code(std::errc::not_enough_memory);return {};}
}
std::shared_ptr<ProjectFileLease> ProjectFileLease::openShared(const fs::path& path,std::error_code& error) noexcept {return open(path,false,false,error);}
std::shared_ptr<ProjectFileLease> ProjectFileLease::createShared(const fs::path& path,std::error_code& error) noexcept {return open(path,true,false,error);}
std::shared_ptr<ProjectFileLease> ProjectFileLease::openExclusive(const fs::path& path,std::error_code& error) noexcept {return open(path,false,true,error);}
FileIdentity ProjectFileLease::identity()const noexcept{return impl_->id;}
const fs::path& ProjectFileLease::path()const noexcept{return impl_->path;}
bool ProjectFileLease::removeCreated(std::error_code& error) noexcept {
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
        closeNative(impl_->file);impl_->file=invalidFile;
#endif
        const bool removed=fs::remove(impl_->path,error);return removed && !error;
    }catch(const fs::filesystem_error& exception){error=exception.code();return false;}
    catch(...){error=std::make_error_code(std::errc::io_error);return false;}
}
PublishResult replaceFile(const fs::path& selectedSource,const fs::path& selectedTarget,ReplaceHooks hooks) noexcept {
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
            std::shared_ptr<ProjectFileLease> lease;
            if(!absent){
                if(!fs::is_regular_file(status))return {PublishStatus::failed,false,"destination is not a regular file"};
                lease=ProjectFileLease::openExclusive(target,error);
                if(!lease)return {PublishStatus::failed,false,"destination is in use or inaccessible: "+error.message()};
                if(lease->identity()==*sourceId)return {PublishStatus::failed,false,"source and destination identify the same file"};
            }
            if(hooks.validateTarget && (error=hooks.validateTarget(target)))return {PublishStatus::failed,false,"destination validation: "+error.message()};
            if(absent){
                error=hooks.rename?hooks.rename(source,target):renameExclusive(source,target);
                if(error==std::errc::file_exists)continue;
            }else{
                for(unsigned attempt=0;;++attempt){
                    const auto current=identityAt(target,error);
                    if(!current || *current!=lease->identity())return {PublishStatus::failed,false,"destination changed during replacement"};
                    error=hooks.rename?hooks.rename(source,target):renameReplacing(source,target);
                    const bool retry=error && (hooks.retryable?hooks.retryable(error):transientRename(error));
                    if(!retry || attempt==7)break;
                    const unsigned delay=std::min(200u,10u<<attempt);
                    if(hooks.retryWait)hooks.retryWait(delay);else std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<std::int64_t>(delay)));
                }
            }
            if(error)return {PublishStatus::failed,false,"replace file: "+error.message()};
            return publishedResult(target,*sourceId,hooks.syncDirectory);
        }
        return {PublishStatus::failed,false,"destination kept changing during replacement"};
    }catch(const std::exception& exception){return {PublishStatus::failed,false,exception.what()};}
}

std::error_code replaceCacheFile(const fs::path& source,const fs::path& target) noexcept {
#if defined(_WIN32)
    if(MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return {};
    return {static_cast<int>(GetLastError()),std::system_category()};
#else
    std::error_code error;fs::rename(source,target,error);return error;
#endif
}
PublishResult publishFile(const fs::path& source, const fs::path& target, PublishHooks hooks,PublishPolicy policy) noexcept {
    if(audio::onAudioThread)return {};
    Cleanup cleanup{target};
    try {
        std::error_code identityError;
        const auto sourceLease=ProjectFileLease::openExclusive(source,identityError);
        if(!sourceLease)return {PublishStatus::failed,false,"staging file is open or inaccessible: "+identityError.message()};
        const auto identity=std::optional<FileIdentity>(sourceLease->identity());
        const auto synced=syncStaged(source);
        if(synced)return {PublishStatus::failed,false,"sync staging file: "+synced.message()};
        const auto error = hooks.rename ? hooks.rename(source, target) : renameExclusive(source, target);
        if (!error) return publishedResult(target,*identity);
        if (error == std::errc::file_exists) return {PublishStatus::exists, false, error.message()};
        if(policy==PublishPolicy::AtomicOnly)return {PublishStatus::failed,false,"atomic publication: "+error.message()};
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
