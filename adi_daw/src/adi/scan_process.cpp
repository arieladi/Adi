// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/scan_process.hpp"
#include "adi/audio/io_audit.hpp"

#include <array>
#include <cerrno>
#include <limits>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace adi::scan {
namespace {
constexpr std::size_t kMaximumOutput = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumArguments = 128U * 1024U;
bool cancelled(const ProcessOptions& options) noexcept {
    return options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed);
}
bool valid(const std::filesystem::path& executable, const std::vector<std::string>& args,
           const ProcessOptions& options) {
    if (!executable.is_absolute() || args.size() > 256 || options.timeout.count() < 1 ||
        options.timeout.count() > 300000 || options.maximumOutput == 0 ||
        options.maximumOutput > kMaximumOutput) return false;
    const auto path = executable.u8string();
    std::size_t bytes = path.size();
    if (bytes > kMaximumArguments || path.find(u8'\0') != path.npos) return false;
    for (const auto& arg : args) {
        if (arg.find('\0') != arg.npos || arg.size() + 1 > kMaximumArguments - bytes)
            return false;
        bytes += arg.size() + 1;
    }
    return true;
}
bool append(ProcessResult& result, const char* bytes, std::size_t size,
            const ProcessOptions& options) {
    if (size > options.maximumOutput - result.output.size()) {
        result.status = ProcessStatus::OutputLimit;
        return false;
    }
    result.output.append(bytes, size);
    return true;
}

#if defined(_WIN32)
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool wide(const std::string& input, std::wstring& output) {
    if (input.empty()) { output.clear(); return true; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                        static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                              static_cast<int>(input.size()), output.data(), count) == count;
}
// CommandLineToArgvW / CRT quoting, including empty arguments and trailing \.
std::wstring quote(const std::wstring& arg) {
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') { ++slashes; continue; }
        out.append(slashes * (ch == L'"' ? 2U : 1U), L'\\');
        slashes = 0;
        if (ch == L'"') out.push_back(L'\\');
        out.push_back(ch);
    }
    out.append(slashes * 2U, L'\\'); out.push_back(L'"');
    return out;
}

