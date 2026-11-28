// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace adi::scan {

enum class ProcessStatus : std::uint8_t {
    Invalid, CallbackRefused, LaunchFailed, Exited, Crashed,
    TimedOut, Cancelled, OutputLimit, IoFailed, ConsumerFailed
};

struct ProcessOptions {
    std::chrono::milliseconds timeout{30000};
    std::size_t maximumOutput = 8U * 1024U * 1024U;
    const std::atomic<bool>* cancel = nullptr;
};

struct ProcessResult {
    ProcessStatus status = ProcessStatus::Invalid;
    std::int64_t exitCode = -1; // only meaningful for Exited
    int signal = 0;         // POSIX crash signal; zero on Windows
    std::uint32_t osError = 0;
    std::string output;     // stdout bytes, never a partial successful protocol
    [[nodiscard]] bool succeeded() const noexcept {
        return status == ProcessStatus::Exited && exitCode == 0;
    }
};

/// Blocking OWNER API. Absolute executable, explicit UTF-8 arguments, no shell
/// or PATH lookup. stdin/stderr go to the null device. stdout is bounded and
/// drained while running. Deadline/cancellation/output failure terminates and
/// reaps the child; its process group (POSIX) / job (Windows) is also terminated
/// on normal exit, so inherited writers cannot keep a scan alive. This is crash
/// containment, not a security sandbox against hostile code escaping that group.
///
/// Bounds: 1..300000 ms, 1..8 MiB output, <=256 args and <=128 KiB total argv
/// bytes, no embedded NUL. Callback refusal occurs before allocation/OS calls.
/// cancel must live through return. Call from an owner worker, not the UI/audio
/// callback; no global signal handlers or current-directory changes are made.
ProcessResult runProcess(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments,
                         const ProcessOptions& options = {});

struct ProcessStreamOptions {
    std::chrono::milliseconds timeout{30000}; // same 1..300000 ms lifecycle bound
    std::uint64_t maximumOutput = 0; // mandatory positive total-byte budget
    const std::atomic<bool>* cancel = nullptr;
};
struct ProcessStreamResult {
    ProcessStatus status = ProcessStatus::Invalid;
    std::int64_t exitCode = -1;
    int signal = 0;
    std::uint32_t osError = 0;
    // Whole chunks acknowledged with true; a refusing/throwing consumer may
    // already have touched its own output. All output is provisional on failure.
    std::uint64_t bytesDelivered = 0;
    [[nodiscard]] bool succeeded() const noexcept {
        return status == ProcessStatus::Exited && exitCode == 0;
    }
};
using ProcessOutputConsumer = std::function<bool(std::span<const std::byte>)>;

/// Same owner/process containment as runProcess, with no accumulated stdout.
/// Calls consumer synchronously with nonempty binary chunks whose storage lives
/// only until that call returns. Chunk boundaries have no protocol/frame meaning.
/// False or any exception is ConsumerFailed; cleanup terminates/waits for the
/// child before returning. Accepted bytes never exceed maximumOutput; an entire
/// crossing chunk is refused before calling the consumer. Only succeeded() makes
/// the stream complete. The caller owns rollback of its provisional output.
///
/// Consumer/options.cancel must live through return. Consumer work must be
/// bounded/cooperative: cancellation/deadline are checked around each call, but
/// cannot interrupt a blocked consumer. No asynchronous delivery or PCM parsing.
/// The byte budget is independent of the scanner's unchanged 8 MiB string limit;
/// the caller must choose a finite storage/work budget appropriate to its job.
ProcessStreamResult runProcessStream(const std::filesystem::path& executable,
                                    const std::vector<std::string>& arguments,
                                    const ProcessOutputConsumer& consumer,
                                    const ProcessStreamOptions& options);

} // namespace adi::scan
