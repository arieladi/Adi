// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_server.hpp"

#include "adi/audio/io_audit.hpp"
#include "juce/audiogridder_transport.hpp"
#include "juce/net_plugin_socket.hpp"

#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace adi::net {
namespace {

constexpr std::string_view serviceName = "_audiogridder._tcp.local";
constexpr std::string_view serviceSuffix = "._audiogridder._tcp.local";
constexpr auto stockQuery = std::to_array<std::uint8_t>({
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
    13, '_', 'a', 'u', 'd', 'i', 'o', 'g', 'r', 'i', 'd', 'd', 'e', 'r',
    4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
    0, 12, 128, 1
});
constexpr std::size_t maximumDatagram = 9000;
constexpr std::size_t maximumDnsRecords = 128;
constexpr std::size_t maximumDnsQuestions = 16;
constexpr int maximumReceivedDatagrams = 128;
constexpr std::size_t maximumDiscoveredServers = 64;
constexpr std::uint16_t multicastDnsPort = 5353;
constexpr std::size_t maximumPlugins = 8192;
constexpr int cacheApplicationId = 0x41445343; // ADSC
constexpr int cacheVersion = 1;
constexpr std::uint32_t dnsPtr = 12, dnsTxt = 16, dnsSrv = 33;

bool allowedText(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::ranges::all_of(value, [](unsigned char ch) { return ch >= 32 && ch != 127; });
}

char asciiLower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }
char asciiUpper(char ch) { return ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch; }

bool isHex(char ch) {
    const auto lower = asciiLower(ch);
    return (lower >= 'a' && lower <= 'f') || (lower >= '0' && lower <= '9');
}

std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), asciiLower);
    return out;
}

std::optional<std::string> advertisedUuid(std::string_view value) {
    if (value.size() != 32 && value.size() != 36) return std::nullopt;
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (value[i] != '-') return std::nullopt;
        } else {
            if (!isHex(value[i])) return std::nullopt;
            out.push_back(asciiLower(value[i]));
        }
    }
    if (std::ranges::all_of(out, [](char ch) { return ch == '0'; })) return std::nullopt;
    return out;
}

struct MalformedPacket {};
void packetCheck(bool condition) { if (!condition) throw MalformedPacket{}; }

