// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/agent_process.hpp"

#include <algorithm>
#include <array>
#include <utility>

#if defined(_WIN32)
#include "adi/control/session_discovery.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace adi::control {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
constexpr Milliseconds kWaitSlice{20};
constexpr auto kMaxWait = std::chrono::minutes{2};
constexpr std::size_t kMaxArgumentBytes = 64 * 1024;

enum class Failure { None, Cancelled, TimedOut, Closed, Pipe };
struct Io {
    std::size_t count = 0;
    Failure failure = Failure::None;
};

const char* failureMessage(Failure failure) {
    switch (failure) {
        case Failure::None: return "";
        case Failure::Cancelled: return "agent helper cancelled";
        case Failure::TimedOut: return "agent helper timed out";
        case Failure::Closed: return "agent helper closed its pipe";
        case Failure::Pipe: return "agent helper pipe failed";
    }
    return "agent helper pipe failed";
}

Failure expired(Clock::time_point deadline, std::stop_token stop) {
    if (stop.stop_requested()) return Failure::Cancelled;
    if (Clock::now() >= deadline) return Failure::TimedOut;
    return Failure::None;
}

Milliseconds waitSlice(Clock::time_point deadline) {
    return std::clamp(std::chrono::ceil<Milliseconds>(deadline - Clock::now()),
                      Milliseconds{1}, kWaitSlice);
}

bool valid(const AgentCommand& command) {
    if (!command.executable.is_absolute() || command.arguments.size() > 128) return false;
    const auto path = command.executable.u8string();
    if (path.find(u8'\0') != path.npos || path.size() >= kMaxArgumentBytes) return false;
    auto size = path.size() + 1;
    for (const auto& argument : command.arguments) {
        if (argument.find('\0') != argument.npos || argument.size() >= kMaxArgumentBytes - size)
            return false;
        size += argument.size() + 1;
    }
    return true;
}

#if defined(_WIN32)
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    [[nodiscard]] bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
    void reset() {
        if (valid()) CloseHandle(value);
        value = nullptr;
    }
    [[nodiscard]] HANDLE release() { return std::exchange(value, nullptr); }
};

// Same CRT quoting as the plug-in scanner. Arguments never go through a shell.
std::wstring quote(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        quoted.append(slashes * (ch == L'"' ? 2u : 1u), L'\\');
        slashes = 0;
        if (ch == L'"') quoted.push_back(L'\\');
        quoted.push_back(ch);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool appendArgument(std::wstring& command, const std::string& argument) {
    std::wstring wide;
    if (!argument.empty()) {
        const auto length = static_cast<int>(argument.size());
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(),
                                              length, nullptr, 0);
        if (count <= 0) return false;
        wide.resize(static_cast<std::size_t>(count));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argument.data(),
                                length, wide.data(), count) != count) return false;
    }
    command.push_back(L' ');
    command += quote(wide);
    return true;
}

// Avoid the modal 16-bit subsystems dialog for a non-PE executable, as the
// scanner does. In particular an executable cannot be a batch script.
bool isPortableExecutable(const std::filesystem::path& executable) {
    Handle file{CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (!file.valid()) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD count = 0;
    if (!ReadFile(file.value, &dos, sizeof(dos), &count, nullptr) || count != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return false;
    LARGE_INTEGER at{};
    at.QuadPart = dos.e_lfanew;
    DWORD signature = 0;
    return SetFilePointerEx(file.value, at, nullptr, FILE_BEGIN) &&
           ReadFile(file.value, &signature, sizeof(signature), &count, nullptr) &&
           count == sizeof(signature) && signature == IMAGE_NT_SIGNATURE;
}

bool makePipe(Handle& parent, Handle& child, bool toChild) {
    const auto token = makeSessionToken();
    if (token.empty()) return false;
    const auto name = L"\\\\.\\pipe\\adi-agent-" + std::wstring(token.begin(), token.end());
    const DWORD direction = toChild ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND;
    const DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    parent.value = CreateNamedPipeW(name.c_str(),
                                    direction | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    mode, 1, 4096, 4096, 0, nullptr);
    if (!parent.valid()) return false;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    child.value = CreateFileW(name.c_str(), toChild ? GENERIC_READ : GENERIC_WRITE,
                              0, &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!child.valid()) return false;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) return false;
    OVERLAPPED overlap{};
    overlap.hEvent = event.value;
    if (ConnectNamedPipe(parent.value, &overlap)) return true;
    const auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error == ERROR_IO_PENDING) {
        DWORD count = 0;
        (void)CancelIoEx(parent.value, &overlap);
        (void)GetOverlappedResult(parent.value, &overlap, &count, TRUE);
    }
    return false;
}

