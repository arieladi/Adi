// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/settings/store.hpp"
#include "temp_directory.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace adi::settings;
namespace fs = std::filesystem;
constexpr auto app = adi::appdata::App::Daw;
int checks = 0, failures = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes; out.close();
    if (!out) throw std::runtime_error("cannot write owned fixture");
}
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
const std::string good = R"({"schema":1,"app":"ADI","values":{"lookfeel.theme":"light","future.key":"keep"}})";

void absenceAndMalformed() {
    adi::test::TempDirectory scratch("settings_read", "controls");
    const auto file = scratch.path()/"settings.json";
    AppSettings absent(app, file);
    std::string why;
    check(absent.status() == LoadStatus::Defaults && !absent.keptAside(), "genuine absence still means defaults");
    check(absent.set("lookfeel.theme", "light", {}, why) && absent.save(why), "absent settings can be created normally");
    const std::string broken = "{\"schema\":1,broken";
    write(file, broken);
    AppSettings malformed(app, file);
    check(malformed.status() == LoadStatus::CorruptKept && malformed.keptAside() &&
          read(*malformed.keptAside()) == broken && !fs::exists(file), "fully read malformed bytes retain existing recovery behavior");
    write(file, "");
    AppSettings empty(app, file);
    check(empty.status() == LoadStatus::CorruptKept && empty.keptAside() &&
          fs::file_size(*empty.keptAside()) == 0, "a truly empty readable file is still malformed, not an I/O failure");
}

void literalNul() {
    adi::test::TempDirectory scratch("settings_read", "literal_nul");
    const auto file = scratch.path() / "settings.json";
    const auto backup = scratch.path() / "settings.json.bak";
    const std::string joined = good + '\0' + "{}";
    write(file, joined);
    AppSettings malformed(app, file);
    check(malformed.status() == LoadStatus::CorruptKept && malformed.keptAside() &&
              read(*malformed.keptAside()) == joined,
          "settings with a literal NUL and suffix retain the complete malformed bytes");

    write(file, good);
    AppSettings loaded(app, file);
    std::string why;
    check(loaded.set("lookfeel.theme", "dark", {}, why), "a complete valid file loads for ordinary edits");
    write(file, joined);
    check(!loaded.save(why) && read(file) == joined && !fs::exists(backup),
          "save reread refuses a newly malformed document and leaves it intact");
}

void notAFile() {
    adi::test::TempDirectory scratch("settings_read", "not_file");
    const auto file = scratch.path()/"settings.json";
    fs::create_directory(file);
    write(file/"owned.txt", "keep directory contents");
    AppSettings unavailable(app, file);
    check(unavailable.status() == LoadStatus::Unavailable && !unavailable.loadError().empty(),
          "a directory is unavailable, not corrupt bytes or absent settings");
    check(!unavailable.keptAside() && fs::is_directory(file) && read(file/"owned.txt") == "keep directory contents",
          "non-file settings path is never moved aside");
    std::string why;
    check(!unavailable.set("lookfeel.theme", "light", {}, why) && !unavailable.save(why),
          "unavailable constructor state cannot edit or save defaults");
    check(!unavailable.reset("lookfeel.theme", {}, why), "unavailable instance refuses reset");
    const auto* theme = find("lookfeel.theme");
    if (!theme) throw std::runtime_error("missing theme registry fixture");
    unavailable.resetPage(theme->page, {});
    check(unavailable.stored().empty() && !fs::exists(unavailable.changeLogFile()),
          "unavailable reset/page changes do not log or retain fallback edits");
}