class DnsReply final {
public:
    explicit DnsReply(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    struct Record {
        std::string owner;
        std::uint32_t ttl;
        std::uint32_t type;
        std::size_t offset;
        std::size_t size;
    };

    [[nodiscard]] std::vector<Record> records() {
        std::size_t cursor = 2;
        const auto flags = integer(cursor, 2, bytes_.size());
        packetCheck((flags & 0xf80fu) == 0x8000u && (flags & 0x0200u) == 0);
        const auto questions = integer(cursor, 2, bytes_.size());
        const auto answers = integer(cursor, 2, bytes_.size());
        const auto authority = integer(cursor, 2, bytes_.size());
        const auto additional = integer(cursor, 2, bytes_.size());
        const auto records = answers + authority + additional;
        packetCheck(questions <= maximumDnsQuestions && records <= maximumDnsRecords);
        for (std::uint32_t i = 0; i < questions; ++i) {
            (void) name(cursor, bytes_.size());
            (void) integer(cursor, 4, bytes_.size());
        }
        std::vector<Record> out;
        out.reserve(records);
        for (std::uint32_t i = 0; i < records; ++i) {
            auto owner = folded(name(cursor, bytes_.size()));
            const auto type = integer(cursor, 2, bytes_.size());
            const auto klass = integer(cursor, 2, bytes_.size());
            const auto ttl = integer(cursor, 4, bytes_.size());
            const auto size = integer(cursor, 2, bytes_.size());
            packetCheck(size <= bytes_.size() - cursor);
            // Authority entries cannot advertise an available server.
            if ((i < answers || i >= answers + authority) && (klass & 0x7fffu) == 1)
                out.push_back({std::move(owner), ttl, type, cursor, size});
            cursor += size;
        }
        packetCheck(cursor == bytes_.size());
        return out;
    }

    [[nodiscard]] std::string instance(const Record& ptr) const {
        auto cursor = ptr.offset;
        const auto end = cursor + ptr.size;
        auto result = name(cursor, end);
        packetCheck(cursor == end);
        return result;
    }

    [[nodiscard]] std::uint16_t port(const Record& srv) const {
        auto cursor = srv.offset;
        const auto end = cursor + srv.size;
        (void) integer(cursor, 4, end); // priority and weight
        const auto result = integer(cursor, 2, end);
        const auto host = folded(name(cursor, end));
        packetCheck(cursor == end && host.ends_with(".local") && result != 0);
        return static_cast<std::uint16_t>(result);
    }

    [[nodiscard]] std::optional<nlohmann::json> info(const Record& txt) const {
        auto cursor = txt.offset;
        const auto end = cursor + txt.size;
        std::optional<nlohmann::json> found;
        while (cursor < end) {
            const auto size = integer(cursor, 1, end);
            packetCheck(size <= end - cursor);
            const char* const data = reinterpret_cast<const char*>(bytes_.data() + cursor);
            const std::string_view value(data, size);
            if (value.starts_with("INFO=")) {
                packetCheck(!found && value.size() > 5);
                auto json = nlohmann::json::parse(value.substr(5), nullptr, false);
                packetCheck(json.is_object());
                found = std::move(json);
            }
            cursor += size;
        }
        return found;
    }

private:
    [[nodiscard]] std::uint32_t integer(std::size_t& cursor, std::size_t count,
                                       std::size_t limit) const {
        packetCheck(cursor <= limit && count <= limit - cursor);
        std::uint32_t result = 0;
        for (std::size_t i = 0; i < count; ++i) result = (result << 8u) | bytes_[cursor++];
        return result;
    }

    [[nodiscard]] std::string name(std::size_t& field, std::size_t end) const {
        std::string result;
        auto cursor = field;
        bool followedPointer = false;
        std::size_t jumps = 0;
        for (;;) {
            const auto length = integer(cursor, 1, end);
            if (length == 0) {
                if (!followedPointer) field = cursor;
                return result;
            }
            if ((length & 0xc0u) == 0xc0u) {
                const auto offset = ((length & 0x3fu) << 8u) | integer(cursor, 1, end);
                packetCheck(offset >= 12 && offset < cursor - 2 && ++jumps <= 32);
                if (!followedPointer) field = cursor;
                followedPointer = true;
                end = cursor - 2; // A reference points back to an earlier encoded name.
                cursor = offset;
                continue;
            }
            const std::size_t separator = result.empty() ? 0 : 1;
            packetCheck(length <= 63 && length <= end - cursor &&
                result.size() + length + separator <= 253);
            if (!result.empty()) result.push_back('.');
            for (std::uint32_t i = 0; i < length; ++i) {
                const auto byte = bytes_[cursor++];
                packetCheck(byte >= 32 && byte != 127 && byte != '.');
                result.push_back(static_cast<char>(byte));
            }
        }
    }

    std::span<const std::uint8_t> bytes_;
};

const DnsReply::Record* uniqueRecord(const std::vector<DnsReply::Record>& records,
                                    std::uint32_t type, std::string_view owner) {
    const DnsReply::Record* found = nullptr;
    for (const auto& record : records) {
        if (record.type != type || record.owner != owner) continue;
        packetCheck(found == nullptr);
        found = &record;
    }
    return found;
}

namespace udp {
using namespace plugin_socket;

#ifdef _WIN32
using DatagramLength = int;
SocketHandle openSocket() {
    return WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
}
bool oversized(int error) noexcept { return error == WSAEMSGSIZE; }
#else
using DatagramLength = std::size_t;
SocketHandle openSocket() {
#ifdef SOCK_CLOEXEC
    return socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
#else
    const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle != invalidSocket && fcntl(handle, F_SETFD, FD_CLOEXEC) != 0) {
        closeSocket(handle);
        return invalidSocket;
    }
    return handle;
#endif
}
bool oversized(int error) noexcept { return error == EMSGSIZE; }
#endif

class BrowseSocket final {
public:
    BrowseSocket() : handle(openSocket()) {}
    ~BrowseSocket() { if (handle != invalidSocket) closeSocket(handle); }
    BrowseSocket(const BrowseSocket&) = delete;
    BrowseSocket& operator=(const BrowseSocket&) = delete;
    const SocketHandle handle;
};

bool sendQuery(SocketHandle socket, std::span<const std::uint8_t> packet,
               bool loopback, std::uint16_t port) {
    if (socket == invalidSocket || !setNonblocking(socket)) return false;
    if (!loopback) {
#ifdef _WIN32
        const int ttl = 255;
#else
        const unsigned char ttl = 255;
#endif
        if (setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL,
                       reinterpret_cast<const char*>(&ttl), static_cast<SocketLength>(sizeof(ttl))) != 0) return false;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : 0xe00000fbu); // 224.0.0.251
    const auto bytes = sendto(socket, reinterpret_cast<const char*>(packet.data()),
        static_cast<DatagramLength>(packet.size()), 0,
        reinterpret_cast<const sockaddr*>(&destination), static_cast<SocketLength>(sizeof(destination)));
    return bytes >= 0 && static_cast<std::size_t>(bytes) == packet.size();
}
} // namespace udp

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

