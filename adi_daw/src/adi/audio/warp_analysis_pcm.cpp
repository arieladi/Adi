// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/warp_analysis_pcm.hpp"
#include "adi/audio/io_audit.hpp"

#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace adi::audio {
namespace {
constexpr std::size_t blockFrames = 4096;

float clean(float sample) noexcept {
    return std::isfinite(sample) ? std::clamp(sample, -1e12f, 1e12f) : 0.0f;
}
}

struct WarpAnalysisPcm::State {
    State(std::uint32_t rate, std::uint32_t sourceChannels, Sink&& output, WarpAnalysisLayout layout)
        : sourceRate(rate), channels(sourceChannels),
          lanes(sourceChannels > 1 && layout == WarpAnalysisLayout::MidSide ? 2u : 1u),
          ratio(static_cast<double>(kWarpAnalysisRate) / rate), sink(std::move(output)) {
        if (rate != kWarpAnalysisRate) {
            converter.reset(src_new(SRC_SINC_BEST_QUALITY, static_cast<int>(lanes), nullptr));
            if (!converter) error = AnalysisError::Io;
        }
    }

    bool countInput(std::size_t frames) noexcept {
        const auto elapsed = static_cast<std::uint64_t>(frames) * kWarpAnalysisRate + remainder;
        const auto increment = elapsed / sourceRate;
        if (increment > std::numeric_limits<std::uint64_t>::max() - expectedFrames) {
            error = AnalysisError::InvalidSource;
            return false;
        }
        expectedFrames += increment;
        remainder = elapsed % sourceRate;
        return true;
    }

    bool send(std::span<const float> frames, bool end = false) {
        const auto count = frames.size() / lanes;
        const auto remaining = expectedFrames - emittedFrames;
        if (!end && count > remaining) {
            error = AnalysisError::Io;
            return false;
        }
        const auto published = static_cast<std::size_t>(std::min<std::uint64_t>(count, remaining));
        try {
            if (published > 0 && !sink(frames.first(published * lanes))) error = AnalysisError::Io;
        } catch (...) {
            error = AnalysisError::Io;
        }
        if (error == AnalysisError::None) emittedFrames += published;
        return error == AnalysisError::None;
    }

    bool convert(std::span<const float> input, bool end, std::stop_token stop) {
        std::size_t pending = input.size() / lanes;
        do {
            if (stop.stop_requested()) {
                error = AnalysisError::Cancelled;
                return false;
            }
            SRC_DATA data{};
            // libsamplerate's terminal call may still drain buffered output.
            data.data_in = input.empty() ? source.data() : input.data();
            data.data_out = target.data();
            data.input_frames = static_cast<long>(pending);
            data.output_frames = static_cast<long>(blockFrames);
            data.src_ratio = ratio;
            data.end_of_input = end ? 1 : 0;
            if (src_process(converter.get(), &data) != 0 ||
                (pending > 0 && data.input_frames_used == 0 && data.output_frames_gen == 0)) {
                error = AnalysisError::Io;
                return false;
            }
            const auto written = static_cast<std::size_t>(data.output_frames_gen);
            if (!send(std::span(target).first(written * lanes), end)) return false;
            const auto consumed = static_cast<std::size_t>(data.input_frames_used);
            input = input.subspan(consumed * lanes);
            pending -= consumed;
            if (pending == 0 && (!end || written == 0)) break;
        } while (true);
        return true;
    }

    void downmix(std::span<const float> input, std::size_t start) noexcept {
        if (channels == 1) {
            std::transform(input.begin(), input.end(), source.data() + start, clean);
            return;
        }
        for (std::size_t offset = 0; offset < input.size(); offset += channels) {
            double positive = 0, negative = 0;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto& sum = channel % 2 == 0 ? positive : negative;
                sum += clean(input[offset + channel]);
            }
            source[start * lanes] = static_cast<float>((positive + negative) / channels);
            if (lanes == 2) source[start * lanes + 1] = static_cast<float>((positive - negative) / channels);
            ++start;
        }
    }

    std::uint32_t sourceRate, channels, lanes;
    double ratio;
    Sink sink;
    std::unique_ptr<SRC_STATE, decltype(&src_delete)> converter{nullptr, src_delete};
    std::array<float, blockFrames * 2> source{}, target{};
    std::array<float, 2> tail{};
    std::uint64_t expectedFrames = 0, emittedFrames = 0, remainder = 0;
    AnalysisError error = AnalysisError::None;
    bool hasTail = false, finished = false;
};

WarpAnalysisPcm::WarpAnalysisPcm(std::uint32_t rate, std::uint32_t channels, Sink sink, WarpAnalysisLayout layout) {
    if (onAudioThread) {
        initialError_ = AnalysisError::Realtime;
        return;
    }
    if (rate >= 8000 && rate <= 768000 && channels > 0 && channels <= 64 && sink)
        state_ = std::make_unique<State>(rate, channels, std::move(sink), layout);
}

WarpAnalysisPcm::~WarpAnalysisPcm() = default;

bool WarpAnalysisPcm::append(std::span<const float> interleaved, std::stop_token stop) {
    if (onAudioThread || !state_ || state_->finished || state_->error != AnalysisError::None) return false;
    auto& state = *state_;
    if (interleaved.size() % state.channels != 0) state.error = AnalysisError::InvalidSource;
    if (stop.stop_requested()) state.error = AnalysisError::Cancelled;
    if (state.error != AnalysisError::None) return false;
    while (!interleaved.empty()) {
        if (stop.stop_requested()) {
            state.error = AnalysisError::Cancelled;
            return false;
        }
        const std::size_t start = state.hasTail ? 1 : 0;
        const auto count = std::min(interleaved.size() / state.channels, blockFrames - start);
        const auto samples = count * state.channels;
        if (!state.countInput(count)) return false;
        if (state.hasTail) std::copy_n(state.tail.data(), state.lanes, state.source.data());
        state.downmix(interleaved.first(samples), start);
        const auto ready = std::span(state.source).first((count + start) * state.lanes);
        if (state.converter) {
            // Retain a physical frame so the terminal call marks the last real
            // input rather than ending between unrelated decoder blocks.
            std::copy_n(ready.last(state.lanes).data(), state.lanes, state.tail.data());
            state.hasTail = true;
            if (ready.size() > state.lanes && !state.convert(ready.first(ready.size() - state.lanes), false, stop))
                return false;
        } else if (!state.send(ready)) {
            return false;
        }
        interleaved = interleaved.subspan(samples);
    }
    return true;
}

bool WarpAnalysisPcm::finish(std::stop_token stop) {
    if (onAudioThread || !state_ || state_->finished || state_->error != AnalysisError::None) return false;
    auto& state = *state_;
    state.finished = true;
    if (stop.stop_requested()) {
        state.error = AnalysisError::Cancelled;
        return false;
    }
    if (state.hasTail) {
        // A real tail followed by one explicit zero makes enough output to
        // cover an exactly integral duration at rates where roundoff would
        // otherwise discard the final frame. Deliver only the rational extent.
        std::array<float, 4> paddedTail{};
        std::copy_n(state.tail.data(), state.lanes, paddedTail.data());
        if (!state.convert(std::span(paddedTail).first(2 * state.lanes), true, stop)) return false;
    }
    if (state.emittedFrames != state.expectedFrames) state.error = AnalysisError::Io;
    return state.error == AnalysisError::None;
}

AnalysisError WarpAnalysisPcm::error() const noexcept {
    if (onAudioThread) return AnalysisError::Realtime;
    return state_ ? state_->error : initialError_;
}

} // namespace adi::audio
