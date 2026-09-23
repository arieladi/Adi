// SPDX-License-Identifier: GPL-3.0-or-later
// ADR-0102: synthetic callback timing, without an audio device or platform API.
// Build Release with ADI_BUILD_BENCHMARKS=ON; run --self-test before measuring.
#include "adi/engine/graph.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace adi::engine;
constexpr double kRate = 48000.0;
constexpr std::size_t kWarmup = 32;

struct Summary {
    double p50, p99, maximum;
    std::size_t deadlineMisses;
};

Summary summarize(std::vector<double> times, double deadline) {
    if (times.empty()) throw std::runtime_error("no measured callbacks");
    std::sort(times.begin(), times.end());
    const auto percentile = [&](std::size_t percent) {
        // Nearest rank; even a single observation has a well-defined rank.
        const std::size_t rank = (times.size() * percent + 99) / 100;
        return times[rank - 1];
    };
    return {percentile(50), percentile(99), times.back(),
            static_cast<std::size_t>(std::count_if(times.begin(), times.end(),
                [deadline](double value) { return value > deadline; }))};
}

bool selfTest() {
    std::vector<double> times(100);
    std::iota(times.begin(), times.end(), 1.0);
    std::reverse(times.begin(), times.end());
    const auto s = summarize(times, 50.0);
    const auto one = summarize({7.0}, 7.0);
    bool emptyRejected = false;
    try { (void)summarize({}, 1.0); }
    catch (const std::runtime_error&) { emptyRejected = true; }
    const bool ok = s.p50 == 50.0 && s.p99 == 99.0 && s.maximum == 100.0 &&
                    s.deadlineMisses == 50 && one.p50 == 7.0 && one.p99 == 7.0 &&
                    one.maximum == 7.0 && one.deadlineMisses == 0 && emptyRejected;
    std::puts(ok ? "PASS: quantiles, strict deadline and empty-input rejection"
                 : "FAIL: benchmark statistics");
    return ok;
}

class Source final : public Node {
public:
    explicit Source(float value) : value_(value) {}
    void process(const NodeIo& io) noexcept override {
        for (std::int32_t ch = 0; ch < io.channels; ++ch)
            for (std::int32_t frame = 0; frame < io.frames; ++frame)
                io.out[ch][io.blockOffset + frame] = value_;
    }
    std::int64_t tailSamples() const noexcept override {
        return value_ == 0.0f ? 0 : kInfiniteTail;
    }
private:
    float value_;
};

struct Project {
    const char* name;
    int tracks;
    int effects;
    bool mostlySilent;
    bool expressionStorm;
};