Failure pipeFailure(DWORD error) {
    return (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA)
        ? Failure::Closed : Failure::Pipe;
}

Io transfer(HANDLE pipe, bool writing, char* bytes, std::size_t size,
            Clock::time_point deadline, std::stop_token stop) {
    if (const auto failure = expired(deadline, stop); failure != Failure::None) return {0, failure};
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.valid()) return {0, Failure::Pipe};
    OVERLAPPED overlap{};
    overlap.hEvent = event.value;
    const auto requested = static_cast<DWORD>(size);
    DWORD count = 0;
    const auto inlineDone = writing ? WriteFile(pipe, bytes, requested, &count, &overlap)
                                    : ReadFile(pipe, bytes, requested, &count, &overlap);
    if (!inlineDone) {
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) return {0, pipeFailure(error)};
        for (;;) {
            const auto failure = expired(deadline, stop);
            if (failure != Failure::None) {
                // The stack OVERLAPPED stays alive until the kernel releases it.
                (void)CancelIoEx(pipe, &overlap);
                (void)GetOverlappedResult(pipe, &overlap, &count, TRUE);
                return {0, failure};
            }
            const auto ready = WaitForSingleObject(event.value, static_cast<DWORD>(waitSlice(deadline).count()));
            if (ready == WAIT_TIMEOUT) continue;
            if (ready != WAIT_OBJECT_0) {
                (void)CancelIoEx(pipe, &overlap);
                (void)GetOverlappedResult(pipe, &overlap, &count, TRUE);
                return {0, Failure::Pipe};
            }
            if (!GetOverlappedResult(pipe, &overlap, &count, FALSE))
                return {0, pipeFailure(GetLastError())};
            break;
        }
    }
    return {static_cast<std::size_t>(count), count == 0 ? Failure::Closed : Failure::None};
}

#else
struct Fd {
    int value = -1;
    ~Fd() { reset(); }
    void reset() {
        if (value >= 0) ::close(value);
        value = -1;
    }
    [[nodiscard]] int release() { return std::exchange(value, -1); }
};

bool makePipe(Fd& reader, Fd& writer) {
    int fds[2] = {-1, -1};
#if defined(__linux__)
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
#else
    if (pipe(fds) != 0) return false;
#endif
    reader.value = fds[0];
    writer.value = fds[1];
    // Redirections cannot share one of the standard descriptor numbers.
    for (auto* fd : {&reader, &writer}) {
        if (fd->value < 3) {
            const int moved = fcntl(fd->value, F_DUPFD_CLOEXEC, 3);
            if (moved < 0) return false;
            fd->reset();
            fd->value = moved;
        }
        if (fcntl(fd->value, F_SETFD, FD_CLOEXEC) != 0) return false;
    }
    return true;
}

// A crashed peer can close stdin between poll and write. Suppress the
// resulting SIGPIPE only for this write, without changing ADI's signal policy.
ssize_t writeWithoutSigpipe(int fd, const char* bytes, std::size_t size) {
    sigset_t pipeSignal, saved, pending;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &pipeSignal, &saved) != 0) {
        errno = EIO;
        return -1;
    }
    const bool alreadyPending = sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) == 1;
    const auto count = ::write(fd, bytes, size);
    const auto error = errno;
    if (count < 0 && error == EPIPE && !alreadyPending &&
        sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        int signal = 0;
        (void)sigwait(&pipeSignal, &signal);
    }
    (void)pthread_sigmask(SIG_SETMASK, &saved, nullptr);
    errno = error;
    return count;
}