void require(bool value, const char* error) {
    if (!value) throw std::runtime_error(error);
}

void validatePlugin(ServerPlugin& plugin) {
    require(allowedText(plugin.name, 256) && allowedText(plugin.serverToken, 512),
            "plug-in catalogue contains invalid metadata");
    if (plugin.format == "vst3") {
        require(plugin.uid.size() == 32 && std::ranges::all_of(plugin.uid, isHex),
                "the server catalogue requires a full VST3 class ID");
        std::ranges::transform(plugin.uid, plugin.uid.begin(), asciiUpper);
    } else {
        require(plugin.format == "clap" && allowedText(plugin.uid, 512),
                "the server catalogue requires a CLAP descriptor ID");
    }
}

void validateSqliteType(SQLite::Database& db) {
    require(db.execAndGet("PRAGMA application_id").getInt() == cacheApplicationId &&
            db.execAndGet("PRAGMA user_version").getInt() == cacheVersion,
            "invalid or unsupported plug-in server catalogue cache");
}

bool cacheExists(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) return false;
    require(!error && std::filesystem::is_regular_file(status),
            "plug-in server catalogue path is unavailable");
    return true;
}

void initializeCache(SQLite::Database& db) {
    if (db.execAndGet("PRAGMA application_id").getInt() != 0) return;
    require(db.execAndGet("PRAGMA user_version").getInt() == 0 &&
            db.execAndGet("SELECT count(*) FROM sqlite_master").getInt() == 0,
            "plug-in server cache path already contains unrelated data");
    db.exec("PRAGMA application_id = " + std::to_string(cacheApplicationId));
    db.exec("PRAGMA user_version = " + std::to_string(cacheVersion));
    db.exec("CREATE TABLE plugin_server_catalogue("
            "server_fingerprint TEXT NOT NULL, format TEXT NOT NULL, uid TEXT NOT NULL, "
            "name TEXT NOT NULL, server_token TEXT NOT NULL, instrument INTEGER NOT NULL, "
            "PRIMARY KEY(server_fingerprint, format, uid)) STRICT");
}

} // namespace

std::span<const std::uint8_t> PluginServerDiscovery::query() const noexcept {
    return enabled_ && !audio::onAudioThread ? std::span<const std::uint8_t>(stockQuery)
                                            : std::span<const std::uint8_t>{};
}

