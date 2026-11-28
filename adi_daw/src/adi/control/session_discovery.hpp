// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace adi::control {

namespace detail {
struct PrivateFileLock;
}

/// Returns 256 bits from the operating system as 64 lowercase hex digits.
/// An empty token means the listener must stay off.
[[nodiscard]] std::string makeSessionToken();

[[nodiscard]] std::filesystem::path sessionDiscoveryPath();

struct SessionDiscovery {
    std::uint16_t port = 0;
    std::string token;
};

/// A missing, abandoned, malformed, or unsafe record yields no connection.
[[nodiscard]] std::optional<SessionDiscovery> readSessionDiscovery(
    const std::filesystem::path& = sessionDiscoveryPath());

/// The optional discovery file belongs to one listener for its lifetime.
/// For show-only pairing, generate a token without calling publish.
class SessionDiscoveryFile {
public:
    explicit SessionDiscoveryFile(std::filesystem::path = sessionDiscoveryPath());
    ~SessionDiscoveryFile();
    SessionDiscoveryFile(const SessionDiscoveryFile&) = delete;
    SessionDiscoveryFile& operator=(const SessionDiscoveryFile&) = delete;

    [[nodiscard]] bool publish(const SessionDiscovery&);
    void reset() noexcept;

private:
    std::filesystem::path file_;
    std::shared_ptr<detail::PrivateFileLock> owner_;
    std::string record_;
};

}  // namespace adi::control
