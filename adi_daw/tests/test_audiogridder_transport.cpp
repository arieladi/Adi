// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/audiogridder_transport.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

bool check(bool condition, const char* reason) {
    if (!condition) std::fprintf(stderr, "%s\n", reason);
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string_view portArg(argv[1]);
    std::uint16_t port = 0;
    const auto portRead = std::from_chars(portArg.data(), portArg.data() + portArg.size(), port);
    if (portRead.ec != std::errc{} || portRead.ptr != portArg.data() + portArg.size() || port == 0)
        return 2;

    using namespace std::chrono_literals;
    adi::device::AudioGridderEndpoint endpoint{"127.0.0.1", port, 2s, 150ms};
    const std::string_view mode(argv[2]);
    auto processor = adi::device::makeAudioGridderProcessor(
        endpoint, {mode == "unknown" ? "VST3-400" : "VST3-12ab", "Stereo"});
    std::string error;
    std::int32_t latency = 0;
    const bool prepared = processor->prepare(48000, 2, 64, false, latency, error);
    if (mode == "version")
        return check(!prepared && error.find("protocol version") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "control")
        return check(!prepared && error.find("command length") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "unknown")
        return check(!prepared && error.find("VST3-400") != std::string::npos, error.c_str()) ? 0 : 1;
    if (!check(prepared, error.c_str()) || !check(latency == 7, "initial plugin latency was not 7"))
        return 1;

    std::array<float, 128> input{}, output{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(static_cast<int>(i) - 64) / 128.0f;
    const std::array events{adi::engine::Event{.frame = 17, .type = adi::engine::EventType::NoteOn,
                                             .channel = 3, .dim = 69, .noteId = 14, .value = 1.0}};
    adi::engine::DeviceTransport transport;
    transport.sample = 24000;
    transport.bar = 2;
    transport.quarterNotes = 1;
    transport.barStart = 0;
    transport.bpm = 120;
    transport.numerator = 5;
    transport.denominator = 4;
    transport.playing = true;
    const adi::device::RemoteBlock block{.channels = 2, .frames = 64, .audio = input,
                                         .events = events, .transport = transport};
    const auto start = std::chrono::steady_clock::now();
    const bool exchanged = processor->exchange(block, output, error);
    if (mode == "timeout")
        return check(!exchanged && error.find("timed out") != std::string::npos &&
                     std::chrono::steady_clock::now() - start < 1s, error.c_str()) ? 0 : 1;
    if (mode == "disconnect")
        return check(!exchanged && error.find("closed the connection") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "shape" || mode == "latency" || mode == "nonfinite")
        return check(!exchanged && error.find(mode) != std::string::npos, error.c_str()) ? 0 : 1;
    if (!check(exchanged, error.c_str())) return 1;
    float maximumDifference = 0;
    for (std::size_t i = 0; i < input.size(); ++i)
        maximumDifference = std::max(maximumDifference, std::abs(output[i] - input[i] * 0.5f));
    std::printf("stock v13 loopback fixture: 128 samples, maximum absolute difference %.9g\n",
                static_cast<double>(maximumDifference));
    const std::array unsupported{adi::engine::Event{.frame = 2,
        .type = adi::engine::EventType::NoteExpression, .channel = 3, .noteId = 14, .value = 1.0}};
    auto expression = block;
    expression.events = unsupported;
    const auto expressiveAudio = processor->exchange(expression, output, error);
    processor->release();
    return check(maximumDifference == 0 && !expressiveAudio &&
                 error.find("per-note expression") != std::string::npos,
                 "PCM mismatch or expression was silently discarded") ? 0 : 1;
}