std::vector<PluginServerAdvertisement> PluginServerDiscovery::receive(
    std::span<const std::uint8_t> datagram, std::string_view sourceHost) const {
    if (!enabled_ || audio::onAudioThread || datagram.size() > maximumDatagram ||
        !allowedText(sourceHost, 255)) return {};
    try {
        DnsReply reply(datagram);
        const auto records = reply.records();
        std::vector<PluginServerAdvertisement> result;
        for (const auto& ptr : records) {
            if (ptr.type != dnsPtr || ptr.owner != serviceName) continue;
            const auto name = reply.instance(ptr);
            const auto key = folded(name);
            if (!key.ends_with(serviceSuffix)) continue;
            const auto* srv = uniqueRecord(records, dnsSrv, key);
            const auto* txt = uniqueRecord(records, dnsTxt, key);
            if (srv == nullptr || txt == nullptr) continue;
            const auto info = reply.info(*txt);
            if (!info) continue;
            const auto uuid = advertisedUuid(info->value("UUID", ""));
            const auto version = info->value("V", std::string{});
            const auto instanceName = name.substr(0, name.size() - serviceSuffix.size());
            if (!uuid || !allowedText(instanceName, 63) || !allowedText(version, 64)) continue;
            const auto ttl = std::min({ptr.ttl, srv->ttl, txt->ttl, 3600u});
            result.push_back({*uuid, instanceName, version,
                {std::string(sourceHost), reply.port(*srv)}, ttl});
        }
        return result;
    } catch (const MalformedPacket&) {
        return {};
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

std::optional<std::vector<PluginServerAdvertisement>> PluginServerDiscovery::browse(
    PluginServerBrowseOptions options, std::string& error) const {
    if (audio::onAudioThread) return std::nullopt;
    error.clear();
    std::vector<PluginServerAdvertisement> servers;
    const auto packet = query();
    if (packet.empty()) return servers;
    using namespace std::chrono_literals;
    if (options.timeout <= 0ms || options.timeout > 5s || options.loopbackPort == 0) {
        error = "invalid plug-in server browse bounds";
        return std::nullopt;
    }
    using namespace plugin_socket;
#ifdef _WIN32
    static const SocketRuntime runtime;
    if (!runtime.ready) {
        error = "could not initialize UDP for plug-in server browse";
        return std::nullopt;
    }
#endif
    const auto port = options.loopbackPort.value_or(multicastDnsPort);
    const auto deadline = Clock::now() + options.timeout;
    udp::BrowseSocket socket;
    if (!udp::sendQuery(socket.handle, packet, options.loopbackPort.has_value(), port)) {
        error = "could not send plug-in server browse query";
        return std::nullopt;
    }
    std::array<std::uint8_t, maximumDatagram + 1> buffer{};
    for (int datagrams = 0; datagrams < maximumReceivedDatagrams;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero()) break;
        SocketPollFd descriptor{socket.handle, POLLIN, 0};
        const auto timeout = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        const int ready = socketPoll(descriptor, static_cast<int>(timeout));
        if (ready == 0) break;
        if (ready < 0 && interrupted(socketError())) continue;
        if (ready < 0 || !(descriptor.revents & POLLIN)) {
            error = "plug-in server browse socket wait failed";
            return std::nullopt;
        }
        sockaddr_in peer{};
        constexpr auto peerLength = static_cast<SocketLength>(sizeof(peer));
        auto size = peerLength;
        const auto bytes = recvfrom(socket.handle, reinterpret_cast<char*>(buffer.data()),
            static_cast<udp::DatagramLength>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&peer), &size);
        if (bytes < 0) {
            const int status = socketError();
            if (interrupted(status) || wouldBlock(status)) continue;
            if (udp::oversized(status)) {
                ++datagrams;
                continue;
            }
            error = "could not read plug-in server browse response";
            return std::nullopt;
        }
        ++datagrams;
        if (size != peerLength || peer.sin_family != AF_INET || ntohs(peer.sin_port) != port ||
            (options.loopbackPort && peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))) continue;
        std::array<char, INET_ADDRSTRLEN> host{};
        if (inet_ntop(AF_INET, &peer.sin_addr, host.data(), host.size()) == nullptr) continue;
        auto found = receive(std::span(buffer).first(static_cast<std::size_t>(bytes)), host.data());
        for (auto& entry : found) {
            const auto sameServer = [&](const PluginServerAdvertisement& existing) {
                return existing.advertisedUuid == entry.advertisedUuid && existing.address == entry.address;
            };
            const auto existing = std::ranges::find_if(servers, sameServer);
            if (existing != servers.end()) *existing = std::move(entry);
            else servers.push_back(std::move(entry));
            if (servers.size() == maximumDiscoveredServers) return servers;
        }
    }
    return servers;
}

std::filesystem::path pluginServerCataloguePath(appdata::App app) {
    const auto cache = appdata::pathsFor(app).cache;
    if (cache.empty()) return {};
    return cache / appdata::displayName(app) / "plugin-servers.sqlite";
}

PluginServerCatalogue::PluginServerCatalogue(std::filesystem::path database)
    : database_(std::move(database)) {}