ProcessResult launch(const std::filesystem::path& executable,
                     const std::vector<std::string>& arguments, const ProcessOptions& options) {
    ProcessResult result;
    result.status = ProcessStatus::LaunchFailed;
    std::wstring command = quote(executable.native());
    for (const auto& arg : arguments) {
        std::wstring value;
        if (!wide(arg, value)) { result.status = ProcessStatus::Invalid; return result; }
        command += L' '; command += quote(value);
    }
    if (command.size() >= 32767) { result.status = ProcessStatus::Invalid; return result; }
    Handle reader, writer, null, job, process, thread;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    auto failed = [&]() { result.osError = GetLastError(); return result; };
    if (!CreatePipe(&reader.value, &writer.value, &security, 0) ||
        !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) return failed();
    null.value = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (null.value == INVALID_HANDLE_VALUE) return failed();
    job.value = CreateJobObjectW(nullptr, nullptr);
    if (job.value == nullptr) return failed();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits,
                                 sizeof limits)) return failed();
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::max_align_t> storage((attributeBytes + sizeof(std::max_align_t) - 1) /
                                         sizeof(std::max_align_t));
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) return failed();
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } guard{attributes};
    HANDLE inherited[] = {writer.value, null.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof inherited, nullptr, nullptr)) return failed();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null.value;
    startup.StartupInfo.hStdOutput = writer.value;
    startup.StartupInfo.hStdError = null.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &startup.StartupInfo, &info)) return failed();
    process.value = info.hProcess; thread.value = info.hThread;
    if (!AssignProcessToJobObject(job.value, process.value) ||
        ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        result.osError = GetLastError();
        TerminateProcess(process.value, 1); WaitForSingleObject(process.value, INFINITE);
        return result;
    }
    CloseHandle(writer.value); writer.value = nullptr;
    result.status = ProcessStatus::Exited;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD available = 0;
        bool ok = true;
        // Bound each drain so cancellation/deadline cannot starve under output.
        for (unsigned batch = 0; batch < 16; ++batch) {
            if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() != ERROR_BROKEN_PIPE) {
                    result.status = ProcessStatus::IoFailed; result.osError = GetLastError(); ok = false;
                }
                break;
            }
            if (available == 0) break;
            DWORD read = 0;
            const DWORD wanted = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
            if (!ReadFile(reader.value, buffer.data(), wanted, &read, nullptr)) {
                result.status = ProcessStatus::IoFailed; result.osError = GetLastError(); ok = false; break;
            }
            if (!append(result, buffer.data(), read, options)) { ok = false; break; }
        }
        if (!ok) break;
        const DWORD state = WaitForSingleObject(process.value, 0);
        if (state == WAIT_OBJECT_0 && available == 0) {
            DWORD code = 0;
            if (!GetExitCodeProcess(process.value, &code)) {
                result.status = ProcessStatus::IoFailed; result.osError = GetLastError();
            } else if (code >= 0xC0000000UL) result.status = ProcessStatus::Crashed;
            else result.exitCode = code;
            break;
        }
        if (state == WAIT_FAILED) {
            result.status = ProcessStatus::IoFailed; result.osError = GetLastError(); break;
        }
        if (cancelled(options)) { result.status = ProcessStatus::Cancelled; break; }
        if (std::chrono::steady_clock::now() >= deadline) { result.status = ProcessStatus::TimedOut; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    TerminateJobObject(job.value, 1);
    WaitForSingleObject(process.value, INFINITE);
    return result;
}
#else
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) ::close(value); }
};
ProcessResult launch(const std::filesystem::path& executable,
                     const std::vector<std::string>& arguments, const ProcessOptions& options) {
    ProcessResult result;
    result.status = ProcessStatus::LaunchFailed;
    int descriptors[2] = {-1, -1};
#if defined(__linux__)
    if (pipe2(descriptors, O_CLOEXEC) != 0) { result.osError = static_cast<std::uint32_t>(errno); return result; }
#else
    if (pipe(descriptors) != 0) { result.osError = static_cast<std::uint32_t>(errno); return result; }
#endif
    Fd reader{descriptors[0]}, writer{descriptors[1]};
    // The caller may have closed a standard descriptor. Never let the pipe
    // occupy 0/1/2, where the child's redirection actions would overwrite it.
    for (auto* fd : std::array{&reader, &writer}) {
        if (fd->value < 3) {
            const int replacement = fcntl(fd->value, F_DUPFD_CLOEXEC, 3);
            if (replacement < 0) { result.osError = static_cast<std::uint32_t>(errno); return result; }
            ::close(fd->value); fd->value = replacement;
        }
    }
    if (fcntl(reader.value, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(writer.value, F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(reader.value, F_SETFL, O_NONBLOCK) != 0) {
        result.osError = static_cast<std::uint32_t>(errno); return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int error = posix_spawn_file_actions_init(&actions);
    if (error != 0) { result.osError = static_cast<std::uint32_t>(error); return result; }
    struct ActionsGuard {
        posix_spawn_file_actions_t* value;
        ~ActionsGuard() { posix_spawn_file_actions_destroy(value); }
    } actionsGuard{&actions};
    error = posix_spawnattr_init(&attributes);
    if (error != 0) { result.osError = static_cast<std::uint32_t>(error); return result; }
    struct AttributesGuard {
        posix_spawnattr_t* value;
        ~AttributesGuard() { posix_spawnattr_destroy(value); }
    } attributesGuard{&attributes};
    auto add = [&](int next) { if (error == 0) error = next; };
    add(posix_spawn_file_actions_adddup2(&actions, writer.value, STDOUT_FILENO));
    add(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    add(posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
    add(posix_spawn_file_actions_addclose(&actions, reader.value));
    add(posix_spawn_file_actions_addclose(&actions, writer.value));
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
    add(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#endif
#endif
    add(posix_spawnattr_setpgroup(&attributes, 0));
    short flags = POSIX_SPAWN_SETPGROUP;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    add(posix_spawnattr_setflags(&attributes, flags));
    if (error != 0) { result.osError = static_cast<std::uint32_t>(error); return result; }
    const std::string program = executable.string();
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    error = posix_spawn(&pid, program.c_str(), &actions, &attributes, argv.data(), environ);
    if (error != 0) { result.osError = static_cast<std::uint32_t>(error); return result; }
    ::close(writer.value); writer.value = -1;
    // From here every path kills the group and reaps the exact unreaped PID,
    // including C++ allocation failure while collecting output.
    struct ChildGuard {
        pid_t pid;
        ~ChildGuard() {
            ::kill(-pid, SIGKILL);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    } child{pid};
    result.status = ProcessStatus::Exited;
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::array<char, 4096> buffer{};
    for (;;) {
        bool ok = true;
        bool drained = false;
        for (unsigned batch = 0; batch < 16; ++batch) {
            const auto count = ::read(reader.value, buffer.data(), buffer.size());
            if (count > 0) {
                if (!append(result, buffer.data(), static_cast<std::size_t>(count), options)) { ok = false; break; }
            } else if (count == 0 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
                drained = true; break;
            } else if (errno != EINTR) {
                result.status = ProcessStatus::IoFailed; result.osError = static_cast<std::uint32_t>(errno);
                ok = false; break;
            }
        }
        if (!ok) break;
        siginfo_t info{};
        // WNOWAIT keeps the PID reserved until ChildGuard has killed the group.
        if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == EINTR) continue;
            result.status = ProcessStatus::IoFailed; result.osError = static_cast<std::uint32_t>(errno); break;
        }
        if (info.si_pid == pid && drained) {
            if (info.si_code == CLD_EXITED) result.exitCode = info.si_status;
            else { result.status = ProcessStatus::Crashed; result.signal = info.si_status; }
            break;
        }
        if (cancelled(options)) { result.status = ProcessStatus::Cancelled; break; }
        if (std::chrono::steady_clock::now() >= deadline) { result.status = ProcessStatus::TimedOut; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return result;
}
#endif
// The only named object and the only return, so MSVC elides it even at /Od. A
// result moved out of runProcess (several different returns, not elided) would
// cost the callback refusal a second MSVC Debug string-proxy allocation.
ProcessResult refusal(ProcessStatus status) {
    ProcessResult result;
    result.status = status;
    return result;
}
} // namespace

ProcessResult runProcess(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments, const ProcessOptions& options) {
    if (audio::onAudioThread) return refusal(ProcessStatus::CallbackRefused);
    if (!valid(executable, arguments, options)) return {};
    if (cancelled(options)) return refusal(ProcessStatus::Cancelled);
    return launch(executable, arguments, options);
}
} // namespace adi::scan
