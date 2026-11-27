// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "juce/audiogridder_transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace adi::device {
namespace {

using Bytes = std::span<std::byte>;
using ReadOnlyBytes = std::span<const std::byte>;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using Json = nlohmann::json;

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr auto invalidSocket = INVALID_SOCKET;

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() { if (ready) WSACleanup(); }
    bool ready = false;
};

int socketError() noexcept { return WSAGetLastError(); }
bool interrupted(int error) noexcept { return error == WSAEINTR; }
bool wouldBlock(int error) noexcept { return error == WSAEWOULDBLOCK; }
bool connecting(int error) noexcept { return wouldBlock(error) || error == WSAEINPROGRESS; }
void closeSocket(SocketHandle handle) noexcept { closesocket(handle); }
SocketHandle createSocket() noexcept {
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
}
bool setNonblocking(SocketHandle handle) noexcept {
    unsigned long state = 1;
    return ioctlsocket(handle, FIONBIO, &state) == 0;
}
using SocketPollFd = WSAPOLLFD;
int socketPoll(SocketPollFd& fd, int timeout) { return WSAPoll(&fd, 1, timeout); }
int sendBytes(SocketHandle handle, ReadOnlyBytes bytes) {
    return send(handle, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
}
int receiveBytes(SocketHandle handle, Bytes bytes) {
    return recv(handle, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
}
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr auto invalidSocket = -1;
int socketError() noexcept { return errno; }
bool interrupted(int error) noexcept { return error == EINTR; }
bool wouldBlock(int error) noexcept { return error == EAGAIN || error == EWOULDBLOCK; }
bool connecting(int error) noexcept { return error == EINPROGRESS || interrupted(error); }
void closeSocket(SocketHandle handle) noexcept { close(handle); }
SocketHandle createSocket() noexcept {
#ifdef SOCK_CLOEXEC
    return socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    const auto handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle != invalidSocket && fcntl(handle, F_SETFD, FD_CLOEXEC) != 0) {
        closeSocket(handle);
        return invalidSocket;
    }
    return handle;
#endif
}
bool setNonblocking(SocketHandle handle) noexcept {
    const int flags = fcntl(handle, F_GETFL);
    return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
}
using SocketPollFd = pollfd;
int socketPoll(SocketPollFd& fd, int timeout) { return poll(&fd, 1, timeout); }
ssize_t sendBytes(SocketHandle handle, ReadOnlyBytes bytes) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    return send(handle, bytes.data(), bytes.size(), flags);
}
ssize_t receiveBytes(SocketHandle handle, Bytes bytes) {
    return recv(handle, bytes.data(), bytes.size(), 0);
}
#endif

class Socket final {
public:
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }

    void reset() noexcept {
        if (handle_ != invalidSocket) closeSocket(std::exchange(handle_, invalidSocket));
    }

    [[nodiscard]] bool open(in_addr address, std::uint16_t port, Deadline deadline, std::string& error) {
#ifdef _WIN32
        static const SocketRuntime runtime;
        if (!runtime.ready) {
            error = "could not initialize TCP";
            return false;
        }
#endif
        reset();
        handle_ = createSocket();
        const int enabled = 1;
        if (handle_ == invalidSocket || !setNonblocking(handle_) ||
            setsockopt(handle_, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0
#ifdef SO_NOSIGPIPE
            || setsockopt(handle_, SOL_SOCKET, SO_NOSIGPIPE,
                          reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0
#endif
        ) {
            error = "could not open TCP socket";
            return false;
        }
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(port);
        destination.sin_addr = address;
        if (connect(handle_, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) == 0)
            return true;
        if (!connecting(socketError())) {
            error = "TCP connection failed";
            return false;
        }
        if (!wait(POLLOUT, deadline, error)) return false;
        int failure = 0;
        SocketLength length = sizeof(failure);
        if (getsockopt(handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&failure), &length) == 0 && failure == 0)
            return true;
        error = "TCP connection failed";
        return false;
    }

    [[nodiscard]] bool write(ReadOnlyBytes bytes, Deadline deadline, std::string& error) const {
        return transfer(bytes, POLLOUT, sendBytes, deadline, error);
    }

    [[nodiscard]] bool read(Bytes bytes, Deadline deadline, std::string& error) const {
        return transfer(bytes, POLLIN, receiveBytes, deadline, error);
    }

private:
    [[nodiscard]] bool wait(short events, Deadline deadline, std::string& error) const {
        for (;;) {
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) {
                error = "TCP operation timed out";
                return false;
            }
            SocketPollFd descriptor{handle_, events, 0};
            const auto millis = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            const int result = socketPoll(descriptor, static_cast<int>(millis));
            if (result > 0 && !(descriptor.revents & POLLNVAL)) return true;
            if (result == 0 || (result < 0 && interrupted(socketError()))) continue;
            error = "TCP socket wait failed";
            return false;
        }
    }

    template<class Span, class Transfer>
    [[nodiscard]] bool transfer(Span bytes, short events, Transfer action,
                                Deadline deadline, std::string& error) const {
        while (!bytes.empty()) {
            if (!wait(events, deadline, error)) return false;
            const auto moved = action(handle_, bytes);
            if (moved > 0) {
                bytes = bytes.subspan(static_cast<std::size_t>(moved));
            } else if (moved == 0) {
                error = "TCP peer closed the connection";
                return false;
            } else {
                const auto failure = socketError();
                if (interrupted(failure) || wouldBlock(failure)) continue;
                error = "TCP data transfer failed";
                return false;
            }
        }
        return true;
    }

    SocketHandle handle_ = invalidSocket;
};

