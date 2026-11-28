// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/recording_recovery_wave.hpp"
#include "adi/audio/io_audit.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace adi::engine::recording_recovery {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t sentinel = 0xffffffffULL;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void ownerOnly() {
    require(!audio::onAudioThread, "recording recovery requires an owner worker");
    audio::fileIoPoint();
}
[[noreturn]] void ioError(const char* message) {
#if defined(_WIN32)
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
#else
    throw std::system_error(errno, std::generic_category(), message);
#endif
}
#if defined(_WIN32)
using Native = HANDLE;
const Native invalid = INVALID_HANDLE_VALUE;
void closeNative(Native file) noexcept { if (file != invalid) (void)CloseHandle(file); }
#else
using Native = int;
constexpr Native invalid = -1;
void closeNative(Native file) noexcept { if (file != invalid) (void)::close(file); }
#endif
struct Handle {
    Native value = invalid;
    explicit Handle(Native file = invalid) noexcept : value(file) {}
    ~Handle() { closeNative(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, invalid)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { closeNative(value); value = std::exchange(other.value, invalid); }
        return *this;
    }
};
void checkPath(const fs::path& path) {
    require(path.is_absolute() && path.has_filename(), "recovery requires an absolute file path");
    require(path.native().find(fs::path::value_type{}) == fs::path::string_type::npos,
            "NUL in recovery path");
#if defined(_WIN32)
    const auto root = path.root_name().native();
    require(root.size() == 2 && root[1] == L':' &&
            ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')),
            "recovery requires an ordinary drive-rooted Windows path");
#endif
    for (const auto& component : path.relative_path()) {
        require(component != fs::path{"."} && component != fs::path{".."} && !component.empty(),
                "relative component in recovery path");
#if defined(_WIN32)
        const auto& name = component.native();
        require(name.find(L':') == std::wstring::npos && name.back() != L'.' && name.back() != L' ',
                "ambiguous Windows recovery path");
#endif
    }
}

