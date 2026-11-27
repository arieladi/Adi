// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_socket.hpp"
#include "juce/audiogridder_transport.hpp"

#include "adi/audio/io_audit.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
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
using Socket = net::plugin_socket::PluginSocket;

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
// Leave room for the inner length and the state string's decimal prefix.
constexpr std::size_t maxStateBytes = 3 * ((maxControlPayload - 20) / 4);
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
    GetPluginSettings = 70,
    SetPluginSettings = 71,
    PluginSettings = 72,
    Parameters = 100,
    Presets = 110,
};

bool writePluginIndex(const Socket& socket, Command command, Deadline deadline, std::string& error) {
    std::array<std::byte, 12> bytes{};
    put(bytes, 0, static_cast<std::int32_t>(command));
    put(bytes, 4, std::int32_t{4});
    // Each admission opens one plug-in, at index 0.
    return socket.write(bytes, deadline, error);
}

// Stock's single-instance state string is decimal byte count, a dot, and
// least-significant-first six-bit digits. The saved ADI chunk is raw VST3
// state so the same bytes can be recalled on a locally hosted instance.
constexpr std::string_view stateAlphabet = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";

std::string encodeState(std::span<const std::uint8_t> bytes) {
    std::string encoded = std::to_string(bytes.size()) + '.';
    encoded.reserve(encoded.size() + (bytes.size() * 8 + 5) / 6);
    unsigned pending = 0;
    unsigned bits = 0;
    for (const auto byte : bytes) {
        pending |= static_cast<unsigned>(byte) << bits;
        bits += 8;
        while (bits >= 6) {
            encoded += stateAlphabet[pending & 63u];
            pending >>= 6u;
            bits -= 6;
        }
    }
    if (bits != 0) encoded += stateAlphabet[pending];
    return encoded;
}

bool decodeState(std::string_view encoded, std::vector<std::uint8_t>& output) {
    const auto separator = encoded.find('.');
    if (separator == std::string_view::npos || separator == 0) return false;
    const auto* sizeEnd = encoded.data() + separator;
    std::size_t count = 0;
    const auto size = std::from_chars(encoded.data(), sizeEnd, count);
    if (size.ec != std::errc{} || size.ptr != sizeEnd || count > maxStateBytes) return false;
    const auto digits = encoded.substr(separator + 1);
    if (digits.size() != (count * 8 + 5) / 6) return false;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(count);
    unsigned pending = 0;
    unsigned bits = 0;
    for (const auto digit : digits) {
        const auto value = stateAlphabet.find(digit);
        if (value == std::string_view::npos) return false;
        pending |= static_cast<unsigned>(value) << bits;
        bits += 6;
        if (bits >= 8) {
            bytes.push_back(static_cast<std::uint8_t>(pending & 255u));
            pending >>= 8u;
            bits -= 8;
        }
    }
    if (bytes.size() != count || pending != 0) return false;
    output = std::move(bytes);
    return true;
}

