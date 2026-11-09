// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
namespace adi::engine {
enum class LinkTransport {Keep,Play,Stop};
struct LinkCommand {
    double tempo=0; // zero: leave the shared tempo alone; otherwise 20..999 BPM
    LinkTransport transport=LinkTransport::Keep;
    bool requestBeat=false;
    double beat=0;
};
struct LinkBuffer {
    // Link's own monotonic clock domain, never wall time or project samples.
    std::int64_t callbackMicros=0,outputLatencyMicros=0;
    double quantum=4;
    // Query a transport launch on a copy of this callback's captured state.
    // Unlike LinkCommand::requestBeat, this never changes the shared timeline.
    bool queryLaunch=false;
    double launchBeat=0;
};
struct LinkFrame {
    bool valid=false,enabled=false,startStopSync=false,requestedPlaying=false;
    bool hasLaunchTime=false;
    std::size_t peers=0;
    double tempo=0,beat=0,phase=0;
    std::int64_t audibleMicros=0,transportChangeMicros=0,launchMicros=0;
    std::optional<std::int64_t> capturedMicros;
};
// App-owned SDK participant. Create/configure/retire off callback; never destroy
// while its single audio driver is using it. No SDK types escape this boundary.
// Networking starts disabled and is opened only by explicit owner enable().
class LinkSession final {
public:
    [[nodiscard]] static std::unique_ptr<LinkSession> create(double initialTempo,std::string& error);
    ~LinkSession();
    LinkSession(const LinkSession&)=delete;
    LinkSession& operator=(const LinkSession&)=delete;
    [[nodiscard]] bool enable(bool);
    [[nodiscard]] bool enableStartStopSync(bool);
    [[nodiscard]] bool enabled()const noexcept;
    [[nodiscard]] std::int64_t clockMicros()const noexcept;
    // One audio driver only. Capture, edit, observe and commit ONE SDK snapshot
    // at callback time plus physical output latency. No app-state APIs, project
    // writes, memory allocation, mutexes or network work on this path. Invalid
    // input returns an invalid frame without submitting any part of a command.
    [[nodiscard]] LinkFrame process(const LinkBuffer&,const LinkCommand& = {})noexcept;
private:
    struct Impl;
    explicit LinkSession(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace adi::engine