// Holds every directory on Windows without delete sharing. POSIX uses openat
// from the already opened directory, never a second pathname walk for the file.
class Parents {
    std::vector<Handle> directories_;
public:
    explicit Parents(const fs::path& file) {
        checkPath(file);
#if defined(_WIN32)
        auto path = file.root_path();
        const auto add = [&](const fs::path& directory) {
            Handle handle(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (handle.value == invalid) ioError("open recovery parent");
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle.value, &info)) ioError("inspect recovery parent");
            require((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
                    "recovery parent is not an ordinary directory");
            directories_.push_back(std::move(handle));
        };
        add(path);
        for (const auto& component : file.parent_path().relative_path()) { path /= component; add(path); }
#else
        Handle root(::open(file.root_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        if (root.value == invalid) ioError("open recovery root");
        directories_.push_back(std::move(root));
        for (const auto& component : file.parent_path().relative_path()) {
            Handle next(::openat(directories_.back().value, component.c_str(),
                O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
            if (next.value == invalid) ioError("open recovery parent");
            directories_.push_back(std::move(next));
        }
#endif
    }
    Handle open(const fs::path& file, bool create) const {
#if defined(_WIN32)
        Handle result(CreateFileW(file.c_str(), create ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
#else
        Handle result(::openat(directories_.back().value, file.filename().c_str(),
            (create ? O_RDWR | O_CREAT | O_EXCL : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW, 0600));
#endif
        if (result.value == invalid) ioError(create ? "create recovery stage" : "open recovery source");
        return result;
    }
};

struct FileState {
    media::FileIdentity identity;
    std::uint64_t size = 0;
    std::int64_t modifiedSeconds = 0, changedSeconds = 0;
    std::uint32_t modifiedFraction = 0, changedFraction = 0;
    friend bool operator==(const FileState&, const FileState&) = default;
};
FileState state(Native file, media::FileIdentityScheme scheme) {
    FileState result;
#if defined(_WIN32)
    require(GetFileType(file) == FILE_TYPE_DISK, "recovery requires a disk file");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) ioError("inspect recovery file");
    require((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
            info.nNumberOfLinks == 1, "recovery refuses directories, reparse points and hard links");
    result.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    result.modifiedSeconds = static_cast<std::int64_t>(info.ftLastWriteTime.dwHighDateTime);
    result.modifiedFraction = info.ftLastWriteTime.dwLowDateTime;
    if (scheme == media::FileIdentityScheme::Windows128) {
        FILE_ID_INFO wide{};
        if (!GetFileInformationByHandleEx(file, FileIdInfo, &wide, sizeof(wide))) ioError("read recovery file id");
        const auto half = [&](unsigned begin) {
            std::uint64_t value = 0;
            for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(wide.FileId.Identifier[begin + i]) << (i * 8U);
            return value;
        };
        result.identity = {wide.VolumeSerialNumber, half(0), half(8), scheme};
    } else {
        require(scheme == media::FileIdentityScheme::WindowsLegacy64, "foreign recovery file id scheme");
        result.identity = {info.dwVolumeSerialNumber,
            (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow, 0, scheme};
        require(result.identity.file != 0, "unavailable legacy recovery file id");
    }
#else
    require(scheme == media::FileIdentityScheme::Posix, "foreign recovery file id scheme");
    struct stat info{};
    if (::fstat(file, &info) != 0) ioError("inspect recovery file");
    require(S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0,
            "recovery refuses nonregular or hard-linked files");
    result.identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0, scheme};
    result.size = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
    result.modifiedSeconds = info.st_mtimespec.tv_sec;
    result.modifiedFraction = static_cast<std::uint32_t>(info.st_mtimespec.tv_nsec);
    result.changedSeconds = info.st_ctimespec.tv_sec;
    result.changedFraction = static_cast<std::uint32_t>(info.st_ctimespec.tv_nsec);
#else
    result.modifiedSeconds = info.st_mtim.tv_sec;
    result.modifiedFraction = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
    result.changedSeconds = info.st_ctim.tv_sec;
    result.changedFraction = static_cast<std::uint32_t>(info.st_ctim.tv_nsec);
#endif
#endif
    require(result.size <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
            "recovery file exceeds supported offsets");
    return result;
}
void seek(Native file, std::uint64_t position) {
    require(position <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), "recovery offset overflow");
#if defined(_WIN32)
    LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(position);
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) ioError("seek recovery file");
#else
    require(position <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()), "recovery native offset overflow");
    if (::lseek(file, static_cast<off_t>(position), SEEK_SET) < 0) ioError("seek recovery file");
#endif
}
void transfer(Native file, unsigned char* bytes, std::size_t count, bool write) {
    while (count != 0) {
        const auto part = std::min<std::size_t>(count, 65536);
#if defined(_WIN32)
        DWORD done = 0;
        const auto ok = write ? WriteFile(file, bytes, static_cast<DWORD>(part), &done, nullptr)
                              : ReadFile(file, bytes, static_cast<DWORD>(part), &done, nullptr);
        if (!ok) ioError(write ? "write recovery stage" : "read recovery source");
        const auto amount = static_cast<std::size_t>(done);
#else
        const auto done = write ? ::write(file, bytes, part) : ::read(file, bytes, part);
        if (done < 0) { if (errno == EINTR) continue; ioError(write ? "write recovery stage" : "read recovery source"); }
        const auto amount = static_cast<std::size_t>(done);
#endif
        require(amount != 0 && amount <= count, "short recovery file I/O");
        bytes += amount; count -= amount;
    }
}
using Header = std::array<unsigned char, 116>;
std::uint64_t get(const Header& bytes, std::size_t at, unsigned width) {
    std::uint64_t result = 0;
    for (unsigned i = 0; i < width; ++i) result |= static_cast<std::uint64_t>(bytes[at + i]) << (i * 8U);
    return result;
}
void put(Header& bytes, std::size_t at, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) { bytes[at + i] = static_cast<unsigned char>(value & 255U); value >>= 8; }
}
bool isTag(const Header& bytes, std::size_t at, const char* value) { return std::memcmp(bytes.data() + at, value, 4) == 0; }
void putTag(Header& bytes, std::size_t at, const char* value) { std::memcpy(bytes.data() + at, value, 4); }
unsigned sampleBytes(WaveFormat format) {
    require(format.sampleRate >= 8000 && format.sampleRate <= 768000 && format.channels != 0 && format.channels <= 64,
            "invalid planned recording format");
    switch (format.encoding) {
    case audio::WavFormat::Pcm16: return 2;
    case audio::WavFormat::Pcm24: return 3;
    case audio::WavFormat::Float32: return 4;
    }
    throw std::runtime_error("unsupported planned recording encoding");
}
struct OpenWave {
    Parents parents;
    Handle file;
    FileState initial;
    Header header{};
    WavePrefix prefix;
    unsigned align = 0;
    std::size_t factOffset = 0;
    OpenWave(const fs::path& path, const media::FileIdentity& expected, WaveFormat format)
        : parents(path), file(parents.open(path, false)) {
#if !defined(_WIN32)
        if (::flock(file.value, LOCK_SH | LOCK_NB) != 0) ioError("lock recording source");
#endif
        initial = state(file.value, expected.scheme);
        require(initial.identity == expected, "recording source identity changed");
        const unsigned bytes = sampleBytes(format);
        align = static_cast<unsigned>(format.channels) * bytes;
        const bool floating = format.encoding == audio::WavFormat::Float32;
        const bool extensible = format.channels > 2;
        const unsigned fmtSize = extensible ? 40U : (floating ? 18U : 16U);
        const std::size_t afterFormat = 56U + fmtSize;
        factOffset = floating ? afterFormat + 8U : 0U;
        prefix.dataOffset = afterFormat + (floating ? 12U : 0U) + 8U;
        require(initial.size >= prefix.dataOffset, "truncated recording header");
        transfer(file.value, header.data(), static_cast<std::size_t>(prefix.dataOffset), false);
        require(isTag(header, 0, "RIFF") || isTag(header, 0, "RF64"), "not a recorder RIFF/RF64 file");
        require(isTag(header, 8, "WAVE") && get(header, 16, 4) == 28 &&
                (isTag(header, 12, "JUNK") || isTag(header, 12, "ds64")), "invalid recorder size reservation");
        // The two tags are written separately during promotion. Either known
        // promotion tag retains RF64; partially written/foreign tags refuse.
        prefix.rf64 = isTag(header, 0, "RF64") || isTag(header, 12, "ds64");
        if (isTag(header, 12, "JUNK"))
            require(std::all_of(header.begin() + 20, header.begin() + 48, [](auto byte) { return byte == 0; }),
                    "nonempty recorder JUNK reservation");
        else require(get(header, 44, 4) == 0, "recorder ds64 table must be empty");
        const unsigned encoding = floating ? 3U : 1U;
        require(isTag(header, 48, "fmt ") && get(header, 52, 4) == fmtSize &&
                get(header, 56, 2) == (extensible ? 0xfffeU : encoding) &&
                get(header, 58, 2) == format.channels && get(header, 60, 4) == format.sampleRate &&
                get(header, 64, 4) == static_cast<std::uint64_t>(format.sampleRate) * align &&
                get(header, 68, 2) == align && get(header, 70, 2) == bytes * 8U,
                "recording format differs from its plan");
        if (extensible) {
            constexpr std::array<unsigned char, 12> tail{0, 0, 16, 0, 128, 0, 0, 170, 0, 56, 155, 113};
            require(get(header, 72, 2) == 22 && get(header, 74, 2) == bytes * 8U &&
                    get(header, 76, 4) == 0 && get(header, 80, 4) == encoding &&
                    std::equal(tail.begin(), tail.end(), header.begin() + 84), "invalid recorder extensible format");
        } else if (floating) require(get(header, 72, 2) == 0, "invalid recorder float extension");
        if (floating) require(isTag(header, afterFormat, "fact") && get(header, afterFormat + 4, 4) == 4,
                              "invalid recorder fact chunk");
        require(isTag(header, static_cast<std::size_t>(prefix.dataOffset) - 8U, "data"), "invalid recorder data position");
        prefix.physicalBytes = initial.size;
        const auto payload = initial.size - prefix.dataOffset;
        prefix.frames = payload / align;
        prefix.trailingBytes = payload % align;
        unchanged();
    }
    void unchanged() const { require(state(file.value, initial.identity.scheme) == initial, "recording source changed during recovery"); }
};
} // namespace

WavePrefix inspectWave(const fs::path& source, const media::FileIdentity& expectedIdentity, WaveFormat expectedFormat) {
    ownerOnly();
    return OpenWave(source, expectedIdentity, expectedFormat).prefix;
}

void copyRepaired(const fs::path& source, const media::FileIdentity& expectedIdentity,
                  WaveFormat expectedFormat, const fs::path& destination, std::uint64_t frames, std::stop_token stop) {
    ownerOnly();
    require(!stop.stop_requested(), "recording recovery cancelled");
    OpenWave input(source, expectedIdentity, expectedFormat);
    require(frames <= input.prefix.frames, "recording source is shorter than the requested prefix");
    const auto dataBytes = frames * input.align; // bounded by the opened source length
    const auto outputBytes = input.prefix.dataOffset + dataBytes + (dataBytes & 1U);
    require(outputBytes <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), "repaired recording size overflow");
    const auto riffBytes = outputBytes - 8U;
    const bool rf64 = input.prefix.rf64 || riffBytes >= sentinel || dataBytes >= (std::uint64_t{1} << 32);
    auto header = input.header;
    putTag(header, 0, rf64 ? "RF64" : "RIFF"); put(header, 4, rf64 ? sentinel : riffBytes, 4);
    putTag(header, 12, rf64 ? "ds64" : "JUNK");
    std::fill(header.begin() + 20, header.begin() + 48, 0);
    if (rf64) { put(header, 20, riffBytes, 8); put(header, 28, dataBytes, 8); put(header, 36, frames, 8); }
    put(header, static_cast<std::size_t>(input.prefix.dataOffset) - 4U, rf64 ? sentinel : dataBytes, 4);
    if (input.factOffset != 0) put(header, input.factOffset, std::min(frames, sentinel), 4);
    Parents parents(destination);
    auto output = parents.open(destination, true);
    std::error_code error;
    const auto outputIdentity = media::readFileIdentity(destination, error);
    require(outputIdentity && !error && state(output.value, outputIdentity->scheme).identity == *outputIdentity,
            "cannot bind the private recovery stage");
    transfer(output.value, header.data(), static_cast<std::size_t>(input.prefix.dataOffset), true);
    seek(input.file.value, input.prefix.dataOffset);
    std::array<unsigned char, 65536> buffer{};
    for (std::uint64_t left = dataBytes; left != 0;) {
        require(!stop.stop_requested(), "recording recovery cancelled");
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
        transfer(input.file.value, buffer.data(), count, false);
        transfer(output.value, buffer.data(), count, true);
        left -= count;
    }
    if ((dataBytes & 1U) != 0) { unsigned char pad = 0; transfer(output.value, &pad, 1, true); }
    input.unchanged();
#if defined(_WIN32)
    if (!FlushFileBuffers(output.value)) ioError("flush repaired recording");
#else
    if (::fsync(output.value) != 0) ioError("flush repaired recording");
#endif
    const auto written = state(output.value, outputIdentity->scheme);
    require(written.identity == *outputIdentity && written.size == outputBytes, "repaired recording size/identity changed");
    // Keep the output handle and parent guards alive throughout path-based
    // validation. The destination remains caller-owned, unpublished staging.
    audio::WavReader verify(destination);
    require(verify.frames() == frames && verify.sampleRate() == expectedFormat.sampleRate &&
            verify.channels() == expectedFormat.channels && verify.format() == expectedFormat.encoding,
            "repaired recording failed strict WAV verification");
    const auto identity = media::readFileIdentity(destination, error);
    require(identity && !error && *identity == *outputIdentity, "repaired recording path changed during verification");
    input.unchanged();
}
} // namespace adi::engine::recording_recovery
