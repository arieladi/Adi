// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <samplerate.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace adi::engine::detail {
namespace sinc441 {
constexpr std::int64_t inputPeriod = 147;
constexpr std::int64_t outputPeriod = 160;
constexpr std::int64_t radius = 144;
constexpr std::int64_t lanes = 4;
constexpr std::size_t taps = static_cast<std::size_t>(2 * radius + lanes);
constexpr std::size_t groups = static_cast<std::size_t>(outputPeriod / lanes);

struct alignas(32) Weights {
    std::array<double, static_cast<std::size_t>(lanes)> lane{};
};
using KernelGroup = std::array<Weights, taps>;

struct Kernel {
    std::array<KernelGroup, groups> group{};
    bool ready = false;

    Kernel() noexcept {
        try { ready = measure(); }
        catch (...) {} // Keep the ordinary converter available on allocation failure.
    }

private:
    bool measure() {
        // A fixed 147:160 ratio revisits each output phase every 147 input
        // frames. Pinned sinc-best has less than 144 frames of support on
        // either side. Isolate its responses with impulses 295 frames apart.
        // Since 295 = 2*147+1, the impulses visit every input residue once.
        constexpr int begin = 294, stride = 295;
        constexpr long inputFrames = begin + stride * (inputPeriod - 1) + begin;
        constexpr long outputFrames = inputFrames * outputPeriod / inputPeriod + 1;
        std::vector<float> input(static_cast<std::size_t>(inputFrames), 0.0f);
        std::vector<float> output(static_cast<std::size_t>(outputFrames), 0.0f);
        for (std::int64_t residue = 0; residue < inputPeriod; ++residue)
            input[static_cast<std::size_t>(begin + stride * residue)] = 1.0f;

        SRC_DATA data{};
        data.data_in = input.data();
        data.data_out = output.data();
        data.input_frames = inputFrames;
        data.output_frames = outputFrames;
        data.end_of_input = 1;
        data.src_ratio = static_cast<double>(outputPeriod) / inputPeriod;
        if (src_simple(&data, SRC_SINC_BEST_QUALITY, 1) != 0) return false;

        for (std::int64_t phase = 0; phase < outputPeriod; ++phase) {
            const auto center = phase * inputPeriod / outputPeriod;
            const auto groupStart = phase - phase % lanes;
            const auto shift = center - groupStart * inputPeriod / outputPeriod;
            auto& destination = group[static_cast<std::size_t>(phase / lanes)];
            double dcGain = 0.0;
            for (std::int64_t offset = -radius; offset <= radius; ++offset) {
                // Choose the impulse i for which this tap lands at `phase`.
                // For output frame phase + 160*n the tap reads input frame
                // center + 147*n + offset. Solve that equation for n.
                const auto residue = (center + offset + inputPeriod) % inputPeriod;
                const auto impulse = begin + stride * residue;
                const auto at = phase + outputPeriod * ((impulse - center - offset) / inputPeriod);
                if (at < 0 || at >= data.output_frames_gen) return false;
                const auto coefficient = output[static_cast<std::size_t>(at)];
                if ((offset == -radius || offset == radius) && coefficient != 0.0f) return false;
                dcGain += coefficient;
                destination[static_cast<std::size_t>(radius + shift + offset)]
                    .lane[static_cast<std::size_t>(phase % lanes)] = coefficient;
            }
            if (!(std::abs(dcGain - 1.0) < 1e-5)) return false;
        }
        return true;
    }
};
} // namespace sinc441

// Worker-only stereo fast path for pinned libsamplerate's sinc-best filter.
// Input starts on a rational phase-zero boundary and includes zero padding
// and source context. The measured coefficients retain libsamplerate's float
// precision. Each output accumulates independently in double precision.
[[nodiscard]] inline bool renderSinc441To480(std::span<const float> input,
                                            std::int64_t firstOutputFrame,
                                            std::span<float> output) noexcept {
    using namespace sinc441;
    constexpr std::int64_t maxOutputFrame = std::numeric_limits<std::int64_t>::max() / inputPeriod;
    if (input.size() % 2 != 0 || output.size() % 2 != 0 ||
        firstOutputFrame < 0 || firstOutputFrame > maxOutputFrame ||
        output.size() / 2 > static_cast<std::uint64_t>(maxOutputFrame - firstOutputFrame)) return false;
    if (output.empty()) return true;

    const auto lastOutputFrame = firstOutputFrame + static_cast<std::int64_t>(output.size() / 2) - 1;
    const auto firstGroup = firstOutputFrame - firstOutputFrame % lanes;
    const auto lastGroup = lastOutputFrame - lastOutputFrame % lanes;
    const auto firstSourceFrame = firstGroup * inputPeriod / outputPeriod - radius;
    const auto lastSourceFrame = lastGroup * inputPeriod / outputPeriod + radius + lanes - 1;
    if (firstSourceFrame < 0 || static_cast<std::uint64_t>(lastSourceFrame) >= input.size() / 2) return false;
    const auto source = input.subspan(static_cast<std::size_t>(firstSourceFrame) * 2,
                                     static_cast<std::size_t>(lastSourceFrame - firstSourceFrame + 1) * 2);
    // The padded SIMD lanes contain zero taps. Use the ordinary converter for
    // non-finite WAV data so a zero times NaN cannot widen the affected range.
    for (const auto value : source) if (!std::isfinite(value)) return false;

    static const Kernel kernel;
    if (!kernel.ready) return false;
    for (auto frame = firstGroup; frame <= lastGroup; frame += lanes) {
        const auto& weights = kernel.group[static_cast<std::size_t>(frame % outputPeriod / lanes)];
        const auto* samples = input.data() + static_cast<std::size_t>(frame * inputPeriod / outputPeriod - radius) * 2;
        std::array<double, static_cast<std::size_t>(lanes)> left{}, right{};
        // A tap uses one input frame for all four output lanes. Placing their
        // shifted coefficients next to each other lets the compiler vectorize
        // independent accumulations without reassociating any sum.
        for (std::size_t tap = 0; tap < taps; ++tap) {
            const double a = samples[2 * tap], b = samples[2 * tap + 1];
            for (std::size_t lane = 0; lane < left.size(); ++lane) {
                const auto weight = weights[tap].lane[lane];
                left[lane] += a * weight;
                right[lane] += b * weight;
            }
        }
        for (std::int64_t lane = 0; lane < lanes; ++lane) {
            const auto at = frame + lane;
            if (at < firstOutputFrame || at > lastOutputFrame) continue;
            const auto index = static_cast<std::size_t>(at - firstOutputFrame) * 2;
            output[index] = static_cast<float>(left[static_cast<std::size_t>(lane)]);
            output[index + 1] = static_cast<float>(right[static_cast<std::size_t>(lane)]);
        }
    }
    return true;
}
} // namespace adi::engine::detail