#if defined(_WIN32)
class DenyRead {
public:
    explicit DenyRead(const fs::path& file) {
        // Permit rename, but deny every competing data read. This distinguishes
        // healthy inaccessible bytes from the old corrupt-and-rename behavior.
        handle_ = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock owned settings fixture");
    }
    ~DenyRead() { CloseHandle(handle_); }
    DenyRead(const DenyRead&) = delete;
    DenyRead& operator=(const DenyRead&) = delete;
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
class DenyRename {
public:
    explicit DenyRename(const fs::path& file) {
        // Reads and writes remain shareable; only deletion/rename is denied.
        handle_ = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot hold owned recovery fixture");
    }
    ~DenyRename() { CloseHandle(handle_); }
    DenyRename(const DenyRename&) = delete;
    DenyRename& operator=(const DenyRename&) = delete;
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
void recoveryMoveFailure() {
    adi::test::TempDirectory scratch("settings_read", "recovery_move");
    const auto file = scratch.path()/"settings.json";
    auto previous = file; previous += ".corrupt-1";
    auto attempted = file; attempted += ".corrupt-2";
    const std::string broken = "{\"schema\":1,broken\n";
    write(file, broken); write(previous, "prior recovery belongs here");
    std::optional<AppSettings> unavailable;
    {
        DenyRename lock(file);
        check(read(file) == broken, "native no-delete handle still permits reading every malformed byte");
        unavailable.emplace(app, file);
        check(unavailable->status() == LoadStatus::Unavailable && !unavailable->keptAside(),
              "failed recovery move reports unavailable, not a nonexistent kept copy");
        check(unavailable->loadError().find("malformed") != std::string::npos &&
              unavailable->loadError().find("aside") != std::string::npos &&
              unavailable->loadError().find("reload") != std::string::npos,
              "failed recovery diagnostic names malformed recovery and explicit reload");
        check(fs::exists(file) && read(file) == broken && !fs::exists(attempted) &&
              read(previous) == "prior recovery belongs here", "failed rename leaves original bytes and previous recovery unchanged");
        std::string why;
        check(!unavailable->set("lookfeel.theme", "light", {}, why) && !unavailable->reset("lookfeel.theme", {}, why),
              "failed recovery cannot edit or reset fallback defaults");
        check(!unavailable->save(why) && !why.empty() && why == unavailable->loadError() &&
              unavailable->stored().empty() && !fs::exists(unavailable->changeLogFile()),
              "failed recovery refuses save without retaining or logging edits");
    }
    AppSettings recovered(app, file);
    check(recovered.status() == LoadStatus::CorruptKept && recovered.keptAside() == attempted &&
          recovered.loadError().empty() && read(attempted) == broken && !fs::exists(file),
          "fresh load after handle release performs byte-exact recovery at the next free name");
    std::string why;
    check(!unavailable->save(why) && why.find("reload") != std::string::npos && !fs::exists(file),
          "old unavailable instance cannot create defaults even after another instance completes recovery");
    check(recovered.set("lookfeel.theme", "light", {}, why) && recovered.save(why),
          "successfully recovered fresh instance can save normal settings");
    AppSettings loaded(app, file);
    check(loaded.status() == LoadStatus::Loaded && loaded.get("lookfeel.theme") == "light" &&
          read(attempted) == broken && read(previous) == "prior recovery belongs here",
          "saved recovery loads normally and retains both historical byte copies");
}
class LockedTail {
public:
    explicit LockedTail(const fs::path& file) {
        handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open owned range-lock fixture");
        offset_.Offset = 8192;
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset_)) {
            CloseHandle(handle_); throw std::runtime_error("cannot lock owned fixture byte range");
        }
    }
    ~LockedTail() { UnlockFileEx(handle_, 0, 1, 0, &offset_); CloseHandle(handle_); }
    LockedTail(const LockedTail&) = delete;
    LockedTail& operator=(const LockedTail&) = delete;
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED offset_{};
};
void incompleteRead() {
    adi::test::TempDirectory scratch("settings_read", "partial");
    const auto file = scratch.path()/"settings.json";
    // The first chunk is itself valid JSON. EOF and a failed later read must
    // not be confused just because the successfully read prefix can parse.
    const auto bytes = good + std::string(16384, ' ');
    write(file, bytes);
    {
        LockedTail lock(file);
        const HANDLE reader = CreateFileW(file.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reader == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open independent range reader");
        LARGE_INTEGER offset{}; offset.QuadPart = 8192;
        const bool positioned = SetFilePointerEx(reader, offset, nullptr, FILE_BEGIN) != 0;
        char byte{}; DWORD got = 0;
        const bool readOkay = ReadFile(reader, &byte, 1, &got, nullptr) != 0;
        const DWORD error = GetLastError();
        CloseHandle(reader);
        check(positioned && !readOkay && error == ERROR_LOCK_VIOLATION,
              "actual Windows range lock permits opening but refuses the later read");
        AppSettings unavailable(app, file);
        check(unavailable.status() == LoadStatus::Unavailable && !unavailable.keptAside(),
              "valid JSON prefix followed by read failure is not a loaded or corrupt document");
        std::string why;
        check(!unavailable.set("lookfeel.theme", "dark", {}, why) && !unavailable.save(why),
              "partially read constructor cannot edit or save");
    }
    check(read(file) == bytes, "range-lock failure keeps complete original bytes at the same name");
}
void unreadable() {
    adi::test::TempDirectory scratch("settings_read", "denied");
    const auto file = scratch.path()/"settings.json";
    auto corrupt = file; corrupt += ".corrupt-1";
    write(file, good);
    std::optional<AppSettings> unavailable;
    {
        DenyRead lock(file);
        std::ifstream denied(file, std::ios::binary);
        check(!denied, "real Windows sharing mode prevents reading fixture");
        unavailable.emplace(app, file);
        check(unavailable->status() == LoadStatus::Unavailable && !unavailable->loadError().empty(),
              "sharing-denied healthy settings are unavailable, not corrupt or defaults");
        check(!unavailable->keptAside() && fs::exists(file), "unreadable healthy file stays at its original name");
        std::string why;
        check(!unavailable->set("lookfeel.theme", "dark", {}, why) && !unavailable->save(why),
              "sharing-denied instance cannot write or publish fallback defaults");
    }
    check(read(file) == good && !fs::exists(corrupt), "unlock leaves original bytes and no corrupt copy");
    // Repair only the owned test fixture if the pre-fix version moved it.
    if (!fs::exists(file)) write(file, good);
    std::string why;
    check(!unavailable->save(why) && why.find("reload") != std::string::npos,
          "failed-load instance stays protected after lock release; reload is explicit");
    AppSettings reloaded(app, file);
    check(reloaded.status() == LoadStatus::Loaded && reloaded.get("lookfeel.theme") == "light",
          "fresh load after release sees the actual saved value");
    check(reloaded.set("lookfeel.theme", "dark", {}, why) && reloaded.save(why), "successfully reloaded instance can save normally");
}
void saveRetry() {
    adi::test::TempDirectory scratch("settings_read", "save_retry");
    const auto file = scratch.path()/"settings.json";
    auto backup = file; backup += ".bak";
    const auto bytes = good + std::string(16384, ' ');
    write(file, bytes);
    AppSettings loaded(app, file);
    std::string why;
    check(loaded.set("lookfeel.theme", "dark", {}, why), "known-good instance accepts pending edit");
    {
        DenyRead lock(file);
        check(!loaded.save(why) && !why.empty() && loaded.status() == LoadStatus::Loaded,
              "save reread failure refuses publication without invalidating loaded state");
    }
    {
        LockedTail lock(file);
        check(!loaded.save(why) && loaded.status() == LoadStatus::Loaded,
              "late read failure also prevents publication of a valid but incomplete JSON prefix");
    }
    check(read(file) == bytes && !fs::exists(backup), "failed save preserves destination and does not create backup");
    check(loaded.save(why) && loaded.get("lookfeel.theme") == "dark" &&
          loaded.stored().at("future.key") == "keep" && read(backup) == bytes,
          "retry retains pending edit and backs up the original complete bytes");
}
#endif
}

int main() {
    try {
        absenceAndMalformed();
        literalNul();
#if defined(_WIN32)
        unreadable(); incompleteRead(); saveRetry(); recoveryMoveFailure();
#else
        std::printf("Windows sharing-denial regressions not run on this platform\n");
#endif
        notAFile();
    } catch (const std::exception& error) { ++failures; std::printf("EXCEPTION: %s\n", error.what()); }
    std::printf("%s -- settings read failures: %d checks, %d failures\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
