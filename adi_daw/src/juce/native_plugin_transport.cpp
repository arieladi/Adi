// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_socket.hpp"

#include "juce/native_plugin_transport.hpp"
#include "juce/native_plugin_wire.hpp"
#include "juce/net_native_plugin_parameters.hpp"

#include "adi/engine/mpe_output.hpp"
#include "juce/clap_host.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace adi::device {
namespace {

using net::plugin_socket::Clock;
using net::plugin_socket::Deadline;
using net::plugin_socket::PluginSocket;
namespace wire = net::native_plugin_wire;
namespace blockWire = engine::clap_wire;
using Message = wire::Message;

bool loopback(const net::PluginServerAddress& endpoint, in_addr& address) noexcept {
    constexpr std::uint32_t loopbackPrefix = 0x7f000000;
    return endpoint.port != 0 && endpoint.host.find('\0') == std::string::npos &&
           inet_pton(AF_INET, endpoint.host.c_str(), &address) == 1 &&
           (ntohl(address.s_addr) & 0xff000000u) == loopbackPrefix;
}

std::optional<blockWire::Event> clipEvent(const engine::Event& input, std::int32_t frames) {
    using Type = engine::EventType;
    using Kind = blockWire::Kind;
    if (input.frame < 0 || input.frame >= frames || !std::isfinite(input.value)) return std::nullopt;
    blockWire::Event output;
    output.frame = static_cast<std::uint32_t>(input.frame);
    switch (input.type) {
        case Type::NoteOn:
        case Type::NoteOff:
        case Type::NoteExpression: {
            static constexpr engine::ExpressionCaps caps{};
            engine::MpeOut event;
            if (!engine::noteExpressionOut(input, caps, event)) return std::nullopt;
            output.noteId = engine::pluginNoteId(event.noteId);
            output.port = 0;
            output.channel = event.channel;
            output.key = event.key;
            if (input.type == Type::NoteExpression) {
                const auto expression = clapExprFor(static_cast<ExpressionDim>(event.dim));
                if (expression < 0) return std::nullopt;
                output.kind = Kind::NoteExpression;
                output.selector = static_cast<std::uint32_t>(expression);
                output.value = expression == CLAP_NOTE_EXPRESSION_TUNING
                    ? semitonesToClapTuning(event.plain)
                    : std::clamp(event.plain, 0.0, expression == CLAP_NOTE_EXPRESSION_VOLUME ? 4.0 : 1.0);
            } else {
                output.kind = input.type == Type::NoteOn ? Kind::NoteOn : Kind::NoteOff;
                output.value = event.value;
            }
            return output;
        }
        case Type::Controller:
            output.kind = Kind::Controller;
            output.port = 0;
            output.channel = input.channel;
            output.selector = input.paramId;
            output.value = input.value;
            return output;
        case Type::ParamValue:
        case Type::ParamMod:
            output.kind = input.type == Type::ParamValue ? Kind::NormalizedParamValue : Kind::NormalizedParamMod;
            output.noteId = engine::pluginNoteId(input.noteId);
            output.selector = input.paramId;
            output.value = input.value;
            return output;
    }
    return std::nullopt;
}

} // namespace

struct NativePluginProcessor::Connection {
    NativePluginEndpoint endpoint;
    NativePlugin plugin;
    PluginSocket socket;
    wire::Ready ready;
    bool active = false;
    std::uint32_t channels = 0, frames = 0;
    std::uint64_t sequence = 0;
    std::vector<std::byte> reply;

    Connection(NativePluginEndpoint server, NativePlugin target)
        : endpoint(std::move(server)), plugin(std::move(target)) {}

    bool fail(std::string& error, bool close = true) {
        if (close) {
            active = false;
            socket.reset();
        }
        error = "server plug-in " + plugin.id + ": " + error;
        return false;
    }