bool writeControl(const Socket& socket, Command command, std::string_view payload,
                  Deadline deadline, std::string& error) {
    if (payload.size() > maxControlPayload - 4) {
        error = "v13 command exceeds the payload bound";
        return false;
    }
    std::array<std::byte, 12> header{};
    put(header, 0, static_cast<std::int32_t>(command));
    put(header, 4, static_cast<std::int32_t>(4 + payload.size()));
    put(header, 8, static_cast<std::int32_t>(payload.size()));
    return socket.write(header, deadline, error) &&
           socket.write(std::as_bytes(std::span(payload)), deadline, error);
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

bool validEndpoint(const AudioGridderEndpoint& endpoint, in_addr& address, std::string& error) {
    using namespace std::chrono_literals;
    if (endpoint.port != 0 && endpoint.address.find('\0') == std::string::npos &&
        inet_pton(AF_INET, endpoint.address.c_str(), &address) == 1 &&
        endpoint.setupTimeout > 0ms && endpoint.setupTimeout <= 60s) return true;
    error = "invalid numeric IPv4 endpoint or timeout";
    return false;
}

struct HandshakeShape {
    double sampleRate = 48000;
    std::int32_t channels = 2;
    std::int32_t frames = 64;
    bool instrument = false;
    bool unfilteredList = false;
};

bool openWorker(in_addr address, std::uint16_t port, const HandshakeShape& shape, Deadline deadline,
                const std::array<Socket*, 4>& workers, std::string& error) {
    Socket handshake;
    if (!handshake.open(address, port, deadline, error)) return false;
    std::array<std::byte, handshakeRequestSize> request{};
    put(request, 0, protocolVersion);
    put(request, 4, shape.instrument ? std::int32_t{0} : shape.channels);
    put(request, 8, shape.channels);
    put(request, 16, shape.sampleRate);
    put(request, 24, shape.frames);
    static std::atomic<std::uint64_t> nextId{0};
    const auto now = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    put(request, 32, now + nextId.fetch_add(1, std::memory_order_relaxed));
    request[40] = shape.unfilteredList ? std::byte{1} : std::byte{};
    const auto bits = (std::uint64_t{1} << shape.channels) - 1;
    put(request, 48, shape.instrument ? bits : bits | (bits << 32));
    std::array<std::byte, handshakeResponseSize> response{};
    if (!handshake.write(request, deadline, error) || !handshake.read(response, deadline, error))
        return false;
    if (get<std::int32_t>(response, 0) != protocolVersion) {
        error = "server protocol version is not stock v13";
        return false;
    }
    const auto workerPort = get<std::int32_t>(response, 8);
    if (workerPort < 1 || workerPort > 65535) {
        error = "server worker port is invalid";
        return false;
    }
    handshake.reset();
    // The server assigns command, notifications, audio, and screen sockets
    // solely by connection order, including for a catalogue-only request.
    for (auto* worker : workers) {
        if (!worker->open(address, static_cast<std::uint16_t>(workerPort), deadline, error))
            return false;
    }
    return true;
}

constexpr std::size_t maxCatalogueRows = 16384;

// Check depth, element counts, and duplicate keys before building a JSON DOM
// from the server's MessagePack. The control envelope already caps the bytes.
class CatalogueGuard final : public nlohmann::json_sax<Json> {
public:
    bool null() override { return token(); }
    bool boolean(bool) override { return token(); }
    bool number_integer(number_integer_t) override { return token(); }
    bool number_unsigned(number_unsigned_t) override { return token(); }
    bool number_float(number_float_t, const string_t&) override { return token(); }
    bool string(string_t& value) override { return value.size() <= 4096 && token(); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t count) override { return count <= 128 && start(); }
    bool key(string_t& value) override {
        return value.size() <= 128 && token() && scopes_.back().insert(value).second;
    }
    bool end_object() override { return end(); }
    bool start_array(std::size_t count) override { return count <= maxCatalogueRows && start(); }
    bool end_array() override { return end(); }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }
private:
    bool token() { return ++tokens_ <= 500000; }
    bool start() {
        if (scopes_.size() >= 8 || !token()) return false;
        scopes_.emplace_back();
        return true;
    }
    bool end() {
        scopes_.pop_back();
        return true;
    }
    std::vector<std::set<std::string>> scopes_;
    std::size_t tokens_ = 0;
};

