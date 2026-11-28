// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/consent_scope.hpp"

#include "adi/audio/io_audit.hpp"
#include "adi/media/publish_file.hpp"
#include "adi/store.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/stat.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace adi::control {
namespace {

using namespace std::literals;

char asciiLower(char value) noexcept {
    if (value >= 'A' && value <= 'Z') return static_cast<char>(value - 'A' + 'a');
    return value;
}

bool equalAsciiFolded(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return asciiLower(x) == asciiLower(y); });
}

bool decimalAtMost(std::string_view text, unsigned max, unsigned min = 0) noexcept {
    if (text.empty()) return false;
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= min && value <= max;
}

bool loopbackIpv4(std::string_view host) noexcept {
    if (!host.starts_with("127.")) return false;
    host.remove_prefix(4);
    for (int octet = 0; octet < 3; ++octet) {
        const auto dot = host.find('.');
        if ((dot == std::string_view::npos) != (octet == 2)) return false;
        const auto number = host.substr(0, dot);
        if ((number.size() > 1 && number.front() == '0') || !decimalAtMost(number, 255)) return false;
        if (dot != std::string_view::npos) host.remove_prefix(dot + 1);
    }
    return true;
}

// The one IPv6 loopback address is seven zero groups followed by a one.
// Validate the spelling as well as the value (including one possible ::).
std::optional<unsigned> loopbackGroups(std::string_view text, bool endsWithOne) noexcept {
    if (text.empty()) return endsWithOne ? std::nullopt : std::optional<unsigned>{0};
    unsigned count = 0;
    for (;;) {
        const auto separator = text.find(':');
        auto group = text.substr(0, separator);
        if (group.empty() || group.size() > 4) return std::nullopt;
        if (endsWithOne && separator == std::string_view::npos) {
            if (group.back() != '1') return std::nullopt;
            group.remove_suffix(1);
        }
        if (group.find_first_not_of('0') != std::string_view::npos) return std::nullopt;
        if (++count > 8) return std::nullopt;
        if (separator == std::string_view::npos) return count;
        text.remove_prefix(separator + 1);
    }
}

bool loopbackIpv6(std::string_view host) noexcept {
    const auto compression = host.find("::");
    if (compression == std::string_view::npos) return loopbackGroups(host, true) == 8;
    const auto before = loopbackGroups(host.substr(0, compression), false);
    const auto after = loopbackGroups(host.substr(compression + 2), true);
    return before && after && *before + *after < 8;
}

// The birth stamp distinguishes an unlinked file from a later file on a
// recycled inode/ID. A volume without a birth stamp yields no durable grant.
std::string birthStamp(const media::ProjectFileLease& lease) {
    const auto& path = lease.path();
#if defined(_WIN32)
    // The Store holds a handle that denies delete and replacement.
    std::error_code error;
    const auto resolved = std::filesystem::canonical(path, error);
    if (error || media::readFileIdentity(resolved, error) != lease.identity()) return {};
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(resolved.c_str(), GetFileExInfoStandard, &attributes)) return {};
    const auto birth = attributes.ftCreationTime;
    if (birth.dwHighDateTime == 0 && birth.dwLowDateTime == 0) return {};
    return std::to_string(birth.dwHighDateTime) + "." + std::to_string(birth.dwLowDateTime);
#elif defined(__APPLE__)
    const auto id = lease.identity();
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        id.volume != static_cast<std::uint64_t>(info.st_dev) || id.file != static_cast<std::uint64_t>(info.st_ino))
        return {};
    const auto birth = info.st_birthtimespec;
    if (birth.tv_sec == 0 && birth.tv_nsec == 0) return {};
    return std::to_string(birth.tv_sec) + "." + std::to_string(birth.tv_nsec);
#elif defined(__linux__)
    const auto id = lease.identity();
    struct statx info {};
    constexpr auto fields = STATX_INO | STATX_TYPE | STATX_BTIME;
    if (::statx(AT_FDCWD, path.c_str(), 0, fields, &info) != 0 || (info.stx_mask & fields) != fields ||
        !S_ISREG(info.stx_mode) || id.file != info.stx_ino ||
        id.volume != static_cast<std::uint64_t>(makedev(info.stx_dev_major, info.stx_dev_minor)))
        return {};
    const auto birth = info.stx_btime;
    if (birth.tv_sec == 0 && birth.tv_nsec == 0) return {};
    return std::to_string(birth.tv_sec) + "." + std::to_string(birth.tv_nsec);
#else
    (void)path;
    return {};
#endif
}

}  // namespace

bool isLoopbackDestination(std::string_view endpoint) noexcept {
    if (std::any_of(endpoint.begin(), endpoint.end(), [](unsigned char c) {
            return c <= 0x20 || c == 0x7f || c == '\\';
        }))
        return false;

    const auto schemeEnd = endpoint.find("://");
    auto authority = endpoint;
    if (schemeEnd != std::string_view::npos) {
        const auto scheme = endpoint.substr(0, schemeEnd);
        if (!equalAsciiFolded(scheme, "http"sv) && !equalAsciiFolded(scheme, "https"sv) &&
            !equalAsciiFolded(scheme, "ws"sv) && !equalAsciiFolded(scheme, "wss"sv))
            return false;
        authority.remove_prefix(schemeEnd + 3);
        authority = authority.substr(0, authority.find_first_of("/?#"));
    }
    if (authority.empty() || authority.find_first_of("@%/?#") != std::string_view::npos) return false;

    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        const auto port = authority.substr(close + 1);
        return (port.empty() || (port.front() == ':' && decimalAtMost(port.substr(1), 65535, 1))) &&
               loopbackIpv6(authority.substr(1, close - 1));
    }
    if (authority.find_first_of("[]") != std::string_view::npos) return false;

    const auto separator = authority.find(':');
    if (separator != std::string_view::npos) {
        const auto tail = authority.substr(separator + 1);
        if (tail.find(':') != std::string_view::npos)
            return schemeEnd == std::string_view::npos && loopbackIpv6(authority);
        if (!decimalAtMost(tail, 65535, 1)) return false;
    }
    const auto host = authority.substr(0, separator);
    return equalAsciiFolded(host, "localhost"sv) || loopbackIpv4(host);
}

std::string projectFileConsentBinding(const Store& store) {
    if (audio::onAudioThread || !store.fileLease_) return {};
    const auto& lease = *store.fileLease_;
    const auto birth = birthStamp(lease);
    if (birth.empty()) return {};

    const auto id = lease.identity();
    return "v1:" + std::to_string(static_cast<int>(id.scheme)) + ":" + std::to_string(id.volume) +
           ":" + std::to_string(id.file) + ":" + std::to_string(id.fileHigh) + ":" + birth;
}

}  // namespace adi::control
