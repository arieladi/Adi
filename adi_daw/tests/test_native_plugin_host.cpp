// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/audio/io_audit.hpp"
#include "adi/engine/offline_render_scope.hpp"
#include "juce/clap_host.hpp"
#include "juce/native_plugin_transport.hpp"
#include "juce/native_plugin_wire.hpp"
#include "juce/net_native_plugin_parameters.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

using namespace adi;
namespace wire = engine::clap_wire;
namespace protocol = net::native_plugin_wire;
using protocol::Message;

constexpr auto pluginId = "org.adi.test.native-host";
constexpr auto pluginName = "Native host test";
constexpr std::int32_t frames = 64;
constexpr std::int32_t pluginLatency = 7;
constexpr std::uint32_t gainParameter = 0x123456;
constexpr std::uint32_t automationFrame = 40;
constexpr double sampleRate = 48000;
constexpr double leftTuning = 0.12500000000000022;
constexpr double rightTuning = -0.37500000000000044;
constexpr auto pitch = static_cast<std::uint16_t>(ExpressionDim::Pitch);
using Pcm = std::array<float, 2 * frames>;

void require(bool result, std::string_view reason) {
    if (!result) throw std::runtime_error(std::string(reason));
}

std::unique_ptr<device::DeviceInstance> openPlugin(device::ClapHost& host, const char* path) {
    device::ClapPluginRef ref;
    ref.bundlePath = path;
    ref.id = pluginId;
    ref.name = pluginName;
    ref.isInstrument = true;
    std::string error;
    auto instance = host.makeDevice(ref, sampleRate, frames, error);
    require(instance && instance->loaded(), error);
    auto* clap = dynamic_cast<device::ClapDevice*>(instance.get());
    require(clap && clap->notePorts().input &&
            (clap->notePorts().supported & CLAP_NOTE_DIALECT_CLAP), "fixture lacks its CLAP note port");
    clap->setNoteDialect(device::ClapDialectChoice::Clap);
    clap->setOffline(true);
    clap->prepare(sampleRate, frames);
    require(clap->latencySamples() == pluginLatency, "the fixture did not declare seven samples");
    return instance;
}

Pcm render(device::DeviceInstance& plugin, std::span<const engine::Event> events) {
    const Pcm silence{};
    Pcm output{};
    const std::array<const float*, 2> inputs{silence.data(), silence.data() + frames};
    const std::array<float*, 2> outputs{output.data(), output.data() + frames};
    engine::NodeIo io;
    io.in = inputs.data();
    io.out = outputs.data();
    io.channels = 2;
    io.frames = frames;
    io.events.first = events.data();
    io.events.count = static_cast<std::int32_t>(events.size());
    const audio::CallbackScope callback;
    plugin.process(io);
    return output;
}

bool receive(protocol::Header& header, std::vector<std::byte>& payload) {
    std::array<std::byte, protocol::kHeaderBytes> bytes{};
    const auto read = std::fread(bytes.data(), 1, bytes.size(), stdin);
    if (read == 0 && std::feof(stdin)) return false;
    require(read == bytes.size() && protocol::decodeHeader(bytes, header), "invalid fixture protocol header");
    payload.resize(header.bodyBytes);
    require(payload.empty() || std::fread(payload.data(), 1, payload.size(), stdin) == payload.size(),
            "incomplete fixture protocol message");
    return true;
}

void send(Message message, std::span<const std::byte> payload = {}) {
    const auto header = protocol::encodeHeader(message, payload.size());
    require(std::fwrite(header.data(), 1, header.size(), stdout) == header.size() &&
            (payload.empty() || std::fwrite(payload.data(), 1, payload.size(), stdout) == payload.size()) &&
            std::fflush(stdout) == 0, "could not write fixture protocol reply");
}

engine::Event graphEvent(const wire::Event& event) {
    if (event.kind == wire::Kind::NormalizedParamValue) {
        require(event.flags == 0 && event.port == -1 && event.channel == -1 &&
                event.key == -1 && event.noteId == -1 && event.selector == gainParameter &&
                event.frame == automationFrame && event.value == 0.0,
                "the fixture received a different normalized parameter value or frame");
        return {.frame = static_cast<std::int32_t>(event.frame), .type = engine::EventType::ParamValue,
                .paramId = event.selector, .value = event.value};
    }
    require(event.flags == 0 && event.port == 0 && event.channel == 0 &&
            (event.noteId == 11 || event.noteId == 12), "unexpected fixture note address");
    engine::Event note;
    note.frame = static_cast<std::int32_t>(event.frame);
    note.noteId = static_cast<std::uint64_t>(event.noteId);
    note.value = event.value;
    if (event.kind == wire::Kind::NoteOn || event.kind == wire::Kind::NoteOff) {
        require(event.key == 69, "the fixture expects overlapping notes of the same pitch");
        note.type = event.kind == wire::Kind::NoteOn ? engine::EventType::NoteOn : engine::EventType::NoteOff;
        note.dim = static_cast<std::uint16_t>(event.key);
        return note;
    }
    require(event.kind == wire::Kind::NoteExpression && event.selector == CLAP_NOTE_EXPRESSION_TUNING,
            "the fixture expected a native CLAP tuning expression");
    const auto left = event.noteId == 11;
    require(event.frame == (left ? 10u : 17u) && event.value == (left ? leftTuning : rightTuning),
            "the fixture received a different per-note frame or full-precision expression");
    note.type = engine::EventType::NoteExpression;
    note.dim = pitch;
    return note;
}