Io transfer(int pipe, bool writing, char* bytes, std::size_t size,
            Clock::time_point deadline, std::stop_token stop) {
    for (;;) {
        if (const auto failure = expired(deadline, stop); failure != Failure::None) return {0, failure};
        const auto count = writing ? writeWithoutSigpipe(pipe, bytes, size) : ::read(pipe, bytes, size);
        if (count > 0) return {static_cast<std::size_t>(count), Failure::None};
        if (count == 0 || errno == EPIPE) return {0, Failure::Closed};
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return {0, Failure::Pipe};
        pollfd event{pipe, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        if (::poll(&event, 1, static_cast<int>(waitSlice(deadline).count())) < 0 && errno != EINTR)
            return {0, Failure::Pipe};
    }
}
#endif

}  // namespace

class AgentProcess::Impl {
public:
    bool launch(const AgentCommand& command) {
#if defined(_WIN32)
        if (!isPortableExecutable(command.executable)) return false;
        auto commandLine = quote(command.executable.native());
        for (const auto& argument : command.arguments)
            if (!appendArgument(commandLine, argument)) return false;
        if (commandLine.size() >= 32767) return false;

        Handle input, output, stdinPeer, stdoutPeer, stderrPeer, processJob, child, thread;
        if (!makePipe(input, stdinPeer, true) || !makePipe(output, stdoutPeer, false)) return false;
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        stderrPeer.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inheritable,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!stderrPeer.valid()) return false;
        processJob.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!processJob.valid() || !SetInformationJobObject(processJob.value, JobObjectExtendedLimitInformation,
                                                             &limits, sizeof(limits))) return false;

        SIZE_T size = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0) return false;
        std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
        struct Attributes {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~Attributes() { DeleteProcThreadAttributeList(value); }
        } attrGuard{attributes};
        HANDLE handles[] = {stdinPeer.value, stdoutPeer.value, stderrPeer.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles, sizeof(handles), nullptr, nullptr)) return false;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = stdinPeer.value;
        startup.StartupInfo.hStdOutput = stdoutPeer.value;
        startup.StartupInfo.hStdError = stderrPeer.value;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
        if (!CreateProcessW(command.executable.c_str(), commandLine.data(), nullptr, nullptr,
                            TRUE, flags, nullptr, nullptr, &startup.StartupInfo, &info)) return false;
        child.value = info.hProcess;
        thread.value = info.hThread;
        if (!AssignProcessToJobObject(processJob.value, child.value) ||
            ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            (void)TerminateProcess(child.value, 1);
            (void)WaitForSingleObject(child.value, INFINITE);
            return false;
        }
        job_.value = processJob.release();
        process_.value = child.release();
#else
        Fd input, output, stdinPeer, stdoutPeer;
        if (!makePipe(stdinPeer, input) || !makePipe(output, stdoutPeer) ||
            fcntl(input.value, F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(output.value, F_SETFL, O_NONBLOCK) != 0) return false;
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) return false;
        struct Actions {
            posix_spawn_file_actions_t& value;
            ~Actions() { posix_spawn_file_actions_destroy(&value); }
        } actionsGuard{actions};
        posix_spawnattr_t attrs;
        if (posix_spawnattr_init(&attrs) != 0) return false;
        struct Attributes {
            posix_spawnattr_t& value;
            ~Attributes() { posix_spawnattr_destroy(&value); }
        } attrGuard{attrs};
        if (posix_spawn_file_actions_adddup2(&actions, stdinPeer.value, STDIN_FILENO) != 0 ||
            posix_spawn_file_actions_adddup2(&actions, stdoutPeer.value, STDOUT_FILENO) != 0 ||
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0)
            return false;
        for (const auto* fd : {&input, &output, &stdinPeer, &stdoutPeer})
            if (posix_spawn_file_actions_addclose(&actions, fd->value) != 0) return false;
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
        if (posix_spawn_file_actions_addclosefrom_np(&actions, 3) != 0) return false;