// Stock v13 writes C++ object representations directly. These offsets describe
// the little-endian, 64-bit release ABI. Explicit bytes keep compiler padding
// and local JUCE types out of the client. Upstream sources are pinned in the
// engine study. v13 supplies no way to negotiate these layouts.
constexpr std::int32_t protocolVersion = 13;
constexpr std::size_t handshakeRequestSize = 64;
constexpr std::size_t handshakeResponseSize = 36;
constexpr std::size_t audioRequestHeaderSize = 40;
constexpr std::size_t audioResponseHeaderSize = 16;
constexpr std::size_t positionInfoSize = 104;
constexpr std::size_t maxControlPayload = 4 * 1024 * 1024;
constexpr std::size_t maxMidiEvents = 4096;
constexpr std::int32_t maxFrames = 8192;

static_assert(sizeof(float) == 4 && sizeof(double) == 8 &&
              std::numeric_limits<float>::is_iec559 && std::numeric_limits<double>::is_iec559);
static_assert(std::endian::native == std::endian::little, "Stock audio PCM is little endian");

template<class Value>
void put(Bytes bytes, std::size_t offset, Value value) noexcept {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<class Value>
Value get(ReadOnlyBytes bytes, std::size_t offset) noexcept {
    Value value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

enum class Command : std::int32_t {
    PluginList = 10,
    AddPlugin = 20,
    AddPluginResult = 21,
    Parameters = 100,
    Presets = 110,
};

bool writeControl(const Socket& socket, Command command, std::string_view payload,
                  Deadline deadline, std::string& error) {
    std::vector<std::byte> bytes(12 + payload.size());
    put(bytes, 0, static_cast<std::int32_t>(command));
    put(bytes, 4, static_cast<std::int32_t>(4 + payload.size()));
    put(bytes, 8, static_cast<std::int32_t>(payload.size()));
    std::memcpy(bytes.data() + 12, payload.data(), payload.size());
    return socket.write(bytes, deadline, error);
}

bool readControl(const Socket& socket, Command command, std::vector<std::byte>& payload,
                 Deadline deadline, std::string& error) {
    std::array<std::byte, 12> header{};
    if (!socket.read(Bytes{header}.first(8), deadline, error)) return false;
    const auto type = get<std::int32_t>(header, 0);
    const auto size = get<std::int32_t>(header, 4);
    if (type != static_cast<std::int32_t>(command) || size < 4 ||
        static_cast<std::size_t>(size) > maxControlPayload) {
        error = "invalid v13 command header";
        return false;
    }
    if (!socket.read(Bytes{header}.subspan(8), deadline, error)) return false;
    if (size - 4 != get<std::int32_t>(header, 8)) {
        error = "inconsistent v13 command length";
        return false;
    }
    payload.resize(static_cast<std::size_t>(size - 4));
    return socket.read(payload, deadline, error);
}

bool validToken(std::string_view token) {
    constexpr auto prefix = std::string_view{"VST3-"};
    constexpr auto hex = std::string_view{"0123456789abcdefABCDEF"};
    return token.starts_with(prefix) && token.size() > prefix.size() &&
           token.size() <= prefix.size() + 8 &&
           token.substr(prefix.size()).find_first_not_of(hex) == std::string_view::npos;
}

std::array<std::byte, positionInfoSize> positionInfo(
    const std::optional<engine::DeviceTransport>& transport, double sampleRate) {
    std::array<std::byte, positionInfoSize> bytes{};
    put(bytes, 0, std::int32_t{4});
    put(bytes, 4, std::int32_t{4});
    put(bytes, 24, std::int32_t{24});
    bytes[29] = std::byte{1}; // default 23.976 fps pull-down, frame-rate flag unset
    if (!transport) return bytes;

    const auto& t = *transport;
    put(bytes, 0, static_cast<std::int32_t>(t.numerator));
    put(bytes, 4, static_cast<std::int32_t>(t.denominator));
    put(bytes, 8, t.loopBeginQuarterNotes);
    put(bytes, 16, t.loopEndQuarterNotes);
    put(bytes, 32, static_cast<double>(t.sample) / sampleRate);
    put(bytes, 40, t.barStart);
    put(bytes, 48, t.quarterNotes);
    put(bytes, 64, t.bpm);
    put(bytes, 72, t.sample);
    put(bytes, 80, t.bar);
    std::int64_t flags = (1 << 0) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 9);
    if (t.playing) flags |= 1 << 11;
    if (t.recording) flags |= 1 << 12;
    if (t.looping) flags |= (1 << 1) | (1 << 13);
    put(bytes, 96, flags);
    return bytes;
}

bool appendMidi(std::vector<std::byte>& request, const engine::Event& event, std::int32_t frames,
                std::string& error) {
    using Type = engine::EventType;
    std::uint32_t key = event.dim;
    std::uint8_t status{};
    switch (event.type) {
        case Type::NoteOff: status = 0x80; break;
        case Type::NoteOn:  status = 0x90; break;
        case Type::Controller: status = 0xb0; key = event.paramId; break;
        case Type::NoteExpression:
            error = "stock v13 cannot carry per-note expression";
            return false;
        case Type::ParamValue:
        case Type::ParamMod:
            error = "stock v13 cannot carry sample-accurate parameter events";
            return false;
    }
    if (status == 0 || event.frame < 0 || event.frame >= frames || event.channel > 15 ||
        key > 127 || !std::isfinite(event.value) || event.value < 0.0 || event.value > 1.0) {
        error = "invalid MIDI event for stock v13";
        return false;
    }
    auto value = static_cast<std::uint8_t>(std::lround(event.value * 127.0));
    if (event.type == Type::NoteOn) value = std::max(value, std::uint8_t{1});
    const auto offset = request.size();
    request.resize(offset + 11);
    put(request, offset, event.frame);
    put(request, offset + 4, std::int32_t{3});
    request[offset + 8] = static_cast<std::byte>(status | event.channel);
    request[offset + 9] = static_cast<std::byte>(key);
    request[offset + 10] = static_cast<std::byte>(value);
    return true;
}

class AudioGridderProcessor final : public RemoteProcessor {
public:
    AudioGridderProcessor(AudioGridderEndpoint endpoint, AudioGridderPlugin plugin)
        : endpoint_(std::move(endpoint)), plugin_(std::move(plugin)) {}

    bool prepare(double sampleRate, std::int32_t channels, std::int32_t frames, bool,
                 std::int32_t& pluginLatency, std::string& error) override {
        release();
        error.clear();
        in_addr address{};
        using namespace std::chrono_literals;
        if (endpoint_.port == 0 || inet_pton(AF_INET, endpoint_.address.c_str(), &address) != 1 ||
            endpoint_.setupTimeout <= 0ms || endpoint_.setupTimeout > 60s ||
            endpoint_.blockTimeout <= 0ms || endpoint_.blockTimeout > 10s) {
            error = "invalid numeric IPv4 endpoint or timeout";
            return fail(error);
        }
        if (!validToken(plugin_.serverToken) || plugin_.layout.size() > 128 ||
            plugin_.layout == "Multi-Mono" || plugin_.encodedState.size() > maxControlPayload / 8) {
            error = "invalid VST3 server token, layout, or initial state";
            return fail(error);
        }
        if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 768000 ||
            channels < 1 || channels > 2 || frames < 1 || frames > maxFrames ||
            static_cast<double>(frames) / sampleRate < 0.0005) {
            error = "unsupported stream shape or rate for stock v13";
            return fail(error);
        }
        sampleRate_ = sampleRate;
        channels_ = channels;
        frames_ = frames;
        const auto deadline = Clock::now() + endpoint_.setupTimeout;
        std::uint16_t workerPort = 0;
        if (!handshake(address, deadline, workerPort, error)) return fail(error);

        // The server assigns four worker sockets by connection order. Without
        // an editor, keep the final two ancillary receive streams open and idle.
        if (!commands_.open(address, workerPort, deadline, error) ||
            !notifications_.open(address, workerPort, deadline, error) ||
            !audio_.open(address, workerPort, deadline, error) ||
            !screen_.open(address, workerPort, deadline, error) ||
            !loadPlugin(deadline, error)) {
            return fail(error);
        }
        ready_ = true;
        pluginLatency = latency_;
        return true;
    }

    bool exchange(const RemoteBlock& input, std::span<float> output, std::string& error) override {
        error.clear();
        const auto samples = static_cast<std::size_t>(channels_) * static_cast<std::size_t>(frames_);
        if (!ready_ || input.channels != channels_ || input.frames != frames_ ||
            input.audio.size() != samples || output.size() != samples || input.events.size() > maxMidiEvents) {
            error = "connection is closed or audio block shape is invalid";
            return fail(error);
        }
        const auto audioBytes = plugin_.instrument ? std::size_t{0} : input.audio.size_bytes();
        request_.assign(audioRequestHeaderSize + audioBytes, std::byte{});
        put(request_, 0, plugin_.instrument ? std::int32_t{0} : channels_);
        put(request_, 4, plugin_.instrument ? std::int32_t{0} : frames_);
        put(request_, 8, channels_);
        put(request_, 12, frames_);
        put(request_, 16, static_cast<std::int32_t>(input.events.size()));
        if (audioBytes != 0) std::memcpy(request_.data() + audioRequestHeaderSize, input.audio.data(), audioBytes);
        for (const auto& event : input.events) {
            if (!appendMidi(request_, event, frames_, error)) return fail(error);
        }
        const auto position = positionInfo(input.transport, sampleRate_);
        request_.insert(request_.end(), position.begin(), position.end());
        const auto deadline = Clock::now() + endpoint_.blockTimeout;
        std::array<std::byte, audioResponseHeaderSize> response{};
        if (!audio_.write(request_, deadline, error) || !audio_.read(response, deadline, error))
            return fail(error);
        if (get<std::int32_t>(response, 0) != channels_ || get<std::int32_t>(response, 4) != frames_) {
            error = "server changed the audio block shape";
            return fail(error);
        }
        if (get<std::int32_t>(response, 12) != latency_) {
            error = "server changed plug-in latency during the stream";
            return fail(error);
        }
        const auto returnedEvents = get<std::int32_t>(response, 8);
        if (returnedEvents < 0 || static_cast<std::size_t>(returnedEvents) > maxMidiEvents) {
            error = "invalid v13 response MIDI count";
            return fail(error);
        }
        if (!audio_.read(std::as_writable_bytes(output), deadline, error) ||
            !discardMidi(returnedEvents, deadline, error)) return fail(error);
        if (std::any_of(output.begin(), output.end(), [](float sample) { return !std::isfinite(sample); })) {
            error = "server sent nonfinite audio";
            return fail(error);
        }
        return true;
    }

    void release() noexcept override {
        ready_ = false;
        commands_.reset();
        notifications_.reset();
        audio_.reset();
        screen_.reset();
    }

private:
    bool fail(std::string& error) {
        release();
        error = "server plug-in " + plugin_.serverToken + ": " + error;
        return false;
    }

    bool handshake(in_addr address, Deadline deadline, std::uint16_t& workerPort, std::string& error) {
        Socket handshakeSocket;
        if (!handshakeSocket.open(address, endpoint_.port, deadline, error)) return false;
        std::array<std::byte, handshakeRequestSize> request{};
        put(request, 0, protocolVersion);
        put(request, 4, plugin_.instrument ? std::int32_t{0} : channels_);
        put(request, 8, channels_);
        put(request, 16, sampleRate_);
        put(request, 24, frames_);
        static std::atomic<std::uint64_t> nextId{0};
        const auto now = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
        put(request, 32, now + nextId.fetch_add(1, std::memory_order_relaxed));
        const auto bits = (std::uint64_t{1} << channels_) - 1;
        put(request, 48, plugin_.instrument ? bits : bits | (bits << 32));
        std::array<std::byte, handshakeResponseSize> response{};
        if (!handshakeSocket.write(request, deadline, error) ||
            !handshakeSocket.read(response, deadline, error)) return false;
        const auto port = get<std::int32_t>(response, 8);
        if (get<std::int32_t>(response, 0) != protocolVersion) {
            error = "server protocol version is not stock v13";
            return false;
        }
        if (port < 1 || port > 65535) {
            error = "server worker port is invalid";
            return false;
        }
        workerPort = static_cast<std::uint16_t>(port);
        return true;
    }

    bool loadPlugin(Deadline deadline, std::string& error) {
        std::vector<std::byte> payload;
        if (!readControl(commands_, Command::PluginList, payload, deadline, error)) return false;
        // The caller already selected a server token. Only frame the unsolicited
        // catalogue and later presets/parameters. Their contents are not used.
        const Json settings{{"id", plugin_.serverToken}, {"layout", plugin_.layout},
                            {"settings", plugin_.encodedState}, {"monoChannels", 0}};
        if (!writeControl(commands_, Command::AddPlugin, settings.dump(), deadline, error) ||
            !readControl(commands_, Command::AddPluginResult, payload, deadline, error))
            return false;
        try {
            const auto result = Json::parse(payload.begin(), payload.end(),
                [](int depth, Json::parse_event_t, Json&) {
                    if (depth > 8) throw std::runtime_error("v13 plug-in result too deep");
                    return true;
                });
            if (!result.at("success").get<bool>()) {
                error = "server refused to load plug-in";
                return false;
            }
            const auto& latency = result.at("latency");
            const auto& instances = result.at("channelInstances");
            if (!latency.is_number_integer() || latency < 0 ||
                latency > std::numeric_limits<std::int32_t>::max() ||
                !instances.is_number_integer() || instances != 1) {
                error = "server reported unsupported plug-in latency or instances";
                return false;
            }
            latency_ = latency.get<std::int32_t>();
        } catch (const std::exception&) {
            error = "invalid v13 plug-in load result";
            return false;
        }
        return readControl(commands_, Command::Presets, payload, deadline, error) &&
               readControl(commands_, Command::Parameters, payload, deadline, error);
    }

    bool discardMidi(std::int32_t events, Deadline deadline, std::string& error) const {
        // The current device contract has no plug-in MIDI output stream. Stock
        // responses can echo input events, so consume their bounded frames.
        std::array<std::byte, 8> header{};
        std::array<std::byte, 1024> data{};
        for (std::int32_t event = 0; event < events; ++event) {
            if (!audio_.read(header, deadline, error)) return false;
            const auto size = get<std::int32_t>(header, 4);
            if (size <= 0 || static_cast<std::size_t>(size) > data.size()) {
                error = "unsupported v13 response MIDI message size";
                return false;
            }
            if (!audio_.read(Bytes{data}.first(static_cast<std::size_t>(size)), deadline, error))
                return false;
        }
        return true;
    }

    AudioGridderEndpoint endpoint_;
    AudioGridderPlugin plugin_;
    Socket commands_, notifications_, audio_, screen_;
    std::vector<std::byte> request_;
    double sampleRate_ = 0;
    std::int32_t channels_ = 0, frames_ = 0, latency_ = 0;
    bool ready_ = false;
};

} // namespace

std::unique_ptr<RemoteProcessor> makeAudioGridderProcessor(
    AudioGridderEndpoint endpoint, AudioGridderPlugin plugin) {
    return std::make_unique<AudioGridderProcessor>(std::move(endpoint), std::move(plugin));
}

} // namespace adi::device