void hostPlugin(const char* path) {
#if defined(_WIN32)
    require(_setmode(_fileno(stdin), _O_BINARY) != -1 && _setmode(_fileno(stdout), _O_BINARY) != -1,
            "cannot use binary fixture pipes");
#endif
    protocol::Header header;
    protocol::Hello hello;
    std::vector<std::byte> payload;
    require(receive(header, payload) && header.message == Message::Hello &&
            protocol::decodeHello(payload, hello) && hello.pluginId == pluginId &&
            hello.sampleRate == sampleRate && hello.channels == 2 && hello.frames == frames &&
            hello.offline && hello.instrument, "fixture refused the hello or requested plug-in");

    device::ClapHost host;
    auto instance = openPlugin(host, path);
    const auto gainId = device::ClapDevice::paramIdToText(gainParameter);
    send(Message::Ready, protocol::encodeReady({instance->latencySamples(), true, true}));
    std::uint64_t sequence = 0;
    int expressionCount = 0;
    int parameterReads = 0, parameterWrites = 0, automationEvents = 0;
    while (receive(header, payload)) {
        if (header.message == Message::ReadParameters) {
            const auto* descriptor = instance->paramAt(0);
            require(instance->paramCount() == 1 && descriptor && descriptor->id == gainId,
                    "missing real CLAP fixture parameter descriptor");
            const std::array manifest{
                device::RemoteParameter{gainParameter, *descriptor, instance->getParam(gainId)},
            };
            require(protocol::encodeParameters(manifest, payload), "could not encode the CLAP parameter descriptor");
            send(Message::Parameters, payload);
            ++parameterReads;
        } else if (header.message == Message::SetParameter) {
            std::uint32_t selector = 0;
            double normalized = 0;
            require(protocol::decodeParameterValue(payload, selector, normalized) && selector == gainParameter &&
                    instance->setParam(gainId, device::ParamValue::fromNormalized(normalized)),
                    "the real CLAP fixture rejected a parameter value");
            const auto applied = instance->getParam(gainId);
            send(Message::ParameterValue, protocol::encodeParameterValue(selector, applied.normalized));
            ++parameterWrites;
        } else if (header.message == Message::FetchState) {
            const auto state = instance->saveState("chunk");
            require(!state.empty(), "CLAP fixture state capture failed");
            send(Message::State, std::as_bytes(std::span(state)));
        } else if (header.message == Message::RestoreState) {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(payload.size());
            for (const auto byte : payload) bytes.push_back(std::to_integer<std::uint8_t>(byte));
            require(instance->loadState("chunk", bytes), "CLAP fixture state restore failed");
            send(Message::Ack);
        } else if (header.message == Message::Process) {
            wire::Block request;
            std::string error;
            require(wire::decodeBlock(payload, request, error) && request.sequence == sequence++ &&
                    request.channels == 0 && request.frames == frames, "invalid fixture audio block");
            std::vector<engine::Event> events;
            events.reserve(request.events.size());
            for (const auto& decoded : request.events) {
                events.push_back(graphEvent(decoded));
                if (decoded.kind == wire::Kind::NoteExpression) ++expressionCount;
                if (decoded.kind == wire::Kind::NormalizedParamValue) ++automationEvents;
            }
            const auto pcm = render(*instance, events);
            wire::Block result;
            result.sequence = request.sequence;
            result.channels = 2;
            result.frames = frames;
            result.audio.assign(pcm.begin(), pcm.end());
            require(wire::encodeBlock(result, payload, error), error);
            std::vector<std::byte> reply(4 + payload.size());
            protocol::put(reply, 0, static_cast<std::uint32_t>(instance->latencySamples()));
            std::copy(payload.begin(), payload.end(), reply.begin() + 4);
            send(Message::Result, reply);
        } else {
            throw std::runtime_error("unexpected loopback CLAP fixture message");
        }
    }
    require(sequence == 3 && expressionCount == 2, "the CLAP host did not process every test block and expression");
    require(parameterReads >= 2 && parameterWrites == 1 && automationEvents == 1,
            "the CLAP fixture did not receive metadata, owner write and sample-offset automation");
}