bool PluginServerCatalogue::refresh(const PluginCatalogueScope& scope,
    const PluginCatalogueGrant& grant, const PluginCatalogueQuery& query, std::string& error) const {
    if (audio::onAudioThread) return false;
    error.clear();
    try {
        require(database_.is_absolute(), "plug-in server catalogue needs an absolute app cache path");
        require(allowedText(scope.projectUuid, 128) && allowedText(scope.serverFingerprint, 512) &&
                allowedText(scope.address.host, 255) && scope.address.port != 0,
                "plug-in server catalogue requires a scoped project, server and destination");
        require(grant && grant(scope), "plug-in server catalogue contact requires consent");
        require(static_cast<bool>(query), "plug-in server catalogue query is unavailable");
        auto plugins = query(scope, error);
        if (!plugins) {
            if (error.empty()) error = "plug-in server catalogue query failed";
            return false;
        }
        require(plugins->size() <= maximumPlugins, "plug-in server catalogue exceeds its entry bound");
        for (auto& plugin : *plugins) validatePlugin(plugin);
        std::ranges::sort(*plugins, [](const ServerPlugin& a, const ServerPlugin& b) {
            return std::tie(a.format, a.uid) < std::tie(b.format, b.uid);
        });
        const auto repeated = std::ranges::adjacent_find(*plugins, [](const ServerPlugin& a, const ServerPlugin& b) {
            return a.format == b.format && a.uid == b.uid;
        });
        require(repeated == plugins->end(), "plug-in server catalogue repeats a plug-in identity");

        (void) cacheExists(database_);
        std::filesystem::create_directories(database_.parent_path());
        SQLite::Database db(utf8(database_), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE, 1000);
        SQLite::Transaction change(db, SQLite::TransactionBehavior::IMMEDIATE);
        initializeCache(db);
        validateSqliteType(db);
        SQLite::Statement remove(db, "DELETE FROM plugin_server_catalogue WHERE server_fingerprint = ?");
        remove.bind(1, scope.serverFingerprint);
        remove.exec();
        SQLite::Statement write(db, "INSERT INTO plugin_server_catalogue VALUES (?, ?, ?, ?, ?, ?)");
        for (const auto& plugin : *plugins) {
            write.bind(1, scope.serverFingerprint);
            write.bind(2, plugin.format);
            write.bind(3, plugin.uid);
            write.bind(4, plugin.name);
            write.bind(5, plugin.serverToken);
            write.bind(6, plugin.instrument ? 1 : 0);
            write.exec();
            write.reset();
        }
        change.commit();
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool PluginServerCatalogue::refreshStock(const PluginCatalogueScope& scope,
    const PluginCatalogueGrant& grant, const StockPluginIdentity& resolve, std::string& error) const {
    if (audio::onAudioThread) return false;
    return refresh(scope, grant, [&](const PluginCatalogueScope& authorized, std::string& issue)
            -> std::optional<std::vector<ServerPlugin>> {
        require(static_cast<bool>(resolve), "stock catalogue needs a full VST3 class ID resolver");
        const device::AudioGridderEndpoint endpoint{authorized.address.host, authorized.address.port};
        std::vector<device::AudioGridderCatalogueEntry> stock;
        if (!device::readAudioGridderCatalogue(endpoint, stock, issue)) return std::nullopt;
        std::vector<ServerPlugin> plugins;
        plugins.reserve(stock.size());
        for (auto& item : stock) {
            if (auto uid = resolve(authorized, item.serverToken)) {
                plugins.push_back({std::move(item.format), std::move(*uid), std::move(item.name),
                    std::move(item.serverToken), item.instrument});
            }
        }
        return plugins;
    }, error);
}

std::optional<std::vector<ServerPlugin>> PluginServerCatalogue::cached(
    std::string_view serverFingerprint, std::string& error) const {
    if (audio::onAudioThread) return std::nullopt;
    error.clear();
    try {
        require(database_.is_absolute() && allowedText(serverFingerprint, 512),
                "invalid plug-in server catalogue lookup");
        std::vector<ServerPlugin> result;
        if (!cacheExists(database_)) return result;
        SQLite::Database db(utf8(database_), SQLite::OPEN_READONLY, 1000);
        validateSqliteType(db);
        SQLite::Statement rows(db, "SELECT format, uid, name, server_token, instrument, "
            "length(CAST(format AS BLOB)), length(CAST(uid AS BLOB)), length(CAST(name AS BLOB)), "
            "length(CAST(server_token AS BLOB)) FROM plugin_server_catalogue WHERE server_fingerprint = ? "
            "ORDER BY format, uid LIMIT ?");
        rows.bind(1, std::string(serverFingerprint));
        rows.bind(2, static_cast<int>(maximumPlugins + 1));
        while (rows.executeStep()) {
            const auto instrument = rows.getColumn(4).getInt64();
            require(result.size() < maximumPlugins && (instrument == 0 || instrument == 1) &&
                    rows.getColumn(0).isText() && rows.getColumn(1).isText() &&
                    rows.getColumn(2).isText() && rows.getColumn(3).isText() &&
                    rows.getColumn(4).isInteger() &&
                    rows.getColumn(5).getInt64() == 4 && rows.getColumn(6).getInt64() <= 512 &&
                    rows.getColumn(7).getInt64() <= 256 && rows.getColumn(8).getInt64() <= 512,
                    "invalid entry bounds in the plug-in server catalogue");
            ServerPlugin plugin{rows.getColumn(0).getString(), rows.getColumn(1).getString(),
                rows.getColumn(2).getString(), rows.getColumn(3).getString(), instrument != 0};
            const auto uid = plugin.uid;
            validatePlugin(plugin);
            require(uid == plugin.uid, "cached plug-in catalogue entry is not canonical");
            result.push_back(std::move(plugin));
        }
        return result;
    } catch (const std::exception& failure) {
        error = failure.what();
        return std::nullopt;
    }
}

} // namespace adi::net
