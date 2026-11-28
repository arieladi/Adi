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

constexpr std::uint64_t maxSourceFrames = (std::uint64_t{1} << 31) - 1;
constexpr std::size_t maxWarpPoints = 1000000;
constexpr double beatsAttackSeconds = 0.01;
constexpr double beatsEnvelopeRate = 12;
constexpr int sincPhases = 256;
constexpr int rePitchHalfWidth = 144;
constexpr int nativeHalfWidth = 46;
constexpr double maxSincStride = 256;
constexpr int maxPageRangeSegments = 8;

template<int converter, std::size_t halfWidth>
const float* impulseResponse() {
    // Obtain the pinned libsamplerate kernel to table precision by resampling
    // a centred impulse. Random access to the kernel lets a loop, seek or
    // tempo edit start directly at its exact fractional source frame.
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
        std::array<float, halfWidth * sincPhases + 2> result{};
        if (src_simple(&data, converter, 1) != 0 ||
            data.output_frames_gen < static_cast<long>(center * sincPhases + result.size()))
            throw std::runtime_error("cannot prepare the streaming warp sample-rate filter");
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

template<auto from>
const WarpMarker* markerAfter(std::span<const WarpMarker> markers, double position) noexcept {
    const auto* begin = markers.data();
    const auto* end = begin + markers.size();
    const auto* next = std::upper_bound(begin, end, position,
        [](double at, const WarpMarker& point) { return at < static_cast<double>(point.*from); });
    return std::clamp(next, begin + 1, end - 1);
}

template<auto from, auto to>
double project(std::span<const WarpMarker> markers, double position) noexcept {
    const auto* high = markerAfter<from>(markers, position);
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
    if (sourceRate < 8000u || sourceRate > 768000u || plan.outputRate < 44100u || plan.outputRate > 768000u ||
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
        sinc_ = impulseResponse<SRC_SINC_BEST_QUALITY, rePitchHalfWidth>();
        sincHalfWidth_ = rePitchHalfWidth;
        break;
    default:
        throw std::invalid_argument("the native streaming player cannot play this warp mode");
    }
    if (mode_ != WarpMode::RePitch && sourceRate != plan.outputRate) {
        // Keep Beats and Texture pitch in the device band even when the file
        // is at a different rate. Their same-rate interpolation is unchanged.
        sinc_ = impulseResponse<SRC_SINC_MEDIUM_QUALITY, nativeHalfWidth>();
        sincHalfWidth_ = nativeHalfWidth;
        nativeCutoff_ = std::min(1.0, static_cast<double>(plan.outputRate) / sourceRate);
    }
}

double StreamingWarp::sourceAtTick(double tick) const noexcept {
    return project<&WarpMarker::tick, &WarpMarker::sourceFrame>(markers_, tick);
}

double StreamingWarp::tickAtSource(double frame) const noexcept {
    return project<&WarpMarker::sourceFrame, &WarpMarker::tick>(markers_, frame);
}

double StreamingWarp::maxFramesPerTick(double firstTick, double lastTick) const noexcept {
    const auto* first = markerAfter<&WarpMarker::tick>(markers_, firstTick);
    const auto* last = markerAfter<&WarpMarker::tick>(markers_, lastTick);
    // Bound the page planner's callback work even for a densely marked clip.
    if (last < first || last - first >= maxPageRangeSegments) return maxFramesPerTick_;
    double maximum = 0;
    for (auto* marker = first; marker <= last; ++marker) {
        maximum = std::max(maximum, static_cast<double>(marker->sourceFrame - marker[-1].sourceFrame) /
            static_cast<double>(marker->tick - marker[-1].tick));
    }
    return maximum;
}

StreamingWarp::SliceLocation StreamingWarp::sliceAtFrame(double frame, WarpStreamClock clock) const noexcept {
    // A physical onset time is rounded once to the nearest output frame.
    // The approach from below at a half-frame keeps ties on the later frame.
    const double tick = clock.tickAtFrame(clock.context, frame + 0.5);
    const auto* begin = slices_.data();
    const auto* end = begin + slices_.size();
    const auto* next = std::lower_bound(begin, end, tick,
        [](const auto& point, double position) { return point.tick < position; });
    const auto projected = [&](const Slice& slice) { return std::round(clock.frameAtTick(clock.context, slice.tick)); };
    const auto* current = std::clamp(next, begin + 1, end - 1) - 1;
    auto start = projected(*current);
    // A floating-point inverse can land either side of an exact half-sample.
    // In that case the forward projection owns the rounded boundary.
    if ((current != begin && start > frame) || (current + 1 < end - 1 && projected(current[1]) <= frame)) {
        next = std::upper_bound(begin, end, frame,
            [&](double position, const Slice& slice) { return position < projected(slice); });
        current = std::clamp(next, begin + 1, end - 1) - 1;
        start = projected(*current);
    }
    const auto* following = current + 1;
    // Several detected onsets can round to the same output sample. As in
    // the prepared render, keep the earliest source attack for that slice.
    if (current != begin && projected(current[-1]) == start) {
        current = std::lower_bound(begin, current, start,
            [&](const Slice& slice, double position) { return projected(slice) < position; });
    }
    return {static_cast<std::size_t>(current - begin), static_cast<std::size_t>(following - begin), start};
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

std::pair<double, double> StreamingWarp::beatsRange(double first, double last, SliceLocation slice) const noexcept {
    const double localFirst = std::max(0.0, (first - slice.outputFrame) * pitch_);
    const double localLast = (last - slice.outputFrame) * pitch_;
    const double start = static_cast<double>(slices_[slice.index].frame);
    const double span = static_cast<double>(slices_[slice.next].frame - slices_[slice.index].frame);
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

void StreamingWarp::sampleFrame(WarpStreamInput input, double position, std::span<float> output) const noexcept {
    if (!(position >= 0 && position < static_cast<double>(sourceFrames_))) {
        std::fill_n(output.data(), output.size(), 0.0f);
        return;
    }
    if (nativeCutoff_ != 0) {
        sincFrame(input, position, nativeCutoff_, output);
        return;
    }
    const auto first = static_cast<std::int64_t>(position);
    const double fraction = position - static_cast<double>(first);
    const bool needsNext = fraction != 0 && first + 1 < sourceFrames_;
    for (std::uint16_t channel = 0; channel < output.size(); ++channel) {
        const double a = finiteSample(input.sample(input.context, first, channel));
        const double b = needsNext ? finiteSample(input.sample(input.context, first + 1, channel)) : 0;
        output[channel] = static_cast<float>(a * (1 - fraction) + b * fraction);
    }
}

std::pair<std::int64_t, std::int64_t> StreamingWarp::textureGrains(double first, double last,
                                                                 WarpStreamClock clock) const noexcept {
    const auto half = static_cast<double>(grain_ / 2);
    const auto grain = static_cast<double>(grain_);
    const auto hop = static_cast<double>(grainHop_);
    const auto shift = static_cast<double>(clock.textureGridShift);
    return {static_cast<std::int64_t>(std::floor((first - shift - grain + half) / hop)) + 1,
            static_cast<std::int64_t>(std::floor((last - shift + half) / hop))};
}

double StreamingWarp::textureGrainCenter(std::int64_t index, WarpStreamClock clock) const noexcept {
    return static_cast<double>(index * grainHop_ + clock.textureGridShift);
}

double StreamingWarp::textureGrainStart(double center) const noexcept {
    return center + static_cast<double>(grain_ - grain_ / 2 - 1);
}

double StreamingWarp::reverseTextureCenterTick(double center, WarpStreamClock clock) const noexcept {
    // This is the latest virtual frame in the grain and therefore
    // its first physical sample. Use only its preceding clock rate
    // so a later tempo edit cannot move a grain that has started.
    const auto start = textureGrainStart(center);
    const auto startTick = clock.tickAtFrame(clock.context, start);
    const auto slope = std::clamp(clock.tickAtFrame(clock.context, start + 1) - startTick,
                                  0.0, clock.maxTicksPerFrame);
    return startTick + (center - start) * slope;
}

void StreamingWarp::renderTexture(double frame, WarpStreamInput input, WarpStreamClock clock,
                                  std::span<float> output, WarpTextureCursor* cursor) const noexcept {
    const auto half = grain_ / 2;
    const auto grain = static_cast<double>(grain_);
    const auto [first, last] = textureGrains(frame, frame, clock);
    double weights = 0;
    std::array<double, 64> mix{};
    std::array<float, 64> grainPcm;
    for (auto index = first; index <= last; ++index) {
        const auto center = textureGrainCenter(index, clock);
        const double phase = (frame - center + static_cast<double>(half)) / grain;
        const double weight = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * phase);
        if (weight == 0) continue;
        const double jitter = (static_cast<double>(grainRandom(seed_, index) >> 11) / 0x1p53 - 0.5) *
            (2 * grainJitterFrames_);
        double source;
        if (cursor) {
            auto& pivot = cursor->pivots_[static_cast<std::size_t>(index) % cursor->pivots_.size()];
            if (pivot.index != index) pivot = {index, sourceAtTick(reverseTextureCenterTick(center, clock))};
            source = pivot.sourceFrame;
        } else {
            const auto sourceTick = clock.causalTextureCenters && clock.maxTicksPerFrame > 0 ?
                reverseTextureCenterTick(center, clock) : clock.tickAtFrame(clock.context, center);
            source = sourceAtTick(sourceTick);
        }
        const double readAt = source + jitter + (frame - center) * pitch_;
        sampleFrame(input, readAt, {grainPcm.data(), output.size()});
        for (std::uint16_t channel = 0; channel < output.size(); ++channel)
            mix[channel] += grainPcm[channel] * weight;
        weights += weight;
    }
    if (weights > 0)
        for (std::uint16_t channel = 0; channel < output.size(); ++channel)
            output[channel] = static_cast<float>(mix[channel] / weights);
}

void StreamingWarp::sincFrame(WarpStreamInput input, double source, double cutoff,
                             std::span<float> output) const noexcept {
    const double radius = sincHalfWidth_ / cutoff;
    const double from = std::max(0.0, std::ceil(source - radius));
    const double to = std::min(static_cast<double>(sourceFrames_ - 1), std::floor(source + radius));
    if (from > to) {
        std::fill_n(output.data(), output.size(), 0.0f);
        return;
    }
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

bool StreamingWarp::renderRePitch(double frame, double source, WarpStreamInput input, WarpStreamClock clock,
                                  std::span<float> output) const noexcept {
    const double stride = sourceAtTick(clock.tickAtFrame(clock.context, frame + 1)) - source;
    // Account for one frame's subtraction at a long absolute source position.
    const double error = 2 * std::numeric_limits<double>::epsilon() * (std::abs(source) + maxSincStride);
    if (!(stride > 0 && stride + error >= 1.0 / maxSincStride && stride - error <= maxSincStride)) return false;
    // The same bandwidth reduction and gain normalization as libsamplerate
    // applies when decimating. At unity, the impulse table is its kernel.
    sincFrame(input, source, 1 / std::clamp(stride, 1.0, maxSincStride), output);
    return true;
}

bool StreamingWarp::renderFrame(std::int64_t outputFrame, WarpStreamInput input, WarpStreamClock clock,
                                std::span<float> output) const noexcept {
    std::fill_n(output.data(), output.size(), 0.0f);
    if (!input.sample || !clock.tickAtFrame || !clock.frameAtTick || output.empty()) return true;
    output = output.first(std::min<std::size_t>(channels_, output.size()));
    const double frame = static_cast<double>(outputFrame);
    const auto tick = clock.tickAtFrame(clock.context, frame);
    const auto endFrame = std::round(clock.frameAtTick(clock.context, static_cast<double>(markers_.back().tick)));
    if (!std::isfinite(tick) || frame >= endFrame) return true;
    if (mode_ == WarpMode::Beats) {
        const auto slice = sliceAtFrame(frame, clock);
        const double start = static_cast<double>(slices_[slice.index].frame);
        const double span = static_cast<double>(slices_[slice.next].frame - slices_[slice.index].frame);
        const double local = (frame - slice.outputFrame) * pitch_;
        if (!(local >= 0)) return true;
        const double readAt = beatsReadAt(local, span);
        if (readAt < 0) return true;
        const double gain = std::exp(-beatsEnvelopeRate * (1 - transientEnvelope_) * local / span);
        sampleFrame(input, start + readAt, output);
        for (std::uint16_t channel = 0; channel < output.size(); ++channel)
            output[channel] = static_cast<float>(output[channel] * gain);
        return true;
    }
    if (frame < std::round(clock.frameAtTick(clock.context, 0))) return true;
    if (mode_ == WarpMode::RePitch) return renderRePitch(frame, sourceAtTick(tick), input, clock, output);
    renderTexture(frame, input, clock, output);
    return true;
}

void StreamingWarp::renderReversedTexture(std::int64_t outputFrame, WarpStreamInput input, WarpStreamClock clock,
                                         WarpTextureCursor& cursor, std::span<float> output) const noexcept {
    std::fill_n(output.data(), output.size(), 0.0f);
    if (!input.sample || !clock.tickAtFrame || !clock.frameAtTick || output.empty()) return;
    if (!cursor.hasBounds_) {
        cursor.first_ = std::round(clock.frameAtTick(clock.context, 0));
        cursor.end_ = std::round(clock.frameAtTick(clock.context, static_cast<double>(markers_.back().tick)));
        cursor.hasBounds_ = true;
    }
    const auto frame = static_cast<double>(outputFrame);
    if (frame < cursor.first_ || frame >= cursor.end_) return;
    renderTexture(frame, input, clock, output.first(std::min<std::size_t>(channels_, output.size())), &cursor);
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
        if (a.index == b.index) {
            const auto range = beatsRange(first, last, a);
            begin = range.first;
            end = range.second + 2;
        } else {
            const auto firstRange = beatsRange(first, sliceStartFrame(a.next, clock) - 1, a);
            const auto lastRange = beatsRange(b.outputFrame, last, b);
            begin = std::min(firstRange.first, lastRange.first);
            end = std::max(firstRange.second, lastRange.second) + 2;
            if (b.index > a.next) {
                begin = std::min(begin, static_cast<double>(slices_[a.next].frame));
                end = std::max(end, static_cast<double>(slices_[b.index].frame) + 2);
            }
        }
        break;
    }
    case WarpMode::Texture: {
        const double half = static_cast<double>(grain_) / 2;
        const double margin = grainJitterFrames_ + (half + 1) * pitch_;
        if (clock.causalTextureCenters && clock.maxTicksPerFrame > 0) {
            const auto [firstGrain, lastGrain] = textureGrains(first, last, clock);
            constexpr auto exactGrainLimit = 32;
            const auto firstCenter = textureGrainCenter(firstGrain, clock);
            const auto lastCenter = textureGrainCenter(lastGrain, clock);
            double firstTick, lastTick;
            if (lastGrain - firstGrain < exactGrainLimit) {
                // A project-wide rate bound can include unrelated tempo
                // events and waste pages. Use the actual source centers
                // where their number is small enough to inspect directly.
                firstTick = lastTick = reverseTextureCenterTick(firstCenter, clock);
                for (auto grain = firstGrain + 1; grain <= lastGrain; ++grain) {
                    const auto tick = reverseTextureCenterTick(textureGrainCenter(grain, clock), clock);
                    firstTick = std::min(firstTick, tick);
                    lastTick = std::max(lastTick, tick);
                }
            } else {
                // Start ticks increase with grain index. Each center
                // projects back at most a half-grain at the maximum
                // clock rate, bounding larger requests in constant time.
                const auto firstStart = textureGrainStart(firstCenter);
                firstTick = clock.tickAtFrame(clock.context, firstStart) -
                            (firstStart - firstCenter) * clock.maxTicksPerFrame;
                lastTick = clock.tickAtFrame(clock.context, textureGrainStart(lastCenter));
            }
            begin = sourceAtTick(firstTick) - margin;
            end = sourceAtTick(lastTick) + margin + 2;
        } else {
            begin = at(first - half - 1) - margin;
            end = at(last + half + 1) + margin + 2;
        }
        break;
    }
    case WarpMode::RePitch: {
        double stride = maxSincStride;
        if (first == last) {
            stride = at(first + 1) - at(first);
        } else if (clock.maxTicksPerFrame > 0) {
            stride = clock.maxTicksPerFrame * maxFramesPerTick(clock.tickAtFrame(clock.context, first),
                                                               clock.tickAtFrame(clock.context, last + 1));
        }
        const double radius = sincHalfWidth_ * std::clamp(stride, 1.0, maxSincStride);
        begin = at(first) - radius;
        end = at(last) + radius + 1;
        break;
    }
    default:
        return {};
    }
    if (!std::isfinite(begin) || !std::isfinite(end) || end <= begin) return {};
    if (nativeCutoff_ != 0) {
        const auto radius = sincHalfWidth_ / nativeCutoff_;
        begin -= radius;
        end += radius;
    }
    const auto length = static_cast<double>(sourceFrames_);
    return {static_cast<std::int64_t>(std::clamp(std::floor(begin), 0.0, length)),
            static_cast<std::int64_t>(std::clamp(std::ceil(end), 0.0, length))};
}

} // namespace adi::audio
