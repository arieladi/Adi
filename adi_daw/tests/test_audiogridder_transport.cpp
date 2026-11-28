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
#include <utility>

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
    if (mode == "catalogue" || mode == "bad-catalogue") {
        std::vector<adi::device::AudioGridderCatalogueEntry> entries{{"original", "Cached row", "vst3", false}};
        std::string error;
        const bool read = adi::device::readAudioGridderCatalogue(endpoint, entries, error);
        if (mode == "bad-catalogue")
            return check(!read && entries.size() == 1 && entries[0].serverToken == "original" &&
                         error.find("catalogue") != std::string::npos,
                         "a malformed catalogue replaced the cached row") ? 0 : 1;
        const bool exact = read && entries.size() == 2 &&
            entries[0].serverToken == "VST3-12ab" && entries[0].name == "Engine Synth" &&
            entries[0].format == "vst3" && entries[0].instrument &&
            entries[1].serverToken == "VST3-34cd" && entries[1].name == "Engine Gain" &&
            entries[1].format == "vst3" && !entries[1].instrument;
        if (exact) std::puts("stock v13 loopback fixture: two exact metadata rows, ambiguous and legacy tokens omitted");
        return check(exact, "stock catalogue did not preserve two eligible metadata rows") ? 0 : 1;
    }
    const auto initialState = mode == "initial-state" ? "4..nx+G." : "";
    if (mode == "state-deadline") endpoint.blockTimeout = 1500ms;
    adi::device::AudioGridderPlugin plugin{mode == "unknown" ? "VST3-400" : "VST3-12ab", "Stereo", initialState};
    if (mode == "params" || mode == "bad-param-reply")
        plugin.parameterBindings = {{1, "440"}, {2, "441"}, {4, "442"}};
    if (mode == "duplicate-binding") plugin.parameterBindings = {{1, "440"}, {2, "440"}};
    auto processor = adi::device::makeAudioGridderProcessor(endpoint, std::move(plugin));
    std::string error;
    std::int32_t latency = 0;
    const bool prepared = processor->prepare(48000, 2, 64, false, latency, error);
    if (mode == "duplicate-binding")
        return check(!prepared && error.find("invalid trusted stock parameter binding") != std::string::npos,
                     error.c_str()) ? 0 : 1;
    if (mode == "version")
        return check(!prepared && error.find("protocol version") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "control")
        return check(!prepared && error.find("command length") != std::string::npos, error.c_str()) ? 0 : 1;
    if (mode == "unknown")
        return check(!prepared && error.find("VST3-400") != std::string::npos, error.c_str()) ? 0 : 1;
    if (!check(prepared, error.c_str()) || !check(latency == 7, "initial plugin latency was not 7"))
        return 1;

    if (mode == "params" || mode == "bad-param-reply" || mode == "params-unbound") {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        std::vector<adi::device::RemoteParameter> controls(1);
        controls[0].descriptor.id = "cached";
        const bool read = processor->readParameters(controls, deadline, error);
        if (mode == "bad-param-reply") {
            if (!check(!read && controls.size() == 1 && controls[0].descriptor.id == "cached",
                       "mismatched stock parameter reply replaced retained controls")) return 1;
        } else if (mode == "params-unbound") {
            double unapplied = -1;
            if (!check(read && controls.empty() && !processor->setParameter(1, 0.6, unapplied, deadline, error),
                       "stock parameter was exposed without an explicit stable-ID binding")) return 1;
        } else {
            if (!check(read && controls.size() == 1 && controls[0].selector == 1 &&
                       controls[0].descriptor.id == "440" && controls[0].descriptor.name == "Gain" &&
                       controls[0].descriptor.configurable && !controls[0].descriptor.automatable &&
                       controls[0].descriptor.defaultValue.normalized == 0.5 &&
                       controls[0].current.normalized == 0.25,
                       "trusted stock parameter did not use its stable ID and live normalized value")) return 1;
            double applied = -1;
            if (!check(!processor->setParameter(0, 0.6, applied, deadline, error) &&
                       !processor->setParameter(2, 0.6, applied, deadline, error) &&
                       !processor->setParameter(4, 0.6, applied, deadline, error) &&
                       processor->setParameter(1, 0.6, applied, deadline, error) && applied == 0.625,
                       "stock set did not read back the plug-in's canonical normalized value")) return 1;
            std::puts("stock v13 loopback fixture: normalized value 0.6 applied as 0.625; unbound selector refused");
        }
    }

    if (mode == "state" || mode == "bad-state" || mode == "state-deadline") {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        std::vector<std::uint8_t> state;
        const bool fetched = processor->fetchState(state, deadline, error);
        if (mode == "bad-state")
            return check(!fetched && state.empty() && error.find("invalid stock plug-in state") != std::string::npos,
                         "invalid stock state size was accepted") ? 0 : 1;
        if (!check(processor->supportsState() && fetched && state == std::vector<std::uint8_t>{0, 42, 255, 7},
                   "stock state was not decoded to exact raw bytes")) return 1;
        const std::vector<std::uint8_t> update{4, 128, 11, 222, 3};
        if (mode == "state-deadline") {
            const auto start = std::chrono::steady_clock::now();
            const auto result = processor->restoreState(update, start + 150ms, error);
            return check(!result && error.find("timed out") != std::string::npos &&
                         std::chrono::steady_clock::now() - start < 750ms,
                         "stock warm-up exceeded the original state deadline") ? 0 : 1;
        }
        const auto next = std::chrono::steady_clock::now() + 1s;
        if (!check(processor->restoreState(update, next, error) && processor->fetchState(state, next, error) &&
                   state == update, "restored stock state did not round-trip")) return 1;
        std::puts("stock v13 loopback fixture: restored and captured five exact state bytes");
    }

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
