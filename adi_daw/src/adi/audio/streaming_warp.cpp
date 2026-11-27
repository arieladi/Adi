// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/audio/streaming_warp.hpp"

#include "adi/audio/io_audit.hpp"

#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace adi::audio {
namespace {

constexpr std::int64_t maxSourceFrames = (std::int64_t{1} << 31) - 1;
constexpr std::size_t maxWarpPoints = 1000000;
constexpr double beatsAttackSeconds = 0.01;
constexpr double beatsEnvelopeRate = 12;
constexpr int sincPhases = 256;
constexpr int sincHalfWidth = 144;
constexpr double maxSincStride = 256;

const float* impulseResponse() {
    // Obtain the pinned libsamplerate sinc-best kernel to table precision by
    // resampling a centred impulse. Random access to the kernel lets a loop,
    // seek or tempo edit start directly at its exact fractional source frame.
    static const auto response = [] {
        constexpr int center = 256;
        constexpr int frames = center * 2 + 1;
        std::array<float, frames> input{};
        input[center] = 1;
        std::vector<float> output(frames * sincPhases);
        SRC_DATA data{};
        data.data_in = input.data();
        data.data_out = output.data();
        data.input_frames = frames;
        data.output_frames = static_cast<long>(output.size());
        data.end_of_input = 1;
        data.src_ratio = sincPhases;
        std::array<float, sincHalfWidth * sincPhases + 2> result{};
        if (src_simple(&data, SRC_SINC_BEST_QUALITY, 1) != 0 ||
            data.output_frames_gen < static_cast<long>(center * sincPhases + result.size()))
            throw std::runtime_error("cannot prepare the streaming Re-Pitch filter");
        std::copy_n(output.begin() + center * sincPhases, result.size(), result.begin());
        return result;
    }();
    return response.data();
}

std::uint64_t grainRandom(std::uint64_t seed, std::int64_t index) noexcept {
    std::uint64_t value = seed + static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

template<auto from, auto to>
double project(std::span<const WarpMarker> markers, double position) noexcept {
    const auto* begin = markers.data();
    const auto* end = begin + markers.size();
    const auto* next = std::upper_bound(begin, end, position,
        [](double at, const WarpMarker& point) { return at < static_cast<double>(point.*from); });
    const auto* high = std::clamp(next, begin + 1, end - 1);
    const auto& a = *(high - 1);
    const auto& b = *high;
    return static_cast<double>(a.*to) + (position - static_cast<double>(a.*from)) *
        static_cast<double>(b.*to - a.*to) / static_cast<double>(b.*from - a.*from);
}

float finiteSample(float value) noexcept {
    return std::isfinite(value) ? value : 0;
}

} // namespace

StreamingWarp::StreamingWarp(const WarpRenderPlan& plan, std::span<const WarpMarker> markers,
                             std::uint32_t sourceRate, std::uint16_t channels) {
    if (onAudioThread)
        throw std::runtime_error("streaming warp must be prepared away from the audio callback");
    if (sourceRate < 8000 || sourceRate > 768000 || plan.outputRate < 44100 || plan.outputRate > 768000 ||
        channels == 0 || channels > 64 || plan.sourceFrames == 0 || plan.sourceFrames > maxSourceFrames ||
        !std::isfinite(plan.transposeSemis) || std::abs(plan.transposeSemis) > 96 ||
        markers.size() < 2 || markers.size() > maxWarpPoints || markers.front() != WarpMarker{0, 0} ||
        markers.back().sourceFrame != static_cast<std::int64_t>(plan.sourceFrames))
        throw std::invalid_argument("invalid streaming warp source, rate, pitch or marker map");
    for (std::size_t i = 1; i < markers.size(); ++i) {
        const auto& a = markers[i - 1];
        const auto& b = markers[i];
        if (b.sourceFrame <= a.sourceFrame || b.tick <= a.tick)
            throw std::invalid_argument("streaming warp markers must increase in both clocks");
        maxFramesPerTick_ = std::max(maxFramesPerTick_, static_cast<double>(b.sourceFrame - a.sourceFrame) /
            static_cast<double>(b.tick - a.tick));
    }

    markers_.assign(markers.begin(), markers.end());
    sourceFrames_ = markers.back().sourceFrame;
    channels_ = channels;
    mode_ = plan.mode;
    pitch_ = std::exp2(plan.transposeSemis / 12) * sourceRate / plan.outputRate;
    switch (mode_) {
    case WarpMode::Beats: {
        if (!std::isfinite(plan.transientEnvelope) || plan.transientEnvelope < 0 || plan.transientEnvelope > 1 ||
            (plan.transientLoop != TransientLoop::Off && plan.transientLoop != TransientLoop::Forward &&
             plan.transientLoop != TransientLoop::PingPong) || plan.transients.size() > maxWarpPoints)
            throw std::invalid_argument("invalid streaming Beats envelope, loop or onset count");
        transientEnvelope_ = plan.transientEnvelope;
        transientLoop_ = plan.transientLoop;
        attackFrames_ = static_cast<double>(sourceRate) * beatsAttackSeconds;
        slices_.reserve(markers.size() + plan.transients.size());
        for (const auto& marker : markers)
            slices_.push_back({marker.sourceFrame, static_cast<double>(marker.tick)});
        for (const auto onset : plan.transients) {
            if (onset >= plan.sourceFrames)
                throw std::invalid_argument("streaming Beats onset lies outside its source window");
            const auto frame = static_cast<std::int64_t>(onset);
            // SPEC stores marker ticks as integers. Keep sub-tick onset
            // precision in the inverse map when projecting slice boundaries.
            slices_.push_back({frame, tickAtSource(static_cast<double>(frame))});
        }
        std::sort(slices_.begin(), slices_.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
        const auto end = std::unique(slices_.begin(), slices_.end(),
            [](const auto& a, const auto& b) { return a.frame == b.frame; });
        slices_.erase(end, slices_.end());
        break;
    }
    case WarpMode::Texture:
        if (!std::isfinite(plan.grainSeconds) || plan.grainSeconds < 0.001 || plan.grainSeconds > 1 ||
            !std::isfinite(plan.fluctuation) || plan.fluctuation < 0 || plan.fluctuation > 1)
            throw std::invalid_argument("invalid streaming Texture grain size or fluctuation");
        grain_ = std::max<std::int64_t>(4, std::llround(plan.grainSeconds * plan.outputRate));
        grainHop_ = std::max<std::int64_t>(1, grain_ / 4);
        grainJitterFrames_ = plan.fluctuation * static_cast<double>(grain_) * sourceRate / plan.outputRate;
        seed_ = plan.seed;
        break;
    case WarpMode::RePitch:
        if (plan.transposeSemis != 0)
            throw std::invalid_argument("streaming Re-Pitch derives pitch from the warp rate");
        sinc_ = impulseResponse();
        break;
    default:
        throw std::invalid_argument("the native streaming player cannot play this warp mode");
    }
}

double StreamingWarp::sourceAtTick(double tick) const noexcept {
    return project<&WarpMarker::tick, &WarpMarker::sourceFrame>(markers_, tick);
}

double StreamingWarp::tickAtSource(double frame) const noexcept {
    return project<&WarpMarker::sourceFrame, &WarpMarker::tick>(markers_, frame);
}

std::size_t StreamingWarp::sliceAtFrame(double frame, WarpStreamClock clock) const noexcept {
    // A physical onset time is rounded once to the nearest output frame.
    // The approach from below at a half-frame keeps ties on the later frame.
    const double tick = clock.tickAtFrame(clock.context, frame + 0.5);
    const auto* begin = slices_.data();
    const auto* end = begin + slices_.size();
    const auto* next = std::lower_bound(begin, end, tick,
        [](const auto& point, double position) { return point.tick < position; });
    return static_cast<std::size_t>(std::clamp(next, begin + 1, end - 1) - begin - 1);
}

double StreamingWarp::sliceStartFrame(std::size_t slice, WarpStreamClock clock) const noexcept {
    return std::round(clock.frameAtTick(clock.context, slices_[slice].tick));
}

double StreamingWarp::beatsReadAt(double local, double span) const noexcept {
    if (local < span) return local;
    if (transientLoop_ == TransientLoop::Off) return -1;
    const double begin = std::min(span - 1, std::max(1.0, attackFrames_));
    const double body = span - begin;
    if (transientLoop_ == TransientLoop::Forward)
        return begin + std::fmod(local - begin, body);
    if (body <= 1) return begin;
    const double phase = std::fmod(local - begin, 2 * (body - 1));
    return begin + std::min(phase, 2 * (body - 1) - phase);
}

std::pair<double, double> StreamingWarp::beatsRange(double first, double last, std::size_t slice,
                                                   WarpStreamClock clock) const noexcept {
    const double startFrame = sliceStartFrame(slice, clock);
    const double localFirst = std::max(0.0, (first - startFrame) * pitch_);
    const double localLast = (last - startFrame) * pitch_;
    const double start = static_cast<double>(slices_[slice].frame);
    const double span = static_cast<double>(slices_[slice + 1].frame - slices_[slice].frame);
    const double a = beatsReadAt(localFirst, span);
    const double b = beatsReadAt(localLast, span);
    const double infinity = std::numeric_limits<double>::infinity();
    if (localLast < localFirst || a < 0) return {infinity, -infinity};
    if (localLast < span || first == last)
        return {start + a, start + b};
    if (transientLoop_ == TransientLoop::Off)
        return {start + a, start + span};

    // The interval may cross a tail wrap or PingPong turnaround. Only that
    // portion of the source slice that can actually play needs paging.
    const double begin = std::min(span - 1, std::max(1.0, attackFrames_));
    const double body = span - begin;
    const double leg = transientLoop_ == TransientLoop::Forward ? body : body - 1;
    if (localFirst >= span && (leg == 0 || std::floor((localFirst - begin) / leg) ==
                                           std::floor((localLast - begin) / leg)))
        return {start + std::min(a, b), start + std::max(a, b)};
    return {start + std::min(a, begin), start + span};
}

float StreamingWarp::sample(WarpStreamInput input, double position, std::uint16_t channel) const noexcept {
    if (!(position >= 0 && position < static_cast<double>(sourceFrames_))) return 0;
    const auto first = static_cast<std::int64_t>(position);
    const double fraction = position - static_cast<double>(first);
    const double a = finiteSample(input.sample(input.context, first, channel));
    const double b = first + 1 < sourceFrames_ ? finiteSample(input.sample(input.context, first + 1, channel)) : 0;
    return static_cast<float>(a * (1 - fraction) + b * fraction);
}

void StreamingWarp::renderTexture(double frame, WarpStreamInput input, WarpStreamClock clock,
                                  std::span<float> output) const noexcept {
    const auto half = grain_ / 2;
    const auto grain = static_cast<double>(grain_);
    const auto hop = static_cast<double>(grainHop_);
    const auto first = static_cast<std::int64_t>(std::floor((frame - grain + static_cast<double>(half)) / hop)) + 1;
    const auto last = static_cast<std::int64_t>(std::floor((frame + static_cast<double>(half)) / hop));
    double weights = 0;
    std::array<double, 64> mix{};
    for (auto index = first; index <= last; ++index) {
        const auto center = static_cast<double>(index * grainHop_);
        const double phase = (frame - center + static_cast<double>(half)) / grain;
        const double weight = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * phase);
        const double jitter = (static_cast<double>(grainRandom(seed_, index) >> 11) / 0x1p53 - 0.5) *
            (2 * grainJitterFrames_);
        const double readAt = sourceAtTick(clock.tickAtFrame(clock.context, center)) + jitter + (frame - center) * pitch_;
        for (std::uint16_t channel = 0; channel < output.size(); ++channel)
            mix[channel] += sample(input, readAt, channel) * weight;
        weights += weight;
    }
    if (weights > 0)
        for (std::uint16_t channel = 0; channel < output.size(); ++channel)
            output[channel] = static_cast<float>(mix[channel] / weights);
}

void StreamingWarp::renderRePitch(double frame, double source, WarpStreamInput input, WarpStreamClock clock,
                                  std::span<float> output) const noexcept {
    const double stride = sourceAtTick(clock.tickAtFrame(clock.context, frame + 1)) - source;
    if (!(stride >= 1.0 / maxSincStride && stride <= maxSincStride)) return;
    // The same bandwidth reduction and gain normalization as libsamplerate
    // applies when decimating. At unity, the impulse table is its kernel.
    const double cutoff = 1 / std::max(1.0, stride);
    const double radius = sincHalfWidth / cutoff;
    const double from = std::max(0.0, std::ceil(source - radius));
    const double to = std::min(static_cast<double>(sourceFrames_ - 1), std::floor(source + radius));
    if (from > to) return;
    const auto first = static_cast<std::int64_t>(from);
    const auto last = static_cast<std::int64_t>(to);
    std::array<double, 64> mix{};
    const auto weightAt = [&](std::int64_t frameAt) {
        const double phase = std::abs(source - static_cast<double>(frameAt)) * cutoff * sincPhases;
        const auto index = static_cast<std::size_t>(phase);
        const double a = sinc_[index];
        const double b = sinc_[index + 1];
        return a + (b - a) * (phase - static_cast<double>(index));
    };
    for (auto inputFrame = first; inputFrame <= last;) {
        const auto run = input.contiguous ? input.contiguous(input.context, inputFrame) : WarpStreamPcm{};
        if (run.frames != 0) {
            const auto count = std::min(static_cast<std::int64_t>(run.frames), last - inputFrame + 1);
            if (const auto* pcm = run.data) {
                for (std::int64_t i = 0; i < count; ++i, pcm += channels_) {
                    const auto weight = weightAt(inputFrame + i);
                    for (std::uint16_t channel = 0; channel < output.size(); ++channel)
                        mix[channel] += finiteSample(pcm[channel]) * weight;
                }
            }
            inputFrame += count;
        } else {
            const auto weight = weightAt(inputFrame);
            for (std::uint16_t channel = 0; channel < output.size(); ++channel)
                mix[channel] += finiteSample(input.sample(input.context, inputFrame, channel)) * weight;
            ++inputFrame;
        }
    }
    for (std::uint16_t channel = 0; channel < output.size(); ++channel)
        output[channel] = static_cast<float>(mix[channel] * cutoff);
}

void StreamingWarp::renderFrame(std::int64_t outputFrame, WarpStreamInput input, WarpStreamClock clock,
                                std::span<float> output) const noexcept {
    std::fill_n(output.data(), output.size(), 0.0f);
    if (!input.sample || !clock.tickAtFrame || !clock.frameAtTick || output.empty()) return;
    output = output.first(std::min<std::size_t>(channels_, output.size()));
    const double frame = static_cast<double>(outputFrame);
    const auto tick = clock.tickAtFrame(clock.context, frame);
    const auto endFrame = std::round(clock.frameAtTick(clock.context, static_cast<double>(markers_.back().tick)));
    if (!std::isfinite(tick) || frame >= endFrame) return;
    if (mode_ == WarpMode::Beats) {
        const auto slice = sliceAtFrame(frame, clock);
        const double start = static_cast<double>(slices_[slice].frame);
        const double span = static_cast<double>(slices_[slice + 1].frame - slices_[slice].frame);
        const double startFrame = sliceStartFrame(slice, clock);
        const double local = (frame - startFrame) * pitch_;
        if (!(local >= 0)) return;
        const double readAt = beatsReadAt(local, span);
        if (readAt < 0) return;
        const double gain = std::exp(-beatsEnvelopeRate * (1 - transientEnvelope_) * local / span);
        for (std::uint16_t channel = 0; channel < output.size(); ++channel)
            output[channel] = static_cast<float>(sample(input, start + readAt, channel) * gain);
        return;
    }
    if (frame < std::round(clock.frameAtTick(clock.context, 0))) return;
    if (mode_ == WarpMode::Texture)
        renderTexture(frame, input, clock, output);
    else
        renderRePitch(frame, sourceAtTick(tick), input, clock, output);
}

WarpSourceRange StreamingWarp::sourceRange(std::int64_t firstOutputFrame, std::int64_t lastOutputFrame,
                                         WarpStreamClock clock) const noexcept {
    if (!clock.tickAtFrame || !clock.frameAtTick || lastOutputFrame < firstOutputFrame) return {};
    const double first = static_cast<double>(firstOutputFrame);
    const double last = static_cast<double>(lastOutputFrame);
    const auto at = [&](double frame) { return sourceAtTick(clock.tickAtFrame(clock.context, frame)); };
    double begin = 0;
    double end = 0;
    switch (mode_) {
    case WarpMode::Beats: {
        const auto a = sliceAtFrame(first, clock);
        const auto b = sliceAtFrame(last, clock);
        if (a == b) {
            const auto range = beatsRange(first, last, a, clock);
            begin = range.first;
            end = range.second + 2;
        } else {
            const auto firstRange = beatsRange(first, sliceStartFrame(a + 1, clock) - 1, a, clock);
            const auto lastRange = beatsRange(sliceStartFrame(b, clock), last, b, clock);
            begin = std::min(firstRange.first, lastRange.first);
            end = std::max(firstRange.second, lastRange.second) + 2;
            if (b > a + 1) {
                begin = std::min(begin, static_cast<double>(slices_[a + 1].frame));
                end = std::max(end, static_cast<double>(slices_[b].frame) + 2);
            }
        }
        break;
    }
    case WarpMode::Texture: {
        const double half = static_cast<double>(grain_) / 2;
        const double margin = grainJitterFrames_ + (half + 1) * pitch_;
        begin = at(first - half - 1) - margin;
        end = at(last + half + 1) + margin + 2;
        break;
    }
    case WarpMode::RePitch: {
        const double maxStride = clock.maxTicksPerFrame > 0 ? clock.maxTicksPerFrame * maxFramesPerTick_ : maxSincStride;
        const double stride = first == last ? at(first + 1) - at(first) : maxStride;
        const double radius = sincHalfWidth * std::clamp(stride, 1.0, maxSincStride);
        begin = at(first) - radius;
        end = at(last) + radius + 1;
        break;
    }
    default:
        return {};
    }
    if (!std::isfinite(begin) || !std::isfinite(end) || end <= begin) return {};
    const auto length = static_cast<double>(sourceFrames_);
    return {static_cast<std::int64_t>(std::clamp(std::floor(begin), 0.0, length)),
            static_cast<std::int64_t>(std::clamp(std::ceil(end), 0.0, length))};
}

} // namespace adi::audio
