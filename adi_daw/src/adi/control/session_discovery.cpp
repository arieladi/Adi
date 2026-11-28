// SPDX-License-Identifier: AGPL-3.0-or-later

#include "adi/control/session_discovery.hpp"

#include "adi/appdata.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/control/consent_file.hpp"
#include "adi/control/jsonrpc.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h> // getentropy is declared here on macOS, not in unistd.h
#endif
#endif

namespace adi::control {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kTokenBytes = 32;
constexpr std::size_t kRecordBytes = 512;

bool validToken(std::string_view token) {
    return token.size() == 2 * kTokenBytes &&
           token.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::uint32_t currentProcess() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

bool processAlive(std::uint32_t process) {
#if defined(_WIN32)
    const auto handle = OpenProcess(SYNCHRONIZE, FALSE, process);
    if (!handle) return false;
    const auto status = WaitForSingleObject(handle, 0);
    (void)CloseHandle(handle);
    return status == WAIT_TIMEOUT;
#else
    if (process > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) return false;
    return ::kill(static_cast<pid_t>(process), 0) == 0 || errno == EPERM;
#endif
}

std::optional<SessionDiscovery> decode(std::string_view bytes) {
    unsigned fields = 0;
    const auto record = Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > 1) throw std::runtime_error("nested session discovery record");
        if (event != Json::parse_event_t::key) return true;
        const auto& key = parsed.get_ref<const std::string&>();
        const unsigned field = key == "port" ? 1U : key == "token" ? 2U : key == "pid" ? 4U : 0U;
        if (field == 0 || (fields & field) != 0) throw std::runtime_error("invalid session discovery record");
        fields |= field;
        return true;
    }, false);
    if (fields != 7U || !record.is_object() || record.size() != 3 ||
        !record.at("port").is_number_unsigned() || !record.at("pid").is_number_unsigned() ||
        !record.at("token").is_string()) return std::nullopt;
    const auto port = record.at("port").get<std::uint64_t>();
    const auto process = record.at("pid").get<std::uint64_t>();
    const auto& token = record.at("token").get_ref<const std::string&>();
    if (port == 0 || port > std::numeric_limits<std::uint16_t>::max() ||
        process == 0 || process > std::numeric_limits<std::uint32_t>::max() ||
        !validToken(token) || !processAlive(static_cast<std::uint32_t>(process))) return std::nullopt;
    return SessionDiscovery{static_cast<std::uint16_t>(port), token};
}

void sweepStages(const fs::path& record) {
    auto pattern = record.filename();
    pattern += ".stage-";
    using NativeView = std::basic_string_view<fs::path::value_type>;
    const auto& prefix = pattern.native();
    const auto digits = [](NativeView value) {
        return !value.empty() && std::all_of(value.begin(), value.end(),
            [](fs::path::value_type ch) { return ch >= '0' && ch <= '9'; });
    };
    std::error_code error;
    for (fs::directory_iterator entry(record.parent_path(), error), end;
         !error && entry != end; entry.increment(error)) {
        const auto name = entry->path().filename();
        if (!name.native().starts_with(prefix)) continue;
        const auto number = NativeView(name.native()).substr(prefix.size());
        const auto separator = number.find('-');
        if (separator != NativeView::npos && digits(number.substr(0, separator)) &&
            digits(number.substr(separator + 1))) (void)detail::removePrivateFile(entry->path());
    }
}

}  // namespace

std::string makeSessionToken() {
    if (audio::onAudioThread) return {};
    std::array<unsigned char, kTokenBytes> bytes{};
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
#else
    int status;
    do { status = ::getentropy(bytes.data(), bytes.size()); } while (status != 0 && errno == EINTR);
    if (status != 0) return {};
#endif
    constexpr char hex[] = "0123456789abcdef";
    std::string token(2 * bytes.size(), '0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        token[2 * i] = hex[bytes[i] >> 4U];
        token[2 * i + 1] = hex[bytes[i] & 0x0fU];
    }
    return token;
}

fs::path sessionDiscoveryPath() {
    const auto data = appdata::pathsFor(appdata::App::Daw).data;
    return data.empty() ? fs::path{} : data / "control-adi" / "session.json";
}

std::optional<SessionDiscovery> readSessionDiscovery(const fs::path& file) {
    if (audio::onAudioThread) return std::nullopt;
    try {
        const auto publisher = detail::lockPrivateFile(file, false);
        if (publisher.lock) {
            // After an abnormal exit, the kernel releases the publisher's lock.
            (void)detail::removePrivateFile(file);
            sweepStages(file);
            return std::nullopt;
        }
        if (!publisher.busy) return std::nullopt;
        const auto bytes = detail::readConsentFile(file, kRecordBytes);
        if (bytes && !bytes->empty()) return decode(*bytes);
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

SessionDiscoveryFile::SessionDiscoveryFile(fs::path file) : file_(std::move(file)) {}

SessionDiscoveryFile::~SessionDiscoveryFile() { reset(); }

bool SessionDiscoveryFile::publish(const SessionDiscovery& discovery) {
    if (audio::onAudioThread || discovery.port == 0 || !validToken(discovery.token)) return false;
    try {
        detail::PrivateFileLockAttempt publisher;
        if (owner_) {
            if (detail::readConsentFile(file_, kRecordBytes) != record_) return false;
        } else {
            publisher = detail::lockPrivateFile(file_, true);
            if (!publisher.lock || !detail::removePrivateFile(file_)) return false;
            sweepStages(file_);
        }
        auto record = Json{{"port", discovery.port}, {"token", discovery.token}, {"pid", currentProcess()}}.dump();
        if (!detail::writeConsentFile(file_, record) &&
            detail::readConsentFile(file_, kRecordBytes) != record) return false;
        record_ = std::move(record);
        if (publisher.lock) owner_ = std::move(publisher.lock);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void SessionDiscoveryFile::reset() noexcept {
    if (!owner_ || audio::onAudioThread) return;
    if (detail::readConsentFile(file_, kRecordBytes) == record_)
        (void)detail::removePrivateFile(file_);
    record_.clear();
    owner_.reset();
}

}  // namespace adi::control
