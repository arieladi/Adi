// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/native_plugin_transport.hpp"

#include "adi/blob.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool check(bool okay, const char* reason) {
    if (!okay) std::fprintf(stderr, "%s\n", reason);
    return okay;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string_view mode(argv[2]);
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    const std::string_view portText(argv[1]);
    std::uint16_t port = 0;
    const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != portText.data() + portText.size() || port == 0) return 2;

    const auto endpoint = mode == "loopback" ? "192.0.2.1" : "127.0.0.1";
    adi::device::NativePluginProcessor transport({{endpoint, port}, 2s, 150ms},
                                               {"org.adi.native-fixture"});
    std::int32_t latency = 0;
    std::string error;
    const bool prepared = transport.prepare(48000, 2, 64, true, latency, error);
    if (mode == "loopback")
        return check(!prepared && error.find("loopback") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "version")
        return check(!prepared && error.find("reply header") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "refused")
        return check(!prepared && error.find("org.adi.native-fixture") != std::string::npos,
                     error.c_str()) ? 0 : 1;
    if (!check(prepared && latency == 7, error.c_str())) return 1;

    const std::vector<std::uint8_t> saved{0, 255, 42, 0};
    if (mode == "pcm" || mode == "state-failure") {
        if (!check(transport.supportsState() && transport.restoreState(saved, Clock::now() + 1s, error),
                   error.c_str())) return 1;
        std::vector<std::uint8_t> read{17, 19};
        const bool fetched = transport.fetchState(read, Clock::now() + 1s, error);
        if (mode == "state-failure") {
            if (!check(!fetched && read == std::vector<std::uint8_t>({17, 19}),
                       "failed state read changed cached bytes")) return 1;
        } else if (!check(fetched && read == saved, error.c_str())) return 1;
    }

    using Type = adi::engine::EventType;
    using Kind = adi::engine::clap_wire::Kind;
    std::array<float, 128> input{}, output{};
    output.fill(-10.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i) / 128.0f;
    const auto pitch = static_cast<std::uint16_t>(adi::ExpressionDim::Pitch);
    const std::array events{
        adi::engine::Event{.frame = 4, .type = Type::NoteOn, .channel = 5, .dim = 69, .noteId = 11, .value = .7},
        adi::engine::Event{.frame = 4, .type = Type::NoteOn, .channel = 5, .dim = 69, .noteId = 12, .value = .7},
        adi::engine::Event{.frame = 4, .type = Type::NoteExpression, .channel = 5, .dim = pitch,
                           .noteId = 12, .value = .1778234567889},
        adi::engine::Event{.frame = 4, .type = Type::NoteExpression, .channel = 5, .dim = pitch,
                           .noteId = 11, .value = -.13923249},
        adi::engine::Event{.frame = 5, .type = Type::Controller, .channel = 5, .paramId = 74,
                           .value = .144123456789},
        adi::engine::Event{.frame = 5, .type = Type::ParamValue, .paramId = 1500, .value = .41},
        adi::engine::Event{.frame = 6, .type = Type::ParamMod, .noteId = 12, .paramId = 1500,
                           .value = -.43217889},
    };
    adi::engine::DeviceTransport position;
    position.sample = 24000;
    position.quarterNotes = 1.125;
    position.bpm = 120.03125;
    position.playing = true;
    const adi::device::RemoteBlock block{2, 64, input, events, position};
    std::vector<adi::engine::clap_wire::Event> replyEvents;
    const auto start = Clock::now();
    const bool processed = transport.exchangeWithEvents(block, output, replyEvents, error);
    if (mode == "sequence" || mode == "latency" || mode == "deadline") {
        const bool explained = mode == "deadline" ? error.find("timed out") != std::string::npos &&
                                                     Clock::now() - start < 1s
                                                  : error.find(mode) != std::string::npos;
        return check(!processed && explained && output[3] == -10 && replyEvents.empty(),
                     error.c_str()) ? 0 : 1;
    }

    if (!check(processed && replyEvents.size() == 1 && replyEvents[0].kind == Kind::NoteEnd &&
               replyEvents[0].frame == 15 && replyEvents[0].noteId == 12 && replyEvents[0].port == 0 &&
               replyEvents[0].channel == 0 && replyEvents[0].key == 69, error.c_str())) return 1;
    const auto maximumError = [&] {
        float maximumDifference = 0;
        for (std::size_t i = 0; i < output.size(); ++i)
            maximumDifference = std::max(maximumDifference, std::abs(output[i] - input[i] * 0.25f));
        return maximumDifference;
    };
    const float typedError = maximumError();
    if (!check(transport.exchange(block, output, error), error.c_str())) return 1;
    const float maximumDifference = std::max(typedError, maximumError());
    if (!check(maximumDifference == 0, "native reply PCM changed")) return 1;
    std::printf("Native loopback: %zu stereo samples, max PCM error %.9g; typed reply and state path verified\n",
                output.size() * 2, static_cast<double>(maximumDifference));
    return 0;
}