void measure(const Project& project, std::int32_t frames, std::size_t iterations) {
    // Graph does not own nodes. Declare owners first so the graph dies first.
    std::vector<std::unique_ptr<Node>> nodes;
    Graph graph;
    const auto add = [&](std::unique_ptr<Node> node) {
        const NodeId id = graph.addNode(*node);
        nodes.push_back(std::move(node));
        return id;
    };
    const auto connect = [&](NodeId from, NodeId to) {
        if (!graph.connect(from, to)) throw std::runtime_error("invalid fixture edge");
    };
    const NodeId master = add(std::make_unique<SumNode>());
    NodeId expressionTarget = 0;
    for (int track = 0; track < project.tracks; ++track) {
        const float value = project.mostlySilent && track != 0 ? 0.0f : 0.001f;
        NodeId previous = add(std::make_unique<Source>(value));
        if (track == 0) expressionTarget = previous;
        for (int effect = 0; effect < project.effects; ++effect) {
            const auto next = add(std::make_unique<GainNode>());
            connect(previous, next);
            previous = next;
        }
        connect(previous, master);
    }
    graph.setOutput(master);
    BlockProcessor& processor = graph;
    processor.prepare(kRate, frames);
    if (!graph.ok()) throw std::runtime_error(graph.error());

    std::vector<float> left(static_cast<std::size_t>(frames));
    std::vector<float> right(left.size());
    std::array<float*, 2> channels{left.data(), right.data()};
    AudioIo io;
    io.out = channels.data();
    io.numOut = 2;
    io.frames = frames;
    std::vector<double> times(iterations); // no allocation while measuring
    std::uint64_t rejected = 0;
    std::int64_t warmupDrops = 0;
    for (std::size_t block = 0; block < kWarmup + iterations; ++block) {
        if (block == kWarmup) {
            rejected = 0;
            warmupDrops = graph.stats().eventsDropped;
        }
        // Produce the exact 500 Hz stream on a global sample clock, outside
        // timing. Small blocks must not accidentally emit 500 Hz per block.
        if (project.expressionStorm) {
            const auto first = static_cast<std::int32_t>(
                (96 - io.streamTimeSamples % 96) % 96);
            for (std::int32_t frame = first; frame < frames; frame += 96)
                for (std::uint64_t note = 1; note <= 16; ++note)
                    for (std::uint16_t dim = 0; dim < 3; ++dim) {
                        Event event;
                        event.frame = frame;
                        event.type = EventType::NoteExpression;
                        event.noteId = note;
                        event.dim = dim;
                        event.value = 0.5;
                        if (!graph.pushInputEvent(expressionTarget, event)) ++rejected;
                    }
        }
        const auto start = std::chrono::steady_clock::now();
        processor.process(io);
        const auto end = std::chrono::steady_clock::now();
        if (block >= kWarmup)
            times[block - kWarmup] = std::chrono::duration<double, std::micro>(end - start).count();
        io.streamTimeSamples += frames;
    }
    const auto s = summarize(std::move(times), static_cast<double>(frames) * 1e6 / kRate);
    const auto drops = graph.stats().eventsDropped - warmupDrops;
    // A broken or empty fixture must not publish impressive silence timings.
    const float expected = 0.001f * static_cast<float>(project.mostlySilent ? 1 : project.tracks);
    const auto valid = [expected](float value) {
        return value > expected * 0.999f && value < expected * 1.001f;
    };
    if (!std::all_of(left.begin(), left.end(), valid) ||
        !std::all_of(right.begin(), right.end(), valid))
        throw std::runtime_error("fixture output is not the expected track sum");
    if (drops != 0 || rejected != 0)
        throw std::runtime_error("fixture lost expression events");
    std::printf("%s,%d,%d,%d,%zu,%zu,%.3f,%.3f,%.3f,%zu,%lld,%llu\n",
        project.name, project.tracks, project.effects, frames, graph.nodeCount(), iterations,
        s.p50, s.p99, s.maximum, s.deadlineMisses,
        static_cast<long long>(drops), static_cast<unsigned long long>(rejected));
    processor.release();
}
} // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 1000;
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") return selfTest() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::puts("adi_block_benchmark [--iterations N | --self-test]\n"
                  "N: 1..1000000, default 1000; 32 warmup callbacks per row.\n"
                  "Use Release. 48 kHz stereo; CSV times in microseconds.\n"
                  "dropouts = callback time > frames/sample_rate, not device xruns.\n"
                  "Times include Graph processing; exclude event production and setup.\n"
                  "Fixed fixtures: 8x4 active, 64x4 with 63 silent tracks, 8x4 with MPE+.\n"
                  "MPE+ = 16 notes x 3 dimensions at 500 Hz into the first track.\n"
                  "This is synthetic timing, not an Ableton comparison.");
        return 0;
    }
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--iterations") {
            std::fputs("use --help for usage\n", stderr); return 2;
        }
        const std::string_view value(argv[2]);
        const auto result = std::from_chars(value.data(), value.data() + value.size(), iterations);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            iterations == 0 || iterations > 1000000) {
            std::fputs("iterations must be an integer in 1..1000000\n", stderr); return 2;
        }
    }
    std::puts("project,tracks,effects_per_track,frames,nodes,callbacks,p50_us,p99_us,max_us,dropouts,event_drops,rejected_events");
    try {
        for (const Project& project : std::array<Project, 3>{{
                 {"active", 8, 4, false, false},
                 {"silence-heavy", 64, 4, true, false},
                 {"mpe-storm", 8, 4, false, true}}})
            for (std::int32_t frames : {32, 64, 128, 2048, 4096})
                measure(project, frames, iterations);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "benchmark failed: %s\n", error.what()); return 1;
    }
}
