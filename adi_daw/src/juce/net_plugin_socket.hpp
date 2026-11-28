// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace adi::net::plugin_socket {

using Bytes = std::span<std::byte>;
using ReadOnlyBytes = std::span<const std::byte>;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

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

inline int socketError() noexcept { return WSAGetLastError(); }
inline bool interrupted(int error) noexcept { return error == WSAEINTR; }
inline bool wouldBlock(int error) noexcept { return error == WSAEWOULDBLOCK; }
inline bool connecting(int error) noexcept { return wouldBlock(error) || error == WSAEINPROGRESS; }
inline void closeSocket(SocketHandle handle) noexcept { closesocket(handle); }
inline SocketHandle createSocket() noexcept {
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
}
inline bool setNonblocking(SocketHandle handle) noexcept {
    unsigned long state = 1;
    // Winsock takes a signed long, while FIONBIO's encoding sets bit 31.
    constexpr auto command = std::bit_cast<long>(static_cast<unsigned long>(FIONBIO));
    return ioctlsocket(handle, command, &state) == 0;
}
using SocketPollFd = WSAPOLLFD;
inline int socketPoll(SocketPollFd& fd, int timeout) { return WSAPoll(&fd, 1, timeout); }
inline int sendBytes(SocketHandle handle, ReadOnlyBytes bytes) {
    return send(handle, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
}
inline int receiveBytes(SocketHandle handle, Bytes bytes) {
    return recv(handle, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
}
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr auto invalidSocket = -1;
inline int socketError() noexcept { return errno; }
inline bool interrupted(int error) noexcept { return error == EINTR; }
inline bool wouldBlock(int error) noexcept { return error == EAGAIN || error == EWOULDBLOCK; }
inline bool connecting(int error) noexcept { return error == EINPROGRESS || interrupted(error); }
inline void closeSocket(SocketHandle handle) noexcept { close(handle); }
inline SocketHandle createSocket() noexcept {
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
inline bool setNonblocking(SocketHandle handle) noexcept {
    const int flags = fcntl(handle, F_GETFL);
    return flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
}
using SocketPollFd = pollfd;
inline int socketPoll(SocketPollFd& fd, int timeout) { return poll(&fd, 1, timeout); }
inline ssize_t sendBytes(SocketHandle handle, ReadOnlyBytes bytes) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    return send(handle, bytes.data(), bytes.size(), flags);
}
inline ssize_t receiveBytes(SocketHandle handle, Bytes bytes) {
    return recv(handle, bytes.data(), bytes.size(), 0);
}
#endif

class PluginSocket final {
public:
    PluginSocket() = default;
    PluginSocket(const PluginSocket&) = delete;
    PluginSocket& operator=(const PluginSocket&) = delete;
    ~PluginSocket() { reset(); }

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
            const auto timeout = std::min(millis, static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max()));
            const int result = socketPoll(descriptor, static_cast<int>(timeout));
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

} // namespace adi::net::plugin_socket