bool decodeCatalogue(ReadOnlyBytes bytes, std::vector<AudioGridderCatalogueEntry>& entries) {
    CatalogueGuard bounds;
    if (!Json::sax_parse(bytes, &bounds, Json::input_format_t::msgpack)) return false;
    const auto document = Json::from_msgpack(bytes, /*strict=*/true, /*allow_exceptions=*/false);
    const auto rows = document.find("plugins");
    if (rows == document.end() || !rows->is_array()) return false;
    std::map<std::string, std::size_t> tokenCounts;
    for (const auto& row : *rows) {
        const auto type = row.find("type");
        const auto id = row.find("id2");
        if (type == row.end() || *type != "VST3" || id == row.end() || !id->is_string()) continue;
        const auto& token = id->get_ref<const std::string&>();
        if (!validToken(token)) continue;
        ++tokenCounts[token];
        const auto label = row.find("name");
        const auto instrument = row.find("isInstrument");
        if (label == row.end() || !label->is_string() ||
            instrument == row.end() || !instrument->is_boolean()) continue;
        const auto& name = label->get_ref<const std::string&>();
        if (name.empty() || name.size() > 256 || std::any_of(name.begin(), name.end(),
            [](unsigned char ch) { return ch < 32 || ch == 127; })) continue;
        entries.push_back({token, name, "vst3", instrument->get<bool>()});
    }
    std::erase_if(entries, [&](const auto& entry) { return tokenCounts.at(entry.serverToken) > 1; });
    return true;
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
        if (!validEndpoint(endpoint_, address, error)) return fail(error);
        if (endpoint_.blockTimeout <= 0ms || endpoint_.blockTimeout > 10s) {
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
        const HandshakeShape setup{sampleRate, channels, frames, plugin_.instrument};
        if (!openWorker(address, endpoint_.port, setup, deadline,
                        {&commands_, &notifications_, &audio_, &screen_}, error) ||
            !loadPlugin(deadline, error)) return fail(error);
        ready_ = true;
        stateStreamUsable_ = true;
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

    [[nodiscard]] bool supportsState() const noexcept override { return true; }

    bool fetchState(std::vector<std::uint8_t>& output, StateDeadline deadline, std::string& error) override {
        if (!ready_ || !stateStreamUsable_) {
            error = "the stock server state stream is unavailable";
            return false;
        }
        std::vector<std::byte> payload;
        if (!writePluginIndex(commands_, Command::GetPluginSettings, deadline, error) ||
            !readControl(commands_, Command::PluginSettings, payload, deadline, error)) {
            stateStreamUsable_ = false; // A partial reply cannot frame the next control operation.
            return false;
        }
        if (payload.empty() || !decodeState({reinterpret_cast<const char*>(payload.data()), payload.size()}, output)) {
            error = "invalid stock plug-in state encoding";
            return false;
        }
        return true;
    }

    bool restoreState(std::span<const std::uint8_t> bytes, StateDeadline deadline, std::string& error) override {
        if (!ready_ || !stateStreamUsable_ || bytes.empty() || bytes.size() > maxStateBytes) {
            error = "the stock server cannot restore this plug-in state";
            return false;
        }
        if (!writePluginIndex(commands_, Command::SetPluginSettings, deadline, error) ||
            !writeControl(commands_, Command::PluginSettings, encodeState(bytes), deadline, error)) {
            stateStreamUsable_ = false;
            return false;
        }
        // Stock SET has no receipt. A GET on the same command stream waits
        // until the server dispatches SET before audio starts on its other
        // socket. The plug-in can reserialize valid bytes differently.
        std::vector<std::uint8_t> settled;
        return fetchState(settled, deadline, error);
    }

    void release() noexcept override {
        ready_ = false;
        stateStreamUsable_ = false;
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
    bool stateStreamUsable_ = false;
};

} // namespace

bool readAudioGridderCatalogue(const AudioGridderEndpoint& endpoint,
                               std::vector<AudioGridderCatalogueEntry>& rows, std::string& error) {
    if (audio::onAudioThread) return false;
    error.clear();
    in_addr address{};
    if (!validEndpoint(endpoint, address, error)) return false;
    const auto deadline = Clock::now() + endpoint.setupTimeout;
    Socket commands, notifications, audioSocket, screen;
    std::vector<std::byte> payload;
    const HandshakeShape setup{.unfilteredList = true};
    if (!openWorker(address, endpoint.port, setup, deadline,
                    {&commands, &notifications, &audioSocket, &screen}, error) ||
        !readControl(commands, Command::PluginList, payload, deadline, error)) return false;
    std::vector<AudioGridderCatalogueEntry> entries;
    if (!decodeCatalogue(payload, entries)) {
        error = "invalid stock plug-in catalogue";
        return false;
    }
    if (Clock::now() >= deadline) {
        error = "stock plug-in catalogue timed out";
        return false;
    }
    rows = std::move(entries);
    return true;
}

std::unique_ptr<RemoteProcessor> makeAudioGridderProcessor(
    AudioGridderEndpoint endpoint, AudioGridderPlugin plugin) {
    return std::make_unique<AudioGridderProcessor>(std::move(endpoint), std::move(plugin));
}

} // namespace adi::device