void testRemote(const char* path, std::uint16_t port) {
    device::ClapHost host;
    auto local = openPlugin(host, path);
    const std::vector<std::uint8_t> initial{2}, saved{1};
    require(local->saveState("chunk") == initial && local->loadState("chunk", saved),
            "local CLAP fixture state did not change");

    device::NativePluginEndpoint endpoint;
    endpoint.address = {"127.0.0.1", port};
    device::RemoteDevice remote({"clap", pluginId, pluginName, {}, {}}, 2,
        std::make_unique<device::NativePluginProcessor>(endpoint, device::NativePlugin{pluginId, true}),
        engine::EventFlow::Consume);
    remote.setOffline(true);
    remote.prepare(sampleRate, frames);
    require(remote.loaded(), remote.processingIssue());
    require(remote.stateRoles() == std::vector<std::string>{"chunk"} &&
            remote.saveState("chunk") == initial && remote.loadState("chunk", saved) &&
            remote.saveState("chunk") == saved && remote.stateCaptureIssue().empty(),
            "remote CLAP state restore and fresh capture did not match");
    require(remote.latencySamples() == frames + pluginLatency,
            "the remote latency must include the plug-in and its one-block pipeline");

    const auto gainId = device::ClapDevice::paramIdToText(gainParameter);
    const auto* gain = remote.paramAt(0);
    require(remote.paramCount() == 1 && gain && gain->id == gainId && gain->name == "Gain" &&
            gain->automatable && gain->domain == device::ParamDomain::Real && gain->hasRealRange &&
            gain->minReal == .5 && gain->maxReal == 1.5 && gain->defaultValue.hasReal &&
            gain->defaultValue.normalized == .75 && gain->defaultValue.real == 1.25,
            "remote CLAP parameter did not retain the plug-in's stable ID and range");
    const auto initialGain = remote.getParam(gainId);
    require(initialGain.hasReal && initialGain.normalized == .75 && initialGain.real == 1.25,
            "remote CLAP parameter did not report the fixture's initial value");
    const auto unityGain = device::ParamValue::fromNormalized(.5);
    require(local->setParam(gainId, unityGain) && remote.setParam(gainId, unityGain),
            "the gain change did not reach the local and remote CLAP plug-ins");
    const auto appliedGain = remote.getParam(gainId);
    require(appliedGain.hasReal && appliedGain.normalized == .5 && appliedGain.real == 1.0,
            "the remote instance did not mirror the acknowledged gain");

    using enum engine::EventType;
    const std::array first{
        engine::Event{.frame = 3, .type = NoteOn, .dim = 69, .noteId = 11, .value = .5},
        engine::Event{.frame = 5, .type = NoteOn, .dim = 69, .noteId = 12, .value = .75},
        engine::Event{.frame = 10, .type = NoteExpression, .dim = pitch, .noteId = 11, .value = leftTuning},
        engine::Event{.frame = 17, .type = NoteExpression, .dim = pitch, .noteId = 12, .value = rightTuning},
        engine::Event{.frame = static_cast<std::int32_t>(automationFrame), .type = ParamValue,
                      .paramId = gainParameter, .value = 0.0},
    };
    const std::array second{
        engine::Event{.frame = 13, .type = NoteOff, .dim = 69, .noteId = 12},
        engine::Event{.frame = 31, .type = NoteOff, .dim = 69, .noteId = 11},
    };
    const auto localFirst = render(*local, first);
    const auto localSecond = render(*local, second);
    require(localFirst[9] == 0.0f && localFirst[10] == .25f && localFirst[17] == .3125f &&
            localFirst[frames + 11] == 0.0f && localFirst[frames + 12] == .375f &&
            localFirst[frames + 17] == .375f && localFirst[frames + 24] == .1875f &&
            localSecond[frames + 20] == 0.0f && localSecond[20] == .15625f && localSecond[38] == 0.0f,
            "the real CLAP fixture did not render restored gain and separate note expressions at expected offsets");
    require(localFirst[46] == .3125f && localFirst[47] == .15625f &&
            localFirst[frames + 46] == .1875f && localFirst[frames + 47] == .09375f,
            "normalized CLAP automation did not halve both voices at the delayed sample");

    {
        const engine::OfflineRenderScope offline;
        require(render(remote, first) == Pcm{} && render(remote, second) == localFirst &&
                render(remote, {}) == localSecond && remote.processingIssue().empty(),
                "the TCP-hosted CLAP PCM did not match the local plug-in with one-block compensation");
    }
    require(remote.droppedBlocks() == 0 && remote.saveState("chunk") == saved &&
            remote.stateCaptureIssue().empty(), "offline TCP rendering lost audio or the captured state");
    std::printf("Real CLAP over native TCP: 256 exact samples, independent note tuning, state round trip, "
                "gain control and sample-offset automation with %d samples of latency\n", remote.latencySamples());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "host") {
            hostPlugin(argv[2]);
        } else {
            require(argc == 4 && std::string_view(argv[1]) == "client", "expected the test mode and CLAP path");
            const std::string_view token(argv[3]);
            std::uint16_t port = 0;
            const auto* const end = token.data() + token.size();
            const auto parsed = std::from_chars(token.data(), end, port);
            require(parsed.ec == std::errc{} && parsed.ptr == end && port != 0,
                    "invalid fixture TCP port");
            testRemote(argv[2], port);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
