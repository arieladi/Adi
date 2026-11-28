// SPDX-License-Identifier: GPL-3.0-or-later
#include "juce/net_plugin_socket.hpp"
#include "juce/audiogridder_transport.hpp"
#include "adi/audio/io_audit.hpp"

#include "replace_operator_new.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace adi;
using namespace std::chrono_literals;

constexpr std::int32_t frames = 64;
constexpr float dryLevel = 0.25f;
thread_local bool auditCallback = false;
int callbackAllocations = 0;

class Tone final : public engine::Node {
public:
    explicit Tone(float level) : level(level) {}

    void process(const engine::NodeIo& io) noexcept override {
        for (std::int32_t channel = 0; channel < io.channels; ++channel)
            std::fill_n(io.out[channel] + io.blockOffset, io.frames, level);
    }

    float level;
};

enum class Block { Invalid, Priming, Dry, Wet };

bool check(bool condition, const char* reason) {
    if (!condition) std::fprintf(stderr, "%s\n", reason);
    return condition;
}

// The Python harness acknowledges the control command after it reaches the
// requested peer state. These barriers always run outside Graph::process().
bool barrier(const char* command) {
    std::puts(command);
    std::fflush(stdout);
    std::array<char, 16> reply{};
    return check(std::fgets(reply.data(), static_cast<int>(reply.size()), stdin) != nullptr &&
                 std::string_view(reply.data()) == "ok\n", "TCP fixture barrier failed");
}

}  // namespace

void adi::test::countReplacedNew() noexcept {
    if (auditCallback) ++callbackAllocations;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view portText(argv[1]);
    std::uint16_t port = 0;
    const auto* const end = portText.data() + portText.size();
    const auto parsed = std::from_chars(portText.data(), end, port);
    if (parsed.ec != std::errc{} || parsed.ptr != end || port == 0) return 2;

    // DEV-27: never a socket on the audio thread. The guard refuses and counts
    // a TCP call made on a callback thread (port 1: nothing of the fixture's).
    {
        net::plugin_socket::PluginSocket probe;
        std::string error;
        in_addr loopback{};
        loopback.s_addr = htonl(INADDR_LOOPBACK);
        bool opened = false;
        {
            const audio::CallbackScope scope;
            opened = probe.open(loopback, 1, std::chrono::steady_clock::now() + 1s, error);
        }
        if (!check(!opened && net::plugin_socket::callbackRefusals.load() == 1,
                   "a TCP call on the audio thread was not refused and counted")) return 1;
    }

    auto processor = device::makeAudioGridderProcessor(
        {"127.0.0.1", port, 5s, 10s}, {"VST3-12ab", "Stereo"});
    device::RemoteDevice remote({"vst3", "tcp-test", "TCP Fixture Gain", "ADI", "1"},
                                2, std::move(processor));
    device::DeviceNode wet(remote);
    Tone input(0.0f);
    Tone dry(dryLevel);
    engine::SumNode sum;
    engine::Graph graph;
    const auto wetInput = graph.addNode(input);
    const auto wetOutput = graph.addNode(wet);
    const auto dryOutput = graph.addNode(dry);
    const auto main = graph.addNode(sum);
    if (!check(graph.connect(wetInput, wetOutput) && graph.connect(wetOutput, main) &&
               graph.connect(dryOutput, main), "could not wire the wet and dry tracks")) return 1;
    graph.setOutput(main);
    graph.prepare(48000, frames);
    if (!check(graph.ok() && remote.loaded() && graph.outputLatencyOf(main) == frames,
               "the live graph could not load the TCP device with one block of latency")) return 1;

    std::array<float, frames> left{}, right{};
    std::array<float*, 2> outputs{left.data(), right.data()};
    engine::AudioIo io;
    io.out = outputs.data();
    io.numOut = 2;
    io.frames = frames;
    int block = 0;
    const auto flat = [&](float level) {
        return left == right && std::all_of(left.begin(), left.end(),
            [level](float sample) { return sample == level; });
    };
    const auto callback = [&] {
        const auto previousMisses = remote.droppedBlocks();
        // The fixture halves the input. This block's one-slot-old reply has
        // a unique level, so playing a stale packet fails even if it is wet.
        input.level = static_cast<float>(2 * (block + 1));
        auditCallback = true;
        {
            const audio::CallbackScope scope;
            graph.process(io);
        }
        auditCallback = false;
        const auto currentBlock = block++;
        const auto misses = remote.droppedBlocks() - previousMisses;
        if (currentBlock == 0 && flat(0.0f) && misses == 0) return Block::Priming;
        if (currentBlock > 0 && flat(dryLevel) && misses == 1) return Block::Dry;
        if (currentBlock > 0 && flat(dryLevel + static_cast<float>(currentBlock)) &&
            misses == 0) return Block::Wet;
        std::fprintf(stderr, "callback %d produced unexpected level %.9g or misses %llu\n",
                     currentBlock, static_cast<double>(left.front()),
                     static_cast<unsigned long long>(misses));
        return Block::Invalid;
    };
    const auto driveUntil = [&](Block target) {
        const auto limit = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < limit) {
            const auto result = callback();
            if (result == target) return true;
            if (result == Block::Invalid) return false;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    };
    const auto exactDryTrack = [&] {
        for (int i = 0; i < 4; ++i)
            if (callback() != Block::Dry) return false;
        return true;
    };

    if (!check(callback() == Block::Priming, "the compensation delay could not prime") ||
        !check(driveUntil(Block::Wet), "the TCP peer produced no aligned audio") ||
        !barrier("arm stall") ||
        callback() == Block::Invalid ||
        !barrier("wait for stall") ||
        !check(driveUntil(Block::Dry) && exactDryTrack(),
               "a withheld TCP reply disturbed the dry track or drop counter") ||
        !barrier("release stall") ||
        !check(driveUntil(Block::Wet), "the TCP peer failed to recover after the stalled reply") ||
        !barrier("arm disconnect") ||
        callback() == Block::Invalid ||
        !barrier("wait for disconnect")) return 1;

    const auto limit = std::chrono::steady_clock::now() + 5s;
    while (remote.processingIssue().empty() && std::chrono::steady_clock::now() < limit) {
        if (callback() == Block::Invalid) return 1;
        std::this_thread::sleep_for(1ms);
    }
    const auto issue = remote.processingIssue();
    if (!check(issue.find("TCP Fixture Gain") != std::string_view::npos &&
               issue.find("closed the connection") != std::string_view::npos,
               "peer disconnect did not name the affected plug-in and TCP fault") ||
        !check(driveUntil(Block::Dry) && exactDryTrack(),
               "the disconnected TCP peer disturbed the dry track or drop counter") ||
        !check(callbackAllocations == 0, "the live callback allocated memory") ||
        !check(net::plugin_socket::callbackRefusals.load() == 1,
               "the live callback reached a TCP call; only the worker may")) return 1;

    std::printf("Live TCP graph recovered, reported the named disconnect and counted %llu dry blocks\n",
                static_cast<unsigned long long>(remote.droppedBlocks()));
    return 0;
}