    bool request(Message command, std::span<const std::byte> body, Message expected,
                 Deadline deadline, std::string& error) {
        if (!wire::validLength(command, body.size())) {
            error = "invalid native server request length";
            return fail(error);
        }
        const auto header = wire::encodeHeader(command, body.size());
        std::array<std::byte, wire::kHeaderBytes> response{};
        if (!socket.write(header, deadline, error) || !socket.write(body, deadline, error) ||
            !socket.read(response, deadline, error)) return fail(error);
        wire::Header replyHeader;
        if (!wire::decodeHeader(response, replyHeader) ||
            (replyHeader.message != expected && replyHeader.message != Message::Failure)) {
            error = "invalid native server reply header";
            return fail(error);
        }
        reply.resize(replyHeader.bodyBytes);
        if (!socket.read(reply, deadline, error)) return fail(error);
        if (replyHeader.message == Message::Failure) {
            error = "server refused the operation";
            return fail(error, command != Message::FetchState && command != Message::RestoreState &&
                               command != Message::ReadParameters && command != Message::SetParameter);
        }
        return true;
    }

    bool process(const RemoteBlock& input, std::span<float> output,
                 std::vector<blockWire::Event>* outputEvents, std::string& error) {
        const auto total = static_cast<std::size_t>(channels) * frames;
        if (!active || input.channels != static_cast<std::int32_t>(channels) ||
            input.frames != static_cast<std::int32_t>(frames) ||
            input.audio.size() != total || output.size() != total ||
            input.events.size() > blockWire::kMaxEvents || sequence == std::numeric_limits<std::uint64_t>::max()) {
            error = "native audio stream is closed or block shape is invalid";
            return fail(error);
        }
        const auto deadline = Clock::now() + endpoint.blockTimeout;
        blockWire::Block block;
        block.sequence = sequence;
        block.channels = plugin.instrument ? 0u : channels;
        block.frames = frames;
        block.transport = input.transport;
        if (!plugin.instrument) block.audio.assign(input.audio.begin(), input.audio.end());
        block.events.reserve(input.events.size());
        for (const auto& event : input.events) {
            auto converted = clipEvent(event, input.frames);
            if (!converted) {
                error = "invalid or unsupported CLAP input event";
                return fail(error);
            }
            block.events.push_back(*converted);
        }
        std::vector<std::byte> payload;
        if (!blockWire::encodeBlock(block, payload, error)) return fail(error);
        if (!request(Message::Process, payload, Message::Result, deadline, error)) return false;
        if (wire::get<std::uint32_t>(reply, 0) != static_cast<std::uint32_t>(ready.pluginLatency)) {
            error = "server changed plug-in latency during the stream";
            return fail(error);
        }
        blockWire::Block result;
        if (!blockWire::decodeBlock(std::span(reply).subspan(4), result, error)) return fail(error);
        if (result.sequence != sequence || result.channels != channels || result.frames != frames) {
            error = "native audio response sequence or shape changed";
            return fail(error);
        }
        std::copy(result.audio.begin(), result.audio.end(), output.begin());
        if (outputEvents) *outputEvents = std::move(result.events);
        ++sequence;
        return true;
    }
};

NativePluginProcessor::NativePluginProcessor(NativePluginEndpoint endpoint, NativePlugin plugin)
    : connection_(std::make_unique<Connection>(std::move(endpoint), std::move(plugin))) {}

NativePluginProcessor::~NativePluginProcessor() = default;