#endif
#endif
        short flags = POSIX_SPAWN_SETPGROUP;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
        if (posix_spawnattr_setpgroup(&attrs, 0) != 0 || posix_spawnattr_setflags(&attrs, flags) != 0)
            return false;
        const auto executable = command.executable.string();
        std::vector<char*> argv;
        argv.reserve(command.arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& argument : command.arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        pid_t pid = -1;
        if (posix_spawn(&pid, executable.c_str(), &actions, &attrs, argv.data(), environ) != 0)
            return false;
        pid_ = pid;
#endif
        input_.value = input.release();
        output_.value = output.release();
        return true;
    }

    [[nodiscard]] bool running() const {
#if defined(_WIN32)
        return process_.valid();
#else
        return pid_ > 0;
#endif
    }

    void stop() noexcept {
        input_.reset();
        output_.reset();
#if defined(_WIN32)
        if (process_.valid()) {
            // The job's close also kills inherited pipe owners.
            job_.reset();
            (void)WaitForSingleObject(process_.value, INFINITE);
            process_.reset();
        }
#else
        if (pid_ > 0) {
            // Never reap before killing the group, so the PID is not reused.
            const auto pid = std::exchange(pid_, -1);
            (void)::kill(-pid, SIGKILL);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
#endif
        buffered.clear();
    }

    Io write(char* bytes, std::size_t size, Clock::time_point deadline, std::stop_token stop) {
        return transfer(input_.value, true, bytes, size, deadline, stop);
    }
    Io read(char* bytes, std::size_t size, Clock::time_point deadline, std::stop_token stop) {
        return transfer(output_.value, false, bytes, size, deadline, stop);
    }
    std::string buffered;

private:
#if defined(_WIN32)
    Handle input_, output_, process_, job_;
#else
    Fd input_, output_;
    pid_t pid_ = -1;
#endif
};

AgentProcess::AgentProcess() : impl_(std::make_unique<Impl>()) {}
AgentProcess::~AgentProcess() { stop(); }

bool AgentProcess::start(const AgentCommand& command, std::string& error) {
    stop();
    error.clear();
    if (!valid(command)) {
        error = "invalid agent helper command";
        return false;
    }
    if (!impl_->launch(command)) {
        error = "could not start the agent helper";
        return false;
    }
    return true;
}

bool AgentProcess::send(std::string_view frame, Milliseconds timeout, std::string& error,
                        std::stop_token token) {
    error.clear();
    if (!impl_->running()) {
        error = "agent helper is not running";
        return false;
    }
    if (timeout <= Milliseconds::zero() || timeout > kMaxWait || frame.empty() ||
        frame.size() > kMaxFrameBytes || frame.find_first_of("\r\n") != frame.npos ||
        frame.find('\0') != frame.npos) {
        error = "invalid agent helper frame or timeout";
        return false;
    }
    const auto deadline = Clock::now() + timeout;
    std::string payload(frame);
    payload.push_back('\n');
    for (std::size_t offset = 0; offset < payload.size();) {
        const auto length = std::min<std::size_t>(4096, payload.size() - offset);
        const auto result = impl_->write(payload.data() + offset, length, deadline, token);
        if (result.failure != Failure::None) {
            error = failureMessage(result.failure);
            stop();
            return false;
        }
        offset += result.count;
    }
    return true;
}

std::optional<std::string> AgentProcess::read(Milliseconds timeout, std::string& error,
                                              std::stop_token token) {
    error.clear();
    if (!impl_->running()) {
        error = "agent helper is not running";
        return std::nullopt;
    }
    if (timeout <= Milliseconds::zero() || timeout > kMaxWait) {
        error = "invalid agent helper timeout";
        return std::nullopt;
    }
    const auto deadline = Clock::now() + timeout;
    const auto fail = [&](const char* message) -> std::optional<std::string> {
        error = message;
        stop();
        return std::nullopt;
    };
    std::array<char, 4096> bytes{};
    auto& buffer = impl_->buffered;
    for (;;) {
        if (const auto failure = expired(deadline, token); failure != Failure::None)
            return fail(failureMessage(failure));
        const auto newline = buffer.find('\n');
        if (newline != buffer.npos) {
            const auto length = newline > 0 && buffer[newline - 1] == '\r' ? newline - 1 : newline;
            if (length == 0 || length > kMaxFrameBytes) return fail("invalid agent helper output frame");
            auto frame = buffer.substr(0, length);
            buffer.erase(0, newline + 1);
            if (frame.find('\0') != frame.npos) return fail("invalid agent helper output frame");
            return frame;
        }
        if (buffer.size() > kMaxFrameBytes + 1) return fail("agent helper output frame is too large");
        const auto result = impl_->read(bytes.data(), bytes.size(), deadline, token);
        if (result.failure != Failure::None) return fail(failureMessage(result.failure));
        buffer.append(bytes.data(), result.count);
    }
}

void AgentProcess::stop() noexcept { impl_->stop(); }

}  // namespace adi::control