bool NativePluginProcessor::prepare(double sampleRate, std::int32_t channels, std::int32_t frames, bool offline,
                                    std::int32_t& pluginLatency, std::string& error) {
    release();
    error.clear();
    auto& connection = *connection_;
    in_addr address{};
    if (!loopback(connection.endpoint.address, address)) {
        error = "plain native transport requires an explicitly authorized numeric IPv4 loopback endpoint";
        return connection.fail(error);
    }
    using namespace std::chrono_literals;
    if (connection.endpoint.setupTimeout <= 0ms || connection.endpoint.setupTimeout > 60s ||
        connection.endpoint.blockTimeout <= 0ms || connection.endpoint.blockTimeout > 10s) {
        error = "invalid native server timeout";
        return connection.fail(error);
    }
    const wire::Hello hello{sampleRate, static_cast<std::uint32_t>(channels),
                            static_cast<std::uint32_t>(frames), offline, connection.plugin.instrument,
                            connection.plugin.id};
    std::vector<std::byte> payload;
    if (!wire::encodeHello(hello, payload)) {
        error = "invalid native plug-in activation";
        return connection.fail(error);
    }
    const auto deadline = Clock::now() + connection.endpoint.setupTimeout;
    if (!connection.socket.open(address, connection.endpoint.address.port, deadline, error)) return connection.fail(error);
    if (!connection.request(Message::Hello, payload, Message::Ready, deadline, error)) return false;
    if (!wire::decodeReady(connection.reply, connection.ready)) {
        error = "invalid native plug-in activation reply";
        return connection.fail(error);
    }
    connection.channels = hello.channels;
    connection.frames = hello.frames;
    connection.sequence = 0;
    connection.active = true;
    pluginLatency = connection.ready.pluginLatency;
    return true;
}

bool NativePluginProcessor::exchange(const RemoteBlock& input, std::span<float> output, std::string& error) {
    error.clear();
    return connection_->process(input, output, nullptr, error);
}

bool NativePluginProcessor::exchangeWithEvents(const RemoteBlock& input, std::span<float> output,
                                              std::vector<blockWire::Event>& events, std::string& error) {
    error.clear();
    return connection_->process(input, output, &events, error);
}

bool NativePluginProcessor::fetchState(std::vector<std::uint8_t>& state, StateDeadline deadline, std::string& error) {
    error.clear();
    auto& connection = *connection_;
    if (!connection.active || !connection.ready.state) {
        error = "the native server does not currently provide plug-in state";
        return connection.fail(error, false);
    }
    if (!connection.request(Message::FetchState, {}, Message::State, deadline, error)) return false;
    state.resize(connection.reply.size());
    std::ranges::copy(connection.reply, std::as_writable_bytes(std::span(state)).begin());
    return true;
}

bool NativePluginProcessor::restoreState(std::span<const std::uint8_t> state, StateDeadline deadline,
                                         std::string& error) {
    error.clear();
    auto& connection = *connection_;
    if (!connection.active || !connection.ready.state || state.size() > wire::kMaxStateBytes) {
        error = "the native server cannot restore this plug-in state";
        return connection.fail(error, false);
    }
    return connection.request(Message::RestoreState, std::as_bytes(state), Message::Ack, deadline, error);
}

bool NativePluginProcessor::readParameters(std::vector<RemoteParameter>& parameters,
                                           StateDeadline deadline, std::string& error) {
    error.clear();
    auto& connection = *connection_;
    if (!connection.active) {
        error = "the native plug-in connection is closed";
        return connection.fail(error, false);
    }
    if (!connection.ready.parameters) {
        parameters.clear();
        return true;
    }
    if (!connection.request(Message::ReadParameters, {}, Message::Parameters, deadline, error)) return false;
    if (!wire::decodeParameters(connection.reply, parameters)) {
        error = "invalid native plug-in parameter manifest";
        return connection.fail(error);
    }
    return true;
}

bool NativePluginProcessor::setParameter(std::uint32_t selector, double normalized, double& applied,
                                         StateDeadline deadline, std::string& error) {
    error.clear();
    auto& connection = *connection_;
    if (!connection.active || !connection.ready.parameters || selector == CLAP_INVALID_ID ||
        !wire::normalized(normalized)) {
        error = "the native server cannot apply this plug-in parameter";
        return connection.fail(error, false);
    }
    if (!connection.request(Message::SetParameter, wire::encodeParameterValue(selector, normalized),
                            Message::ParameterValue, deadline, error)) return false;
    std::uint32_t returned = 0;
    double value = 0;
    if (!wire::decodeParameterValue(connection.reply, returned, value) || returned != selector) {
        error = "invalid native plug-in parameter reply";
        return connection.fail(error);
    }
    applied = value;
    return true;
}

void NativePluginProcessor::release() noexcept {
    connection_->active = false;
    connection_->socket.reset();
}

} // namespace adi::device
